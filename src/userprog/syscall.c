#include "userprog/syscall.h"
#include <stdio.h>
#include <stdint.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "userprog/pagedir.h"
#include "devices/shutdown.h"
#include "userprog/process.h"
#include "threads/synch.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "devices/input.h"
#include "threads/vaddr.h"
#include "filesys/directory.h"
#include "filesys/inode.h"
#include "userprog/mac.h"
#include <string.h>
#ifdef VM
#include "vm/mmap.h"
#include "vm/page.h"
#include "vm/frame.h"
#include "vm/swap.h"
#include "threads/malloc.h"
#endif

/* Open flags for permission checking. */
#define O_RDONLY  0x01
#define O_WRONLY  0x02
#define O_RDWR    0x03

static void syscall_handler (struct intr_frame *);
/* Global lock to ensure file system */
struct lock filesys_lock;
/* Helper functions for file descriptor management */
static int process_add_file (struct file *f);
static struct file *process_get_file (int fd);
static void process_close_file (int fd);

/* Helper functions for memory validation */
int get_user (const uint8_t *uaddr);
bool put_user (uint8_t *udst, uint8_t byte);

static int32_t copy_word (const void *uaddr);

static void check_buffer (void *buffer, unsigned size);
static void check_valid_string (const void *str);

#ifdef VM
static void pin_user_buffer (void *buffer, unsigned size, bool writable);
static void unpin_user_buffer (void *buffer, unsigned size);
#endif

void exit (int status);
void check_address (void *addr);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init (&filesys_lock);
  mac_init();
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  const uint8_t *uesp = (const uint8_t *) f->esp;
#ifdef VM
  thread_current ()->user_esp = (void *) uesp;
