obj-m	:= ppscsi.o t348.o t358.o onscsi.o epsa2.o epst.o vpi0.o  sparcsi.o

KERNEL_SOURCE := /lib/modules/`uname -r`/build
all:
	$(MAKE) -C $(KERNEL_SOURCE) M=`pwd` modules

install:
	cp ppscsi.ko epst.ko /lib/modules/`uname -r`/kernel/drivers/parport
	depmod -a

load:
	modprobe ppscsi
	modprobe epst

clean:
	rm -f ./*.o
	rm -f ./*.ko
	rm -f ./*.mod.c
	rm -f ./modules.order
	rm -f ./Module.symvers
