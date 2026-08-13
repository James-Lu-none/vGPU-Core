#include "../include/vgpu/vgpu_core.h"

int queue_mode = 0;
module_param(queue_mode, int, 0644);
MODULE_PARM_DESC(queue_mode, "0: Global Shared Queue, 1: Private Context Queue");

int dma_mode = 0;
module_param(dma_mode, int, 0644);
MODULE_PARM_DESC(dma_mode, "0: Software MMAP, 1: DMA API");

struct class *g_vgpu_class = NULL;

// vgpu_dev_num is outside of vgpu_dev structure since dev_t contains
// major and minor number for all devices that use this driver module
static dev_t vgpu_dev_num;

/*
 * VFS Hook: open()
 * When User Space calls open("/dev/vgpu0"), VFS (Virtual File System) intercepts it.
 * VFS looks up the inode (Index Node) of /dev/vgpu0, extracts the Major/Minor number,
 * and searches the cdev_map to find our registered 'struct cdev'.
 * VFS then creates a 'struct file', assigns our vgpu_fops to file->f_op,
 * and finally calls this vgpu_open() function.
 */
static int vgpu_open(struct inode *inode, struct file *file)
{
    // by passing inode->i_cdev and call container_of, we can know that which device among /dev/vgpuX
    // user is opening and assign the correct dev to that context, this is for private context queue mode
    struct vgpu_dev *dev = container_of(inode->i_cdev, struct vgpu_dev, cdev);
    struct vgpu_context *ctx;
    
    pr_info("vGPU-Core: open vgpu%d\n", dev->minor);
    
    ctx = kzalloc(sizeof(struct vgpu_context), GFP_KERNEL);
    if (!ctx) {
        pr_err("vGPU-Core: failed to allocate context structure\n");
        return -ENOMEM;
    }
    
    // assign dev to that context
    ctx->dev = dev;
    init_waitqueue_head(&ctx->wait_q);
    ctx->irq_fired = 0;
    
    spin_lock(&dev->ctx_lock);
    list_add_tail(&ctx->list_node, &dev->ctx_list);
    spin_unlock(&dev->ctx_lock);
    
    // put context into private data, which contains dev information to distinguish them
    file->private_data = ctx;
    return 0;
}

static int vgpu_release(struct inode *inode, struct file *file)
{
    struct vgpu_context *ctx = file->private_data;
    struct vgpu_dev *dev = ctx->dev;
    
    pr_info("vGPU-Core: release vgpu%d\n", dev->minor);
    
    if (ctx) {
        spin_lock(&dev->ctx_lock);
        list_del(&ctx->list_node);
        spin_unlock(&dev->ctx_lock);
        kfree(ctx);
    }
    return 0;
}

static ssize_t vgpu_read(struct file *file, char __user *user_buf, size_t size, loff_t *offset)
{
    return 0;
}

static ssize_t vgpu_write(struct file *file, const char __user *user_buf, size_t size, loff_t *offset)
{
    return size;
}

/*
 * VFS Contract: struct file_operations
 * This is the core bridge between User Space and Kernel Space.
 * It tells the VFS which driver function to call when a specific
 * system call (like read, write, ioctl, mmap) is executed on our file descriptor.
 */
static const struct file_operations vgpu_fops = {
    .owner          = THIS_MODULE,
    .open           = vgpu_open,
    .release        = vgpu_release,
    .read           = vgpu_read,
    .write          = vgpu_write,
    .unlocked_ioctl = vgpu_ioctl,
    .mmap           = vgpu_mmap,
};

