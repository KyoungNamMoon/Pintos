#include "filesys/cache.h"
#include "threads/synch.h"
#include "devices/block.h"
#include <stdbool.h>
#include <string.h>

struct cache_entry buffer_cache[BUFFER_CACHE_SIZE];
static int clock_hand = 0;  /* Points to cache entry for clock algorithm*/
struct lock cache_global_lock; /*Global lock */
extern struct block *fs_device; /* The file system block device */

/* Initializes the buffer cache */
void buffer_cache_init(void) {
    lock_init(&cache_global_lock);

    for (int i = 0; i < BUFFER_CACHE_SIZE; i++) {
        buffer_cache[i].valid = false;     
        buffer_cache[i].dirty = false;     
        buffer_cache[i].accessed = false;  
        buffer_cache[i].readers = 0;
        lock_init(&buffer_cache[i].rw_lock);
        lock_init(&buffer_cache[i].readers_lock);
    }
}

/* Evicts a cache entry using the clock algorithm */
struct cache_entry *buffer_cache_evict(void) {
    while (true) {
        struct cache_entry *ce = &buffer_cache[clock_hand];
        
        if (!ce->valid) {
            lock_acquire(&ce->rw_lock); 
            return ce;
        }

        if (lock_try_acquire(&ce->rw_lock)) {
            if (ce->accessed) {
                ce->accessed = false;
                lock_release(&ce->rw_lock);
            } else {
                if (ce->dirty) {
                    block_write(fs_device, ce->sector, ce->data);
                    ce->dirty = false;
                }
                ce->valid = false;
                return ce;
            }
        }
        clock_hand = (clock_hand + 1) % BUFFER_CACHE_SIZE;
    }
}

/* Reads a secotr form the buffer cache into the provided buffer */
void buffer_cache_read(block_sector_t sector, void *buffer) {
    struct cache_entry *ce = NULL;

    lock_acquire(&cache_global_lock);
    for (int i = 0; i < BUFFER_CACHE_SIZE; i++) {
        if (buffer_cache[i].valid && buffer_cache[i].sector == sector) {
            ce = &buffer_cache[i];
            lock_acquire(&ce->readers_lock);
            ce->readers++;
            if (ce->readers == 1) {
                lock_acquire(&ce->rw_lock);
            }
            lock_release(&ce->readers_lock);
            break; 
        }
    }
    
    if (ce == NULL) {
        ce = buffer_cache_evict(); 
        
        ce->valid = true;
        ce->sector = sector;
        ce->dirty = false;
        
        lock_release(&cache_global_lock);
        block_read(fs_device, sector, ce->data); 
        lock_acquire(&ce->readers_lock);
        ce->readers++; 
        lock_release(&ce->readers_lock);
    } else {
        lock_release(&cache_global_lock);
    }

    ce->accessed = true;
    memcpy(buffer, ce->data, BLOCK_SECTOR_SIZE);
    lock_acquire(&ce->readers_lock);
    ce->readers--; 
    if (ce->readers == 0) {
        lock_release(&ce->rw_lock);
    }
    lock_release(&ce->readers_lock);
}

/* Writes data from the provided buffer into the buffer cache. */
void buffer_cache_write(block_sector_t sector, const void *buffer) {
    struct cache_entry *ce = NULL;

    lock_acquire(&cache_global_lock);
    for (int i = 0; i < BUFFER_CACHE_SIZE; i++) {
        if (buffer_cache[i].valid && buffer_cache[i].sector == sector) {
            ce = &buffer_cache[i];
            lock_acquire(&ce->rw_lock);
            break; 
        }
    }

    if (ce == NULL) {
        ce = buffer_cache_evict(); 
        ce->valid = true;
        ce->sector = sector;
        ce->dirty = false; 
        lock_release(&cache_global_lock);
        block_read(fs_device, sector, ce->data); 
    } else {
        lock_release(&cache_global_lock);
    }

    ce->accessed = true;
    ce->dirty = true; 
    memcpy(ce->data, buffer, BLOCK_SECTOR_SIZE);
    lock_release(&ce->rw_lock);
}


/* Flushes all dirty cache entries back to the disk. 
   Usually called during OS shutdown. */
void buffer_cache_flush(void) {
    lock_acquire(&cache_global_lock);
    
    for (int i = 0; i < BUFFER_CACHE_SIZE; i++) {
        struct cache_entry *ce = &buffer_cache[i];     
        if (ce->valid && ce->dirty) {
            lock_acquire(&ce->rw_lock);
            block_write(fs_device, ce->sector, ce->data);
            ce->dirty = false;
            lock_release(&ce->rw_lock);
        }
    }  
    lock_release(&cache_global_lock);
}

