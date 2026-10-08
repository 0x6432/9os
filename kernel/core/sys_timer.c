/* POSIX per-process timers (M33): timer_create/settime/gettime/getoverrun/delete.
 * Timers are scanned by the "alarm" kernel thread (sys_proc.c), which sleeps until the
 * earliest deadline of setitimer/alarm and armed POSIX timers. Expiry queues a SI_TIMER
 * signal; while that signal is still pending further expiries only count overruns, which
 * are handed to the siginfo when it is dequeued (posix_timer_dequeued). Everything runs
 * under the BKL with interrupts off around signal queue accesses. */
#include <kernel/process.h>
#include <kernel/syscall.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/time.h>
#include <kernel/mm.h>

#define SIGEV_SIGNAL 0
#define SIGEV_NONE 1
#define SIGEV_THREAD 2
#define SIGEV_THREAD_ID 4
#define TIMER_ABSTIME 1
#define DELAYTIMER_MAX 0x7fffffff

struct ptimer {
    struct list_node node;          /* process->timers */
    int id, clock, notify, signo, tid;
    uint64_t sigval;
    bool armed, queued;
    uint64_t expires, interval;     /* monotonic ns */
    int64_t overrun, last_overrun;
};

void alarm_changed(void);

static uint64_t clock_off(int clock) { return clock == 0 || clock == 8 ? (uint64_t)boot_epoch * 1000000000ull : 0; }
static uint64_t ts2ns(const struct timespec *t) { return t->tv_sec * 1000000000ull + t->tv_nsec; }
static struct timespec ns2ts(uint64_t ns) { return (struct timespec){ ns / 1000000000ull, ns % 1000000000ull }; }

static struct ptimer *find_timer(struct process *p, int id) {
    list_for_each(it, &p->timers) {
        struct ptimer *t = list_entry(it, struct ptimer, node);
        if (t->id == id) return t;
    }
    return nullptr;
}

static struct thread *timer_thread(struct process *p, struct ptimer *t) {
    list_for_each(it, &p->threads) {
        struct thread *th = list_entry(it, struct thread, proc_node);
        if (th->tid == t->tid) return th;
    }
    return nullptr;
}

static void timer_fire(struct process *p, struct ptimer *t, uint64_t now) {
    uint64_t n = 1;
    if (t->interval) { n += (now - t->expires) / t->interval; t->expires += n * t->interval; }
    else t->armed = false;
    if (t->notify == SIGEV_NONE) return;
    struct thread *th = t->notify == SIGEV_THREAD_ID ? timer_thread(p, t) : nullptr;
    uint64_t bit = SIGBIT(t->signo);
    if (t->queued && !((p->sig_pending | (th ? th->sig_pending : 0)) & bit)) t->queued = false;  /* merged away */
    if (t->queued) {
        t->overrun = MIN(t->overrun + (int64_t)n, (int64_t)DELAYTIMER_MAX);
        return;
    }
    t->overrun = MIN((int64_t)n - 1, (int64_t)DELAYTIMER_MAX);
    struct ksiginfo ki = { .signo = t->signo, .code = SI_TIMER, .i2 = t->id, .v = t->sigval };
    t->queued = true;
    if (th) { if (signal_thread_info(th, &ki)) t->queued = false; }
    else signal_send_info(p, &ki);
}

/* called by the alarm thread for every process (BKL held) */
void posix_timers_scan(struct process *p, uint64_t *next) {
    if (p->state == P_ZOMBIE || !p->timers.next) return;
    uint64_t now = time_ns();
    list_for_each(it, &p->timers) {
        struct ptimer *t = list_entry(it, struct ptimer, node);
        if (!t->armed) continue;
        if (t->expires <= now) timer_fire(p, t, now);
        if (t->armed && t->expires < *next) *next = t->expires;
    }
}

/* a SI_TIMER signal leaves the queue: report and reset the overrun count */
void posix_timer_dequeued(struct process *p, struct ksiginfo *ki) {
    if (!p || !p->timers.next) return;
    struct ptimer *t = find_timer(p, ki->i2);
    if (!t || t->signo != ki->signo) return;
    ki->i1 = (int32_t)t->overrun;
    t->last_overrun = t->overrun;
    t->overrun = 0;
    t->queued = false;
}

