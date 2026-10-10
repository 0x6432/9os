#include <kernel/irq.h>
#include <kernel/uaccess.h>
#include <kernel/mutex.h>
/* procfs: process and system information (enough for BusyBox ps/top/free/mount). */
#include <kernel/vfs.h>
#include <kernel/cred.h>
int blk_proc_partitions(char *buf, size_t max);
int blk_proc_diskstats(char *buf, size_t max);
int net_proc_file(int which, char *buf, size_t max);
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

int locks_report(char *buf, int cap);
enum pkind { P_ROOT, P_SELF, P_PIDDIR, P_FDDIR, P_FD, P_FILE, P_CWD, P_EXE, P_NETDIR, P_SYSDIR };
enum pfile { F_STAT, F_STATUS, F_CMDLINE, F_COMM, F_ENVIRON, F_MAPS,
             G_MEMINFO, G_UPTIME, G_VERSION, G_CPUINFO, G_MOUNTS, G_LOADAVG, G_STAT, G_FILESYSTEMS, G_SCHED, G_VMSTAT, G_LOCKDEP, G_HARDEN, G_INTERRUPTS, G_PARTITIONS, G_DISKSTATS, G_LOCKS,
             N_DEV, N_ROUTE, N_ARP, N_TCP, N_UDP, N_RAW, N_UNIX, N_SNMP, N_IGMP,
             N_IGMP6, N_IF_INET6, N_IPV6_ROUTE, N_TCP6, N_UDP6, N_RAW6, N_SNMP6, N_NETLINK, N_NETSTAT, S_SYSCTL };
/* P_SYSDIR: the /proc/sys directory whose path is sysctls[sysi].path[0, syslen) */
struct pinfo { enum pkind kind; int pid; int fd; enum pfile file; int sysi, syslen; };

/* ---- /proc/sys (M32b): integer and string knobs from one table; directories are implied by
 * the paths. Writes need write permission on the 0644 root-owned file (DAC) as on Linux. */
extern int sysctl_ip_forward, sysctl_ip_default_ttl, sysctl_icmp_echo_ignore_all, sysctl_icmp_echo_ignore_broadcasts;
extern int sysctl_somaxconn, sysctl_lo_drop_every, sysctl_tcp_window_scaling, sysctl_tcp_timestamps;
extern int sysctl_tcp_sack, sysctl_tcp_tlp, sysctl_tcp_lost_rexmit, sysctl_tcp_fin_timeout, sysctl_ipv6_forwarding, sysctl_ipv6_disable;
extern int sysctl_ipv6_hop_limit, sysctl_ipv6_accept_ra, sysctl_ipv6_dad_transmits, sysctl_ipv6_autoconf, sysctl_icmpv6_echo_ignore_all;
extern int randomize_va_space;
const char *kernel_hostname(void);
int kernel_set_hostname(const char *name, size_t len);
static const char *sys_ostype(void) { return "Linux"; }
static const char *sys_osrelease(void) { return "6.1.0-9os"; }
static const struct sysctl {
    const char *path;
    int *var; int min, max;
    const char *(*sget)(void);
    int (*sset)(const char *, size_t);
} sysctls[] = {
    { "kernel/hostname", nullptr, 0, 0, kernel_hostname, kernel_set_hostname },
    { "kernel/ostype", nullptr, 0, 0, sys_ostype, nullptr },
    { "kernel/osrelease", nullptr, 0, 0, sys_osrelease, nullptr },
    { "kernel/randomize_va_space", &randomize_va_space, 0, 2, nullptr, nullptr },
    { "net/core/somaxconn", &sysctl_somaxconn, 1, 65535, nullptr, nullptr },
    { "net/core/9os_lo_drop_every", &sysctl_lo_drop_every, 0, 1000000, nullptr, nullptr },   /* test aid: lose 1/N of lo packets */
    { "net/ipv4/ip_forward", &sysctl_ip_forward, 0, 1, nullptr, nullptr },
    { "net/ipv4/conf/all/forwarding", &sysctl_ip_forward, 0, 1, nullptr, nullptr },
    { "net/ipv4/ip_default_ttl", &sysctl_ip_default_ttl, 1, 255, nullptr, nullptr },
    { "net/ipv4/icmp_echo_ignore_all", &sysctl_icmp_echo_ignore_all, 0, 1, nullptr, nullptr },
    { "net/ipv4/icmp_echo_ignore_broadcasts", &sysctl_icmp_echo_ignore_broadcasts, 0, 1, nullptr, nullptr },
    { "net/ipv4/tcp_window_scaling", &sysctl_tcp_window_scaling, 0, 1, nullptr, nullptr },
    { "net/ipv4/tcp_timestamps", &sysctl_tcp_timestamps, 0, 1, nullptr, nullptr },
    { "net/ipv4/tcp_sack", &sysctl_tcp_sack, 0, 1, nullptr, nullptr },
    { "net/ipv4/9os_tcp_tlp", &sysctl_tcp_tlp, 0, 1, nullptr, nullptr },
    { "net/ipv4/9os_tcp_lost_rexmit", &sysctl_tcp_lost_rexmit, 0, 1, nullptr, nullptr },
    { "net/ipv4/tcp_fin_timeout", &sysctl_tcp_fin_timeout, 1, 3600, nullptr, nullptr },
    { "net/ipv6/conf/all/forwarding", &sysctl_ipv6_forwarding, 0, 1, nullptr, nullptr },
    { "net/ipv6/conf/all/disable_ipv6", &sysctl_ipv6_disable, 0, 1, nullptr, nullptr },
    { "net/ipv6/conf/all/hop_limit", &sysctl_ipv6_hop_limit, 1, 255, nullptr, nullptr },
    { "net/ipv6/conf/all/accept_ra", &sysctl_ipv6_accept_ra, 0, 2, nullptr, nullptr },
    { "net/ipv6/conf/all/dad_transmits", &sysctl_ipv6_dad_transmits, 0, 10, nullptr, nullptr },
    { "net/ipv6/conf/all/autoconf", &sysctl_ipv6_autoconf, 0, 1, nullptr, nullptr },
    { "net/ipv6/icmp/echo_ignore_all", &sysctl_icmpv6_echo_ignore_all, 0, 1, nullptr, nullptr },
};
/* the entry under directory (sysi, len) named 'name' (len 0 = /proc/sys); *dir: it is a directory */
static int sys_child(int sysi, int len, const char *name, size_t nlen, bool *dir) {
    const char *pre = sysctls[sysi].path;
    for (size_t k = 0; k < ARRAY_SIZE(sysctls); k++) {
        const char *p = sysctls[k].path;
        if (strncmp(p, pre, (size_t)len) || strncmp(p + len, name, nlen)) continue;
        char c = p[len + nlen];
        if (c == '/' || !c) { *dir = c == '/'; return (int)k; }
    }
    return -1;
}

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
    *pi = (struct pinfo){ kind, pid, fd, file, 0, 0 };
    i->priv = pi;
    i->iops = &proc_iops;
    i->fops = S_ISDIR(mode) ? &proc_dir_fops : &proc_file_fops;
    i->dev = 3;
    i->ino = (kind == P_ROOT) ? 1 : ((uint64_t)pid << 16) | (kind << 8) | (file + fd + 1);
    i->uid = i->gid = 0;
    struct process *p = pid > 0 ? process_find(pid) : nullptr;
    if (p) {                              /* M31: /proc/<pid> belongs to the process's euid/egid */
        struct cred *c = proc_cred(p);
        i->uid = c->euid; i->gid = c->egid;
        cred_put(c);
    }
    return i;
}

