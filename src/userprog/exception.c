#include "userprog/exception.h"
#include <inttypes.h>
#include <stdio.h>
#include "threads/gdt.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "userprog/syscall.h"
#include "threads/vaddr.h"
#ifdef VM
#include <string.h>
#include "vm/page.h"
#include "vm/frame.h"
#include "vm/swap.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"
#include "filesys/file.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/synch.h"

extern struct lock filesys_lock;
void exit (int status);
#endif

/* Number of page faults processed. */
static long long page_fault_cnt;

static void kill (struct intr_frame *);
static void page_fault (struct intr_frame *);

/* Registers handlers for interrupts that can be caused by user
   programs.

   In a real Unix-like OS, most of these interrupts would be
   passed along to the user process in the form of signals, as
   described in [SV-386] 3-24 and 3-25, but we don't implement
   signals.  Instead, we'll make them simply kill the user
   process.

   Page faults are an exception.  Here they are treated the same
   way as other exceptions, but this will need to change to
   implement virtual memory.

   Refer to [IA32-v3a] section 5.15 "Exception and Interrupt
   Reference" for a description of each of these exceptions. */
void
exception_init (void) 
{
  /* These exceptions can be raised explicitly by a user program,
     e.g. via the INT, INT3, INTO, and BOUND instructions.  Thus,
     we set DPL==3, meaning that user programs are allowed to
     invoke them via these instructions. */
  intr_register_int (3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
  intr_register_int (4, 3, INTR_ON, kill, "#OF Overflow Exception");
  intr_register_int (5, 3, INTR_ON, kill,
                     "#BR BOUND Range Exceeded Exception");

  /* These exceptions have DPL==0, preventing user processes from
     invoking them via the INT instruction.  They can still be
     caused indirectly, e.g. #DE can be caused by dividing by
     0.  */
  intr_register_int (0, 0, INTR_ON, kill, "#DE Divide Error");
  intr_register_int (1, 0, INTR_ON, kill, "#DB Debug Exception");
  intr_register_int (6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
  intr_register_int (7, 0, INTR_ON, kill,
                     "#NM Device Not Available Exception");
  intr_register_int (11, 0, INTR_ON, kill, "#NP Segment Not Present");
  intr_register_int (12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
  intr_register_int (13, 0, INTR_ON, kill, "#GP General Protection Exception");
  intr_register_int (16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
  intr_register_int (19, 0, INTR_ON, kill,
                     "#XF SIMD Floating-Point Exception");

  /* Most exceptions can be handled with interrupts turned on.
     We need to disable interrupts for page faults because the
     fault address is stored in CR2 and needs to be preserved. */
  intr_register_int (14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

/* Prints exception statistics. */
void
exception_print_stats (void) 
{
  printf ("Exception: %lld page faults\n", page_fault_cnt);
}

/* Handler for an exception (probably) caused by a user process. */
static void
kill (struct intr_frame *f) 
{
  /* This interrupt is one (probably) caused by a user process.
     For example, the process might have tried to access unmapped
     virtual memory (a page fault).  For now, we simply kill the
     user process.  Later, we'll want to handle page faults in
     the kernel.  Real Unix-like operating systems pass most
     exceptions back to the process via signals, but we don't
     implement them. */
     
  /* The interrupt frame's code segment value tells us where the
     exception originated. */
  switch (f->cs)
    {
    case SEL_UCSEG:
      /* User's code segment, so it's a user exception, as we
         expected.  Kill the user process.  */
      printf ("%s: dying due to interrupt %#04x (%s).\n",
              thread_name (), f->vec_no, intr_name (f->vec_no));
      intr_dump_frame (f);
      thread_current ()->exit_status = -1;
      thread_exit (); 

    case SEL_KCSEG:
      /* Kernel's code segment, which indicates a kernel bug.
         Kernel code shouldn't throw exceptions.  (Page faults
         may cause kernel exceptions--but they shouldn't arrive
         here.)  Panic the kernel to make the point.  */
      intr_dump_frame (f);
      PANIC ("Kernel bug - unexpected interrupt in kernel"); 

    default:
      /* Some other code segment?  Shouldn't happen.  Panic the
         kernel. */
      printf ("Interrupt %#04x (%s) in unknown segment %04x\n",
             f->vec_no, intr_name (f->vec_no), f->cs);
      thread_exit ();
    }
}

/* Page fault handler.  This is a skeleton that must be filled in
   to implement virtual memory.  Some solutions to project 2 may
   also require modifying this code.

   At entry, the address that faulted is in CR2 (Control Register
   2) and information about the fault, formatted as described in
   the PF_* macros in exception.h, is in F's error_code member.  The
   example code here shows how to parse that information.  You
   can find more information about both of these in the
   description of "Interrupt 14--Page Fault Exception (#PF)" in
   [IA32-v3a] section 5.15 "Exception and Interrupt Reference". */
static void
page_fault (struct intr_frame *f) 
{
  bool not_present;  /* True: not-present page, false: writing r/o page. */
  bool write;        /* True: access was write, false: access was read. */
  bool user;         /* True: access by user, false: access by kernel. */
  void *fault_addr;  /* Fault address. */

  /* Obtain faulting address, the virtual address that was
     accessed to cause the fault.  It may point to code or to
     data.  It is not necessarily the address of the instruction
     that caused the fault (that's f->eip).
     See [IA32-v2a] "MOV--Move to/from Control Registers" and
     [IA32-v3a] 5.15 "Interrupt 14--Page Fault Exception
     (#PF)". */
  asm ("movl %%cr2, %0" : "=r" (fault_addr));

  /* Turn interrupts back on (they were only off so that we could
     be assured of reading CR2 before it changed). */
  intr_enable ();

  /* Count page faults. */
  page_fault_cnt++;

  /* Determine cause. */
  not_present = (f->error_code & PF_P) == 0;
  write = (f->error_code & PF_W) != 0;
  user = (f->error_code & PF_U) != 0;

  /* If this page fault occurred while the kernel was validating
     user pointers for get_user()/put_user(), then we may need to
     grow the user stack. */
  bool is_get_user_fault = false;
  bool is_put_user_fault = false;

  if (!user) {
    extern char begin_user_access[];
    extern char end_user_access[];
    extern char begin_user_write[]; 
    extern char end_user_write[];   

    is_get_user_fault = ((void *)f->eip >= (void *)begin_user_access
                         && (void *)f->eip < (void *)end_user_access);
    is_put_user_fault = ((void *)f->eip >= (void *)begin_user_write
                         && (void *)f->eip < (void *)end_user_write);

    if (is_get_user_fault || is_put_user_fault) {
#ifndef VM
      f->eip = (void *)f->eax;
      f->eax = 0xffffffff; /* -1 */
      return; 
#endif
    }
   }

#ifdef VM
  /* User-mode fault or fault on user virtual addresses: demand page. */
  if (user || is_user_vaddr (fault_addr))
    {
      /* At entry, CR2 holds the faulting address. */
      struct thread *cur = thread_current ();
      void *upage = pg_round_down (fault_addr);
      struct sup_page_table_entry *spte = spt_find (&cur->spt, fault_addr);

#define STACK_MAX_BYTES (8 * 1024 * 1024)
#define STACK_GROWTH_GAP 32

      if (!not_present)
        exit (-1);

      /* Allocate stack pages on demand. */
      if (spte == NULL)
        {
          uint8_t *base_esp = NULL;
          if (user)
            base_esp = (uint8_t *) f->esp;
          else if (cur->user_esp != NULL)
            base_esp = (uint8_t *) cur->user_esp;

          if (base_esp != NULL &&
              (uint8_t *) fault_addr >= base_esp - STACK_GROWTH_GAP &&
              fault_addr < PHYS_BASE &&
              (uint8_t *) fault_addr >= (uint8_t *) PHYS_BASE - STACK_MAX_BYTES)
            {
              spte = malloc (sizeof *spte);
              if (spte == NULL)
                exit (-1);
              memset (spte, 0, sizeof *spte);
              spte->upage = upage;
              spte->writable = true;
              spte->type = VM_ANON;
              spte->is_mmap = false;
              spte->is_loaded = false;
              spte->kpage = NULL;

              if (!spt_insert (&cur->spt, spte))
                {
                  free (spte);
                  exit (-1);
                }
            }
          else
            exit (-1);
        }

      if (write && !spte->writable)
        exit (-1);

      void *kpage = NULL;
      switch (spte->type)
        {
        case VM_SWAP:
          kpage = frame_alloc (0, spte);
          if (kpage == NULL)
            exit (-1);
          swap_in (spte->swap_slot, kpage);
          spte->swap_slot = SPTE_SWAP_SLOT_INVALID;
          break;

        case VM_ANON:
          kpage = frame_alloc (PAL_ZERO, spte);
          if (kpage == NULL)
            exit (-1);
          break;

        case VM_BIN:
        case VM_FILE:
          /* VM_FILE is only used for mmap pages (backed by an actual file). */
          if (spte->type == VM_FILE && !spte->is_mmap)
            exit (-1);

          kpage = frame_alloc (0, spte);
          if (kpage == NULL)
            exit (-1);

          if (spte->read_bytes > 0)
            {
              bool need_unlock = false;
              if (!lock_held_by_current_thread (&filesys_lock))
                {
                  lock_acquire (&filesys_lock);
                  need_unlock = true;
                }
              off_t nread = file_read_at (spte->file, kpage,
                                          (off_t) spte->read_bytes,
                                          (off_t) spte->offset);
              if (need_unlock)
                lock_release (&filesys_lock);
              if (nread < 0 || (size_t) nread != spte->read_bytes)
                {
                  frame_free (kpage);
                  exit (-1);
                }
            }
          if (spte->zero_bytes > 0)
            memset ((uint8_t *) kpage + spte->read_bytes, 0, spte->zero_bytes);
          break;

        default:
          exit (-1);
        }

      if (!process_install_page (spte->upage, kpage, spte->writable))
        {
          frame_free (kpage);
          exit (-1);
        }

      /* Flush TLB (needed on SMP so user retry sees the new mapping). */
      pagedir_activate (cur->pagedir);
      /* Newly-installed pages are clean until the user stores. */
      pagedir_set_dirty (cur->pagedir, spte->upage, false);
      spte->kpage = kpage;
      spte->is_loaded = true;
      frame_unpin (kpage);
      return;
    }
#else
  if (user || is_user_vaddr (fault_addr))
    exit (-1);
#endif

  printf ("Page fault at %p: %s error %s page in %s context.\n",
          fault_addr,
          not_present ? "not present" : "rights violation",
          write ? "writing" : "reading",
          user ? "user" : "kernel");
  kill (f);
}

