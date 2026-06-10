/* Memory-mapped files (mmap/munmap) per Pintos VM spec and design doc. */

#include "vm/mmap.h"
#include "vm/page.h"
#include "vm/frame.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "debug.h"
#include "userprog/pagedir.h"
#include "filesys/file.h"
#include "threads/synch.h"
#include "lib/kernel/hash.h"
#include <string.h>

extern struct lock filesys_lock;

/* How many bytes at PAGE_OFF are backed by the file's current length.
   Avoids extending the file to a full page when write-back only needs
   the tail (e.g. mmap-exit).  If the page starts at/after EOF, allow a
   full page write so mmap can still extend the file. */
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

/* Per-process mmap state; allocated separately from struct thread
   to avoid consuming precious kernel stack space. */
struct process_mmap
{
  struct hash by_mapid;  /* Hash table of mmap_region, keyed by mapid */
  int next_mapid;        /* Next mapid to assign (increments from 1) */
};

/* One mmap region; represents a single mmap() call.
   Hash key is mapid (design doc: mmap_descriptor). */
struct mmap_region
{
  int mapid;             /* Unique identifier for this mapping */
  struct file *file;     /* Reopened file for this mapping */
  void *vaddr;           /* Starting virtual address of mapping */
  size_t length;         /* Length of file in bytes */
  struct hash_elem elem; /* Hash table element */
};

/* Hash function for mmap regions (keyed by mapid). */
unsigned
mmap_hash_func (const struct hash_elem *e, void *aux UNUSED)
{
  const struct mmap_region *m = hash_entry (e, struct mmap_region, elem);
  return hash_int (m->mapid);
}

/* Comparison function for mmap regions (orders by mapid). */
bool
mmap_less_func (const struct hash_elem *a, const struct hash_elem *b,
                void *aux UNUSED)
{
  const struct mmap_region *ma = hash_entry (a, struct mmap_region, elem);
  const struct mmap_region *mb = hash_entry (b, struct mmap_region, elem);
  return ma->mapid < mb->mapid;
}

/* Finds the mmap region with the given mapid, or NULL if not found. */
static struct mmap_region *
find_region (struct thread *t, int mapid)
{
  struct mmap_region key;
  memset (&key, 0, sizeof key);
  key.mapid = mapid;
  if (t->mmap == NULL)
    return NULL;
  struct hash_elem *e = hash_find (&t->mmap->by_mapid, &key.elem);
  return e == NULL ? NULL : hash_entry (e, struct mmap_region, elem);
}

/* Removes an SPT entry from the list and frees it.
   Does not free the associated frame - caller must do that. */
static void
spt_remove_entry (struct sup_page_table_entry *spte)
{
  list_remove (&spte->elem);
  free (spte);
}

/* Writes back a loaded mmap page only if the *user* mapping was written. */
static void
write_mmap_page_back (struct thread *t, struct sup_page_table_entry *spte)
{
  /* Skip if page not in memory */
  if (!spte->is_loaded || spte->kpage == NULL)
    return;
  
  /* Read-only mappings never need writeback */
  if (!spte->writable)
    return;
  
  /* Check if user modified the page */
  if (!pagedir_is_dirty (t->pagedir, spte->upage))
    return;
  
  /* Write only file-backed bytes so we do not grow the file with
     zero-filled mmap tail (mmap-exit). */
  size_t nbytes = mmap_writeback_size (spte->file, (off_t) spte->offset);
  if (nbytes > 0)
    file_write_at (spte->file, spte->kpage, nbytes, (off_t) spte->offset);
  
  /* Clear dirty bit so we don't write back again */
  pagedir_set_dirty (t->pagedir, spte->upage, false);
}

/* Unmaps an mmap region, writing back dirty pages and freeing resources.
   For each page in the region, it writes back to file if user modified, 
   frees the physical frame if loaded, clears the hardware page table entry, 
   and removes/frees the SPT entry. Finally closes the file and removes the 
   region from the hash table. */
