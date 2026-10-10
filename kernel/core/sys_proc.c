#include <kernel/vmm.h>
/* Process, time and miscellaneous system calls. */
#include <kernel/vfs.h>
#include <kernel/syscall.h>
#include <kernel/exec.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/printk.h>
#include <kernel/time.h>
#include <kernel/pmm.h>
#include <kernel/tty.h>
#include <kernel/acpi.h>
#include <arch/syscall.h>

struct trap_frame *thread_user_frame(struct thread *t);
uint64_t random_u64(void);
int64_t sys_arch_prctl(int code, uint64_t addr);
[[gnu::weak]] void arch_poweroff(void) {}
[[gnu::weak]] void arch_reboot(void) {}

int64_t sys_exit(int code) {
    struct process *p = curproc;
    if (p->threads.next->next != &p->threads) thread_exit_only();   /* other threads remain */
    process_exit((code & 0xff) << 8);
}
int64_t sys_exit_group(int code) { process_exit((code & 0xff) << 8); }

int64_t sys_fork(void) { return process_fork(thread_user_frame(current), 17, 0, nullptr, nullptr, 0); }
int64_t sys_vfork(void) { return process_fork(thread_user_frame(current), 0x4000 | 0x100 | 17, 0, nullptr, nullptr, 0); }
int64_t sys_clone(uint64_t flags, uint64_t sp, int *ptid, int *ctid, uint64_t tls) {
#if defined(__x86_64__)
    return process_fork(thread_user_frame(current), flags, sp, ptid, ctid, tls);
#else
    /* aarch64/riscv64 order: flags, stack, ptid, tls, ctid */
    return process_fork(thread_user_frame(current), flags, sp, ptid, (int *)tls, (uint64_t)ctid);
#endif
}

static void free_strv(char **v) {
    if (!v) return;
    for (char **p = v; *p; p++) kfree(*p);
    kfree(v);
}

static int64_t copy_strv(char *const *uv, char ***out) {
    *out = nullptr;
    size_t n = 0, cap = 16, total = 0;
    char **v = kmalloc(sizeof(char *) * cap);
    if (uv) {
        for (;;) {
            char *up;
            if (copy_from_user(&up, &uv[n], sizeof up)) { v[n] = nullptr; free_strv(v); return -EFAULT; }
            if (!up) break;
            char *k = kmalloc(4096);
            int64_t l = strncpy_from_user(k, up, 4096);
            if (l < 0) { kfree(k); v[n] = nullptr; free_strv(v); return l == -ENAMETOOLONG ? -E2BIG : l; }
            total += l + 1;
            if (total > 256 * 1024) { kfree(k); v[n] = nullptr; free_strv(v); return -E2BIG; }
            if (n + 2 > cap) { cap *= 2; v = krealloc(v, sizeof(char *) * cap); }
            v[n++] = k;
        }
    }
    v[n] = nullptr;
    *out = v;
    return 0;
}

int64_t sys_execve(const char *upath, char *const *uargv, char *const *uenvp) {
    char *path = kmalloc(4096);
    int64_t r = user_path(upath, path);
    char **argv = nullptr, **envp = nullptr;
    if (!r) r = copy_strv(uargv, &argv);
    if (!r) r = copy_strv(uenvp, &envp);
    if (!r) r = do_execve(path, argv, envp, thread_user_frame(current));
    free_strv(argv); free_strv(envp); kfree(path);
    return r;
}

int rusage_to_user(void *u, const struct rusage_k *r) {
    int64_t ru[18] = {0};       /* struct rusage: utime, stime timevals + 14 longs */
    uint64_t us = r->utime_ns / 1000, ss = r->stime_ns / 1000;
    ru[0] = us / 1000000; ru[1] = us % 1000000;
    ru[2] = ss / 1000000; ru[3] = ss % 1000000;
    ru[4 + 4] = r->min_flt;     /* ru_minflt */
    ru[4 + 12] = r->nvcsw; ru[4 + 13] = r->nivcsw;
    return copy_to_user(u, ru, sizeof ru);
}

int64_t sys_wait4(int pid, int *status, int options, void *rusage) {
    current->reaped_ru = (struct rusage_k){0};
    if (options & ~(1 | 2 | 8 | 0x20000000 | 0x40000000 | 0x80000000)) return -EINVAL;
    int64_t r = do_wait(pid, status, options, nullptr);
    if (r >= 0 && rusage && rusage_to_user(rusage, &current->reaped_ru)) return -EFAULT;
    return r;
}

