/* PS/2 keyboard (scancode set 1) and COM1 receive interrupt -> console tty. */
#include <kernel/tty.h>
#include <kernel/irq.h>
#include <kernel/printk.h>
#include <arch/cpu.h>

static const char normal[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n', 0, 'a','s',
    'd','f','g','h','j','k','l',';','\'','`', 0, '\\','z','x','c','v',
    'b','n','m',',','.','/', 0, '*', 0, ' ',
};
static const char shifted[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n', 0, 'A','S',
    'D','F','G','H','J','K','L',':','"','~', 0, '|','Z','X','C','V',
    'B','N','M','<','>','?', 0, '*', 0, ' ',
};
static bool shift, ctrl, alt, caps, ext;

static void kbd_irq(struct trap_frame *f, void *ctx) {
    uint8_t sc = inb(0x60);
    irq_eoi();
    if (sc == 0xe0) { ext = true; return; }
    bool rel = sc & 0x80;
    sc &= 0x7f;
    if (ext) {
        ext = false;
        if (sc == 0x1d) { ctrl = !rel; return; }
        if (sc == 0x38) { alt = !rel; return; }
        if (rel) return;
        const char *seq = nullptr;
        switch (sc) {
        case 0x48: seq = "\x1b[A"; break;
        case 0x50: seq = "\x1b[B"; break;
        case 0x4d: seq = "\x1b[C"; break;
        case 0x4b: seq = "\x1b[D"; break;
        case 0x47: seq = "\x1b[H"; break;
        case 0x4f: seq = "\x1b[F"; break;
        case 0x53: seq = "\x1b[3~"; break;
        case 0x49: seq = "\x1b[5~"; break;
        case 0x51: seq = "\x1b[6~"; break;
        case 0x1c: seq = "\n"; break;
        case 0x35: seq = "/"; break;
        }
        if (seq) tty_input_str(&console_tty, seq);
        return;
    }
    switch (sc) {
    case 0x2a: case 0x36: shift = !rel; return;
    case 0x1d: ctrl = !rel; return;
    case 0x38: alt = !rel; return;
    case 0x3a: if (!rel) caps = !caps; return;
    }
    if (rel) return;
    char c = shift ? shifted[sc] : normal[sc];
    if (!c) return;
    if (caps && c >= 'a' && c <= 'z' && !shift) c -= 32;
    else if (caps && c >= 'A' && c <= 'Z' && shift) c += 32;
    if (c == '\b') c = 0x7f;
    if (ctrl) {
        if (c >= 'a' && c <= 'z') c = c - 'a' + 1;
        else if (c >= 'A' && c <= 'Z') c = c - 'A' + 1;
        else if (c == '[') c = 27;
        else if (c == '\\') c = 28;
    }
    if (alt) tty_input(&console_tty, 27);
    tty_input(&console_tty, c);
}

bool serial_can_read(void);
char serial_getc(void);

static void serial_irq(struct trap_frame *f, void *ctx) {
    while (serial_can_read()) {
        char c = serial_getc();
        tty_input(&console_tty, c);
    }
    irq_eoi();
}

void input_init(void) {
    while (inb(0x64) & 1) inb(0x60);          /* drain controller */
    irq_install(1, kbd_irq, nullptr);
    irq_install(4, serial_irq, nullptr);
    outb(0x3f8 + 1, 0x01);                     /* COM1: interrupt on received data */
    pr_info("input: ps/2 keyboard + serial console\n");
}
