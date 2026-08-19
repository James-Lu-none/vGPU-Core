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

            if (user_cmd.payload_size > 0 && user_cmd.payload_vaddr != 0) {
                /*
                 * Unified Memory: Demand Paging (Software Page Table)
                 * 1. Calculate how many 4KB pages we need.
                 * 2. Call get_user_pages_fast() to trigger Linux Demand Paging.
                 *    If the pages aren't physically allocated yet, Linux will allocate them now.
                 * 3. Lock/Pin these pages in physical RAM so they don't get swapped out.
                 */
                int num_pages = (user_cmd.payload_size + PAGE_SIZE - 1) / PAGE_SIZE;
                if (num_pages > 512) return -EINVAL; // Max 2MB for our demo
                
                dev->num_pinned_pages = get_user_pages_fast(user_cmd.payload_vaddr, num_pages, 
                                                            FOLL_WRITE, dev->pinned_pages);
                if (dev->num_pinned_pages < 0) {
                    pr_err("vGPU-Core: get_user_pages_fast failed\n");
                    return dev->num_pinned_pages;
                }
                
                /*
                 * Scatter-Gather Mapping:
                 * Map each individual scattered physical page to the DMA bus.
                 * Write the resulting Bus Addresses into our coherent Page Table buffer.
                 * The FPGA will read this Page Table to locate the scattered data.
                 */
                for (int i = 0; i < dev->num_pinned_pages; i++) {
                    dma_addr_t dma_addr = dma_map_page(&dev->pci_dev->dev, dev->pinned_pages[i], 
                                                       0, PAGE_SIZE, DMA_BIDIRECTIONAL);
                    if (dma_mapping_error(&dev->pci_dev->dev, F)) {
                        pr_err("vGPU-Core: dma_map_page failed\n");
                        return -ENOMEM;
                    }
                    dev->page_table[i] = dma_addr;
                }
                pr_info("vGPU-Core: Demand Paging: Pinned %d pages and built Page Table\n", dev->num_pinned_pages);
            }

            if (queue_mode == 0) {
                u32 head, tail;

                // Protect multiple CPU producer threads from stepping on each other
                spin_lock(&dev->global_lock);
                
                // Lock-Free Memory Barrier: smp_load_acquire()
                // Guarantees that we read the most up-to-date 'head' updated by the FPGA DMA.
                // Any memory operations after this will not be reordered before it.
                head = smp_load_acquire(&dev->ring->head);
                tail = dev->ring->tail;
                
                if ((tail + 1) % QUEUE_SIZE == head) {
                    spin_unlock(&dev->global_lock);
                    return -EBUSY; // Queue Full
                }
                
                // Write the command data into the DMA-mapped Ring Buffer
                dev->ring->cmds[tail] = user_cmd;
                
                // Lock-Free Memory Barrier: smp_store_release()
                // CRITICAL: Ensures that the user_cmd is completely committed to System RAM 
                // BEFORE we update the tail pointer. If the CPU out-of-order execution writes 
                // the tail first, the FPGA might fetch garbage data!
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

        case VGPU_IOC_DOORBELL:
            pr_info("vGPU-Core: [Atomic Submit & Wait] Ringing doorbell for vgpu%d...\n", dev->minor);

            // Clear irq_fired BEFORE ringing the doorbell to prevent Lost Wakeup.
            // If the FPGA fires the IRQ instantly, it will set dev->irq_fired = 1.
            // Then wait_event_interruptible will see the 1 and return immediately (no deadlock)
            if (queue_mode == 0) {
                dev->irq_fired = 0;
                // // Streaming DMA: Cache Flush (Clean) (old implementation when using fixed continuous 1M payload buffer)
                // // Before we tell the FPGA to start computing, we MUST flush any 
                // // modified payload data from the CPU's L1/L2 cache out to physical RAM.
                // // If we don't, the FPGA will DMA read stale garbage data from RAM.
                // if (dma_mode == 1 && dev->payload_buffer) {
                //    dma_sync_single_for_device(&dev->pci_dev->dev, dev->payload_dma_handle,
                //                               dev->payload_size, DMA_BIDIRECTIONAL);
                // }

                // The dma_map_page() call in SUBMIT_CMD implicitly handled Cache Flush for us.

                iowrite32(1, dev->mmio_base + VGPU_DOORBELL_OFFSET);
                wait_event_interruptible(dev->wait_q, dev->irq_fired != 0);
                
                // // Streaming DMA: Cache Invalidate (old implementation when using fixed continuous 1M payload buffer)
                // // The FPGA has finished computing and DMA-written the results to RAM.
                // // We MUST invalidate the CPU's cache for this region, forcing the CPU 
                // // to fetch the fresh results from RAM instead of reading stale cache lines.
                // if (dma_mode == 1 && dev->payload_buffer) {
                //     dma_sync_single_for_cpu(&dev->pci_dev->dev, dev->payload_dma_handle,
                //                             dev->payload_size, DMA_BIDIRECTIONAL);
                // }

                /*
                 * Unified Memory: Teardown
                 * The FPGA has finished computing. We must unmap the DMA addresses
                 * and release (put_page) the locked physical pages so Linux can 
                 * swap them out or free them if necessary.
                 */
                if (dev->num_pinned_pages > 0) {
                    for (int i = 0; i < dev->num_pinned_pages; i++) {
                        dma_unmap_page(&dev->pci_dev->dev, dev->page_table[i], PAGE_SIZE, DMA_BIDIRECTIONAL);
                        put_page(dev->pinned_pages[i]);
                    }
                    pr_info("vGPU-Core: Unified Memory: Unmapped and unpinned %d pages\n", dev->num_pinned_pages);
                    dev->num_pinned_pages = 0;
                }

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
