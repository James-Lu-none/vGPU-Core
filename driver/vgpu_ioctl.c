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
            /*
             * MMIO Write: Ringing the Doorbell
             * Instead of simulating hardware with a Kernel Workqueue, we now
             * write a value directly to the FPGA's Base Address Register (BAR0).
             * iowrite32() translates to an atomic Memory Write TLP (Transaction Layer Packet)
             * over the PCIe bus, telling the FPGA: "Data is ready in memory, start DMA!"
             */
            pr_info("vGPU-Core: Doorbell Rung! Notifying FPGA %d via MMIO...\n", dev->minor);
            if (dev->data_buffer) {
                pr_info("vGPU-Core: [Data Path] GPU reading Data Buffer: '%s'\n", (char *)dev->data_buffer);
            }
            // Write 1 to the Doorbell offset in BAR0
            iowrite32(1, dev->mmio_base + VGPU_DOORBELL_OFFSET);
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
