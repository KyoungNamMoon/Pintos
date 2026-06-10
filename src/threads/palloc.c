#include "threads/palloc.h"
#include <bitmap.h>
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "threads/loader.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include <stdbool.h>
#include "threads/cpu.h"
#include "threads/pte.h"

/* Page allocator.  Hands out memory in page-size (or
   page-multiple) chunks.  See malloc.h for an allocator that
   hands out smaller chunks.

   System memory is divided into two "pools" called the kernel
   and user pools.  The user pool is for user (virtual) memory
   pages, the kernel pool for everything else.  The idea here is
   that the kernel needs to have memory for its own operations
   even if user processes are swapping like mad.

   Pintos's original design used a single bitmap for each pool.
   This worked ok as long as physical memory was small (64M or less).

   We now use a 2-level bitmap design here. The lower level consists
   of page_cnt/512 blocks of 512 pages each, which is one block per
   2MB of physical memory.  A 512-bit (64 byte) bitmap represents each.
   The upper level consists of a root bitmap of size 512.
   This design supports up to 1 GB of memory, or 256k pages.
 */
#define L2_PAGES    512
struct map_entry
  {
    /* space in which to allocate a struct bitmap, followed by the
     * actual bits (512 bits = 64 bytes). */
    char bitmap[64 + L2_PAGES/8];
    #define AS_BITMAP(map_entry) ((struct bitmap *)(map_entry)->bitmap)
  };

/* A memory pool. */
struct pool
  {
    struct spinlock lock;               /* Mutual exclusion. */
    struct map_entry *used_map;         /* Root bitmap of free pages.
                 The second level bitmap array follows directly at used_map+1, +2, ...  */
    uint8_t *base;                      /* Base of pool - address of first usable page. */
    uint8_t *end;                       /* End of pool - address after last usable page. */
  };

/* Two pools: one for kernel data, one for user pages. */
static struct pool kernel_pool, user_pool;

static void init_pool (struct pool *, void *base, size_t page_cnt,
                       const char *name);
static bool page_from_pool (const struct pool *, void *page);

/* Our current policy is as follows.
 * We devote a fraction of USER_PERCENT of the memory to the user pool,
 * but we also ensure that at least KERN_MINIMUM_PAGES are left for
 * the kernel.  KERN_MINIMUM_PAGES is 384, corresponding to about 1.5MB.
 * In addition, the user pages can be limited via the USER_PAGE_LIMIT
 * command line parameter.
 *
 * The default USER_PERCENT is 50%, and there is no default USER_PAGE_LIMIT.
 * Assuming Pintos is still started with 4MB of memory by default, these
 * settings should create similar defaults than before support for
 * extended memory was added.
 */
#define KERN_MINIMUM_PAGES (384)

static size_t
user_proportion_of_mem (size_t mem_size,
                        size_t user_percent,
                        size_t user_page_limit)
{
  size_t user_pages;

  size_t user_prop = mem_size * user_percent / 100;
  if (mem_size - user_prop < KERN_MINIMUM_PAGES)
    user_pages = mem_size - KERN_MINIMUM_PAGES;
  else
    user_pages = user_prop;

  if (user_pages > user_page_limit)
    user_pages = user_page_limit;

  return user_pages;
}

/* Initializes the page allocator.  */
void
palloc_init (size_t user_page_limit, size_t user_percent)
{
  /* Free memory starts at free_ram_start and runs to the end of RAM. */
  uint8_t *free_start = ptov (free_ram_start);
  uint8_t *free_end;
  uint8_t *user_start;
  size_t free_pages;
  size_t kernel_pages;

  free_end = ptov (init_ram_pages * PGSIZE);
  /* Memory above this address is used for PCI. */
  if (free_end > (uint8_t *)PCI_ADDR_ZONE_BEGIN)
    free_end = (uint8_t *)PCI_ADDR_ZONE_BEGIN;

  free_pages = (free_end - free_start) / PGSIZE;

  size_t user_pages = user_proportion_of_mem (free_pages,
                                              user_percent,
                                              user_page_limit);

  kernel_pages = free_pages - user_pages;
  user_start = free_start + kernel_pages * PGSIZE;

  init_pool (&kernel_pool, free_start, kernel_pages, "kernel pool");
  init_pool (&user_pool, user_start, user_pages, "user pool");
}

/* Obtains and returns a group of PAGE_CNT contiguous free pages.
   If PAL_USER is set, the pages are obtained from the user pool,
   otherwise from the kernel pool.  If PAL_ZERO is set in FLAGS,
   then the pages are filled with zeros.  If too few pages are
   available, returns a null pointer, unless PAL_ASSERT is set in
   FLAGS, in which case the kernel panics.

   Using a two-level design, we look through all 512-page blocks
   that have at least one page available until we find one that
   has a large enough number of contiguous pages.

   This design may suffer from external fragmentation because it will
   not allocate memory regions that straddle blocks, but we assume
   that there will be few large, multi-page allocations from the
   kernel pool (and none from the user pool), at least until Pintos
   supports huge pages or similar features in the future.
 */
