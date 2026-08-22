#include "../include/vgpu/vgpu_core.h"
#include <linux/pci.h>

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
static atomic_t vgpu_minor_counter = ATOMIC_INIT(0);

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
// static int vgpu_probe(struct platform_device *pdev)
static int vgpu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    // The physical pci device itself is hardwired with its requirements in the Base Address Register (BAR) (etc: 256MB of MMIO space, 64bit addressing)
    // And during the system boot, the pci subsystem of bios/kernel will fetch those info from device and allocate a portion of physical address space accordingly
    // and write the address of the allocated physical address space back into the device's BAR

    int result;
    struct vgpu_dev *dev;

    pr_info("vGPU-Core: probing PCI device %04x:%04x...\n", pdev->vendor, pdev->device);

    // Enable the physical PCI device and configure PCIe device's Configuration Space, enable memory/IO decoding, assigning interrupt numbers
    result = pci_enable_device(pdev);
    if (result) {
        pr_err("vGPU-Core: failed to enable PCI device\n");
        return result;
    }

    // declare the the pci device is now under driver's control, preventing other drivers
    // from accessing the same resource, and will be released when the driver is unloaded
    result = pci_request_regions(pdev, "vgpu_core");
    if (result) {
        pr_err("vGPU-Core: failed to request PCI regions\n");
        goto err_disable;
    }

    // enable DMA bus mastering capability of the physical PCI device
    // this is required for the device to perform DMA operations
    // as it allows the device to initiate DMA transfers without CPU intervention
    pci_set_master(pdev);

    /*
     * Hardware Capability: MSI/MSI-X Interrupt Allocation
     * Modern PCIe devices don't use physical interrupt pins (INTx) which cause sharing conflicts.
     * Instead, they use Message Signaled Interrupts (MSI), where the device writes a specific value
     * to a specific memory address to trigger an interrupt on the CPU.
     * pci_alloc_irq_vectors() asks the PCI subsystem to allocate these vectors for us.
     */
    result = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI | PCI_IRQ_MSIX);
    if (result < 0) {
        pr_err("vGPU-Core: failed to allocate IRQ vectors\n");
        goto err_disable;
    }

    dev = kzalloc(sizeof(struct vgpu_dev), GFP_KERNEL);
    if (!dev) {
        pr_err("vGPU-Core: failed to allocate vgpu_dev\n");
        result = -ENOMEM;
        goto err_regions;
    }
    // platform_device uses pdev->id to identify each device, which corresponds to minor number
    // dev->minor = pdev->id;
    // dev->pdev = pdev;
    // platform_set_drvdata(pdev, dev);

    // Map BAR0 for MMIO (AXI Lite registers)
    // see page 292 of https://bootlin.com/doc/training/linux-kernel/linux-kernel-slides.pdf
    // pci_ioremap_bar() will read BAR0's physical address that was assigned during boot by pci subsystem from FPGA
    // and call void __iomem *ioremap(phys_addr_t phys_addr, unsigned long size);
    // and update the page table to add a mapping entry that maps a virtual memory region to physical memory region
    // mmio_base is the pointer that points to the virtual memory address of BAR0 in kernel space
    // writing data to mmio_base with offset can then trigger cpu to perform a Memory write TLP (Transaction Layer Packet)
    // to the XDMA IP in the FPGA, and FPGA will write data to the target memory-mapped register in FPGA.
    dev->mmio_base = pci_ioremap_bar(pdev, 0);
    if (!dev->mmio_base) {
        pr_err("vGPU-Core: failed to ioremap BAR0\n");
        result = -ENOMEM;
        goto err_free;
    }

    /*
     * Retrieve the actual IRQ number assigned to vector 0.
     * This number represents the Linux logical IRQ number mapped to the physical MSI vector.
     */
    dev->irq = pci_irq_vector(pdev, 0);

    dev->minor = atomic_inc_return(&vgpu_minor_counter) - 1;
    if (dev->minor >= MAX_VGPU_DEVICES) {
        pr_err("vGPU-Core: exceeded maximum number of devices\n");
        result = -ENOSPC;
        goto err_iounmap;
    }

    dev->pci_dev = pdev;
    pci_set_drvdata(pdev, dev);

    spin_lock_init(&dev->global_lock);
    
    spin_lock_init(&dev->ctx_lock);
    INIT_LIST_HEAD(&dev->ctx_list);

    init_waitqueue_head(&dev->wait_q);
    dev->irq_fired = 0;

    if (dma_mode == 1) {
        // see page 376 at https://bootlin.com/doc/training/linux-kernel/linux-kernel-slides.pdf
        dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
        
        dev->ring_buffer = dma_alloc_coherent(&pdev->dev, PAGE_SIZE, 
                                              &dev->dma_handle, GFP_KERNEL);
        if (!dev->ring_buffer) {
            pr_err("vGPU-Core: failed to allocate DMA memory\n");
            result = -ENOMEM;
            goto err_iounmap;
        }
        pr_info("vGPU-Core: Allocated DMA buffer at %p (bus addr: %llx)\n", 
                dev->ring_buffer, (unsigned long long)dev->dma_handle);
                
        // tell the FPGA where the DMA buffer is located in System RAM.
        // otherwise, the FPGA's XDMA IP doesn't know where to Read/Write.
        // we write the 64-bit Bus Address into two 32-bit registers for compatibility with 32-bit DMA address register.
        // this dma memory is for command queue
        iowrite32(lower_32_bits(dev->dma_handle), dev->mmio_base + VGPU_DMA_ADDR_LOW_OFFSET);
        iowrite32(upper_32_bits(dev->dma_handle), dev->mmio_base + VGPU_DMA_ADDR_HIGH_OFFSET);
        pr_info("vGPU-Core: Configured FPGA Ring Buffer Base Address to 0x%llx\n", (unsigned long long)dev->dma_handle);
        
        // Allocate 64KB Coherent DMA memory for XDMA Descriptor Ring (8192 descriptors)
        // this memory is for storing the descriptor ring that will be sent to FPGA, 
        // 64KB / 8 bytes = 8192 entries (supports up to 32MB payload)
        dev->desc_ring = dma_alloc_coherent(&pdev->dev, 8192 * sizeof(struct xdma_desc),
                                             &dev->desc_ring_dma_addr, GFP_KERNEL);
        if (!dev->desc_ring) {
            pr_err("vGPU-Core: failed to allocate XDMA descriptor ring buffer\n");
            result = -ENOMEM;
            goto err_mem_pt;
        }

        pr_info("vGPU-Core: Allocated XDMA Descriptor Ring at %p (bus addr: %llx)\n", 
                dev->desc_ring, (unsigned long long)dev->desc_ring_dma_addr);
        
        // Allocate array for pointers to the pinned pages (8192 entries)
        dev->pinned_pages = kzalloc(8192 * sizeof(struct page *), GFP_KERNEL);
        if (!dev->pinned_pages) {
            pr_err("vGPU-Core: failed to allocate pinned_pages array\n");
            result = -ENOMEM;
            goto err_mem_pt;
        }
        
    } else {
        dev->ring_buffer = (void *)__get_free_pages(GFP_KERNEL, 0);
        if (!dev->ring_buffer) {
            pr_err("vGPU-Core: failed to allocate pages\n");
            result = -ENOMEM;
            goto err_iounmap;
        }
        pr_info("vGPU-Core: Allocated pure software page buffer at %p\n", 
                dev->ring_buffer);
        
    }

    dev->ring = (struct vgpu_ring_buffer *)dev->ring_buffer;
    dev->ring->head = 0;
    dev->ring->tail = 0;

    /*
     * IRQ Registration: request_irq()
     * We tell the Kernel's generic IRQ subsystem: "When interrupt number dev->irq fires,
     * please execute vgpu_irq_handler". 
     * The last argument 'dev' is the dev_id, an opaque pointer passed back to our handler
     * so we know *which* specific vGPU device triggered the interrupt.
     */
    result = request_irq(dev->irq, vgpu_irq_handler, 0, "vgpu_irq", dev);
    if (result) {
        pr_err("vGPU-Core: failed to request IRQ %d\n", dev->irq);
        goto err_irq;
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

    pr_info("vGPU-Core: vgpu%d probed successfully at MMIO %p\n", dev->minor, dev->mmio_base);
    return 0;

err_cdev:
    cdev_del(&dev->cdev);
err_irq:
    free_irq(dev->irq, dev);
err_mem_pt:
    if (dma_mode == 1) {
        if (dev->pinned_pages) kfree(dev->pinned_pages);
        if (dev->desc_ring) dma_free_coherent(&pdev->dev, 8192 * sizeof(struct xdma_desc), dev->desc_ring, dev->desc_ring_dma_addr);
    }
err_mem:
    if (dma_mode == 1) {
        if (dev->ring_buffer) dma_free_coherent(&pdev->dev, PAGE_SIZE, dev->ring_buffer, dev->dma_handle);
    } else {
        free_pages((unsigned long)dev->ring_buffer, 0);
    }
err_iounmap:
    pci_iounmap(pdev, dev->mmio_base);
err_free:
    kfree(dev);
err_regions:
    pci_free_irq_vectors(pdev);
    pci_release_regions(pdev);
err_disable:
    pci_disable_device(pdev);
    return result;
}

