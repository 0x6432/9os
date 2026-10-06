#pragma once
#include <kernel/types.h>
#include <kernel/vfs.h>
#include <kernel/process.h>
#include <kernel/mm.h>
#include <kernel/errno.h>
#include <arch/unistd.h>

struct file *fd_get(int fd);
struct file *fd_get_ref(int fd);
void fd_borrow_release(void);      /* drop this syscall's fd_get() pins */
bool fd_cloexec_get(int fd);
int fd_cloexec_set(int fd, bool v);
struct process;
struct file *fd_slot_set(struct process *p, int fd, struct file *f);
int fd_alloc(struct file *f, int min, bool cloexec);
int fd_install(int fd, struct file *f, bool cloexec);
int fd_close(int fd);
/* Resolve a dirfd for *at() calls. Returns 0 and sets *base (nullptr = cwd) */
int dirfd_base(int dirfd, const char *path, struct inode **base);
int user_path(const char *upath, char *kpath);   /* copies into a 4096-byte buffer */
extern bool syscall_trace;
