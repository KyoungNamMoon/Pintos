#include "userprog/mac.h"
#include "threads/thread.h"
#include <string.h>
#include <stdio.h>

/* Forward declarations of security profiles */
static struct security_profile shell_profile;
static struct security_profile editor_profile;
static struct security_profile viewer_profile;
static struct security_profile user_mgmt_profile;
static struct security_profile chmod_profile;
static struct security_profile login_profile;
static struct security_profile sudo_profile;

/* ============================================================================
   PROFILE 1: Normal User Shell (shell, bash, sh)
   ============================================================================
 */
static struct security_profile shell_profile = {
    .name = "shell",
    .default_allow = true,
    .path_rules = NULL,
    .num_path_rules = 0,
    .allow_exec = true,
    .allow_useradd = false,
    .allow_userdel = false,
    .allow_usermod = false,
    .allow_passwd = true,
    .allow_sudo = true,
    .allow_chmod = false,
};

/* ============================================================================
   PROFILE 2: Administrator Shell (after sudo)
   ============================================================================
 */
struct security_profile shell_admin_profile = {
    .name = "shell_admin",
    .default_allow = true,
    .path_rules = NULL,
    .num_path_rules = 0,
    .allow_exec = true,
    .allow_useradd = true,
    .allow_userdel = true,
    .allow_usermod = true,
    .allow_passwd = true,
    .allow_sudo = true,
    .allow_chmod = true,
};

/* ============================================================================
   PROFILE 3: Read-Only Viewers (cat, less, more, head, tail, ls, whoami, who)
   ============================================================================
 */
static struct security_profile viewer_profile = {
    .name = "viewer",
    .default_allow = false,
    .path_rules = NULL,
    .num_path_rules = 0,
    .allow_exec = false,
    .allow_useradd = false,
    .allow_userdel = false,
    .allow_usermod = false,
    .allow_passwd = false,
    .allow_sudo = false,
    .allow_chmod = false,
};

/* ============================================================================
   PROFILE 4: User Management Tools (useradd, userdel, usermod, passwd)
   ============================================================================
 */
static struct security_profile user_mgmt_profile = {
    .name = "user_mgmt",
    .default_allow = false,
    .path_rules = NULL,
    .num_path_rules = 0,
    .allow_exec = false,
    .allow_useradd = true,
    .allow_userdel = true,
    .allow_usermod = true,
    .allow_passwd = true,
    .allow_sudo = false,
    .allow_chmod = true,
};

/* ============================================================================
   PROFILE 5: Permission Management Tool (chmod)
   ============================================================================
 */
static struct security_profile chmod_profile = {
    .name = "chmod",
    .default_allow = false,
    .path_rules = NULL,
    .num_path_rules = 0,
    .allow_exec = false,
    .allow_useradd = false,
    .allow_userdel = false,
    .allow_usermod = false,
    .allow_passwd = false,
    .allow_sudo = false,
    .allow_chmod = true,
};

/* ============================================================================
   PROFILE 6: Login Program
   ============================================================================
 */
static struct security_profile login_profile = {
    .name = "login",
    .default_allow = false,
    .path_rules = NULL,
    .num_path_rules = 0,
    .allow_exec = true,
    .allow_useradd = false,
    .allow_userdel = false,
    .allow_usermod = false,
    .allow_passwd = true,
    .allow_sudo = false,
    .allow_chmod = true,
};

/* ============================================================================
   PROFILE 7: Auth Program
   ============================================================================
 */
static struct security_profile auth_profile = {
    .name = "auth",
    .default_allow = false,
    .path_rules = NULL,
    .num_path_rules = 0,
    .allow_exec = true,
    .allow_useradd = false,
    .allow_userdel = false,
    .allow_usermod = false,
    .allow_passwd = false,
    .allow_sudo = false,
    .allow_chmod = false,
};

/* ============================================================================
   PROFILE 8: Sudo Program
   ============================================================================
 */
static struct security_profile sudo_profile = {
    .name = "sudo",
    .default_allow = false,
    .path_rules = NULL,
    .num_path_rules = 0,
    .allow_exec = true,
    .allow_useradd = false,
    .allow_userdel = false,
    .allow_usermod = false,
    .allow_passwd = false,
    .allow_sudo = true,
    .allow_chmod = false,
};


