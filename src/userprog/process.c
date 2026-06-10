#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "threads/gdt.h"
#include "userprog/pagedir.h"
#include "threads/tss.h"
#include "userprog/syscall.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/mac.h"
#include "vm/page.h"
#ifdef VM
#include "vm/frame.h"
#include "vm/mmap.h"
#endif

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
  char *fn_copy;
  tid_t tid;

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);

  /* Extract program name (first token). */
  char *save_ptr;
  char prog_name[NAME_MAX + 1];
  strlcpy (prog_name, file_name, sizeof prog_name);
  strtok_r (prog_name, " ", &save_ptr);

  /* Create thread with program name only. */
  tid = thread_create (prog_name, NICE_DEFAULT, start_process, fn_copy);
  if (tid == TID_ERROR)
    {
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }

  /* Find the child in OUR children list (no global lookup by tid). */
  struct thread *cur = thread_current ();
  struct thread *child = NULL;
  struct list_elem *e;

  for (e = list_begin (&cur->children);
       e != list_end (&cur->children);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, child_elem);
      if (t->tid == tid)
        {
          child = t;
          child->uid = cur->uid;
          child->euid = cur->euid;
          child->suid = cur->euid; /* suid = parent's euid at exec time */
          child->gid = cur->gid;
          child->egid = cur->egid;
          break;
        }
    }

  if (child == NULL)
    return TID_ERROR;

  /* Wait for child to finish loading. */
  sema_down (&child->load_sema);
  if (!child->load_success)
    return TID_ERROR;

  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *file_name_)
{
  char *file_name = file_name_;
  struct intr_frame if_;
  bool success;
  struct thread *cur = thread_current ();
  #ifdef VM
    spt_init (&cur->spt);
  #endif
#ifdef VM
  mmap_process_init (cur);
#endif
  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load (file_name, &if_.eip, &if_.esp);

  /* Signal parent that load is complete. */
  cur->load_success = success;
  sema_up (&cur->load_sema);

  /* If load failed, quit. */
  palloc_free_page (file_name);
  if (!success)
    {
#ifdef VM
      mmap_destroy_all (cur);
#endif
      thread_exit ();
    }

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (tid_t child_tid) 
{
  struct thread *cur = thread_current ();
  struct list_elem *e;
  struct thread *child = NULL;
  int status;

  // Search children list for child with matching tid
  for (e = list_begin (&cur->children);
       e != list_end (&cur->children);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, child_elem);
      if (t->tid == child_tid)
        {
          child = t;
          break;
        }
    }

  if (child == NULL)
    return -1;

  // Remove from children list so we can't wait on it twice 
  list_remove (&child->child_elem);

  // Wait for child to exit 
  sema_down (&child->exit_sema);
  status = child->exit_status;
  // Let child finish cleanup and free its struct thread
  sema_up (&child->wait_sema);

  return status;
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;
  int i;
#ifdef VM
  mmap_destroy_all (cur);
  spt_destroy (&cur->spt);
#endif
  /* Release executable write deny + close it. */
  lock_acquire (&filesys_lock);
  if (cur->running_file != NULL)
    {
      file_allow_write (cur->running_file);
      file_close (cur->running_file);
      cur->running_file = NULL;
    }

  /* Close all open fds. */
  for (i = 2; i < 128; i++)
    if (cur->fd_table[i] != NULL)
      {
        file_close (cur->fd_table[i]);
        cur->fd_table[i] = NULL;
      }

  if (cur->cwd != NULL)
    {
      dir_close (cur->cwd);
      cur->cwd = NULL;
    }
  lock_release (&filesys_lock);

  /* If this thread exits without waiting on its children, release them so
     they do not block forever in sema_down(wait_sema) during their exit path. */
  struct list_elem *e;
  for (e = list_begin (&cur->children);
       e != list_end (&cur->children);
       e = list_next (e))
    {
      struct thread *child = list_entry (e, struct thread, child_elem);
      sema_up (&child->wait_sema);
    }

  /* Now do parent synchronization. */
  sema_up (&cur->exit_sema);
  sema_down (&cur->wait_sema);

  /* Destroy page directory... (unchanged) */
  pd = cur->pagedir;
  if (pd != NULL)
    {
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp, const char *cmd_line);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
/* load() helpers. */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  char *fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    goto done;

  strlcpy (fn_copy, file_name, PGSIZE);

  char *save_ptr;
  char *prog_name = strtok_r (fn_copy, " ", &save_ptr);

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  lock_acquire (&filesys_lock);

  /* Open executable file. */
  file = filesys_open (prog_name);
  if (file == NULL && prog_name[0] != '/' && strchr (prog_name, '/') == NULL)
    {
      char root_prog[NAME_MAX + 2];
      root_prog[0] = '/';
      strlcpy (root_prog + 1, prog_name, sizeof root_prog - 1);
      file = filesys_open (root_prog);
    }
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      goto done_locked; 
    }

  /* Setuid binary support: if the executable has S_ISUID set and is
     owned by root, run with euid=ROOT_UID regardless of caller's uid.
     This is the proper Unix mechanism for privileged programs like
     sudo, auth, and su. */
  {
    struct inode *exec_inode = file_get_inode (file);
    if (exec_inode != NULL &&
        (inode_get_mode (exec_inode) & S_ISUID) &&
        inode_get_owner (exec_inode) == ROOT_UID)
      {
        t->euid = ROOT_UID;
      }
  }

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done_locked; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done_locked;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done_locked;
      file_ofs += sizeof phdr;

      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done_locked;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;

              if (phdr.p_filesz > 0)
                {
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }

              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done_locked;
            }
          else
            goto done_locked;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp, file_name))
    goto done_locked;

  /* Extract program name from file_name (remove path and arguments) */
  static char program_name_buf[64];
  const char *start = file_name;
  const char *end = file_name;

  /* Find the start of the program name (after last slash) */
  for (const char *p = file_name; *p && *p != ' '; p++) {
    if (*p == '/')
        start = p + 1;
    end = p + 1;  /* Track end position */
  }

  /* Copy program name only (up to space or end) */
  int len = 0;
  while (start < end && len < 63) {
    program_name_buf[len] = *start;
    start++;
    len++;
  }
  program_name_buf[len] = '\0';

  /* Assign MAC security profile based on program name */
  mac_assign_profile(t, program_name_buf); 
  
  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  /* SUCCESS: deny writes and keep the executable open. */
  file_deny_write (file);
  t->running_file = file;
  file = NULL;                /* so we don't close it in cleanup */
  success = true;

