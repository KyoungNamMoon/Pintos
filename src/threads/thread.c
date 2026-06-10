#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/switch.h"
#include "threads/spinlock.h"
#include "threads/vaddr.h"
#include "filesys/filesys.h"
#include "threads/scheduler.h"
#ifdef USERPROG
#include "userprog/process.h"
#include "userprog/syscall.h"
#include "filesys/directory.h"
#endif
#include "threads/cpu.h"
#include "devices/lapic.h"
#include "lib/kernel/x86.h"
#include <atomic-ops.h>
#include "lib/kernel/bitmap.h"
#include "threads/ipi.h"
#include "devices/timer.h"

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;
static struct spinlock all_lock;
/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;
/* Stack frame for kernel_thread(). */
struct kernel_thread_frame
{
  void *eip; /* Return address. */
  thread_func *function; /* Function to call. */
  void *aux; /* Auxiliary data for function. */
};

static void kernel_thread_entry (thread_func *, void *aux);
static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static bool is_thread (struct thread *) UNUSED;
static void do_thread_exit (void) NO_RETURN;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);
static struct thread *do_thread_create (const char *, int, thread_func *, void *);
static void init_boot_thread (struct thread *boot_thread, struct cpu *cpu);
static void init_thread (struct thread *t, const char *name, int nice);
static void lock_own_ready_queue (void);
static void unlock_own_ready_queue (void);

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void)
{
  list_init (&all_list);
  list_init (&bcpu->sleep_list);
  spinlock_init (&bcpu->sleep_lock);
  spinlock_init (&all_lock);
  ASSERT (intr_get_level () == INTR_OFF);
  /* Initialize the scheduler for the Bootstrap CPU (bcpu). */
  sched_init (&bcpu->rq);
  spinlock_init (&bcpu->rq.lock);
  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_boot_thread (initial_thread, bcpu);
}

/*
 * Create the initial thread on an application processor/CPU.
 * Called once for each AP.  Initializes the run queue on that CPU.
 */
void
thread_init_on_ap (void)
{
  ASSERT (intr_get_level () == INTR_OFF);
  struct cpu *cpu = &cpus[lapic_get_cpuid ()];
  ASSERT(cpu != NULL);
  list_init (&cpu->sleep_list);
  spinlock_init (&cpu->sleep_lock);
  sched_init (&cpu->rq);
  spinlock_init (&cpu->rq.lock);
  struct thread *cur_thread = running_thread ();
  init_boot_thread (cur_thread, cpu);
}

/* Creates the idle thread on this CPU. */
void
thread_start_idle_thread (void)
{
  ASSERT (intr_get_level () == INTR_OFF);
  /* Create name for idle thread that indicates which CPU it is on */
  char idle_name[THREAD_NAME_MAX];
  snprintf (idle_name, THREAD_NAME_MAX, "idle_cpu%"PRIu8, get_cpu ()->id);

  /* Create the idle thread. */
  struct thread *idle_thread = do_thread_create (idle_name, NICE_MAX, idle, NULL);
  ASSERT (idle_thread);
  idle_thread->cpu = get_cpu ();
  get_cpu ()->rq.idle_thread = idle_thread;
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void)
{
  ASSERT (intr_get_level () == INTR_OFF);
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == get_cpu ()->rq.idle_thread)
    get_cpu ()->idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    get_cpu ()->user_ticks++;
#endif
  else
    get_cpu ()->kernel_ticks++;

  lock_own_ready_queue ();
  enum sched_return_action ret_action = sched_tick (&get_cpu ()->rq, t);
  if (ret_action == RETURN_YIELD)
    {
      /* We are processing an external interrupt, so we cannot yield
         right now. Instead, set a flag to yield at the end of the
         interrupt. */
      intr_yield_on_return ();
    }
  unlock_own_ready_queue ();
}

/* Prints thread statistics. */
void
thread_print_stats (void)
{
  struct cpu *c;
  for (c = cpus; c < cpus + ncpu; c++)
    {
      printf (
          "CPU%d: %llu idle ticks, %llu kernel ticks, %llu user ticks, %llu context switches\n",
          c->id, c->idle_ticks, c->kernel_ticks, c->user_ticks, c->cs);
    }
}

