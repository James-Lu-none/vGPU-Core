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

    // 2. Allocate Data Payload using standard malloc (Demand Paging)
    // before implementing demand paging, the memory was allocated and own by gpu with dma_alloc_coherent() in probe function
    // and userspace side has to perform dma_mmap_coherent to map the memory that gpu is holding and read/write data from/to the memory
    // !Now, with demand paging, the userspace side can just perform malloc, which only allocate virtual address space initially
    // and kernel will allocate physical memory when the userspace side access the memory, also, we pass this virtual address to
    // into cmd over ioctl, and the driver performs get_user_pages_fast() to tell kernel to pin the physical memmory and provide the phsical address
    // for  
    size_t payload_size = 1024 * 1024; // 1MB
    void *payload_ptr = malloc(payload_size);
    if (!payload_ptr) {
        perror("malloc payload buffer error");
        munmap(ring_ptr, MMAP_SIZE);
        close(fd);
        return -1;
    }
    printf("[User Space] Allocated Data Payload (Virtual Address) at %p\n", payload_ptr);

    // Write massive data to payload buffer (simulating AI weights / images)
    // This write operation triggers Linux CPU Page Faults, allocating physical RAM on-demand!
    const char *msg = "Hello from User Space! This is a massive 1MB Payload for Demand Paging (UVM).";
    strcpy((char *)payload_ptr, msg);
    printf("[User Space] Wrote data to Payload Buffer: '%s'\n", msg);

    // Prepare a Command
    struct vgpu_command cmd = {
        .opcode = 1,
        .payload_size = payload_size,
        .payload_vaddr = (unsigned long)payload_ptr
    };

    // 發送 Command (Control Path)
    // The Kernel will pin the memory and build a Scatter-Gather Page Table for the FPGA
    if (ioctl(fd, VGPU_IOC_SUBMIT_CMD, &cmd) < 0) {
        perror("ioctl VGPU_IOC_SUBMIT_CMD error");
    }

    // 敲響 Doorbell 告訴 Kernel/GPU 資料準備好了
    printf("[User Space] Ringing Doorbell...\n");
    if (ioctl(fd, VGPU_IOC_DOORBELL, 1) < 0) {
        perror("ioctl VGPU_IOC_DOORBELL error");
    }

    // 解除映射並關閉檔案
    free(payload_ptr);
    munmap(ring_ptr, MMAP_SIZE);
    close(fd);
    return 0;
}