/* ============================================================================
   PROFILE 9: Su Program (switch user)
   ============================================================================
 */
static struct security_profile su_profile = {
    .name = "su",
    .default_allow = false,
    .path_rules = NULL,
    .num_path_rules = 0,
    .allow_exec = true,
    .allow_useradd = false,
    .allow_userdel = false,
    .allow_usermod = false,
    .allow_passwd = false,
    .allow_sudo = false,
    .allow_chmod = false,
};

/* ============================================================================
   MAC System Implementation
   ============================================================================ */

void mac_init(void){
    printf("MAC: Loaded 9 security profiles\n");
}

bool mac_check_file_access(const struct security_profile *profile, 
                           const char *path, 
                           int mode) {
    /* No file-based rules - always allow file access */
    /* Security is enforced through capabilities only */
    (void)profile;  /* Suppress unused warning */
    (void)path;     /* Suppress unused warning */
    (void)mode;     /* Suppress unused warning */
    return true;
}

bool mac_check_capability(const struct security_profile *profile, 
                         const char *capability) {
    if (profile == NULL)
        return true;
    
    if (strcmp(capability, "exec") == 0)
        return profile->allow_exec;
    else if (strcmp(capability, "useradd") == 0)
        return profile->allow_useradd;
    else if (strcmp(capability, "userdel") == 0)
        return profile->allow_userdel;
    else if (strcmp(capability, "usermod") == 0)
        return profile->allow_usermod;
    else if (strcmp(capability, "passwd") == 0)
        return profile->allow_passwd;
    else if (strcmp(capability, "sudo") == 0)
        return profile->allow_sudo;
    else if (strcmp(capability, "chmod") == 0)
        return profile->allow_chmod;
    
    /* Unknown capability: deny */
    return false;
}

void mac_assign_profile(struct thread *t, const char *program_name) {
    if (program_name == NULL) {
        t->mac_profile = NULL;
        return;
    }
    
    if (strcmp(program_name, "login") == 0) {
        t->mac_profile = &login_profile;
        return;
    }
    if (strcmp(program_name, "auth") == 0) {
        t->mac_profile = &auth_profile;
        return;
    }
    
    if (strcmp(program_name, "shell") == 0 || 
        strcmp(program_name, "sh") == 0 ||
        strcmp(program_name, "bash") == 0) {
        t->mac_profile = &shell_profile;
        return;
    }
    
    if (strcmp(program_name, "nano") == 0 ||
        strcmp(program_name, "vim") == 0 ||
        strcmp(program_name, "vi") == 0 ||
        strcmp(program_name, "ed") == 0) {
        t->mac_profile = &editor_profile;
        return;
    }
    
    if (strcmp(program_name, "cat") == 0 ||
        strcmp(program_name, "less") == 0 ||
        strcmp(program_name, "more") == 0 ||
        strcmp(program_name, "head") == 0 ||
        strcmp(program_name, "tail") == 0 ||
        strcmp(program_name, "ls") == 0 ||
        strcmp(program_name, "whoami") == 0 ||
        strcmp(program_name, "who") == 0 ||
        strcmp(program_name, "pwd") == 0) {
        t->mac_profile = &viewer_profile;
        return;
    }
    
    if (strcmp(program_name, "useradd") == 0 ||
        strcmp(program_name, "userdel") == 0 ||
        strcmp(program_name, "usermod") == 0 ||
        strcmp(program_name, "passwd") == 0 ||
        strcmp(program_name, "addgroup") == 0) {
        t->mac_profile = &user_mgmt_profile;
        return;
    }
    
    if (strcmp(program_name, "chmod") == 0 ||
        strcmp(program_name, "chmodtest") == 0) {
        t->mac_profile = &chmod_profile;
        return;
    }
    
    if (strcmp(program_name, "sudo") == 0) {
        t->mac_profile = &sudo_profile;
        return;
    }

    if (strcmp(program_name, "su") == 0) {
        t->mac_profile = &su_profile;
        return;
    }
    
    /* Unknown program: use restrictive viewer profile */
    t->mac_profile = &viewer_profile;
}