/* The default policy for choosing to which CPU to assign a
 * new thread is a round-robin policy.  If the other CPUs haven't
 * been started, we must choose CPU #0.
 */
static struct cpu *
choose_cpu_for_new_thread (struct thread *t)
{
  if (atomic_load (&cpu_started_others))
    return &cpus[t->tid % ncpu];
  else
    return &cpus[0];
}

/* Wake a new thread for the first time. Assign a CPU for it.
 * Add it to the new CPU's ready queue.
 *
 * `sched_unblock` requires that the current CPU's ready queue
 * be locked as well as the ready queue of the CPU onto which
 * the thread will be placed.  To avoid deadlock, we lock both
 * in increasing address order.
 */
static void
wake_up_new_thread (struct thread *t)
{
  intr_disable_push ();
  struct thread *curr = thread_current ();
  struct ready_queue *this_rq = &get_cpu ()->rq;
  ASSERT (this_rq->curr != NULL);

  t->status = THREAD_READY;
  t->cpu = choose_cpu_for_new_thread (t);
  struct ready_queue *other_rq = &t->cpu->rq;
  struct spinlock *lock1 = NULL;
  struct spinlock *lock2 = NULL;

  if (this_rq != other_rq) {
    lock1 = &this_rq->lock < &other_rq->lock ? &this_rq->lock : &other_rq->lock;
    lock2 = &this_rq->lock < &other_rq->lock ? &other_rq->lock : &this_rq->lock;
    spinlock_acquire (lock1);
    spinlock_acquire (lock2);
  } else {
    lock1 = &other_rq->lock;
    spinlock_acquire (lock1);
  }
  intr_enable_pop ();

  sched_unblock (&t->cpu->rq, t, 1, curr);
  spinlock_release (lock1);
  if (lock2 != NULL)
    spinlock_release (lock2);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument.
   Returns a pointer to the new thread's struct thread, or NULL
   if creation fails. */
static struct thread *
do_thread_create (const char *name, int nice, thread_func *function, void *aux)
{
    struct thread *t;
    struct kernel_thread_frame *kf;
    struct switch_entry_frame *ef;
    struct switch_threads_frame *sf;
    ASSERT (function != NULL);

    /* Allocate thread. */
    t = palloc_get_page (PAL_ZERO);
    if (t == NULL)
      return NULL;

    /* Initialize thread. */
    init_thread (t, name, nice);
    t->tid = allocate_tid ();

    /* Stack frame for kernel_thread(). */
    kf = alloc_frame (t, sizeof *kf);
    kf->eip = NULL;
    kf->function = function;
    kf->aux = aux;

    /* Stack frame for switch_entry(). */
    ef = alloc_frame (t, sizeof *ef);
    ef->eip = (void (*) (void)) kernel_thread_entry;

    /* Stack frame for switch_threads(). */
    sf = alloc_frame (t, sizeof *sf);
    sf->eip = switch_entry;
    sf->ebp = 0;

    /* CFS scheduling*/
    t->vruntime = 0;
    t->last_activation = 0;

    return t;
}

/* Creates a new thread and adds it to the ready queue.

   Returns the thread identifier for the new thread, or TID_ERROR
   if creation fails.

   Except during system startup, the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's 'nice' member to
   nice, but it's not actually used. You will implement it as part of
   Project 1. */
tid_t
thread_create (const char *name, int nice, thread_func *function, void *aux)
{
  struct thread *t;

  t = do_thread_create(name, nice, function, aux);
  if (t == NULL)
    return TID_ERROR;

  /* Must save tid here - 't' could already be freed when we return 
     from wake_up_new_thread */ 
  tid_t tid = t->tid;
  /* Add to ready queue. */
  wake_up_new_thread (t);
  return tid;
}

/* Puts the current thread to sleep (i.e., in the BLOCKED state).
   It will not be scheduled again until awoken by thread_unblock().

   This function must be called with interrupts on the current
   CPU turned off. It may not be called from an interrupt context.

   This function is usually called with a held spinlock to avoid
   lost wakeups.  If provided, this spinlock will be released
   before the thread is switched out, and reacquired upon return.

   This function is primarily used inside the threading system.
   For most other tasks, it is usually a better idea to use one
   of the synchronization primitives in synch.h. */
void
thread_block (struct spinlock *lk)
{
  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (!intr_context ());

  lock_own_ready_queue ();
  struct thread *curr = thread_current ();
  /* Release the provided lock (if any) before switching contexts. */
  if (lk != NULL)
    spinlock_release (lk);

  curr->status = THREAD_BLOCKED;
  sched_block (&get_cpu ()->rq, curr);
  schedule (); /* Switch to the next thread. */
  unlock_own_ready_queue ();

  if (lk != NULL)
    spinlock_acquire (lk);
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data.

   This function will lock the ready of the CPU to which the
   unblocked thread is assigned, as well as the ready queue of the
   current CPU, which may be different.  To avoid deadlock, we
   acquire these locks in address order. */
void
thread_unblock (struct thread *t)
{
  ASSERT (is_thread (t));
  ASSERT (t->cpu != NULL);

  struct spinlock *lock1 = NULL;
  struct spinlock *lock2 = NULL;

  intr_disable_push ();

  struct thread *curr = thread_current ();
  struct ready_queue *this_rq = &curr->cpu->rq;
  struct ready_queue *other_rq = &t->cpu->rq;

  /* Lock ready queues in address order to prevent deadlock. */
  if (this_rq != other_rq) {
    lock1 = &this_rq->lock < &other_rq->lock ? &this_rq->lock : &other_rq->lock;
    lock2 = &this_rq->lock < &other_rq->lock ? &other_rq->lock : &this_rq->lock;
    spinlock_acquire (lock1);
    spinlock_acquire (lock2);
  } else {
    lock1 = &other_rq->lock;
    spinlock_acquire (lock1);
  }

  ASSERT (t->status == THREAD_BLOCKED);
  t->status = THREAD_READY;

  enum sched_return_action ret_action;
  ret_action = sched_unblock (&t->cpu->rq, t, 0, curr->cpu->rq.curr);

  /* If the thread is on another CPU and needs preemption, send an IPI. */
  if (ret_action == RETURN_YIELD && t->cpu != get_cpu()) {
    lapic_send_ipi_to(IPI_SCHEDULE, t->cpu->id);
  }

  spinlock_release (lock1);
  if (lock2 != NULL)
    spinlock_release (lock2);


  intr_enable_pop ();

  /* If the current CPU needs preemption, yield now. */
  if (ret_action == RETURN_YIELD && t->cpu == get_cpu()) {
    if(intr_context()) {
      intr_yield_on_return();
    } else if (intr_get_level() == INTR_ON) {
      thread_yield();
    }
  }
}

/* Returns the name of the running thread. */
const char *
thread_name (void)
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void)
{
  struct thread *t = running_thread ();

  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);
//  TODO: ASSERT (t->cpu == get_cpu ());

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void)
{
  return thread_current ()->tid;
}

