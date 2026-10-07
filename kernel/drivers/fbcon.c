/* Framebuffer text console with a small ANSI/VT100 subset. */
#include <kernel/fbcon.h>
#include <kernel/boot.h>
#include <kernel/string.h>
#include <kernel/printk.h>

extern const uint8_t font8x16[128][16];

static struct {
    uint8_t *fb;
    uint64_t pitch, width, height;
    int cols, rows, cx, cy;
    uint32_t fg, bg;
    bool ready;
    int esc;            /* 0 none, 1 got ESC, 2 in CSI */
    int params[8], nparams;
    bool priv;
} c;

static const uint32_t palette[16] = {
    0x000000, 0xcd3131, 0x0dbc79, 0xe5e510, 0x2472c8, 0xbc3fbc, 0x11a8cd, 0xe5e5e5,
    0x666666, 0xf14c4c, 0x23d18b, 0xf5f543, 0x3b8eea, 0xd670d6, 0x29b8db, 0xffffff,
};
#define DEF_FG 0xcccccc
#define DEF_BG 0x101018

static inline void px(uint64_t x, uint64_t y, uint32_t col) {
    *(volatile uint32_t *)(c.fb + y * c.pitch + x * 4) = col;
}

static void draw_cell(int cx, int cy, char ch) {
    unsigned uc = (unsigned char)ch;
    const uint8_t *g = font8x16[uc < 128 ? uc : '?'];
    for (int y = 0; y < 16; y++) {
        uint32_t *row = (uint32_t *)(c.fb + (cy * 16 + y) * c.pitch) + cx * 8;
        for (int x = 0; x < 8; x++) row[x] = (g[y] & (0x80 >> x)) ? c.fg : c.bg;
    }
}

static void clear_cells(int x0, int y0, int x1, int y1) { /* inclusive-exclusive in cells */
    for (int y = y0 * 16; y < y1 * 16; y++)
        for (int x = x0 * 8; x < x1 * 8; x++) px(x, y, c.bg);
}

static void scroll(void) {
    memmove(c.fb, c.fb + 16 * c.pitch, (c.rows - 1) * 16 * c.pitch);
    clear_cells(0, c.rows - 1, c.cols, c.rows);
}

static void newline(void) {
    c.cx = 0;
    if (++c.cy >= c.rows) { scroll(); c.cy = c.rows - 1; }
}

static void sgr(void) {
    if (c.nparams == 0) { c.fg = DEF_FG; c.bg = DEF_BG; return; }
    for (int i = 0; i < c.nparams; i++) {
        int p = c.params[i];
        if (p == 0) { c.fg = DEF_FG; c.bg = DEF_BG; }
        else if (p >= 30 && p <= 37) c.fg = palette[p - 30];
        else if (p >= 90 && p <= 97) c.fg = palette[p - 90 + 8];
        else if (p >= 40 && p <= 47) c.bg = palette[p - 40];
        else if (p >= 100 && p <= 107) c.bg = palette[p - 100 + 8];
        else if (p == 39) c.fg = DEF_FG;
        else if (p == 49) c.bg = DEF_BG;
        else if (p == 7) { uint32_t t = c.fg; c.fg = c.bg; c.bg = t; }
    }
}

