#!/bin/bash

#
# Build a bootable disk for Pintos, Fall 2008
#
# Note: this script is intended to work with the version
# of pintos installed in /home/courses/cs3204/bin/pintos
#
# It's intended to run from the command line in the src/ 
# directory
#

# CHANGE this line to install a P3/P4 kernel
BUILDDIR=./userprog/build
EXAMPLEDIR=./examples
DISKIMAGE=usbdisk.img

# check that BUILDDIR exists
test -d ${BUILDDIR} || cat << EOF
${BUILDDIR} does not exist --- 

did you build your Pintos P2 kernel and are you currently in the src/ directory ? 
EOF
test -d ${BUILDDIR} || exit

/bin/rm -f ${DISKIMAGE}
test -e ${BUILDDIR}/kernel.bin || echo 'No kernel found in ' ${BUILDDIR}

# Create a new disk image and start up the kernel to populate it with
# several useful programs.  
#
# CHANGE this to place other apps on the disk
#
# For a P3 kernel, you should also create a swap disk via
# --swap-size=4
#
pintos -v -k --qemu \
        --make-disk ${DISKIMAGE} \
        --filesys-size=4 \
        --loader ${BUILDDIR}/loader.bin \
        --kernel=${BUILDDIR}/kernel.bin \
        --align=full \
        -p ${EXAMPLEDIR}/shell -a shell \
        -p ${EXAMPLEDIR}/ls -a ls \
        -p ${EXAMPLEDIR}/halt -a halt \
        -p ${EXAMPLEDIR}/echo -a echo \
        -p ${EXAMPLEDIR}/cat -a cat \
        -p ${EXAMPLEDIR}/insult -a insult \
        -p ${EXAMPLEDIR}/id -a id \
        -p ${EXAMPLEDIR}/login -a login \
        -p ${EXAMPLEDIR}/auth -a auth \
        -p ${EXAMPLEDIR}/shell.c -a shell.c \
        -p ${EXAMPLEDIR}/useradd -a useradd \
        -p ${EXAMPLEDIR}/userdel -a userdel \
        -p ${EXAMPLEDIR}/passwd -a passwd \
        -p ${EXAMPLEDIR}/sudo -a sudo \
        -p ${EXAMPLEDIR}/pwd -a pwd \
        -p ${EXAMPLEDIR}/mkdir -a mkdir \
        -p ${EXAMPLEDIR}/rm -a rm \
        -p ${EXAMPLEDIR}/whoami -a whoami \
        -p ${EXAMPLEDIR}/usermod -a usermod \
        -p ${EXAMPLEDIR}/who -a who \
        -p ${EXAMPLEDIR}/addtest -a addtest \
        -p ${EXAMPLEDIR}/authtest -a authtest \
        -p ${EXAMPLEDIR}/chmod -a chmod \
        -p ${EXAMPLEDIR}/chmodtest -a chmodtest \
        -p ${EXAMPLEDIR}/touch -a touch \
        -p ${EXAMPLEDIR}/touchtest -a touchtest \
        -p ${EXAMPLEDIR}/addgroup -a addgroup \
        -- -q -f

# Boot into shell (which auto-runs login on first boot)
pintos-set-cmdline \
        ${DISKIMAGE} \
        -- run shell
