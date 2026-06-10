#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include "threads/palloc.h"
#include "vm/page.h"

void  frame_init  (void);                                                       /* Initialize frame table */
void *frame_alloc (enum palloc_flags flags, struct sup_page_table_entry *spte); /* Allocate frame, evict if needed */
void  frame_free  (void *kpage);                                                /* Free frame and return to pool */
void  frame_pin   (void *kpage);                                                /* Pin frame to prevent eviction */
void  frame_unpin (void *kpage);                                                /* Unpin frame after I/O complete */

#endif /* vm/frame.h */ 