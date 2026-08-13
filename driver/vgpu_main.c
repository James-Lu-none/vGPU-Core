#include "../include/vgpu/vgpu_core.h"

int queue_mode = 0;
module_param(queue_mode, int, 0644);
MODULE_PARM_DESC(queue_mode, "0: Global Shared Queue, 1: Private Context Queue");

int dma_mode = 0;
module_param(dma_mode, int, 0644);
MODULE_PARM_DESC(dma_mode, "0: Software MMAP, 1: DMA API");

struct vgpu_dev *g_vgpu_dev = NULL;
struct class *g_vgpu_class = NULL;

// vgpu_dev_num is outside of vgpu_dev structure since dev_t contains
// major and minor number for all devices that use this driver module
static dev_t vgpu_dev_num;

// outer platform_device pointer for fake platform device
static struct platform_device *vgpu_device_pdev = NULL;

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
    pr_info("vGPU-Core: open\n");
    if (queue_mode == 0){
        pr_info("vGPU-Core: Global Shared Queue, skip context allocate\n");
    } else {
        pr_info("vGPU-Core: Private Context Queue, start to allocate context\n");
        struct vgpu_context *ctx;
        
        ctx = kzalloc(sizeof(struct vgpu_context), GFP_KERNEL);
        if (!ctx) {
            pr_err("vGPU-Core: failed to allocate context structure\n");
            return -ENOMEM;
        }
        
        init_waitqueue_head(&ctx->wait_q);
        ctx->irq_fired = 0;
        
        spin_lock(&g_vgpu_dev->ctx_lock);
        list_add_tail(&ctx->list_node, &g_vgpu_dev->ctx_list);
        spin_unlock(&g_vgpu_dev->ctx_lock);
        
        file->private_data = ctx;
    }
    return 0;
}

