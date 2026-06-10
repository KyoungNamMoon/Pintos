#include "vm/swap.h"
#include "devices/block.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include <bitmap.h>
#include <debug.h>

/* Number of disk sectors per page. */
#define SECTORS_PER_PAGE (PGSIZE / BLOCK_SECTOR_SIZE)

/* The swap disk block device. */
static struct block *swap_block;

/* Tracks which swap slots are free.
   Bit i is true if slot i is available, false if in use. */
static struct bitmap *swap_map;

/* Protects swap_map.  Released before disk I/O to allow
   parallel swap operations. */
static struct lock swap_lock;

/* Initializes the swap table.  Obtains the swap block device,
   creates a bitmap over all swap slots, and marks them all free.
   Must be called after locate_block_devices() in init.c. */
void
swap_init (void)
{
  swap_block = block_get_role (BLOCK_SWAP);
  if (swap_block == NULL) {
    return; 
  }
  
  size_t slot_count = block_size (swap_block) / SECTORS_PER_PAGE;
  swap_map = bitmap_create (slot_count);
  ASSERT (swap_map != NULL);

  bitmap_set_all (swap_map, true);
  lock_init (&swap_lock);
}

/* Writes the page at KPAGE to a free swap slot and returns the
   slot index.  Reserves the slot under the lock, then releases
   it before writing so other threads are not blocked during I/O.
   Panics the kernel if no swap slots are available. */
size_t
swap_out (void *kpage)
{
  /* swap_* can be reached via page faults while the current thread is
     already inside swap_* (e.g., nested demand paging). Pintos locks are
     non-recursive, so avoid re-acquiring swap_lock from the same thread. */
  bool already_held = lock_held_by_current_thread (&swap_lock);
  if (!already_held)
    lock_acquire (&swap_lock);
  size_t slot = bitmap_scan_and_flip (swap_map, 0, 1, true);
  ASSERT (slot != BITMAP_ERROR);
  if (!already_held)
    lock_release (&swap_lock);

  /* Write page to disk one sector at a time */
  for (size_t i = 0; i < SECTORS_PER_PAGE; i++)
    block_write (swap_block,
                 slot * SECTORS_PER_PAGE + i,
                 (uint8_t *) kpage + i * BLOCK_SECTOR_SIZE);

  return slot;
}

/* Reads swap slot SLOT back into the frame at KPAGE and frees
   the slot.  I/O is done without the lock; the lock is acquired
   only to update the bitmap afterward. */
void
swap_in (size_t slot, void *kpage)
{
  /* Read page from disk one sector at a time */
  for (size_t i = 0; i < SECTORS_PER_PAGE; i++)
    block_read (swap_block,
                slot * SECTORS_PER_PAGE + i,
                (uint8_t *) kpage + i * BLOCK_SECTOR_SIZE);

  bool already_held = lock_held_by_current_thread (&swap_lock);
  if (!already_held)
    lock_acquire (&swap_lock);
  ASSERT (!bitmap_test (swap_map, slot));
  bitmap_set (swap_map, slot, true);
  if (!already_held)
    lock_release (&swap_lock);
}

/* Frees swap slot SLOT without reading it back into memory.
   Called on process exit to avoid leaking swap slots. */
void
swap_free (size_t slot)
{
  bool already_held = lock_held_by_current_thread (&swap_lock);
  if (!already_held)
    lock_acquire (&swap_lock);
  ASSERT (!bitmap_test (swap_map, slot));
  bitmap_set (swap_map, slot, true);
  if (!already_held)
    lock_release (&swap_lock);
}