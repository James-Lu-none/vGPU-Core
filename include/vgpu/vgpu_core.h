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

#define VGPU_RING_OFFSET 0x0001_8000 // Direct BAR0 BRAM Ring Buffer Offset (about 32KB)
#define QUEUE_SIZE 512 // size of cuda_task_descriptor is 64 bytes, 32KB/64 = 512

/*
 * XDMA Hardware Engine Control Registers (Mapped to BAR1)
 * These registers are used to program the Xilinx XDMA IP's internal DMA engine.
 * We use them to initiate a Host-to-Card (H2C) DMA transfer from Host RAM to FPGA DRAM.
 * Why do we need this? Because copying large payloads (like images/matrices) via
 * MMIO (CPU writes) is extremely slow. DMA offloads this to hardware.
 */
#define XDMA_H2C_CHAN0_CTRL   0x0004 // H2C Channel 0 Control Register (Write 1 to Run)
#define XDMA_H2C_CHAN0_SG_LO  0x0080 // H2C Channel 0 Scatter-Gather First Descriptor Address Low (32-bit)
#define XDMA_H2C_CHAN0_SG_HI  0x0084 // H2C Channel 0 Scatter-Gather First Descriptor Address High (32-bit)

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
    u32 reserved[5];   /* Padding to 64 bytes */
} __packed __aligned(64);

/*
 * Lock-Free Ring Buffer (Now mapped directly into FPGA BRAM via BAR0)
 * Why place it in BRAM instead of Host RAM? 
 * Because the PicoRV32 (FPGA Firmware) does not have an AXI Master to initiate 
 * PCIe reads from Host RAM. By placing the Ring Buffer in BRAM, both Host CPU 
 * and PicoRV32 can access it easily (Host via PCIe MMIO, PicoRV32 via local AXI).
 * This enables an asynchronous architecture where Host pushes multiple tasks 
 * without waiting, and FPGA fetches them without blocking.
 */
struct vgpu_ring_buffer {
    volatile u32 head; /* Updated by FPGA (Consumer) */
    volatile u32 tail; /* Updated by CPU Host (Producer) */
    struct cuda_task_descriptor cmds[QUEUE_SIZE];
};

extern int queue_mode;

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
    void __iomem *mmio_base; // Mapped PCIe BAR0 (AXI-Lite to BRAM & GPU Engine)
    void __iomem *xdma_base; // Mapped PCIe BAR1 (XDMA Config Registers)
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
