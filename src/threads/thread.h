#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "filesys/file.h"
#include "threads/synch.h"
#include "lib/kernel/list.h"
/* States in a thread's life cycle. */
enum thread_status
{
  THREAD_RUNNING,       /* Running thread. */
  THREAD_READY,         /* Not running but ready to run. */
  THREAD_BLOCKED,       /* Waiting for an event to trigger. */
  THREAD_DYING          /* About to be destroyed. */
};

/* ========================================================== */
/* Add this for multi-user support & access control */
typedef int uid_t;
#define ROOT_UID 0
/* ========================================================== */

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */
#define THREAD_NAME_MAX 16
/* Thread priorities. */
#define NICE_MIN -20                    /* Highest priority. */
#define NICE_DEFAULT 0                  /* Default priority. */
#define NICE_MAX 19                     /* Lowest priority. */

/* A kernel thread or user process.

   Each thread structure is stored in its own 4 kB page.  The
   thread structure itself sits at the very bottom of the page
   (at offset 0).  The rest of the page is reserved for the
   thread's kernel stack, which grows downward from the top of
   the page (at offset 4 kB).  Here's an illustration:

        4 kB +---------------------------------+
             |          kernel stack           |
             |                |                |
             |                |                |
             |                V                |
             |         grows downward          |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             +---------------------------------+
             |              magic              |
             |                :                |
             |                :                |
             |               name              |
             |              status             |
        0 kB +---------------------------------+

   The upshot of this is twofold:

      1. First, `struct thread' must not be allowed to grow too
         big.  If it does, then there will not be enough room for
         the kernel stack.  Our base `struct thread' is only a
         few bytes in size.  It probably should stay well under 1
         kB.

      2. Second, kernel stacks must not be allowed to grow too
         large.  If a stack overflows, it will corrupt the thread
         state.  Thus, kernel functions should not allocate large
         structures or arrays as non-static local variables.  Use
         dynamic allocation with malloc() or palloc_get_page()
         instead.

   The first symptom of either of these problems will probably be
   an assertion failure in thread_current(), which checks that
   the `magic' member of the running thread's `struct thread' is
   set to THREAD_MAGIC.  Stack overflow will normally change this
   value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
   the run queue (thread.c), or it can be an element in a
   semaphore wait list (synch.c).  It can be used these two ways
   only because they are mutually exclusive: only a thread in the
   ready state is on the run queue, whereas only a thread in the
   blocked state is on a semaphore wait list. */

#ifdef VM
struct process_mmap;
#endif

struct thread
{
  /* Owned by thread.c. */
  tid_t tid; /* Thread identifier. */
  uid_t uid;  /* Real user ID. */
  uid_t euid; /* Effective user ID (permission checks). */
  uid_t suid; /* Saved set-user-ID (allows privilege restoration). */
  uid_t gid;  /* Real group ID. */
  uid_t egid; /* Effective group ID (permission checks). */
  struct security_profile *mac_profile; /* MAC checks */
  enum thread_status status; /* Thread state. */
  char name[THREAD_NAME_MAX]; /* Name (for debugging purposes). */
  uint8_t *stack; /* Saved stack pointer. */
  int nice; /* Nice value. */
  int exit_status; /* exit code */
  struct file *fd_table[128]; /* file descriptor table */
  struct dir *cwd;   /* Current working directory. NULL means root. */
  struct file *running_file; /* current fd denying write to executables */
  struct list_elem allelem; /* List element for all threads list. */
  int64_t wakeup_tick; /* Time the thread should wake up */
  uint64_t vruntime; /*For CFS scheduling; total usage time*/
  uint64_t last_activation; /* last usage of CPU*/
  uint64_t run_start_time; /* Time when the thread started running on CPU */
  struct cpu *cpu; /* Points to the CPU this thread is currently bound to.
                      thread_unblock () will add a thread to the rq of
                      this CPU.  A load balancer needs to update this
                      field when migrating threads.
                    */
   
   /* Process wait / exec synchronization. */
  struct semaphore wait_sema;   /* Child blocks in exit until parent reaps (wait()) */
  struct semaphore exit_sema;   /* Parent blocks in wait() until child signals exit */
  struct list children;         /* Parent's list of child threads */
  struct list_elem child_elem;  /* Child's link in parent's children list */
  struct semaphore load_sema;   /* Parent blocks in exec until child finishes load() */
  bool load_success;            /* Child sets true iff load() succeeded */
  
  /* Shared between thread.c and synch.c. */
  struct list_elem elem; /* List element. */

  struct list spt;
#ifdef VM
  /* Heap-allocated in start_process — keeps struct thread small (kernel stack). */
  struct process_mmap *mmap;
  /* Saved user stack pointer on transition into the kernel (for kernel-mode
     page faults triggered during get_user/put_user validation). */
  void *user_esp;
#endif

#ifdef USERPROG
  /* Owned by userprog/process.c. */
  uint32_t *pagedir; /* Page directory. */
#endif
  /* Owned by thread.c. */
  unsigned magic; /* Detects stack overflow. */
};

void thread_init (void);
void thread_init_on_ap (void);
void thread_start_idle_thread (void);
void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (struct spinlock *);
void thread_unblock (struct thread *);
struct thread *running_thread (void);
struct thread * thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);
void thread_awake(int64_t); /* Move thread from sleep list to ready list if ready*/
void thread_sleep(int64_t); /* Move thread to sleep list */
bool sorted_wakeup_tick(const struct list_elem *, const struct list_elem *, void *);
void thread_exit (void) NO_RETURN;
void thread_yield (void);
void thread_exit_ap (void) NO_RETURN;
/* Performs some operation on thread t, given auxiliary data AUX. */
typedef void thread_action_func (struct thread *t, void *aux);
void thread_foreach (thread_action_func *, void *);
int thread_get_nice (void);
void thread_set_nice (int);

#endif /* threads/thread.h */
