// reference:
// 0. kzalloc and kfree
#include <linux/slab.h>
// 1. module_init(), module_exit()
// https://github.com/torvalds/linux/blob/master/include/linux/module.h
// https://github.com/torvalds/linux/blob/master/include/linux/init.h 
#include <linux/init.h>
#include <linux/module.h>
// 2. alloc_chrdev_region(dev_t *, unsigned, unsigned, const char *);, unregister_chrdev_region(), dev_t
// https://github.com/torvalds/linux/blob/master/include/linux/fs.h 
#include <linux/fs.h>
// 3. cdev_alloc(), cdev_add(), cdev_del(), cdev_init()
// https://github.com/torvalds/linux/blob/master/include/linux/cdev.h 
#include <linux/cdev.h>
#include <linux/uaccess.h> // for copy_from_user

#include <linux/list.h> // Intrusive Doubly Linked List for context

#include "vgpu_ioctl.h"

#define QUEUE_SIZE 128

// 架構切換開關 (Module Parameter)
// 0: Global Shared Queue (Mode A)
// 1: Private Context Queue (Mode B)
static int vgpu_mode = 0;
module_param(vgpu_mode, int, 0444);
MODULE_PARM_DESC(vgpu_mode, "0: Global Shared Queue (Mode A), 1: Private Context Queue (Mode B)");

struct vgpu_context {
    struct vgpu_command private_queue[QUEUE_SIZE];
    int head;
    int tail;
    struct list_head list_node; // 這是用來串接在 vgpu_dev->ctx_list 上面的節點
};

struct vgpu_dev {
    dev_t dev;          // 儲存裝置號 (Major/Minor number)
    struct cdev cdev;     // 儲存核心的 cdev 結構
    struct device *device; // 儲存核心的 device 結構

    // 因為這個結構體的生命週期跟硬體一樣長。只要裝置插在主機板上，這個結構體就在
    // 所以裡面通常會塞所有全域狀態，包含 dev_t, cdev, 暫存器(Registers)的記憶體映射位址 (MMIO base address), IRQ 中斷號碼。
    // 全域共享資源、控制硬體電源狀態的鎖、全域的 Wait Queue, 排程器 (Scheduler)...
    // 而如果今天這台機器插了 4 張 vGPU 卡，我們只需要 kzalloc 4 個 struct vgpu_dev 就可以了，每張卡都有自己獨立的 cdev、lock 和 queue，彼此不衝突
    // 所以把所有的資源都集中在 struct vgpu_dev 結構體裡是 best practice

    /* [Mode A] Global Shared Ring Buffer */
    struct vgpu_command global_queue[QUEUE_SIZE];
    int head;
    int tail;
    spinlock_t global_lock;

    /* [Mode B] Private Context Queue (Single Process Single Context) */
    struct list_head ctx_list; // 這是清單的「頭」
    spinlock_t ctx_lock;       // 保護這個清單的鎖
};

struct vgpu_dev *g_vgpu_dev = NULL; // 全域變數，指向我們建立的裝置結構
struct class *g_vgpu_class = NULL;

static int vgpu_open(struct inode *inode, struct file *file)
{
    pr_info("vGPU-Core: open\n");
    if (vgpu_mode == 0){
        pr_info("vGPU-Core: Global Shared Queue (Mode A), skip context allocate\n");
    } else {
        pr_info("vGPU-Core: Private Context Queue (Mode B), start to allocate context\n");
        struct vgpu_context *ctx;
        
        ctx = kzalloc(sizeof(struct vgpu_context), GFP_KERNEL);
        if (!ctx) {
            pr_err("vGPU-Core: failed to allocate context structure\n");
            return -ENOMEM;
        }
        
        // 將這個 Context 加進全域清單中
        spin_lock(&g_vgpu_dev->ctx_lock);
        list_add_tail(&ctx->list_node, &g_vgpu_dev->ctx_list);
        spin_unlock(&g_vgpu_dev->ctx_lock);
        
        file->private_data = ctx;
    }
    return 0;
}

static int vgpu_release(struct inode *inode, struct file *file)
{
    pr_info("vGPU-Core: release\n");
    if (vgpu_mode == 0){
        pr_info("vGPU-Core: Global Shared Queue (Mode A), skip context allocate\n");
    } else {
        pr_info("vGPU-Core: Private Context Queue (Mode B), start to release context\n");
        struct vgpu_context *ctx = file->private_data;
        if (ctx) {
            // 從全域清單中移除這個 Context
            spin_lock(&g_vgpu_dev->ctx_lock);
            list_del(&ctx->list_node);
            spin_unlock(&g_vgpu_dev->ctx_lock);
            
            kfree(ctx);
        }
    }
    return 0;
}

