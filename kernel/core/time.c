#include <kernel/time.h>
#include <kernel/arch.h>

volatile uint64_t jiffies;
int64_t boot_epoch;

[[gnu::weak]] void sched_tick(void) {}

void timer_tick(void) {
    jiffies++;
    sched_tick();
}

void udelay(uint64_t us) {
    uint64_t end = time_ns() + us * 1000;
    while (time_ns() < end) arch_cpu_relax();
}
