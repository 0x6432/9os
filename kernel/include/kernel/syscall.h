#pragma once
#include <kernel/types.h>
#include <kernel/vfs.h>
#include <kernel/process.h>
#include <kernel/mm.h>
#include <kernel/errno.h>
#include <arch/unistd.h>

struct file *fd_get(int fd);
int fd_alloc(struct file *f, int min, bool cloexec);
int fd_install(int fd, struct file *f, bool cloexec);
int fd_close(int fd);
/* Resolve a dirfd for *at() calls. Returns 0 and sets *base (nullptr = cwd) */
int dirfd_base(int dirfd, const char *path, struct inode **base);
int user_path(const char *upath, char *kpath);   /* copies into a 4096-byte buffer */
extern bool syscall_trace;