#endif
  int32_t syscall_num = copy_word (uesp);

  switch (syscall_num)
  {
  case SYS_HALT:
    shutdown_power_off();
    break;

  case SYS_EXIT:
    /* Argument: Exit status code */
    exit(copy_word (uesp + 4));
    break;

  case SYS_CREATE:
    {
      const char *filename = (const char *) (uintptr_t) copy_word (uesp + 4);
      unsigned initial_size = (unsigned) copy_word (uesp + 8);
      
      check_address((void *) filename);
      check_valid_string(filename);
      
      /* DAC check (existing code) */
      lock_acquire(&filesys_lock);
      f->eax = filesys_create(filename, initial_size);
      lock_release(&filesys_lock);
    }
    break;

  case SYS_OPEN:
    {
      const char *name = (const char *) (uintptr_t) copy_word (uesp + 4);
      int flags = (int) copy_word (uesp + 8);
      check_address ((void *) name);
      check_valid_string (name);
      lock_acquire (&filesys_lock);
      struct file *file_ptr = filesys_open (name);
      if (file_ptr == NULL)
        {
          f->eax = -1;
        }
      else
        {
          struct inode *inode = file_get_inode (file_ptr);
          /* Determine required permission based on open flags. */
          int required = 4;          /* O_RDONLY — default */
          if (flags == O_WRONLY)
            required = 2;
          else if (flags == O_RDWR)
            required = 6;
          if (!inode_check_permission (inode, required))
            {
              file_close (file_ptr);
              f->eax = -1;
            }
          else
            {
              f->eax = process_add_file (file_ptr);
              if ((int) f->eax == -1)
                file_close (file_ptr);
            }
        }
      lock_release (&filesys_lock);
      break;
    }

  case SYS_FILESIZE:
    /* Arguement: file descriptor */
    lock_acquire(&filesys_lock);
    struct file *f_ptr = process_get_file(((int) copy_word (uesp + 4)));
    if (f_ptr == NULL) f->eax = -1;
    else f->eax = file_length(f_ptr);
    lock_release(&filesys_lock);
    break;

  case SYS_READ: 
    /* Arguments: fd, buffer, size*/
    check_address((void *) (uintptr_t) copy_word (uesp + 8)); 

    int fd_read = ((int) copy_word (uesp + 4));
    void *buffer_read = (void *) (uintptr_t) copy_word (uesp + 8);
    unsigned size_read = ((unsigned) copy_word (uesp + 12));

#ifdef VM
    pin_user_buffer (buffer_read, size_read, true);
#else
    check_buffer (buffer_read, size_read);
#endif

    lock_acquire(&filesys_lock);
    if (fd_read == 0) { 
      unsigned i;
      for (i = 0; i < size_read; i++) {
        ((char *)buffer_read)[i] = input_getc();
      }
      f->eax = size_read;
    } else { 
      struct file *read_f = process_get_file(fd_read);
      if (read_f == NULL) f->eax = -1;
      else f->eax = file_read(read_f, buffer_read, size_read);
    }
    lock_release(&filesys_lock);
#ifdef VM
    unpin_user_buffer (buffer_read, size_read);
#endif
    break;

  case SYS_EXEC: 
    const char *cmd_line = (const char *) (uintptr_t) copy_word (uesp + 4);
    check_address((void *) cmd_line);
    check_valid_string(cmd_line);

    /* MAC check */
    if (!mac_check_capability(thread_current()->mac_profile, "exec")) {
      printf("Program not allowed to execute other programs\n");
      f->eax = -1;
      break;
    }
    
    f->eax = process_execute((const char *) (uintptr_t) copy_word (uesp + 4));
    break;

  case SYS_WAIT: 
    /* Arguement: PID */
    f->eax = process_wait(((int) copy_word (uesp + 4)));
    break;

  case SYS_WRITE: 
    /* Arguments: fd, buffer, size */
    check_address((void *) (uintptr_t) copy_word (uesp + 8));

    int fd_write = ((int) copy_word (uesp + 4));
    void *buffer_write = (void *) (uintptr_t) copy_word (uesp + 8);
    unsigned size_write = ((unsigned) copy_word (uesp + 12));

    /* Source buffer is read by write(); it need not be writable (e.g. mmap-clean
       passes a string in .rodata). check_buffer_write would wrongly exit(-1). */
#ifdef VM
    pin_user_buffer (buffer_write, size_write, false);
#else
    check_buffer (buffer_write, size_write);
#endif

    lock_acquire(&filesys_lock);
    if (fd_write == 1) { 
      putbuf((char *)buffer_write, size_write);
      f->eax = size_write;
    }  
    else { 
      struct file *write_f = process_get_file(fd_write);
      if (write_f == NULL)
        f->eax = -1;
      else
        {
          /* Reject writes to directories. */
          struct inode *inode = file_get_inode (write_f);
          if (inode != NULL && inode_is_dir (inode))
            f->eax = -1;
          else
            f->eax = file_write(write_f, buffer_write, size_write);
        }
    }
    lock_release(&filesys_lock);
#ifdef VM
    unpin_user_buffer (buffer_write, size_write);
#endif
    break;

  case SYS_SEEK: 
    /* Arguements: fd, position */
    lock_acquire(&filesys_lock);
    struct file *seek_f = process_get_file(((int) copy_word (uesp + 4)));
    if (seek_f != NULL) {
      file_seek(seek_f, ((unsigned) copy_word (uesp + 8)));
    }
    lock_release(&filesys_lock);
    break;

  case SYS_TELL: 
    /* Argument: fd */
    lock_acquire(&filesys_lock);
    struct file *tell_f = process_get_file(((int) copy_word (uesp + 4)));
    if (tell_f == NULL) f->eax = -1;
    else f->eax = file_tell(tell_f);
    lock_release(&filesys_lock);
    break;
  
  case SYS_REMOVE:
    /* Argument: filename */
    const char *file = (const char *) (uintptr_t) copy_word (uesp + 4);
    check_address((void *) file);
    check_valid_string(file);

    lock_acquire(&filesys_lock);
    f->eax = filesys_remove((const char *) (uintptr_t) copy_word (uesp + 4));
    lock_release(&filesys_lock);
    break;

  case SYS_CLOSE: 
    /* Argument: fd */
    int fd_close = ((int) copy_word (uesp + 4));
    lock_acquire(&filesys_lock);
    struct file *close_f = process_get_file(fd_close);
    if (close_f != NULL) {
      file_close(close_f);
      process_close_file(fd_close);
    }
    lock_release(&filesys_lock);
    break;
  /* SYS_CHDIR: Change the current working directory of this process. */
  case SYS_CHDIR:
    {
      const char *path = (const char *) (uintptr_t) copy_word (uesp + 4);
      check_valid_string (path);
      lock_acquire (&filesys_lock);
      struct dir *new_dir = filesys_opendir (path);
      if (new_dir == NULL
          || !inode_check_permission (dir_get_inode (new_dir), 1))
        {
          dir_close (new_dir);
          f->eax = 0;
        }
      else
        {
          struct thread *t = thread_current ();
          dir_close (t->cwd);
          t->cwd = new_dir;
          f->eax = 1;
        }
      lock_release (&filesys_lock);
      break;
    }
  /* SYS_MKDIR: Create a new directory at the given path. */
case SYS_MKDIR:
    {
      const char *dir = (const char *) (uintptr_t) copy_word (uesp + 4);
      check_address((void *) dir);
      check_valid_string(dir);
      
      /* DAC check (existing code) */
      lock_acquire(&filesys_lock);
      f->eax = filesys_mkdir(dir);
      lock_release(&filesys_lock);
    }
    break;
  /* SYS_READDIR: Read the next entry from an open directory fd.
     Syncs position through the underlying file offset so that
     successive calls advance correctly. */
  case SYS_READDIR:
    {
      int fd = (int) copy_word (uesp + 4);
      char *name = (char *) (uintptr_t) copy_word (uesp + 8);
      check_address (name);
      lock_acquire (&filesys_lock);
      struct file *file = process_get_file (fd);
      if (file == NULL)
        {
          f->eax = 0;
        }
      else
        {
          struct inode *inode = file_get_inode (file);
          if (inode == NULL || !inode_is_dir (inode))
            {
              f->eax = 0;
            }
          else
            {
              struct dir *dir = dir_open (inode_reopen (inode));
              if (dir == NULL)
                {
                  f->eax = 0;
                }
              else
                {
                  dir_set_pos (dir, file_tell (file));
                  f->eax = dir_readdir (dir, name);
                  file_seek (file, dir_get_pos (dir));
                  dir_close (dir);
                }
            }
        }
      lock_release (&filesys_lock);
      break;
    }
  /* SYS_ISDIR: Return true if fd refers to a directory. */
  case SYS_ISDIR:
    {
      int fd = (int) copy_word (uesp + 4);
      lock_acquire (&filesys_lock);
      struct file *file = process_get_file (fd);
      if (file == NULL)
        f->eax = 0;
      else
        {
          struct inode *inode = file_get_inode (file);
          f->eax = (inode != NULL && inode_is_dir (inode));
        }
      lock_release (&filesys_lock);
      break;
    }
  /* SYS_INUMBER: Return the inode number of the file or directory
     referred to by fd. */
  case SYS_INUMBER:
    {
      int fd = (int) copy_word (uesp + 4);
      lock_acquire (&filesys_lock);
      struct file *file = process_get_file (fd);
      if (file == NULL)
        f->eax = -1;
      else
        {
          struct inode *inode = file_get_inode (file);
          f->eax = (inode != NULL) ? inode_get_inumber (inode) : -1;
        }
      lock_release (&filesys_lock);
      break;
    }
  case SYS_GETUID:
    {
      /* No arguments. */
      f->eax = thread_current ()->uid;
      break;
    }
  case SYS_GETEUID:
    {
      f->eax = thread_current()->euid;
      break;
    }
  case SYS_GETGID:
    {
      f->eax = thread_current ()->gid;
      break;
    }

  case SYS_GETEGID:
    {
      f->eax = thread_current ()->egid;
      break;
    }
  case SYS_CHMOD:
    {
      const char *path = (const char *) (uintptr_t) copy_word (uesp + 4);
      unsigned mode = (unsigned) copy_word (uesp + 8);
      check_valid_string(path);
  
      /* MAC check for chmod capability */
      if (!mac_check_capability(thread_current()->mac_profile, "chmod")) {
        printf("Program '%s' not allowed to change permissions\n", thread_current()->mac_profile->name);
        f->eax = 0;
        break;
      }

      lock_acquire (&filesys_lock);
      struct file *file = filesys_open (path);
      if (file == NULL)
        {
          f->eax = 0;
        }
      else
        {
          struct inode *inode = file_get_inode (file);
          f->eax = inode_set_mode (inode, (uint16_t) mode);
          file_close (file);
        }
      lock_release (&filesys_lock);
      break;
    }

  case SYS_SETRESUID:
    {
      /* setresuid(ruid, euid, suid): each -1 means "don't change".
         Allowed if: caller is root (euid==0), OR each non-(-1) new value
         is in {current ruid, euid, suid}.  Mirrors Linux semantics. */
      uid_t new_ruid = (uid_t) copy_word (uesp + 4);
      uid_t new_euid = (uid_t) copy_word (uesp + 8);
      uid_t new_suid = (uid_t) copy_word (uesp + 12);
      struct thread *cur = thread_current ();

      if (cur->euid != ROOT_UID)
        {
          /* Unprivileged: each value must already be in {ruid, euid, suid}. */
          uid_t allowed[3] = { cur->uid, cur->euid, cur->suid };
          bool ok = true;
          for (int i = 0; i < 3; i++)
            {
              uid_t v = (i == 0) ? new_ruid : (i == 1) ? new_euid : new_suid;
              if (v == (uid_t)-1) continue;
              if (v != allowed[0] && v != allowed[1] && v != allowed[2])
                { ok = false; break; }
            }
          if (!ok) { f->eax = 0; break; }
        }

      if (new_ruid != (uid_t)-1) cur->uid  = new_ruid;
      if (new_euid != (uid_t)-1) cur->euid = new_euid;
      if (new_suid != (uid_t)-1) cur->suid = new_suid;
      f->eax = 1;
      break;
    }
  case SYS_SETRESGID:
    {
      uid_t new_rgid = (uid_t) copy_word (uesp + 4);
      uid_t new_egid = (uid_t) copy_word (uesp + 8);
      uid_t new_sgid = (uid_t) copy_word (uesp + 12);
      struct thread *cur = thread_current ();

      if (cur->euid != ROOT_UID)
        {
          uid_t allowed[3] = { cur->gid, cur->egid, cur->gid };
          bool ok = true;
          for (int i = 0; i < 3; i++)
            {
              uid_t v = (i == 0) ? new_rgid : (i == 1) ? new_egid : new_sgid;
              if (v == (uid_t)-1) continue;
              if (v != allowed[0] && v != allowed[1] && v != allowed[2])
                { ok = false; break; }
            }
          if (!ok) { f->eax = 0; break; }
        }

      if (new_rgid != (uid_t)-1) cur->gid  = new_rgid;
      if (new_egid != (uid_t)-1) cur->egid = new_egid;
      f->eax = 1;
      break;
    }
  case SYS_GETMODE:
    {
      int fd = (int) copy_word (uesp + 4);
      lock_acquire (&filesys_lock);
      struct file *file = process_get_file (fd);
      if (file == NULL)
        f->eax = -1;
      else
        f->eax = (int) inode_get_mode (file_get_inode (file));
      lock_release (&filesys_lock);
      break;
    }
  case SYS_CHOWN:
    {
      const char *path = (const char *) (uintptr_t) copy_word (uesp + 4);
      int uid = (int) copy_word (uesp + 8);
      int gid = (int) copy_word (uesp + 12);
      check_valid_string (path);

      lock_acquire (&filesys_lock);
      struct file *file = filesys_open (path);
      if (file == NULL)
        {
          f->eax = 0;
        }
      else
        {
          struct inode *inode = file_get_inode (file);
          f->eax = inode_set_owner (inode, (uint32_t) uid, (uint32_t) gid);
          file_close (file);
        }
      lock_release (&filesys_lock);
      break;
    }
  case SYS_GETOWNER:
    {
      int fd = (int) copy_word (uesp + 4);
      lock_acquire (&filesys_lock);
      struct file *file = process_get_file (fd);
      if (file == NULL)
        f->eax = -1;
      else
        f->eax = (int) inode_get_owner (file_get_inode (file));
      lock_release (&filesys_lock);
      break;
    }

#ifdef VM
  case SYS_MMAP:
    /* Probe syscall args only; mmap target addr may be NULL/misaligned. */
    check_address ((void *) (uintptr_t) (uesp + 4));
    check_address ((void *) (uintptr_t) (uesp + 8));
    f->eax = mmap_syscall ((int) copy_word (uesp + 4),
                           (void *) (uintptr_t) copy_word (uesp + 8));
    break;

  case SYS_MUNMAP:
    munmap_syscall ((int) copy_word (uesp + 4));
    break;
#endif

  default:
    /* Invalid system call number*/
    exit(-1);
  }
}

