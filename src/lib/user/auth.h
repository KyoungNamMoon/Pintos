#ifndef LIB_USER_AUTH_H
#define LIB_USER_AUTH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SHA256_HASH_SIZE 32
#define SALT_SIZE        16
#define MAX_USERNAME_LEN 32
#define MAX_HOME_LEN     64
#define MAX_SLOTS        16   /* max users; keep arrays stack-safe */
#define GROUP_PATH     "/etc/group"
#define GROUP_MAXSIZE  4096

/* In-memory representations — files are stored as text, not binary. */
struct passwd_entry {
    char username[MAX_USERNAME_LEN];
    int  uid;
    int  gid;
    char home_dir[MAX_HOME_LEN];
};

struct shadow_entry {
    char    username[MAX_USERNAME_LEN];
    uint8_t salt[SALT_SIZE];
    uint8_t hash[SHA256_HASH_SIZE];
    int     failed_attempts;
    bool    is_locked;
};

struct group_entry {
    char groupname[MAX_USERNAME_LEN];
    int  gid;
    char members[128];  /* comma-separated member list */
};

/* ── crypto ──────────────────────────────────────────────────────── */
void generate_salt          (uint8_t *salt_out);
void hash_password          (const char *password, const uint8_t *salt,
                             uint8_t *hash_out);
bool password_complexity_ok (const char *pw, char *reason_out);

/* ── /etc/passwd text I/O ────────────────────────────────────────── */
/* Format: username:x:uid:gid::home_dir:/bin/shell\n               */
int  passwd_read_all  (struct passwd_entry *out, int max);
bool passwd_write_all (const struct passwd_entry *entries, int count);
bool passwd_lookup_uid (int uid, char *name_out, int size);

/* ── /etc/shadow text I/O ────────────────────────────────────────── */
/* Format: username:$5$<salt_hex>$<hash_hex>:failed:locked\n        */
int  shadow_read_all  (struct shadow_entry *out, int max);
bool shadow_write_all (const struct shadow_entry *entries, int count);

int  group_read_all  (struct group_entry *out, int max);
bool group_write_all (const struct group_entry *entries, int count);
bool group_lookup_gid (int gid, char *name_out, int size);

#endif /* LIB_USER_AUTH_H */