done_locked:
  lock_release (&filesys_lock);

done:
  if (fn_copy != NULL)
    palloc_free_page (fn_copy);

  /* If load failed, close file if it was opened. */
  if (file != NULL)
    file_close (file);

  return success;
}
static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

#ifdef VM
  struct thread *t = thread_current ();
  while (read_bytes > 0 || zero_bytes > 0)
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      struct sup_page_table_entry *spte = malloc (sizeof *spte);
      if (spte == NULL)
        return false;
      memset (spte, 0, sizeof *spte);

      spte->upage = upage;
      spte->writable = writable;
      spte->type = VM_BIN;
      spte->file = file;
      spte->offset = (size_t) ofs;
      spte->read_bytes = page_read_bytes;
      spte->zero_bytes = page_zero_bytes;
      spte->is_loaded = false;
      spte->is_mmap = false;
      spte->kpage = NULL;

      /* Prevent duplicates due to overlapping segments. */
      if (spt_find (&t->spt, upage) != NULL)
        {
          free (spte);
          return false;
        }
      if (!spt_insert (&t->spt, spte))
        {
          free (spte);
          return false;
        }

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      ofs += page_read_bytes;
      upage += PGSIZE;
    }
  return true;
#else
  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Get a page of memory. */
      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      /* Load this page. */
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false; 
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      /* Add the page to the process's address space. */
      if (!install_page (upage, kpage, writable)) 
        {
          palloc_free_page (kpage);
          return false; 
        }

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
#endif
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
/**
 * Take in cmd_line, parse it, and push the arguments onto the stack in the correct order.
 */
