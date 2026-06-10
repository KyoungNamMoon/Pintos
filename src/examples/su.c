/* su.c - Switch user.
 * Root can switch to any user without a password.
 * Regular users must provide the target user's password.
 */
#include <stdio.h>
#include <string.h>
#include <syscall.h>
#include <stdbool.h>
#include <stdint.h>
#include "auth.h"

#define MAX_LEN 64

static bool
get_input (char *buf, int size, bool echo)
{
  int i = 0;
  char c;
  while (i < size - 1)
    {
      if (read (0, &c, 1) <= 0) return false;
      if (c == '\n' && i == 0) continue;
      if (c == '\n' || c == '\r') { printf ("\n"); break; }
      if ((c == '\b' || c == 127) && i > 0) { i--; continue; }
      if (echo) printf ("%c", c);
      buf[i++] = c;
    }
  buf[i] = '\0';
  return true;
}

int
main (int argc, char *argv[])
{
  if (argc < 2)
    {
      printf ("Usage: su <username>\n");
      return -1;
    }

  const char *target = argv[1];
  int target_uid;
  int target_gid;
  int caller_uid = getuid ();

  /* Lookup target user in its own block so the large array
     is freed from the stack before we call setresuid/setresgid. */
  {
    struct passwd_entry pe[MAX_SLOTS];
    int n = passwd_read_all (pe, MAX_SLOTS);
    int i;
    for (i = 0; i < n; i++)
      if (strcmp (pe[i].username, target) == 0)
        break;
    if (i == n)
      {
        printf ("su: user '%s' does not exist\n", target);
        return -1;
      }
    target_uid = pe[i].uid;
    target_gid = pe[i].gid;
  }
  /* pe array is now off the stack */

  /* Root can switch without password.
     Others must know the target user's password. */
  if (caller_uid != 0)
    {
      char password[MAX_LEN];
      printf ("Password: ");
      if (!get_input (password, MAX_LEN, false))
        return -1;

      /* Verify password in its own block to free stack space. */
      bool auth_ok = false;
      {
        struct shadow_entry se[MAX_SLOTS];
        int sn = shadow_read_all (se, MAX_SLOTS);
        int j;
        for (j = 0; j < sn; j++)
          if (strcmp (se[j].username, target) == 0)
            break;
        if (j == sn)
          {
            printf ("su: authentication failure\n");
            return -1;
          }
        if (se[j].is_locked || se[j].failed_attempts >= 5)
          {
            printf ("su: account is locked\n");
            return -1;
          }
        uint8_t computed[SHA256_HASH_SIZE];
        hash_password (password, se[j].salt, computed);
        auth_ok = (memcmp (computed, se[j].hash, SHA256_HASH_SIZE) == 0);
      }
      /* se array is now off the stack */

      if (!auth_ok)
        {
          printf ("su: authentication failure\n");
          return -1;
        }
    }

  /* Switch to target user — set gid first, then uid.
     Must set gid before uid since dropping root euid prevents gid changes. */
  setresgid (target_gid, target_gid, target_gid);

  if (!setresuid (target_uid, target_uid, target_uid))
    {
      printf ("su: failed to switch user\n");
      return -1;
    }

  /* Launch shell as target user. */
  char shell_cmd[MAX_LEN + 8];
  snprintf (shell_cmd, sizeof shell_cmd, "shell %s", target);
  int pid = exec (shell_cmd);
  if (pid < 0)
    {
      printf ("su: failed to launch shell\n");
      return -1;
    }
  wait (pid);
  return 0;
}