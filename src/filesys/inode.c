#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "filesys/cache.h"
#include "threads/thread.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* Number of direct pointers in inode */
#define DIRECT_BLOCKS 121

/* Number of pointers per indirect block */
#define PTRS_PER_BLOCK (BLOCK_SECTOR_SIZE / sizeof(block_sector_t))

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    off_t length;                          /* File size in bytes. */
    unsigned magic;                        /* Magic number. */
    bool is_dir;                           /* True if directory, false if file. */
    uint8_t padding[1];                    /* Alignment padding. */
    uint16_t mode;                         /* Unix permission bits e.g. 0644. */
    uint32_t owner_uid;                    /* UID of the file's creator. */
    uint32_t owner_gid;                    /* GID of the file's creator. */
    block_sector_t direct[DIRECT_BLOCKS];  /* Direct block pointers. */
    block_sector_t indirect;              /* Indirect block pointer. */
    block_sector_t doubly_indirect;       /* Doubly indirect block pointer. */
  };

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /* Inode content. */
  };

/* Allocates a new data block and zeros it.
   Returns the sector number, or 0 on failure (0 is never returned for
   a successful data allocation — FREE_MAP_SECTOR is reserved). */
static block_sector_t
allocate_block (void)
{
  block_sector_t sector = 0;
  if (!free_map_allocate (1, &sector))
    return 0;

  /* Zero out the new block */
  static char zeros[BLOCK_SECTOR_SIZE];
  buffer_cache_write (sector, zeros);

  return sector;
}

/* Frees a single block at SECTOR. */
static void
free_block (block_sector_t sector)
{
  if (sector != 0)
    free_map_release (sector, 1);
}

/* Frees an indirect block and all blocks it points to. */
static void
free_indirect_block (block_sector_t indirect_sector)
{
  if (indirect_sector == 0)
    return;
  
  block_sector_t indirect_block[PTRS_PER_BLOCK];
  buffer_cache_read (indirect_sector, indirect_block);
  
  /* Free all data blocks pointed to by this indirect block */
  for (int i = 0; i < PTRS_PER_BLOCK; i++)
    {
      if (indirect_block[i] != 0)
        free_block (indirect_block[i]);
    }
  
  /* Free the indirect block itself */
  free_block (indirect_sector);
}

/* Frees a doubly indirect block and all blocks it points to. */
static void
free_doubly_indirect_block (block_sector_t doubly_sector)
{
  if (doubly_sector == 0)
    return;
  
  block_sector_t doubly_block[PTRS_PER_BLOCK];
  buffer_cache_read (doubly_sector, doubly_block);
  
  /* Free all indirect blocks */
  for (int i = 0; i < PTRS_PER_BLOCK; i++)
    {
      if (doubly_block[i] != 0)
        free_indirect_block (doubly_block[i]);
    }
  
  /* Free the doubly indirect block itself */
  free_block (doubly_sector);
}

/* Returns the block device sector that contains byte offset POS
   within INODE, allocating new blocks if ALLOCATE is true.
   Returns -1 if INODE does not contain data for a byte at offset
   POS and ALLOCATE is false. */
