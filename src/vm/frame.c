#include "vm/frame.h"
#include "vm/swap.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "filesys/file.h"
#include "threads/synch.h"
#include <stddef.h>
#include <string.h>

extern struct lock filesys_lock;

/* Match vm/mmap.c: do not extend file with zero tail on write-back. */
static size_t
mmap_writeback_size (struct file *f, off_t page_off)
{
  off_t flen = file_length (f);
  if (page_off >= flen)
    return PGSIZE;
  if (page_off + (off_t) PGSIZE > flen)
    return (size_t) (flen - page_off);
  return PGSIZE;
}

/* Maximum number of frames that can be tracked at once. */
#define MAX_USER_FRAMES 4096

/* One entry per in-use frame in the user pool.
   Free frames are not tracked — only occupied ones. */
struct frame_entry
{
  void                        *kpage;  /* Kernel virtual address of frame. */
  struct sup_page_table_entry *spte;   /* Page currently occupying frame. */
  struct thread               *owner;  /* Thread that owns this frame. */
  bool                         pinned; /* If true, skip during eviction. */
  bool                         in_use; /* True if slot is occupied. */
};

/* Table of all in-use frames. */
static struct frame_entry frame_table[MAX_USER_FRAMES];

/* Number of frame table slots currently occupied. */
static size_t frame_count;

/* Protects all frame table operations. */
static struct lock frame_lock;

/* Position of the clock hand for the eviction algorithm.
   Saved between calls so the clock sweeps continuously. */
static size_t clock_hand;

/* Returns the index of the first unused slot in frame_table,
   or MAX_USER_FRAMES if the table is full. */
static size_t
find_free_slot (void)
{
  for (size_t i = 0; i < MAX_USER_FRAMES; i++)
    if (!frame_table[i].in_use)
      return i;
  return MAX_USER_FRAMES;
}

/* Selects a victim frame using the clock algorithm and evicts it.
   Frames with the accessed bit set are given a second chance.
   Dirty pages are written to swap/file as needed.
   Returns the kernel virtual address of the freed physical page
   for direct reuse by the caller (NOT returned to palloc).
   Must be called with frame_lock held. */
static void *
evict_one (void)
{
  for (;;)
    {
      size_t i = clock_hand;
      clock_hand = (clock_hand + 1) % MAX_USER_FRAMES;

      struct frame_entry *fe = &frame_table[i];

      if (!fe->in_use || fe->pinned)
        continue;

      void     *kpage = fe->kpage;
      void     *upage = fe->spte->upage;
      uint32_t *pd    = fe->owner->pagedir;

      bool accessed = pagedir_is_accessed (pd, upage)
                      || pagedir_is_accessed (pd, kpage);
      if (accessed)
        {
          pagedir_set_accessed (pd, upage, false);
          pagedir_set_accessed (pd, kpage, false);
          continue;
        }

      bool dirty = (fe->spte->is_mmap && fe->spte->type == VM_FILE)
                       ? fe->spte->writable
                       : (pagedir_is_dirty (pd, upage)
                          || pagedir_is_dirty (pd, kpage));

      if (fe->spte->type == VM_ANON || fe->spte->type == VM_SWAP)
        dirty = true;

      /* SMP: writable VM_BIN (.bss, data) must not use the clean eviction path. */
      if (fe->spte->type == VM_BIN && fe->spte->writable)
        dirty = true;

      pagedir_clear_page (pd, upage);
      fe->spte->is_loaded = false;

      if (dirty)
        {
          fe->pinned = true;
          struct sup_page_table_entry *spte = fe->spte;

          if (spte->is_mmap && spte->type == VM_FILE)
            {
              bool need_unlock = false;
              if (!lock_held_by_current_thread (&filesys_lock))
                {
                  lock_acquire (&filesys_lock);
                  need_unlock = true;
                }
              if (spte->writable)
                {
                  size_t n = mmap_writeback_size (spte->file,
                                                  (off_t) spte->offset);
                  if (n > 0)
                    file_write_at (spte->file, kpage, n,
                                   (off_t) spte->offset);
                }
              if (need_unlock)
                lock_release (&filesys_lock);
              fe->pinned  = false;
              spte->kpage = NULL;
            }
          else
            {
              size_t slot = swap_out (kpage);
              fe->pinned      = false;
              spte->type      = VM_SWAP;
              spte->swap_slot = slot;
              spte->kpage     = NULL;
            }
        }
      else
        {
          fe->spte->kpage = NULL;
        }

      fe->in_use = false;
      fe->spte   = NULL;
      fe->owner  = NULL;
      fe->pinned = false;
      fe->kpage  = NULL;

      return kpage;
    }
}

