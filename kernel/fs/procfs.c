#include <kernel/irq.h>
#include <kernel/uaccess.h>
#include <kernel/mutex.h>
/* procfs: process and system information (enough for BusyBox ps/top/free/mount). */
#include <kernel/vfs.h>
int blk_proc_partitions(char *buf, size_t max);
int blk_proc_diskstats(char *buf, size_t max);
#include <kernel/process.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/time.h>
#include <kernel/pmm.h>
#include <kernel/slab.h>
#include <kernel/mm.h>
#include <kernel/tty.h>
#include <kernel/sched.h>
#include <arch/syscall.h>

enum pkind { P_ROOT, P_SELF, P_PIDDIR, P_FDDIR, P_FD, P_FILE, P_CWD, P_EXE };
enum pfile { F_STAT, F_STATUS, F_CMDLINE, F_COMM, F_ENVIRON, F_MAPS,
             G_MEMINFO, G_UPTIME, G_VERSION, G_CPUINFO, G_MOUNTS, G_LOADAVG, G_STAT, G_FILESYSTEMS, G_SCHED, G_VMSTAT, G_LOCKDEP, G_HARDEN, G_INTERRUPTS, G_PARTITIONS, G_DISKSTATS };
struct pinfo { enum pkind kind; int pid; int fd; enum pfile file; };

static struct thread *main_thread(struct process *p) {
    return list_empty(&p->threads) ? nullptr : list_first(&p->threads, struct thread, proc_node);
}
static int main_nice(struct process *p) { struct thread *t = main_thread(p); return t ? t->nice : 0; }
static int main_prio(struct process *p) {      /* /proc stat 'priority': 20+nice, or -1-rt_prio */
    struct thread *t = main_thread(p);
    if (!t) return 20;
    return t->policy == 1 || t->policy == 2 ? -1 - t->rt_prio : 20 + t->nice;
}
static int nthreads(struct process *p) {
    int n = 0;
    list_for_each(it, &p->threads) n++;
    return n ? n : 1;
}

static const struct inode_ops proc_iops;
static const struct file_ops proc_file_fops, proc_dir_fops;
static struct inode *proc_root_inode;

static struct inode *pnew(uint32_t mode, enum pkind kind, int pid, int fd, enum pfile file) {
    struct inode *i = inode_alloc(mode);
    struct pinfo *pi = kmalloc(sizeof *pi);
    *pi = (struct pinfo){ kind, pid, fd, file };
    i->priv = pi;
    i->iops = &proc_iops;
    i->fops = S_ISDIR(mode) ? &proc_dir_fops : &proc_file_fops;
    i->dev = 3;
    i->ino = (kind == P_ROOT) ? 1 : ((uint64_t)pid << 16) | (kind << 8) | (file + fd + 1);
    return i;
}

static const struct { const char *name; enum pfile f; } global_files[] = {
    { "meminfo", G_MEMINFO }, { "uptime", G_UPTIME }, { "version", G_VERSION }, { "cpuinfo", G_CPUINFO },
    { "mounts", G_MOUNTS }, { "loadavg", G_LOADAVG }, { "stat", G_STAT }, { "filesystems", G_FILESYSTEMS }, { "sched", G_SCHED },
    { "vmstat", G_VMSTAT }, { "lockdep", G_LOCKDEP }, { "hardening", G_HARDEN }, { "interrupts", G_INTERRUPTS },
    { "partitions", G_PARTITIONS }, { "diskstats", G_DISKSTATS },
};
static const struct { const char *name; enum pfile f; } pid_files[] = {
    { "stat", F_STAT }, { "status", F_STATUS }, { "cmdline", F_CMDLINE }, { "comm", F_COMM },
    { "environ", F_ENVIRON }, { "maps", F_MAPS },
};