// static void vgpu_remove(struct platform_device *pdev)
static void vgpu_remove(struct pci_dev *pdev)
{
    // struct vgpu_dev *dev = platform_get_drvdata(pdev);
    // pr_info("vGPU-Core: removing device %d...\n", dev->minor);
    struct vgpu_dev *dev = pci_get_drvdata(pdev);

    if (dev) {
        pr_info("vGPU-Core: removing vgpu%d...\n", dev->minor);
        device_destroy(g_vgpu_class, MKDEV(MAJOR(vgpu_dev_num), dev->minor));
        cdev_del(&dev->cdev);

        if (dma_mode == 1) {
            kfree(dev->pinned_pages);
            if (dev->desc_ring) {
                dma_free_coherent(&pdev->dev, 8192 * sizeof(struct xdma_desc), dev->desc_ring, dev->desc_ring_dma_addr);
            }
            dma_free_coherent(&pdev->dev, PAGE_SIZE, dev->ring_buffer, dev->dma_handle);
        } else {
            free_pages((unsigned long)dev->ring_buffer, 0);
        }

        if (dev->mmio_base) {
            pci_iounmap(pdev, dev->mmio_base);
        }

        if (dev->irq) {
            /*
             * IRQ Teardown: free_irq()
             * Unregisters our handler. Crucially, this function will wait for any 
             * currently executing interrupt handlers to finish before returning,
             * ensuring we don't free 'dev' while the IRQ handler is still using it.
             */
            free_irq(dev->irq, dev);
        }

        kfree(dev);
    }
    
    /*
     * Hardware Teardown: pci_free_irq_vectors()
     * Tells the PCI subsystem to release the MSI/MSI-X vectors assigned to this device,
     * freeing up system interrupt resources.
     */
    pci_free_irq_vectors(pdev);
    pci_release_regions(pdev);
    pci_disable_device(pdev);
}

