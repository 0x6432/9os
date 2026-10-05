#pragma once
#include <kernel/types.h>
#define TIMER_HZ 1000
extern volatile uint64_t jiffies;
uint64_t time_ns(void);              /* monotonic nanoseconds since boot */
void udelay(uint64_t us);
void timer_tick(void);               /* called by the arch timer interrupt */
/* IRQ-off clock-event interface. UINT64_MAX disables the idle CPU's timer. */
void arch_timer_idle(uint64_t deadline_ns);
void arch_timer_active(void);
uint64_t arch_idle_poll_ns(void);      /* max console polling latency, or UINT64_MAX */
/* wall clock (seconds since epoch at boot, from RTC) */
extern int64_t boot_epoch;
