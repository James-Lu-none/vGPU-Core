#include "../include/vgpu/vgpu_core.h"

long vgpu_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct vgpu_command user_cmd;
    
    switch (cmd) {
        case VGPU_IOC_SUBMIT_CMD:
            if (copy_from_user(&user_cmd, (struct vgpu_command __user *)arg, sizeof(user_cmd))) {
                return -EFAULT;
            }

            if (queue_mode == 0) {
                spin_lock(&g_vgpu_dev->global_lock);
                g_vgpu_dev->global_queue[g_vgpu_dev->tail] = user_cmd;
                g_vgpu_dev->tail = (g_vgpu_dev->tail + 1) % QUEUE_SIZE;
                spin_unlock(&g_vgpu_dev->global_lock);
            } else {
                struct vgpu_context *ctx = file->private_data;
                if (!ctx) return -EFAULT;
                
                ctx->private_queue[ctx->tail] = user_cmd;
                ctx->tail = (ctx->tail + 1) % QUEUE_SIZE;
            }
            break;

        case VGPU_IOC_DOORBELL:
            pr_info("vGPU-Core: Doorbell Rung! Notifying hardware...\n");
            if (g_vgpu_dev->data_buffer) {
                pr_info("vGPU-Core: [Data Path] GPU reading Data Buffer: '%s'\n", (char *)g_vgpu_dev->data_buffer);
            }
            queue_work(g_vgpu_dev->hw_wq, &g_vgpu_dev->hw_work);
            break;

        case VGPU_IOC_WAIT_FOR_IRQ:
            if (queue_mode == 0) {
                pr_info("vGPU-Core: [Global Queue] Process sleeping on wait_q...\n");
                g_vgpu_dev->irq_fired = 0;
                wait_event_interruptible(g_vgpu_dev->wait_q, g_vgpu_dev->irq_fired != 0);
                pr_info("vGPU-Core: [Global Queue] Process woken up by IRQ!\n");
            } else {
                struct vgpu_context *ctx = file->private_data;
                if (!ctx) return -EFAULT;
                pr_info("vGPU-Core: [Private Queue] Process sleeping on ctx %p wait_q...\n", ctx);
                ctx->irq_fired = 0;
                wait_event_interruptible(ctx->wait_q, ctx->irq_fired != 0);
                pr_info("vGPU-Core: [Private Queue] Process woken up by IRQ!\n");
            }
            break;

        default:
            return -ENOTTY;
    }

    return 0;
}
