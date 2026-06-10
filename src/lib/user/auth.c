#include "auth.h"
#include "syscall.h"
#include <random.h>
#include <string.h>
#include <stdio.h>
#include "../sha256.h"

#define PASSWD_PATH    "/etc/passwd"
#define SHADOW_PATH    "/etc/shadow"
#define PASSWD_MAXSIZE 4096
#define SHADOW_MAXSIZE 8192

/* ── hex helpers ─────────────────────────────────────────────────── */

static void
hex_encode (const uint8_t *src, int len, char *dst)
{
  static const char h[] = "0123456789abcdef";
  for (int i = 0; i < len; i++)
    {
      dst[i * 2]     = h[src[i] >> 4];
      dst[i * 2 + 1] = h[src[i] & 0xf];
    }
  dst[len * 2] = '\0';
}

static bool
hex_decode (const char *src, uint8_t *dst, int len)
{
  for (int i = 0; i < len; i++)
    {
      int hi, lo;
      char c;
      c = src[i * 2];
      if      (c >= '0' && c <= '9') hi = c - '0';
      else if (c >= 'a' && c <= 'f') hi = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') hi = c - 'A' + 10;
      else return false;
      c = src[i * 2 + 1];
      if      (c >= '0' && c <= '9') lo = c - '0';
      else if (c >= 'a' && c <= 'f') lo = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') lo = c - 'A' + 10;
      else return false;
      dst[i] = (uint8_t) ((hi << 4) | lo);
    }
  return true;
}

/* ── string helpers ──────────────────────────────────────────────── */

static int
parse_int (const char *s)
{
  int n = 0;
  while (*s >= '0' && *s <= '9')
    n = n * 10 + (*s++ - '0');
  return n;
}

/* Reads field up to ':', '\n', or NUL; advances *p past the ':'. */
static void
next_field (const char **p, char *out, int max)
{
  const char *s = *p;
  int i = 0;
  while (*s && *s != ':' && *s != '\n' && i < max - 1)
    out[i++] = *s++;
  out[i] = '\0';
  if (*s == ':') s++;
  *p = s;
}

/* ── low-level file I/O ──────────────────────────────────────────── */

/* Read one text line. Returns byte count; 0 = EOF or zero-padding. */
static int
read_line (int fd, char *buf, int max)
{
  int i = 0;
  char c;
  while (i < max - 1)
    {
      if (read (fd, &c, 1) != 1 || c == '\0')
        break;
      buf[i++] = c;
      if (c == '\n') break;
    }
  buf[i] = '\0';
  return i;
}

/* Zero-pad the rest of the file up to file_size. */
static void
zero_pad (int fd, int file_size)
{
  char zero[64];
  memset (zero, 0, sizeof zero);
  for (int rem = file_size; rem > 0; )
    {
      int chunk = rem < 64 ? rem : 64;
      write (fd, zero, chunk);
      rem -= chunk;
    }
}

/* ── /etc/passwd ─────────────────────────────────────────────────── */

static bool
parse_passwd_line (const char *line, struct passwd_entry *e)
{
  const char *p = line;
  char tmp[16];

  next_field (&p, e->username, MAX_USERNAME_LEN);
  if (e->username[0] == '\0' || e->username[0] == '#') return false;
  next_field (&p, tmp, sizeof tmp);          /* 'x'  */
  next_field (&p, tmp, sizeof tmp);          /* uid  */
  e->uid = parse_int (tmp);
  next_field (&p, tmp, sizeof tmp);          /* gid  */
  e->gid = parse_int (tmp);
  next_field (&p, tmp, sizeof tmp);          /* gecos (skip) */
  next_field (&p, e->home_dir, MAX_HOME_LEN);
  return true;
}

int
passwd_read_all (struct passwd_entry *out, int max)
{
  int fd = openf (PASSWD_PATH, O_RDONLY);
  if (fd < 0) return 0;
  char line[160];
  int count = 0;
  while (count < max)
    {
      if (read_line (fd, line, sizeof line) == 0) break;
      if (line[0] == '\n') continue;
      if (parse_passwd_line (line, &out[count]))
        count++;
    }
  close (fd);
  return count;
}

bool
passwd_write_all (const struct passwd_entry *e, int count)
{
  int fd = openf (PASSWD_PATH, O_RDWR);
  if (fd < 0) return false;
  seek (fd, 0);
  int written = 0;
  char line[160];
  for (int i = 0; i < count; i++)
    {
      int n = snprintf (line, sizeof line, "%s:x:%d:%d::%s:/bin/shell\n",
                        e[i].username, e[i].uid, e[i].gid, e[i].home_dir);
      write (fd, line, n);
      written += n;
    }
  zero_pad (fd, PASSWD_MAXSIZE - written);
  close (fd);
  return true;
}

bool
passwd_lookup_uid (int uid, char *name_out, int size)
{
  struct passwd_entry pw[MAX_SLOTS];
  int n = passwd_read_all (pw, MAX_SLOTS);
  for (int i = 0; i < n; i++)
    if (pw[i].uid == uid)
      {
        strlcpy (name_out, pw[i].username, size);
        return true;
      }
  return false;
}

/* ── /etc/shadow ─────────────────────────────────────────────────── */
/* Line format: username:$5$<32hex>$<64hex>:failed:locked           */

