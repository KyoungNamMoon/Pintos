#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include <stdbool.h>
#include <stdint.h>
#include "filesys/off_t.h"
#include "devices/block.h"
#include "threads/thread.h"

/* Permission bit constants (POSIX, see man 2 chmod) */
#define S_IRUSR  0400    /* Owner read */
#define S_IWUSR  0200    /* Owner write */
#define S_IXUSR  0100    /* Owner execute */
#define S_IROTH  0004    /* Others read */
#define S_IWOTH  0002    /* Others write */
#define S_IXOTH  0001    /* Others execute */
#define S_ISVTX  01000   /* Sticky bit — only owner can delete files in directory */
#define S_ISUID  04000   /* Set user ID on execution (defined, not enforced) */
#define S_IRGRP  0040    /* Group read */
#define S_IWGRP  0020    /* Group write */
#define S_IXGRP  0010    /* Group execute */
#define S_ISGID  02000   /* Set group ID on execution */

struct bitmap;

void inode_init (void);
bool inode_create (block_sector_t, off_t);
struct inode *inode_open (block_sector_t);
struct inode *inode_reopen (struct inode *);
block_sector_t inode_get_inumber (const struct inode *);
void inode_close (struct inode *);
void inode_remove (struct inode *);
off_t inode_read_at (struct inode *, void *, off_t size, off_t offset);
off_t inode_write_at (struct inode *, const void *, off_t size, off_t offset);
void inode_deny_write (struct inode *);
void inode_allow_write (struct inode *);
off_t inode_length (const struct inode *);
bool inode_is_dir (const struct inode *);
void inode_set_dir (struct inode *, bool);
bool inode_is_removed (const struct inode *);
bool inode_check_permission (const struct inode *, int requested);
uint32_t inode_get_owner (const struct inode *);
uint32_t inode_get_group (const struct inode *);
uint16_t inode_get_mode (const struct inode *);
bool inode_set_mode (struct inode *, uint16_t mode);
bool inode_set_owner (struct inode *, uint32_t uid, uint32_t gid);

#endif /* filesys/inode.h */