#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H
#include "threads/synch.h"

void syscall_init (void);
void exit (int);
void check_address(void *);
extern struct lock filesys_lock;
int getuid (void);
int geteuid (void);
#endif /* userprog/syscall.h */
