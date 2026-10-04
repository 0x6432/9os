/*
 * DRM/KMS-lite: /dev/dri/card0 (char 226:0) with the legacy modesetting subset used by libdrm
 * clients and compositors: one connector → encoder → CRTC driving the system framebuffer
 * (firmware fb or virtio-gpu scanout), GEM dumb buffers (CREATE/MAP/DESTROY_DUMB, mmap through
 * the fake offset), framebuffers (ADDFB/ADDFB2/RMFB), SETCRTC, PAGE_FLIP with flip-complete
 * events delivered through read(2) at a 60 Hz "vblank", DIRTYFB and WAIT_VBLANK.
 *
 * "Scanout" copies the client buffer into the real framebuffer (on SETCRTC, page flip and
 * DIRTYFB), so it works with any backend; zero-copy virtio-gpu scanout can come later.
 */
#include <kernel/vfs.h>
#include <kernel/boot.h>
#include <kernel/fbcon.h>
#include <kernel/pmm.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/sched.h>
#include <kernel/time.h>
#include <kernel/mm.h>
#include <kernel/printk.h>
#include <kernel/spinlock.h>
#include <kernel/arch.h>

#define DRM_MAJOR 226
#define ID_CONNECTOR 31
#define ID_ENCODER 32
#define ID_CRTC 33
#define ID_PLANE 34
#define MAX_OBJ 64
#define DUMB_OFF_SHIFT 28        /* mmap offset = handle << 28 (buffers up to 256 MiB) */
#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define FMT_XRGB8888 FOURCC('X', 'R', '2', '4')
#define FMT_ARGB8888 FOURCC('A', 'R', '2', '4')

struct drm_modeinfo {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh, flags, type;
    char name[32];
};
_Static_assert(sizeof(struct drm_modeinfo) == 68, "modeinfo");

struct dfile;
struct dumb { uint32_t handle; struct dfile *owner; size_t npages, size; paddr_t *pages; };
struct dfb { uint32_t id; struct dfile *owner; struct dumb *bo; uint32_t w, h, pitch, offset, format; };
struct devent { struct list_node node; uint32_t len; uint8_t data[32]; };
struct dfile { struct list_node events; size_t nevents; bool master; };

static struct dumb *dumbs[MAX_OBJ];
static struct dfb *fbs[MAX_OBJ];
static uint32_t next_handle = 1, next_fb = 100;
static struct limine_framebuffer *scan;
static struct drm_modeinfo mode;
static struct { uint32_t fb_id; bool active; struct dfile *owner; struct drm_modeinfo mode; } crtc;
static struct { bool pending; uint32_t fb_id; uint64_t user_data; struct dfile *f; bool event; } flip;
static uint32_t vblank_seq;
static spinlock_t lock;
static bool vblank_thread_started;

static struct dfb *fb_lookup(uint32_t id) {
    for (int i = 0; i < MAX_OBJ; i++) if (fbs[i] && fbs[i]->id == id) return fbs[i];
    return nullptr;
}
static struct dumb *bo_lookup(uint32_t h) {
    for (int i = 0; i < MAX_OBJ; i++) if (dumbs[i] && dumbs[i]->handle == h) return dumbs[i];
    return nullptr;
}

/* copy a framebuffer into the scanout memory */
static void present(struct dfb *fb) {
    if (!fb || !scan) return;
    uint8_t *dst = scan->address;
    uint32_t w = MIN(fb->w, (uint32_t)scan->width), h = MIN(fb->h, (uint32_t)scan->height);
    size_t rowb = (size_t)w * 4;
    for (uint32_t y = 0; y < h; y++) {
        size_t off = fb->offset + (size_t)y * fb->pitch, done = 0;
        while (done < rowb) {
            size_t pg = (off + done) / PAGE_SIZE, po = (off + done) % PAGE_SIZE;
            if (pg >= fb->bo->npages) return;
            size_t n = MIN(rowb - done, PAGE_SIZE - po);
            memcpy(dst + (size_t)y * scan->pitch + done, (uint8_t *)PHYS_TO_VIRT(fb->bo->pages[pg]) + po, n);
            done += n;
        }
    }
    fb_damage();
}

