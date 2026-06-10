#include "vm/page.h"
#include "vm/swap.h"
#include "vm/frame.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "userprog/pagedir.h"

/* Initializes the supplemental page table for a new process. */
void spt_init (struct list *spt) {
    list_init (spt);
}

/* Inserts a new SPTE into the supplemental page table. 
   Returns true on success. */
bool spt_insert (struct list *spt, struct sup_page_table_entry *spte) {
    if (spte == NULL) return false;
    list_push_back (spt, &spte->elem);
    return true;
}

/* Finds an SPTE in the table that contains the given faulting address. 
   Returns NULL if not found. */
struct sup_page_table_entry *spt_find (struct list *spt, void *fault_addr) {
    /* Round down the faulting address to the nearest page boundary. */
    void *page_addr = pg_round_down (fault_addr);
    struct list_elem *e;

    /* Iterate through the list to find the matching entry. */
    for (e = list_begin (spt); e != list_end (spt); e = list_next (e)) {
        struct sup_page_table_entry *spte = list_entry (e, struct sup_page_table_entry, elem);
        if (spte->upage == page_addr) {
            return spte;
        }
    }
    return NULL;
}

/* Frees all resources associated with the supplemental page table.
   Releases any swap slots and physical frames still held by the 
   process, then frees each SPTE.  Called when the process exits. */
void spt_destroy (struct list *spt) {
    struct thread *t = thread_current ();
    uint32_t *pd = t->pagedir;
    struct list_elem *e = list_begin (spt);
    while (e != list_end (spt)) {
        struct sup_page_table_entry *spte = list_entry (e, struct sup_page_table_entry, elem);
        e = list_remove (e);
        /* swap_in() already frees the slot when bringing the page back.
           Only free the slot if the page is currently evicted (not loaded). */
        if (spte->type == VM_SWAP && !spte->is_loaded)
            swap_free (spte->swap_slot);
        if (spte->is_loaded && spte->kpage != NULL)
          {
            /* process_exit() later calls pagedir_destroy(), which frees
               any user pages still mapped in the hardware page table.
               Clear the PTE first to avoid double-free. */
            if (pd != NULL)
              pagedir_clear_page (pd, spte->upage);
            frame_free (spte->kpage);
          }
        free (spte);
    }
}

/* Copies the supplemental page table from a parent to a child. 
   Used for process_fork(). Returns true on success, false ifs
   memory allocation fails. */
bool spt_copy (struct list *dst_spt, struct list *src_spt) {
    struct list_elem *e;
    
    for (e = list_begin (src_spt); e != list_end (src_spt); e = list_next (e)) {
        struct sup_page_table_entry *src_spte = list_entry (e, struct sup_page_table_entry, elem);
        
        /* Allocate a new entry for the destination process. */
        struct sup_page_table_entry *dst_spte = malloc (sizeof (struct sup_page_table_entry));
        if (dst_spte == NULL) return false;

        /* Copy all metadata from the source entry. */
        dst_spte->upage = src_spte->upage;
        dst_spte->kpage = src_spte->kpage;
        dst_spte->writable = src_spte->writable;
        dst_spte->is_loaded = src_spte->is_loaded;
        dst_spte->type = src_spte->type;
        dst_spte->file = src_spte->file;
        dst_spte->offset = src_spte->offset;
        dst_spte->read_bytes = src_spte->read_bytes;
        dst_spte->zero_bytes = src_spte->zero_bytes;
        dst_spte->is_mmap = src_spte->is_mmap;

        /* Insert the copied entry into the destination SPT. */
        if (!spt_insert (dst_spt, dst_spte)) {
            free (dst_spte);
            return false;
        }
    }
    return true;
}