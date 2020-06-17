#ifneq($(KERNELRELEASE),)
obj-m := blockio_driver.o
#else
KERNEL_DIR ?= /usr/src/linux-headers-$(shell uname -r)/

all:
	$(MAKE) -C $(KERNEL_DIR) M=$$PWD
