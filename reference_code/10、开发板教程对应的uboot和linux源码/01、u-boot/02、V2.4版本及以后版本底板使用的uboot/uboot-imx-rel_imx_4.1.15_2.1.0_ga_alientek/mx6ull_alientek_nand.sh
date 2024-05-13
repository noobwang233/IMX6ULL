#!/bin/bash
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- distclean
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- mx6ull_alientek_nand_defconfig
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- V=1 -j12