/*
 * M28: generic interrupt lines with threaded handlers, MSI vectors, irq_work and
 * /proc/interrupts (see kernel/irq.h). Drivers no longer poll from kernel threads: the hard
 * handler acknowledges the device in interrupt context and the thread handler does the work
 * in a sleepable kernel thread (which, like every kernel thread, runs under the BKL).
 */
#include <kernel/irq.h>
#include <kernel/sched.h>
#include <kernel/cpu.h>
#include <kernel/kmalloc.h>
#include <kernel/printk.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/arch.h>

struct irq_action {
    struct irq_action *next;
    struct irq_desc *desc;
    char name[24];
    irq_hard_t hard;
    irq_thread_t fn;
    void *ctx;
    struct thread *thr;
    struct wait_queue wq;
    int pending;
    uint64_t handled, thread_runs;
};

struct irq_desc {
    int line;              /* GSI / INTID / PLIC source, or -1 for MSI */
    int vector;            /* arch vector (x86) or line */
    const char *kind;
    struct irq_action *actions;
    uint64_t count, unhandled;
};

#define MAX_DESCS 64
static struct irq_desc descs[MAX_DESCS];
static int ndescs;

static void irq_thread(void *arg) {
    struct irq_action *a = arg;
    for (;;) {
        wait_until_sl(&a->wq, __atomic_load_n(&a->pending, __ATOMIC_ACQUIRE));
        __atomic_store_n(&a->pending, 0, __ATOMIC_RELEASE);
        a->fn(a->ctx);
        a->thread_runs++;
    }
}

/* IRQ context, IRQs off, BKL held */
static void desc_dispatch(struct trap_frame *f, void *arg) {
    struct irq_desc *d = arg;
    d->count++;
    this_cpu()->irqs++;
    bool any = false;
    for (struct irq_action *a = d->actions; a; a = a->next) {
        int r = a->hard ? a->hard(a->ctx) : IRQ_WAKE_THREAD;
        if (r == IRQ_NONE) continue;
        any = true;
        a->handled++;
        if (r == IRQ_WAKE_THREAD && a->fn) {
            __atomic_store_n(&a->pending, 1, __ATOMIC_RELEASE);
            wake_up(&a->wq);
        }
    }
    if (!any) d->unhandled++;
    irq_eoi();
}

static struct irq_action *new_action(struct irq_desc *d, const char *name, irq_hard_t hard, irq_thread_t fn, void *ctx) {
    struct irq_action *a = kzalloc(sizeof *a);
    if (!a) return nullptr;
    a->desc = d;
    strlcpy(a->name, name, sizeof a->name);
    a->hard = hard; a->fn = fn; a->ctx = ctx;
    wait_queue_init(&a->wq);
    if (fn) {
        char tn[16];
        snprintf(tn, sizeof tn, "irq/%d-%s", d->line >= 0 ? d->line : d->vector, name);
        a->thr = thread_create(tn, irq_thread, a);
    }
    return a;
}

static void link_action(struct irq_desc *d, struct irq_action *a) {
    uint64_t f = arch_irq_save();
    struct irq_action **pp = &d->actions;
    while (*pp) pp = &(*pp)->next;
    *pp = a;
    arch_irq_restore(f);
}

int irq_request(int line, const char *name, irq_hard_t hard, irq_thread_t fn, void *ctx) {
    if (line < 0) return -EINVAL;
    struct irq_desc *d = nullptr;
    for (int i = 0; i < ndescs; i++) if (descs[i].line == line) d = &descs[i];
    bool fresh = !d;
    if (fresh) {
        if (ndescs >= MAX_DESCS) return -ENOSPC;
        d = &descs[ndescs];
        *d = (struct irq_desc){ .line = line, .kind = arch_irq_chip() };
    }
    struct irq_action *a = new_action(d, name, hard, fn, ctx);
    if (!a) return -ENOMEM;
    link_action(d, a);
    if (fresh) {
        int v = irq_install(line, desc_dispatch, d);
        if (v < 0) { kfree(a); d->actions = nullptr; return v; }
        d->vector = v;
        ndescs++;
    }
    return line;
}