static bool
parse_shadow_line (const char *line, struct shadow_entry *e)
{
  const char *p = line;
  char hash_field[128], tmp[8];

  next_field (&p, e->username, MAX_USERNAME_LEN);
  if (e->username[0] == '\0' || e->username[0] == '#') return false;
  next_field (&p, hash_field, sizeof hash_field);
  next_field (&p, tmp, sizeof tmp);
  e->failed_attempts = parse_int (tmp);
  next_field (&p, tmp, sizeof tmp);
  e->is_locked = (tmp[0] == '1');

  /* hash_field: $5$<SALT_SIZE*2 hex>$<SHA256_HASH_SIZE*2 hex> */
  if (hash_field[0] != '$' || hash_field[1] != '5' || hash_field[2] != '$')
    return false;
  const char *s = hash_field + 3;
  if (!hex_decode (s, e->salt, SALT_SIZE)) return false;
  s += SALT_SIZE * 2;
  if (*s != '$') return false;
  s++;
  if (!hex_decode (s, e->hash, SHA256_HASH_SIZE)) return false;
  return true;
}

int
shadow_read_all (struct shadow_entry *out, int max)
{
  int fd = openf (SHADOW_PATH, O_RDONLY);
  if (fd < 0) return 0;
  char line[192];
  int count = 0;
  while (count < max)
    {
      if (read_line (fd, line, sizeof line) == 0) break;
      if (line[0] == '\n') continue;
      if (parse_shadow_line (line, &out[count]))
        count++;
    }
  close (fd);
  return count;
}

bool
shadow_write_all (const struct shadow_entry *e, int count)
{
  int fd = openf (SHADOW_PATH, O_RDWR);
  if (fd < 0) return false;
  seek (fd, 0);
  int written = 0;
  char salt_hex[SALT_SIZE * 2 + 1];
  char hash_hex[SHA256_HASH_SIZE * 2 + 1];
  char line[192];
  for (int i = 0; i < count; i++)
    {
      hex_encode (e[i].salt, SALT_SIZE, salt_hex);
      hex_encode (e[i].hash, SHA256_HASH_SIZE, hash_hex);
      int n = snprintf (line, sizeof line, "%s:$5$%s$%s:%d:%d\n",
                        e[i].username, salt_hex, hash_hex,
                        e[i].failed_attempts, e[i].is_locked ? 1 : 0);
      write (fd, line, n);
      written += n;
    }
  zero_pad (fd, SHADOW_MAXSIZE - written);
  close (fd);
  return true;
}

/* ── crypto ──────────────────────────────────────────────────────── */

void
generate_salt (uint8_t *salt_out)
{
  random_bytes (salt_out, SALT_SIZE);
}

void
hash_password (const char *password, const uint8_t *salt, uint8_t *hash_out)
{
  struct sha256_buff ctx;
  sha256_init (&ctx);
  sha256_update (&ctx, salt, SALT_SIZE);
  sha256_update (&ctx, password, strlen (password));
  sha256_finalize (&ctx);
  sha256_read (&ctx, hash_out);
}

bool
password_complexity_ok (const char *pw, char *reason_out)
{
  if (pw == NULL)
    {
      if (reason_out) strlcpy (reason_out, "NULL password", 64);
      return false;
    }
  size_t len = strlen (pw);
  bool has_upper = false, has_lower = false;
  bool has_digit = false, has_special = false;

  for (size_t i = 0; i < len; i++)
    {
      char c = pw[i];
      if      (c >= 'A' && c <= 'Z') has_upper  = true;
      else if (c >= 'a' && c <= 'z') has_lower  = true;
      else if (c >= '0' && c <= '9') has_digit  = true;
      else                           has_special = true;
    }

  if (len < 8)
    { if (reason_out) strlcpy (reason_out, "too short (min 8 chars)", 64);    return false; }
  if (!has_upper)
    { if (reason_out) strlcpy (reason_out, "needs uppercase letter", 64);     return false; }
  if (!has_lower)
    { if (reason_out) strlcpy (reason_out, "needs lowercase letter", 64);     return false; }
  if (!has_digit)
    { if (reason_out) strlcpy (reason_out, "needs a digit", 64);              return false; }
  if (!has_special)
    { if (reason_out) strlcpy (reason_out, "needs a special character", 64);  return false; }
  return true;
}

/* ── /etc/group ──────────────────────────────────────────────────── */

static bool
parse_group_line (const char *line, struct group_entry *e)
{
  const char *p = line;
  char tmp[16];
  next_field (&p, e->groupname, MAX_USERNAME_LEN);
  if (e->groupname[0] == '\0' || e->groupname[0] == '#') return false;
  next_field (&p, tmp, sizeof tmp);   /* 'x' */
  next_field (&p, tmp, sizeof tmp);   /* gid */
  e->gid = parse_int (tmp);
  next_field (&p, e->members, sizeof e->members);
  return true;
}

int
group_read_all (struct group_entry *out, int max)
{
  int fd = openf (GROUP_PATH, O_RDONLY);
  if (fd < 0) return 0;
  char line[192];
  int count = 0;
  while (count < max)
    {
      if (read_line (fd, line, sizeof line) == 0) break;
      if (line[0] == '\n') continue;
      if (parse_group_line (line, &out[count]))
        count++;
    }
  close (fd);
  return count;
}

bool
group_write_all (const struct group_entry *e, int count)
{
  int fd = openf (GROUP_PATH, O_RDWR);
  if (fd < 0) return false;
  seek (fd, 0);
  int written = 0;
  char line[192];
  for (int i = 0; i < count; i++)
    {
      int n = snprintf (line, sizeof line, "%s:x:%d:%s\n",
                        e[i].groupname, e[i].gid, e[i].members);
      write (fd, line, n);
      written += n;
    }
  zero_pad (fd, GROUP_MAXSIZE - written);
  close (fd);
  return true;
}

bool
group_lookup_gid (int gid, char *name_out, int size)
{
  struct group_entry ge[MAX_SLOTS];
  int n = group_read_all (ge, MAX_SLOTS);
  for (int i = 0; i < n; i++)
    if (ge[i].gid == gid)
      {
        strlcpy (name_out, ge[i].groupname, size);
        return true;
      }
  return false;
}
