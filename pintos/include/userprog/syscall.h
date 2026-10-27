#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <stdbool.h>
#include <debug.h>
#include "threads/synch.h"

struct thread;
/* x86-64에서 포인터 한 페이지 분량이며, 커널 스택에는 두지 않는다. */
#define FD_COUNT 512
extern struct lock filesys_lock;

void syscall_init (void);
void syscall_exit (int status) NO_RETURN;
bool syscall_files_init (void);
bool syscall_files_copy (struct thread *parent);
void syscall_files_close (void);

#endif /* userprog/syscall.h */
