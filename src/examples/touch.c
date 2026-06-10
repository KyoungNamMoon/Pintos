/* touch.c

   Creates an empty file if it does not already exist. */

#include <stdio.h>
#include <syscall.h>

int
main (int argc, char *argv[]) 
{
  if (argc != 2) 
    {
      printf ("usage: %s FILE\n", argv[0]);
      return EXIT_FAILURE;
    }

  int fd = open (argv[1]);
  if (fd >= 0) 
    {
      close (fd);
      return EXIT_SUCCESS;
    }

  if (!create (argv[1], 0)) 
    {
      printf ("%s: create failed\n", argv[1]);
      return EXIT_FAILURE;
    }

  return EXIT_SUCCESS;
}
