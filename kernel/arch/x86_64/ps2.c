/* PS/2 keyboard (scancode set 1) → evdev keyboard; COM1 receive interrupt → console tty. */
#include <kernel/tty.h>
#include <kernel/input.h>
#include <kernel/irq.h>
#include <kernel/printk.h>
#include <arch/cpu.h>

/* set-1 make codes 0x01..0x58 equal Linux KEY_* codes; 0xe0-prefixed keys need a table */
static uint8_t ext_map(uint8_t sc) {
    switch (sc) {
    case 0x1c: return 96;  case 0x1d: return 97;  case 0x35: return 98;  case 0x38: return 100;
    case 0x47: return 102; case 0x48: return 103; case 0x49: return 104; case 0x4b: return 105;
    case 0x4d: return 106; case 0x4f: return 107; case 0x50: return 108; case 0x51: return 109;
    case 0x52: return 110; case 0x53: return 111; case 0x5b: return 125; case 0x5c: return 126;
    case 0x5d: return 127;
    }
    return 0;
}

static struct input_dev kbd = {
    .name = "AT Translated Set 2 keyboard", .phys = "isa0060/serio0/input0",
    .id = { BUS_I8042, 1, 1, 0xab41 }, .console_keys = true,
};
static bool ext;

static int kbd_irq(void *ctx) {
    if (!(inb(0x64) & 1)) return IRQ_NONE;
    uint8_t sc = inb(0x60);
    if (sc == 0xe0) { ext = true; return IRQ_HANDLED; }
    bool rel = sc & 0x80;
    sc &= 0x7f;
    unsigned code = ext ? ext_map(sc) : sc <= 0x58 ? sc : 0;
    ext = false;
    if (!code) return IRQ_HANDLED;             /* includes the fake shifts of e0 sequences */
    input_event(&kbd, EV_MSC, 4 /* MSC_SCAN */, sc);
    input_event(&kbd, EV_KEY, code, !rel);
    input_sync(&kbd);
    return IRQ_HANDLED;
}

bool serial_can_read(void);
char serial_getc(void);

static int serial_irq(void *ctx) {
    if (!serial_can_read()) return IRQ_NONE;
    while (serial_can_read()) {
        char c = serial_getc();
        tty_input(&console_tty, c);
    }
    return IRQ_HANDLED;
}

void input_init(void) {
    input_set_bit(kbd.evbit, EV_KEY); input_set_bit(kbd.evbit, EV_MSC);
    input_set_bit(kbd.evbit, EV_REP); input_set_bit(kbd.evbit, EV_LED);
    input_set_bit(kbd.mscbit, 4);
    for (unsigned k = 1; k <= 0x58; k++) input_set_bit(kbd.keybit, k);
    for (unsigned k = 96; k <= 127; k++) if (k != 99 && k != 101 && k != 112) input_set_bit(kbd.keybit, k);
    for (unsigned l = 0; l < 3; l++) input_set_bit(kbd.ledbit, l);
    input_register(&kbd);
    while (inb(0x64) & 1) inb(0x60);          /* drain controller */
    irq_request(1, "i8042", kbd_irq, nullptr, nullptr);
    irq_request(4, "serial", serial_irq, nullptr, nullptr);
    outb(0x3f8 + 1, 0x01);                     /* COM1: interrupt on received data */
    pr_info("input: ps/2 keyboard (irq 1) + serial console (irq 4), interrupt-driven\n");
}
