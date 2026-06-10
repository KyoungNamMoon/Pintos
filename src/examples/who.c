#include <stdio.h>
#include <syscall.h>
#include "lib/user/auth.h"

int
main (void)
{
  int uid = getuid ();
  char name[MAX_USERNAME_LEN];
  if (!passwd_lookup_uid (uid, name, sizeof name))
    snprintf (name, sizeof name, "uid%d", uid);
  printf ("%-16s pts/0\n", name);
  return 0;
}
