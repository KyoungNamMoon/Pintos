
The patches contained in this directory appear to be for an older
version of Bochs.  At the time of this writing  (May 2024), Bochs
appears to be bit rotting - their gdb stub doesn't work with recent
versions of gdb, for instance.

At Virginia Tech, we do not recommend the use of Bochs anyway.
The primary debugging environment (with a well-supported gdb stub)
is Qemu (without KVM).

Nevertheless, I build Bochs 2.7 and 2.8 recently without
patches as shown below.

Changes for Bochs 2.7. + 2.8
----------------------------

This version should work with Bochs, with the following caveats.
Bochs does not support the GDB stub and SMP mode simultaneously.
Therefore, I built two versions of bochs:

```
./configure \
    --with-x --with-x11 --with-term --with-nogui \
    --enable-smp \
    --enable-x86-64 \
    --enable-e1000 \
    --enable-cpu-level=6 \
    --prefix=/web/courses/cs4284/pintostools
```
The version is renamed `bochs-smp`

```
./configure \
    --with-x --with-x11 --with-term --with-nogui \
    --enable-gdb-stub \
    --enable-x86-64 \
    --enable-e1000 \
    --enable-cpu-level=6 \
    --prefix=/web/courses/cs4284/pintostools
```
The version is named `bochs`

I also changed `pintos` to use different names for Bochs's configuration file.
This allows the use of the `-j` flag when running make grade.
