#include "../include/vgpu/vgpu_core.h"

/*
 * IRQ Handler (Top Half)
 * When the FPGA finishes its DMA operation, it sends an MSI/MSI-X interrupt over PCIe.
 * The Linux Kernel's generic IRQ layer catches it and executes this function.
 * 
 * NOTE: This function runs in an "Interrupt Context". It MUST NOT sleep!
 * (That means no msleep, no kmalloc with GFP_KERNEL, no wait_event).
 */
irqreturn_t vgpu_irq_handler(int irq, void *dev_id)
{
    struct vgpu_dev *dev = (struct vgpu_dev *)dev_id;
    u32 status;

    /*
     * Step 1: Read and clear the hardware interrupt status.
     * We read the Interrupt Status Register (BAR0 + 0x44) to verify if the FPGA 
     * actually generated this interrupt. If so, we clear it by writing to the 
     * Acknowledge Register (BAR0 + 0x48).
     */
    status = ioread32(dev->mmio_base + VGPU_INT_STATUS_OFFSET);
    if (!status) {
        /* Not our interrupt. Return IRQ_NONE to let the kernel know. */
        return IRQ_NONE; 
    }
    
    // Clear the interrupt on the FPGA side so it doesn't keep firing
    iowrite32(status, dev->mmio_base + VGPU_INT_ACK_OFFSET);

    pr_info("vGPU-Core: [Hardware] IRQ received on vgpu%d! (status: 0x%x)\n", dev->minor, status);
    
    /*
     * Step 2: Update software state & Wake up User Space.
     * In the old workqueue simulation, we used msleep() to simulate compute time.
     * Here, the compute time *actually happened in the hardware*, so we are already done.
     * We just need to update the Ring Buffer head and wake up the User Space thread.
     */
    if (queue_mode == 0) {
        spin_lock(&dev->global_lock);
        dev->head = dev->tail; 
        dev->irq_fired = 1;
        spin_unlock(&dev->global_lock);
        
        // Wake up any thread stuck in VGPU_IOC_WAIT_FOR_IRQ
        wake_up_interruptible(&dev->wait_q);
    } else {
        struct vgpu_context *ctx;
        
        spin_lock(&dev->ctx_lock);
        list_for_each_entry(ctx, &dev->ctx_list, list_node) {
            if (ctx->head != ctx->tail) {
                ctx->head = ctx->tail; 
                ctx->irq_fired = 1;
                wake_up_interruptible(&ctx->wait_q);
            }
        }
        spin_unlock(&dev->ctx_lock);
    }

    /* Tell the kernel we successfully handled the interrupt */
    return IRQ_HANDLED;
}
