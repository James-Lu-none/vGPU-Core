obj-m += vgpu_core.o


KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all: user_program
	$(MAKE) -C $(KDIR) M=$(PWD) modules

user_program:
	gcc test_ioctl.c -o test_ioctl -pthread
	gcc test_mmap.c -o test_mmap
	gcc test_interrupt.c -o test_interrupt

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f test_ioctl test_mmap test_interrupt
