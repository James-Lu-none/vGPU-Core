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
 * MMIO Register Offsets
 * These are the offsets within BAR0 that the FPGA exposes.
 * 
 * In a real FPGA design (e.g. using Xilinx XDMA IP), the Address mapping works as follows:
 * 1. CPU writes to (dev->mmio_base + 0x40).
 * 2. This creates a PCIe Memory Write TLP aimed at BAR0 + 0x40.
 * 3. XDMA IP receives the TLP and applies "PCIe-to-AXI Translation", converting it to an 
 *    AXI-Lite transaction with address (AXI_BASE_ADDR + 0x40) on its M_AXI_LITE port.
 * 4. The Vivado AXI Interconnect (configured via Address Editor) routes this transaction
 *    to our custom AXI-Lite Slave IP.
 * 5. Our custom IP decodes the 0x00 offset (slv_reg0) to trigger the hardware state machine (Doorbell).
 * Driver -> PCIe -> XDMA -> AXI Interconnect -> Custom IP
 */

#define VGPU_DOORBELL_OFFSET      0x00 /* slv_reg0: Trigger DMA */
#define VGPU_INT_STATUS_OFFSET    0x04 /* slv_reg1: Read IRQ status */
#define VGPU_INT_ACK_OFFSET       0x08 /* slv_reg2: Clear IRQ status */
#define VGPU_DMA_ADDR_LOW_OFFSET  0x0C /* slv_reg3: DMA Buffer Bus Address (Lower 32-bit) */
#define VGPU_DMA_ADDR_HIGH_OFFSET 0x10 /* slv_reg4: DMA Buffer Bus Address (Upper 32-bit) */
#define VGPU_PAYLOAD_ADDR_LOW_OFFSET  0x14 /* slv_reg5: Page Table Base Address (Lower 32-bit) */
#define VGPU_PAYLOAD_ADDR_HIGH_OFFSET 0x18 /* slv_reg6: Page Table Base Address (Upper 32-bit) */
#define VGPU_UVM_MODE_OFFSET          0x20 /* slv_reg7: 0=Direct/IOMMU, 1=Scatter-Gather */

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
extern int uvm_mode;

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

    dma_addr_t *page_table;      // Page Table (Array of dma_addr_t)
    dma_addr_t page_table_dma;   // Bus Address of the Page Table
    struct page **pinned_pages;  // Array to hold pinned pages (Max 32MB payload)
    int num_pinned_pages;
    
    struct scatterlist *sgl;     // For IOMMU dma_map_sg (IOVA i/o virtual address)
    int sgl_nents;               // Number of SG entries returned by dma_map_sg
};

#define MAX_VGPU_DEVICES 4

extern struct class *g_vgpu_class;

long vgpu_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
int vgpu_mmap(struct file *file, struct vm_area_struct *vma);
void vgpu_hw_work_func(struct work_struct *work);
irqreturn_t vgpu_irq_handler(int irq, void *dev_id);

#endif /* _VGPU_CORE_H */