struct process *pidfd_process(int fd, unsigned *flags);
int64_t sys_waitid(int idtype, int id, void *uinfo, int options, void *ru) {
    if (options & ~(1 | 2 | 4 | 8 | 0x01000000 | 0x20000000 | 0x40000000 | 0x80000000)) return -EINVAL;
    if (!(options & (2 | 4 | 8))) return -EINVAL;           /* WSTOPPED / WEXITED / WCONTINUED */
    if (idtype == 3) {                                      /* P_PIDFD */
        unsigned fl;
        struct process *p = pidfd_process(id, &fl);
        if (!p) return -EBADF;
        if (p->reaped) return -ECHILD;
        if ((fl & 0x800) && !(p->state == P_ZOMBIE)) options |= 1;   /* PIDFD_NONBLOCK -> WNOHANG semantics */
        idtype = 1; id = p->pid;
    } else if (idtype < 0 || idtype > 2) return -EINVAL;
    else if (idtype == 1 && id <= 0) return -EINVAL;
    current->reaped_ru = (struct rusage_k){0};
    struct wait_result res;
    int64_t r = do_wait_ex(idtype, id, options, &res);
    if (r < 0) return r;
    if ((options & 1) && r == 0 && idtype == 1) {
        /* PIDFD_NONBLOCK: the child exists but has nothing to report */
    }
    if (uinfo) {
        uint8_t si[128] = {0};
        if (r > 0) {
            struct ksiginfo ki = { .signo = SIGCHLD, .code = res.code, .pid = res.pid, .uid = res.uid,
                                   .v = res.utime, .v2 = res.stime };
            int st = res.status;
            ki.i1 = res.code == CLD_EXITED ? (st >> 8) & 0xff : res.code == CLD_STOPPED || res.code == CLD_TRAPPED ? (st >> 8) & 0xff
                  : res.code == CLD_CONTINUED ? SIGCONT : st & 0x7f;
            siginfo_to_user(&ki, si);
        }
        if (copy_to_user(uinfo, si, sizeof si)) return -EFAULT;
    }
    if (ru && rusage_to_user(ru, &current->reaped_ru)) return -EFAULT;
    return 0;
}

/* clone3(2): struct clone_args { flags, pidfd, child_tid, parent_tid, exit_signal, stack,
 * stack_size, tls, set_tid, set_tid_size, cgroup } (u64 each) */
int64_t sys_clone3(const uint64_t *uargs, size_t size) {
    uint64_t a[11] = {0};
    if (size < 64 || size > 4096) return -EINVAL;
    if (copy_from_user(a, uargs, MIN(size, sizeof a))) return -EFAULT;
    for (size_t o = sizeof a; o < size; o += 8) {         /* unknown trailing fields must be zero */
        uint64_t z = 0;
        if (copy_from_user(&z, (const uint8_t *)uargs + o, MIN((size_t)8, size - o))) return -EFAULT;
        if (z) return -E2BIG;
    }
    uint64_t flags = a[0];
    if (flags & 0xff) return -EINVAL;                       /* CSIGNAL goes in exit_signal */
    if (flags & 0x200000000ULL) return -EINVAL;             /* CLONE_INTO_CGROUP */
    if (a[4] >= NSIG) return -EINVAL;
    if (a[9]) return capable(CAP_SYS_ADMIN) ? -EINVAL : -EPERM;    /* set_tid unsupported */
    if ((a[5] == 0) != (a[6] == 0) && a[5]) return -EINVAL;
    if ((flags & 0x10000) && a[4]) return -EINVAL;          /* CLONE_THREAD with an exit signal */
    uint64_t sp = a[5] ? a[5] + a[6] : 0;
    return process_fork_ex(thread_user_frame(current), flags, sp, (int *)a[3], (int *)a[2], a[7], (int)a[4],
                           (flags & 0x1000) ? (int *)a[1] : nullptr);
}

int64_t sys_getpid(void) { return curproc->pid; }
int64_t sys_gettid(void) { return current->tid; }
int64_t sys_getppid(void) { return curproc->parent ? curproc->parent->pid : 0; }
int64_t sys_setpgid(int pid, int pgid) {
    struct process *p = pid ? process_find(pid) : curproc;
    if (!p) return -ESRCH;
    if (pgid < 0) return -EINVAL;
    p->pgid = pgid ? pgid : p->pid;
    return 0;
}
int64_t sys_getpgid(int pid) {
    struct process *p = pid ? process_find(pid) : curproc;
    return p ? p->pgid : -ESRCH;
}
int64_t sys_getpgrp(void) { return curproc->pgid; }
int64_t sys_setsid(void) {
    struct process *p = curproc;
    if (p->pgid == p->pid) {
        /* already a group leader: allowed if no other member (simplification) */
    }
    p->sid = p->pgid = p->pid;
    p->ctty = nullptr;
    return p->sid;
}
int64_t sys_getsid(int pid) {
    struct process *p = pid ? process_find(pid) : curproc;
    return p ? p->sid : -ESRCH;
}

int64_t sys_umask(uint32_t m) { uint32_t o = curproc->umask; curproc->umask = m & 0777; return o; }

struct utsname { char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65]; };
static char hostname[65] = "9os";
int64_t sys_uname(struct utsname *u) {
    struct utsname k;
    memset(&k, 0, sizeof k);
    strcpy(k.sysname, "Linux");          /* programs check for Linux semantics */
    strcpy(k.nodename, hostname);
    strcpy(k.release, "6.1.0-9os");
    strcpy(k.version, "#1 9os " __DATE__);
#if defined(__x86_64__)
    strcpy(k.machine, "x86_64");
#elif defined(__aarch64__)
    strcpy(k.machine, "aarch64");
#else
    strcpy(k.machine, "riscv64");
#endif
    strcpy(k.domainname, "(none)");
    return copy_to_user(u, &k, sizeof k);
}
int64_t sys_sethostname(const char *name, size_t len) {
    if (!capable(CAP_SYS_ADMIN)) return -EPERM;
    if (len > 64) return -EINVAL;
    char k[65] = {0};
    if (copy_from_user(k, name, len)) return -EFAULT;
    memcpy(hostname, k, 65);
    return 0;
}

const char *kernel_hostname(void) { return hostname; }
int kernel_set_hostname(const char *name, size_t len) {     /* /proc/sys/kernel/hostname */
    if (len > 64) return -EINVAL;
    memset(hostname, 0, sizeof hostname);
    memcpy(hostname, name, len);
    return 0;
}

int64_t sys_sched_yield(void) { schedule(); return 0; }