static void queue_event(struct dfile *f, uint32_t type, uint64_t user_data) {
    struct devent *e = kzalloc(sizeof *e);
    if (!e) return;
    uint64_t ns = time_ns();
    struct { uint32_t type, length; uint64_t user_data; uint32_t sec, usec, seq, crtc; } ev =
        { type, 32, user_data, (uint32_t)(ns / 1000000000ull), (uint32_t)(ns % 1000000000ull / 1000), vblank_seq, ID_CRTC };
    memcpy(e->data, &ev, 32); e->len = 32;
    uint64_t fl = spin_lock_irqsave(&lock);
    list_add_tail(&f->events, &e->node); f->nevents++;
    spin_unlock_irqrestore(&lock, fl);
    poll_notify();
}

static void vblank_thread(void *arg) {
    for (;;) {
        sleep_ns(16666667ull);
        vblank_seq++;
        if (flip.pending) {
            struct dfb *fb = fb_lookup(flip.fb_id);
            if (fb) { crtc.fb_id = fb->id; present(fb); }
            flip.pending = false;
            if (flip.event && flip.f) queue_event(flip.f, 2 /* DRM_EVENT_FLIP_COMPLETE */, flip.user_data);
        }
    }
}

static void make_mode(void) {
    uint32_t w = scan->width, h = scan->height;
    mode = (struct drm_modeinfo){ 0 };
    mode.hdisplay = w; mode.hsync_start = w + 16; mode.hsync_end = w + 96; mode.htotal = w + 160;
    mode.vdisplay = h; mode.vsync_start = h + 3; mode.vsync_end = h + 9; mode.vtotal = h + 30;
    mode.vrefresh = 60;
    mode.clock = (uint32_t)((uint64_t)mode.htotal * mode.vtotal * 60 / 1000);
    mode.type = (1 << 3) | (1 << 6);            /* PREFERRED | DRIVER */
    mode.flags = 0x5;                            /* PHSYNC | PVSYNC */
    snprintf(mode.name, sizeof mode.name, "%ux%u", w, h);
}

/* ------------------------------------------------------------------ objects */
static void bo_free(struct dumb *b) {
    for (size_t i = 0; i < b->npages; i++) if (b->pages[i]) page_put_pa(b->pages[i]);
    kfree(b->pages); kfree(b);
}
static void fb_remove(int i) {
    if (crtc.fb_id == fbs[i]->id) crtc.fb_id = 0;
    if (flip.pending && flip.fb_id == fbs[i]->id) flip.pending = false;
    kfree(fbs[i]); fbs[i] = nullptr;
}

static int create_dumb(struct dfile *f, void *arg) {
    struct { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; } c;
    if (copy_from_user(&c, arg, sizeof c)) return -EFAULT;
    if (!c.width || !c.height || c.width > 8192 || c.height > 8192 || !c.bpp || c.bpp > 32) return -EINVAL;
    uint32_t pitch = ALIGN_UP(c.width * ((c.bpp + 7) / 8), 64);
    size_t size = ALIGN_UP((size_t)pitch * c.height, PAGE_SIZE);
    int slot = -1;
    for (int i = 0; i < MAX_OBJ; i++) if (!dumbs[i]) { slot = i; break; }
    if (slot < 0) return -ENOSPC;
    struct dumb *b = kzalloc(sizeof *b);
    if (!b) return -ENOMEM;
    b->npages = size / PAGE_SIZE; b->size = size; b->owner = f;
    b->pages = kzalloc(b->npages * sizeof(paddr_t));
    if (!b->pages) { kfree(b); return -ENOMEM; }
    for (size_t i = 0; i < b->npages; i++) {
        b->pages[i] = pmm_alloc_zeroed(0);
        if (!b->pages[i]) { bo_free(b); return -ENOMEM; }
        phys_to_page(b->pages[i])->refcount = 1;
    }
    b->handle = next_handle++;
    dumbs[slot] = b;
    c.handle = b->handle; c.pitch = pitch; c.size = size;
    return copy_to_user(arg, &c, sizeof c);
}

