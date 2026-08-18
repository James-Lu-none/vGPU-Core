#include "../include/vgpu/vgpu_core.h"

int vgpu_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct vgpu_context *ctx = file->private_data;
    struct vgpu_dev *dev = ctx->dev;
    size_t size = vma->vm_end - vma->vm_start;
    unsigned long pfn;

    pr_info("vGPU-Core: mmap called on vgpu%d, size: %zu, pgoff: %lu\n", dev->minor, size, vma->vm_pgoff);

    if (vma->vm_pgoff == 0) {
                 // Offset 0: Map the Command Queue (Ring Buffer)
         // This uses Consistent DMA mapping (uncached).
        if (size > PAGE_SIZE) return -EINVAL;

        if (dma_mode == 1) {
            return dma_mmap_coherent(&dev->pci_dev->dev, vma, 
                                     dev->data_buffer, 
                                     dev->dma_handle, size);
        } else {
            pfn = virt_to_phys(dev->data_buffer) >> PAGE_SHIFT;
            return remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot);
        }
    } else if (vma->vm_pgoff == 1) {
                 // Offset 1: Map the Data Payload Buffer
         // This uses standard cached memory, meaning CPU accesses are fast.
         // Cache flushes/invalidates will be handled by Streaming DMA API during ioctl.
        if (size > dev->payload_size) return -EINVAL;

        // Reset pgoff to 0 so remap_pfn_range doesn't add the offset to the physical address
        vma->vm_pgoff = 0;
        
        pfn = virt_to_phys(dev->payload_buffer) >> PAGE_SHIFT;
        return remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot);
    }

    return -EINVAL;
}
