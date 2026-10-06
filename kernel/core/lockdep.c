/*
 * Debug lock-order checker ("lockdep lite", M24). Every classified lock has a rank (see
 * kernel/spinlock.h); acquiring a lock whose rank is not above every lock already held on
 * this CPU (spinlocks) or by this thread (mutexes) is a lock-order violation, as is taking a
 * sleeping lock while holding a spinlock, or the BKL while holding any classified spinlock.
 * Each offending pair is reported once on the console and listed in /proc/lockdep; tests
 * check the violation count stays zero.
 */
#include <kernel/mutex.h>
#include <kernel/printk.h>
#include <kernel/string.h>

static uint64_t ld_checks, ld_violations, ld_overflows;
#define MAX_REPORTS 32
static struct { const struct lock_class *held, *want; const char *what; } reports[MAX_REPORTS];
static int nreports;
static spinlock_t report_lock = SPINLOCK_INIT;     /* unclassified */

const struct lock_class bkl_class = { "bkl", 0, false };

static void report(const char *what, const struct lock_class *held, const struct lock_class *want) {
    uint64_t f = spin_lock_irqsave(&report_lock);
    for (int i = 0; i < nreports; i++)
        if (reports[i].held == held && reports[i].want == want && reports[i].what == what) { spin_unlock_irqrestore(&report_lock, f); return; }
    if (nreports < MAX_REPORTS) { reports[nreports].held = held; reports[nreports].want = want; reports[nreports].what = what; nreports++; }
    ld_violations++;
    spin_unlock_irqrestore(&report_lock, f);
    printk("lockdep: %s: acquiring %s (rank %d) while holding %s (rank %d) in %s\n", what,
           want->name, want->rank, held ? held->name : "-", held ? held->rank : -1,
           current ? current->name : "?");
}

static bool bad_order(const struct lock_class *h, const struct lock_class *c) {
    if (h == c) return !c->nest;
    return h->rank >= c->rank;
}

void lockdep_acquire(const struct lock_class *c, bool sleeping) {
    uint64_t f = arch_irq_save();
    struct cpu *cpu = this_cpu();
    struct thread *t = current;
    if (!cpu || cpu->ld_busy) { arch_irq_restore(f); return; }
    cpu->ld_busy = true;
    ld_checks++;
    if (c == &bkl_class) {
        if (cpu->ld_nspin) report("BKL under spinlock", cpu->ld_spin[cpu->ld_nspin - 1], c);
        goto out;                           /* the BKL itself is tracked by bkl_depth */
    }
    if (sleeping && cpu->ld_nspin) report("sleeping lock in atomic context", cpu->ld_spin[cpu->ld_nspin - 1], c);
    for (int i = 0; i < cpu->ld_nspin; i++)
        if (bad_order(cpu->ld_spin[i], c)) { report("lock order", cpu->ld_spin[i], c); break; }
    if (t)
        for (int i = 0; i < t->ld_nheld; i++)
            if (bad_order(t->ld_held[i], c)) { report("lock order", t->ld_held[i], c); break; }
    if (sleeping) {
        if (t && t->ld_nheld < (int)(sizeof t->ld_held / sizeof t->ld_held[0])) t->ld_held[t->ld_nheld++] = c;
        else ld_overflows++;
    } else {
        if (cpu->ld_nspin < (int)(sizeof cpu->ld_spin / sizeof cpu->ld_spin[0])) cpu->ld_spin[cpu->ld_nspin++] = c;
        else ld_overflows++;
    }
out:
    cpu->ld_busy = false;
    arch_irq_restore(f);
}

static void pop(const struct lock_class **st, int *n, const struct lock_class *c) {
    for (int i = *n - 1; i >= 0; i--)
        if (st[i] == c) {
            for (int j = i; j < *n - 1; j++) st[j] = st[j + 1];
            (*n)--;
            return;
        }
}

void lockdep_release(const struct lock_class *c, bool sleeping) {
    uint64_t f = arch_irq_save();
    struct cpu *cpu = this_cpu();
    struct thread *t = current;
    if (cpu && !cpu->ld_busy) {
        cpu->ld_busy = true;
        if (sleeping) { if (t) pop(t->ld_held, &t->ld_nheld, c); }
        else pop(cpu->ld_spin, &cpu->ld_nspin, c);
        cpu->ld_busy = false;
    }
    arch_irq_restore(f);
}

uint64_t mutex_contended_count(void);
void lockdep_get_stats(struct lockdep_stats *s) {
    s->checks = ld_checks; s->violations = ld_violations; s->overflows = ld_overflows;
    s->mutex_contended = mutex_contended_count();
}

int lockdep_report(char *buf, int max) {
    int n = snprintf(buf, max, "checks %lu\nviolations %lu\noverflows %lu\nmutex_contended %lu\n",
                     (unsigned long)ld_checks, (unsigned long)ld_violations, (unsigned long)ld_overflows,
                     (unsigned long)mutex_contended_count());
    uint64_t f = spin_lock_irqsave(&report_lock);
    for (int i = 0; i < nreports && n < max; i++)
        n += snprintf(buf + n, max - n, "%s: %s(%d) -> %s(%d)\n", reports[i].what,
                      reports[i].held ? reports[i].held->name : "-", reports[i].held ? reports[i].held->rank : -1,
                      reports[i].want->name, reports[i].want->rank);
    spin_unlock_irqrestore(&report_lock, f);
    return n < max ? n : max;
}
