#include "../include/uapi/vgpu_ioctl.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>

#define MMAP_SIZE (4096) // 1 Page

int main(int argc, char **argv) {
    int fd = open("/dev/vgpu0", O_RDWR);
    if (fd < 0) {
        perror("open /dev/vgpu0 error");
        return -1;
    }

    // 呼叫 mmap
    // PROT_READ | PROT_WRITE 代表可讀可寫
    // MAP_SHARED 代表與其他映射此空間的 process (或硬體) 共享記憶體修改
    void *map_ptr = mmap(NULL, MMAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map_ptr == MAP_FAILED) {
        perror("mmap error");
        close(fd);
        return -1;
    }

    printf("[User Space] mmap success! Address: %p\n", map_ptr);

    // 直接將字串寫入 mmap 記憶體 (Zero-Copy Data Path)
    const char *msg = "Hello from User Space (Zero-Copy)! AI Weights Loaded.";
    strcpy((char *)map_ptr, msg);
    printf("[User Space] Wrote message to mapped buffer: '%s'\n", msg);

    // 準備一個 Command
    struct vgpu_command cmd = {
        .opcode = 1,
        .operand1 = 100,
        .operand2 = 200,
        .result = 0
    };
    unsigned long cmd_ptr = (unsigned long)&cmd;

    // 發送 Command (Control Path)
    if (ioctl(fd, VGPU_IOC_SUBMIT_CMD, cmd_ptr) < 0) {
        perror("ioctl VGPU_IOC_SUBMIT_CMD error");
    }

    // 敲響 Doorbell 告訴 Kernel/GPU 資料準備好了
    printf("[User Space] Ringing Doorbell...\n");
    if (ioctl(fd, VGPU_IOC_DOORBELL, 1) < 0) {
        perror("ioctl VGPU_IOC_DOORBELL error");
    }

    // 解除映射並關閉檔案
    munmap(map_ptr, MMAP_SIZE);
    close(fd);
    return 0;
}
