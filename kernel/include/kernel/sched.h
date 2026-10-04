#pragma once
#include <kernel/types.h>
#include <kernel/list.h>
#include <arch/thread.h>
#include <kernel/arch.h>

#define KSTACK_ORDER 2                         /* 16 KiB kernel stacks */
#define KSTACK_SIZE (PAGE_SIZE << KSTACK_ORDER)
#define SCHED_QUANTUM 10                       /* ticks (ms) per time slice */

enum thread_state { T_RUNNABLE, T_RUNNING, T_BLOCKED, T_SLEEPING, T_ZOMBIE };

struct process;
struct thread {
    struct arch_thread arch;   /* must stay first (alignment) */
    int tid;
    enum thread_state state;
    char name[16];
    void *kstack;              /* base of kernel stack */
    int quantum;
    uint64_t wake_ns;
    struct list_node run_node;     /* run queue / wait queue */
    struct list_node timer_node;   /* timeout list */
    bool timer_active, timed_out;
    struct list_node proc_node;    /* process thread list */
    struct list_node all_node;
    struct process *proc;
    bool interrupted;              /* woken by signal */
    void *wait_chan;
    int *clear_child_tid;
    /* signals */
    uint64_t sig_mask, sig_pending, saved_mask;
    bool restore_mask;
    uint64_t altstack_sp, altstack_size;
    int altstack_flags;
    uint64_t last_syscall;
    bool killed;
};

struct wait_queue { struct list_node head; };
#define WAIT_QUEUE_INIT(n) { LIST_INIT((n).head) }
static inline void wait_queue_init(struct wait_queue *q) { list_init(&q->head); }

extern struct thread *current;
extern volatile bool need_resched;

void sched_init(void);
struct thread *thread_create(const char *name, void (*fn)(void *), void *arg);
struct thread *thread_alloc(const char *name);   /* allocated but not runnable */
void thread_start(struct thread *t);
void thread_free(struct thread *t);
__noreturn void thread_exit(void);
void schedule(void);
void sched_yield(void);
void sched_tick(void);
void thread_wake(struct thread *t);
void sleep_ns(uint64_t ns);
/* Block on a queue. Returns 0, or -EINTR if interrupted by a signal. */
int wait_event(struct wait_queue *q);
/* 0 on wakeup, -ETIMEDOUT on timeout, -EINTR on signal. ns == UINT64_MAX means no timeout. */
int wait_event_timeout(struct wait_queue *q, uint64_t ns);
void wake_up(struct wait_queue *q);
void wake_up_one(struct wait_queue *q);

/* condition-style helper: sleep until cond is true (re-checked after each wakeup) */
#define wait_until(q, cond) ({ int __r = 0; uint64_t __f = arch_irq_save(); \
    while (!(cond)) { if ((__r = wait_event(q))) break; } arch_irq_restore(__f); __r; })

/* provided by arch */
void arch_thread_init(struct thread *t, void (*entry)(void *), void *arg);
void arch_switch_to(struct thread *prev, struct thread *next);