/* Lock our ready queue from a context in which interrupts (and thus preemption)
 * may be enabled.  To ensure that the CPU obtained by get_cpu() is not stale
 * by the time we use it, we first disable preemption.  Otherwise, we may be
 * preempted and possibly migrated to another CPU once load balancing is 
 * implemented.
 */
static void
lock_own_ready_queue (void)
{
  intr_disable_push ();
  spinlock_acquire (&get_cpu ()->rq.lock);
  intr_enable_pop ();
}

static void
unlock_own_ready_queue (void)
{
  spinlock_release (&get_cpu ()->rq.lock);
}

static void
do_thread_exit (void)
{
  struct thread * cur = thread_current ();

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  ASSERT (cpu_can_acquire_spinlock);

  spinlock_acquire (&all_lock);
  list_remove (&cur->allelem);
  spinlock_release (&all_lock);

  lock_own_ready_queue ();
  cur->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void)
{
  ASSERT(!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  do_thread_exit ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void)
{
  struct thread *cur = thread_current ();
  ASSERT (!intr_context ());

  lock_own_ready_queue ();

  cur->status = THREAD_READY;

  /* Add thread to ready list if it's not the idle thread. */
  if (cur != get_cpu ()->rq.idle_thread) {
    sched_yield (&get_cpu ()->rq, cur);
  }
  schedule ();
  unlock_own_ready_queue ();
}

/* Called from ap_main to terminate an AP's main thread.  */
void
thread_exit_ap (void)
{
  do_thread_exit ();
}

/* Invoke function 'func' on all threads, passing along 'aux'. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;
  spinlock_acquire (&all_lock);
  for (e = list_begin (&all_list); e != list_end (&all_list); e = list_next (e))
    {
      struct thread *t = list_entry(e, struct thread, allelem);
      func (t, aux);
    }
  spinlock_release (&all_lock);
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice)
{
  thread_current ()->nice = nice;
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void)
{
  return thread_current ()->nice;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED)
{
  for (;;) {
    intr_disable ();

    /*Load_balance*/
    load_balance ();

    struct ready_queue *rq = &get_cpu ()->rq;
    /* Check if load_balance successfully pulled any threads into the local ready list. */
    if (!list_empty (&rq->ready_list)) {
      lock_own_ready_queue ();
      struct thread *cur = thread_current ();
      /* Transition the idle thread's status. schedule() asserts that the 
             current thread is NOT in a RUNNING state when switching away. */
      cur->status = THREAD_BLOCKED; 
          
      schedule ();
      unlock_own_ready_queue ();
    } else {
      /* No work found*/
      asm volatile ("sti; hlt" : : : "memory");
    }
  }
}

