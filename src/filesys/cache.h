#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "devices/block.h"
#include "threads/synch.h"

/* The maximum number of cache entries (64 blocks) */
#define BUFFER_CACHE_SIZE 64

/* Buffer cache entry structure */
struct cache_entry {
    bool valid;                      /* True if this entry contains valid disk data */
    bool dirty;                      /* True if the data was modified and needs to be written to disk */
    bool accessed;                   /* Used for the clock replacement algorithm (second-chance) */
    block_sector_t sector;           /* The disk sector number currently held in this cache entry */
    uint8_t data[BLOCK_SECTOR_SIZE]; /* The actual 512-byte block of data */

    /* Synchronization variables for Reader-Writer Lock */
    struct lock rw_lock;             /* Lock for exclusive write access or preventing eviction */
    int readers;                     /* Number of threads currently reading this block */
    struct lock readers_lock;        /* Lock to protect the 'readers' counter */
};

/* Buffer cache function prototypes */
void buffer_cache_init(void);
void buffer_cache_read(block_sector_t sector, void *buffer);
void buffer_cache_write(block_sector_t sector, const void *buffer);
struct cache_entry *buffer_cache_evict(void);
void buffer_cache_flush(void);

#endif /* filesys/cache.h */