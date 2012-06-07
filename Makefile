KERNEL_TREE := /home/richard/Pi/git/linux

all:	Panalyzer pandriver.ko

pandriver.ko:	pandriver.c panalyzer.h
	make -C ${KERNEL_TREE} ARCH=arm CROSS_COMPILE=/usr/bin/arm-linux-gnueabi- M=$(PWD) modules

Panalyzer:	Panalyzer.c panalyzer.h
	gcc -Wall -g -o Panalyzer Panalyzer.c `pkg-config --cflags gtk+-2.0` `pkg-config --libs gtk+-2.0`

clean:
	make -C ${KERNEL_TREE} ARCH=arm CROSS_COMPILE=/usr/bin/arm-linux-gnueabi- M=$(PWD) clean
	rm -f Panalyzer

