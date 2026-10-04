#pragma once
#include <kernel/types.h>
struct trap_frame;
struct file;
/* Replace the current process image. argv/envp are kernel copies. */
int do_execve(const char *path, char *const argv[], char *const envp[], struct trap_frame *f);