static struct wait_queue sleep_wq = WAIT_QUEUE_INIT(sleep_wq);

static int64_t sleep_until(uint64_t end, struct timespec *urem) {
    uint64_t now0 = time_ns(), ns = end > now0 ? end - now0 : 0;
    current->restart_sleep = false;
    int r = ns ? wait_event_timeout(&sleep_wq, ns) : 0;
    if (r == -EINTR) {
        /* a signal without handler (stop, ptrace stop, ignored) resumes via restart_syscall */
        current->restart_end_ns = end; current->restart_urem = urem; current->restart_sleep = true;
        if (urem) {
            uint64_t now = time_ns(), left = end > now ? end - now : 0;
            struct timespec rem = { left / 1000000000ULL, left % 1000000000ULL };
            copy_to_user(urem, &rem, sizeof rem);
        }
        return -EINTR;
    }
    return 0;
}
static int64_t sleep_interruptible(uint64_t ns, struct timespec *urem) { return sleep_until(time_ns() + ns, urem); }
int64_t sys_restart_syscall(void) {
    if (!current->restart_sleep) return -EINTR;
    return sleep_until(current->restart_end_ns, current->restart_urem);
}

int64_t sys_nanosleep(const struct timespec *ureq, struct timespec *urem) {
    struct timespec ts;
    if (copy_from_user(&ts, ureq, sizeof ts)) return -EFAULT;
    if (ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000 || ts.tv_sec < 0) return -EINVAL;
    return sleep_interruptible(ts.tv_sec * 1000000000ULL + ts.tv_nsec, urem);
}

int64_t sys_clock_nanosleep(int clk, int flags, const struct timespec *ureq, struct timespec *urem) {
    struct timespec ts;
    if (copy_from_user(&ts, ureq, sizeof ts)) return -EFAULT;
    uint64_t ns = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    if (flags & 1) {           /* TIMER_ABSTIME */
        struct timespec now = clk == 0 ? now_timespec() : (struct timespec){ time_ns() / 1000000000ULL, time_ns() % 1000000000ULL };
        uint64_t nn = now.tv_sec * 1000000000ULL + now.tv_nsec;
        ns = ns > nn ? ns - nn : 0;
        urem = nullptr;
    }
    return sleep_interruptible(ns, urem);
}

int64_t sys_clock_gettime(int clk, struct timespec *uts) {
    struct timespec ts;
    uint64_t ns = time_ns();
    if (clk == 0 || clk == 5 || clk == 8)   /* REALTIME, REALTIME_COARSE, REALTIME_ALARM */
        ts = now_timespec();
    else if (clk == 2 || clk == 3) {        /* PROCESS_CPUTIME_ID, THREAD_CPUTIME_ID */
        uint64_t f = arch_irq_save();       /* no switch while sampling the running slice */
        uint64_t run = time_ns() - current->exec_start_ns;
        uint64_t cpu = (clk == 2 ? __atomic_load_n(&curproc->sum_exec_ns, __ATOMIC_RELAXED) : current->sum_exec_ns) + run;
        arch_irq_restore(f);
        ts = (struct timespec){ cpu / 1000000000ULL, cpu % 1000000000ULL };
    }
    else ts = (struct timespec){ ns / 1000000000ULL, ns % 1000000000ULL };
    return copy_to_user(uts, &ts, sizeof ts);
}
int64_t sys_clock_getres(int clk, struct timespec *uts) {
    struct timespec ts = { 0, 1000000 };
    return uts ? copy_to_user(uts, &ts, sizeof ts) : 0;
}
int64_t sys_gettimeofday(int64_t *utv, void *tz) {
    struct timespec ts = now_timespec();
    int64_t tv[2] = { ts.tv_sec, ts.tv_nsec / 1000 };
    return utv ? copy_to_user(utv, tv, sizeof tv) : 0;
}
int64_t sys_time(int64_t *ut) {
    int64_t t = now_timespec().tv_sec;
    if (ut && copy_to_user(ut, &t, 8)) return -EFAULT;
    return t;
}
int64_t sys_times(uint64_t *ubuf) {
    struct process *p = curproc;     /* clock_t at USER_HZ = 100 */
    uint64_t t[4] = { p->utime_ns / 10000000, p->stime_ns / 10000000, p->cutime_ns / 10000000, p->cstime_ns / 10000000 };
    if (ubuf && copy_to_user(ubuf, t, sizeof t)) return -EFAULT;
    return jiffies / 10;
}
int64_t sys_getrusage(int who, void *u) {
    struct process *p = curproc;
    struct rusage_k r;
    if (who == 0)            /* RUSAGE_SELF */
        r = (struct rusage_k){ p->utime_ns, p->stime_ns, p->min_flt, p->nvcsw, p->nivcsw };
    else if (who == -1)      /* RUSAGE_CHILDREN */
        r = (struct rusage_k){ p->cutime_ns, p->cstime_ns, p->cmin_flt, p->cnvcsw, p->cnivcsw };
    else if (who == 1)       /* RUSAGE_THREAD */
        r = (struct rusage_k){ current->utime_ns, current->stime_ns, 0, current->nvcsw, current->nivcsw };
    else return -EINVAL;
    return rusage_to_user(u, &r);
}
int64_t sys_sysinfo(void *u) {
    uint64_t free, total;
    pmm_stats(&free, &total);
    struct { int64_t uptime; uint64_t loads[3], totalram, freeram, sharedram, bufferram, totalswap, freeswap;
             uint16_t procs, pad; uint64_t totalhigh, freehigh; uint32_t mem_unit; char _f[4]; } s;
    memset(&s, 0, sizeof s);
    s.uptime = time_ns() / 1000000000ULL;
    s.totalram = total * PAGE_SIZE; s.freeram = free * PAGE_SIZE;
    s.procs = 1; s.mem_unit = 1;
    return copy_to_user(u, &s, sizeof s);
}

