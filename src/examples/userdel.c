#include <stdio.h>
#include <string.h>
#include <syscall.h>
#include <stdbool.h>
#include "auth.h"

static bool
del_from_shadow (const char *username)
{
  struct shadow_entry se[MAX_SLOTS];
  int sn = shadow_read_all (se, MAX_SLOTS);
  int i;
  for (i = 0; i < sn; i++)
    if (strcmp (se[i].username, username) == 0) break;
  if (i == sn) return false;
  for (int j = i; j < sn - 1; j++)
    se[j] = se[j + 1];
  return shadow_write_all (se, sn - 1);
}

static void
del_from_passwd (const char *username)
{
  struct passwd_entry pw[MAX_SLOTS];
  int pn = passwd_read_all (pw, MAX_SLOTS);
  for (int i = 0; i < pn; i++)
    if (strcmp (pw[i].username, username) == 0)
      {
        for (int j = i; j < pn - 1; j++)
          pw[j] = pw[j + 1];
        passwd_write_all (pw, pn - 1);
        return;
      }
}

int
main (int argc, char *argv[])
{
  if (geteuid () != 0)
    {
      printf ("userdel: only root can delete users\n");
      return 1;
    }
  if (argc != 2)
    {
      printf ("Usage: userdel <username>\n");
      return 1;
    }

  const char *username = argv[1];

  if (strcmp (username, "root") == 0)
    {
      printf ("userdel: cannot delete the root account\n");
      return 1;
    }

  if (!del_from_shadow (username))
    {
      printf ("userdel: user '%s' not found\n", username);
      return 1;
    }
  del_from_passwd (username);

  printf ("userdel: user '%s' deleted\n", username);
  return 0;
}
