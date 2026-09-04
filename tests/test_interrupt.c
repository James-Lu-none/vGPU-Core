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

    void *map_ptr = mmap(NULL, MMAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map_ptr != MAP_FAILED) {
        const char *msg = "Data ready for compute.";
        strcpy((char *)map_ptr, msg);
    }

    struct vgpu_command cmd = {
        .opcode = 1,
        .payload_size = 0,
        .payload_vaddr = 0,
    };
    unsigned long cmd_ptr = (unsigned long)&cmd;

    if (ioctl(fd, VGPU_IOC_SUBMIT_CMD, cmd_ptr) < 0) {
        perror("ioctl VGPU_IOC_SUBMIT_CMD error");
    }

    printf("[User Space] Ringing Doorbell...\n");
    if (ioctl(fd, VGPU_IOC_DOORBELL, 1) < 0) {
        perror("ioctl VGPU_IOC_DOORBELL error");
    }

    printf("[User Space] Sleeping and waiting for GPU hardware interrupt...\n");
    if (ioctl(fd, VGPU_IOC_WAIT_FOR_IRQ, 1) < 0) {
        perror("ioctl VGPU_IOC_WAIT_FOR_IRQ error");
    }
    
    printf("[User Space] Woken up! IRQ received. GPU compute done!\n");

    if (map_ptr != MAP_FAILED) {
        munmap(map_ptr, MMAP_SIZE);
    }
    close(fd);
    return 0;
}
