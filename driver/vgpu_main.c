#include "../include/vgpu/vgpu_core.h"

int queue_mode = 0;
module_param(queue_mode, int, 0644);
MODULE_PARM_DESC(queue_mode, "0: Global Shared Queue, 1: Private Context Queue");

int dma_mode = 0;
module_param(dma_mode, int, 0644);
MODULE_PARM_DESC(dma_mode, "0: Software MMAP, 1: DMA API");

struct vgpu_dev *g_vgpu_dev = NULL;
struct class *g_vgpu_class = NULL;

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

static const struct file_operations vgpu_fops = {
    .owner          = THIS_MODULE,
    .open           = vgpu_open,
    .release        = vgpu_release,
    .read           = vgpu_read,
    .write          = vgpu_write,
    .unlocked_ioctl = vgpu_ioctl,
    .mmap           = vgpu_mmap,
};

static dev_t vgpu_dev_num;

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

    g_vgpu_dev = kzalloc(sizeof(struct vgpu_dev), GFP_KERNEL);
    if (!g_vgpu_dev) {
        pr_err("vGPU-Core: failed to allocate vgpu_dev\n");
        class_destroy(g_vgpu_class);
        unregister_chrdev_region(vgpu_dev_num, 1);
        return -ENOMEM;
    }

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
        g_vgpu_dev->pdev = platform_device_register_simple("vgpu_device", -1, NULL, 0);
        if (IS_ERR(g_vgpu_dev->pdev)) {
            pr_err("vGPU-Core: failed to register platform device\n");
            result = PTR_ERR(g_vgpu_dev->pdev);
            goto err_device_create;
        }
        
        dma_set_mask_and_coherent(&g_vgpu_dev->pdev->dev, DMA_BIT_MASK(64));
        
        g_vgpu_dev->data_buffer = dma_alloc_coherent(&g_vgpu_dev->pdev->dev, PAGE_SIZE, 
                                                     &g_vgpu_dev->dma_handle, GFP_KERNEL);
        if (!g_vgpu_dev->data_buffer) {
            pr_err("vGPU-Core: failed to allocate DMA memory\n");
            result = -ENOMEM;
            platform_device_unregister(g_vgpu_dev->pdev);
            goto err_device_create;
        }
        pr_info("vGPU-Core: Allocated DMA buffer at %p (bus addr: %llx)\n", 
                g_vgpu_dev->data_buffer, (unsigned long long)g_vgpu_dev->dma_handle);
    } else {
        g_vgpu_dev->data_buffer = (void *)__get_free_pages(GFP_KERNEL, 0);
        if (!g_vgpu_dev->data_buffer) {
            pr_err("vGPU-Core: failed to allocate pages\n");
            result = -ENOMEM;
            goto err_device_create;
        }
        pr_info("vGPU-Core: Allocated pure software page buffer at %p\n", 
                g_vgpu_dev->data_buffer);
    }

    cdev_init(&g_vgpu_dev->cdev, &vgpu_fops);
    g_vgpu_dev->cdev.owner = THIS_MODULE;

    result = cdev_add(&g_vgpu_dev->cdev, vgpu_dev_num, 1);
    if (result < 0) {
        pr_err("vGPU-Core: failed to add cdev\n");
        goto err_cdev_add;
    }

    g_vgpu_dev->device = device_create(g_vgpu_class, NULL, vgpu_dev_num, NULL, "vgpu0");
    if (IS_ERR(g_vgpu_dev->device)) {
        pr_err("vGPU-Core: failed to create device node\n");
        result = PTR_ERR(g_vgpu_dev->device);
        goto err_device_node;
    }

    pr_info("vGPU-Core: module loaded successfully\n");
    return 0;

err_device_node:
    cdev_del(&g_vgpu_dev->cdev);
err_cdev_add:
    if (dma_mode == 1) {
        if (g_vgpu_dev->data_buffer) {
            dma_free_coherent(&g_vgpu_dev->pdev->dev, PAGE_SIZE, g_vgpu_dev->data_buffer, g_vgpu_dev->dma_handle);
        }
        if (g_vgpu_dev->pdev) {
            platform_device_unregister(g_vgpu_dev->pdev);
        }
    } else {
        if (g_vgpu_dev->data_buffer) {
            free_pages((unsigned long)g_vgpu_dev->data_buffer, 0);
        }
    }
err_device_create:
    if (g_vgpu_dev->hw_wq) {
        destroy_workqueue(g_vgpu_dev->hw_wq);
    }
    kfree(g_vgpu_dev);
    class_destroy(g_vgpu_class);
    unregister_chrdev_region(vgpu_dev_num, 1);
    return result;
}

static void __exit vgpu_core_exit(void)
{
    pr_info("vGPU-Core: module unloading...\n");

    if (g_vgpu_dev) {
        if (g_vgpu_dev->hw_wq) {
            flush_workqueue(g_vgpu_dev->hw_wq);
            destroy_workqueue(g_vgpu_dev->hw_wq);
        }

        if (dma_mode == 1) {
            if (g_vgpu_dev->data_buffer) {
                dma_free_coherent(&g_vgpu_dev->pdev->dev, PAGE_SIZE, g_vgpu_dev->data_buffer, g_vgpu_dev->dma_handle);
            }
            if (g_vgpu_dev->pdev) {
                platform_device_unregister(g_vgpu_dev->pdev);
            }
        } else {
            if (g_vgpu_dev->data_buffer) {
                free_pages((unsigned long)g_vgpu_dev->data_buffer, 0);
            }
        }

        device_destroy(g_vgpu_class, vgpu_dev_num);
        cdev_del(&g_vgpu_dev->cdev);
        kfree(g_vgpu_dev);
    }

    if (g_vgpu_class) {
        class_destroy(g_vgpu_class);
    }

    unregister_chrdev_region(vgpu_dev_num, 1);
    pr_info("vGPU-Core: module unloaded\n");
}

module_init(vgpu_core_init);
module_exit(vgpu_core_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("vGPU Team");
MODULE_DESCRIPTION("Virtual PCIe GPU/Accelerator Core Driver");
