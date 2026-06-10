#!/bin/bash

#
# Boot the Pintos demo disk created by makedemodisk.sh
# Run from the src/ directory: bash ../rundisk.sh
#

DISKIMAGE=usbdisk.img

if [ ! -f ${DISKIMAGE} ]; then
    echo "Error: ${DISKIMAGE} not found. Run makedemodisk.sh first."
    exit 1
fi

qemu-system-x86_64 \
    -drive file=${DISKIMAGE},format=raw \
    -serial file:serial.stdio \
    -display curses \
    -m 64M \
    -smp cores=1,threads=1,sockets=4 \
    -enable-kvm