static int p_lookup(struct inode *dir, const char *name, struct inode **out) {
    struct pinfo *pi = dir->priv;
    if (pi->kind == P_ROOT) {
        if (!strcmp(name, "self")) { *out = pnew(S_IFLNK | 0777, P_SELF, 0, 0, 0); return 0; }
        for (size_t k = 0; k < ARRAY_SIZE(global_files); k++)
            if (!strcmp(name, global_files[k].name)) { *out = pnew(S_IFREG | 0444, P_FILE, 0, 0, global_files[k].f); return 0; }
        int pid = 0;
        for (const char *c = name; *c; c++) { if (*c < '0' || *c > '9') return -ENOENT; pid = pid * 10 + (*c - '0'); }
        if (!process_find(pid)) return -ENOENT;
        struct inode *i = pnew(S_IFDIR | 0555, P_PIDDIR, pid, 0, 0);
        i->parent = dir;
        *out = i;
        return 0;
    }
    if (pi->kind == P_PIDDIR) {
        if (!process_find(pi->pid)) return -ENOENT;
        if (!strcmp(name, "fd")) { struct inode *i = pnew(S_IFDIR | 0500, P_FDDIR, pi->pid, 0, 0); i->parent = dir; *out = i; return 0; }
        if (!strcmp(name, "cwd")) { *out = pnew(S_IFLNK | 0777, P_CWD, pi->pid, 0, 0); return 0; }
        if (!strcmp(name, "exe")) { *out = pnew(S_IFLNK | 0777, P_EXE, pi->pid, 0, 0); return 0; }
        for (size_t k = 0; k < ARRAY_SIZE(pid_files); k++)
            if (!strcmp(name, pid_files[k].name)) { *out = pnew(S_IFREG | 0444, P_FILE, pi->pid, 0, pid_files[k].f); return 0; }
        return -ENOENT;
    }
    if (pi->kind == P_FDDIR) {
        struct process *p = process_find(pi->pid);
        int fd = 0;
        for (const char *c = name; *c; c++) { if (*c < '0' || *c > '9') return -ENOENT; fd = fd * 10 + (*c - '0'); }
        if (!p || fd >= MAX_FDS || !p->fds[fd]) return -ENOENT;
        *out = pnew(S_IFLNK | 0700, P_FD, pi->pid, fd, 0);
        return 0;
    }
    return -ENOTDIR;
}

/* another process's fd slot, referenced (its threads may close it without the BKL) */
static struct file *proc_fd_ref(struct process *p, int fd) {
    if (fd < 0 || fd >= MAX_FDS) return nullptr;
    uint64_t fl = spin_lock_irqsave(&p->fd_lock);
    struct file *f = p->fds[fd];
    if (f) file_get(f);
    spin_unlock_irqrestore(&p->fd_lock, fl);
    return f;
}

static int p_follow(struct inode *i, struct inode **out) {
    struct pinfo *pi = i->priv;
    if (pi->kind == P_SELF) {
        char n[16];
        snprintf(n, sizeof n, "%d", curproc ? curproc->pid : 1);
        return p_lookup(proc_root_inode, n, out);
    }
    struct process *p = process_find(pi->pid);
    if (!p) return -ENOENT;
    if (pi->kind == P_FD) {
        struct file *f = proc_fd_ref(p, pi->fd);
        if (!f) return -ENOENT;
        iget(f->inode);
        *out = f->inode;
        vfs_close(f);
        return 0;
    }
    if (pi->kind == P_CWD) { if (!p->cwd) return -ENOENT; iget(p->cwd); *out = p->cwd; return 0; }
    if (pi->kind == P_EXE) return p->exe ? vfs_lookup(p->exe, true, out) : -ENOENT;
    return -EINVAL;
}

static int p_readlink(struct inode *i, char *buf, size_t size) {
    struct pinfo *pi = i->priv;
    char tmp[256];
    if (pi->kind == P_SELF) snprintf(tmp, sizeof tmp, "%d", curproc ? curproc->pid : 1);
    else {
        struct process *p = process_find(pi->pid);
        if (!p) return -ENOENT;
        if (pi->kind == P_FD) {
            struct file *f = proc_fd_ref(p, pi->fd);
            if (!f) return -ENOENT;
            if (S_ISFIFO(f->inode->mode)) snprintf(tmp, sizeof tmp, "pipe:[%lu]", f->inode->ino);
            else snprintf(tmp, sizeof tmp, "%s", f->path ? f->path : "anon_inode:[unknown]");
            vfs_close(f);
        } else if (pi->kind == P_CWD) {
            int r = vfs_getcwd(p->cwd, tmp, sizeof tmp);
            if (r < 0) return r;
        } else snprintf(tmp, sizeof tmp, "%s", p->exe ? p->exe : "");
    }
    size_t l = MIN(strlen(tmp), size);
    memcpy(buf, tmp, l);
    return (int)l;
}

