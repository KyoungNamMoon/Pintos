#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/cache.h"
#include "threads/thread.h"
#include "threads/malloc.h"

struct block *fs_device;

static void do_format (void);

/* Parses PATH and returns the directory containing the final component,
   storing the final component name in NAME_OUT.  Supports absolute and
   relative paths; consecutive slashes are treated as one.  Returns NULL
   if any intermediate component is missing or not a directory, or if the
   final component name exceeds NAME_MAX.  Caller must close the returned
   directory. */
static struct dir *
resolve_path (const char *path, char name_out[NAME_MAX + 1])
{
  if (path == NULL || *path == '\0')
    return NULL;

  /* Make a working copy since strtok_r destroys its input. */
  char *copy = malloc (strlen (path) + 1);
  if (copy == NULL)
    return NULL;
  strlcpy (copy, path, strlen (path) + 1);

  /* Absolute paths start from root; relative paths start from the
     current thread's working directory, falling back to root if
     no working directory has been set yet. */
  struct dir *dir;
  if (copy[0] == '/')
    dir = dir_open_root ();
  else
    {
      struct thread *t = thread_current ();
      if (t->cwd == NULL)
        dir = dir_open_root ();
      else
        {
          /* After removing the cwd inode, relative paths must fail (dir-rm-cwd). */
          if (inode_is_removed (dir_get_inode (t->cwd)))
            {
              free (copy);
              return NULL;
            }
          dir = dir_reopen (t->cwd);
        }
    }

  if (dir == NULL)
    {
      free (copy);
      return NULL;
    }

  char *token, *save_ptr;
  char *next_token = NULL;

  /* Get the first path component. */
  token = strtok_r (copy, "/", &save_ptr);

  if (token == NULL)
    {
      /* Path was "/" or all slashes — the target is the directory
         itself, so return it with "." as the name. */
      strlcpy (name_out, ".", NAME_MAX + 1);
      free (copy);
      return dir;
    }

  /* Walk all components except the last, advancing DIR one level
     at a time.  Each intermediate component must exist and be a
     directory with execute permission. */
  next_token = strtok_r (NULL, "/", &save_ptr);
  while (next_token != NULL)
    {
      /* Check execute permission on current directory before
         looking up the next component. */
      if (!inode_check_permission (dir_get_inode (dir), 1))
        {
          dir_close (dir);
          free (copy);
          return NULL;
        }

      struct inode *inode = NULL;
      if (!dir_lookup (dir, token, &inode))
        {
          /* Intermediate component not found. */
          dir_close (dir);
          free (copy);
          return NULL;
        }
      struct dir *next_dir = dir_open (inode);
      if (next_dir == NULL)
        {
          /* Component exists but is not a directory. */
          dir_close (dir);
          free (copy);
          return NULL;
        }
      dir_close (dir);
      dir = next_dir;
      token = next_token;
      next_token = strtok_r (NULL, "/", &save_ptr);
    }

  /* TOKEN is now the final path component.  Reject names that
     exceed NAME_MAX to match the filesystem's name length limit. */
  if (strlen (token) > NAME_MAX)
    {
      dir_close (dir);
      free (copy);
      return NULL;
    }

  strlcpy (name_out, token, NAME_MAX + 1);
  free (copy);
  return dir;
}

/* Initializes the file system module, locating the file system block
   device and setting up the inode table, free map, and buffer cache.
   If FORMAT is true, reformats the file system from scratch. */
void
filesys_init (bool format)
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  free_map_init ();
  buffer_cache_init ();

  if (format)
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, flushing all dirty cache blocks
   to disk and closing the free map file. */
void
filesys_done (void)
{
  buffer_cache_flush ();
  free_map_close ();
}

/* Creates a new file at PATH with INITIAL_SIZE bytes.  Resolves the
   path to find the parent directory, allocates an inode sector, and
   adds the new entry.  Frees the allocated sector on failure.
   Returns true if successful, false otherwise. */
bool
filesys_create (const char *path, off_t initial_size)
{
  block_sector_t inode_sector = 0;
  char name[NAME_MAX + 1];
  struct dir *dir = resolve_path (path, name);
  bool success = (dir != NULL
                  && *name != '\0'
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size)
                  && dir_add (dir, name, inode_sector));
  if (!success && inode_sector != 0)
    free_map_release (inode_sector, 1);
  dir_close (dir);
  return success;
}

