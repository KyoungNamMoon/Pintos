#include <stdio.h>
#include <syscall.h>
#include "lib/user/auth.h"

int
main (void)
{
  int uid = getuid ();
  char name[MAX_USERNAME_LEN];
  if (passwd_lookup_uid (uid, name, sizeof name))
    printf ("%s\n", name);
  else
    printf ("uid=%d\n", uid);
  return 0;
}
