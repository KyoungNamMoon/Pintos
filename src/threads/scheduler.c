#include "threads/scheduler.h"
#include "threads/cpu.h"
#include "threads/interrupt.h"
#include "list.h"
#include "threads/spinlock.h"
#include <debug.h>
#include "devices/timer.h"
#include <stdio.h>

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
#define SLEEP_BONUS 20000000


/* * Nice value to Weight conversion table.
 * Maps nice values [-20, 19] to weights to ensure fair CPU distribution.
 * A difference of 1 in nice value corresponds to a ~10% difference in CPU share.
 */
static const uint32_t prio_to_weight[40] = {
    /* -20 */    88761, 71755, 56483, 46273, 36291,
    /* -15 */    29154, 23254, 18705, 14949, 11916,
    /* -10 */    9548, 7620, 6100, 4904, 3906,
    /*  -5 */    3121, 2501, 1991, 1586, 1277,
    /*   0 */    1024, 820, 655, 526, 423,
    /*   5 */    335, 272, 215, 172, 137,
    /*  10 */    110, 87, 70, 56, 45,
    /*  15 */    36, 29, 23, 18, 15,
};

/* Returns the weight associated with a thread's nice value. */
static int get_weight(int nice) {
  int nice_idx = nice + 20;
  if (nice_idx < 0) nice_idx = 0;
  if (nice_idx > 39) nice_idx = 39;
  return prio_to_weight[nice_idx];
}

/* 
 * Comparator for the ready list.
 * Threads are sorted by vruntime. 
 * If vruntimes are equal, the thread with the smaller TID is picked to ensure 
 * deterministic behavior.
 */
static bool thread_compare_vruntime (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
  struct thread *t_a = list_entry(a, struct thread, elem);
  struct thread *t_b = list_entry(b, struct thread, elem);
  if (t_a->vruntime != t_b->vruntime) {
    return t_a->vruntime < t_b->vruntime;
  }
  return t_a->tid < t_b->tid;
  
}

/* 
 * Updates the current thread's vruntime based on actual execution time.
 * Also tracks the minimum vruntime (min_vruntime) of the ready queue to 
 * prevent newly awoken threads from starving existing ones.
 */
static void update_curr(struct ready_queue *rq) {
  struct thread *curr = rq->curr;
  uint64_t now = timer_gettime();
  
  if (curr != NULL && curr != rq->idle_thread) {
    uint64_t delta_time = 0;
    /* Calculate wall-clock time elapsed since last update. */
    if (now > curr->last_activation) {
      delta_time = now - curr->last_activation;
    }

    curr->last_activation = now;

    /* Calculate delta_vruntime: physical_time * (NICE_0_LOAD / weight). */
    int weight = get_weight(curr->nice);
    uint64_t delta_vruntime = (delta_time * 1024) / weight;
    curr->vruntime += delta_vruntime;
  }

  /* Update min_vruntime. */
  uint64_t cur_vruntime = (curr && curr != rq->idle_thread) ? curr->vruntime : UINT64_MAX;
  uint64_t readylist_min = UINT64_MAX;

  if (!list_empty(&rq->ready_list)) {
    struct thread *front = list_entry(list_front(&rq->ready_list), struct thread, elem);
    readylist_min = front->vruntime;
  }

  uint64_t real_min = (cur_vruntime < readylist_min) ? cur_vruntime : readylist_min;

  /* min_vruntime must monotonically increase. */
  if (real_min != UINT64_MAX) {
    if (real_min > rq->min_vruntime) {
      rq->min_vruntime = real_min;
    }
  }
}

/*
 * In the provided baseline implementation, threads are kept in an unsorted list.
 *
 * Threads are added to the back of the list upon creation.
 * The thread at the front is picked for scheduling.
 * Upon preemption, the current thread is added to the end of the queue
 * (in sched_yield), creating a round-robin policy if multiple threads
 * are in the ready queue.
 * Preemption occurs every TIME_SLICE ticks.
 */