static int destroy_handle(uint32_t h) {
    for (int i = 0; i < MAX_OBJ; i++) if (dumbs[i] && dumbs[i]->handle == h) {
        for (int j = 0; j < MAX_OBJ; j++) if (fbs[j] && fbs[j]->bo == dumbs[i]) fb_remove(j);
        bo_free(dumbs[i]); dumbs[i] = nullptr;
        return 0;
    }
    return -ENOENT;
}

static int add_fb(struct dfile *f, uint32_t w, uint32_t h, uint32_t pitch, uint32_t offset, uint32_t format, uint32_t handle, uint32_t *id) {
    struct dumb *b = bo_lookup(handle);
    if (!b) return -ENOENT;
    if (format != FMT_XRGB8888 && format != FMT_ARGB8888) return -EINVAL;
    if (!w || !h || pitch < w * 4 || (uint64_t)offset + (uint64_t)pitch * h > b->size) return -EINVAL;
    for (int i = 0; i < MAX_OBJ; i++) if (!fbs[i]) {
        struct dfb *fb = kzalloc(sizeof *fb);
        if (!fb) return -ENOMEM;
        *fb = (struct dfb){ next_fb++, f, b, w, h, pitch, offset, format };
        fbs[i] = fb;
        *id = fb->id;
        return 0;
    }
    return -ENOSPC;
}

/* ------------------------------------------------------------------ ioctls */
static int put_ids(uint64_t uptr, uint32_t ucount, const uint32_t *ids, uint32_t n) {
    if (uptr && ucount >= n && n && copy_to_user((void *)uptr, ids, n * 4)) return -EFAULT;
    return 0;
}