static bool
setup_stack (void **esp, const char *cmd_line) 
{
  uint8_t *kpage;
  bool success = false;

#ifdef VM
  struct thread *t = thread_current ();
  void *upage = (uint8_t *) PHYS_BASE - PGSIZE;

  struct sup_page_table_entry *spte = malloc (sizeof *spte);
  if (spte == NULL)
    return false;
  memset (spte, 0, sizeof *spte);
  spte->upage = upage;
  spte->writable = true;
  spte->type = VM_ANON;
  spte->is_mmap = false;
  spte->is_loaded = false;
  spte->kpage = NULL;

  if (!spt_insert (&t->spt, spte))
    {
      free (spte);
      return false;
    }

  kpage = frame_alloc (PAL_USER | PAL_ZERO, spte);
  if (kpage == NULL)
    {
      list_remove (&spte->elem);
      free (spte);
      return false;
    }

  success = process_install_page (spte->upage, kpage, spte->writable);
  if (!success)
    {
      frame_free (kpage);
      list_remove (&spte->elem);
      free (spte);
      return false;
    }
  frame_unpin (kpage);
#else
  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage == NULL)
    return false;

  success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
  if (!success)
    {
      palloc_free_page (kpage);
      return false;
    }
#endif

  /* Stack starts at the top of user virtual memory. */
  *esp = PHYS_BASE;
  void *stack_bottom = PHYS_BASE - PGSIZE;

  /* Copy command line for tokenization. */
  char *cmd_copy = palloc_get_page (0);
  if (cmd_copy == NULL)
    return false;

  strlcpy (cmd_copy, cmd_line, PGSIZE);

  /* Store addresses of argument strings as pushed on the user stack. */
  char *argv[128];
  int argc = 0;

  char *token, *save_ptr;

  for (token = strtok_r (cmd_copy, " ", &save_ptr);
       token != NULL && argc < (int) (sizeof argv / sizeof argv[0]);
       token = strtok_r (NULL, " ", &save_ptr))
    {
      size_t len = strlen (token) + 1;

      *esp -= len;
      if (*esp < stack_bottom)
        {
          success = false;
          goto done;
        }

      memcpy (*esp, token, len);
      argv[argc++] = *esp;
    }

  /* Word align to 4 bytes. */
  *esp = (void *) ((uintptr_t) *esp & ~3);

  /* argv[argc] = NULL sentinel. */
  *esp -= sizeof (char *);
  if (*esp < stack_bottom) { success = false; goto done; }
  *(char **) *esp = NULL;

  /* Push argv pointers in reverse. */
  for (int i = argc - 1; i >= 0; i--)
    {
      *esp -= sizeof (char *);
      if (*esp < stack_bottom) { success = false; goto done; }
      *(char **) *esp = argv[i];
    }

  /* Push argv (address of argv[0]). */
  char **argv_addr = (char **) *esp;
  *esp -= sizeof (char **);
  if (*esp < stack_bottom) { success = false; goto done; }
  *(char ***) *esp = argv_addr;

  /* Push argc. */
  *esp -= sizeof (int);
  if (*esp < stack_bottom) { success = false; goto done; }
  *(int *) *esp = argc;

  /* Push fake return address. */
  *esp -= sizeof (void *);
  if (*esp < stack_bottom) { success = false; goto done; }
  *(void **) *esp = NULL;

  success = true;

done:
  palloc_free_page (cmd_copy);
  return success;
}


/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}

#ifdef VM
bool
process_install_page (void *upage, void *kpage, bool writable)
{
  return install_page (upage, kpage, writable);
}
#endif