/* Finds an empty slot in the curernt thread's file descriptor table*/
static int process_add_file (struct file *f) {
  struct thread *cur = thread_current();
  for (int i = 2; i < 128; i++) {
    if (cur->fd_table[i] == NULL) {
      cur->fd_table[i] = f;
      return i;
    }
  }
  return -1;
}

/* Clears the file descriptor entry in the current thread's table*/
static void process_close_file (int fd) {
  struct thread *cur = thread_current();
  if (fd < 2 || fd >= 128) return;
  cur->fd_table[fd] = NULL;
}

/* Retrieves the file struct associated with a file descriptor */
static struct file *process_get_file (int fd) {
  struct thread *cur = thread_current ();
  if (fd < 2 || fd >= 128) return NULL;
  return cur->fd_table[fd];
}

/* 
 * Reads a byte at user virtual address UADDR.
 * UADDR must be below PHYS_BASE.
 * Returns the byte value if successful, -1 if a segfault occurred. 
 */
__attribute__((noinline, noclone))
int get_user (const uint8_t *uaddr) {
  int result;
  if (uaddr == NULL || !is_user_vaddr(uaddr)) {
    return -1;
  }
  asm volatile (
    "movl $1f, %0\n\t"
    ".globl begin_user_access\n"
    "begin_user_access:\n\t"
    "movzbl %1, %0\n\t"
    ".globl end_user_access\n"
    "end_user_access:\n\t"
    "1:"
    : "=&a" (result) : "m" (*uaddr));
  return result;
}
/*
 * Writes a single byte to user memory at the given virtual address (UDST).
 * Returns true if successful, false if a page fault occurred or the address is invalid.
 */
