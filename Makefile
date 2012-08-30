# You probably need to fix KERNEL_TREE to point at your kernel tree.
# If you are compiling natively on the Pi, remove the ARCH and CROSS_COMPILE settings

KERNEL_TREE := /home/richard/Pi/git/linux

all:	Panalyzer pandriver.ko pandriver-dma.ko

pandriver.ko:	pandriver.c panalyzer.h
	make -C ${KERNEL_TREE} ARCH=arm CROSS_COMPILE=/usr/bin/arm-linux-gnueabi- M=$(PWD) modules

pandriver-dma.ko:	pandriver-dma.c panalyzer.h
	make -C ${KERNEL_TREE} ARCH=arm CROSS_COMPILE=/usr/bin/arm-linux-gnueabi- M=$(PWD) modules

Panalyzer:	Panalyzer.c panalyzer.h
	gcc -Wall -g -O2 -o Panalyzer Panalyzer.c -Wl,--export-dynamic `pkg-config --cflags gtk+-3.0 gmodule-export-2.0` `pkg-config --libs gtk+-3.0 gmodule-export-2.0`

clean:
	make -C ${KERNEL_TREE} ARCH=arm CROSS_COMPILE=/usr/bin/arm-linux-gnueabi- M=$(PWD) clean
	rm -f Panalyzer