// Xilinx Vendor ID is 0x10EE
// Match any device ID for now, or specify 0x7021 (Artix-7 XDMA)
static const struct pci_device_id vgpu_pci_id_table[] = {
    { PCI_DEVICE(0x10EE, PCI_ANY_ID) },
    { 0, }
};
MODULE_DEVICE_TABLE(pci, vgpu_pci_id_table);

// static struct platform_driver vgpu_driver = {
//     .probe = vgpu_probe,
//     .remove = vgpu_remove,
//     .driver = {
//         .name = "vgpu_device",
//         .owner = THIS_MODULE,
//     },
// };

static struct pci_driver vgpu_pci_driver = {
    .name = "vgpu_core",
    .id_table = vgpu_pci_id_table,
    .probe = vgpu_probe,
    .remove = vgpu_remove,
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

    // result = platform_driver_register(&vgpu_driver);
    result = pci_register_driver(&vgpu_pci_driver);
    if (result < 0) {
        pr_err("vGPU-Core: failed to register driver\n");
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

    pci_unregister_driver(&vgpu_pci_driver);
    class_destroy(g_vgpu_class);
    unregister_chrdev_region(vgpu_dev_num, MAX_VGPU_DEVICES);

    pr_info("vGPU-Core: module unloaded\n");
}

module_init(vgpu_core_init);
module_exit(vgpu_core_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("vGPU Team");
MODULE_DESCRIPTION("Virtual PCIe GPU/Accelerator Core Driver (Real PCIe Mode)");