static int vgpu_release(struct inode *inode, struct file *file)
{
    pr_info("vGPU-Core: release\n");
    if (queue_mode == 0){
        pr_info("vGPU-Core: Global Shared Queue, skip context allocate\n");
    } else {
        pr_info("vGPU-Core: Private Context Queue, start to release context\n");
        struct vgpu_context *ctx = file->private_data;
        if (ctx) {
            spin_lock(&g_vgpu_dev->ctx_lock);
            list_del(&ctx->list_node);
            spin_unlock(&g_vgpu_dev->ctx_lock);
            kfree(ctx);
        }
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

static int vgpu_probe(struct platform_device *pdev)
{
    int result;
    pr_info("vGPU-Core: probing device...\n");

    g_vgpu_dev = kzalloc(sizeof(struct vgpu_dev), GFP_KERNEL);
    if (!g_vgpu_dev) {
        pr_err("vGPU-Core: failed to allocate vgpu_dev\n");
        return -ENOMEM;
    }

    g_vgpu_dev->pdev = pdev;
    platform_set_drvdata(pdev, g_vgpu_dev);

    spin_lock_init(&g_vgpu_dev->global_lock);
    g_vgpu_dev->head = 0;
    g_vgpu_dev->tail = 0;
    
    spin_lock_init(&g_vgpu_dev->ctx_lock);
    INIT_LIST_HEAD(&g_vgpu_dev->ctx_list);

    g_vgpu_dev->hw_wq = create_singlethread_workqueue("vgpu_hw_wq");
    INIT_WORK(&g_vgpu_dev->hw_work, vgpu_hw_work_func);
    init_waitqueue_head(&g_vgpu_dev->wait_q);
    g_vgpu_dev->irq_fired = 0;

    if (dma_mode == 1) {
        dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
        
        g_vgpu_dev->data_buffer = dma_alloc_coherent(&pdev->dev, PAGE_SIZE, 
                                                     &g_vgpu_dev->dma_handle, GFP_KERNEL);
        if (!g_vgpu_dev->data_buffer) {
            pr_err("vGPU-Core: failed to allocate DMA memory\n");
            result = -ENOMEM;
            goto err_wq;
        }
        pr_info("vGPU-Core: Allocated DMA buffer at %p (bus addr: %llx)\n", 
                g_vgpu_dev->data_buffer, (unsigned long long)g_vgpu_dev->dma_handle);
    } else {
        g_vgpu_dev->data_buffer = (void *)__get_free_pages(GFP_KERNEL, 0);
        if (!g_vgpu_dev->data_buffer) {
            pr_err("vGPU-Core: failed to allocate pages\n");
            result = -ENOMEM;
            goto err_wq;
        }
        pr_info("vGPU-Core: Allocated pure software page buffer at %p\n", 
                g_vgpu_dev->data_buffer);
    }

    /*
     * VFS Registration (Part 1/2): cdev_init
     * We initialize the character device structure and link it to our vgpu_fops.
     * This prepares the cdev to handle system calls routed by VFS.
     */
    cdev_init(&g_vgpu_dev->cdev, &vgpu_fops);
    g_vgpu_dev->cdev.owner = THIS_MODULE;

    /*
     * VFS Registration (Part 2/2): cdev_add
     * We register this cdev into the kernel's cdev_map using our device number (Major/Minor).
     * After this call, VFS knows that any operation on this device number
     * should be handled by our vgpu_fops.
     */
    result = cdev_add(&g_vgpu_dev->cdev, vgpu_dev_num, 1);
    if (result < 0) {
        pr_err("vGPU-Core: failed to add cdev\n");
        goto err_mem;
    }

    g_vgpu_dev->device = device_create(g_vgpu_class, &pdev->dev, vgpu_dev_num, NULL, "vgpu0");
    if (IS_ERR(g_vgpu_dev->device)) {
        pr_err("vGPU-Core: failed to create device node\n");
        result = PTR_ERR(g_vgpu_dev->device);
        goto err_cdev;
    }

    pr_info("vGPU-Core: device probed successfully\n");
    return 0;

err_cdev:
    cdev_del(&g_vgpu_dev->cdev);
err_mem:
    if (dma_mode == 1) {
        dma_free_coherent(&pdev->dev, PAGE_SIZE, g_vgpu_dev->data_buffer, g_vgpu_dev->dma_handle);
    } else {
        free_pages((unsigned long)g_vgpu_dev->data_buffer, 0);
    }
err_wq:
    destroy_workqueue(g_vgpu_dev->hw_wq);
    kfree(g_vgpu_dev);
    g_vgpu_dev = NULL;
    return result;
}

static void vgpu_remove(struct platform_device *pdev)
{
    struct vgpu_dev *dev = platform_get_drvdata(pdev);
    pr_info("vGPU-Core: removing device...\n");

    if (dev) {
        device_destroy(g_vgpu_class, vgpu_dev_num);
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
        g_vgpu_dev = NULL;
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

    result = alloc_chrdev_region(&vgpu_dev_num, 0, 1, "vgpu_core");
    if (result < 0) {
        pr_err("vGPU-Core: failed to allocate char dev region\n");
        return result;
    }

    g_vgpu_class = class_create("vgpu_class");
    if (IS_ERR(g_vgpu_class)) {
        pr_err("vGPU-Core: failed to create class\n");
        unregister_chrdev_region(vgpu_dev_num, 1);
        return PTR_ERR(g_vgpu_class);
    }

    // fake a platform device named vgpu_device to match vgpu_driver.driver.name
    // in real scenario, if the device was connected to PCIe, and PCIe bus detected it
    // then kernel will create a pci_dev or platform_device automatically
    vgpu_device_pdev = platform_device_register_simple("vgpu_device", -1, NULL, 0);
    if (IS_ERR(vgpu_device_pdev)) {
        pr_err("vGPU-Core: failed to register dummy platform device\n");
        result = PTR_ERR(vgpu_device_pdev);
        goto err_class;
    }

    result = platform_driver_register(&vgpu_driver);
    if (result < 0) {
        pr_err("vGPU-Core: failed to register platform driver\n");
        goto err_pdev;
    }

    pr_info("vGPU-Core: module loaded successfully\n");
    return 0;

err_pdev:
    platform_device_unregister(vgpu_device_pdev);
err_class:
    class_destroy(g_vgpu_class);
    unregister_chrdev_region(vgpu_dev_num, 1);
    return result;
}

static void __exit vgpu_core_exit(void)
{
    pr_info("vGPU-Core: module unloading...\n");

    platform_driver_unregister(&vgpu_driver);
    platform_device_unregister(vgpu_device_pdev);

    class_destroy(g_vgpu_class);
    unregister_chrdev_region(vgpu_dev_num, 1);

    pr_info("vGPU-Core: module unloaded\n");
}

module_init(vgpu_core_init);
module_exit(vgpu_core_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("vGPU Team");
MODULE_DESCRIPTION("Virtual PCIe GPU/Accelerator Core Driver");