struct rlimit { uint64_t cur, max; };
int64_t sys_prlimit64(int pid, int res, const struct rlimit *unew, struct rlimit *uold) {
    struct rlimit r = { ~0ULL, ~0ULL };
    if (res == 3) r.cur = 8 << 20;          /* RLIMIT_STACK */
    if (res == 7) r.cur = r.max = MAX_FDS;  /* RLIMIT_NOFILE */
    if (uold && copy_to_user(uold, &r, sizeof r)) return -EFAULT;
    return 0;
}
int64_t sys_getrlimit(int res, struct rlimit *u) { return sys_prlimit64(0, res, nullptr, u); }
int64_t sys_setrlimit(int res, const struct rlimit *u) { return 0; }

int64_t sys_getrandom(void *ubuf, size_t n, unsigned flags) {
    uint8_t tmp[256];
    size_t done = 0;
    while (done < n) {
        size_t c = MIN(n - done, sizeof tmp);
        for (size_t i = 0; i < c; i += 8) { uint64_t r = random_u64(); memcpy(tmp + i, &r, MIN(8, c - i)); }
        if (copy_to_user((uint8_t *)ubuf + done, tmp, c)) return -EFAULT;
        done += c;
    }
    return n;
}

int64_t sys_set_tid_address(int *tidptr) { current->clear_child_tid = tidptr; return current->tid; }
int64_t sys_set_robust_list(void *h, size_t len) {
    if (len != 24) return -EINVAL;               /* sizeof(struct robust_list_head) */
    current->robust_list = (uint64_t)h;
    current->robust_len = len;
    return 0;
}
int64_t sys_get_robust_list(int pid, uint64_t *uhead, size_t *ulen) {
    struct thread *t = pid ? process_find_thread(pid) : current;
    if (!t) return -ESRCH;
    if (t != current && !capable(CAP_SYS_PTRACE) && t->ptracer != curproc) return -EPERM;
    uint64_t h = t->robust_list, l = 24;
    if (copy_to_user(uhead, &h, 8) || copy_to_user(ulen, &l, 8)) return -EFAULT;
    return 0;
}
int64_t sys_rseq(void) { return -ENOSYS; }
int64_t sys_prctl(int opt, uint64_t a2) {
    if (opt == 15) {   /* PR_SET_NAME */
        char n[16] = {0};
        strncpy_from_user(n, (const char *)a2, 15);
        strlcpy(current->name, n, sizeof current->name);
        return 0;
    }
    if (opt == 16) return copy_to_user((void *)a2, current->name, 16);   /* PR_GET_NAME */
    extern int64_t cred_prctl(int opt, uint64_t a2);
    int64_t r = cred_prctl(opt, a2);
    return r == -ENOSYS ? -EINVAL : r;
}
int64_t sys_personality(uint64_t p) { return 0; }
/* M31: may the caller change t's scheduling? Other users' threads need CAP_SYS_NICE, as do
 * raising priority (lower nice; EACCES like Linux) and real-time policies (RLIMIT_RTPRIO 0). */
static int sched_perm(struct thread *t, int policy, int rt_prio, int nice) {
    if (capable(CAP_SYS_NICE)) return 0;
    if (!t->proc) return -EPERM;
    if (t->proc != curproc) {
        const struct cred *c = current_cred();
        struct cred *pc = proc_cred(t->proc);
        bool ok = pc->uid == c->euid || pc->euid == c->euid;
        cred_put(pc);
        if (!ok) return -EPERM;
    }
    if ((policy == 1 || policy == 2) && (t->policy != policy || rt_prio > t->rt_prio)) return -EPERM;
    if (nice < t->nice) return -EACCES;
    return 0;
}

/* nice: PRIO_PROCESS applies to every thread of the process (who = pid, 0 = caller) */
static struct process *prio_target(int which, int who) {
    if (which != 0) return nullptr;              /* PRIO_PGRP/PRIO_USER not supported */
    return who ? process_find(who) : curproc;
}
int64_t sys_getpriority(int which, int who) {
    if (which != 0) return -EINVAL;
    struct process *p = prio_target(which, who);
    if (!p) return -ESRCH;
    struct thread *t = who ? list_first(&p->threads, struct thread, proc_node) : current;
    return 20 - t->nice;                          /* kernel ABI: 40..1 */
}
int64_t sys_setpriority(int which, int who, int prio) {
    if (which != 0) return -EINVAL;
    struct process *p = prio_target(which, who);
    if (!p) return -ESRCH;
    list_for_each(it, &p->threads) {
        struct thread *t = list_entry(it, struct thread, proc_node);
        int r = sched_perm(t, t->policy, t->rt_prio, prio < -20 ? -20 : prio);
        if (r) return r;
    }
    list_for_each(it, &p->threads) {
        struct thread *t = list_entry(it, struct thread, proc_node);
        sched_set_policy(t, t->policy, t->rt_prio, prio);
    }
    return 0;
}

