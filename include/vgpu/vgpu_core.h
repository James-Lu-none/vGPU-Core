#ifndef _VGPU_CORE_H
#define _VGPU_CORE_H

#include <linux/module.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/workqueue.h>
#include <linux/wait.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/scatterlist.h>
#include "../uapi/vgpu_ioctl.h"

/*
 * Direct Host-to-CP Mailbox & Hardware IRQ MMIO Offset (BAR0)
 * 0x0000 ~ 0x3EFF: RISC-V On-Chip BRAM Code & Data Space
 * 0x3F00 ~ 0x3FFF: Direct Host-to-CP BRAM Mailbox Window (256-Byte Window)
 * 
 * Writing struct cuda_task_descriptor to BAR0 + 0x3F00 automatically 
 * triggers a 1-cycle hardware interrupt pulse to PicoRV32's irq[0] line
 */
#define VGPU_MAILBOX_OFFSET       0x3F00 /* Direct BAR0 BRAM Mailbox Offset */

/*
 * CUDA Task Descriptor Structure (64-byte aligned)
 * User Space & Driver -> Direct PCIe Write -> RISC-V Command Processor (PicoRV32)
 */
struct cuda_task_descriptor {
    u32 magic;         /* 0x43554441 ("CUDA") */
    u32 opcode;        /* 1: Add, 2: Mul, 3: Render, 4: SMEM_Write, 5: SMEM_Accumulate */
    u32 grid_dim_x;    /* Grid Dimension X */
    u32 grid_dim_y;    /* Grid Dimension Y */
    u32 block_dim_x;   /* Block Dimension X */
    u32 block_dim_y;   /* Block Dimension Y */
    u64 src_dma_addr;  /* Host Input DMA Buffer Address (PCIe Bus Address) */
    u64 dst_dma_addr;  /* Host Output DMA Buffer Address (PCIe Bus Address) */
    u32 num_elements;  /* Vector Element Count */
    u32 reserved[7];   /* Padding to 64 bytes */
} __packed __aligned(64);

#define QUEUE_SIZE 256

/*
 * Lock-Free Ring Buffer (Shared between CPU and FPGA)
 * This structure is mapped directly into the DMA buffer.
 * - CPU (Producer): Writes to cmds[tail], then uses smp_store_release(&tail).
 * - FPGA (Consumer): Reads cmds[head], processes, then DMA writes to 'head'.
 */
struct vgpu_ring_buffer {
    volatile u32 head; /* Updated by FPGA via DMA Write */
    volatile u32 tail; /* Updated by CPU (Host) */
    struct vgpu_command cmds[QUEUE_SIZE];
};

extern int queue_mode;
extern int dma_mode;

struct vgpu_context {
    // for one vgpu, each open context only needs the private queue
    // in multi vgpu, additional parameter dev is required to know
    // which vgpu device this context belongs to
    struct vgpu_dev *dev;
    struct vgpu_ring_buffer *ring; // Points to a separate DMA buffer if queue_mode == 1
    wait_queue_head_t wait_q;
    int irq_fired;
    struct list_head list_node;
};

/*
 * XDMA Hardware Descriptor Format (32-byte aligned)
 */
struct xdma_desc {
    u32 control;         /* Magic (0xAD4B0000), EOP (bit 0), IRQ (bit 1) */
    u32 bytes;           /* Transfer size in bytes */
    u64 src_addr;        /* Source DMA Address (Host System RAM) */
    u64 dst_addr;        /* Destination AXI Address (FPGA internal buffer) */
    u64 next_desc;       /* Next Descriptor DMA Address (Host System RAM) */
} __packed;

#define XDMA_DESC_MAGIC 0xAD4B0000
#define XDMA_DESC_EOP   (1u << 0)

struct vgpu_dev {
    // use minor number to identify each vgpu device in /dev/vgpuX
    int minor;
    struct cdev cdev;
    struct device *device;
    struct pci_dev *pci_dev;
    void __iomem *mmio_base; // Mapped PCIe Base Address Register 0 (BAR0) address
    int irq;                 // The IRQ number allocated by the PCI subsystem for this device

    struct vgpu_ring_buffer *ring; // Points to ring_buffer (used as global queue)
    spinlock_t global_lock;        // Lock among multiple CPU producers (threads)

    struct list_head ctx_list;
    spinlock_t ctx_lock;

    wait_queue_head_t wait_q;
    int irq_fired;

    void *ring_buffer;           // Ring Buffer (Consistent DMA)
    dma_addr_t dma_handle;

    struct xdma_desc *desc_ring; // XDMA Descriptor Ring (Coherent DMA Buffer)
    dma_addr_t desc_ring_dma_addr; // Bus Address of XDMA Descriptor Ring
    struct page **pinned_pages;  // Array to hold pinned pages (Max 32MB payload)
    int num_pinned_pages;
    
    struct scatterlist *sgl;     // For dma_map_sg (Scatter-Gather DMA)
    int sgl_nents;               // Number of SG entries returned by dma_map_sg
};

#define MAX_VGPU_DEVICES 4

extern struct class *g_vgpu_class;

long vgpu_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
int vgpu_mmap(struct file *file, struct vm_area_struct *vma);
void vgpu_hw_work_func(struct work_struct *work);
irqreturn_t vgpu_irq_handler(int irq, void *dev_id);

#endif /* _VGPU_CORE_H */
