#pragma once
#include <kernel/types.h>
#define TIMER_HZ 1000
extern volatile uint64_t jiffies;
uint64_t time_ns(void);              /* monotonic nanoseconds since boot */
void udelay(uint64_t us);
void timer_tick(void);               /* called by the arch timer interrupt */
/* wall clock (seconds since epoch at boot, from RTC) */
extern int64_t boot_epoch;
