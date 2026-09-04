CC = gcc
CFLAGS = -Wall -O2

all: test_ioctl test_interrupt

test_ioctl: test_ioctl.c
	$(CC) $(CFLAGS) $< -o $@ -pthread

test_interrupt: test_interrupt.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f test_ioctl test_interrupt