static int drm_ioctl(struct file *file, uint64_t cmd, uint64_t uarg) {
    struct dfile *f = file->priv;
    void *arg = (void *)uarg;
    if (((cmd >> 8) & 0xff) != 'd') return -ENOTTY;
    switch (cmd & 0xff) {
    case 0x00: {                                                             /* VERSION */
        struct { int32_t maj, min, patch, pad; uint64_t name_len, name; uint64_t date_len, date; uint64_t desc_len, desc; } v;
        if (copy_from_user(&v, arg, sizeof v)) return -EFAULT;
        static const char *strs[3] = { "9os", "20261004", "9os KMS-lite (dumb buffers)" };
        uint64_t *lens[3] = { &v.name_len, &v.date_len, &v.desc_len }, ptrs[3] = { v.name, v.date, v.desc };
        for (int i = 0; i < 3; i++) {
            size_t l = strlen(strs[i]);
            if (ptrs[i] && *lens[i] && copy_to_user((void *)ptrs[i], strs[i], MIN(l, *lens[i]))) return -EFAULT;
            *lens[i] = l;
        }
        v.maj = 1; v.min = 0; v.patch = 0;
        return copy_to_user(arg, &v, sizeof v);
    }
    case 0x01: { uint64_t u[2]; if (copy_from_user(u, arg, 16)) return -EFAULT; u[0] = 0; return copy_to_user(arg, u, 16); } /* GET_UNIQUE */
    case 0x02: { uint32_t m = 1; return copy_to_user(arg, &m, 4); }       /* GET_MAGIC */
    case 0x11: return 0;                                                     /* AUTH_MAGIC */
    case 0x09: { uint32_t h; if (copy_from_user(&h, arg, 4)) return -EFAULT; return destroy_handle(h); }   /* GEM_CLOSE */
    case 0x0c: {                                                             /* GET_CAP */
        uint64_t c[2];
        if (copy_from_user(c, arg, 16)) return -EFAULT;
        switch (c[0]) {
        case 1: c[1] = 1; break;          /* DUMB_BUFFER */
        case 3: c[1] = 24; break;         /* DUMB_PREFERRED_DEPTH */
        case 4: c[1] = 0; break;          /* DUMB_PREFER_SHADOW */
        case 6: c[1] = 1; break;          /* TIMESTAMP_MONOTONIC */
        case 8: case 9: c[1] = 64; break; /* CURSOR_WIDTH/HEIGHT */
        case 0x12: c[1] = 1; break;       /* CRTC_IN_VBLANK_EVENT */
        case 2: case 5: case 7: case 0x10: case 0x11: case 0x13: c[1] = 0; break;
        default: return -EINVAL;
        }
        return copy_to_user(arg, c, 16);
    }
    case 0x0d: {                                                             /* SET_CLIENT_CAP */
        uint64_t c[2];
        if (copy_from_user(c, arg, 16)) return -EFAULT;
        if (c[0] == 1 /* STEREO_3D */ || c[0] == 2 /* UNIVERSAL_PLANES */) return 0;
        return -EOPNOTSUPP;                                                  /* ATOMIC etc. */
    }
    case 0x1e: f->master = true; return 0;                                   /* SET_MASTER */
    case 0x1f: f->master = false; return 0;                                  /* DROP_MASTER */
    case 0x3a: {                                                             /* WAIT_VBLANK */
        struct { uint32_t type, seq; int64_t sec, usec; } w;
        if (copy_from_user(&w, arg, sizeof w)) return -EFAULT;
        uint32_t target = (w.type & 1) ? vblank_seq + w.seq : w.seq;        /* RELATIVE : ABSOLUTE */
        if (w.type & 0x4000000) {                                            /* EVENT */
            queue_event(f, 1 /* DRM_EVENT_VBLANK */, (uint64_t)w.usec);
        } else {
            while ((int32_t)(vblank_seq - target) < 0) sleep_ns(4000000);
        }
        uint64_t ns = time_ns();
        w.seq = vblank_seq; w.sec = ns / 1000000000ull; w.usec = ns % 1000000000ull / 1000;
        return copy_to_user(arg, &w, sizeof w);
    }
    case 0xA0: {                                                             /* MODE_GETRESOURCES */
        struct { uint64_t fb_ptr, crtc_ptr, conn_ptr, enc_ptr; uint32_t nfb, ncrtc, nconn, nenc, minw, maxw, minh, maxh; } r;
        if (copy_from_user(&r, arg, sizeof r)) return -EFAULT;
        uint32_t ids[MAX_OBJ], n = 0;
        for (int i = 0; i < MAX_OBJ; i++) if (fbs[i] && fbs[i]->owner == f) ids[n++] = fbs[i]->id;
        uint32_t c = ID_CRTC, k = ID_CONNECTOR, e = ID_ENCODER;
        if (put_ids(r.fb_ptr, r.nfb, ids, n) || put_ids(r.crtc_ptr, r.ncrtc, &c, 1) ||
            put_ids(r.conn_ptr, r.nconn, &k, 1) || put_ids(r.enc_ptr, r.nenc, &e, 1)) return -EFAULT;
        r.nfb = n; r.ncrtc = 1; r.nconn = 1; r.nenc = 1;
        r.minw = 1; r.minh = 1; r.maxw = 8192; r.maxh = 8192;
        return copy_to_user(arg, &r, sizeof r);
    }
    case 0xA1: case 0xA2: {                                                  /* MODE_GETCRTC / SETCRTC */
        struct { uint64_t conn_ptr; uint32_t nconn, crtc_id, fb_id, x, y, gamma_size, mode_valid; struct drm_modeinfo mode; } c;
        if (copy_from_user(&c, arg, sizeof c)) return -EFAULT;
        if (c.crtc_id != ID_CRTC) return -ENOENT;
        if ((cmd & 0xff) == 0xA1) {
            c.fb_id = crtc.fb_id; c.x = c.y = 0; c.gamma_size = 256;
            c.mode_valid = crtc.active; c.mode = crtc.active ? crtc.mode : (struct drm_modeinfo){ 0 };
            return copy_to_user(arg, &c, sizeof c);
        }
        if (!c.mode_valid || !c.fb_id) {                                     /* disable */
            crtc.active = false; crtc.fb_id = 0; crtc.owner = nullptr;
            fbcon_set_graphics(false);
            return 0;
        }
        struct dfb *fb = c.fb_id == 0xffffffffu ? fb_lookup(crtc.fb_id) : fb_lookup(c.fb_id);
        if (!fb) return -ENOENT;
        if (c.mode.hdisplay != mode.hdisplay || c.mode.vdisplay != mode.vdisplay) return -EINVAL;
        crtc.mode = c.mode; crtc.active = true; crtc.fb_id = fb->id; crtc.owner = f;
        fbcon_set_graphics(true);
        present(fb);
        return 0;
    }
    case 0xA3: case 0xBB: return -ENXIO;                                     /* MODE_CURSOR(2): no hw cursor */
    case 0xA4: case 0xA5: return 0;                                          /* GET/SETGAMMA */
    case 0xA6: {                                                             /* MODE_GETENCODER */
        uint32_t e[5];
        if (copy_from_user(e, arg, sizeof e)) return -EFAULT;
        if (e[0] != ID_ENCODER) return -ENOENT;
        e[1] = 5 /* VIRTUAL */; e[2] = crtc.active ? ID_CRTC : 0; e[3] = 1; e[4] = 0;
        return copy_to_user(arg, e, sizeof e);
    }
    case 0xA7: {                                                             /* MODE_GETCONNECTOR */
        struct { uint64_t enc_ptr, modes_ptr, props_ptr, prop_values_ptr; uint32_t nmodes, nprops, nenc;
                 uint32_t enc_id, conn_id, type, type_id, connection, mm_w, mm_h, subpixel, pad; } c;
        if (copy_from_user(&c, arg, sizeof c)) return -EFAULT;
        if (c.conn_id != ID_CONNECTOR) return -ENOENT;
        uint32_t e = ID_ENCODER;
        if (put_ids(c.enc_ptr, c.nenc, &e, 1)) return -EFAULT;
        if (c.modes_ptr && c.nmodes >= 1 && copy_to_user((void *)c.modes_ptr, &mode, sizeof mode)) return -EFAULT;
        c.nmodes = 1; c.nprops = 0; c.nenc = 1;
        c.enc_id = ID_ENCODER; c.type = 15 /* VIRTUAL */; c.type_id = 1; c.connection = 1;
        c.mm_w = scan->width * 254 / 960; c.mm_h = scan->height * 254 / 960;   /* 96 dpi */
        c.subpixel = 1;
        return copy_to_user(arg, &c, sizeof c);
    }
    case 0xAA: case 0xAC: return -ENOENT;                                    /* GETPROPERTY / GETPROPBLOB */
    case 0xAB: case 0xBA: return -EINVAL;                                    /* SETPROPERTY / OBJ_SETPROPERTY */
    case 0xB9: {                                                             /* OBJ_GETPROPERTIES */
        struct { uint64_t props, values; uint32_t count, obj_id, obj_type, pad; } o;
        if (copy_from_user(&o, arg, sizeof o)) return -EFAULT;
        o.count = 0;
        return copy_to_user(arg, &o, sizeof o);
    }
    case 0xAE: {                                                             /* MODE_ADDFB */
        uint32_t a[7];   /* fb_id, width, height, pitch, bpp, depth, handle */
        if (copy_from_user(a, arg, sizeof a)) return -EFAULT;
        if (a[4] != 32) return -EINVAL;
        int r = add_fb(f, a[1], a[2], a[3], 0, a[5] == 32 ? FMT_ARGB8888 : FMT_XRGB8888, a[6], &a[0]);
        return r ? r : copy_to_user(arg, a, sizeof a);
    }
    case 0xB8: {                                                             /* MODE_ADDFB2 */
        struct { uint32_t fb_id, w, h, fmt, flags, handles[4], pitches[4], offsets[4]; uint64_t mod[4]; } a;
        if (copy_from_user(&a, arg, sizeof a)) return -EFAULT;
        if ((a.flags & 2) && a.mod[0] != 0) return -EINVAL;                  /* only LINEAR modifiers */
        int r = add_fb(f, a.w, a.h, a.pitches[0], a.offsets[0], a.fmt, a.handles[0], &a.fb_id);
        return r ? r : copy_to_user(arg, &a, sizeof a);
    }
    case 0xAF: {                                                             /* MODE_RMFB */
        uint32_t id;
        if (copy_from_user(&id, arg, 4)) return -EFAULT;
        for (int i = 0; i < MAX_OBJ; i++) if (fbs[i] && fbs[i]->id == id) { fb_remove(i); return 0; }
        return -ENOENT;
    }
    case 0xB0: {                                                             /* MODE_PAGE_FLIP */
        struct { uint32_t crtc_id, fb_id, flags, reserved; uint64_t user_data; } p;
        if (copy_from_user(&p, arg, sizeof p)) return -EFAULT;
        if (p.crtc_id != ID_CRTC) return -ENOENT;
        if (!crtc.active) return -EINVAL;
        if (!fb_lookup(p.fb_id)) return -ENOENT;
        if (flip.pending) return -EBUSY;
        flip.fb_id = p.fb_id; flip.user_data = p.user_data; flip.f = f; flip.event = p.flags & 1;
        __atomic_store_n(&flip.pending, true, __ATOMIC_RELEASE);
        return 0;
    }
    case 0xB1: {                                                             /* MODE_DIRTYFB */
        uint32_t d[4];
        if (copy_from_user(d, arg, sizeof d)) return -EFAULT;
        struct dfb *fb = fb_lookup(d[0]);
        if (!fb) return -ENOENT;
        if (crtc.active && crtc.fb_id == fb->id) present(fb);
        return 0;
    }
    case 0xB2: return create_dumb(f, arg);                                   /* MODE_CREATE_DUMB */
    case 0xB3: {                                                             /* MODE_MAP_DUMB */
        struct { uint32_t handle, pad; uint64_t offset; } m;
        if (copy_from_user(&m, arg, sizeof m)) return -EFAULT;
        if (!bo_lookup(m.handle)) return -ENOENT;
        m.offset = (uint64_t)m.handle << DUMB_OFF_SHIFT;
        return copy_to_user(arg, &m, sizeof m);
    }
    case 0xB4: { uint32_t h; if (copy_from_user(&h, arg, 4)) return -EFAULT; return destroy_handle(h); }  /* DESTROY_DUMB */
    case 0xB5: {                                                             /* MODE_GETPLANERESOURCES */
        struct { uint64_t ptr; uint32_t count, pad; } r;
        if (copy_from_user(&r, arg, sizeof r)) return -EFAULT;
        uint32_t p = ID_PLANE;
        if (put_ids(r.ptr, r.count, &p, 1)) return -EFAULT;
        r.count = 1;
        return copy_to_user(arg, &r, sizeof r);
    }
    case 0xB6: {                                                             /* MODE_GETPLANE */
        struct { uint32_t plane_id, crtc_id, fb_id, possible_crtcs, gamma_size, nfmt; uint64_t fmt_ptr; } p;
        if (copy_from_user(&p, arg, sizeof p)) return -EFAULT;
        if (p.plane_id != ID_PLANE) return -ENOENT;
        uint32_t fmts[2] = { FMT_XRGB8888, FMT_ARGB8888 };
        if (p.fmt_ptr && p.nfmt >= 2 && copy_to_user((void *)p.fmt_ptr, fmts, sizeof fmts)) return -EFAULT;
        p.crtc_id = crtc.active ? ID_CRTC : 0; p.fb_id = crtc.fb_id; p.possible_crtcs = 1; p.gamma_size = 0; p.nfmt = 2;
        return copy_to_user(arg, &p, sizeof p);
    }
    }
    return -EINVAL;
}

