/* auth.c - Authenticates a user and drops privileges before execing shell.
 * Runs as root, verifies password, drops to user uid/gid, execs shell.
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
      int n = read (0, &c, 1);
      if (n <= 0) return false;
      if (c == '\n' && i == 0) continue;
      if (c == '\n' || c == '\r') { printf ("\n"); break; }
      if (c == '\b' || c == 127)
        {
          if (i > 0) { i--; if (echo) printf ("\b \b"); }
          continue;
        }
      if (echo) printf ("%c", c);
      buf[i++] = c;
    }
  buf[i] = '\0';
  return true;
}

static bool
verify_password (const char *username, const char *password)
{
  struct shadow_entry se[MAX_SLOTS];
  int n = shadow_read_all (se, MAX_SLOTS);
  int i;
  for (i = 0; i < n; i++)
    if (strcmp (se[i].username, username) == 0) break;
  if (i == n) return false;
  if (se[i].is_locked || se[i].failed_attempts >= 5) return false;

  uint8_t computed[SHA256_HASH_SIZE];
  hash_password (password, se[i].salt, computed);
  bool ok = (memcmp (computed, se[i].hash, SHA256_HASH_SIZE) == 0);

  if (ok) se[i].failed_attempts = 0;
  else { se[i].failed_attempts++; if (se[i].failed_attempts >= 5) se[i].is_locked = true; }
  shadow_write_all (se, n);
  return ok;
}

static void
get_user_ids (const char *username, int *uid_out, int *gid_out)
{
  struct passwd_entry pe[MAX_SLOTS];
  int n = passwd_read_all (pe, MAX_SLOTS);
  for (int i = 0; i < n; i++)
    if (strcmp (pe[i].username, username) == 0)
      { *uid_out = pe[i].uid; *gid_out = pe[i].gid; return; }
}

static bool
user_in_group (const char *members, const char *username)
{
  /* members is a comma-separated list e.g. "alice,bob" */
  const char *p = members;
  int ulen = strlen (username);
  while (*p)
    {
      const char *comma = p;
      while (*comma && *comma != ',') comma++;
      if ((int)(comma - p) == ulen && memcmp (p, username, ulen) == 0)
        return true;
      p = (*comma == ',') ? comma + 1 : comma;
    }
  return false;
}

int
main (void)
{
  char input_user[MAX_LEN], input_pass[MAX_LEN];

  printf ("login: ");
  if (!get_input (input_user, MAX_LEN, true)) return -1;
  printf ("password: ");
  if (!get_input (input_pass, MAX_LEN, false)) return -1;

  if (!verify_password (input_user, input_pass))
    {
      printf ("Login incorrect or account locked.\n");
      return -1;
    }

  printf ("Login successful! Welcome, %s.\n", input_user);

  int target_uid = 0;
  int target_gid = 0;
  get_user_ids (input_user, &target_uid, &target_gid);

  /* Check /etc/group for supplementary group membership.
     If the user belongs to a group other than their private group,
     use that as their effective gid at login. */
  {
    struct group_entry ge[MAX_SLOTS];
    int gn = group_read_all (ge, MAX_SLOTS);
    for (int i = 0; i < gn; i++)
      {
        if (ge[i].gid == target_gid) continue;  /* skip private group */
        if (user_in_group (ge[i].members, input_user))
          {
            target_gid = ge[i].gid;
            break;
          }
      }
  }

  setresgid (target_gid, target_gid, target_gid);
  setresuid (target_uid, target_uid, target_uid);

  /* Change to user's home directory. */
  char home[MAX_LEN + 8];
  snprintf (home, sizeof home, "/home/%s", input_user);
  if (!chdir (home))
    chdir ("/");  /* fallback to root if home doesn't exist */

  /* Exec shell as the logged-in user. */
  char shell_cmd[MAX_LEN + 8];
  snprintf (shell_cmd, sizeof shell_cmd, "shell %s", input_user);
  int pid = exec (shell_cmd);
  if (pid < 0)
    {
      printf ("Error: Failed to execute shell.\n");
      return -1;
    }
  wait (pid);
  printf ("\nLogged out.\n");
  return 0;
}