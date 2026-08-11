obj-m += vgpu_core.o
vgpu_core-objs := vgpu_main.o vgpu_ioctl.o vgpu_mmap.o vgpu_sched.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
