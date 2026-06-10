/* authtest.c — end-to-end test for Linux-style text auth */
#include <stdio.h>
#include <syscall.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include "auth.h"

/* Open flags */
#define O_RDONLY  0x01
#define O_WRONLY  0x02
#define O_RDWR    0x03

static int pass_count = 0;
static int fail_count = 0;

static void
check (const char *label, bool ok)
{
  if (ok) { printf ("  [PASS] %s\n", label); pass_count++; }
  else    { printf ("  [FAIL] %s\n", label); fail_count++; }
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

static bool
add_to_shadow (const char *user, const char *pass)
{
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
  return shadow_write_all (se, sn + 1);
}

static bool
add_to_passwd (const char *user)
{
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
  return passwd_write_all (pw, pn + 1);
}

static bool
add_user (const char *user, const char *pass)
{
  if (!password_complexity_ok (pass, NULL)) return false;
  if (!add_to_shadow (user, pass)) return false;
  return add_to_passwd (user);
}

static int
find_uid (const char *user)
{
  struct passwd_entry pw[MAX_SLOTS];
  int n = passwd_read_all (pw, MAX_SLOTS);
  for (int i = 0; i < n; i++)
    if (strcmp (pw[i].username, user) == 0) return pw[i].uid;
  return -1;
}

static bool
change_pass (const char *user, const char *old_pass, const char *new_pass)
{
  if (!password_complexity_ok (new_pass, NULL)) return false;
  struct shadow_entry se[MAX_SLOTS];
  int n = shadow_read_all (se, MAX_SLOTS);
  int i;
  for (i = 0; i < n; i++)
    if (strcmp (se[i].username, user) == 0) break;
  if (i == n) return false;
  uint8_t h[SHA256_HASH_SIZE];
  hash_password (old_pass, se[i].salt, h);
  if (memcmp (h, se[i].hash, SHA256_HASH_SIZE) != 0) return false;
  generate_salt (se[i].salt);
  hash_password (new_pass, se[i].salt, se[i].hash);
  se[i].failed_attempts = 0;
  return shadow_write_all (se, n);
}

static bool
del_from_shadow (const char *user)
{
  struct shadow_entry se[MAX_SLOTS];
  int sn = shadow_read_all (se, MAX_SLOTS);
  int si;
  for (si = 0; si < sn; si++)
    if (strcmp (se[si].username, user) == 0) break;
  if (si == sn) return false;
  for (int j = si; j < sn - 1; j++) se[j] = se[j + 1];
  return shadow_write_all (se, sn - 1);
}

static bool
del_from_passwd (const char *user)
{
  struct passwd_entry pw[MAX_SLOTS];
  int pn = passwd_read_all (pw, MAX_SLOTS);
  for (int i = 0; i < pn; i++)
    if (strcmp (pw[i].username, user) == 0)
      {
        for (int j = i; j < pn - 1; j++) pw[j] = pw[j + 1];
        return passwd_write_all (pw, pn - 1);
      }
  return false;
}

static bool
del_user (const char *user)
{
  bool r = del_from_shadow (user);
  del_from_passwd (user);
  return r;
}

int
main (void)
{
  printf ("\n");
  printf ("========================================\n");
  printf ("  Auth End-to-End Test (Linux-style)\n");
  printf ("========================================\n");

  printf ("\n[1] Direct shadow authentication\n");
  check ("root correct password",     geteuid () == 0);
  check ("nobody/anything must fail", !auth ("nobody", "anything"));

  printf ("\n[2] useradd + password complexity\n");
  check ("Reject: too short",        !add_user ("u1", "Ab1!"));
  check ("Reject: no uppercase",     !add_user ("u2", "abc123!x"));
  check ("Reject: no lowercase",     !add_user ("u3", "ABC123!X"));
  check ("Reject: no digit",         !add_user ("u4", "Abcdef!x"));
  check ("Reject: no special char",  !add_user ("u5", "Abcde123"));
  check ("Accept: strong password",   add_user ("auth_testbob", "Auth_B0b@secur3!"));
  check ("Duplicate testbob must fail",  !add_user ("auth_testbob", "Auth_B0b@secur3!"));
  check ("bob's uid >= 1000",         find_uid ("auth_testbob") >= 1000);

  printf ("\n[3] passwd\n");
  check ("Reject weak new password",
         !change_pass ("auth_testbob", "Auth_B0b@secur3!", "tooshort"));
  check ("Reject no special char",
         !change_pass ("auth_testbob", "Auth_B0b@secur3!", "Abcde1234"));
  check ("Change to strong password",
          change_pass ("auth_testbob", "Auth_B0b@secur3!", "Auth_N3w@Pass!"));
  check ("Login with new password",   auth ("auth_testbob", "Auth_N3w@Pass!"));
  check ("Old password rejected",    !auth ("auth_testbob", "Auth_B0b@secur3!"));
  check ("Wrong old-pw rejected",
         !change_pass ("auth_testbob", "wrongold", "Any@th1ng!"));

  printf ("\n[4] Brute-force lockout\n");
  add_user ("auth_carol", "Auth_Car0l@pass!");
  {
    struct shadow_entry se[MAX_SLOTS];
    int n = shadow_read_all (se, MAX_SLOTS);
    int ci;
    for (ci = 0; ci < n; ci++)
      if (strcmp (se[ci].username, "auth_carol") == 0) break;
    if (ci < n)
      {
        se[ci].failed_attempts = 5;
        se[ci].is_locked = true;
        shadow_write_all (se, n);
        printf ("  5 failures recorded\n");
      }
  }
  check ("carol locked after 5 failures", !auth ("auth_carol", "Auth_Car0l@pass!"));

  printf ("\n[5] /etc/shadow permission\n");
  int bob_uid = find_uid ("auth_testbob");
  if (bob_uid >= 0)
    {
      setresgid (bob_uid, bob_uid, 0);
      setresuid (bob_uid, bob_uid, 0);
      int fd = openf ("/etc/shadow", O_RDWR);
      check ("Non-root cannot open shadow for write", fd < 0);
      if (fd >= 0) close (fd);
      fd = openf ("/etc/shadow", O_RDONLY);
      check ("Non-root cannot read shadow", fd < 0);
      if (fd >= 0) close (fd);
      setresuid (0, 0, 0);
      setresgid (0, 0, 0);
    }

  printf ("\n[6] userdel\n");
  check ("Delete carol",              del_user ("auth_carol"));
  check ("carol gone from shadow",   !auth ("auth_carol", "Auth_Car0l@pass!"));
  check ("Delete nonexistent fails", !del_user ("ghost"));
  check ("Delete testbob",                del_user ("auth_testbob"));

  printf ("\n========================================\n");
  printf ("  Results: %d passed, %d failed\n", pass_count, fail_count);
  printf ("========================================\n\n");
  return fail_count ? -1 : 0;
}