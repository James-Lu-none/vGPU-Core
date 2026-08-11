#include "vgpu_ioctl.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

int main(int argc, char **argv){
    int fd = open("/dev/vgpu0", O_RDWR);
    if (fd < 0) {
        perror("open /dev/vgpu0 error");
        return -1;
    }
    unsigned long arg;

    struct vgpu_command cmd;

    cmd.opcode = 1;
    cmd.operand1 = 1;
    cmd.operand2 = 1;
    cmd.result = 0;
    arg = (unsigned long)&cmd;

    if (ioctl(fd, VGPU_IOC_SUBMIT_CMD, arg) < 0) {
        perror("ioctl VGPU_IOC_SUBMIT_CMD error");
        return -1;
    }

    // 這裡的 arg 是一個 unsigned long，通常用來傳遞簡單的值或位址。
    // 對於 VGPU_IOC_DOORBELL，我們不需要傳遞額外的結構體，所以直接把 1 丟進去。
    arg = 1;
    // A user calls ioctl() in user space with a file descriptor.
    // The system call goes into the virtual file system (VFS) in the kernel.
    // The VFS looks at the target device file's file_operations structure.
    // The kernel runs the unlocked_ioctl pointer directly without holding the old global lock.
    if (ioctl(fd, VGPU_IOC_DOORBELL, arg) < 0) {
        perror("ioctl VGPU_IOC_DOORBELL error");
        return -1;
    }
    close(fd);
    return 0;
}