__attribute__((noinline, noclone))
bool put_user (uint8_t *udst, uint8_t byte) {
  int error_code;
  if (udst == NULL || !is_user_vaddr(udst)) return false;
  
  asm volatile (
    "movl $1f, %0\n\t"
    ".globl begin_user_write\n"
    "begin_user_write:\n\t"
    "movb %b2, %1\n\t"
    ".globl end_user_write\n"
    "end_user_write:\n\t"
    "1:"
    : "=&a" (error_code), "=m" (*udst) : "q" (byte));
    
  return error_code != -1;
}


/* Safely reads a 32-bit word from user memory at UADDR.
   Terminates the process with exit(-1) if any byte is unreadable. */
static int32_t
copy_word (const void *uaddr)
{
  const uint8_t *p = (const uint8_t *) uaddr;
  int32_t value = 0;

  for (int i = 0; i < 4; i++)
    {
      int b = get_user (p + i);
      if (b == -1)
        exit (-1);
      value |= ((int32_t) (uint8_t) b) << (i * 8);
    }
  return value;
}


/* 
 * Validates that a buffer is entirely within valid, writable user memory.
 * Instead of checking every single byte, it only checks the boundaries 
 * and then probes one byte per page (4KB). 
 */
/* Verifies that every byte in the provided buffer lies with in user virtual memory */
static void check_buffer(void *buffer, unsigned size) {
 if (size == 0) return;
  char *ptr = (char *)buffer;
  if (get_user((const uint8_t *)ptr) == -1) exit(-1);
  if (get_user((const uint8_t *)(ptr + size - 1)) == -1) exit(-1);

  for (unsigned i = 0; i < size; i += PGSIZE) {
    if (get_user((const uint8_t *)(ptr + i)) == -1) exit(-1);
  }
}