static const struct { const char *name; enum pfile f; } global_files[] = {
    { "meminfo", G_MEMINFO }, { "uptime", G_UPTIME }, { "version", G_VERSION }, { "cpuinfo", G_CPUINFO },
    { "mounts", G_MOUNTS }, { "loadavg", G_LOADAVG }, { "stat", G_STAT }, { "filesystems", G_FILESYSTEMS }, { "sched", G_SCHED },
    { "vmstat", G_VMSTAT }, { "lockdep", G_LOCKDEP }, { "hardening", G_HARDEN }, { "interrupts", G_INTERRUPTS },
    { "partitions", G_PARTITIONS }, { "diskstats", G_DISKSTATS }, { "locks", G_LOCKS },
};
/* /proc/net (M32) */
static const struct { const char *name; enum pfile f; } net_files[] = {
    { "dev", N_DEV }, { "route", N_ROUTE }, { "arp", N_ARP }, { "tcp", N_TCP }, { "udp", N_UDP }, { "raw", N_RAW },
    { "unix", N_UNIX }, { "snmp", N_SNMP }, { "igmp", N_IGMP },
    { "igmp6", N_IGMP6 }, { "if_inet6", N_IF_INET6 }, { "ipv6_route", N_IPV6_ROUTE }, { "tcp6", N_TCP6 }, { "udp6", N_UDP6 },
    { "raw6", N_RAW6 }, { "snmp6", N_SNMP6 }, { "netlink", N_NETLINK }, { "netstat", N_NETSTAT },
};
static const struct { const char *name; enum pfile f; } pid_files[] = {
    { "stat", F_STAT }, { "status", F_STATUS }, { "cmdline", F_CMDLINE }, { "comm", F_COMM },
    { "environ", F_ENVIRON }, { "maps", F_MAPS },
};

