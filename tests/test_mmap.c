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

    // 1. Map the Command Queue (Ring Buffer) -> offset 0
    void *ring_ptr = mmap(NULL, MMAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ring_ptr == MAP_FAILED) {
        perror("mmap ring buffer error");
        close(fd);
        return -1;
    }
    printf("[User Space] Mapped Command Queue at %p\n", ring_ptr);

    // 2. Map the Data Payload (1MB) -> offset 1 page
    size_t payload_size = 1024 * 1024;
    void *payload_ptr = mmap(NULL, payload_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 4096);
    if (payload_ptr == MAP_FAILED) {
        perror("mmap payload buffer error");
        munmap(ring_ptr, MMAP_SIZE);
        close(fd);
        return -1;
    }
    printf("[User Space] Mapped Data Payload at %p\n", payload_ptr);

    // Write massive data to payload buffer (simulating AI weights / images)
    const char *msg = "Hello from User Space! This is a massive 1MB Payload for Streaming DMA.";
    strcpy((char *)payload_ptr, msg);
    printf("[User Space] Wrote data to Payload Buffer: '%s'\n", msg);

    // 準備一個 Command
    struct vgpu_command cmd = {
        .opcode = 1,
        .operand1 = 100,
        .operand2 = 200,
        .result = 0
    };

    // 發送 Command (Control Path)
    if (ioctl(fd, VGPU_IOC_SUBMIT_CMD, &cmd) < 0) {
        perror("ioctl VGPU_IOC_SUBMIT_CMD error");
    }

    // 敲響 Doorbell 告訴 Kernel/GPU 資料準備好了
    printf("[User Space] Ringing Doorbell & Triggering dma_sync_single_for_device...\n");
    if (ioctl(fd, VGPU_IOC_DOORBELL, 1) < 0) {
        perror("ioctl VGPU_IOC_DOORBELL error");
    }

    // 解除映射並關閉檔案
    munmap(payload_ptr, payload_size);
    munmap(ring_ptr, MMAP_SIZE);
    close(fd);
    return 0;
}
