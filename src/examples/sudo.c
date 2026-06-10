#include <stdio.h>
#include <string.h>
#include <syscall.h>
#include <stdbool.h>
#include <stdint.h>
#include "auth.h"

#define MAX_LEN 64

static bool
get_input_silent (char *buf, int size)
{
  int i = 0;
  char c;
  while (i < size - 1)
    {
      if (read (0, &c, 1) <= 0) return false;
      if (c == '\n' && i == 0) continue;
      if (c == '\n' || c == '\r') { printf ("\n"); break; }
      if ((c == '\b' || c == 127) && i > 0) { i--; continue; }
      buf[i++] = c;
    }
  buf[i] = '\0';
  return true;
}

static bool
verify_root_password (const char *password)
{
  int saved_euid = geteuid ();
  if (saved_euid != 0) setresuid (-1, 0, -1);

  struct shadow_entry se[MAX_SLOTS];
  int n = shadow_read_all (se, MAX_SLOTS);

  if (saved_euid != 0) setresuid (-1, saved_euid, -1);

  for (int i = 0; i < n; i++)
    {
      if (strcmp (se[i].username, "root") != 0) continue;
      if (se[i].is_locked || se[i].failed_attempts >= 5) return false;
      uint8_t computed[SHA256_HASH_SIZE];
      hash_password (password, se[i].salt, computed);
      return memcmp (computed, se[i].hash, SHA256_HASH_SIZE) == 0;
    }
  return false;
}

int
main (int argc, char *argv[])
{
  if (argc < 2)
    {
      printf ("Usage: sudo <command> [args...]\n");
      return 1;
    }

  char password[MAX_LEN];
  printf ("[sudo] password for root: ");
  if (!get_input_silent (password, MAX_LEN)) return 1;

  if (!verify_root_password (password))
    {
      printf ("sudo: authentication failure\n");
      return 1;
    }

  char cmd[128];
  cmd[0] = '\0';
  for (int i = 1; i < argc; i++)
    {
      if (i > 1) strlcat (cmd, " ", sizeof cmd);
      strlcat (cmd, argv[i], sizeof cmd);
    }

  int orig_uid  = getuid ();
  int orig_euid = geteuid ();
  setresuid (0, 0, 0);

  int pid = exec (cmd);
  if (pid < 0)
    {
      printf ("sudo: %s: command not found\n", argv[1]);
      setresuid (orig_uid, orig_euid, 0);
      return 1;
    }
  int status = wait (pid);
  setresuid (orig_uid, orig_euid, 0);
  return status >= 0 ? 0 : 1;
}