int irq_request_msi(const char *name, irq_hard_t hard, irq_thread_t fn, void *ctx, uint64_t *addr, uint32_t *data) {
    if (ndescs >= MAX_DESCS) return -ENOSPC;
    struct irq_desc *d = &descs[ndescs];
    *d = (struct irq_desc){ .line = -1, .kind = "MSI-X" };
    int v = arch_msi_alloc(desc_dispatch, d, addr, data);
    if (v < 0) return v;
    d->vector = v;
    struct irq_action *a = new_action(d, name, hard, fn, ctx);
    if (!a) return -ENOMEM;
    link_action(d, a);
    ndescs++;
    return v;
}

int irq_proc_read(char *buf, size_t max) {
    size_t n = 0;
#define OUT(...) do { if (n < max) n += snprintf(buf + n, max - n, __VA_ARGS__); } while (0)
    OUT("     ");
    for (int c = 0; c < ncpus; c++) OUT(" %10s%d", "CPU", c);
    OUT("\n");
    OUT("LOC: ");
    for (int c = 0; c < ncpus; c++) OUT(" %11lu", cpus[c].ticks);
    OUT("   timer ticks\nIPI: ");
    for (int c = 0; c < ncpus; c++) OUT(" %11lu", cpus[c].ipis);
    OUT("   inter-processor interrupts\nDEV: ");
    for (int c = 0; c < ncpus; c++) OUT(" %11lu", cpus[c].irqs);
    OUT("   device interrupts\n");
    for (int i = 0; i < ndescs; i++) {
        struct irq_desc *d = &descs[i];
        if (d->line >= 0) OUT("%4d: %11lu  %-10s", d->line, d->count, d->kind);
        else OUT("v%3d: %11lu  %-10s", d->vector, d->count, d->kind);
        for (struct irq_action *a = d->actions; a; a = a->next)
            OUT(" %s%s(handled %lu%s", a == d->actions ? "" : ", ", a->name, a->handled, a->fn ? ", thread" : "");
        if (d->actions) {
            for (struct irq_action *a = d->actions; a; a = a->next)
                if (a->fn) OUT(" runs %lu", a->thread_runs);
            OUT(")");
        }
        if (d->unhandled) OUT(" unhandled %lu", d->unhandled);
        OUT("\n");
    }
    return (int)MIN(n, max);
#undef OUT
}

/* ---- irq_work: lock-free stack drained from the self-IPI ---- */
static struct irq_work *work_head;
static bool work_ipi;

void irq_work_enable(void) { __atomic_store_n(&work_ipi, true, __ATOMIC_RELEASE); }

void irq_work_queue(struct irq_work *w) {
    if (__atomic_exchange_n(&w->pending, 1, __ATOMIC_ACQ_REL)) return;
    struct irq_work *h = __atomic_load_n(&work_head, __ATOMIC_RELAXED);
    do w->next = h;
    while (!__atomic_compare_exchange_n(&work_head, &h, w, true, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    if (!__atomic_load_n(&work_ipi, __ATOMIC_ACQUIRE)) return;
    struct cpu *c = this_cpu();
    __atomic_or_fetch(&c->ipi_pending, IPI_WORK, __ATOMIC_RELEASE);
    arch_send_ipi(c);
}

void irq_work_run(void) {
    if (!__atomic_load_n(&work_head, __ATOMIC_ACQUIRE)) return;
    struct irq_work *w = __atomic_exchange_n(&work_head, nullptr, __ATOMIC_ACQ_REL);
    while (w) {
        struct irq_work *next = w->next;
        __atomic_store_n(&w->pending, 0, __ATOMIC_RELEASE);
        w->fn(w);
        w = next;
    }
}

/* the IPI vector of every arch: the TLB/resched work, then irq_work (never from spin loops) */
void ipi_handle(void);
void ipi_service(void) {
    struct cpu *c = this_cpu();
    ipi_handle();
    __atomic_and_fetch(&c->ipi_pending, ~IPI_WORK, __ATOMIC_RELEASE);
    irq_work_run();
}
void ipi_irq(void) {
    this_cpu()->ipis++;
    ipi_service();
}
