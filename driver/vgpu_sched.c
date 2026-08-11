#include "../include/vgpu/vgpu_core.h"

void vgpu_hw_work_func(struct work_struct *work)
{
    pr_info("vGPU-Core: [Hardware] GPU is processing commands...\n");
    
    msleep(500); 

    if (queue_mode == 0) {
        spin_lock(&g_vgpu_dev->global_lock);
        g_vgpu_dev->head = g_vgpu_dev->tail; 
        g_vgpu_dev->irq_fired = 1;
        spin_unlock(&g_vgpu_dev->global_lock);
        
        pr_info("vGPU-Core: [Hardware] Global Queue compute done. Firing IRQ...\n");
        wake_up_interruptible(&g_vgpu_dev->wait_q);
    } else {
        struct vgpu_context *ctx;
        
        spin_lock(&g_vgpu_dev->ctx_lock);
        list_for_each_entry(ctx, &g_vgpu_dev->ctx_list, list_node) {
            if (ctx->head != ctx->tail) {
                ctx->head = ctx->tail; 
                ctx->irq_fired = 1;
                pr_info("vGPU-Core: [Hardware] Private Queue compute done for ctx %p. Firing IRQ...\n", ctx);
                wake_up_interruptible(&ctx->wait_q);
            }
        }
        spin_unlock(&g_vgpu_dev->ctx_lock);
    }
}
