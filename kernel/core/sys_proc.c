/* Process, time and miscellaneous system calls. */
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
    int64_t r = do_wait(pid, status, options, nullptr);
    if (r >= 0 && rusage && rusage_to_user(rusage, &current->reaped_ru)) return -EFAULT;
    return r;
}

int64_t sys_waitid(int idtype, int id, void *uinfo, int options, void *ru) {
    int pid = idtype == 0 ? -1 : idtype == 1 ? id : -id;
    int status;
    int64_t r = do_wait(pid, &status, options & ~0x4, nullptr);  /* WEXITED */
    if (r < 0) return r;
    if (uinfo) {
        int32_t si[32] = {0};
        if (r > 0) {
            si[0] = SIGCHLD;
            si[4] = (int)r;
            if ((status & 0x7f) == 0) { si[2] = 1; si[6] = (status >> 8) & 0xff; }        /* CLD_EXITED */
            else if ((status & 0xff) == 0x7f) { si[2] = 5; si[6] = (status >> 8) & 0xff; } /* CLD_STOPPED */
            else { si[2] = 2; si[6] = status & 0x7f; }                                       /* CLD_KILLED */
        }
        if (copy_to_user(uinfo, si, sizeof si)) return -EFAULT;
    }
    return 0;
}

int64_t sys_getpid(void) { return curproc->pid; }
int64_t sys_gettid(void) { return current->tid; }
int64_t sys_getppid(void) { return curproc->parent ? curproc->parent->pid : 0; }
int64_t sys_getuid(void) { return curproc->uid; }
int64_t sys_geteuid(void) { return curproc->euid; }
int64_t sys_getgid(void) { return curproc->gid; }
int64_t sys_getegid(void) { return curproc->egid; }
int64_t sys_setuid(uint32_t u) { curproc->uid = curproc->euid = u; return 0; }
int64_t sys_setgid(uint32_t g) { curproc->gid = curproc->egid = g; return 0; }
int64_t sys_setreuid(uint32_t r, uint32_t e) { if (r != (uint32_t)-1) curproc->uid = r; if (e != (uint32_t)-1) curproc->euid = e; return 0; }
int64_t sys_setregid(uint32_t r, uint32_t e) { if (r != (uint32_t)-1) curproc->gid = r; if (e != (uint32_t)-1) curproc->egid = e; return 0; }
int64_t sys_setresuid(uint32_t r, uint32_t e, uint32_t s) { return sys_setreuid(r, e); }
int64_t sys_setresgid(uint32_t r, uint32_t e, uint32_t s) { return sys_setregid(r, e); }
int64_t sys_getresuid(uint32_t *r, uint32_t *e, uint32_t *s) {
    if (copy_to_user(r, &curproc->uid, 4) || copy_to_user(e, &curproc->euid, 4) || copy_to_user(s, &curproc->euid, 4)) return -EFAULT;
    return 0;
}
int64_t sys_getresgid(uint32_t *r, uint32_t *e, uint32_t *s) {
    if (copy_to_user(r, &curproc->gid, 4) || copy_to_user(e, &curproc->egid, 4) || copy_to_user(s, &curproc->egid, 4)) return -EFAULT;
    return 0;
}
int64_t sys_getgroups(int size, uint32_t *list) { return 0; }
int64_t sys_setgroups(int size, const uint32_t *list) { return 0; }
int64_t sys_setfsuid(uint32_t u) { return curproc->euid; }
int64_t sys_setfsgid(uint32_t g) { return curproc->egid; }

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
    if (len > 64) return -EINVAL;
    char k[65] = {0};
    if (copy_from_user(k, name, len)) return -EFAULT;
    memcpy(hostname, k, 65);
    return 0;
}

int64_t sys_sched_yield(void) { schedule(); return 0; }

static struct wait_queue sleep_wq = WAIT_QUEUE_INIT(sleep_wq);