/* ------------------------------------------------------------------ file ops */
static int drm_open(struct inode *ino, struct file *file) {
    if (!scan) return -ENODEV;
    struct dfile *f = kzalloc(sizeof *f);
    if (!f) return -ENOMEM;
    list_init(&f->events);
    file->priv = f;
    if (!vblank_thread_started) { vblank_thread_started = true; thread_create("drm-vblank", vblank_thread, nullptr); }
    return 0;
}

static void drm_release(struct file *file) {
    struct dfile *f = file->priv;
    if (flip.f == f) { flip.pending = false; flip.f = nullptr; }
    for (int i = 0; i < MAX_OBJ; i++) if (fbs[i] && fbs[i]->owner == f) fb_remove(i);
    for (int i = 0; i < MAX_OBJ; i++) if (dumbs[i] && dumbs[i]->owner == f) { bo_free(dumbs[i]); dumbs[i] = nullptr; }
    if (crtc.owner == f) { crtc.active = false; crtc.fb_id = 0; crtc.owner = nullptr; fbcon_set_graphics(false); }
    list_for_each_safe(it, tmp, &f->events) kfree(list_entry(it, struct devent, node));
    kfree(f);
}

static ssize_t drm_read(struct file *file, void *buf, size_t n, off_t *off) {
    struct dfile *f = file->priv;
    for (;;) {
        uint64_t fl = spin_lock_irqsave(&lock);
        if (f->nevents) {
            size_t done = 0;
            uint8_t tmp[256];
            while (!list_empty(&f->events)) {
                struct devent *e = list_entry(f->events.next, struct devent, node);
                if (done + e->len > n || done + e->len > sizeof tmp) break;
                memcpy(tmp + done, e->data, e->len); done += e->len;
                list_del(&e->node); f->nevents--; kfree(e);
            }
            spin_unlock_irqrestore(&lock, fl);
            if (!done) return -EINVAL;
            return copy_to_user(buf, tmp, done) ? -EFAULT : (ssize_t)done;
        }
        spin_unlock(&lock);
        if (file->flags & O_NONBLOCK) { arch_irq_restore(fl); return -EAGAIN; }
        int r = wait_event_timeout(&poll_wq, 20000000ull);
        arch_irq_restore(fl);
        if (r == -EINTR) return r;
    }
}

