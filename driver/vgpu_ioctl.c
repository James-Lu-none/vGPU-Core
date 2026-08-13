#include "../include/vgpu/vgpu_core.h"

long vgpu_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct vgpu_context *ctx = file->private_data;
    struct vgpu_dev *dev = ctx->dev;
    struct vgpu_command user_cmd;
    
    if (_IOC_TYPE(cmd) != VGPU_IOC_MAGIC) return -ENOTTY;
    if (_IOC_NR(cmd) > VGPU_IOC_MAXNR) return -ENOTTY;

    switch (cmd) {
        case VGPU_IOC_SUBMIT_CMD:
            if (copy_from_user(&user_cmd, (struct vgpu_command __user *)arg, sizeof(user_cmd))) {
                return -EFAULT;
            }

            if (queue_mode == 0) {
                spin_lock(&dev->global_lock);
                dev->global_queue[dev->tail] = user_cmd;
                dev->tail = (dev->tail + 1) % QUEUE_SIZE;
                spin_unlock(&dev->global_lock);
            } else {
                ctx->private_queue[ctx->tail] = user_cmd;
                ctx->tail = (ctx->tail + 1) % QUEUE_SIZE;
            }
            break;

        case VGPU_IOC_DOORBELL:
            pr_info("vGPU-Core: Doorbell Rung! Notifying hardware %d...\n", dev->minor);
            if (dev->data_buffer) {
                pr_info("vGPU-Core: [Data Path] GPU reading Data Buffer: '%s'\n", (char *)dev->data_buffer);
            }
            queue_work(dev->hw_wq, &dev->hw_work);
            break;

        case VGPU_IOC_WAIT_FOR_IRQ:
            if (queue_mode == 0) {
                pr_info("vGPU-Core: [Global Queue] Process sleeping on wait_q for vgpu%d...\n", dev->minor);
                dev->irq_fired = 0;
                wait_event_interruptible(dev->wait_q, dev->irq_fired != 0);
                pr_info("vGPU-Core: [Global Queue] Process woken up by IRQ!\n");
            } else {
                pr_info("vGPU-Core: [Private Queue] Process sleeping on ctx %p wait_q for vgpu%d...\n", ctx, dev->minor);
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
