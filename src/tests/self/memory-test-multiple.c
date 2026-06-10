/*
 * Test memory (allocations of multiple pages at a time)
 */
#include "tests.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "list.h"

struct page {
    void *ptr;
    struct list_elem elem;
    int npages;
    uint32_t number;
};

static void
allocate_from_pool (struct list *pages, int npages, int flags, const char *pool)
{
  uint32_t pagecount = 0;
  for (int number = 1;; number++) {
    struct page *page = malloc(sizeof *page);
    if (page == NULL)
      break; 
    page->number = number;
    page->npages = npages;
    page->ptr = palloc_get_multiple(flags, npages);
    if (page->ptr == NULL)
      break;
    *(uint32_t *) page->ptr = number;
    pagecount += page->npages;
    list_push_front(pages, &page->elem);
  }
  msg ("allocated %d pages from %s pool %d at a time.", pagecount, pool, npages);
}

static void
free_allocated_pages(struct list *pages)
{
  while (!list_empty(pages))
    {
      struct page *page = list_entry(list_pop_front(pages), struct page, elem); 
      ASSERT (*(uint32_t *)page->ptr == page->number);
      palloc_free_multiple (page->ptr, page->npages);
      free (page);
    } 
}

void
test_memory_multiple (void)
{
  struct list pages;
  list_init(&pages);
  for (int i = 2; i <= 512; i *= 2)
    {
      allocate_from_pool(&pages, i, 0, "kernel");
      free_allocated_pages(&pages);
      allocate_from_pool(&pages, i, PAL_USER, "user");
      free_allocated_pages(&pages);
    }
  pass ();
}