struct iter_ctx { uint64_t idx; uint64_t *pos; filldir_t fill; void *ctx; bool stop; };
static bool emit(struct iter_ctx *c, const char *name, uint64_t ino, unsigned type) {
    if (c->stop) return false;
    if (c->idx++ < *c->pos) return true;
    if (c->fill(c->ctx, name, strlen(name), ino, type)) { c->stop = true; return false; }
    (*c->pos)++;
    return true;
}
static void emit_pid(struct process *p, void *ctx) {
    char n[16];
    snprintf(n, sizeof n, "%d", p->pid);
    emit(ctx, n, (uint64_t)p->pid << 16, 4);
}

static int p_iterate(struct inode *dir, uint64_t *pos, filldir_t fill, void *ctx) {
    struct pinfo *pi = dir->priv;
    struct iter_ctx c = { 0, pos, fill, ctx, false };
    emit(&c, ".", dir->ino, 4);
    emit(&c, "..", dir->parent ? dir->parent->ino : dir->ino, 4);
    if (pi->kind == P_ROOT) {
        emit(&c, "self", 2, 10);
        for (size_t k = 0; k < ARRAY_SIZE(global_files); k++) emit(&c, global_files[k].name, 100 + k, 8);
        process_list(emit_pid, &c);
    } else if (pi->kind == P_PIDDIR) {
        emit(&c, "fd", 3, 4); emit(&c, "cwd", 4, 10); emit(&c, "exe", 5, 10);
        for (size_t k = 0; k < ARRAY_SIZE(pid_files); k++) emit(&c, pid_files[k].name, 10 + k, 8);
    } else if (pi->kind == P_FDDIR) {
        struct process *p = process_find(pi->pid);
        for (int fd = 0; p && fd < MAX_FDS; fd++) {
            if (!p->fds[fd]) continue;
            char n[8];
            snprintf(n, sizeof n, "%d", fd);
            emit(&c, n, 1000 + fd, 10);
        }
    }
    return 0;
}

static void p_evict(struct inode *i) { kfree(i->priv); }

/* ---- file contents ---- */
struct buf { char *data; size_t len, cap; };
static void bprintf(struct buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void bprintf(struct buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[512];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof tmp - 1) n = sizeof tmp - 1;
    if (b->len + n + 1 > b->cap) { b->cap = (b->len + n + 1) * 2; b->data = krealloc(b->data, b->cap); }
    memcpy(b->data + b->len, tmp, n);
    b->len += n;
}

static char pstate(struct process *p) {
    if (p->state == P_ZOMBIE) return 'Z';
    if (p->stopped) return 'T';
    list_for_each(it, &p->threads) {
        struct thread *t = list_entry(it, struct thread, proc_node);
        if (t->state == T_RUNNING || t->state == T_RUNNABLE) return 'R';
    }
    return 'S';
}

extern uint64_t pagecache_pages, pagecache_lru_pages, pagecache_filled, pagecache_reclaimed;
static uint64_t vm_size(struct process *p) {
    uint64_t s = 0;
    if (!p->mm) return 0;
    mm_lock(p->mm);
    list_for_each(it, &p->mm->vmas) { struct vma *v = list_entry(it, struct vma, node); s += v->end - v->start; }
    mm_unlock(p->mm);
    return s;
}
static uint64_t vm_rss(struct process *p) { return p->mm ? (uint64_t)__atomic_load_n(&p->mm->rss, __ATOMIC_RELAXED) : 0; }

static void sched_thread_line(struct thread *t, void *arg) {
    static const char st[] = "RXBSZ";
    bprintf(arg, "thread %d %s %c cpu %d rq %d syscall %lu bkl %d/%d\n", t->tid, t->name,
            t->state <= T_ZOMBIE ? st[t->state] : '?', t->cpu ? t->cpu->id : -1, t->rq_cpu,
            t->last_syscall, t->bkl_depth, t->bkl_saved);
}