static char *msg_Ptr="Hi i am vGPU driver\n";
static ssize_t vgpu_read(struct file *filp,
   char *buffer,    /* The buffer to fill with data */
   size_t length,   /* The length of the buffer     */
   loff_t *offset)  /* Our offset in the file       */
{
   /* Number of bytes actually written to the buffer */
   int bytes_read = 0;

   /* If we're at the end of the message, return 0 signifying end of file */
   if (*msg_Ptr == 0) return 0;

   /* Actually put the data into the buffer */
   while (length && *msg_Ptr)  {

        /* The buffer is in the user data segment, not the kernel segment;
         * assignment won't work.  We have to use put_user which copies data from
         * the kernel data segment to the user data segment. */
         put_user(*(msg_Ptr++), buffer++);

         length--;
         bytes_read++;
   }

   /* Most read functions return the number of bytes put into the buffer */
   return bytes_read;
}

static long vgpu_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct vgpu_command user_cmd;
    
    switch (cmd) {
        case VGPU_IOC_SUBMIT_CMD:
            // 1. 安全地把 User Space 的資料搬到 Kernel Space
            if (copy_from_user(&user_cmd, (struct vgpu_command __user *)arg, sizeof(user_cmd))) {
                return -EFAULT; // 存取錯誤，可能是非法記憶體位址
            }

            if (vgpu_mode == 0) {
                // [Mode A] Global Queue，需要上鎖保護
                spin_lock(&g_vgpu_dev->global_lock);
                g_vgpu_dev->global_queue[g_vgpu_dev->tail] = user_cmd;
                g_vgpu_dev->tail = (g_vgpu_dev->tail + 1) % QUEUE_SIZE;
                
                // pr_info("vGPU-Core: [Mode A] Received command: opcode=%u, op1=%u, op2=%u. Queue Tail: %d\n",
                //        user_cmd.opcode, user_cmd.operand1, user_cmd.operand2, g_vgpu_dev->tail);
                // 釋放lock
                spin_unlock(&g_vgpu_dev->global_lock);
            } else {
                // [Mode B] 從 private_data 取出專屬 Queue，直接寫入 (Lock-free!)
                struct vgpu_context *ctx = file->private_data;
                if (!ctx) return -EFAULT;
                
                ctx->private_queue[ctx->tail] = user_cmd;
                ctx->tail = (ctx->tail + 1) % QUEUE_SIZE;
                
                // pr_info("vGPU-Core: [Mode B] Received command: opcode=%u. Private Tail: %d\n",
                //        user_cmd.opcode, ctx->tail);
            }
            break;

        case VGPU_IOC_DOORBELL:
            pr_info("vGPU-Core: Doorbell Rung! Hardware is starting to process commands...\n");
            if (vgpu_mode == 1) {
                struct vgpu_context *ctx;
                int count = 0;
                
                spin_lock(&g_vgpu_dev->ctx_lock);
                list_for_each_entry(ctx, &g_vgpu_dev->ctx_list, list_node) {
                    pr_info("vGPU-Core: [Scheduler] Checking context %p, Head: %d, Tail: %d\n", 
                            ctx, ctx->head, ctx->tail);
                    count++;
                }
                spin_unlock(&g_vgpu_dev->ctx_lock);
                pr_info("vGPU-Core: [Scheduler] Checked %d active contexts.\n", count);
            }
            break;

        default:
            return -ENOTTY;
    }
    
    return 0;
}

// 告訴 Kernel 當使用者對 /dev/vgpu0 按下 open()、ioctl() 時，要執行我這裡定義的哪個函數
static const struct file_operations vgpu_core_fops = {
    .owner          = THIS_MODULE,
    .read           = vgpu_read,
    .open           = vgpu_open,
    .release        = vgpu_release,
    // .ioctl was removed in kernel version 2.6.39, which is slower since it uses BKL. 
    // ioctl是舊時代使用BKL的kernel function, 現在使用 unlocked_ioctl 的意思是
    // 我不需要 kernel 幫我拿大鎖來保護 vgpu_dev 資料結構裡的數據
    // 而是開發者要自己想辦法用正確的鎖來保護我自己的數據
    .unlocked_ioctl = vgpu_ioctl,
};


