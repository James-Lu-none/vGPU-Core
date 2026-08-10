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

struct vgpu_dev {
    dev_t dev;          // 儲存裝置號 (Major/Minor number)
    struct cdev cdev;     // 儲存核心的 cdev 結構
    struct device *device; // 儲存核心的 device 結構
};

struct vgpu_dev *g_vgpu_dev = NULL; // 全域變數，指向我們建立的裝置結構
struct class *g_vgpu_class = NULL;

static int vgpu_open(struct inode *inode, struct file *file)
{
    pr_info("vGPU-Core: open\n");
    return 0;
}

static int vgpu_release(struct inode *inode, struct file *file)
{
    pr_info("vGPU-Core: release\n");
    return 0;
}

// 告訴 Kernel 當使用者對 /dev/vgpu0 按下 open()、ioctl() 時，要執行我這裡定義的哪個函數
static const struct file_operations vgpu_core_fops = {
    .owner   = THIS_MODULE,
    .open      = vgpu_open,
    .release   = vgpu_release,
};


// 自定義 initfn
// __init 是一個標記，告訴 Kernel 這個函數只有在初始化時會用到，
// 執行完後它的記憶體就可以被回收，節省空間。
static int __init vgpu_core_init(void)
{
    int result;
    pr_info("vGPU-Core: module loaded\n");

    // 1. alloc 結構體
    g_vgpu_dev = kzalloc(sizeof(struct vgpu_dev), GFP_KERNEL);
    if (!g_vgpu_dev) {
        pr_err("vGPU-Core: failed to allocate device structure\n");
        return -ENOMEM;
    }
    // 2. 取得裝置號
    // 0 means base number, 1 means number of devices
    // -> /dev/vgpu_core0
    // 0, 4 -> /dev/vgpu_core0 ~ /dev/vgpu_core3
    // the name "vgpu_core" here is for kernel, will create under entry in /proc/devices
    result = alloc_chrdev_region(&g_vgpu_dev->dev, 0, 1, "vgpu_core");
    if (result < 0) {
        pr_err("vGPU-Core: failed to allocate device number\n");
        kfree(g_vgpu_dev);
        return result;
    }
    // 3. 建立 cdev
    cdev_init(&g_vgpu_dev->cdev, &vgpu_core_fops);
    result = cdev_add(&g_vgpu_dev->cdev, g_vgpu_dev->dev, 1);
    if (result < 0) {
        pr_err("vGPU-Core: failed to add device\n");
        unregister_chrdev_region(g_vgpu_dev->dev, 1);
        kfree(g_vgpu_dev);
        return result;
    }

    // 4. 建立 udev (user space device /dev/vgpu0)
    // 在 Linux 裡，建立 Device 之前，必須先把它歸屬於某個 Class
    // (例如你插上滑鼠，它屬於 input class; 插上網卡，屬於 net class)
    g_vgpu_class = class_create("vgpu_core");
    if (IS_ERR(g_vgpu_class)) {
        pr_err("vGPU-Core: failed to create class\n");
        cdev_del(&g_vgpu_dev->cdev);
        unregister_chrdev_region(g_vgpu_dev->dev, 1);
        kfree(g_vgpu_dev);
        return PTR_ERR(g_vgpu_class);
    }
    g_vgpu_dev->device = device_create(
        g_vgpu_class, // parent class
        NULL,  
        g_vgpu_dev->dev, // device number
        NULL,        // device data
        "vgpu0"      // device name
    );
    if (IS_ERR(g_vgpu_dev->device)) {
        pr_err("vGPU-Core: failed to create device\n");
        cdev_del(&g_vgpu_dev->cdev);
        unregister_chrdev_region(g_vgpu_dev->dev, 1);
        kfree(g_vgpu_dev);
        return PTR_ERR(g_vgpu_dev->device);
    }
    pr_info("vGPU-Core: module loaded successfully\n");
    return 0; 
}

// 自定義 exitfn
// __exit 告訴 Kernel 這個函數只有在模組卸載時才會呼叫
static void __exit vgpu_core_exit(void)
{
    if (g_vgpu_dev) {
        // 4. 移除 udev 
        device_destroy(
            g_vgpu_class,
            g_vgpu_dev->dev
        );
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