#ifdef VM
/* Pins user pages backing BUFFER for the duration of file-system I/O.
   If WRITABLE is true, the pages must have been mapped writable.
   This function also demand-loads missing stack pages (and any
   other not-yet-loaded SPTEs) so that no page faults can occur
   while filesys_lock is held in SYS_READ/SYS_WRITE. */
static void
pin_user_buffer (void *buffer, unsigned size, bool writable)
{
  if (size == 0)
    return;

  struct thread *t = thread_current ();
  uintptr_t start = (uintptr_t) buffer;
  uintptr_t end = start + size - 1;
  uintptr_t start_page = (uintptr_t) pg_round_down ((void *) start);
  uintptr_t end_page = (uintptr_t) pg_round_down ((void *) end);

  /* Mirror the stack-growth heuristic from exception.c. */
#define STACK_MAX_BYTES (8 * 1024 * 1024)
#define STACK_GROWTH_GAP 32

  for (uintptr_t addr = start_page; addr <= end_page; addr += PGSIZE)
    {
      void *fault_page = (void *) addr;
      struct sup_page_table_entry *spte = spt_find (&t->spt, fault_page);

      /* Create an SPTE for stack growth if needed. */
      if (spte == NULL)
        {
          if (t->user_esp != NULL &&
              (uintptr_t) fault_page >= (uintptr_t) t->user_esp - STACK_GROWTH_GAP &&
              (uintptr_t) fault_page < (uintptr_t) PHYS_BASE &&
              (uintptr_t) fault_page >= (uintptr_t) PHYS_BASE - STACK_MAX_BYTES)
            {
              spte = malloc (sizeof *spte);
              if (spte == NULL)
                exit (-1);
              memset (spte, 0, sizeof *spte);
              spte->upage = fault_page;
              spte->writable = true;
              spte->type = VM_ANON;
              spte->is_mmap = false;
              spte->is_loaded = false;
              spte->kpage = NULL;

              if (!spt_insert (&t->spt, spte))
                {
                  free (spte);
                  exit (-1);
                }
            }
          else
            exit (-1);
        }

      if (writable && !spte->writable)
        exit (-1);

      /* Demand-load the page if it isn't installed yet. */
      bool installed =
          pagedir_get_page (t->pagedir, spte->upage) != NULL;
      if (spte->kpage == NULL || !spte->is_loaded || !installed)
        {
          void *kpage = NULL;
          switch (spte->type)
            {
            case VM_SWAP:
              kpage = frame_alloc (0, spte);
              if (kpage == NULL)
                exit (-1);
              /* Pin immediately to avoid eviction races. */
              frame_pin (kpage);
              swap_in (spte->swap_slot, kpage);
              spte->swap_slot = SPTE_SWAP_SLOT_INVALID;
              break;
            case VM_ANON:
              kpage = frame_alloc (PAL_ZERO, spte);
              if (kpage == NULL)
                exit (-1);
              /* Pin immediately to avoid eviction races. */
              frame_pin (kpage);
              break;
            case VM_BIN:
            case VM_FILE:
              if (spte->type == VM_FILE && !spte->is_mmap)
                exit (-1);

              kpage = frame_alloc (0, spte);
              if (kpage == NULL)
                exit (-1);
              /* Pin immediately to avoid eviction races. */
              frame_pin (kpage);

              if (spte->read_bytes > 0)
                {
                  lock_acquire (&filesys_lock);
                  off_t nread = file_read_at (spte->file, kpage,
                                              (off_t) spte->read_bytes,
                                              (off_t) spte->offset);
                  lock_release (&filesys_lock);
                  if (nread < 0 || (size_t) nread != spte->read_bytes)
                    {
                      frame_free (kpage);
                      exit (-1);
                    }
                }
              if (spte->zero_bytes > 0)
                memset ((uint8_t *) kpage + spte->read_bytes, 0,
                        spte->zero_bytes);
              break;
            default:
              exit (-1);
            }

          if (!process_install_page (spte->upage, kpage, spte->writable))
            {
              frame_free (kpage);
              exit (-1);
            }

          pagedir_activate (t->pagedir);
          pagedir_set_dirty (t->pagedir, spte->upage, false);
        }

      if (spte->kpage == NULL)
        exit (-1);

      frame_pin (spte->kpage);
    }
}