static void
unmap_region (struct thread *t, struct mmap_region *mr)
{
  uint32_t *pd = t->pagedir;
  void *p;
  size_t off;

  /* Iterate over all pages in the mapped region */
  for (off = 0; off < mr->length; off += PGSIZE)
    {
      p = (uint8_t *) mr->vaddr + off;
      struct sup_page_table_entry *spte = spt_find (&t->spt, p);
      if (spte == NULL)
        continue;

      /* Write back dirty page to file */
      bool need_unlock = false;
      if (!lock_held_by_current_thread (&filesys_lock))
        {
          lock_acquire (&filesys_lock);
          need_unlock = true;
        }
      write_mmap_page_back (t, spte);
      if (need_unlock)
        lock_release (&filesys_lock);

      /* Free frame if page is loaded */
      if (spte->is_loaded && spte->kpage != NULL)
        {
          pagedir_clear_page (pd, spte->upage);
          frame_free (spte->kpage);
        }
      /* Clear any stale hardware mapping */
      else if (pagedir_get_page (pd, spte->upage) != NULL)
        pagedir_clear_page (pd, spte->upage);

      /* Remove SPT entry */
      spt_remove_entry (spte);
    }

  /* Close the reopened file and free the region */
  bool need_unlock = false;
  if (!lock_held_by_current_thread (&filesys_lock))
    {
      lock_acquire (&filesys_lock);
      need_unlock = true;
    }
  file_close (mr->file);
  hash_delete (&t->mmap->by_mapid, &mr->elem);
  if (need_unlock)
    lock_release (&filesys_lock);
  free (mr);
}

/* Unmaps the region identified by mapid.
   Does nothing if mapid is invalid. */
void
munmap_syscall (int mapid)
{
  struct thread *t = thread_current ();
  struct mmap_region *mr = find_region (t, mapid);
  if (mr == NULL)
    return;
  unmap_region (t, mr);
}

/* Unmaps all mmap regions for this process.
   Called on process exit to clean up resources and write back dirty pages. */
void
mmap_destroy_all (struct thread *t)
{
  if (t->mmap == NULL)
    return;
  
  /* Unmap all regions one by one */
  while (hash_size (&t->mmap->by_mapid) > 0)
    {
      struct hash_iterator it;
      hash_first (&it, &t->mmap->by_mapid);
      ASSERT (hash_next (&it));
      struct mmap_region *mr =
          hash_entry (hash_cur (&it), struct mmap_region, elem);
      unmap_region (t, mr);
    }
  
  /* Free the process mmap state */
  hash_destroy (&t->mmap->by_mapid, NULL);
  free (t->mmap);
  t->mmap = NULL;
}

/* Initializes mmap data structures for a new process.
   Must be called during process creation before any mmap calls. */
void
mmap_process_init (struct thread *t)
{
  ASSERT (t->mmap == NULL);
  struct process_mmap *pm = calloc (1, sizeof *pm);
  if (pm == NULL || !hash_init (&pm->by_mapid, mmap_hash_func, mmap_less_func,
                                 NULL))
    PANIC ("mmap_process_init");
  pm->next_mapid = 1;  /* mapids start at 1 (0 reserved for error) */
  t->mmap = pm;
}

/* Gets file from file descriptor table. Returns NULL if fd invalid. */
static struct file *
process_get_file (int fd)
{
  struct thread *t = thread_current ();
  if (fd < 2 || fd >= 128)
    return NULL;
  return t->fd_table[fd];
}

/* Maps file at fd into memory at addr, returning mapid or -1 on error.
   In order to be validated, addr must be page-aligned and not null, 
   the file must have nonzero length, and the entire range must be unmapped.
   On success, it reopens the file, creates lazy-loaded SPT entries for each 
   page, and registers the mmap_region. On failure, it rolls back any SPT 
   entries created, closes reopened file, and returns -1.*/