static struct thread *affinity_target(int pid);
int64_t sys_sched_setscheduler(int pid, int policy, const int *uparam) {
    int prio;
    if (!uparam || copy_from_user(&prio, uparam, sizeof prio)) return -EFAULT;
    struct thread *t = affinity_target(pid);
    if (!t) return -ESRCH;
    int r = sched_perm(t, policy & ~0x40000000, prio, t->nice);
    if (r) return r == -EACCES ? -EPERM : r;
    return sched_set_policy(t, policy & ~0x40000000, prio, t->nice);    /* ignore SCHED_RESET_ON_FORK */
}
int64_t sys_sched_getscheduler(int pid) {
    struct thread *t = affinity_target(pid);
    return t ? t->policy : -ESRCH;
}
int64_t sys_sched_setparam(int pid, const int *uparam) {
    int prio;
    if (!uparam || copy_from_user(&prio, uparam, sizeof prio)) return -EFAULT;
    struct thread *t = affinity_target(pid);
    if (!t) return -ESRCH;
    int r = sched_perm(t, t->policy, prio, t->nice);
    if (r) return r == -EACCES ? -EPERM : r;
    return sched_set_policy(t, t->policy, prio, t->nice);
}
int64_t sys_sched_getparam(int pid, int *uparam) {
    struct thread *t = affinity_target(pid);
    if (!t) return -ESRCH;
    int prio = t->rt_prio;
    return copy_to_user(uparam, &prio, sizeof prio);
}
int64_t sys_sched_get_priority_max(int policy) { return policy == 1 || policy == 2 ? 99 : policy == 0 || policy == 3 || policy == 5 ? 0 : -EINVAL; }
int64_t sys_sched_get_priority_min(int policy) { return policy == 1 || policy == 2 ? 1 : policy == 0 || policy == 3 || policy == 5 ? 0 : -EINVAL; }
int64_t sys_sched_rr_get_interval(int pid, struct timespec *uts) {
    struct thread *t = affinity_target(pid);
    if (!t) return -ESRCH;
    uint64_t ms = t->policy == 1 ? 0 : t->policy == 2 ? SCHED_RR_QUANTUM : SCHED_QUANTUM;
    struct timespec ts = { 0, (int64_t)ms * 1000000 };
    return copy_to_user(uts, &ts, sizeof ts);
}
/* pid 0 = calling thread; otherwise a TID, including a non-leader pthread. BKL held. */
static struct thread *affinity_target(int pid) {
    if (!pid || pid == current->tid) return current;
    return process_find_thread(pid);
}

int64_t sys_sched_getaffinity(int pid, size_t len, uint64_t *mask) {       /* lock-free syscall */
    if (len < 8 || (len & 7)) return -EINVAL;
    uint64_t online = ncpus >= 64 ? ~0ULL : (1ULL << ncpus) - 1, m;
    if (!pid || pid == current->tid) m = current->affinity & online;
    else {
        bkl_enter();
        struct thread *t = affinity_target(pid);
        m = t ? t->affinity & online : 0;
        bkl_exit();
        if (!t) return -ESRCH;
    }
    if (copy_to_user(mask, &m, 8)) return -EFAULT;
    uint64_t z = 0;
    for (size_t o = 8; o < len && o < 128; o += 8) if (copy_to_user((char *)mask + o, &z, 8)) return -EFAULT;
    return len < 128 ? (int64_t)len : 128;
}

int64_t sys_sched_setaffinity(int pid, size_t len, const uint64_t *umask) {
    if (len < 8) return -EINVAL;
    uint64_t m;
    if (copy_from_user(&m, umask, 8)) return -EFAULT;
    struct thread *t = affinity_target(pid);
    if (!t) return -ESRCH;
    if (t != current && sched_perm(t, t->policy, t->rt_prio, t->nice)) return -EPERM;
    return sched_set_affinity(t, m);
}
int64_t sys_getcpu(unsigned *cpu, unsigned *node) {
    unsigned z = 0, id = this_cpu()->id;
    if (cpu) copy_to_user(cpu, &id, 4);
    if (node) copy_to_user(node, &z, 4);
    return 0;
}
int64_t sys_syslog(int type, char *buf, int len) {
    extern size_t log_read(char *buf, size_t len);
    if (type == 10) return 32768;                    /* SYSLOG_ACTION_SIZE_BUFFER */
    if (type != 3 && type != 4) return 0;            /* READ_ALL / READ_CLEAR */
    if (len < 0) return -EINVAL;
    char *k = kmalloc(32768);
    if (!k) return -ENOMEM;
    size_t n = log_read(k, MIN((size_t)len, (size_t)32768));
    int r = copy_to_user(buf, k, n);
    kfree(k);
    return r ? r : (int64_t)n;
}
int64_t sys_zero(void) { return 0; }

/* M32: SIGALRM for processes blocked in the kernel. user_return_work() only fires alarms of
 * processes that run; the "alarm" thread sleeps until the earliest deadline and sends the
 * signal, which interrupts the sleep (busybox ping blocks in recvfrom waiting for SIGALRM). */