static block_sector_t
byte_to_sector (struct inode *inode, off_t pos, bool allocate) 
{
  ASSERT (inode != NULL);
  
  if (pos >= inode->data.length && !allocate)
    return -1;
  
  off_t block_idx = pos / BLOCK_SECTOR_SIZE;
  
  /* Direct blocks */
  if (block_idx < DIRECT_BLOCKS)
    {
      if (inode->data.direct[block_idx] == 0 && allocate)
        {
          block_sector_t s = allocate_block ();
          if (s == 0)
            return -1;
          inode->data.direct[block_idx] = s;
          buffer_cache_write (inode->sector, &inode->data);
        }
      return inode->data.direct[block_idx];
    }
  
  block_idx -= DIRECT_BLOCKS;
  
  /* Indirect block.  Do not use large on-stack sector arrays: two
     PTRS_PER_BLOCK tables exceed the kernel stack (one page). */
  if (block_idx < PTRS_PER_BLOCK)
    {
      /* Allocate indirect block if needed */
      if (inode->data.indirect == 0 && allocate)
        {
          block_sector_t s = allocate_block ();
          if (s == 0)
            return -1;
          inode->data.indirect = s;
          buffer_cache_write (inode->sector, &inode->data);
        }
      
      if (inode->data.indirect == 0)
        return -1;
      
      block_sector_t *indirect_block = malloc (BLOCK_SECTOR_SIZE);
      if (indirect_block == NULL)
        return -1;

      buffer_cache_read (inode->data.indirect, indirect_block);
      
      /* Allocate data block if needed */
      if (indirect_block[block_idx] == 0 && allocate)
        {
          block_sector_t s = allocate_block ();
          if (s == 0)
            {
              free (indirect_block);
              return -1;
            }
          indirect_block[block_idx] = s;
          buffer_cache_write (inode->data.indirect, indirect_block);
        }
      
      block_sector_t ret = indirect_block[block_idx];
      free (indirect_block);
      return ret;
    }
  
  block_idx -= PTRS_PER_BLOCK;
  
  /* Doubly indirect block */
  if (block_idx < PTRS_PER_BLOCK * PTRS_PER_BLOCK)
    {
      block_sector_t *buf = malloc (BLOCK_SECTOR_SIZE);
      if (buf == NULL)
        return -1;

      /* Allocate doubly indirect block if needed */
      if (inode->data.doubly_indirect == 0 && allocate)
        {
          block_sector_t s = allocate_block ();
          if (s == 0)
            {
              free (buf);
              return -1;
            }
          inode->data.doubly_indirect = s;
          buffer_cache_write (inode->sector, &inode->data);
        }
      
      if (inode->data.doubly_indirect == 0)
        {
          free (buf);
          return -1;
        }
      
      buffer_cache_read (inode->data.doubly_indirect, buf);
      
      size_t indirect_idx = block_idx / PTRS_PER_BLOCK;
      size_t data_idx = block_idx % PTRS_PER_BLOCK;
      
      /* Allocate indirect block if needed */
      if (buf[indirect_idx] == 0 && allocate)
        {
          block_sector_t s = allocate_block ();
          if (s == 0)
            {
              free (buf);
              return -1;
            }
          buf[indirect_idx] = s;
          buffer_cache_write (inode->data.doubly_indirect, buf);
        }
      
      if (buf[indirect_idx] == 0)
        {
          free (buf);
          return -1;
        }
      
      block_sector_t indirect_sec = buf[indirect_idx];
      buffer_cache_read (indirect_sec, buf);
      
      /* Allocate data block if needed */
      if (buf[data_idx] == 0 && allocate)
        {
          block_sector_t s = allocate_block ();
          if (s == 0)
            {
              free (buf);
              return -1;
            }
          buf[data_idx] = s;
          buffer_cache_write (indirect_sec, buf);
        }
      
      block_sector_t ret = buf[data_idx];
      free (buf);
      return ret;
    }
  
  /* Beyond maximum file size */
  return -1;
}

/* List of open inodes, so that opening a single inode twice
   returns the same inode. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  ASSERT (sizeof (struct inode_disk) == BLOCK_SECTOR_SIZE);
  list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      disk_inode->is_dir = false;
      /* Set owner to the current thread's UID and default permissions.
         Directories get 755, regular files get 644. */
      disk_inode->owner_uid = thread_current ()->euid;
      disk_inode->owner_gid = thread_current ()->egid;
      disk_inode->mode = 0644;
      
      /* Initialize all block pointers to 0 */
      for (int i = 0; i < DIRECT_BLOCKS; i++)
        disk_inode->direct[i] = 0;
      disk_inode->indirect = 0;
      disk_inode->doubly_indirect = 0;
      
      /* Write inode to disk */
      buffer_cache_write (sector, disk_inode);
      success = true;
      
      /* If creating with nonzero length, allocate blocks now.
         This is needed for directory creation. */
      if (length > 0)
        {
          struct inode *inode = inode_open (sector);
          if (inode == NULL)
            {
              success = false;
            }
          else
            {
              /* Allocate all blocks by writing zeros */
              static char zeros[BLOCK_SECTOR_SIZE];
              off_t offset;
              for (offset = 0; offset < length; offset += BLOCK_SECTOR_SIZE)
                {
                  size_t write_size = length - offset < BLOCK_SECTOR_SIZE ? 
                                      length - offset : BLOCK_SECTOR_SIZE;
                  if (inode_write_at (inode, zeros, write_size, offset) != write_size)
                    {
                      success = false;
                      break;
                    }
                }
              inode_close (inode);
            }
        }
      
      free (disk_inode);
    }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  buffer_cache_read(inode->sector, &inode->data);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          /* Free inode sector */
          free_map_release (inode->sector, 1);
          
          /* Free all direct blocks */
          for (int i = 0; i < DIRECT_BLOCKS; i++)
            {
              if (inode->data.direct[i] != 0)
                free_block (inode->data.direct[i]);
            }
          
          /* Free indirect block and all blocks it points to */
          if (inode->data.indirect != 0)
            free_indirect_block (inode->data.indirect);
          
          /* Free doubly indirect block and all blocks it points to */
          if (inode->data.doubly_indirect != 0)
            free_doubly_indirect_block (inode->data.doubly_indirect);
        }

      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Handles sparse files by returning zero bytes for any unallocated
   regions within the file's length.  Returns the number of bytes
   actually read, which may be less than SIZE if end of file is
   reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;

  while (size > 0) 
    {
      /* Look up the disk sector containing OFFSET.  Pass false so
         no new blocks are allocated for reads. */
      block_sector_t sector_idx = byte_to_sector (inode, offset, false);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Determine how many bytes are available in this sector and
         in the file, then take the smaller of the two limits. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_idx == (block_sector_t) -1 || sector_idx == 0)
        {
          /* Sector is unallocated — this is a sparse region of the
             file.  Return zeros rather than reading from disk. */
          memset (buffer + bytes_read, 0, chunk_size);
        }
      else if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Aligned full-sector read — copy directly into caller's
             buffer via the cache, avoiding a bounce buffer. */
          buffer_cache_read (sector_idx, buffer + bytes_read);
        }
      else 
        {
          /* Partial sector read — read the full sector into a bounce
             buffer and copy out only the requested bytes. */
          uint8_t bounce[BLOCK_SECTOR_SIZE];
          buffer_cache_read (sector_idx, bounce);
          memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
        }
      
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
    
  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if an error occurs.
   Extends the file if writing past EOF. */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
 
  if (inode->deny_write_cnt)
    return 0;

  /* Extend file if writing past EOF */
  if (offset + size > inode->data.length)
    {
      /* Update file length */
      inode->data.length = offset + size;
      buffer_cache_write (inode->sector, &inode->data);
    }

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset, true);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in sector */
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < sector_left ? size : sector_left;
      if (chunk_size <= 0)
        break;
      
      if (sector_idx == (block_sector_t) -1)
        {
          /* Allocation failed */
          break;
        }

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Write full sector directly to disk. */
          buffer_cache_write(sector_idx, buffer + bytes_written);
        }
      else 
        {
          uint8_t bounce[BLOCK_SECTOR_SIZE];

          /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */
          if (sector_ofs > 0 || chunk_size < sector_left) 
            buffer_cache_read (sector_idx, bounce);
          else
            memset (bounce, 0, BLOCK_SECTOR_SIZE);
          memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
          buffer_cache_write(sector_idx, bounce);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->data.length;
}

