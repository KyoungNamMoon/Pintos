#include <stdio.h>
#include <stdlib.h>
#include <syscall.h>
#include "auth.h"

int
main (void)
{
  int uid = getuid ();
  int gid = getgid ();
  char uname[MAX_USERNAME_LEN] = "";
  char gname[MAX_USERNAME_LEN] = "";

  passwd_lookup_uid (uid, uname, sizeof uname);
  group_lookup_gid (gid, gname, sizeof gname);

  if (uname[0] && gname[0])
    printf ("uid=%d(%s) gid=%d(%s)\n", uid, uname, gid, gname);
  else if (uname[0])
    printf ("uid=%d(%s) gid=%d\n", uid, uname, gid);
  else
    printf ("uid=%d gid=%d\n", uid, gid);

  return EXIT_SUCCESS;
}