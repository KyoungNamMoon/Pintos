/* chmodtest.c — end-to-end test for chmod and file permission enforcement */
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

static int
add_to_passwd (const char *user)
{
  struct passwd_entry pw[MAX_SLOTS];
  int pn = passwd_read_all (pw, MAX_SLOTS);
  if (pn >= MAX_SLOTS) return -1;
  int max_uid = 999;
  for (int i = 0; i < pn; i++)
    if (pw[i].uid > max_uid) max_uid = pw[i].uid;
  struct passwd_entry *np = &pw[pn];
  memset (np, 0, sizeof *np);
  strlcpy (np->username, user, MAX_USERNAME_LEN);
  np->uid = max_uid + 1;
  np->gid = np->uid;
  snprintf (np->home_dir, MAX_HOME_LEN, "/home/%s", user);
  if (!passwd_write_all (pw, pn + 1)) return -1;
  return np->uid;
}

static int
add_user (const char *user, const char *pass)
{
  if (!password_complexity_ok (pass, NULL)) return -1;
  if (!add_to_shadow (user, pass)) return -1;
  return add_to_passwd (user);
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

static void
del_user (const char *user)
{
  del_from_shadow (user);
  del_from_passwd (user);
}

int
main (void)
{
  printf ("\n");
  printf ("========================================\n");
  printf ("  chmod End-to-End Test\n");
  printf ("========================================\n");

  /* ---- 1. Root creates a file and sets permissions ---- */
  printf ("\n[1] Root file creation and chmod\n");
  check ("Create testfile", create ("testfile", 0));

  int fd = openf ("testfile", O_RDWR);
  check ("Open testfile as root", fd >= 0);
  check ("Write as root", write (fd, "hello", 5) == 5);
  close (fd);

  check ("chmod 444 as root", chmod ("testfile", 0444));

  /* Root bypasses all permission checks (euid=0). */
  fd = openf ("testfile", O_RDWR);
  check ("Root open after chmod 444", fd >= 0);
  check ("Root write overrides permissions", write (fd, "root", 4) > 0);
  close (fd);

  /* ---- 2. Non-root permission enforcement ---- */
  printf ("\n[2] Non-root permission enforcement\n");

  int alice_uid = add_user ("chmod_alice", "Ch_Al1ce@pass!");
  check ("Add alice", alice_uid >= 0);

  create ("rootfile", 0);
  fd = openf ("rootfile", O_RDWR);
  write (fd, "data", 4);
  close (fd);
  chmod ("rootfile", 0644);

  /* Switch to alice */
  setresgid (alice_uid, alice_uid, 0);
  setresuid (alice_uid, alice_uid, 0);

  fd = open ("rootfile");
  check ("Non-root can open 644 file", fd >= 0);
  if (fd >= 0) close (fd);

  /* Switch back to root to chmod */
  setresuid (0, 0, 0);
  setresgid (0, 0, 0);
  chmod ("rootfile", 0600);

  /* Switch to alice again */
  setresgid (alice_uid, alice_uid, 0);
  setresuid (alice_uid, alice_uid, 0);

  fd = open ("rootfile");
  check ("Non-root blocked from 600 file", fd == -1);
  if (fd >= 0) close (fd);

/* ---- 3. Owner can chmod their own file ---- */
  printf ("\n[3] Owner chmod\n");
  /* Create home dir as root, then create file as alice */
  setresuid (0, 0, 0);
  setresgid (0, 0, 0);
  mkdir ("/home");
  mkdir ("/home/chmod_alice");
  chown ("/home/chmod_alice", alice_uid, alice_uid);
  /* Now switch to alice and create file */
  setresgid (alice_uid, alice_uid, 0);
  setresuid (alice_uid, alice_uid, 0);
  create ("/home/chmod_alice/alicefile", 0);
  int afd = openf ("/home/chmod_alice/alicefile", O_RDWR);
  if (afd >= 0) { write (afd, "chmod_alice data", 10); close (afd); }
  check ("Owner can chmod own file", chmod ("/home/chmod_alice/alicefile", 0600));

  /* ---- 4. Non-owner cannot chmod ---- */
  printf ("\n[4] Non-owner chmod rejected\n");
  setresuid (0, 0, 0);
  setresgid (0, 0, 0);
  int bob_uid = add_user ("chmod_bob", "Ch_B0b@secur3!");
  check ("Add bob", bob_uid >= 0);
  setresgid (bob_uid, bob_uid, 0);
  setresuid (bob_uid, bob_uid, 0);
  check ("Non-owner chmod rejected", !chmod ("/home/chmod_alice/alicefile", 0777));


  /* Cleanup — remove test users so login still works after test. */
  setresuid (0, 0, 0);
  setresgid (0, 0, 0);
  del_user ("chmod_alice");
  del_user ("chmod_bob");

  /* ---- Summary ---- */
  printf ("\n========================================\n");
  printf ("  Results: %d passed, %d failed\n", pass_count, fail_count);
  printf ("========================================\n\n");
  return fail_count ? -1 : 0;



}