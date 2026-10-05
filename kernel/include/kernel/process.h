#pragma once
#include <kernel/types.h>
#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <kernel/sched.h>
#include <kernel/signal.h>

#define MAX_FDS 256

struct mm;
struct file;
struct inode;

enum proc_state { P_ALIVE, P_ZOMBIE };

struct process {
    int pid, pgid, sid;
    enum proc_state state;
    bool stopped, stop_reported, cont_reported;
    int stop_sig;
    int exit_status;                  /* wait4 status word */
    struct process *parent;
    struct list_node children;
    struct list_node sibling;
    struct list_node threads;
    struct list_node all_node;
    struct wait_queue child_wait;     /* parent sleeps here in wait4 */
    struct mm *mm;
    struct file *fds[MAX_FDS];
    spinlock_t fd_lock;     /* slot updates vs. lock-free fd_get_ref(); mutators also hold the BKL */
    uint64_t cloexec[MAX_FDS / 64];
    struct inode *cwd;
    struct inode *root;
    uint32_t umask;
    uint32_t uid, gid, euid, egid;
    struct k_sigaction sigactions[NSIG];
    uint64_t sig_pending;
    struct tty *ctty;
    char name[32];
    char *cmdline;                    /* NUL-separated argv for /proc/pid/cmdline */
    size_t cmdline_len;
    char *exe;
    uint64_t start_ticks;
    uint64_t utime_ns, stime_ns;      /* tick-sampled user/system split, weighted by elapsed time */
    uint64_t cutime_ns, cstime_ns;    /* reaped children (and their reaped children) */
    uint64_t sum_exec_ns;                   /* precise on-CPU time of switched-out slices */
    uint64_t min_flt, cmin_flt, nvcsw, nivcsw, cnvcsw, cnivcsw;
    uint64_t alarm_ns;
    struct vfork_done { bool done; struct wait_queue wq; } *vfork;
};


/* resource usage in kernel units, turned into struct rusage by rusage_to_user() */
int rusage_to_user(void *u, const struct rusage_k *r);

struct process *process_current(void);
#define curproc (current->proc)
struct process *process_find(int pid);
struct thread *process_find_thread(int tid);  /* caller holds BKL */
struct process *process_create_init(const char *path);
int64_t do_wait(int pid, int *ustatus, int options, int *out_pid);
__noreturn void thread_exit_only(void);
void process_exit(int status) __attribute__((noreturn));
int process_fork(struct trap_frame *f, uint64_t flags, uint64_t newsp, int *ptid, int *ctid, uint64_t tls);
void process_list(void (*fn)(struct process *, void *), void *ctx);
void process_init(void);