static struct wait_queue alarm_wq = WAIT_QUEUE_INIT(alarm_wq);
static uint64_t alarm_gen;
static struct thread *alarm_thread;
void posix_timers_scan(struct process *p, uint64_t *next);
static void alarm_scan(struct process *p, void *arg) {
    uint64_t *next = arg, a = p->alarm_ns;
    posix_timers_scan(p, next);
    if (!a || p->state == P_ZOMBIE) return;
    if (a <= time_ns()) { p->alarm_ns = 0; signal_send(p, SIGALRM); }
    else if (a < *next) *next = a;
}
static void alarm_fn(void *arg) {
    for (;;) {
        uint64_t g = __atomic_load_n(&alarm_gen, __ATOMIC_SEQ_CST), next = UINT64_MAX;
        bkl_enter();
        process_list(alarm_scan, &next);
        bkl_exit();
        uint64_t f = sched_wait_lock();
        if (__atomic_load_n(&alarm_gen, __ATOMIC_SEQ_CST) != g) { sched_wait_unlock(f); continue; }
        uint64_t now = time_ns();
        if (next != UINT64_MAX && next <= now) { sched_wait_unlock(f); continue; }
        wait_event_timeout_locked(&alarm_wq, next == UINT64_MAX ? UINT64_MAX : next - now, f);
    }
}
void alarm_changed(void) {
    if (!alarm_thread) alarm_thread = thread_create("alarm", alarm_fn, nullptr);
    __atomic_add_fetch(&alarm_gen, 1, __ATOMIC_SEQ_CST);
    wake_up(&alarm_wq);
}

int64_t sys_alarm(unsigned secs) {
    uint64_t now = time_ns(), old = curproc->alarm_ns;
    curproc->alarm_ns = secs ? now + secs * 1000000000ULL : 0;
    alarm_changed();
    return old > now ? (old - now + 999999999ULL) / 1000000000ULL : 0;
}
int64_t sys_setitimer(int which, const int64_t *unew, int64_t *uold) {
    if (which != 0) return -EINVAL;
    int64_t n[4] = {0}, o[4] = {0};
    uint64_t now = time_ns();
    if (curproc->alarm_ns > now) { uint64_t l = curproc->alarm_ns - now; o[2] = l / 1000000000ULL; o[3] = (l % 1000000000ULL) / 1000; }
    if (uold && copy_to_user(uold, o, sizeof o)) return -EFAULT;
    if (unew) {
        if (copy_from_user(n, unew, sizeof n)) return -EFAULT;
        uint64_t ns = n[2] * 1000000000ULL + n[3] * 1000ULL;
        curproc->alarm_ns = ns ? now + ns : 0;
        alarm_changed();
    }
    return 0;
}
int64_t sys_getitimer(int which, int64_t *ucur) {
    int64_t o[4] = {0};
    uint64_t now = time_ns();
    if (curproc->alarm_ns > now) { uint64_t l = curproc->alarm_ns - now; o[2] = l / 1000000000ULL; o[3] = (l % 1000000000ULL) / 1000; }
    return copy_to_user(ucur, o, sizeof o);
}

/* 64-bit parameters: musl passes the magic 0xfee1dead zero-extended, while riscv64 code expects
 * 32-bit values sign-extended in registers - truncate explicitly */
int64_t sys_reboot(uint64_t m1_, uint64_t m2_, uint64_t cmd_, void *arg) {
    uint32_t m1 = (uint32_t)m1_, m2 = (uint32_t)m2_, cmd = (uint32_t)cmd_;
    if (!capable(CAP_SYS_BOOT)) return -EPERM;
    if (m1 != 0xfee1dead || (m2 != 672274793 && m2 != 85072278 && m2 != 369367448 && m2 != 537993216)) return -EINVAL;
    if (cmd == 0x4321fedc || cmd == 0xcdef0123 || cmd == 0x01234567) vfs_shutdown();   /* M30: nothing dirty is lost, disks clean */
    if (cmd == 0x4321fedc || cmd == 0xcdef0123) { pr_info("system halted\n"); acpi_poweroff(); arch_poweroff(); arch_halt_forever(); }
    if (cmd == 0x01234567) { pr_info("rebooting\n"); acpi_reboot(); arch_reboot(); arch_halt_forever(); }
    return 0;
}

/* ---- futex (process-private, keyed by mm + virtual address) ----
 * futex(2) runs without the BKL. Waiters hang off one of FUTEX_BUCKETS hashed buckets; each
 * bucket has an IRQ-off lock spun with spin_lock_ipi() because FUTEX_WAIT reads the user word
 * (mm lock, possibly TLB-shootdown waits) while holding it. Holding the bucket lock across
 * "read *uaddr, enqueue" and across "dequeue, set woken, wake_up" makes the value check and
 * the wakeup atomic with respect to each other. The sleep itself uses the wait_until_sl()
 * pattern (woken is re-checked under sched_lock). A waiter always retakes its bucket lock
 * before returning, so a waker that still holds the lock never touches a dead stack frame. */
#define FUTEX_BUCKETS 64
struct futex_bucket;
struct futex_waiter { struct list_node node; struct mm *mm; uint32_t *addr; struct wait_queue wq; bool woken; struct futex_bucket *bucket; };
struct futex_bucket { spinlock_t lock; struct list_node head; } __attribute__((aligned(64)));
static struct futex_bucket futex_buckets[FUTEX_BUCKETS];
static volatile bool futex_ready;
static spinlock_t futex_init_lock = SPINLOCK_INIT;
static const struct lock_class futex_class = { "futex_bucket", LR_FUTEX, true };   /* requeue nests two buckets in address order */
uint64_t futex_waits, futex_wakes;      /* approximate counters for /proc/sched-style debugging */

