/* Framebuffer text console with a small ANSI/VT100 subset.
 * M35: one virtual console per VT (tty1..tty6) with a shadow cell buffer, so switching VTs
 * redraws the target console; only the foreground VT draws to the framebuffer, and only while
 * it is in KD_TEXT mode and no fbdev/DRM client owns the display. All state is serialised by
 * the console lock (console_write / console_run_locked). */
#include <kernel/fbcon.h>
#include <kernel/vt.h>
#include <kernel/boot.h>
#include <kernel/string.h>
#include <kernel/printk.h>
#include <kernel/kmalloc.h>

extern const uint8_t font8x16[128][16];

struct cell { uint32_t fg, bg; char ch; };
struct vc {
    struct cell *cells;
    int cx, cy;
    uint32_t fg, bg;
    int esc;            /* 0 none, 1 got ESC, 2 in CSI */
    int params[8], nparams;
    bool priv;
};

static struct {
    uint8_t *fb;
    uint64_t pitch, width, height;
    int cols, rows;
    bool ready;         /* a framebuffer is attached */
} c = { .cols = 80, .rows = 25 };
static struct vc vcs[NR_VT];
static int fg_vt;
static bool vt_graphics[NR_VT];     /* KD_GRAPHICS */
static bool graphics;               /* a client owns the framebuffer (/dev/fb0 open, DRM master) */
static bool shadows;                /* cell buffers allocated */
static bool con_registered;
void fbcon_write(const char *s, size_t n);
static void reg(void) { if (!con_registered) { con_registered = true; console_register(fbcon_write); } }

static const uint32_t palette[16] = {
    0x000000, 0xcd3131, 0x0dbc79, 0xe5e510, 0x2472c8, 0xbc3fbc, 0x11a8cd, 0xe5e5e5,
    0x666666, 0xf14c4c, 0x23d18b, 0xf5f543, 0x3b8eea, 0xd670d6, 0x29b8db, 0xffffff,
};
#define DEF_FG 0xcccccc
#define DEF_BG 0x101018

static bool visible(struct vc *v) { return c.ready && !graphics && v == &vcs[fg_vt] && !vt_graphics[fg_vt]; }

static void paint(int cx, int cy, char ch, uint32_t fgc, uint32_t bgc) {
    unsigned uc = (unsigned char)ch;
    const uint8_t *g = font8x16[uc < 128 ? uc : '?'];
    for (int y = 0; y < 16; y++) {
        uint32_t *row = (uint32_t *)(c.fb + (cy * 16 + y) * c.pitch) + cx * 8;
        for (int x = 0; x < 8; x++) row[x] = (g[y] & (0x80 >> x)) ? fgc : bgc;
    }
}

static void draw_cell(struct vc *v, int cx, int cy, char ch) {
    if (v->cells) v->cells[cy * c.cols + cx] = (struct cell){ v->fg, v->bg, ch };
    if (visible(v)) paint(cx, cy, ch, v->fg, v->bg);
}

static void clear_cells(struct vc *v, int x0, int y0, int x1, int y1) { /* inclusive-exclusive in cells */
    if (v->cells)
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) v->cells[y * c.cols + x] = (struct cell){ v->fg, v->bg, ' ' };
    if (!visible(v)) return;
    for (int y = y0 * 16; y < y1 * 16; y++) {
        uint32_t *row = (uint32_t *)(c.fb + y * c.pitch);
        for (int x = x0 * 8; x < x1 * 8; x++) row[x] = v->bg;
    }
}

static void scroll(struct vc *v) {
    if (v->cells) memmove(v->cells, v->cells + c.cols, (size_t)(c.rows - 1) * c.cols * sizeof *v->cells);
    if (visible(v)) memmove(c.fb, c.fb + 16 * c.pitch, (c.rows - 1) * 16 * c.pitch);
    clear_cells(v, 0, c.rows - 1, c.cols, c.rows);
}

static void newline(struct vc *v) {
    v->cx = 0;
    if (++v->cy >= c.rows) { scroll(v); v->cy = c.rows - 1; }
}

static void sgr(struct vc *v) {
    if (v->nparams == 0) { v->fg = DEF_FG; v->bg = DEF_BG; return; }
    for (int i = 0; i < v->nparams; i++) {
        int p = v->params[i];
        if (p == 0) { v->fg = DEF_FG; v->bg = DEF_BG; }
        else if (p >= 30 && p <= 37) v->fg = palette[p - 30];
        else if (p >= 90 && p <= 97) v->fg = palette[p - 90 + 8];
        else if (p >= 40 && p <= 47) v->bg = palette[p - 40];
        else if (p >= 100 && p <= 107) v->bg = palette[p - 100 + 8];
        else if (p == 39) v->fg = DEF_FG;
        else if (p == 49) v->bg = DEF_BG;
        else if (p == 7) { uint32_t t = v->fg; v->fg = v->bg; v->bg = t; }
    }
}