static int p_lookup(struct inode *dir, const char *name, struct inode **out) {
    struct pinfo *pi = dir->priv;
    if (pi->kind == P_ROOT) {
        if (!strcmp(name, "self")) { *out = pnew(S_IFLNK | 0777, P_SELF, 0, 0, 0); return 0; }
        if (!strcmp(name, "net")) { struct inode *i = pnew(S_IFDIR | 0555, P_NETDIR, 0, 0, 0); i->parent = dir; *out = i; return 0; }
        if (!strcmp(name, "sys")) {
            struct inode *i = pnew(S_IFDIR | 0555, P_SYSDIR, 0, 0, 0);
            i->ino = 0x7000000; i->parent = dir; *out = i; return 0;
        }
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
            if (!strcmp(name, pid_files[k].name)) { *out = pnew(S_IFREG | (pid_files[k].f == F_ENVIRON ? 0400 : 0444), P_FILE, pi->pid, 0, pid_files[k].f); return 0; }
        return -ENOENT;
    }
    if (pi->kind == P_NETDIR) {
        for (size_t k = 0; k < ARRAY_SIZE(net_files); k++)
            if (!strcmp(name, net_files[k].name)) { *out = pnew(S_IFREG | 0444, P_FILE, 0, 0, net_files[k].f); return 0; }
        return -ENOENT;
    }
    if (pi->kind == P_SYSDIR) {
        bool isdir;
        int k = sys_child(pi->sysi, pi->syslen, name, strlen(name), &isdir);
        if (k < 0) return -ENOENT;
        struct inode *i;
        if (isdir) {
            i = pnew(S_IFDIR | 0555, P_SYSDIR, 0, 0, 0);
            ((struct pinfo *)i->priv)->sysi = k;
            ((struct pinfo *)i->priv)->syslen = pi->syslen + (int)strlen(name) + 1;
            i->ino = 0x7000000 + (uint64_t)k * 128 + (uint64_t)pi->syslen + strlen(name) + 1;
            i->parent = dir;
        } else {
            bool rw = sysctls[k].var || sysctls[k].sset;
            i = pnew(S_IFREG | (rw ? 0644 : 0444), P_FILE, 0, k, S_SYSCTL);
            i->ino = 0x7000000 + (uint64_t)k * 128 + 127;
        }
        *out = i;
        return 0;
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

/* M31: cwd/exe/fd links of another user's process need CAP_SYS_PTRACE (ptrace_may_access-lite) */
static bool proc_may_peek(struct process *p) {
    if (p == curproc || capable(CAP_SYS_PTRACE)) return true;
    const struct cred *c = current_cred();
    struct cred *t = proc_cred(p);
    bool ok = c->uid == t->uid && c->uid == t->euid && c->uid == t->suid &&
              c->gid == t->gid && c->gid == t->egid && c->gid == t->sgid;
    cred_put(t);
    return ok;
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
    if (!proc_may_peek(p)) return -EACCES;
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
        if (!proc_may_peek(p)) return -EACCES;
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
        emit(&c, "net", 6, 4);
        emit(&c, "sys", 0x7000000, 4);
        for (size_t k = 0; k < ARRAY_SIZE(global_files); k++) emit(&c, global_files[k].name, 100 + k, 8);
        process_list(emit_pid, &c);
    } else if (pi->kind == P_PIDDIR) {
        emit(&c, "fd", 3, 4); emit(&c, "cwd", 4, 10); emit(&c, "exe", 5, 10);
        for (size_t k = 0; k < ARRAY_SIZE(pid_files); k++) emit(&c, pid_files[k].name, 10 + k, 8);
    } else if (pi->kind == P_NETDIR) {
        for (size_t k = 0; k < ARRAY_SIZE(net_files); k++) emit(&c, net_files[k].name, 200 + k, 8);
    } else if (pi->kind == P_SYSDIR) {
        const char *pre = sysctls[pi->sysi].path;
        for (size_t k = 0; k < ARRAY_SIZE(sysctls); k++) {
            const char *p = sysctls[k].path;
            if (strncmp(p, pre, (size_t)pi->syslen)) continue;
            const char *nm = p + pi->syslen, *e = strchr(nm, '/');
            size_t nl = e ? (size_t)(e - nm) : strlen(nm);
            bool d;
            if (sys_child(pi->sysi, pi->syslen, nm, nl, &d) != (int)k) continue;   /* listed by an earlier entry */
            char n[64];
            snprintf(n, sizeof n, "%.*s", (int)nl, nm);
            if (!emit(&c, n, 0x7000000 + k * 128 + (e ? (uint64_t)pi->syslen + nl + 1 : 127), e ? 4 : 8)) break;
        }
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

static int tracer_pid(struct process *p) {
    if (list_empty(&p->threads)) return 0;
    struct process *tr = list_entry(p->threads.next, struct thread, proc_node)->ptracer;
    return tr ? tr->pid : 0;
}
static char pstate(struct process *p) {
    if (p->state == P_ZOMBIE) return 'Z';
    if (p->stopped) return 'T';
    if (!list_empty(&p->threads) && list_entry(p->threads.next, struct thread, proc_node)->pt_stopped) return 't';
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
    case G_MOUNTS: case G_FILESYSTEMS: case G_PARTITIONS: case G_DISKSTATS: case G_LOCKS: {
        char *t = kmalloc(8192);
        if (t) {
            int n = pi->file == G_MOUNTS ? vfs_proc_mounts(t, 8192) : pi->file == G_FILESYSTEMS ? vfs_proc_filesystems(t, 8192)
                  : pi->file == G_PARTITIONS ? blk_proc_partitions(t, 8192) : pi->file == G_LOCKS ? locks_report(t, 8192)
                  : blk_proc_diskstats(t, 8192);
            if (b->len + n + 1 > b->cap) { b->cap = b->len + n + 1; b->data = krealloc(b->data, b->cap); }
            memcpy(b->data + b->len, t, n);
            b->len += n;
            kfree(t);
        }
        break;
    }
    case N_DEV: case N_ROUTE: case N_ARP: case N_TCP: case N_UDP: case N_RAW: case N_UNIX: case N_SNMP: case N_IGMP:
    case N_IGMP6: case N_IF_INET6: case N_IPV6_ROUTE: case N_TCP6: case N_UDP6: case N_RAW6: case N_SNMP6: case N_NETLINK: case N_NETSTAT: {
        char *t = kmalloc(65536);
        if (t) {
            int n = net_proc_file(pi->file - N_DEV, t, 65536);
            if (b->len + n + 1 > b->cap) { b->cap = b->len + n + 1; b->data = krealloc(b->data, b->cap); }
            memcpy(b->data + b->len, t, n);
            b->len += n;
            kfree(t);
        }
        break;
    }
    case S_SYSCTL: {
        const struct sysctl *y = &sysctls[pi->fd];
        if (y->var) bprintf(b, "%d\n", *y->var);
        else bprintf(b, "%s\n", y->sget());
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
    case F_STATUS: {
        char credbuf[1024];
        cred_proc_status(p, credbuf, sizeof credbuf);
        bprintf(b, "Name:\t%s\nState:\t%c\nTgid:\t%d\nPid:\t%d\nPPid:\t%d\nTracerPid:\t%d\n%sVmSize:\t%8lu kB\nVmLck:\t%8lu kB\nVmRSS:\t%8lu kB\nThreads:\t%d\n"
                   "voluntary_ctxt_switches:\t%lu\nnonvoluntary_ctxt_switches:\t%lu\n",
                p->name, pstate(p), p->pid, p->pid, p->parent ? p->parent->pid : 0, tracer_pid(p),
                credbuf, vm_size(p) >> 10, p->mm ? p->mm->locked_vm >> 10 : 0, vm_rss(p) * (PAGE_SIZE / 1024),
                nthreads(p), p->nvcsw, p->nivcsw);
        break;
    }
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
static ssize_t pf_write(struct file *f, const void *buf, size_t n, off_t *off) {
    struct pinfo *pi = f->inode->priv;
    if (pi->kind != P_FILE || pi->file != S_SYSCTL) return -EINVAL;
    const struct sysctl *y = &sysctls[pi->fd];
    char t[72];
    if (n >= sizeof t) return -EINVAL;
    memcpy(t, buf, n);
    t[n] = 0;
    size_t l = n;
    while (l && (t[l - 1] == '\n' || t[l - 1] == ' ' || t[l - 1] == '\t')) t[--l] = 0;
    if (y->sset) { int r = y->sset(t, l); if (r) return r; *off += n; return (ssize_t)n; }
    if (!y->var) return -EPERM;
    const char *c = t;
    while (*c == ' ' || *c == '\t') c++;
    bool neg = *c == '-';
    if (neg || *c == '+') c++;
    if (*c < '0' || *c > '9') return -EINVAL;
    long v = 0;
    for (; *c >= '0' && *c <= '9'; c++) { v = v * 10 + (*c - '0'); if (v > 1L << 31) return -EINVAL; }
    if (*c) return -EINVAL;
    if (neg) v = -v;
    if (v < y->min || v > y->max) return -EINVAL;
    __atomic_store_n(y->var, (int)v, __ATOMIC_RELAXED);
    *off += n;
    return (ssize_t)n;
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
static const struct file_ops proc_file_fops = { .open = pf_open, .read = pf_read, .write = pf_write, .release = pf_release, .poll = pf_poll };
static const struct file_ops proc_dir_fops = { .poll = pf_poll };

struct inode *procfs_create_root(void) {
    proc_root_inode = pnew(S_IFDIR | 0555, P_ROOT, 0, 0, 0);
    proc_root_inode->nlink = 2;
    return proc_root_inode;
}