static struct futex_bucket *futex_bucket(struct mm *mm, uint32_t *uaddr) {
    if (!__atomic_load_n(&futex_ready, __ATOMIC_ACQUIRE)) {
        uint64_t f = spin_lock_irqsave(&futex_init_lock);
        if (!futex_ready) {
            for (int i = 0; i < FUTEX_BUCKETS; i++) { spin_lock_init_class(&futex_buckets[i].lock, &futex_class); list_init(&futex_buckets[i].head); }
            __atomic_store_n(&futex_ready, true, __ATOMIC_RELEASE);
        }
        spin_unlock_irqrestore(&futex_init_lock, f);
    }
    uint64_t k = ((uint64_t)(uintptr_t)uaddr >> 2) ^ ((uint64_t)(uintptr_t)mm >> 6);
    k *= 0x9e3779b97f4a7c15ULL;
    return &futex_buckets[k >> 58];      /* top 6 bits: 64 buckets */
}
static uint64_t fb_lock(struct futex_bucket *b) { uint64_t f = arch_irq_save(); spin_lock_ipi(&b->lock); return f; }
static void fb_unlock(struct futex_bucket *b, uint64_t f) { spin_unlock(&b->lock); arch_irq_restore(f); }

static int futex_wake_mm(struct mm *mm, uint32_t *uaddr, int n) {
    if (n <= 0) return 0;
    struct futex_bucket *b = futex_bucket(mm, uaddr);
    int woken = 0;
    uint64_t f = fb_lock(b);
    list_for_each_safe(it, tmp, &b->head) {
        if (woken >= n) break;
        struct futex_waiter *w = list_entry(it, struct futex_waiter, node);
        if (w->addr != uaddr || w->mm != mm) continue;
        list_del(&w->node);
        __atomic_store_n(&w->woken, true, __ATOMIC_RELEASE);
        wake_up(&w->wq);            /* under the bucket lock: w stays alive until we drop it */
        woken++;
    }
    fb_unlock(b, f);
    __atomic_fetch_add(&futex_wakes, (uint64_t)woken, __ATOMIC_RELAXED);
    return woken;
}
int futex_wake(uint32_t *uaddr, int n) { return futex_wake_mm(curproc->mm, uaddr, n); }

/* M33: shared futexes (no FUTEX_PRIVATE_FLAG) in MAP_SHARED file/shmem mappings are keyed by
 * (inode, file offset) so that waiters in different processes meet; the (mm, addr) pair of a
 * waiter is an opaque key here: mm = inode | 1, addr = offset; other shared mappings (device
 * memory, driver pages) use mm = 3, addr = physical address. Everything else keys by mm + address. */
struct futex_key { struct mm *mm; uint32_t *addr; };
static struct futex_key futex_key_of(uint32_t *uaddr, bool shared) {
    struct mm *mm = curproc->mm;
    struct futex_key k = { mm, uaddr };
    if (!shared) return k;
    mm_lock(mm);
    struct vma *v = vma_find(mm, (vaddr_t)uaddr);
    if (v && (vaddr_t)uaddr >= v->start && (v->flags & VMA_SHARED) && v->file && v->file->inode) {
        uint64_t off = (v->pgoff << PAGE_SHIFT) + ((vaddr_t)uaddr - v->start);
        k.mm = (struct mm *)((uintptr_t)v->file->inode | 1);
        k.addr = (uint32_t *)(uintptr_t)off;
    } else if (v && (vaddr_t)uaddr >= v->start && (v->flags & VMA_SHARED)) {
        /* device memory / driver pages: always resident and never moved, key by physical address */
        paddr_t pa; unsigned fl;
        if (vmm_query(mm->pt, (vaddr_t)uaddr & ~(vaddr_t)(PAGE_SIZE - 1), &pa, &fl)) {
            k.mm = (struct mm *)(uintptr_t)3;
            k.addr = (uint32_t *)(uintptr_t)((pa & ~(paddr_t)(PAGE_SIZE - 1)) + ((vaddr_t)uaddr & (PAGE_SIZE - 1)));
        }
    }
    mm_unlock(mm);
    return k;
}
/* robust futex owner death: wake under both keys (the waiter may use either flavour) */
int futex_wake_any(uint32_t *uaddr, int n) {
    struct futex_key k = futex_key_of(uaddr, true);
    int r = futex_wake_mm(k.mm, k.addr, n);
    if (k.mm != curproc->mm && r < n) r += futex_wake_mm(curproc->mm, uaddr, n - r);
    return r;
}

/* move up to n waiters from key a to key b (FUTEX_REQUEUE): a waiter's bucket may change, so
 * it records which bucket currently holds it (w->bucket) and re-checks that when it leaves */
static int futex_requeue(struct futex_key a, struct futex_key b, int n) {
    struct futex_bucket *ba = futex_bucket(a.mm, a.addr), *bb = futex_bucket(b.mm, b.addr);
    if (n <= 0) return 0;
    /* lock both buckets in address order */
    struct futex_bucket *l1 = ba < bb ? ba : bb, *l2 = ba < bb ? bb : ba;
    uint64_t f = arch_irq_save();
    spin_lock_ipi(&l1->lock);
    if (l2 != l1) spin_lock_ipi(&l2->lock);
    int moved = 0;
    list_for_each_safe(it, tmp, &ba->head) {
        if (moved >= n) break;
        struct futex_waiter *w = list_entry(it, struct futex_waiter, node);
        if (w->addr != a.addr || w->mm != a.mm) continue;
        list_del(&w->node);
        w->mm = b.mm; w->addr = b.addr;
        __atomic_store_n(&w->bucket, bb, __ATOMIC_RELEASE);
        list_add_tail(&bb->head, &w->node);
        moved++;
    }
    if (l2 != l1) spin_unlock(&l2->lock);
    spin_unlock(&l1->lock);
    arch_irq_restore(f);
    return moved;
}