/* Opens the file or directory at PATH and returns a struct file for it.
   Handles the root directory as a special case.  Returns NULL if the
   path cannot be resolved, the entry does not exist, or the inode has
   been removed (e.g. deleted while still open). */
struct file *
filesys_open (const char *path)
{
  /* Opening root is a special case since resolve_path returns "."
     as the name, which would require an extra lookup. */
  if (strcmp (path, "/") == 0)
    return file_open (inode_open (ROOT_DIR_SECTOR));

  char name[NAME_MAX + 1];
  struct dir *dir = resolve_path (path, name);
  if (dir == NULL)
    return NULL;

  /* If the final component is empty or ".", the target is the
     directory itself rather than an entry within it. */
  struct inode *inode = NULL;
  if (*name == '\0' || strcmp (name, ".") == 0)
    inode = inode_reopen (dir_get_inode (dir));
  else
    dir_lookup (dir, name, &inode);

  dir_close (dir);

  /* Reject inodes that have been removed but not yet freed, so that
     callers cannot open files or directories pending deletion. */
  if (inode != NULL && inode_is_removed (inode))
    {
      inode_close (inode);
      return NULL;
    }
  return file_open (inode);
}

/* Deletes the file or empty directory at PATH.  Returns true if
   successful, false if the path cannot be resolved or the target
   directory is not empty. */
bool
filesys_remove (const char *path)
{
  char name[NAME_MAX + 1];
  struct dir *dir = resolve_path (path, name);
  bool success = dir != NULL && dir_remove (dir, name);
  dir_close (dir);
  return success;
}

/* Creates a new directory at PATH with an initial capacity of 16 entries.
   On success, populates the new directory with "." and ".." entries and
   marks its inode as a directory.  Frees the allocated sector on failure.
   Returns true if successful, false otherwise. */
bool
filesys_mkdir (const char *path)
{
  block_sector_t inode_sector = 0;
  char name[NAME_MAX + 1];
  struct dir *dir = resolve_path (path, name);

  if (dir == NULL || *name == '\0')
    {
      dir_close (dir);
      return false;
    }

  bool success = (free_map_allocate (1, &inode_sector)
                  && dir_create (inode_sector, 16)
                  && dir_add (dir, name, inode_sector));

  if (!success && inode_sector != 0)
    free_map_release (inode_sector, 1);

  if (success)
    {
      /* Add "." pointing to itself and ".." pointing to the parent
         so that relative path traversal works correctly. */
      struct dir *new_dir = dir_open (inode_open (inode_sector));
      if (new_dir != NULL)
        {
          dir_add (new_dir, ".", inode_sector);
          dir_add (new_dir, "..", inode_get_inumber (dir_get_inode (dir)));
          dir_close (new_dir);
        }
      /* Mark the inode as a directory so isdir() and path resolution
         can distinguish it from regular files. */
      struct inode *inode = inode_open (inode_sector);
      if (inode != NULL)
        {
          inode_set_dir (inode, true);
          inode_close (inode);
        }
    }

  dir_close (dir);
  return success;
}

/* Opens and returns the directory at PATH, or NULL if the path cannot
   be resolved, the target is not a directory, or it has been removed. */
struct dir *
filesys_opendir (const char *path)
{
  if (strcmp (path, "/") == 0)
    return dir_open_root ();

  char name[NAME_MAX + 1];
  struct dir *dir = resolve_path (path, name);
  if (dir == NULL)
    return NULL;

  /* If the final component is empty or ".", the target is DIR itself. */
  struct inode *inode = NULL;
  if (*name == '\0' || strcmp (name, ".") == 0)
    inode = inode_reopen (dir_get_inode (dir));
  else
    dir_lookup (dir, name, &inode);

  dir_close (dir);

  if (inode == NULL || !inode_is_dir (inode) || inode_is_removed (inode))
    {
      inode_close (inode);
      return NULL;
    }
  return dir_open (inode);
}

/* Formats the file system by creating a fresh free map and root directory.
   Initializes "." and ".." in the root to point to itself. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");

  /* Root's "." and ".." both point to itself since it has no parent. */
  struct dir *root = dir_open_root ();
  if (root != NULL)
    {
      dir_add (root, ".", ROOT_DIR_SECTOR);
      dir_add (root, "..", ROOT_DIR_SECTOR);
      struct inode *inode = dir_get_inode (root);
      inode_set_dir (inode, true);
      dir_close (root);
    }

  free_map_close ();
  printf ("done.\n");
}