#include <stdio.h>
#include <syscall.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include "auth.h"

static bool
add_user (const char *user, const char *pass)
{
  if (!password_complexity_ok (pass, NULL)) return false;

  struct shadow_entry se[MAX_SLOTS];
  int sn = shadow_read_all (se, MAX_SLOTS);
  for (int i = 0; i < sn; i++)
    if (strcmp (se[i].username, user) == 0) return false;
  if (sn >= MAX_SLOTS) return false;

  struct shadow_entry *ns = &se[sn];
  memset (ns, 0, sizeof *ns);
  strlcpy (ns->username, user, MAX_USERNAME_LEN);
  generate_salt (ns->salt);
  hash_password (pass, ns->salt, ns->hash);
  shadow_write_all (se, sn + 1);

  struct passwd_entry pw[MAX_SLOTS];
  int pn = passwd_read_all (pw, MAX_SLOTS);
  if (pn >= MAX_SLOTS) return false;
  int max_uid = 999;
  for (int i = 0; i < pn; i++)
    if (pw[i].uid > max_uid) max_uid = pw[i].uid;
  struct passwd_entry *np = &pw[pn];
  memset (np, 0, sizeof *np);
  strlcpy (np->username, user, MAX_USERNAME_LEN);
  np->uid = max_uid + 1;
  np->gid = np->uid;
  snprintf (np->home_dir, MAX_HOME_LEN, "/home/%s", user);
  passwd_write_all (pw, pn + 1);
  return true;
}

static bool
auth (const char *user, const char *pass)
{
  struct shadow_entry se[MAX_SLOTS];
  int n = shadow_read_all (se, MAX_SLOTS);
  for (int i = 0; i < n; i++)
    {
      if (strcmp (se[i].username, user) != 0) continue;
      if (se[i].is_locked || se[i].failed_attempts >= 5) return false;
      uint8_t h[SHA256_HASH_SIZE];
      hash_password (pass, se[i].salt, h);
      return memcmp (h, se[i].hash, SHA256_HASH_SIZE) == 0;
    }
  return false;
}

int
main (void)
{
  printf ("=== useradd/login test (Linux-style text I/O) ===\n");

  bool added = add_user ("tuser", "Test1234!");
  printf ("useradd tuser/Test1234!: %s\n", added ? "OK" : "FAIL");

  bool ok = auth ("tuser", "Test1234!");
  printf ("auth tuser/Test1234!: %s\n", ok ? "OK" : "FAIL");

  bool bad = auth ("tuser", "wrongpass");
  printf ("auth tuser/wrongpass (must fail): %s\n", !bad ? "OK" : "FAIL");

  printf ("=== done ===\n");
  return 0;
}