/* lock the bucket that currently holds w (it can be requeued concurrently) */
static struct futex_bucket *fw_lock(struct futex_waiter *w, uint64_t *f) {
    for (;;) {
        struct futex_bucket *b = __atomic_load_n(&w->bucket, __ATOMIC_ACQUIRE);
        *f = fb_lock(b);
        if (__atomic_load_n(&w->bucket, __ATOMIC_ACQUIRE) == b) return b;
        fb_unlock(b, *f);
    }
}

static int futex_wait(uint32_t *uaddr, uint32_t val, uint64_t ns, bool shared) {
    if ((uintptr_t)uaddr & 3) return -EINVAL;
    struct futex_key key = futex_key_of(uaddr, shared);
    struct mm *mm = key.mm;
    struct futex_bucket *b = futex_bucket(mm, key.addr);
    struct futex_waiter w = { .mm = mm, .addr = key.addr, .bucket = b };
    wait_queue_init(&w.wq);
    uint64_t f = fb_lock(b);
    uint32_t cur;
    if (copy_from_user(&cur, uaddr, 4)) { fb_unlock(b, f); return -EFAULT; }
    if (cur != val) { fb_unlock(b, f); return -EAGAIN; }
    list_add_tail(&b->head, &w.node);
    fb_unlock(b, f);
    __atomic_fetch_add(&futex_waits, 1, __ATOMIC_RELAXED);

    uint64_t deadline = ns == UINT64_MAX ? UINT64_MAX : time_ns() + ns;
    int r = 0;
    for (;;) {
        if (__atomic_load_n(&w.woken, __ATOMIC_ACQUIRE)) break;
        uint64_t g = sched_wait_lock();
        if (__atomic_load_n(&w.woken, __ATOMIC_ACQUIRE)) { sched_wait_unlock(g); break; }
        uint64_t left = UINT64_MAX;
        if (deadline != UINT64_MAX) {
            uint64_t now = time_ns();
            if (now >= deadline) { sched_wait_unlock(g); r = -ETIMEDOUT; break; }
            left = deadline - now;
        }
        if ((r = wait_event_timeout_locked(&w.wq, left, g))) break;
    }
    b = fw_lock(&w, &f);
    bool woken = w.woken;
    if (!woken) list_del(&w.node);
    fb_unlock(b, f);
    return woken ? 0 : r;
}

int64_t sys_futex(uint32_t *uaddr, int op, uint32_t val, const struct timespec *uts, uint32_t *uaddr2, uint32_t val3) {
    int cmd = op & 0x7f;    /* FUTEX_PRIVATE_FLAG (128) / CLOCK_REALTIME (256) masked off */
    bool shared = !(op & 128);
    switch (cmd) {
    case 0: case 9: {    /* FUTEX_WAIT, FUTEX_WAIT_BITSET */
        uint64_t ns = UINT64_MAX;
        if (cmd == 9 && !val3) return -EINVAL;
        if (uts) {
            struct timespec ts;
            if (copy_from_user(&ts, uts, sizeof ts)) return -EFAULT;
            if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L) return -EINVAL;
            ns = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
            if (cmd == 9) {     /* absolute deadline */
                uint64_t now = (op & 256) ? time_ns() + (uint64_t)boot_epoch * 1000000000ULL : time_ns();
                ns = ns > now ? ns - now : 0;
            }
        }
        return futex_wait(uaddr, val, ns, shared);
    }
    case 1: case 10:     /* FUTEX_WAKE(_BITSET) */
        if (cmd == 10 && !val3) return -EINVAL;
        { struct futex_key k = futex_key_of(uaddr, shared); return futex_wake_mm(k.mm, k.addr, (int)MIN(val, (uint32_t)INT32_MAX)); }
    case 3: case 4: {    /* FUTEX_REQUEUE / CMP_REQUEUE: wake val, then wake (instead of move) the rest */
        if (cmd == 4) {
            struct futex_bucket *b = futex_bucket(curproc->mm, uaddr);
            uint64_t f = fb_lock(b);
            uint32_t cur;
            int e = copy_from_user(&cur, uaddr, 4) ? -EFAULT : cur != val3 ? -EAGAIN : 0;
            fb_unlock(b, f);
            if (e) return e;
        }
        struct futex_key k = futex_key_of(uaddr, shared), k2 = futex_key_of(uaddr2, shared);
        if ((uintptr_t)uaddr2 & 3) return -EINVAL;
        int n = futex_wake_mm(k.mm, k.addr, (int)MIN(val, (uint32_t)INT32_MAX));
        int lim = (int)MIN((uint64_t)(uintptr_t)uts, (uint64_t)INT32_MAX);    /* val2 travels in the timeout slot */
        return n + futex_requeue(k, k2, lim);
    }
    default: return -ENOSYS;
    }
}

int64_t sys_arch_prctl_wrap(int code, uint64_t addr) {
#if defined(__x86_64__)
    return sys_arch_prctl(code, addr);
#else
    return -EINVAL;
#endif
}


/* membarrier(2): every other CPU passes through the scheduler (a full barrier) soon after the
 * reschedule IPI; QUERY reports GLOBAL and the PRIVATE_EXPEDITED family. */
#include <kernel/cpu.h>
int64_t sys_membarrier(int cmd, unsigned flags, int cpu_id) {
    if (cmd == 0) return 1 | 2 | 8 | 16;    /* GLOBAL, GLOBAL_EXPEDITED, PRIVATE_EXPEDITED, REGISTER_PRIVATE_EXPEDITED */
    if (cmd == 16 || cmd == 4) return 0;    /* REGISTER_* */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (int i = 0; i < ncpus; i++) if (&cpus[i] != this_cpu()) smp_send_resched(&cpus[i]);
    return 0;
}
