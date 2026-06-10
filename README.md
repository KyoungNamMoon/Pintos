
README 
======================

# Pintos Advanced Operating System

An enhanced implementation of the **Pintos Academic Operating System**, originally developed at Stanford and significantly extended by Virginia Tech. This repository contains our team's complete implementations of core kernel subsystems, extending the base framework with modern scheduling paradigms, advanced multi-threading synchronization, secure user-program execution environments, and cryptographic authentication.

## Team Implementations & Key Features

### 1. Advanced Threading & Scheduling (`src/threads`)
* **Completely Fair Scheduler (CFS):** Replaced the baseline round-robin scheduler with a custom implementation of the Linux-inspired CFS. Utilizes `vruntime` and dynamic `nice` values to ensure equitable CPU time allocation and optimal latency.
* **Efficient Alarm Clock:** Re-engineered the synchronization primitive from a CPU-intensive busy-waiting loop to a blocked-state sleep mechanism driven by timer interrupts.
* **Priority Donation:** Implemented a recursive priority donation system for locks and semaphores to resolve the **Priority Inversion Problem**, supporting multiple donations and nested dependency chains.

### 2. User Program Execution & System Calls (`src/userprog`)
* **ELF Argument Passing:** Engineered process initialization parsing (`process_execute`) to strictly follow the x86 ELF calling convention onto the user stack.
* **Secure System Call Layer:** Implemented safe user memory access boundaries for critical system calls (`fork`, `exec`, `wait`, `exit`, `open`, `read`, `write`, `close`). Validates pointers and page alignments to prevent kernel page faults from malicious user spaces.
* **Executable Locking (ROX):** Implemented file protection mechanisms (`file_deny_write`) to prevent modification of actively running process images, guaranteeing kernel stability.

### 3. Cryptographic Security & Authentication
* **SHA-256 Security Track:** Integrated a robust, hardware-independent SHA-256 cryptographic hashing utility directly within the kernel core (`src/lib/sha256.c`).
* **Secure Auth Subsystem:** Leveraged for salted password hashing, brute-force defense simulations, and secure process identity mapping within customized user authentication system calls (`auth.c`).

---

## Base Framework Enhancements (Virginia Tech Version)

This repository builds upon the enhanced Virginia Tech branch (maintained since 2017), which provides significant architectural upgrades over the original Stanford version:
* **Hardware Support:** Advanced support for PCI, USB, and ACPI.
* **Memory & Processing:** Support for up to 1GB of physical memory and multiple processors (SMP).
* **Project Redesign:** Project 1 (Threads) has been replaced in its entirety to better reflect modern OS challenges.

---

## Development Environment & Toolchain

This project is built and heavily tested inside an x86 Linux emulator ecosystem.

* **Compiler & Linker:**
  * gcc version 11.4.1 20231218 (Red Hat 11.4.1-3) (GCC)
  * GNU ld version 2.35.2-43.el9
* **Supported Emulators:**
  * **QEMU:** version 6.2.0 and 9.0.0 (Default: `--qemu`, also supports hardware acceleration via `--kvm`)
  * **Bochs:** version 2.7 and 2.8 (with limited debugging support)

---

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