static unsigned drm_poll(struct file *file) {
    struct dfile *f = file->priv;
    return (f->nevents ? POLLIN | POLLRDNORM : 0) | POLLOUT | POLLWRNORM;
}

static int drm_mmap_page(struct file *file, uint64_t pgoff, paddr_t *pa) {
    uint32_t h = (uint32_t)(pgoff >> (DUMB_OFF_SHIFT - 12));
    size_t idx = pgoff & ((1ull << (DUMB_OFF_SHIFT - 12)) - 1);
    struct dumb *b = bo_lookup(h);
    if (!b || idx >= b->npages) return -EINVAL;
    *pa = b->pages[idx];
    return 0;
}

static const struct file_ops drm_ops = {
    .open = drm_open, .release = drm_release, .read = drm_read, .poll = drm_poll,
    .ioctl = drm_ioctl, .mmap_page = drm_mmap_page,
};

void drm_init(void) {
    struct limine_framebuffer *fb = boot_framebuffer();
    if (!fb || fb->bpp != 32) return;
    scan = fb;
    make_mode();
    chrdev_register(DRM_MAJOR, 0, &drm_ops);
    vfs_mkdir_at(nullptr, "/dev/dri", 0755);
    vfs_mknod_at(nullptr, "/dev/dri/card0", S_IFCHR | 0666, MKDEV(DRM_MAJOR, 0));
    pr_info("drm: /dev/dri/card0 %s (dumb buffers, legacy KMS)\n", mode.name);
}
