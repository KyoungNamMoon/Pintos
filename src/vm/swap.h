#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <stddef.h>

void   swap_init (void);                     /* Initialize swap partition */
size_t swap_out  (void *kpage);              /* Write page to swap, return slot index */
void   swap_in   (size_t slot, void *kpage); /* Read page from swap and free slot */
void   swap_free (size_t slot);              /* Free swap slot without reading */

#endif /* vm/swap.h */