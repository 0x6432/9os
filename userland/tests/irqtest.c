/* M28 interrupt-driven I/O: /proc/interrupts lists the console UART and the virtio devices on
 * real interrupt lines (IO-APIC/MSI-X, GICv2/v3, PLIC) with threaded handlers, the UART line
 * has fired (this command was typed over it), virtio-gpu command completions raise interrupts
 * when the console is written (fbcon damage -> irq_work -> flush thread), and an idle second
 * costs (almost) no timer ticks now that console input is no longer polled. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (c) printf("  [ok] %s\n", #c); else { printf("  [FAIL] %s (line %d)\n", #c, __LINE__); fails++; } } while (0)

struct line { char name[32]; unsigned long count, handled; int thread; char kind[16]; };
static struct line lines[64];
static int nlines, ncpu;
static unsigned long loc[16];

static int snap(void) {
    FILE *f = fopen("/proc/interrupts", "r");
    if (!f) return -1;
    char buf[512];
    nlines = 0; ncpu = 0;
    while (fgets(buf, sizeof buf, f)) {
        if (strstr(buf, "CPU0")) { for (char *p = buf; (p = strstr(p, "CPU")); p += 3) ncpu++; continue; }
        if (!strncmp(buf, "LOC:", 4)) {
            char *p = buf + 4;
            for (int c = 0; c < ncpu && c < 16; c++) loc[c] = strtoul(p, &p, 10);
            continue;
        }
        if (!strncmp(buf, "IPI:", 4) || !strncmp(buf, "DEV:", 4)) continue;
        char *colon = strchr(buf, ':');
        if (!colon || nlines >= 64) continue;
        struct line *l = &lines[nlines];
        memset(l, 0, sizeof *l);
        if (sscanf(colon + 1, "%lu %15s %31[^(](handled %lu", &l->count, l->kind, l->name, &l->handled) < 3) continue;
        l->name[strcspn(l->name, " ")] = 0;
        l->thread = strstr(colon, "thread") != NULL;
        nlines++;
    }
    fclose(f);
    return nlines;
}
static struct line *find(const char *pfx) {
    for (int i = 0; i < nlines; i++) if (!strncmp(lines[i].name, pfx, strlen(pfx))) return &lines[i];
    return NULL;
}

int main(void) {
    printf("irqtest:\n");
    CHECK(snap() > 0);
    CHECK(ncpu >= 1);
    struct line *uart = find("serial");
    if (!uart) uart = find("pl011");
    CHECK(uart != NULL);
    if (uart) {
        printf("  uart: %s on %s, %lu interrupts\n", uart->name, uart->kind, uart->count);
        CHECK(uart->count > 0 && uart->handled > 0);
        CHECK(strcmp(uart->kind, "MSI-X") && uart->kind[0]);
    }
    struct line *kbd = find("vinput");
    CHECK(kbd != NULL);
    int nv = 0;
    for (int i = 0; i < nlines; i++)
        if (!strncmp(lines[i].name, "vinput", 6)) { nv++; if (!lines[i].thread) { printf("  %s has no thread handler\n", lines[i].name); fails++; } }
    printf("  %d virtio-input line(s), %s\n", nv, kbd ? kbd->kind : "-");

    struct line *gpu = find("virtio-gpu");
    if (gpu) {
        unsigned long before = gpu->count;
        for (int i = 0; i < 8; i++) { printf("  irqtest: damaging the framebuffer console %d\n", i); fflush(stdout); usleep(30000); }
        usleep(200000);
        snap();
        gpu = find("virtio-gpu");
        printf("  virtio-gpu: %lu -> %lu interrupts\n", before, gpu ? gpu->count : 0);
        CHECK(gpu && gpu->count > before);
    } else printf("  (no virtio-gpu: firmware framebuffer)\n");

    /* idle: no 10 ms console poll any more -> secondary CPUs stay tickless */
    if (ncpu > 1) {
        unsigned long l0[16];
        memcpy(l0, loc, sizeof l0);
        sleep(1);
        snap();
        unsigned long ap = 0;
        for (int c = 1; c < ncpu && c < 16; c++) ap += loc[c] - l0[c];
        printf("  AP timer ticks in an idle second: %lu (%d APs)\n", ap, ncpu - 1);
        CHECK(ap < 60ul * (ncpu - 1));
    }
    printf(fails ? "irqtest: FAIL (%d)\n" : "irqtest: PASS\n", fails);
    return fails != 0;
}