static void csi(char f) {
    int p0 = c.nparams > 0 ? c.params[0] : 0;
    int n = p0 ? p0 : 1;
    switch (f) {
    case 'm': sgr(); break;
    case 'A': c.cy = MAX(0, c.cy - n); break;
    case 'B': c.cy = MIN(c.rows - 1, c.cy + n); break;
    case 'C': c.cx = MIN(c.cols - 1, c.cx + n); break;
    case 'D': c.cx = MAX(0, c.cx - n); break;
    case 'G': c.cx = MIN(c.cols - 1, n - 1); break;
    case 'H': case 'f': {
        int r = p0 ? p0 : 1, col = (c.nparams > 1 && c.params[1]) ? c.params[1] : 1;
        c.cy = MIN(c.rows - 1, r - 1); c.cx = MIN(c.cols - 1, col - 1);
        break;
    }
    case 'J':
        if (p0 == 2 || p0 == 3) { clear_cells(0, 0, c.cols, c.rows); }
        else if (p0 == 0) { clear_cells(c.cx, c.cy, c.cols, c.cy + 1); clear_cells(0, c.cy + 1, c.cols, c.rows); }
        else if (p0 == 1) { clear_cells(0, 0, c.cols, c.cy); clear_cells(0, c.cy, c.cx + 1, c.cy + 1); }
        break;
    case 'K':
        if (p0 == 0) clear_cells(c.cx, c.cy, c.cols, c.cy + 1);
        else if (p0 == 1) clear_cells(0, c.cy, c.cx + 1, c.cy + 1);
        else clear_cells(0, c.cy, c.cols, c.cy + 1);
        break;
    default: break;
    }
}

static void putc_fb(char ch) {
    if (c.esc == 1) {
        if (ch == '[') { c.esc = 2; c.nparams = 0; c.params[0] = 0; c.priv = false; return; }
        c.esc = 0; return;
    }
    if (c.esc == 2) {
        if (ch >= '0' && ch <= '9') {
            if (c.nparams == 0) c.nparams = 1;
            c.params[c.nparams - 1] = c.params[c.nparams - 1] * 10 + (ch - '0');
        } else if (ch == ';') {
            if (c.nparams == 0) c.nparams = 1;
            if (c.nparams < 8) c.params[c.nparams++] = 0;
        } else if (ch == '?') c.priv = true;
        else { c.esc = 0; if (!c.priv) csi(ch); }
        return;
    }
    switch (ch) {
    case 0x1b: c.esc = 1; return;
    case '\n': newline(); return;
    case '\r': c.cx = 0; return;
    case '\b': if (c.cx > 0) c.cx--; return;
    case '\t': c.cx = (c.cx + 8) & ~7; if (c.cx >= c.cols) newline(); return;
    case '\a': return;
    }
    if (c.cx >= c.cols) newline();
    draw_cell(c.cx, c.cy, ch);
    c.cx++;
}

static bool graphics;     /* a client owns the framebuffer (/dev/fb0 open) */
void (*fb_flush_hook)(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);
bool fb_explicit_damage;
void fb_damage(void) { if (fb_flush_hook) fb_flush_hook(0, 0, UINT32_MAX, UINT32_MAX); }
void fb_damage_rect(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    if (fb_flush_hook && x1 > x0 && y1 > y0) fb_flush_hook(x0, y0, x1, y1);
}
bool fb_graphics_active(void) { return graphics; }

void fbcon_set_graphics(bool on) {
    if (!c.ready || graphics == on) return;
    graphics = on;
    if (!on) { clear_cells(0, 0, c.cols, c.rows); c.cx = c.cy = 0; }
    fb_damage();          /* also kicks an event-driven flusher into its periodic (mmap) mode */
}

void fbcon_write(const char *s, size_t n) {
    if (!c.ready || graphics) return;
    for (size_t i = 0; i < n; i++) putc_fb(s[i]);
    fb_damage();
}

void fbcon_init(void) {
    struct limine_framebuffer *fb = boot_framebuffer();
    if (!fb || fb->bpp != 32) return;
    bool registered = c.ready;   /* may be called again when a GPU driver provides a new fb */
    if (registered && c.fb == (uint8_t *)fb->address) return;
    c.fb = fb->address; c.pitch = fb->pitch; c.width = fb->width; c.height = fb->height;
    c.cols = c.width / 8; c.rows = c.height / 16;
    c.fg = DEF_FG; c.bg = DEF_BG;
    clear_cells(0, 0, c.cols, c.rows);
    c.cx = c.cy = 0;
    c.ready = true;
    if (!registered) console_register(fbcon_write);
    fb_damage();
}

void fbcon_get_size(int *cols, int *rows) {
    if (c.ready) { *cols = c.cols; *rows = c.rows; }
}
