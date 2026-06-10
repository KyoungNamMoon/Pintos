#include <stdio.h>
#include <stdlib.h>
#include <syscall.h>

/* Simple octal string to unsigned converter. */
static unsigned
parse_octal (const char *s)
{
  unsigned result = 0;
  while (*s >= '0' && *s <= '7')
    {
      result = result * 8 + (*s - '0');
      s++;
    }
  return result;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: chmod <file> <mode>\n");
        return -1;
    }
    unsigned mode = parse_octal (argv[2]);
    if (chmod(argv[1], mode)) {
        printf("chmod: %s -> %o\n", argv[1], mode);
        return 0;
    } else {
        printf("chmod: failed (permission denied or file not found)\n");
        return -1;
    }
}