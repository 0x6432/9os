#include <kernel/irq.h>
#include <kernel/time.h>
#include <kernel/arch.h>
#include <kernel/sched.h>

volatile uint64_t jiffies;
int64_t boot_epoch;

[[gnu::weak]] void sched_tick(void) {}

/* every CPU's local timer calls this; only the boot CPU advances jiffies */
void timer_tick(void) {
    if (this_cpu()->id == 0) jiffies = time_ns() / 1000000ULL;
    irq_work_run();
    sched_tick();
}

void udelay(uint64_t us) {
    uint64_t end = time_ns() + us * 1000;
    while (time_ns() < end) arch_cpu_relax();
}