/* Initializes the frame table.  Must be called once at boot
   after palloc_init() and before any user process runs. */
void
frame_init (void)
{
  lock_init (&frame_lock);
  clock_hand  = 0;
  frame_count = 0;
  memset (frame_table, 0, sizeof frame_table);
}

/* Obtains a physical frame for SPTE.  Tries palloc first; if the
   user pool is exhausted, evicts a frame using the clock algorithm
   and reuses the evicted page directly.  Returns the kernel virtual
   address of the frame, or NULL on failure. */
void *
frame_alloc (enum palloc_flags flags, struct sup_page_table_entry *spte)
{
  void *kpage = palloc_get_page (flags | PAL_USER);

  lock_acquire (&frame_lock);

  if (kpage == NULL)
    {
      kpage = evict_one ();
      if (kpage == NULL)
        {
          lock_release (&frame_lock);
          return NULL;
        }
      if (flags & PAL_ZERO)
        memset (kpage, 0, PGSIZE);
    }

  size_t idx = find_free_slot ();
  ASSERT (idx < MAX_USER_FRAMES);

  frame_table[idx].kpage   = kpage;
  frame_table[idx].spte    = spte;
  frame_table[idx].owner   = thread_current ();
  frame_table[idx].pinned  = true;
  frame_table[idx].in_use  = true;

  if (idx >= frame_count) 
    frame_count = idx + 1;

  spte->kpage     = kpage;
  spte->is_loaded = true;

  lock_release (&frame_lock);
  return kpage;
}

/* Frees the frame at kernel virtual address KPAGE, removing its
   entry from the frame table and returning it to the user pool. */
void
frame_free (void *kpage)
{
  lock_acquire (&frame_lock);

  /* Linear search for the frame table entry matching this kpage */
  for (size_t i = 0; i < MAX_USER_FRAMES; i++)
    {
      if (frame_table[i].in_use && frame_table[i].kpage == kpage)
        {
          /* Clear the frame table entry */
          struct sup_page_table_entry *spte = frame_table[i].spte;
          frame_table[i].in_use = false;
          frame_table[i].spte   = NULL;
          frame_table[i].owner  = NULL;
          frame_table[i].pinned = false;
          frame_table[i].kpage  = NULL;

          /* Keep SPTE metadata consistent.
             Some call sites use frame_free() on failure paths; without
             clearing spte->is_loaded here, process_exit/pagedir teardown can
             attempt to free the same physical page again. */
          if (spte != NULL)
            {
              spte->kpage = NULL;
              spte->is_loaded = false;
            }
          break;
        }
    }

  lock_release (&frame_lock);
  palloc_free_page (kpage);
}

/* Pins the frame at KPAGE so it will not be evicted.
   Used when kernel code is accessing a user frame during I/O. */
void
frame_pin (void *kpage)
{
  lock_acquire (&frame_lock);
  for (size_t i = 0; i < MAX_USER_FRAMES; i++)
    if (frame_table[i].in_use && frame_table[i].kpage == kpage)
      {
        frame_table[i].pinned = true;
        break;
      }
  lock_release (&frame_lock);
}

/* Unpins the frame at KPAGE, making it a candidate for eviction
   again.  Must be called after frame_pin() once I/O is complete. */
void
frame_unpin (void *kpage)
{
  lock_acquire (&frame_lock);
  for (size_t i = 0; i < MAX_USER_FRAMES; i++)
    if (frame_table[i].in_use && frame_table[i].kpage == kpage)
      {
        frame_table[i].pinned = false;
        break;
      }
  lock_release (&frame_lock);
}