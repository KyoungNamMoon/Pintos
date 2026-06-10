#ifndef VM_MMAP_H
#define VM_MMAP_H

#include <stdbool.h>
#include "lib/kernel/hash.h"

struct thread;

unsigned mmap_hash_func (const struct hash_elem *e, void *aux); /* Hash mmap table entry */
bool mmap_less_func (const struct hash_elem *a, const struct hash_elem *b,
                     void *aux);                                /* Compare mmap entries */

void mmap_process_init (struct thread *t);      /* Initialize mmap state for thread */
int  mmap_syscall (int fd, void *addr);         /* Map file fd at addr */
void munmap_syscall (int mapid);                /* Unmap mapping by mapid */
void mmap_destroy_all (struct thread *t);       /* Unmap and clean all mappings for thread */

#endif /* VM_MMAP_H */