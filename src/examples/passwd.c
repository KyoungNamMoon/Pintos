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

int
main (int argc, char *argv[])
{
  if (argc != 2)
    {
      printf ("Usage: passwd <username>\n");
      return 1;
    }

  const char *username = argv[1];

  struct shadow_entry se[MAX_SLOTS];
  int n = shadow_read_all (se, MAX_SLOTS);
  int i;
  for (i = 0; i < n; i++)
    if (strcmp (se[i].username, username) == 0) break;
  if (i == n)
    {
      printf ("passwd: user '%s' not found\n", username);
      return 1;
    }

  char old_pass[MAX_LEN];
  printf ("Current password: ");
  if (!get_input_silent (old_pass, MAX_LEN)) return 1;

  uint8_t computed[SHA256_HASH_SIZE];
  hash_password (old_pass, se[i].salt, computed);
  if (memcmp (computed, se[i].hash, SHA256_HASH_SIZE) != 0)
    {
      printf ("passwd: authentication failure\n");
      return 1;
    }

  char new_pass[MAX_LEN], confirm[MAX_LEN];
  char reason[64];

  printf ("New password: ");
  if (!get_input_silent (new_pass, MAX_LEN)) return 1;
  printf ("Retype new password: ");
  if (!get_input_silent (confirm, MAX_LEN)) return 1;

  if (strcmp (new_pass, confirm) != 0)
    {
      printf ("passwd: passwords do not match\n");
      return 1;
    }
  if (!password_complexity_ok (new_pass, reason))
    {
      printf ("passwd: bad password: %s\n", reason);
      return 1;
    }

  generate_salt (se[i].salt);
  hash_password (new_pass, se[i].salt, se[i].hash);
  se[i].failed_attempts = 0;
  shadow_write_all (se, n);

  printf ("passwd: password updated successfully\n");
  return 0;
}
