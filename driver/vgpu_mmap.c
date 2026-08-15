#include "../include/vgpu/vgpu_core.h"

int vgpu_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct vgpu_context *ctx = file->private_data;
    struct vgpu_dev *dev = ctx->dev;
    size_t size = vma->vm_end - vma->vm_start;
    unsigned long pfn;

    pr_info("vGPU-Core: mmap called on vgpu%d, size: %zu\n", dev->minor, size);

    if (dma_mode == 1) {
        return dma_mmap_coherent(&dev->pci_dev->dev, vma, 
                                 dev->data_buffer, 
                                 dev->dma_handle, size);
    } else {
        if (size > PAGE_SIZE) {
            return -EINVAL;
        }

        pfn = virt_to_phys(dev->data_buffer) >> PAGE_SHIFT;
        if (remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot)) {
            return -EAGAIN;
        }
    }
    
    return 0;
}
