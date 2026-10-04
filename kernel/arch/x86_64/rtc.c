/* CMOS real-time clock -> boot_epoch (seconds since 1970 at boot). */
#include <kernel/time.h>
#include <kernel/printk.h>
#include <arch/cpu.h>

static uint8_t cmos(uint8_t reg) { outb(0x70, reg); return inb(0x71); }

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

void rtc_init(void) {
    while (cmos(0x0a) & 0x80) arch_cpu_relax();
    uint8_t s = cmos(0), mi = cmos(2), h = cmos(4), d = cmos(7), mo = cmos(8), y = cmos(9), b = cmos(0x0b);
    if (!(b & 4)) {
#define BCD(x) (((x) & 15) + ((x) >> 4) * 10)
        s = BCD(s); mi = BCD(mi); h = BCD(h & 0x7f) | (h & 0x80); d = BCD(d); mo = BCD(mo); y = BCD(y);
    }
    if (!(b & 2) && (h & 0x80)) h = ((h & 0x7f) + 12) % 24;
    int64_t days = days_from_civil(2000 + y, mo, d);
    boot_epoch = days * 86400 + h * 3600 + mi * 60 + s - (int64_t)(time_ns() / 1000000000ULL);
    pr_info("rtc: 20%02u-%02u-%02u %02u:%02u:%02u UTC\n", y, mo, d, h, mi, s);
}