/* A new thread's very first scheduling by scheduler () returns here.
 * Called with current CPU's ready queue lock held.
 */
static void
kernel_thread_entry (thread_func *function, void *aux)
{
  ASSERT (function != NULL);
  ASSERT (intr_get_level () == INTR_OFF);

  /* Set intena to 1, so a call to popcli () will enable interrupts */
  get_cpu ()->intena = 1;
  /*
   * At this point, the only lock that should be held
   * is the rq spinlock acquired by the previous thread
   */
  ASSERT (get_cpu ()->ncli == 1);
  unlock_own_ready_queue ();
  if (intr_get_level () == INTR_OFF)
    intr_enable ();
  ASSERT (intr_get_level () == INTR_ON);
  function (aux); /* Execute the thread function. */
  thread_exit (); /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void)
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Initialize the boot_thread as a Pintos thread. Boot threads
 * are special. They are not created with thread_create, but
 * rather they are threads created during the boot process
 * so that it can initialize the CPU. The boot thread for the
 * BSP is allocated by the boot loader, while the boot thread
 * for the AP's are allocated in start_other_cpus ().
 */
static void
init_boot_thread (struct thread *boot_thread, struct cpu *cpu)
{
  init_thread (boot_thread, "initial", NICE_DEFAULT);
  boot_thread->status = THREAD_RUNNING;
  boot_thread->tid = allocate_tid ();
  boot_thread->cpu = cpu;
  cpu->rq.curr = boot_thread;
  boot_thread->last_activation = timer_gettime();
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int nice)
{
  ASSERT (t != NULL);
  ASSERT (NICE_MIN <= nice && nice <= NICE_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->nice = nice;
  t->magic = THREAD_MAGIC;
  t->exit_status = 0;
  t->running_file = NULL;
#ifdef USERPROG
  if (t != initial_thread && running_thread () != t
      && thread_current ()->cwd != NULL)
    {
      lock_acquire (&filesys_lock);
      t->cwd = dir_reopen (thread_current ()->cwd);
      lock_release (&filesys_lock);
    }
#endif
  t->load_success = false;

  /* Initialize UID: Inherit from parent, or ROOT if initial/boot thread. */
  if (t == initial_thread || running_thread () == t) {
      t->uid = ROOT_UID;
      t->euid = ROOT_UID;
  } else {
      t->uid = thread_current()->uid;
      t->euid = thread_current()->euid;
  }
  
  /* Initiate FD table */
  for (int i = 0; i < 128; i++) {
    t->fd_table[i] = NULL;
  }

  sema_init (&t->wait_sema, 0);
  sema_init (&t->exit_sema, 0);
  list_init (&t->children);
  sema_init (&t->load_sema, 0);

  if (t != running_thread())  {
    struct thread *parent = thread_current();
    list_push_back (&parent->children, &t->child_elem);
  }

  if (cpu_can_acquire_spinlock)
    spinlock_acquire (&all_lock);
  list_push_back (&all_list, &t->allelem);
  if (cpu_can_acquire_spinlock)
    spinlock_release (&all_lock);
  
  #ifdef USERPROG
    t->uid = 0;
    t->euid = 0;
    t->suid = 0;
    t->gid = 0;
    t->egid = 0;
    t->mac_profile = NULL;
  #endif
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size)
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof(uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   this CPU's idle_thread. */
static struct thread *
next_thread_to_run (void)
{
  struct thread *ret = NULL;
  ret = sched_pick_next (&get_cpu ()->rq);
  if (!ret)
      ret = get_cpu ()->rq.idle_thread;

  return ret;
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();

  ASSERT(intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  cur->last_activation = timer_gettime();

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread)
    {
      ASSERT(prev != cur);
      palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, the current CPU's ready queue
   must have been locked, and the running process's state must have been
   changed from running to some other state.  This function finds another
   thread to run and switches to it. */
static void
schedule (void)
{
  /* Schedule must be called with current CPU's ready queue lock held */
  ASSERT (spinlock_held_by_current_cpu (&get_cpu ()->rq.lock));

  /* Must not hold any other spinlocks, since interrupt handlers might
     attempt to acquire them, leading to deadlock since the outgoing
     thread would not be able to release them.
   */
  ASSERT (get_cpu ()->ncli == 1);

  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;
  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));
  int intena = get_cpu ()->intena;      /* Save current value of intena. */
  if (cur != next)
    {
      get_cpu ()->cs++;
      get_cpu ()->rq.curr = next == get_cpu ()->rq.idle_thread ? NULL : next;
      prev = switch_threads (cur, next);
    }

  get_cpu ()->intena = intena;          /* Restore value of intena. */
  thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void)
{
  static tid_t next_tid = 0;
  return atomic_inci (&next_tid);
}

/* Returns true if thread a is earlier than thread b*/
bool sorted_wakeup_tick(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
  struct thread *ta = list_entry (a, struct thread, elem);
  struct thread *tb = list_entry (b, struct thread, elem);

  return ta->wakeup_tick < tb->wakeup_tick;

}

/* Wakes up threads whose wakeup time has arrived. Called by the timer interrupt. */
void thread_awake(int64_t ticks) {
  ASSERT (intr_get_level () == INTR_OFF);
  struct cpu *c = get_cpu();

  spinlock_acquire(&c->sleep_lock);

  struct list_elem *e = list_begin (&c->sleep_list);

  while (e != list_end (&c->sleep_list)) {
    struct thread *t = list_entry(e, struct thread, elem);
    if (t->wakeup_tick <= ticks) {
        e = list_remove(e);
        thread_unblock(t);
    } else {
        break; 
    }
  }
  spinlock_release(&c->sleep_lock);

}

/* Transitions the current running thread to a BLOCKED state until the system timer reaches the specified time. */
void thread_sleep(int64_t ticks) {
  struct thread *cur = thread_current();
  intr_disable_push ();
  struct cpu *c = get_cpu();

  spinlock_acquire (&c->sleep_lock);
  intr_enable_pop ();
  if (cur != c->rq.idle_thread) {
    cur->wakeup_tick = ticks;
    list_insert_ordered(&c->sleep_list, &cur->elem, &sorted_wakeup_tick, NULL);
    thread_block(&c->sleep_lock);
  } 
  spinlock_release (&c->sleep_lock);
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof(struct thread, stack);
