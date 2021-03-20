CROSS_COMPILE=arm-linux-gnueabihf-
ARCH=arm
obj-m:=ldd_nw.o

KERNELDIR ?= /home/nishanth/rpi_kernel/linux/ 
default:
	make V=1 ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KERNELDIR) M=$(PWD) modules
