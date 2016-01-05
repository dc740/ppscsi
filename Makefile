obj-m	:= ppscsi.o t348.o t358.o onscsi.o epsa2.o epst.o vpi0.o  sparcsi.o

KERNEL_SOURCE := /lib/modules/`uname -r`/build
all::
	$(MAKE) -C $(KERNEL_SOURCE) M=`pwd` modules