/* Called from thread_init () and thread_init_on_ap ().
   Initializes data structures used by the scheduler. 
 
   This function is called very early.  It is unsafe to call
   thread_current () at this point.
 */
void
sched_init (struct ready_queue *curr_rq)
{
  list_init (&curr_rq->ready_list);
  curr_rq->load_weight = 0;
  curr_rq->min_vruntime = 0;
  curr_rq->curr = NULL;      
  curr_rq->idle_thread = NULL;
  spinlock_init(&curr_rq->lock);
}

/* Called from thread.c:wake_up_new_thread () and
   thread_unblock () with the current CPU's ready queue
   locked (and preemption disabled).
   rq is the ready queue that t should be added to when
   it is awoken. It is not necessarily the current CPU.

   If called from wake_up_new_thread (), initial will be 1.
   If called from thread_unblock (), initial will be 0.

   If called from the idle thread, curr will be NULL.

   Returns RETURN_YIELD if the CPU containing rq should
   be rescheduled when this function returns, else returns
   RETURN_NONE */
enum sched_return_action
sched_unblock (struct ready_queue *
  rq_to_add, struct thread *t, int initial UNUSED, struct thread *curr UNUSED)
{
  update_curr(rq_to_add);

  if (initial) {
    /* New threads inherit the current min_vruntime. */
    t->vruntime = rq_to_add->min_vruntime;
    t->last_activation = timer_gettime();
  } else {
    /* Sleeper bonus*/
    uint64_t bonus_vruntime = 0;
    if (rq_to_add->min_vruntime > SLEEP_BONUS) {
      bonus_vruntime = rq_to_add->min_vruntime - SLEEP_BONUS;
    }
    if (t->vruntime < bonus_vruntime) {
      t->vruntime = bonus_vruntime;
    }
  }

  t->last_activation = timer_gettime();
  list_insert_ordered(&rq_to_add->ready_list, &t->elem, thread_compare_vruntime, NULL);
  rq_to_add->nr_ready++;
  rq_to_add->load_weight += get_weight(t->nice);

  /* Yield if the CPU is idle or the new thread has a smaller vruntime. */
  if (!rq_to_add->curr || rq_to_add->curr == rq_to_add->idle_thread) {
      return RETURN_YIELD;
  }

  if (t->vruntime < rq_to_add->curr->vruntime) {
    return RETURN_YIELD;
  }
 
  return RETURN_NONE;
}

/* Called from thread_yield ().
   Current thread is about to yield.  Add it to the ready list

   Current ready queue is locked upon entry.
 */
void
sched_yield (struct ready_queue *curr_rq, struct thread *current)
{
  update_curr(curr_rq);
  list_insert_ordered (&curr_rq->ready_list, &current->elem, thread_compare_vruntime, NULL);
  curr_rq->nr_ready ++;
  curr_rq->load_weight += get_weight(current->nice);
}

/* Called from next_thread_to_run ().
   Find the next thread to run and remove it from the ready list
   Return NULL if the ready list is empty.

   If the thread returned is different from the thread currently
   running, a context switch will take place.

   Called with current ready queue locked.
 */
struct thread *
sched_pick_next (struct ready_queue *curr_rq)
{
  if (list_empty (&curr_rq->ready_list))
    return NULL;

  struct thread *ret = list_entry(list_pop_front (&curr_rq->ready_list), struct thread, elem);
  curr_rq->nr_ready--;
  curr_rq->load_weight -= get_weight(ret->nice);

  uint64_t now = timer_gettime();
  ret->run_start_time = now;
  ret->last_activation = now;
 
  return ret;
}

/* Called from thread_tick ().
 * Ready queue rq is locked upon entry.
 *
 * Check if the current thread has finished its timeslice,
 * arrange for its preemption if it did.
 *
 * Returns RETURN_YIELD if current thread should yield
 * when this function returns, else returns RETURN_NONE.
 */