static void
unpin_user_buffer (void *buffer, unsigned size)
{
  if (size == 0)
    return;

  struct thread *t = thread_current ();
  uintptr_t start = (uintptr_t) buffer;
  uintptr_t end = start + size - 1;
  uintptr_t start_page = (uintptr_t) pg_round_down ((void *) start);
  uintptr_t end_page = (uintptr_t) pg_round_down ((void *) end);

  for (uintptr_t addr = start_page; addr <= end_page; addr += PGSIZE)
    {
      struct sup_page_table_entry *spte = spt_find (&t->spt, (void *) addr);
      if (spte != NULL && spte->kpage != NULL)
        frame_unpin (spte->kpage);
    }
}
#endif

/* Verifies that a string is a valid null-terminated string located completely within user virtual memory */
static void check_valid_string(const void *str) {
  check_address((void *)str);
  char *ptr = (char *)str;
  while (*ptr != 0) {
    check_address((void *)(ptr + 1));
    ptr++;
  }
}

/* Sets the thread's exit status, prints the exit message */
void exit (int status) {
  struct thread *cur = thread_current();
  cur->exit_status = status;
  printf("%s: exit(%d)\n", cur->name, status);
  thread_exit();
}

/* 
 * Validates a user pointer by attempting to read from it.
 * Exits the process if it causes a page fault or is out of user space.
 */
void check_address(void * addr) {
  if (get_user((const uint8_t *)addr) == -1) {
    exit(-1);
  }
}