int
mmap_syscall (int fd, void *addr)
{
  struct thread *t = thread_current ();
  uint32_t *pd = t->pagedir;
  bool need_unlock = false;

  /* Validate mmap system is initialized */
  if (t->mmap == NULL || pd == NULL)
    return -1;

  /* Validate addr is page-aligned and non-NULL */
  if (addr == NULL || pg_ofs (addr) != 0)
    return -1;

  /* Get the file from fd table */
  struct file *f = process_get_file (fd);
  if (f == NULL)
    return -1;

  /* Get file length and validate it's non-zero */
  if (!lock_held_by_current_thread (&filesys_lock))
    {
      lock_acquire (&filesys_lock);
      need_unlock = true;
    }
  size_t length = (size_t) file_length (f);
  if (length == 0)
    {
      if (need_unlock)
        lock_release (&filesys_lock);
      return -1;
    }

  /* Check if file allows writable mapping */
  bool writable = file_may_mmap_writable (f);

  /* Reopen file so mapping persists even if fd is closed (per spec) */
  struct file *reopen = file_reopen (f);
  if (reopen == NULL)
    {
      if (need_unlock)
        lock_release (&filesys_lock);
      return -1;
    }

  /* Calculate number of pages needed */
  size_t n_pages = (length + PGSIZE - 1) / PGSIZE;
  size_t i;

  /* Validate all pages in range are unmapped before proceeding.
     This prevents partial mappings if we discover overlap partway through. */
  for (i = 0; i < n_pages; i++)
    {
      void *upage = (uint8_t *) addr + i * PGSIZE;
      
      /* Check page is in user address space */
      if (!is_user_vaddr (upage) || !is_user_vaddr ((uint8_t *) upage + PGSIZE - 1))
        {
          file_close (reopen);
          if (need_unlock)
            lock_release (&filesys_lock);
          return -1;
        }
      
      /* Check page not already mapped in hardware page table */
      if (pagedir_get_page (pd, upage) != NULL)
        {
          file_close (reopen);
          if (need_unlock)
            lock_release (&filesys_lock);
          return -1;
        }
      
      /* Check page not reserved in SPT (stack growth, lazy load, etc.) */
      if (spt_find (&t->spt, upage) != NULL)
        {
          file_close (reopen);
          if (need_unlock)
            lock_release (&filesys_lock);
          return -1;
        }
    }

  /* Create SPT entries for lazy loading on page fault */
  for (i = 0; i < n_pages; i++)
    {
      void *upage = (uint8_t *) addr + i * PGSIZE;
      size_t file_off = i * PGSIZE;
      
      /* Calculate bytes to read from file vs. zero-fill */
      size_t read_b = length - file_off;
      if (read_b > PGSIZE)
        read_b = PGSIZE;
      size_t zero_b = PGSIZE - read_b;

      /* Allocate SPT entry */
      struct sup_page_table_entry *spte =
          malloc (sizeof (struct sup_page_table_entry));
      if (spte == NULL)
        {
          /* Out of memory - roll back all SPT entries created so far */
          size_t j;
          for (j = 0; j < i; j++)
            {
              void *u = (uint8_t *) addr + j * PGSIZE;
              struct sup_page_table_entry *sp = spt_find (&t->spt, u);
              if (sp != NULL)
                spt_remove_entry (sp);
            }
          file_close (reopen);
          if (need_unlock)
            lock_release (&filesys_lock);
          return -1;
        }

      /* Initialize SPT entry for lazy-loaded file page */
      memset (spte, 0, sizeof *spte);
      spte->upage = upage;
      spte->type = VM_FILE;
      spte->is_mmap = true;       /* Distinguish from executable pages */
      spte->file = reopen;        /* All pages share the reopened file */
      spte->offset = file_off;
      spte->read_bytes = read_b;
      spte->zero_bytes = zero_b;
      spte->writable = writable;
      spte->is_loaded = false;    /* Will be loaded on first page fault */
      spte->kpage = NULL;
      spt_insert (&t->spt, spte);
    }

  /* Allocate and register the mmap region */
  struct mmap_region *mr = malloc (sizeof (struct mmap_region));
  if (mr == NULL)
    {
      /* Out of memory - roll back all SPT entries */
      for (i = 0; i < n_pages; i++)
        {
          void *u = (uint8_t *) addr + i * PGSIZE;
          struct sup_page_table_entry *sp = spt_find (&t->spt, u);
          if (sp != NULL)
            spt_remove_entry (sp);
        }
      file_close (reopen);
      if (need_unlock)
        lock_release (&filesys_lock);
      return -1;
    }

  /* Initialize and register the region */
  mr->mapid = t->mmap->next_mapid++;
  mr->file = reopen;
  mr->vaddr = addr;
  mr->length = length;
  hash_insert (&t->mmap->by_mapid, &mr->elem);

  if (need_unlock)
    lock_release (&filesys_lock);
  return mr->mapid;
}