static void gen(struct pinfo *pi, struct buf *b) {
    uint64_t freep, totalp;
    pmm_stats(&freep, &totalp);
    if (pi->kind != P_FILE) return;
    struct process *p = pi->pid ? process_find(pi->pid) : nullptr;
    if (pi->pid && !p) return;
    switch (pi->file) {
    case G_MEMINFO:
        bprintf(b, "MemTotal:       %8lu kB\nMemFree:        %8lu kB\nMemAvailable:   %8lu kB\n"
                   "Buffers:               0 kB\nCached:         %8lu kB\nSwapTotal:             0 kB\nSwapFree:              0 kB\n",
                totalp * 4, freep * 4, (freep + pagecache_lru_pages) * 4, pagecache_pages * 4);
        break;
    case G_UPTIME: { uint64_t ms = time_ns() / 1000000; bprintf(b, "%lu.%02lu %lu.%02lu\n", ms / 1000, (ms % 1000) / 10, ms / 1000, (ms % 1000) / 10); break; }
    case G_VERSION: bprintf(b, "Linux version 6.1.0-9os (9os hobby kernel) #1\n"); break;
    case G_CPUINFO:
        for (int i = 0; i < ncpus; i++)
            bprintf(b, "processor\t: %d\nvendor_id\t: 9os\nmodel name\t: 9os virtual CPU (%s)\nhwid\t\t: 0x%lx\nflags\t\t: fpu sse sse2\n\n",
                    i, ARCH_PLATFORM, cpus[i].hwid);
        break;
    case G_MOUNTS: case G_FILESYSTEMS: case G_PARTITIONS: case G_DISKSTATS: {
        char *t = kmalloc(8192);
        if (t) {
            int n = pi->file == G_MOUNTS ? vfs_proc_mounts(t, 8192) : pi->file == G_FILESYSTEMS ? vfs_proc_filesystems(t, 8192)
                  : pi->file == G_PARTITIONS ? blk_proc_partitions(t, 8192) : blk_proc_diskstats(t, 8192);
            if (b->len + n + 1 > b->cap) { b->cap = b->len + n + 1; b->data = krealloc(b->data, b->cap); }
            memcpy(b->data + b->len, t, n);
            b->len += n;
            kfree(t);
        }
        break;
    }
    case G_LOADAVG: bprintf(b, "0.00 0.00 0.00 %d/%d 1\n", sched_runnable_count() + 1, sched_runnable_count() + 1); break;
    case G_STAT: {
        uint64_t tu = 0, ts = 0, ti = 0, cs = 0;
        for (int i = 0; i < ncpus; i++) { tu += cpus[i].user_ticks; ts += cpus[i].sys_ticks; ti += sched_idle_ticks(i); cs += cpus[i].ctx_switches; }
        bprintf(b, "cpu  %lu 0 %lu %lu 0 0 0 0 0 0\n", tu / 10, ts / 10, ti / 10);
        for (int i = 0; i < ncpus; i++)
            bprintf(b, "cpu%d %lu 0 %lu %lu 0 0 0 0 0 0\n", i, cpus[i].user_ticks / 10, cpus[i].sys_ticks / 10, sched_idle_ticks(i) / 10);
        bprintf(b, "ctxt %lu\nbtime %ld\nprocs_running %d\n", cs, (long)boot_epoch, sched_runnable_count() + 1);
        break;
    }
    case G_VMSTAT: {
        uint64_t fr, tot; pmm_stats(&fr, &tot);
        bprintf(b, "nr_free_pages %lu\nnr_total_pages %lu\ncow_shared %lu\ncow_copied %lu\ncow_reused %lu\n",
                fr, tot, cow_stats.shared, cow_stats.copied, cow_stats.reused);
        extern uint64_t pc_stats_mapped, pc_stats_exec;
        bprintf(b, "pagecache_private_mapped %lu\npagecache_exec_mapped %lu\n", pc_stats_mapped, pc_stats_exec);
        bprintf(b, "nr_pagecache %lu\nnr_pagecache_lru %lu\npagecache_filled %lu\npagecache_reclaimed %lu\n",
                pagecache_pages, pagecache_lru_pages, pagecache_filled, pagecache_reclaimed);
        bprintf(b, "pgfault_file %lu\npgfault_anon %lu\npgfault_zero_eof %lu\npgfault_cow %lu\noom_retries %lu\noom_kill %lu\n"
                   "pgscan %lu\npgsteal %lu\nreclaim_runs %lu\nkswapd_wakeups %lu\nrmap_unmapped %lu\n"
                   "madvise_zapped %lu\nmremap_moved %lu\npopulated %lu\nuser_copy_slowpath %lu\n",
                vm_stats.file_faults, vm_stats.anon_faults, vm_stats.zero_eof_faults, vm_stats.cow_faults,
                vm_stats.oom_retries, vm_stats.oom_kills, vm_stats.reclaim_scanned, vm_stats.reclaim_freed,
                vm_stats.reclaim_runs, vm_stats.kswapd_wakeups, vm_stats.rmap_unmapped, vm_stats.madv_zapped,
                vm_stats.mremap_moved, vm_stats.populated, vm_stats.copy_slowpath);
        struct pmm_cache_stats ps; pmm_cache_stats(&ps);
        bprintf(b, "pmm_pcpu_cached %lu\npmm_pcpu_alloc_hits %lu\npmm_pcpu_free_hits %lu\npmm_pcpu_drained %lu\n",
                ps.cached_pages, ps.alloc_hits, ps.free_hits, ps.drained_pages);
        struct slab_cpu_stats ss; slab_cpu_stats(&ss);
        bprintf(b, "slab_pcpu_cached %lu\nslab_pcpu_alloc_hits %lu\nslab_pcpu_free_hits %lu\nslab_pcpu_drained %lu\n",
                ss.cached_objects, ss.alloc_hits, ss.free_hits, ss.drained_objects);
        break;
    }
    case G_HARDEN: {
        extern uintptr_t __stack_chk_guard;
        bprintf(b, "features %s\nstack_protector strong\nstack_guard_random %d\nrandomize_va_space %d\n"
                   "wx_policy %s\nkernel_wx_pages %lu\nwx_mappings %lu\nwx_denied %lu\nextable_fixups %lu\n"
                   "uaccess_violations %lu\n",
                arch_harden_features(), __stack_chk_guard != 0x595e9fbd94fda766ULL, randomize_va_space,
                wx_policy == 2 ? "strict" : wx_policy ? "warn" : "off", harden_stats.kernel_wx_pages,
                harden_stats.wx_mappings, harden_stats.wx_denied, harden_stats.extable_fixups,
                harden_stats.uaccess_violations);
        break;
    }
    case G_INTERRUPTS: {
        char *t = kmalloc(8192);
        if (t) {
            int n = irq_proc_read(t, 8192);
            if (b->len + n + 1 > b->cap) { b->cap = b->len + n + 1; b->data = krealloc(b->data, b->cap); }
            memcpy(b->data + b->len, t, n);
            b->len += n;
            kfree(t);
        }
        break;
    }
    case G_LOCKDEP: {
        char *t = kmalloc(4096);
        if (t) { int n = lockdep_report(t, 4096); bprintf(b, "%.*s", n, t); kfree(t); }
        break;
    }
    case G_SCHED:
        bprintf(b, "policy: %s\ncpus: %d\nrunnable: %d\n", sched_policy_name(), ncpus, sched_runnable_count());
        for (int i = 0; i < ncpus; i++)
            bprintf(b, "cpu%d: hwid 0x%lx ticks %lu idle %lu switches %lu rq %d steals %lu balances %lu local %lu coordinated %lu nohz %lu running %s\n", i, cpus[i].hwid,
                    cpus[i].ticks, sched_idle_ticks(i), cpus[i].ctx_switches, sched_rq_len(i), sched_rq_steals(i),
                    sched_rq_balances(i), sched_rq_local(i), sched_rq_coordinated(i), cpus[i].idle_sleeps,
                    cpus[i].cur ? cpus[i].cur->name : "-");
        sched_for_each_thread(sched_thread_line, b);
        break;
    case F_STAT:
        bprintf(b, "%d (%s) %c %d %d %d %d %d 4194304 %lu %lu 0 0 %lu %lu %lu %lu %d %d %d 0 %lu %lu %lu\n",
                p->pid, p->name, pstate(p), p->parent ? p->parent->pid : 0, p->pgid, p->sid,
                p->ctty ? 0x0501 : 0, p->ctty ? p->ctty->pgrp : -1, p->min_flt, p->cmin_flt,
                p->utime_ns / 10000000, p->stime_ns / 10000000, p->cutime_ns / 10000000, p->cstime_ns / 10000000,
                main_prio(p), main_nice(p), nthreads(p), p->start_ticks / 10, vm_size(p), vm_rss(p));
        break;
    case F_STATUS:
        bprintf(b, "Name:\t%s\nState:\t%c\nTgid:\t%d\nPid:\t%d\nPPid:\t%d\nUid:\t%u\t%u\t%u\t%u\nGid:\t%u\t%u\t%u\t%u\nVmSize:\t%8lu kB\nVmLck:\t%8lu kB\nVmRSS:\t%8lu kB\nThreads:\t%d\n"
                   "voluntary_ctxt_switches:\t%lu\nnonvoluntary_ctxt_switches:\t%lu\n",
                p->name, pstate(p), p->pid, p->pid, p->parent ? p->parent->pid : 0,
                p->uid, p->euid, p->euid, p->euid, p->gid, p->egid, p->egid, p->egid, vm_size(p) >> 10, p->mm ? p->mm->locked_vm >> 10 : 0, vm_rss(p) * (PAGE_SIZE / 1024),
                nthreads(p), p->nvcsw, p->nivcsw);
        break;
    case F_CMDLINE:
        if (p->cmdline && p->cmdline_len) {
            b->data = kmalloc(p->cmdline_len);
            memcpy(b->data, p->cmdline, p->cmdline_len);
            b->len = b->cap = p->cmdline_len;
        }
        break;
    case F_COMM: bprintf(b, "%s\n", p->name); break;
    case F_ENVIRON: break;
    case F_MAPS:
        if (p->mm) {
            mm_lock(p->mm);
            list_for_each(it, &p->mm->vmas) {
                struct vma *v = list_entry(it, struct vma, node);
                const char *name = v->flags & VMA_STACK ? "[stack]" : v->flags & VMA_HEAP ? "[heap]" : "";
                const char *path = v->file && v->file->path ? v->file->path : "";
                bprintf(b, "%012lx-%012lx %c%c%c%c %08lx 00:%02x %lu %s%s\n", v->start, v->end,
                        v->prot & VM_READ ? 'r' : '-', v->prot & VM_WRITE ? 'w' : '-', v->prot & VM_EXEC ? 'x' : '-',
                        v->flags & VMA_SHARED ? 's' : 'p', v->file ? v->pgoff * PAGE_SIZE : 0,
                        v->file ? 1 : 0, v->file ? v->file->inode->ino : 0, name, path);
            }
            mm_unlock(p->mm);
        }
        break;
    }
}