static void csi(struct vc *v, char f) {
    int p0 = v->nparams > 0 ? v->params[0] : 0;
    int n = p0 ? p0 : 1;
    switch (f) {
    case 'm': sgr(v); break;
    case 'A': v->cy = MAX(0, v->cy - n); break;
    case 'B': v->cy = MIN(c.rows - 1, v->cy + n); break;
    case 'C': v->cx = MIN(c.cols - 1, v->cx + n); break;
    case 'D': v->cx = MAX(0, v->cx - n); break;
    case 'G': v->cx = MIN(c.cols - 1, n - 1); break;
    case 'H': case 'f': {
        int r = p0 ? p0 : 1, col = (v->nparams > 1 && v->params[1]) ? v->params[1] : 1;
        v->cy = MIN(c.rows - 1, r - 1); v->cx = MIN(c.cols - 1, col - 1);
        break;
    }
    case 'J':
        if (p0 == 2 || p0 == 3) { clear_cells(v, 0, 0, c.cols, c.rows); }
        else if (p0 == 0) { clear_cells(v, v->cx, v->cy, c.cols, v->cy + 1); clear_cells(v, 0, v->cy + 1, c.cols, c.rows); }
        else if (p0 == 1) { clear_cells(v, 0, 0, c.cols, v->cy); clear_cells(v, 0, v->cy, v->cx + 1, v->cy + 1); }
        break;
    case 'K':
        if (p0 == 0) clear_cells(v, v->cx, v->cy, c.cols, v->cy + 1);
        else if (p0 == 1) clear_cells(v, 0, v->cy, v->cx + 1, v->cy + 1);
        else clear_cells(v, 0, v->cy, c.cols, v->cy + 1);
        break;
    default: break;
    }
}

static void putc_vc(struct vc *v, char ch) {
    if (v->esc == 1) {
        if (ch == '[') { v->esc = 2; v->nparams = 0; v->params[0] = 0; v->priv = false; return; }
        v->esc = 0; return;
    }
    if (v->esc == 2) {
        if (ch >= '0' && ch <= '9') {
            if (v->nparams == 0) v->nparams = 1;
            v->params[v->nparams - 1] = v->params[v->nparams - 1] * 10 + (ch - '0');
        } else if (ch == ';') {
            if (v->nparams == 0) v->nparams = 1;
            if (v->nparams < 8) v->params[v->nparams++] = 0;
        } else if (ch == '?') v->priv = true;
        else { v->esc = 0; if (!v->priv) csi(v, ch); }
        return;
    }
    switch (ch) {
    case 0x1b: v->esc = 1; return;
    case '\n': newline(v); return;
    case '\r': v->cx = 0; return;
    case '\b': if (v->cx > 0) v->cx--; return;
    case '\t': v->cx = (v->cx + 8) & ~7; if (v->cx >= c.cols) newline(v); return;
    case '\a': return;
    }
    if (v->cx >= c.cols) newline(v);
    draw_cell(v, v->cx, v->cy, ch);
    v->cx++;
}

void (*fb_flush_hook)(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);
bool fb_explicit_damage;
void fb_damage(void) { if (fb_flush_hook) fb_flush_hook(0, 0, UINT32_MAX, UINT32_MAX); }
void fb_damage_rect(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    if (fb_flush_hook && x1 > x0 && y1 > y0) fb_flush_hook(x0, y0, x1, y1);
}
bool fb_graphics_active(void) { return graphics || vt_graphics[fg_vt]; }

/* console lock held: repaint the foreground console from its shadow buffer */
static void redraw(void) {
    struct vc *v = &vcs[fg_vt];
    if (!visible(v)) return;
    if (!v->cells) {                                  /* no shadow (early boot): blank */
        for (uint64_t y = 0; y < c.height; y++) {
            uint32_t *row = (uint32_t *)(c.fb + y * c.pitch);
            for (uint64_t x = 0; x < c.width; x++) row[x] = v->bg;
        }
        v->cx = v->cy = 0;
        return;
    }
    for (int y = 0; y < c.rows; y++)
        for (int x = 0; x < c.cols; x++) {
            struct cell *k = &v->cells[y * c.cols + x];
            paint(x, y, k->ch, k->fg, k->bg);
        }
}

static void set_graphics_locked(void *arg) {
    bool on = arg != nullptr;
    if (graphics == on) return;
    graphics = on;
    if (!on) redraw();
}
void fbcon_set_graphics(bool on) {
    if (!c.ready) return;
    console_run_locked(set_graphics_locked, on ? (void *)1 : nullptr);
    fb_damage();          /* also kicks an event-driven flusher into its periodic (mmap) mode */
}

