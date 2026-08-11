#include "../include/vgpu/vgpu_core.h"

int vgpu_mmap(struct file *file, struct vm_area_struct *vma)
{
    unsigned long size = vma->vm_end - vma->vm_start;
    
    if (size > PAGE_SIZE) return -EINVAL;
    
    if (dma_mode == 1) {
        pr_info("vGPU-Core: mmap via dma_mmap_coherent\n");
        return dma_mmap_coherent(&g_vgpu_dev->pdev->dev, vma, 
                                 g_vgpu_dev->data_buffer, 
                                 g_vgpu_dev->dma_handle, size);
    } else {
        unsigned long pfn;
        pr_info("vGPU-Core: mmap via remap_pfn_range\n");
        
        pfn = virt_to_phys(g_vgpu_dev->data_buffer) >> PAGE_SHIFT;
        
        if (remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot)) {
            pr_err("vGPU-Core: remap_pfn_range failed\n");
            return -EAGAIN;
        }
    }
    return 0;
}