static int pf_open(struct inode *i, struct file *f) {
    struct buf *b = kzalloc(sizeof *b);
    gen(i->priv, b);
    f->priv = b;
    return 0;
}
static ssize_t pf_read(struct file *f, void *buf, size_t n, off_t *off) {
    struct buf *b = f->priv;
    if (!b || *off >= (off_t)b->len) return 0;
    n = MIN(n, b->len - *off);
    memcpy(buf, b->data + *off, n);
    *off += n;
    return n;
}
static void pf_release(struct file *f) {
    struct buf *b = f->priv;
    if (b) { kfree(b->data); kfree(b); }
}
static unsigned pf_poll(struct file *f) { return POLLIN | POLLRDNORM; }

/* process lists and fields are still BKL-protected: procfs takes it itself, so the VFS can
 * walk into /proc from lock-free path syscalls (under the namespace mutex: BKL inside is ok) */
#define PBKL(call) ({ bool __t = !bkl_held(); if (__t) bkl_enter(); int __r = (call); if (__t) bkl_exit(); __r; })
static int pl_lookup(struct inode *d, const char *n, struct inode **o) { return PBKL(p_lookup(d, n, o)); }
static int pl_readlink(struct inode *i, char *b, size_t s) { return PBKL(p_readlink(i, b, s)); }
static int pl_iterate(struct inode *d, uint64_t *pos, filldir_t fill, void *c) { return PBKL(p_iterate(d, pos, fill, c)); }
static int pl_follow(struct inode *i, struct inode **o) { return PBKL(p_follow(i, o)); }
static const struct inode_ops proc_iops = {
    .lookup = pl_lookup, .readlink = pl_readlink, .iterate = pl_iterate, .evict = p_evict, .follow_link = pl_follow,
};
static const struct file_ops proc_file_fops = { .open = pf_open, .read = pf_read, .release = pf_release, .poll = pf_poll };
static const struct file_ops proc_dir_fops = { .poll = pf_poll };

struct inode *procfs_create_root(void) {
    proc_root_inode = pnew(S_IFDIR | 0555, P_ROOT, 0, 0, 0);
    proc_root_inode->nlink = 2;
    return proc_root_inode;
}