/* Returns true if inode represents a directory. */
bool
inode_is_dir (const struct inode *inode)
{
  return inode->data.is_dir;
}

/* Sets whether inode represents a directory. */
void
inode_set_dir (struct inode *inode, bool is_dir)
{
  inode->data.is_dir = is_dir;
  /* Directories get 0755 by default — owner full access,
     others can read and execute (traverse). */
  if (is_dir && inode->data.mode == 0644)
    inode->data.mode = 0755;
  buffer_cache_write (inode->sector, &inode->data);
}

bool
inode_is_removed (const struct inode *inode)
{
  return inode->removed;
}

/* Returns true if the current process is allowed to perform the
   requested access on INODE.  REQUESTED is one of:
     4 = read, 2 = write, 1 = execute.
   Root (uid 0) is always allowed.  Otherwise checks owner bits
   if the caller owns the file, or other bits if they do not. */
bool
inode_check_permission (const struct inode *inode, int requested)
{
  uid_t caller = thread_current ()->euid;
  uid_t caller_gid = thread_current ()->egid;

  /* Root bypasses all permission checks. */
  if (caller == ROOT_UID)
    return true;

  uint16_t mode = inode->data.mode;
  uint32_t owner = inode->data.owner_uid;
  uint32_t group = inode->data.owner_gid;

  if (caller == (uid_t) owner)
    return ((mode >> 6) & requested) == (unsigned) requested;
  else if (caller_gid == (uid_t) group)
    return ((mode >> 3) & requested) == (unsigned) requested;
  else
    return ((mode >> 0) & requested) == (unsigned) requested;
}

/* Returns the owner UID of the inode. */
uint32_t
inode_get_owner (const struct inode *inode)
{
  return inode->data.owner_uid;
}

uint32_t
inode_get_group (const struct inode *inode)
{
  return inode->data.owner_gid;
}

/* Returns the permission mode bits of the inode. */
uint16_t
inode_get_mode (const struct inode *inode)
{
  return inode->data.mode;
}

/* Sets the permission mode bits and writes the inode back to disk.
   Only the owner or root may change permissions. Returns true on
   success, false if the caller lacks permission. */
bool
inode_set_mode (struct inode *inode, uint16_t mode)
{
  uid_t caller = thread_current ()->euid;
  if (caller != ROOT_UID && caller != (uid_t) inode->data.owner_uid)
    return false;
  inode->data.mode = mode;
  buffer_cache_write (inode->sector, &inode->data);
  return true;
}

/* Sets the owner uid and gid of the inode. Only root can do this. */
bool
inode_set_owner (struct inode *inode, uint32_t uid, uint32_t gid)
{
  if (thread_current ()->euid != ROOT_UID)
    return false;
  inode->data.owner_uid = uid;
  inode->data.owner_gid = gid;
  buffer_cache_write (inode->sector, &inode->data);
  return true;
}