// 自定義 initfn
// __init 是一個標記，告訴 Kernel 這個函數只有在初始化時會用到，
// 執行完後它的記憶體就可以被回收，節省空間。
static int __init vgpu_core_init(void)
{
    int result;
    pr_info("vGPU-Core: module loaded\n");
    // equivalent to printk(KERN_INFO "vGPU-Core: module loaded\n");
    // 1. alloc 結構體
    g_vgpu_dev = kzalloc(sizeof(struct vgpu_dev), GFP_KERNEL);
    if (!g_vgpu_dev) {
        pr_err("vGPU-Core: failed to allocate device structure\n");
        return -ENOMEM;
    }
    
    // 初始化 Global Ring Buffer 與 Lock (Mode A)
    spin_lock_init(&g_vgpu_dev->global_lock);
    g_vgpu_dev->head = 0;
    g_vgpu_dev->tail = 0;
    // Mode B
    spin_lock_init(&g_vgpu_dev->ctx_lock);
    INIT_LIST_HEAD(&g_vgpu_dev->ctx_list);

    // 2. 取得裝置號
    // 0 means base number, 1 means number of devices
    // -> /dev/vgpu_core0
    // 0, 4 -> /dev/vgpu_core0 ~ /dev/vgpu_core3
    // the name "vgpu_core" here is for kernel, will create under entry in /proc/devices
    result = alloc_chrdev_region(&g_vgpu_dev->dev, 0, 1, "vgpu_core");
    if (result < 0) {
        pr_err("vGPU-Core: failed to allocate device number\n");
        goto err_alloc_chrdev_region;
    }
    // 3. 建立 cdev (character device)
    cdev_init(&g_vgpu_dev->cdev, &vgpu_core_fops);
    result = cdev_add(&g_vgpu_dev->cdev, g_vgpu_dev->dev, 1);
    if (result < 0) {
        goto err_cdev_add;
    }

    // 4. 建立 device class
    // 在 Linux 裡，建立 Device 之前，必須先把它歸屬於某個 Class
    g_vgpu_class = class_create("vgpu_class");
    if (IS_ERR(g_vgpu_class)) {
        pr_err("vGPU-Core: failed to create class\n");
        result = PTR_ERR(g_vgpu_class);
        goto err_class_destroy;
    }
    // 5. 建立 udev (user space device /dev/vgpu0)
    g_vgpu_dev->device = device_create(
        g_vgpu_class, // parent class
        NULL,  
        g_vgpu_dev->dev, // device number
        NULL,        // device data
        "vgpu0"      // device name
    );
    if (IS_ERR(g_vgpu_dev->device)) {
        pr_err("vGPU-Core: failed to create device\n");
        result = PTR_ERR(g_vgpu_dev->device);
        goto err_device_create;
    }
    pr_info("vGPU-Core: module loaded successfully\n");
    return 0;

// Cascading error cleanup paths
err_device_create:
    class_destroy(g_vgpu_class);
err_class_destroy:
    cdev_del(&g_vgpu_dev->cdev);
err_cdev_add:
    unregister_chrdev_region(g_vgpu_dev->dev, 1);
err_alloc_chrdev_region:
    kfree(g_vgpu_dev);
    
    return result;

}

// 自定義 exitfn
// __exit 告訴 Kernel 這個函數只有在模組卸載時才會呼叫
static void __exit vgpu_core_exit(void)
{
    if (g_vgpu_dev) {
        // 5. 移除 udev 
        device_destroy(
            g_vgpu_class,
            g_vgpu_dev->dev
        );
        // 4. 移除 class
        class_destroy(g_vgpu_class);
        // 3. 移除 cdev
        cdev_del(&g_vgpu_dev->cdev);
        // 2. 釋放裝置號碼 (第一個參數是起始裝置號，第二個參數是數量)
        unregister_chrdev_region(g_vgpu_dev->dev, 1);
        // 1. 釋放結構體記憶體
        kfree(g_vgpu_dev);
    }
    pr_info("vGPU-Core: module unloaded\n");
}

module_init(vgpu_core_init);
module_exit(vgpu_core_exit);

// 如果不宣告為 GPL 相關，很多 Kernel 的重要功能你都會被禁止呼叫
MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("James Lu");
MODULE_DESCRIPTION("vGPU-Core Virtual PCIe Accelerator Driver");
MODULE_VERSION("0.1");
