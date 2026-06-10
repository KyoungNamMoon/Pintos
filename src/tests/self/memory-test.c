/*
 * Test memory
 */
#include "tests.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "list.h"

struct page {
    void *ptr;
    struct list_elem elem;
    uint32_t number;
};

static void
allocate_from_pool (struct list *pages, int flags, const char *pool)
{
  uint32_t pagecount = 0;
  for (int number = 1;; number++) {
    struct page *page = malloc(sizeof *page);
    if (page == NULL)
      break; 
    page->number = number;
    page->ptr = palloc_get_page(flags);
    if (page->ptr == NULL)
      break;
    *(uint32_t *) page->ptr = number;
    pagecount++;
    list_push_front(pages, &page->elem);
  }
  msg ("allocated %d pages from %s pool.", pagecount, pool);
}

static void
free_allocated_pages(struct list *pages)
{
  while (!list_empty(pages))
    {
      struct page *page = list_entry(list_pop_front(pages), struct page, elem); 
      ASSERT (*(uint32_t *)page->ptr == page->number);
      palloc_free_page (page->ptr);
      free (page);
    } 
}

void
test_memory (void)
{
  struct list pages;
  list_init(&pages);
  for (int i = 0; i < 3; i++)
    {
      allocate_from_pool(&pages, 0, "kernel");
      free_allocated_pages(&pages);
      allocate_from_pool(&pages, PAL_USER, "user");
      free_allocated_pages(&pages);
    }
  pass ();
}
