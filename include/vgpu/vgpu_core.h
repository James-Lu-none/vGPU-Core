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
#include "../uapi/vgpu_ioctl.h"

#define QUEUE_SIZE 256

extern int queue_mode;
extern int dma_mode;

struct vgpu_context {
    // for one vgpu, each open context only needs the private queue
    // in multi vgpu, additional parameter dev is required to know
    // which vgpu device this context belongs to
    struct vgpu_dev *dev;
    struct vgpu_command private_queue[QUEUE_SIZE];
    int head;
    int tail;
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

    struct vgpu_command global_queue[QUEUE_SIZE];
    int head;
    int tail;
    spinlock_t global_lock;

    struct list_head ctx_list;
    spinlock_t ctx_lock;

    struct workqueue_struct *hw_wq;
    struct work_struct hw_work;
    wait_queue_head_t wait_q;
    int irq_fired;

    void *data_buffer;
    dma_addr_t dma_handle;
};

#define MAX_VGPU_DEVICES 4

extern struct class *g_vgpu_class;

long vgpu_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
int vgpu_mmap(struct file *file, struct vm_area_struct *vma);
void vgpu_hw_work_func(struct work_struct *work);

#endif /* _VGPU_CORE_H */