struct vtreq { int vt; bool on; };
static void kd_mode_locked(void *arg) {
    struct vtreq *r = arg;
    if (vt_graphics[r->vt] == r->on) return;
    vt_graphics[r->vt] = r->on;
    if (!r->on && r->vt == fg_vt) redraw();
}
void fbcon_vt_set_kd_graphics(int vt, bool on) {
    struct vtreq r = { vt, on };
    console_run_locked(kd_mode_locked, &r);
    fb_damage();
}

static void switch_locked(void *arg) {
    int vt = (int)(uintptr_t)arg;
    if (vt == fg_vt) return;
    fg_vt = vt;
    redraw();
}
void fbcon_vt_switch(int vt) {
    console_run_locked(switch_locked, (void *)(uintptr_t)vt);
    fb_damage();
}

static void vt_write(int vt, const char *s, size_t n) {
    struct vc *v = &vcs[vt];
    if (!c.ready && !v->cells) return;
    for (size_t i = 0; i < n; i++) putc_vc(v, s[i]);
}

void fbcon_write(const char *s, size_t n) {           /* the kernel console = tty1 (console lock held) */
    vt_write(0, s, n);
    if (visible(&vcs[0])) fb_damage();
}

struct vtw { int vt; const char *s; size_t n; };
static void vt_write_locked(void *arg) { struct vtw *w = arg; vt_write(w->vt, w->s, w->n); }
void fbcon_vt_write(int vt, const char *s, size_t n) {
    struct vtw w = { vt, s, n };
    console_run_locked(vt_write_locked, &w);
    if (visible(&vcs[vt])) fb_damage();
}

struct vcsread { int vt; char *buf; size_t n; uint64_t off; size_t got; };
static void vcs_read_locked(void *arg) {
    struct vcsread *r = arg;
    struct vc *v = &vcs[r->vt < 0 ? fg_vt : r->vt];
    size_t total = (size_t)c.cols * c.rows;
    for (uint64_t i = r->off; i < total && r->got < r->n; i++)
        r->buf[r->got++] = v->cells ? v->cells[i].ch : ' ';
}
size_t fbcon_vcs_read(int vt, char *buf, size_t n, uint64_t off) {
    struct vcsread r = { vt, buf, n, off, 0 };
    console_run_locked(vcs_read_locked, &r);
    return r.got;
}
size_t fbcon_vcs_size(void) { return (size_t)c.cols * c.rows; }

static void alloc_shadows(void) {
    for (int i = 0; i < NR_VT; i++) {
        struct vc *v = &vcs[i];
        struct cell *nc = kmalloc((size_t)c.cols * c.rows * sizeof *nc);
        if (!nc) continue;
        for (int k = 0; k < c.cols * c.rows; k++) nc[k] = (struct cell){ DEF_FG, DEF_BG, ' ' };
        struct cell *old = v->cells;
        v->cells = nc;
        if (old) kfree(old);
        if (!v->fg) { v->fg = DEF_FG; v->bg = DEF_BG; }
        v->cx = v->cy = 0;
    }
    shadows = true;
}

/* called before the heap exists (no shadow buffers: VT1 draws directly) and again later */
void fbcon_init(void) {
    struct limine_framebuffer *fb = boot_framebuffer();
    if (fb && fb->bpp != 32) fb = nullptr;
    bool registered = c.ready;   /* may be called again when a GPU driver provides a new fb */
    if (registered && fb && c.fb == (uint8_t *)fb->address) return;
    if (!fb && registered) return;
    if (!fb) return;
    int cols = fb->width / 8, rows = fb->height / 16;
    bool resized = cols != c.cols || rows != c.rows;
    c.fb = fb->address; c.pitch = fb->pitch; c.width = fb->width; c.height = fb->height;
    c.cols = cols; c.rows = rows;
    for (int i = 0; i < NR_VT; i++) if (!vcs[i].fg) { vcs[i].fg = DEF_FG; vcs[i].bg = DEF_BG; }
    if (shadows && resized) alloc_shadows();      /* new geometry: start over */
    if (!shadows) { vcs[0].cx = vcs[0].cy = 0; }
    c.ready = true;
    redraw();
    reg();
    fb_damage();
}

/* the heap is up: give every VT a shadow buffer (80x25 without a framebuffer, for /dev/vcs) */
void fbcon_vt_init(void) {
    if (shadows) return;
    for (int i = 0; i < NR_VT; i++) if (!vcs[i].fg) { vcs[i].fg = DEF_FG; vcs[i].bg = DEF_BG; }
    int cx = vcs[0].cx, cy = vcs[0].cy;
    alloc_shadows();
    vcs[0].cx = cx; vcs[0].cy = cy;      /* keep printing where the boot log is */
    reg();                                /* VT1 text for /dev/vcs1 even without a display */
}

void fbcon_get_size(int *cols, int *rows) {
    *cols = c.cols; *rows = c.rows;
}
