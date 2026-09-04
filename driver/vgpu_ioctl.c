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
                 * We use get_user_pages_fast to pin the User Space virtual memory pages
                 * into physical RAM, ensuring they aren't swapped out during DMA.
                 */
                int num_pages = (user_cmd.payload_size + PAGE_SIZE - 1) / PAGE_SIZE;
                if (num_pages > 8192) return -EINVAL; // Max 32MB for our demo
                
                dev->num_pinned_pages = get_user_pages_fast(user_cmd.payload_vaddr, num_pages, 
                                                            FOLL_WRITE, dev->pinned_pages);
                if (dev->num_pinned_pages < 0) {
                    pr_err("vGPU-Core: get_user_pages_fast failed\n");
                    return dev->num_pinned_pages;
                }
                
                /*
                 * Scatter-Gather DMA Mapping
                 * We map the pinned pages for DMA, obtaining physical bus addresses.
                 */
                dev->sgl = kmalloc_array(dev->num_pinned_pages, sizeof(struct scatterlist), GFP_KERNEL);
                if (!dev->sgl) return -ENOMEM;
                
                sg_init_table(dev->sgl, dev->num_pinned_pages);
                for (int i = 0; i < dev->num_pinned_pages; i++) {
                    sg_set_page(&dev->sgl[i], dev->pinned_pages[i], PAGE_SIZE, 0);
                }
                
                dev->sgl_nents = dma_map_sg(&dev->pci_dev->dev, dev->sgl, dev->num_pinned_pages, DMA_BIDIRECTIONAL);
                if (dev->sgl_nents == 0) {
                    pr_err("vGPU-Core: dma_map_sg failed\n");
                    kfree(dev->sgl);
                    dev->sgl = NULL;
                    return -ENOMEM;
                }
                
                /* 
                 * Build XDMA Descriptor Chain
                 * We construct the Scatter-Gather descriptors exactly as the Xilinx XDMA IP expects.
                 */
                struct scatterlist *sg;
                int i;
                for_each_sg(dev->sgl, sg, dev->sgl_nents, i) {
                    dev->desc_ring[i].src_addr = sg_dma_address(sg);
                    dev->desc_ring[i].dst_addr = 0x0;
                    dev->desc_ring[i].bytes    = sg_dma_len(sg);
                    dev->desc_ring[i].control  = XDMA_DESC_MAGIC;
                    // point to the next descriptor in the host
                    if (i < dev->sgl_nents - 1) {
                        dev->desc_ring[i].next_desc = dev->desc_ring_dma_addr + (i + 1) * sizeof(struct xdma_desc);
                    } else {
                        dev->desc_ring[i].next_desc = 0;
                        dev->desc_ring[i].control  |= XDMA_DESC_EOP; 
                    }
                }
                
                /*
                 * Start XDMA Host-to-Card (H2C) Transfer
                 * We write the address of our descriptor ring to the XDMA Config BAR (BAR1).
                 * This initiates the DMA engine on the FPGA, pulling data from Host RAM to FPGA DRAM.
                 */
                iowrite32(lower_32_bits(dev->desc_ring_dma_addr), dev->xdma_base + XDMA_H2C_CHAN0_SG_LO);
                iowrite32(upper_32_bits(dev->desc_ring_dma_addr), dev->xdma_base + XDMA_H2C_CHAN0_SG_HI);
                iowrite32(1, dev->xdma_base + XDMA_H2C_CHAN0_CTRL); // 1 = Run
                
                // For simplicity in this demo, we wait for XDMA to finish synchronously.
                // In a production environment, this should be interrupt-driven.
                // Status register is at 0x0040. Bit 0 is Busy.
                while ((ioread32(dev->xdma_base + 0x0040) & 1) != 0) {
                    cpu_relax();
                }
                iowrite32(0, dev->xdma_base + XDMA_H2C_CHAN0_CTRL); // Stop
                
                /*
                 * Unified Memory: Teardown
                 * Since the XDMA has successfully copied the data into the FPGA's DRAM,
                 * we no longer need to keep the Host pages pinned! We can unmap them immediately.
                 * This is a massive advantage: User Space memory isn't locked while the GPU computes.
                 */
                if (dev->sgl) {
                    dma_unmap_sg(&dev->pci_dev->dev, dev->sgl, dev->num_pinned_pages, DMA_BIDIRECTIONAL);
                    kfree(dev->sgl);
                    dev->sgl = NULL;
                }
                for (int j = 0; j < dev->num_pinned_pages; j++) {
                    put_page(dev->pinned_pages[j]);
                }
                dev->num_pinned_pages = 0;
            }

            if (queue_mode == 0) {
                u32 head, tail;

                // Protect multiple CPU producer threads from stepping on each other
                spin_lock(&dev->global_lock);
                
                // Read head/tail from FPGA BRAM (BAR0)
                head = ioread32(&dev->ring->head);
                tail = ioread32(&dev->ring->tail);
                
                if ((tail + 1) % QUEUE_SIZE == head) {
                    spin_unlock(&dev->global_lock);
                    return -EBUSY; // Queue Full
                }
                
                /* 
                 * Build Hardware Task Descriptor
                 * This is the payload the PicoRV32 firmware actually understands.
                 */
                struct cuda_task_descriptor task = {};
                task.magic        = 0x43554441; /* "CUDA" */
                task.opcode       = user_cmd.opcode;
                task.grid_dim_x   = 32;
                task.block_dim_x  = 32;
                task.num_elements = user_cmd.payload_size / sizeof(u64);
                
                // Because XDMA already copied the data to FPGA DRAM Address 0x0,
                // we tell the GPU Engine to fetch from Address 0x0 (Local memory), not Host memory!
                task.src_dma_addr = 0x0;

                // Write the task directly into the BRAM Ring Buffer
                memcpy_toio(&dev->ring->cmds[tail], &task, sizeof(task));
                
                // Update tail pointer in BRAM.
                // The PicoRV32 Firmware (polling the tail pointer) will notice this 
                // and fetch the task automatically. No explicit Doorbell needed!
                iowrite32((tail + 1) % QUEUE_SIZE, &dev->ring->tail);
                
                spin_unlock(&dev->global_lock);
            } else {
                pr_err("vGPU-Core: Private queue mode currently unsupported\n");
                return -ENOTSUPP;
            }
            break;

        case VGPU_IOC_DOORBELL:
            /*
             * VGPU_IOC_DOORBELL is repurposed as a "Wait for Completion" signal.
             * Instead of relying on a hardware IRQ (which historically required the PicoRV32 
             * to crash/trap to signal the host), we simply poll the 'head' pointer in BRAM.
             * When head == tail, it means the GPU has finished all tasks in the queue.
             */
            pr_info("vGPU-Core: Waiting for GPU to finish all tasks in queue...\n");

            if (queue_mode == 0) {
                u32 tail = ioread32(&dev->ring->tail);
                
                // Poll the head pointer updated by PicoRV32
                while (ioread32(&dev->ring->head) != tail) {
                    // In a production driver, we would use a timeout or yield the CPU here
                    // to prevent locking up the system if the GPU hangs.
                    schedule(); // Yield CPU to other tasks while waiting
                }
                
                pr_info("vGPU-Core: All GPU tasks completed successfully!\n");
            } else {
                pr_err("vGPU-Core: Private queue mode currently unsupported\n");
                return -ENOTSUPP;
            }
            break;

        default:
            return -ENOTTY;
    }
    return 0;
}
