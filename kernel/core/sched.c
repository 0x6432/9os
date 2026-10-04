/* Preemptive round-robin scheduler (uniprocessor). */
#include <kernel/sched.h>
#include <kernel/arch.h>
#include <kernel/pmm.h>
#include <kernel/boot.h>
#include <kernel/slab.h>
#include <kernel/string.h>
#include <kernel/printk.h>
#include <kernel/time.h>
#include <kernel/errno.h>

struct thread *current;
volatile bool need_resched;
static struct list_node run_queue = LIST_INIT(run_queue);
static struct list_node sleep_list = LIST_INIT(sleep_list);
static struct list_node zombies = LIST_INIT(zombies);
static struct list_node all_threads = LIST_INIT(all_threads);
static struct thread *idle_thread;
static struct kmem_cache *thread_cache;
static int next_tid = 1;

static void enqueue(struct thread *t) {
    t->state = T_RUNNABLE;
    list_add_tail(&run_queue, &t->run_node);
}

struct thread *thread_alloc(const char *name) {
    struct thread *t = kmem_cache_alloc(thread_cache);
    if (!t) return nullptr;
    memset(t, 0, sizeof *t);
    struct page *stk = page_alloc(KSTACK_ORDER);
    if (!stk) { kmem_cache_free(thread_cache, t); return nullptr; }
    t->kstack = PHYS_TO_VIRT(page_to_phys(stk));
    t->tid = next_tid++;
    strlcpy(t->name, name, sizeof t->name);
    t->quantum = SCHED_QUANTUM;
    list_init(&t->run_node);
    list_init(&t->proc_node);
    list_add_tail(&all_threads, &t->all_node);
    t->state = T_BLOCKED;
    return t;
}

void thread_start(struct thread *t) {
    uint64_t f = arch_irq_save();
    enqueue(t);
    arch_irq_restore(f);
}

struct thread *thread_create(const char *name, void (*fn)(void *), void *arg) {
    struct thread *t = thread_alloc(name);
    if (!t) return nullptr;
    arch_thread_init(t, fn, arg);
    thread_start(t);
    return t;
}

void thread_free(struct thread *t) {
    list_del(&t->all_node);
    pmm_free_pages(VIRT_TO_PHYS(t->kstack), KSTACK_ORDER);
    kmem_cache_free(thread_cache, t);
}

static void reap_zombies(void) {
    list_for_each_safe(it, tmp, &zombies) {
        struct thread *t = list_entry(it, struct thread, run_node);
        list_del(&t->run_node);
        thread_free(t);
    }
}

/* Must be called with interrupts disabled. */
static void __schedule(void) {
    struct thread *prev = current, *next;
    need_resched = false;
    if (prev->state == T_RUNNING && prev != idle_thread) enqueue(prev);
    if (list_empty(&run_queue)) next = idle_thread;
    else {
        next = list_first(&run_queue, struct thread, run_node);
        list_del(&next->run_node);
    }
    next->state = T_RUNNING;
    next->quantum = SCHED_QUANTUM;
    if (next == prev) return;
    current = next;
    arch_switch_to(prev, next);
}

void schedule(void) {
    uint64_t f = arch_irq_save();
    __schedule();
    arch_irq_restore(f);
}

void sched_yield(void) {
    if (current) schedule();
    else arch_cpu_relax();
}

void sched_tick(void) {
    uint64_t now = time_ns();
    list_for_each_safe(it, tmp, &sleep_list) {
        struct thread *t = list_entry(it, struct thread, run_node);
        if (t->wake_ns <= now) { list_del(&t->run_node); enqueue(t); need_resched = true; }
    }
    if (!current) return;
    if (current == idle_thread) { if (!list_empty(&run_queue)) need_resched = true; }
    else if (--current->quantum <= 0) need_resched = true;
}

void thread_wake(struct thread *t) {
    uint64_t f = arch_irq_save();
    if (t->state == T_BLOCKED || t->state == T_SLEEPING) {
        list_del(&t->run_node);
        enqueue(t);
        need_resched = true;
    }
    arch_irq_restore(f);
}

void sleep_ns(uint64_t ns) {
    uint64_t f = arch_irq_save();
    current->wake_ns = time_ns() + ns;
    current->state = T_SLEEPING;
    list_add_tail(&sleep_list, &current->run_node);
    __schedule();
    arch_irq_restore(f);
}

[[gnu::weak]] bool signal_pending(struct thread *t) { return false; }

int wait_event(struct wait_queue *q) {
    uint64_t f = arch_irq_save();
    if (signal_pending(current)) { arch_irq_restore(f); return -EINTR; }
    current->state = T_BLOCKED;
    current->interrupted = false;
    list_add_tail(&q->head, &current->run_node);
    __schedule();
    bool intr = current->interrupted;
    arch_irq_restore(f);
    return intr ? -EINTR : 0;
}

void wake_up(struct wait_queue *q) {
    uint64_t f = arch_irq_save();
    list_for_each_safe(it, tmp, &q->head) {
        struct thread *t = list_entry(it, struct thread, run_node);
        list_del(&t->run_node);
        enqueue(t);
    }
    need_resched = true;
    arch_irq_restore(f);
}

void wake_up_one(struct wait_queue *q) {
    uint64_t f = arch_irq_save();
    if (!list_empty(&q->head)) {
        struct thread *t = list_first(&q->head, struct thread, run_node);
        list_del(&t->run_node);
        enqueue(t);
        need_resched = true;
    }
    arch_irq_restore(f);
}

__noreturn void thread_exit(void) {
    arch_irq_disable();
    current->state = T_ZOMBIE;
    list_add_tail(&zombies, &current->run_node);
    __schedule();
    panic("zombie thread rescheduled");
}

static void idle_loop(void *arg) {
    for (;;) {
        arch_irq_disable();
        reap_zombies();
        if (!list_empty(&run_queue)) { __schedule(); arch_irq_enable(); continue; }
        arch_wait_for_interrupt();
    }
}

/* The boot context becomes the first thread ("kmain"); an idle thread is created. */
void sched_init(void) {
    thread_cache = kmem_cache_create("thread", sizeof(struct thread), 64);
    struct thread *boot = kmem_cache_alloc(thread_cache);
    memset(boot, 0, sizeof *boot);
    boot->tid = 0;
    strlcpy(boot->name, "kmain", sizeof boot->name);
    boot->state = T_RUNNING;
    boot->quantum = SCHED_QUANTUM;
    list_init(&boot->run_node);
    list_init(&boot->proc_node);
    list_add_tail(&all_threads, &boot->all_node);
    current = boot;
    idle_thread = thread_alloc("idle");
    arch_thread_init(idle_thread, idle_loop, nullptr);
    idle_thread->state = T_RUNNABLE;
    pr_info("sched: round robin, quantum %d ms\n", SCHED_QUANTUM);
}

void trap_exit_hook_sched(void) {
    if (need_resched && current) __schedule();
}
