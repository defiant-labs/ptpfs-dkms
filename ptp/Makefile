#
# Makefile for the linux ramfs routines.
#

O_TARGET := ptpfs.o
CC=gcc
EXTRA_CFLAGS=-g3 

obj-y :=  inode.o root.o ptp.o objects.o
obj-m := $(O_TARGET)

PWD:= $(shell pwd)
KERNEL_SOURCES:= /lib/modules/$(shell uname -r)/build


default:
	$(MAKE) -C $(KERNEL_SOURCES) M=$(PWD) modules

ins: default 
	sudo insmod ptpfs.ko
rm:
	sudo umount tmp
	sudo rmmod ptpfs
clean:
	rm -f *.o *.ko *.mod.c .*o.cmd *.s modules.order Module.symvers