static int64_t sleep_interruptible(uint64_t ns, struct timespec *urem) {
    uint64_t end = time_ns() + ns;
    int r = ns ? wait_event_timeout(&sleep_wq, ns) : 0;
    if (r == -EINTR) {
        if (urem) {
            uint64_t now = time_ns(), left = end > now ? end - now : 0;
            struct timespec rem = { left / 1000000000ULL, left % 1000000000ULL };
            copy_to_user(urem, &rem, sizeof rem);
        }
        return -EINTR;
    }
    return 0;
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
int64_t sys_set_robust_list(void *h, size_t len) { return 0; }
int64_t sys_rseq(void) { return -ENOSYS; }
int64_t sys_prctl(int opt, uint64_t a2) {
    if (opt == 15) {   /* PR_SET_NAME */
        char n[16] = {0};
        strncpy_from_user(n, (const char *)a2, 15);
        strlcpy(current->name, n, sizeof current->name);
        return 0;
    }
    if (opt == 16) return copy_to_user((void *)a2, current->name, 16);   /* PR_GET_NAME */
    return -EINVAL;
}
int64_t sys_personality(uint64_t p) { return 0; }
int64_t sys_capget(void *h, void *d) { if (d) { uint32_t c[6] = { ~0u, ~0u, ~0u, ~0u, ~0u, ~0u }; copy_to_user(d, c, sizeof c); } return 0; }
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
    return sched_set_policy(t, t->policy, prio, t->nice);
}
int64_t sys_sched_getparam(int pid, int *uparam) {
    struct thread *t = affinity_target(pid);
    if (!t) return -ESRCH;
    int prio = t->rt_prio;
    return copy_to_user(uparam, &prio, sizeof prio);
}
int64_t sys_sched_get_priority_max(int policy) { return policy == 1 || policy == 2 ? 99 : 0; }
int64_t sys_sched_get_priority_min(int policy) { return policy == 1 || policy == 2 ? 1 : 0; }
int64_t sys_sched_rr_get_interval(int pid, struct timespec *uts) {
    struct thread *t = affinity_target(pid);
    if (!t) return -ESRCH;
    uint64_t ms = t->policy == 1 ? 0 : t->policy == 2 ? SCHED_RR_QUANTUM : SCHED_QUANTUM;
    struct timespec ts = { 0, (int64_t)ms * 1000000 };
    return copy_to_user(uts, &ts, sizeof ts);
}
/* pid 0 = calling thread; otherwise the thread with that tid in the process of that pid */
static struct thread *affinity_target(int pid) {
    if (!pid || pid == current->tid) return current;
    struct process *p = process_find(pid);
    if (!p) return nullptr;
    list_for_each(it, &p->threads) {
        struct thread *t = list_entry(it, struct thread, proc_node);
        if (t->tid == pid) return t;
    }
    return nullptr;
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
int64_t sys_madvise(void) { return 0; }
int64_t sys_zero(void) { return 0; }

int64_t sys_alarm(unsigned secs) {
    uint64_t now = time_ns(), old = curproc->alarm_ns;
    curproc->alarm_ns = secs ? now + secs * 1000000000ULL : 0;
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
    }
    return 0;
}
int64_t sys_getitimer(int which, int64_t *ucur) {
    int64_t o[4] = {0};
    uint64_t now = time_ns();
    if (curproc->alarm_ns > now) { uint64_t l = curproc->alarm_ns - now; o[2] = l / 1000000000ULL; o[3] = (l % 1000000000ULL) / 1000; }
    return copy_to_user(ucur, o, sizeof o);
}

int64_t sys_reboot(int m1, int m2, unsigned cmd, void *arg) {
    if (cmd == 0x4321fedc || cmd == 0xcdef0123) { pr_info("system halted\n"); acpi_poweroff(); arch_poweroff(); arch_halt_forever(); }
    if (cmd == 0x01234567) { pr_info("rebooting\n"); acpi_reboot(); arch_reboot(); arch_halt_forever(); }
    return 0;
}

/* ---- futex (process-private, keyed by virtual address) ---- */
#define FUTEX_BUCKETS 64
struct futex_waiter { struct list_node node; struct mm *mm; uint32_t *addr; struct wait_queue wq; bool woken; };
static struct list_node futex_list = LIST_INIT(futex_list);

int futex_wake(uint32_t *uaddr, int n) {
    int woken = 0;
    uint64_t f = arch_irq_save();
    list_for_each_safe(it, tmp, &futex_list) {
        struct futex_waiter *w = list_entry(it, struct futex_waiter, node);
        if (w->addr == uaddr && w->mm == curproc->mm && woken < n) {
            list_del(&w->node);
            w->woken = true;
            wake_up(&w->wq);
            woken++;
        }
    }
    arch_irq_restore(f);
    return woken;
}

int64_t sys_futex(uint32_t *uaddr, int op, uint32_t val, const struct timespec *uts, uint32_t *uaddr2, uint32_t val3) {
    int cmd = op & 0x7f;
    switch (cmd) {
    case 0: case 9: {    /* FUTEX_WAIT, FUTEX_WAIT_BITSET */
        uint64_t ns = UINT64_MAX;
        if (uts) {
            struct timespec ts;
            if (copy_from_user(&ts, uts, sizeof ts)) return -EFAULT;
            ns = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
            if (cmd == 9) { uint64_t now = time_ns(); ns = ns > now ? ns - now : 0; }
        }
        uint64_t f = arch_irq_save();
        uint32_t cur;
        if (copy_from_user(&cur, uaddr, 4)) { arch_irq_restore(f); return -EFAULT; }
        if (cur != val) { arch_irq_restore(f); return -EAGAIN; }
        struct futex_waiter w = { .mm = curproc->mm, .addr = uaddr };
        wait_queue_init(&w.wq);
        list_add_tail(&futex_list, &w.node);
        int r = wait_event_timeout(&w.wq, ns);
        if (!w.woken) list_del(&w.node);
        arch_irq_restore(f);
        if (w.woken) return 0;
        return r == -ETIMEDOUT ? -ETIMEDOUT : r ? r : 0;
    }
    case 1: case 10: return futex_wake(uaddr, (int)val);     /* FUTEX_WAKE(_BITSET) */
    case 3: case 4: {    /* FUTEX_REQUEUE / CMP_REQUEUE: wake all as a simplification */
        int n = futex_wake(uaddr, (int)val);
        return n + futex_wake(uaddr2, INT32_MAX);
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
