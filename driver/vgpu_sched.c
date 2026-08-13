#include "../include/vgpu/vgpu_core.h"

void vgpu_hw_work_func(struct work_struct *work)
{
    struct vgpu_dev *dev = container_of(work, struct vgpu_dev, hw_work);

    pr_info("vGPU-Core: [Hardware] vgpu%d is processing commands...\n", dev->minor);
    
    msleep(500); 

    if (queue_mode == 0) {
        spin_lock(&dev->global_lock);
        dev->head = dev->tail; 
        dev->irq_fired = 1;
        spin_unlock(&dev->global_lock);
        
        pr_info("vGPU-Core: [Hardware] Global Queue compute done for vgpu%d. Firing IRQ...\n", dev->minor);
        wake_up_interruptible(&dev->wait_q);
    } else {
        struct vgpu_context *ctx;
        
        spin_lock(&dev->ctx_lock);
        list_for_each_entry(ctx, &dev->ctx_list, list_node) {
            if (ctx->head != ctx->tail) {
                ctx->head = ctx->tail; 
                ctx->irq_fired = 1;
                pr_info("vGPU-Core: [Hardware] Private Queue compute done on vgpu%d for ctx %p. Firing IRQ...\n", dev->minor, ctx);
                wake_up_interruptible(&ctx->wait_q);
            }
        }
        spin_unlock(&dev->ctx_lock);
    }
}
