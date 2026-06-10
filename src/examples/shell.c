#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <syscall.h>

static void read_line (char line[], size_t);
static bool backspace (char **pos, char line[]);
static void echo_char (char c);
static void echo_str (const char *s);
static bool getcwd (char *cwd, size_t cwd_size);
static bool get_inumber (const char *file_name, int *inum);
static bool prepend (const char *prefix, char *dst, size_t *dst_len,
                     size_t dst_size);

int
main (int argc, char *argv[])
{
  const char *username = (argc > 1) ? argv[1] : "user";

  /* If started without a username, run login first. */
  if (argc <= 1)
    {
      pid_t pid = exec ("login");
      if (pid != PID_ERROR)
        wait (pid);
    }

  printf ("Shell starting...\n");
  for (;;)
    {
      char command[80];
      char cwd[128];

      /* Read command. */
      if (!getcwd (cwd, sizeof cwd))
        strlcpy (cwd, "?", sizeof cwd);
      printf ("%s@pintos:%s$ ", username, cwd);
      read_line (command, sizeof command);
      
      /* Execute command. */
      if (!strcmp (command, "exit"))
        {
          break;
        }
      else if (!strcmp (command, "cd") || !memcmp (command, "cd ", 3))
        {
          const char *dir = command + 2;
          while (*dir == ' ')
            dir++;
          if (*dir == '\0')
            dir = "/";
          if (!chdir (dir))
            printf ("\"%s\": chdir failed\n", dir);
        }
      else if (command[0] == '\0') 
        {
          /* Empty command. */
        }
      else
        {
          /* Block login from being called inside a shell session.
             Users must use 'su' to switch users. */
          if (!strcmp (command, "login") ||
              !memcmp (command, "login ", 6))
            {
              printf ("login: cannot run from shell. Use 'su' to switch users.\n");
            }
          else
            {
              pid_t pid = exec (command);
              if (pid != PID_ERROR)
                wait (pid);
              else
                printf ("exec failed\n");
            }
        }
    }

  printf ("Shell exiting.");
  return EXIT_SUCCESS;
}

/* Reads a line of input from the user into LINE, which has room
   for SIZE bytes.  Handles backspace and Ctrl+U in the ways
   expected by Unix users.  On return, LINE will always be
   null-terminated and will not end in a new-line character. */
static void
read_line (char line[], size_t size) 
{
  char *pos = line;
  for (;;)
    {
      char c;
      read (STDIN_FILENO, &c, 1);

      switch (c) 
        {
        case '\r':
          *pos = '\0';
          echo_char ('\n');
          return;

        case '\b':
          backspace (&pos, line);
          break;

        case ('U' - 'A') + 1:       /* Ctrl+U. */
          while (backspace (&pos, line))
            continue;
          break;

        default:
          /* Add character to line. */
          if (pos < line + size - 1) 
            {
              echo_char (c);
              *pos++ = c;
            }
          break;
        }
    }
}

/* If *POS is past the beginning of LINE, backs up one character
   position.  Returns true if successful, false if nothing was
   done. */
static bool
backspace (char **pos, char line[]) 
{
  if (*pos > line)
    {
      /* Back up cursor, overwrite character, back up
         again. */
      echo_str ("\b \b");
      (*pos)--;
      return true;
    }
  else
    return false;
}

static void
echo_char (char c)
{
  write (STDOUT_FILENO, &c, 1);
}

static void
echo_str (const char *s)
{
  write (STDOUT_FILENO, s, strlen (s));
}

static bool
get_inumber (const char *file_name, int *inum)
{
  int fd = open (file_name);
  if (fd < 0)
    return false;

  *inum = inumber (fd);
  close (fd);
  return true;
}

static bool
prepend (const char *prefix, char *dst, size_t *dst_len, size_t dst_size)
{
  size_t prefix_len = strlen (prefix);
  if (prefix_len + *dst_len + 1 > dst_size)
    return false;

  *dst_len += prefix_len;
  memcpy ((dst + dst_size) - *dst_len, prefix, prefix_len);
  return true;
}

static bool
getcwd (char *cwd, size_t cwd_size)
{
  size_t cwd_len = 0;

#define MAX_LEVEL 20
  char name[MAX_LEVEL * 3 + 1 + READDIR_MAX_LEN + 1];
  char *namep;
  int child_inum;

  if (cwd_size < 2 || !get_inumber (".", &child_inum))
    return false;

  namep = name;
  for (;;)
    {
      int parent_inum, parent_fd;

      if ((namep - name) > MAX_LEVEL * 3)
        return false;
      *namep++ = '.';
      *namep++ = '.';
      *namep = '\0';

      parent_fd = open (name);
      if (parent_fd < 0)
        return false;
      *namep++ = '/';

      parent_inum = inumber (parent_fd);
      if (parent_inum == child_inum)
        {
          close (parent_fd);
          break;
        }

      for (;;)
        {
          int test_inum;
          if (!readdir (parent_fd, namep) || !get_inumber (name, &test_inum))
            {
              close (parent_fd);
              return false;
            }
          if (test_inum == child_inum)
            break;
        }
      close (parent_fd);

      if (!prepend (namep - 1, cwd, &cwd_len, cwd_size))
        return false;
      child_inum = parent_inum;
    }

  if (cwd_len > 0)
    {
      memmove (cwd, (cwd + cwd_size) - cwd_len, cwd_len);
      cwd[cwd_len] = '\0';
    }
  else
    {
      strlcpy (cwd, "/", cwd_size);
    }

  return true;
#undef MAX_LEVEL
}