enum sched_return_action
sched_tick (struct ready_queue *curr_rq, struct thread *current UNUSED)
{
  update_curr(curr_rq);


  if (current == curr_rq->idle_thread || list_empty(&curr_rq->ready_list))
    return RETURN_NONE;

  /* 
   * Calculate 'ideal_runtime'. 
   * Threads with more weight (lower nice) get longer physical time slices.
   */
  int curr_weight = get_weight(current->nice);
  uint64_t total_weight = curr_rq->load_weight + curr_weight;

  int n = 1 + curr_rq->nr_ready;
  uint64_t ideal_runtime = 4000000ULL * n * curr_weight / total_weight;

  if (ideal_runtime < 1000) ideal_runtime = 1000;
  uint64_t time_run = timer_gettime() - current->run_start_time;

  /* Preempt if the current thread has exhausted its share. */
  if (time_run >= ideal_runtime) {
    curr_rq->thread_ticks = 0; 
    return RETURN_YIELD;
  }
  return RETURN_NONE;
}

/* Called from thread_block (). The base scheduler does
   not need to do anything here, but your scheduler may. 

   'cur' is the current thread, about to block.
 */
void
sched_block (struct ready_queue *rq UNUSED, struct thread *current UNUSED)
{
  update_curr(rq);
}

/* * Load Balancing Logic.
 * Identifies the busiest CPU and migrates threads to the current CPU 
 * to equalize the total load weight across all cores.
 */
void
load_balance (void)
{
  struct cpu *my_cpu = get_cpu ();
  struct ready_queue *curr_rq = &my_cpu->rq;
  struct ready_queue *busiest_rq = NULL;
  uint64_t busiest_load = 0;

  /* Identify the busiest CPU. */
  struct cpu *c;
  for (c = cpus; c < cpus + ncpu; c++)
    {
      struct ready_queue *remote_rq = &c->rq;
      if (remote_rq == curr_rq) continue;

      if (remote_rq->load_weight > busiest_load)
        {
          busiest_load = remote_rq->load_weight;
          busiest_rq = remote_rq;
        }
    }

  /* Exit if no candidate found or the busiest queue is empty. */
  if (busiest_rq == NULL || list_empty (&busiest_rq->ready_list))
    return;

  /* Calculate imbalance. */
  uint64_t my_load = curr_rq->load_weight;
  uint64_t imbalance = (busiest_load - my_load) / 2;

  if (imbalance * 4 < busiest_load)
    return;

  /* Deadlock*/
  struct spinlock *l1 = &curr_rq->lock < &busiest_rq->lock ? &curr_rq->lock : &busiest_rq->lock;
  struct spinlock *l2 = &curr_rq->lock < &busiest_rq->lock ? &busiest_rq->lock : &curr_rq->lock;
  
  spinlock_acquire (l1);
  spinlock_acquire (l2);

  uint64_t migrated_weight = 0;
  while (migrated_weight < imbalance && !list_empty (&busiest_rq->ready_list))
    {
      /* Pull a thread from the back of the busiest queue. */
      struct list_elem *e = list_pop_back (&busiest_rq->ready_list);
      struct thread *t = list_entry (e, struct thread, elem);
        
      busiest_rq->nr_ready--;
      busiest_rq->load_weight -= get_weight (t->nice);

      /*vruntime_0 = vruntime - busiest_cpu_minvruntime + my_minvruntime */
      uint64_t busy_min = busiest_rq->min_vruntime;
      uint64_t my_min = curr_rq->min_vruntime;

      if (t->vruntime > busy_min)
        t->vruntime = my_min + (t->vruntime - busy_min);
      else {
        uint64_t diff = busy_min - t->vruntime;
        t->vruntime = (my_min > diff) ? my_min - diff : 0; /* Ensure non-negative */
      }
    
      t->cpu = my_cpu; 
      list_insert_ordered (&curr_rq->ready_list, &t->elem, thread_compare_vruntime, NULL);
      
      curr_rq->nr_ready++;
      uint64_t w = get_weight (t->nice);
      curr_rq->load_weight += w;
      migrated_weight += w;
    }

  spinlock_release (l2);
  spinlock_release (l1);
}