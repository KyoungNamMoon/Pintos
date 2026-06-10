
README for pintos-2017
======================

This repository contains the Virginia Tech version of Pintos, which
spawned off Stanford's in 2009.  Added features include support for PCI,
USB, and as of 2017, multiple processors. More recently, some support
for ACPI and up to 1GB of physical memory was added.

Compared to Stanford's version, p1 has been replaced in its entirety.

This version has been in yearly or biyearly use and is actively maintained.

See [AUTHORS](AUTHORS) for individual credits and notes.

This source code is distributed under various respective licenses
as noted.

We do not distribute a "golden" solution for Pintos, but rather
recommend that instructors achieve 100% on the projects themselves
before integrating it into their courses.

Contact Godmar Back (godmar@gmail.com) with any questions.

This version by default uses the Qemu emulator (`--qemu`), but can also 
be used with KVM (`--kvm`).

The most recent version of the GNU toolchain with which it is tested
is 
- gcc version 11.4.1 20231218 (Red Hat 11.4.1-3) (GCC) 
- GNU ld version 2.35.2-43.el9

It is known to work with 
- QEMU emulator version 6.2.0
- QEMU emulator version 9.0.0
- Bochs 2.7 + 2.8 (limited support for debugging)
