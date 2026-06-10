#ifndef USERPROG_MAC_H
#define USERPROG_MAC_H

#include <stdbool.h>
#include <stddef.h>

/* Forward declaration */
struct thread;

/* Mandatory Access Control (MAC) for Pintos
 * 
 * MAC provides program-based security through capability checking.
 * Different programs have different allowed capabilities.
 */

/* Security profile assigned to each program */
struct security_profile {
    const char *name;                   /* Profile name (e.g., "shell", "editor") */
    bool default_allow;                 /* Unused - kept for compatibility */
    
    /* Unused - kept for compatibility */
    const void *path_rules;
    size_t num_path_rules;
    
    /* Capability flags - what this program can do */
    bool allow_exec;        /* Can execute other programs */
    bool allow_useradd;     /* Can create users */
    bool allow_userdel;     /* Can delete users */
    bool allow_usermod;     /* Can modify users */
    bool allow_passwd;      /* Can change passwords */
    bool allow_sudo;        /* Can use sudo */
    bool allow_chmod;       /* Can change file permissions */
};

/* Initialize MAC system */
void mac_init(void);

/* Assign a security profile to a thread based on program name */
void mac_assign_profile(struct thread *t, const char *program_name);

/* Check if current profile allows file access (always returns true now) */
bool mac_check_file_access(const struct security_profile *profile, 
                           const char *path, 
                           int mode);

/* Check if current profile has a specific capability */
bool mac_check_capability(const struct security_profile *profile, 
                         const char *capability);

/* Admin profile for sudo escalation */
extern struct security_profile shell_admin_profile;

#endif /* userprog/mac.h */