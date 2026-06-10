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
      int n = read (0, &c, 1);
      if (n <= 0) return false;
      if (c == '\n' && i == 0) continue;
      if (c == '\n' || c == '\r') { printf ("\n"); break; }
      if ((c == '\b' || c == 127) && i > 0) { i--; continue; }
      buf[i++] = c;
    }
  buf[i] = '\0';
  return true;
}

/* Returns the new UID on success, -1 on failure. */
static int
add_to_passwd (const char *username)
{
  struct passwd_entry pw[MAX_SLOTS];
  int pn = passwd_read_all (pw, MAX_SLOTS);
  if (pn >= MAX_SLOTS) return -1;

  int max_uid = 999;
  for (int i = 0; i < pn; i++)
    if (pw[i].uid > max_uid) max_uid = pw[i].uid;

  struct passwd_entry *np = &pw[pn];
  memset (np, 0, sizeof *np);
  strlcpy (np->username, username, MAX_USERNAME_LEN);
  np->uid = max_uid + 1;
  np->gid = np->uid;
  snprintf (np->home_dir, MAX_HOME_LEN, "/home/%s", username);

  if (!passwd_write_all (pw, pn + 1)) return -1;
  return np->uid;
}

/* Returns true on success. */
static bool
add_to_shadow (const char *username, const char *password)
{
  struct shadow_entry se[MAX_SLOTS];
  int sn = shadow_read_all (se, MAX_SLOTS);

  for (int i = 0; i < sn; i++)
    if (strcmp (se[i].username, username) == 0) return false; /* duplicate */
  if (sn >= MAX_SLOTS) return false;

  struct shadow_entry *ns = &se[sn];
  memset (ns, 0, sizeof *ns);
  strlcpy (ns->username, username, MAX_USERNAME_LEN);
  generate_salt (ns->salt);
  hash_password (password, ns->salt, ns->hash);
  return shadow_write_all (se, sn + 1);
}

int
main (int argc, char *argv[])
{
  const char *msg = "useradd: entered main\n";
  write (1, msg, 22);

  if (geteuid () != 0)
    {
      printf ("useradd: only root can add users\n");
      return 1;
    }
  if (argc != 2)
    {
      printf ("Usage: useradd <username>\n");
      return 1;
    }

  const char *new_user = argv[1];

  char new_pass[MAX_LEN], confirm[MAX_LEN], reason[64];
  printf ("Enter new password for '%s': ", new_user);
  if (!get_input_silent (new_pass, MAX_LEN)) return 1;
  printf ("Confirm password: ");
  if (!get_input_silent (confirm, MAX_LEN)) return 1;

  if (strcmp (new_pass, confirm) != 0)
    {
      printf ("useradd: passwords do not match\n");
      return 1;
    }
  if (!password_complexity_ok (new_pass, reason))
    {
      printf ("useradd: bad password: %s\n", reason);
      return 1;
    }

  if (!add_to_shadow (new_user, new_pass))
    {
      printf ("useradd: failed to add '%s' (already exists or db full)\n",
              new_user);
      return 1;
    }

  int uid = add_to_passwd (new_user);
  if (uid < 0)
    {
      printf ("useradd: failed to write /etc/passwd\n");
      return 1;
    }

  printf ("useradd: user '%s' added (uid=%d)\n", new_user, uid);

  /* Create private group for new user (User Private Group scheme). */
  struct group_entry ge[MAX_SLOTS];
  int gn = group_read_all (ge, MAX_SLOTS);
  if (gn < MAX_SLOTS)
    {
      memset (&ge[gn], 0, sizeof ge[gn]);
      strlcpy (ge[gn].groupname, new_user, MAX_USERNAME_LEN);
      ge[gn].gid = uid;  /* gid == uid for private group */
      strlcpy (ge[gn].members, new_user, sizeof ge[gn].members);
      group_write_all (ge, gn + 1);
    }

  /* Create home directory owned by the new user. */
  mkdir ("/home");
  char home[64];
  snprintf (home, sizeof home, "/home/%s", new_user);
  mkdir (home);
  chmod (home, 0755);
  chown (home, uid, uid);
  return 0;
}
