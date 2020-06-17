#ifneq($(KERNELRELEASE),)
obj-m := scull1.o
#else
KERNEL_DIR := /usr/src/linux-headers-$(shell uname -r)/

all:
	$(MAKE) -C $(KERNEL_DIR) M=$$PWD
