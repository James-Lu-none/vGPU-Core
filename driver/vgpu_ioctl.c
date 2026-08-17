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
                u32 head, tail;

                // Protect multiple CPU producer threads from stepping on each other
                spin_lock(&dev->global_lock);
                
                /*
                 * Lock-Free Memory Barrier: smp_load_acquire()
                 * Guarantees that we read the most up-to-date 'head' updated by the FPGA DMA.
                 * Any memory operations after this will not be reordered before it.
                 */
                head = smp_load_acquire(&dev->ring->head);
                tail = dev->ring->tail;
                
                if ((tail + 1) % QUEUE_SIZE == head) {
                    spin_unlock(&dev->global_lock);
                    return -EBUSY; // Queue Full
                }
                
                // Write the command data into the DMA-mapped Ring Buffer
                dev->ring->cmds[tail] = user_cmd;
                
                /*
                 * Lock-Free Memory Barrier: smp_store_release()
                 * CRITICAL: Ensures that the user_cmd is completely committed to System RAM 
                 * BEFORE we update the tail pointer. If the CPU out-of-order execution writes 
                 * the tail first, the FPGA might fetch garbage data!
                 */
                smp_store_release(&dev->ring->tail, (tail + 1) % QUEUE_SIZE);
                
                spin_unlock(&dev->global_lock);
            } else {
                // 原本一個 context 所有的資訊都在 kernel space 的記憶體裡
                // 但在改成有實體 gpu 跟 dma 之後，每個 process (context) 除了原本 ctx 以外
                // 還需要一個獨立的 dma_alloc_coherent 拿來放 command ring buffer
                // 然後讓 hardware 端做硬體切換 context 的功能
                // 才能實現多租戶 lock free 的功能
                // ctx->private_queue[ctx->tail] = user_cmd;
                // ctx->tail = (ctx->tail + 1) % QUEUE_SIZE;
                pr_err("vGPU-Core: Private queue mode currently unmapped to DMA\n");
                return -ENOTSUPP;
            }
            break;

        case VGPU_IOC_DOORBELL_AND_WAIT:
            pr_info("vGPU-Core: [Atomic Submit & Wait] Ringing doorbell for vgpu%d...\n", dev->minor);
            if (dev->data_buffer) {
                pr_info("vGPU-Core: [Data Path] GPU reading Data Buffer: '%s'\n", (char *)dev->data_buffer);
            }

            // Clear irq_fired BEFORE ringing the doorbell to prevent Lost Wakeup.
            // If the FPGA fires the IRQ instantly, it will set dev->irq_fired = 1.
            // Then wait_event_interruptible will see the 1 and return immediately (no deadlock)
            if (queue_mode == 0) {
                dev->irq_fired = 0;
                iowrite32(1, dev->mmio_base + VGPU_DOORBELL_OFFSET);
                wait_event_interruptible(dev->wait_q, dev->irq_fired != 0);
                pr_info("vGPU-Core: [Global Queue] Process woken up by IRQ!\n");
            } else {
                ctx->irq_fired = 0;
                iowrite32(1, dev->mmio_base + VGPU_DOORBELL_OFFSET);
                wait_event_interruptible(ctx->wait_q, ctx->irq_fired != 0);
                pr_info("vGPU-Core: [Private Queue] Process woken up by IRQ!\n");
            }
            break;

        default:
            return -ENOTTY;
    }
    return 0;
}