void *
palloc_get_multiple (enum palloc_flags flags, size_t page_cnt)
{
  struct pool *pool = flags & PAL_USER ? &user_pool : &kernel_pool;
  void *pages = NULL;
  size_t page_idx;

  if (page_cnt == 0)
    return NULL;

  if (page_cnt > L2_PAGES)
    PANIC ("allocator does not support allocations of %d>%d pages\n",
           page_cnt, L2_PAGES);

  if (cpu_can_acquire_spinlock)
    spinlock_acquire (&pool->lock);

  struct bitmap *root_map = AS_BITMAP (pool->used_map);
  for (size_t start = 0;
       start < bitmap_size(root_map);
       start = page_idx + 1)
    {
      page_idx = bitmap_scan (root_map, start, 1, false);
      if (page_idx == BITMAP_ERROR)
        break;

      struct bitmap *l2map = AS_BITMAP (&pool->used_map[1 + page_idx]);
      size_t l2idx = bitmap_scan_and_flip (l2map, 0, page_cnt, false);
      if (l2idx != BITMAP_ERROR)
        {
          if (bitmap_all (l2map, 0, bitmap_size (l2map)))
            bitmap_set (root_map, page_idx, true);
          pages = pool->base + PGSIZE * (page_idx * L2_PAGES + l2idx);
          break;
        }
    }

  if (cpu_can_acquire_spinlock)
    spinlock_release (&pool->lock);

  if (pages != NULL) 
    {
      if (flags & PAL_ZERO)
        memset (pages, 0, PGSIZE * page_cnt);
    }
  else 
    {
      if (flags & PAL_ASSERT)
        PANIC ("palloc_get: out of pages");
    }

  return pages;
}

/* Obtains a single free page and returns its kernel virtual
   address.
   If PAL_USER is set, the page is obtained from the user pool,
   otherwise from the kernel pool.  If PAL_ZERO is set in FLAGS,
   then the page is filled with zeros.  If no pages are
   available, returns a null pointer, unless PAL_ASSERT is set in
   FLAGS, in which case the kernel panics. */
void *
palloc_get_page (enum palloc_flags flags) 
{
  return palloc_get_multiple (flags, 1);
}

/* Frees the PAGE_CNT pages starting at PAGES. */
void
palloc_free_multiple (void *pages, size_t page_cnt) 
{
  struct pool *pool;
  size_t page_idx;

  ASSERT (pg_ofs (pages) == 0);
  if (pages == NULL || page_cnt == 0)
    return;

  if (page_from_pool (&kernel_pool, pages))
    pool = &kernel_pool;
  else if (page_from_pool (&user_pool, pages))
    pool = &user_pool;
  else
    NOT_REACHED ();

#ifndef NDEBUG
  memset (pages, 0xcc, PGSIZE * page_cnt);
#endif

  page_idx = pg_no (pages) - pg_no (pool->base);
  struct bitmap *l2map = AS_BITMAP (&pool->used_map[1 + page_idx/L2_PAGES]);
  if (cpu_can_acquire_spinlock)
    spinlock_acquire (&pool->lock);

  ASSERT (bitmap_all (l2map, page_idx % L2_PAGES, page_cnt));
  bitmap_set_multiple (l2map, page_idx % L2_PAGES, page_cnt, false);
  bitmap_reset (AS_BITMAP (pool->used_map), page_idx/L2_PAGES);

  if (cpu_can_acquire_spinlock)
    spinlock_release (&pool->lock);
}

/* Frees the page at PAGE. */
void
palloc_free_page (void *page) 
{
  palloc_free_multiple (page, 1);
}

/* Initializes pool P as starting at START and comprising PAGE_CNT
   pages, naming it NAME for debugging purposes. */
static void
init_pool (struct pool *p, void *base, size_t page_cnt, const char *name) 
{
  p->end = base + page_cnt * PGSIZE;

  /* Compute the amount of memory needed for the bitmaps.  This slightly
   * overallocates space since only needs bits for the remainder left. */
  size_t bm_space = (DIV_ROUND_UP (page_cnt, L2_PAGES) + 1) * sizeof (struct map_entry);
  size_t bm_pages = DIV_ROUND_UP (bm_space, PGSIZE);
  if (bm_pages > page_cnt)
    PANIC ("Not enough memory in %s for bitmap.", name);
  page_cnt -= bm_pages;

  spinlock_init (&p->lock);
  size_t MAP_ENTRY_SIZE = sizeof (p->used_map->bitmap);
  p->used_map = (struct map_entry *)base;   // root-level map entry
  size_t blocks = DIV_ROUND_UP (page_cnt, 512);
  bitmap_create_in_buf(blocks, p->used_map->bitmap, MAP_ENTRY_SIZE);
  if (blocks > 512)
    PANIC ("Maximum supported size is 1GB for %s.", name);

  struct map_entry *l2map = p->used_map + 1;  // next level map

  /* Create page_cnt/512 full 512-bit bitmaps and one for the remainder. */
  for (size_t i = 0; i < page_cnt/512; i++)
    bitmap_create_in_buf (512, l2map + i, MAP_ENTRY_SIZE);

  if (page_cnt % 512 != 0)
    bitmap_create_in_buf (page_cnt % 512, l2map + page_cnt/512, MAP_ENTRY_SIZE);

  p->base = (void *) ROUND_UP((uintptr_t) &l2map[DIV_ROUND_UP(page_cnt, 512)],
                              PGSIZE);
}

/* Returns true if PAGE was allocated from POOL,
   false otherwise. */
static bool
page_from_pool (const struct pool *pool, void *page) 
{
  size_t page_no = pg_no (page);
  size_t start_page = pg_no (pool->base);
  size_t end_page = pg_no (pool->end);

  return page_no >= start_page && page_no < end_page;
}
