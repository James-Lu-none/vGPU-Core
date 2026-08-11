#include "vgpu_ioctl.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <time.h>

#define NUM_THREADS 4
#define NUM_LOOPS 100000

void *worker_thread(void *arg) {
    int thread_id = *(int *)arg;
    
    // 每個 Thread 各自 open 一次，在 Mode B 下會取得獨立的 vgpu_context
    int fd = open("/dev/vgpu0", O_RDWR);
    if (fd < 0) {
        perror("open /dev/vgpu0 error");
        return NULL;
    }

    struct vgpu_command cmd = {
        .opcode = 1,
        .operand1 = thread_id,
        .operand2 = 1,
        .result = 0
    };
    unsigned long cmd_ptr = (unsigned long)&cmd;

    for (int i = 0; i < NUM_LOOPS; i++) {
        if (ioctl(fd, VGPU_IOC_SUBMIT_CMD, cmd_ptr) < 0) {
            perror("ioctl VGPU_IOC_SUBMIT_CMD error");
            break;
        }
    }
    
    ioctl(fd, VGPU_IOC_DOORBELL, 1);

    close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    pthread_t threads[NUM_THREADS];
    int thread_ids[NUM_THREADS];
    struct timespec start, end;

    printf("Starting %d threads, each sending %d IOCTLs...\n", NUM_THREADS, NUM_LOOPS);

    // 開始計時
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < NUM_THREADS; i++) {
        thread_ids[i] = i;
        if (pthread_create(&threads[i], NULL, worker_thread, &thread_ids[i]) != 0) {
            perror("pthread_create error");
            return -1;
        }
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_time = (end.tv_sec - start.tv_sec) + 
                          (end.tv_nsec - start.tv_nsec) / 1e9;
    
    int total_ioctls = NUM_THREADS * NUM_LOOPS;
    printf("Total time: %.6f seconds\n", elapsed_time);
    printf("Throughput: %.0f IOCTLs/sec\n", total_ioctls / elapsed_time);

    return 0;
}
