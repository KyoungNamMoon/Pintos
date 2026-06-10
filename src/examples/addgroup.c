/* addgroup.c - Add a user to an existing group. Root only. */
#include <stdio.h>
#include <string.h>
#include <syscall.h>
#include <stdbool.h>
#include "auth.h"

static bool
user_exists (const char *username)
{
  struct passwd_entry pw[MAX_SLOTS];
  int pn = passwd_read_all (pw, MAX_SLOTS);
  for (int i = 0; i < pn; i++)
    if (strcmp (pw[i].username, username) == 0)
      return true;
  return false;
}

static int
add_user_to_group (const char *username, const char *groupname)
{
  struct group_entry ge[MAX_SLOTS];
  int gn = group_read_all (ge, MAX_SLOTS);
  for (int i = 0; i < gn; i++)
    {
      if (strcmp (ge[i].groupname, groupname) != 0) continue;
      if (strstr (ge[i].members, username) != NULL) return 2;
      if (ge[i].members[0] != '\0')
        strlcat (ge[i].members, ",", sizeof ge[i].members);
      strlcat (ge[i].members, username, sizeof ge[i].members);
      group_write_all (ge, gn);
      return 1;
    }
  return 0;
}

int
main (int argc, char *argv[])
{
  if (geteuid () != 0)
    { printf ("addgroup: only root can modify groups\n"); return 1; }
  if (argc != 3)
    { printf ("Usage: addgroup <username> <groupname>\n"); return 1; }

  const char *username  = argv[1];
  const char *groupname = argv[2];

  if (!user_exists (username))
    { printf ("addgroup: user '%s' not found\n", username); return 1; }

  int result = add_user_to_group (username, groupname);
  if (result == 2)
    { printf ("addgroup: '%s' is already in group '%s'\n", username, groupname); return 0; }
  if (result == 1)
    { printf ("addgroup: user '%s' added to group '%s'\n", username, groupname); return 0; }

  printf ("addgroup: group '%s' not found\n", groupname);
  return 1;
}