void posix_timers_exit(struct process *p) {
    if (!p->timers.next) return;
    list_for_each_safe(it, tmp, &p->timers) {
        struct ptimer *t = list_entry(it, struct ptimer, node);
        list_del(&t->node);
        kfree(t);
    }
}
void posix_timers_exec(struct process *p) { posix_timers_exit(p); }

int64_t sys_timer_create(int clock, const void *usev, int *uid) {
    if (clock != 0 && clock != 1 && clock != 4 && clock != 7 && clock != 8 && clock != 9) return clock == 2 || clock == 3 ? -ENOTSUP : -EINVAL;
    struct process *p = curproc;
    struct ptimer *t = kzalloc(sizeof *t);
    if (!t) return -ENOMEM;
    t->clock = clock;
    t->notify = SIGEV_SIGNAL;
    t->signo = SIGALRM;
    t->id = p->next_timer_id;
    if (usev) {
        struct { uint64_t value; int32_t signo, notify, tid, pad; } sev;
        if (copy_from_user(&sev, usev, sizeof sev)) { kfree(t); return -EFAULT; }
        t->notify = sev.notify;
        t->signo = sev.signo;
        t->sigval = sev.value;
        t->tid = sev.tid;
        if (t->notify != SIGEV_SIGNAL && t->notify != SIGEV_NONE && t->notify != SIGEV_THREAD_ID) { kfree(t); return -EINVAL; }
        if (t->notify != SIGEV_NONE && (t->signo <= 0 || t->signo >= NSIG)) { kfree(t); return -EINVAL; }
        if (t->notify == SIGEV_THREAD_ID && !timer_thread(p, t)) { kfree(t); return -EINVAL; }
    } else t->sigval = (uint64_t)t->id;
    if (copy_to_user(uid, &t->id, sizeof t->id)) { kfree(t); return -EFAULT; }
    p->next_timer_id++;
    list_add_tail(&p->timers, &t->node);
    return 0;
}

static void timer_get(struct ptimer *t, struct timespec out[2]) {
    uint64_t now = time_ns();
    out[0] = ns2ts(t->interval);
    out[1] = ns2ts(t->armed ? (t->expires > now ? t->expires - now : 1) : 0);
}

int64_t sys_timer_settime(int id, int flags, const struct timespec *unew, struct timespec *uold) {
    struct ptimer *t = find_timer(curproc, id);
    if (!t) return -EINVAL;
    struct timespec nv[2];
    if (copy_from_user(nv, unew, sizeof nv)) return -EFAULT;
    for (int i = 0; i < 2; i++)
        if (nv[i].tv_nsec < 0 || nv[i].tv_nsec >= 1000000000L || nv[i].tv_sec < 0) return -EINVAL;
    if (uold) { struct timespec o[2]; timer_get(t, o); if (copy_to_user(uold, o, sizeof o)) return -EFAULT; }
    uint64_t v = ts2ns(&nv[1]);
    t->armed = false;
    t->interval = ts2ns(&nv[0]);
    if (v) {
        uint64_t now = time_ns();
        if (flags & TIMER_ABSTIME) {
            uint64_t off = clock_off(t->clock);
            t->expires = v > off ? v - off : 0;
            if (t->expires < now) t->expires = now;
        } else t->expires = now + v;
        t->armed = true;
    }
    alarm_changed();
    return 0;
}

int64_t sys_timer_gettime(int id, struct timespec *ucur) {
    struct ptimer *t = find_timer(curproc, id);
    if (!t) return -EINVAL;
    struct timespec o[2];
    timer_get(t, o);
    return copy_to_user(ucur, o, sizeof o) ? -EFAULT : 0;
}

int64_t sys_timer_getoverrun(int id) {
    struct ptimer *t = find_timer(curproc, id);
    return t ? t->last_overrun : -EINVAL;
}

int64_t sys_timer_delete(int id) {
    struct ptimer *t = find_timer(curproc, id);
    if (!t) return -EINVAL;
    list_del(&t->node);
    kfree(t);
    alarm_changed();
    return 0;
}
