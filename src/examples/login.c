/* login.c - Initial login loop.
 * Runs as root forever. Bootstraps auth files, then loops execing auth.
 * Auth handles authentication and privilege dropping.
 */
#include <stdio.h>
#include <string.h>
#include <syscall.h>
#include <stdbool.h>
#include <stdint.h>
#include "auth.h"

#define MAX_LEN 64
#ifndef S_ISUID
#define S_ISUID 04000
#endif

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
do_passwd (const char *username, const char *old_pass, const char *new_pass)
{
  char reason[64];
  if (!password_complexity_ok (new_pass, reason))
    {
      printf ("Bad password: %s\n", reason);
      return false;
    }

  struct shadow_entry se[MAX_SLOTS];
  int n = shadow_read_all (se, MAX_SLOTS);
  int i;
  for (i = 0; i < n; i++)
    if (strcmp (se[i].username, username) == 0) break;
  if (i == n) return false;

  uint8_t computed[SHA256_HASH_SIZE];
  hash_password (old_pass, se[i].salt, computed);
  if (memcmp (computed, se[i].hash, SHA256_HASH_SIZE) != 0) return false;

  generate_salt (se[i].salt);
  hash_password (new_pass, se[i].salt, se[i].hash);
  se[i].failed_attempts = 0;
  se[i].is_locked = false;
  shadow_write_all (se, n);
  return true;
}

static void
bootstrap_auth_files (void)
{
  int probe = open ("/etc/passwd");
  if (probe >= 0) { close (probe); return; }

  mkdir ("/etc");
  create ("/etc/passwd", 4096);
  create ("/etc/shadow", 8192);

  struct passwd_entry pw;
  memset (&pw, 0, sizeof pw);
  strlcpy (pw.username, "root", MAX_USERNAME_LEN);
  pw.uid = 0;
  pw.gid = 0;
  strlcpy (pw.home_dir, "/root", MAX_HOME_LEN);
  passwd_write_all (&pw, 1);
  chmod ("/etc/passwd", 0644);

  struct shadow_entry se;
  memset (&se, 0, sizeof se);
  strlcpy (se.username, "root", MAX_USERNAME_LEN);
  generate_salt (se.salt);
  hash_password ("Pintos1!", se.salt, se.hash);
  shadow_write_all (&se, 1);
  chmod ("/etc/shadow", 0000);
  /* Bootstrap /etc/group with root's private group. */
  create ("/etc/group", GROUP_MAXSIZE);
  struct group_entry ge;
  memset (&ge, 0, sizeof ge);
  strlcpy (ge.groupname, "root", MAX_USERNAME_LEN);
  ge.gid = 0;
  strlcpy (ge.members, "root", sizeof ge.members);
  group_write_all (&ge, 1);
  chmod ("/etc/group", 0644);
  /* Set setuid bit on trusted binaries so they run as root
     regardless of who calls them. This is the Unix mechanism
     for programs that need temporary root access. */
  chmod ("/sudo", S_ISUID | 0755);
  chmod ("/auth", S_ISUID | 0755);
  chmod ("/su",   S_ISUID | 0755);

  create ("/etc/first_boot", 0);
}

static void
first_boot_setup (void)
{
  int marker = open ("/etc/first_boot");
  if (marker < 0) return;
  close (marker);

  printf ("\n================================\n");
  printf ("  First Boot: Set Root Password  \n");
  printf ("================================\n");
  printf ("Password must be 8+ chars with uppercase,\n");
  printf ("lowercase, digit, and special character.\n\n");

  char new_pass[MAX_LEN], confirm[MAX_LEN];
  while (true)
    {
      printf ("New root password: ");
      if (!get_input (new_pass, MAX_LEN, false)) return;
      printf ("Confirm password:  ");
      if (!get_input (confirm, MAX_LEN, false)) return;

      if (strcmp (new_pass, confirm) != 0)
        { printf ("Passwords do not match. Try again.\n\n"); continue; }
      if (do_passwd ("root", "Pintos1!", new_pass))
        {
          remove ("/etc/first_boot");
          printf ("Root password set successfully.\n\n");
          return;
        }
      printf ("Password does not meet requirements. Try again.\n\n");
    }
}

int
main (void)
{
  printf ("================================\n");
  printf ("   Welcome to Pintos OS Login   \n");
  printf ("================================\n");

  bootstrap_auth_files ();
  first_boot_setup ();

  /* Login loop — always runs as root.
     Execs auth for each login attempt.
     Auth drops privileges and execs shell. */
  while (true)
    {
      int pid = exec ("auth");
      if (pid < 0)
        {
          printf ("Error: Failed to exec auth.\n");
          break;
        }
      wait (pid);
    }
  return 0;
}