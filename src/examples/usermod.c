#include <stdio.h>
#include <string.h>
#include <syscall.h>
#include <stdbool.h>
#include "auth.h"

static bool
update_passwd (const char *username, const char *new_username,
               const char *new_home)
{
  struct passwd_entry pw[MAX_SLOTS];
  int pn = passwd_read_all (pw, MAX_SLOTS);
  int i;
  for (i = 0; i < pn; i++)
    if (strcmp (pw[i].username, username) == 0) break;
  if (i == pn) return false;

  if (new_username != NULL)
    strlcpy (pw[i].username, new_username, MAX_USERNAME_LEN);
  if (new_home != NULL)
    strlcpy (pw[i].home_dir, new_home, MAX_HOME_LEN);
  return passwd_write_all (pw, pn);
}

static void
rename_in_shadow (const char *old_name, const char *new_name)
{
  struct shadow_entry se[MAX_SLOTS];
  int sn = shadow_read_all (se, MAX_SLOTS);
  for (int i = 0; i < sn; i++)
    if (strcmp (se[i].username, old_name) == 0)
      {
        strlcpy (se[i].username, new_name, MAX_USERNAME_LEN);
        shadow_write_all (se, sn);
        return;
      }
}

int
main (int argc, char *argv[])
{
  if (geteuid () != 0)
    {
      printf ("usermod: only root can modify users\n");
      return 1;
    }
  if (argc < 3)
    {
      printf ("Usage: usermod [-l new_name] [-d new_home] <username>\n");
      printf ("  -l new_name   rename user\n");
      printf ("  -d new_home   change home directory\n");
      return 1;
    }

  const char *username     = argv[argc - 1];
  const char *new_username = NULL;
  const char *new_home     = NULL;

  for (int i = 1; i < argc - 1; i++)
    {
      if (strcmp (argv[i], "-l") == 0 && i + 1 < argc - 1)
        new_username = argv[++i];
      else if (strcmp (argv[i], "-d") == 0 && i + 1 < argc - 1)
        new_home = argv[++i];
    }

  if (new_username == NULL && new_home == NULL)
    {
      printf ("usermod: specify -l <new_name> or -d <new_home>\n");
      return 1;
    }
  if (new_username != NULL && strcmp (username, "root") == 0)
    {
      printf ("usermod: cannot rename the root account\n");
      return 1;
    }

  if (!update_passwd (username, new_username, new_home))
    {
      printf ("usermod: user '%s' not found\n", username);
      return 1;
    }

  if (new_username != NULL)
    rename_in_shadow (username, new_username);

  printf ("usermod: user '%s' updated successfully\n", username);
  return 0;
}
