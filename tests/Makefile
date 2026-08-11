CC = gcc
CFLAGS = -Wall -O2

all: test_ioctl test_mmap test_interrupt

test_ioctl: test_ioctl.c
	$(CC) $(CFLAGS) $< -o $@ -pthread

test_mmap: test_mmap.c
	$(CC) $(CFLAGS) $< -o $@

test_interrupt: test_interrupt.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f test_ioctl test_mmap test_interrupt