/*
 * when device is registered, this function is called to
 * 1. allocate and setup driver data for the device 
 * 2. cdev_init() bind character device with fops
 * 3. cdev_add() registers character device driver with the kernel's internal map of character devices.
 * 4. device_create() create files under /sys/class/vgpu_class/vgpuX and setup userspace device node under /dev
 * p.s: all operation involves kobject since struct cdev, struct device, struct platform_device all contains kobject.
 * (platform_device containes struct device, so it also contains kobject.)
*/
static int vgpu_probe(struct platform_device *pdev)
{
    int result;
    struct vgpu_dev *dev;
    pr_info("vGPU-Core: probing device %d...\n", pdev->id);

    dev = kzalloc(sizeof(struct vgpu_dev), GFP_KERNEL);
    if (!dev) {
        pr_err("vGPU-Core: failed to allocate vgpu_dev\n");
        return -ENOMEM;
    }

    dev->minor = pdev->id;
    dev->pdev = pdev;
    platform_set_drvdata(pdev, dev);

    spin_lock_init(&dev->global_lock);
    dev->head = 0;
    dev->tail = 0;
    
    spin_lock_init(&dev->ctx_lock);
    INIT_LIST_HEAD(&dev->ctx_list);

    dev->hw_wq = alloc_workqueue("vgpu_hw_wq_%d", WQ_UNBOUND, 1, dev->minor);
    INIT_WORK(&dev->hw_work, vgpu_hw_work_func);
    init_waitqueue_head(&dev->wait_q);
    dev->irq_fired = 0;

    if (dma_mode == 1) {
        dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
        
        dev->data_buffer = dma_alloc_coherent(&pdev->dev, PAGE_SIZE, 
                                              &dev->dma_handle, GFP_KERNEL);
        if (!dev->data_buffer) {
            pr_err("vGPU-Core: failed to allocate DMA memory\n");
            result = -ENOMEM;
            goto err_wq;
        }
        pr_info("vGPU-Core: Allocated DMA buffer at %p (bus addr: %llx)\n", 
                dev->data_buffer, (unsigned long long)dev->dma_handle);
    } else {
        dev->data_buffer = (void *)__get_free_pages(GFP_KERNEL, 0);
        if (!dev->data_buffer) {
            pr_err("vGPU-Core: failed to allocate pages\n");
            result = -ENOMEM;
            goto err_wq;
        }
        pr_info("vGPU-Core: Allocated pure software page buffer at %p\n", 
                dev->data_buffer);
    }

    /*
     * VFS Registration (Part 1/2): cdev_init
     * We initialize the character device structure and link it to our vgpu_fops.
     * This prepares the cdev to handle system calls routed by VFS.
     */
    cdev_init(&dev->cdev, &vgpu_fops);
    dev->cdev.owner = THIS_MODULE;

    /*
     * VFS Registration (Part 2/2): cdev_add
     * We register this cdev into the kernel's cdev_map using our device number (Major/Minor).
     * After this call, VFS knows that any operation on this device number
     * should be handled by our vgpu_fops.
     */
    result = cdev_add(&dev->cdev, MKDEV(MAJOR(vgpu_dev_num), dev->minor), 1);
    if (result < 0) {
        pr_err("vGPU-Core: failed to add cdev\n");
        goto err_mem;
    }

    dev->device = device_create(g_vgpu_class, &pdev->dev, MKDEV(MAJOR(vgpu_dev_num), dev->minor), NULL, "vgpu%d", dev->minor);
    if (IS_ERR(dev->device)) {
        pr_err("vGPU-Core: failed to create device node\n");
        result = PTR_ERR(dev->device);
        goto err_cdev;
    }

    pr_info("vGPU-Core: device %d probed successfully\n", dev->minor);
    return 0;

err_cdev:
    cdev_del(&dev->cdev);
err_mem:
    if (dma_mode == 1) {
        dma_free_coherent(&pdev->dev, PAGE_SIZE, dev->data_buffer, dev->dma_handle);
    } else {
        free_pages((unsigned long)dev->data_buffer, 0);
    }
err_wq:
    destroy_workqueue(dev->hw_wq);
    kfree(dev);
    return result;
}

static void vgpu_remove(struct platform_device *pdev)
{
    struct vgpu_dev *dev = platform_get_drvdata(pdev);
    pr_info("vGPU-Core: removing device %d...\n", dev->minor);

    if (dev) {
        device_destroy(g_vgpu_class, MKDEV(MAJOR(vgpu_dev_num), dev->minor));
        cdev_del(&dev->cdev);

        if (dma_mode == 1) {
            dma_free_coherent(&pdev->dev, PAGE_SIZE, dev->data_buffer, dev->dma_handle);
        } else {
            free_pages((unsigned long)dev->data_buffer, 0);
        }

        if (dev->hw_wq) {
            flush_workqueue(dev->hw_wq);
            destroy_workqueue(dev->hw_wq);
        }

        kfree(dev);
    }
}

static struct platform_driver vgpu_driver = {
    .probe = vgpu_probe,
    .remove = vgpu_remove,
    .driver = {
        .name = "vgpu_device",
        .owner = THIS_MODULE,
    },
};

static int __init vgpu_core_init(void)
{
    int result;

    pr_info("vGPU-Core: module loading...\n");

    result = alloc_chrdev_region(&vgpu_dev_num, 0, MAX_VGPU_DEVICES, "vgpu_core");
    if (result < 0) {
        pr_err("vGPU-Core: failed to allocate char dev region\n");
        return result;
    }

    g_vgpu_class = class_create("vgpu_class");
    if (IS_ERR(g_vgpu_class)) {
        pr_err("vGPU-Core: failed to create class\n");
        unregister_chrdev_region(vgpu_dev_num, MAX_VGPU_DEVICES);
        return PTR_ERR(g_vgpu_class);
    }

    result = platform_driver_register(&vgpu_driver);
    if (result < 0) {
        pr_err("vGPU-Core: failed to register platform driver\n");
        class_destroy(g_vgpu_class);
        unregister_chrdev_region(vgpu_dev_num, MAX_VGPU_DEVICES);
        return result;
    }

    pr_info("vGPU-Core: module loaded successfully\n");
    return 0;
}

static void __exit vgpu_core_exit(void)
{
    pr_info("vGPU-Core: module unloading...\n");

    platform_driver_unregister(&vgpu_driver);
    class_destroy(g_vgpu_class);
    unregister_chrdev_region(vgpu_dev_num, MAX_VGPU_DEVICES);

    pr_info("vGPU-Core: module unloaded\n");
}

module_init(vgpu_core_init);
module_exit(vgpu_core_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("vGPU Team");
MODULE_DESCRIPTION("Virtual PCIe GPU/Accelerator Core Driver");
