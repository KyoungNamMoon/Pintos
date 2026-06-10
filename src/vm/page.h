#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lib/kernel/list.h"


/* Type of the supplemental page to track its data source. */
enum page_type {
    VM_BIN,    /* Binary executable file (Lazy Loading) */
    VM_FILE,   /* Memory-mapped file (mmap) */
    VM_SWAP,   /* Page stored in the swap partition */
    VM_ANON    /* Anonymous page (Stack, etc.) */
};

/* Supplemental Page Table Entry (SPTE). 
   Each entry represents a virtual page in the user process. */
struct sup_page_table_entry {
    void *upage;              /* User virtual address (The Key) */
    void *kpage;              /* Associated kernel virtual address (Frame) */
    bool writable;            /* True if the page is writable */
    bool is_loaded;           /* True if the page is currently in physical memory */
    
    enum page_type type;      /* Source of the page data */
    struct file *file;        /* File pointer if it's a file-backed page */
    size_t offset;            /* Offset in the file */
    size_t read_bytes;        /* Number of bytes to read from the file */
    size_t zero_bytes;        /* Number of bytes to zero-fill */
    bool is_mmap;             /* True if VM_FILE page is from mmap (not exec) */

    struct list_elem elem;    /* List element for thread's SPT list */
    /* When type == VM_SWAP: on-disk slot index while the page is only on swap;
       SPTE_SWAP_SLOT_INVALID after swap_in() (slot freed — data is only in RAM). */
    size_t swap_slot;
};

#define SPTE_SWAP_SLOT_INVALID ((size_t) -1)

/* Supplemental Page Table Management APIs */
void spt_init (struct list *spt);                                               /* Initialize SPT for a new process */
bool spt_insert (struct list *spt, struct sup_page_table_entry *spte);          /* Add a page entry to the SPT */
struct sup_page_table_entry *spt_find (struct list *spt, void *fault_addr);     /* Find SPT entry containing fault_addr */
void spt_destroy (struct list *spt);                                            /* Free all SPT entries and their resources */
bool spt_copy (struct list *dst_spt, struct list *src_spt);                     /* Copies SPT from parent to child for fork */

#endif /* vm/page.h */