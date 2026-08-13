#include <linux/module.h>
#include <linux/platform_device.h>
#include "../include/vgpu/vgpu_core.h"

static struct platform_device *vgpu_pdevs[MAX_VGPU_DEVICES];

static int __init vgpu_board_init(void)
{
    int i;
    int ret;
    pr_info("vGPU-Board: loading fake board with %d vGPUs...\n", MAX_VGPU_DEVICES);

    for (i = 0; i < MAX_VGPU_DEVICES; i++) {
        vgpu_pdevs[i] = platform_device_register_simple("vgpu_device", i, NULL, 0);
        if (IS_ERR(vgpu_pdevs[i])) {
            pr_err("vGPU-Board: failed to register device %d\n", i);
            ret = PTR_ERR(vgpu_pdevs[i]);
            goto err_register;
        }
    }
    return 0;

err_register:
    while (--i >= 0) {
        platform_device_unregister(vgpu_pdevs[i]);
    }
    return ret;
}

static void __exit vgpu_board_exit(void)
{
    int i;
    pr_info("vGPU-Board: unloading fake board...\n");
    for (i = 0; i < MAX_VGPU_DEVICES; i++) {
        if (vgpu_pdevs[i]) {
            platform_device_unregister(vgpu_pdevs[i]);
        }
    }
}

module_init(vgpu_board_init);
module_exit(vgpu_board_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("vGPU Team");
MODULE_DESCRIPTION("Fake Motherboard for vGPU Devices");
