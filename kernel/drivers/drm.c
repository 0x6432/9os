/*
 * DRM/KMS: /dev/dri/card0 (char 226:0). One connector -> encoder -> CRTC with a primary and
 * a cursor plane, driving the system framebuffer (firmware fb or virtio-gpu).
 *
 * M18 had the legacy subset (dumb buffers, ADDFB/ADDFB2/RMFB, SETCRTC, PAGE_FLIP + events,
 * DIRTYFB, WAIT_VBLANK). M36 turns it into an atomic driver:
 *  - mode objects with properties (connector EDID, DPMS, link-status, non-desktop, CRTC_ID,
 *    vrr_capable; CRTC ACTIVE, MODE_ID, VRR_ENABLED; plane type, FB_ID, CRTC_ID, SRC_X/Y/W/H,
 *    CRTC_X/Y/W/H, IN_FORMATS, FB_DAMAGE_CLIPS), property blobs (CREATE/DESTROY/GETPROPBLOB),
 *    GETPROPERTY, OBJ_GET/SETPROPERTIES, client caps UNIVERSAL_PLANES and ATOMIC;
 *  - DRM_IOCTL_MODE_ATOMIC with TEST_ONLY, NONBLOCK, ALLOW_MODESET and PAGE_FLIP_EVENT; the
 *    legacy ioctls (SETCRTC, PAGE_FLIP, SETPLANE, CURSOR/CURSOR2, connector DPMS) are thin
 *    wrappers that build an atomic state and go through the same check/commit code;
 *  - GEM objects are refcounted with per-file handles; PRIME export/import through dma-buf
 *    fds (mmap, DMA_BUF_IOCTL_SYNC, lseek for the size), GEM flink names, GETFB/GETFB2;
 *  - DRM master: the first opener is master, SET/DROP_MASTER, modesetting needs master;
 *  - scanout: on virtio-gpu a framebuffer whose layout matches the display gets its own host
 *    resource backed by the dumb buffer pages (zero-copy, hardware cursor through the cursor
 *    queue); otherwise the planes are composited into the system framebuffer by copying.
 *    A VT switch to a text console takes the display back (drm_console_reclaim), as fbcon
 *    does on Linux; the next commit from a client shows its planes again.
 */
#include <kernel/vfs.h>
#include <kernel/device.h>
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
#include <kernel/mutex.h>
#include <kernel/arch.h>
#include <kernel/cred.h>
#include <kernel/syscall.h>
#include <kernel/gpu.h>

#define DRM_MAJOR 226
#define ID_CONNECTOR 31
#define ID_ENCODER 32
#define ID_CRTC 33
#define ID_PRIMARY 34
#define ID_CURSOR 35
#define FIRST_DYN_ID 40
#define MAX_FB 128
#define MAX_BLOB 128
#define MAX_HANDLES 256
#define CURSOR_SIZE 64
#define DUMB_OFF_SHIFT 28        /* mmap offset = handle << 28 (buffers up to 256 MiB) */
#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define FMT_XRGB8888 FOURCC('X', 'R', '2', '4')
#define FMT_ARGB8888 FOURCC('A', 'R', '2', '4')

#define OBJ_CRTC 0xccccccccu
#define OBJ_CONNECTOR 0xc0c0c0c0u
#define OBJ_ENCODER 0xe0e0e0e0u
#define OBJ_PROPERTY 0xb0b0b0b0u
#define OBJ_FB 0xfbfbfbfbu
#define OBJ_BLOB 0xbbbbbbbbu
#define OBJ_PLANE 0xeeeeeeeeu

#define PROP_RANGE (1u << 1)
#define PROP_IMMUTABLE (1u << 2)
#define PROP_ENUM (1u << 3)
#define PROP_BLOB (1u << 4)
#define PROP_OBJECT (1u << 6)
#define PROP_SIGNED_RANGE (2u << 6)
#define PROP_ATOMIC 0x80000000u

#define ATOMIC_EVENT 0x01
#define ATOMIC_ASYNC 0x02
#define ATOMIC_TEST_ONLY 0x100
#define ATOMIC_NONBLOCK 0x200
#define ATOMIC_ALLOW_MODESET 0x400

struct drm_modeinfo {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh, flags, type;
    char name[32];
};
_Static_assert(sizeof(struct drm_modeinfo) == 68, "modeinfo");

/* ------------------------------------------------------------------ objects */
struct dfile;
struct gem {                     /* a dumb buffer; shared between files through handles/PRIME/flink */
    struct list_node node;
    int refs;
    size_t npages, size;
    paddr_t *pages;
    struct file *dmabuf;         /* exported dma-buf (weak: cleared by its release) */
    uint32_t name;               /* flink name, 0 if none */
};
struct dfb {
    uint32_t id;
    struct dfile *owner;         /* nullptr: closed with CLOSEFB / internal cursor fb, freed when unused */
    struct gem *bo;
    uint32_t w, h, pitch, offset, format;
    uint32_t vres;               /* virtio-gpu resource for zero-copy scanout, 0 if none */
    bool internal;               /* created by the legacy cursor ioctl */
};
struct blob { uint32_t id; int refs; struct dfile *owner; size_t len; uint8_t *data; };
struct devent { struct list_node node; uint32_t len; uint8_t data[32]; };
struct dfile {
    struct list_node events;
    size_t nevents;
    bool was_master, universal, atomic;
    struct gem *handles[MAX_HANDLES];      /* handle = index + 1 */
};

struct plane_st {
    uint32_t fb, crtc;                     /* object ids */
    uint32_t src_x, src_y, src_w, src_h;   /* 16.16 */
    int32_t crtc_x, crtc_y;
    uint32_t crtc_w, crtc_h;
    uint32_t damage;                       /* FB_DAMAGE_CLIPS blob (not sticky) */
    bool touched;                          /* part of the commit being built */
};
struct kms_st {
    bool enable, active, vrr;              /* enable = has a mode (MODE_ID != 0) */
    uint32_t mode_blob;
    struct drm_modeinfo mode;
    uint32_t conn_crtc, dpms, link;
    struct plane_st pl[2];                 /* 0 primary, 1 cursor */
};

static const struct lock_class kms_class = { "drm", LR_MUTEX_DRM, false };
static struct mutex kms = MUTEX_INIT(kms, &kms_class);      /* everything below */
static spinlock_t evlock;                                    /* per-file event lists */
static struct list_node gems = LIST_INIT(gems);
static struct dfb *fbs[MAX_FB];
static struct blob *blobs[MAX_BLOB];
static uint32_t next_id = FIRST_DYN_ID, next_name = 1;
static struct limine_framebuffer *scan;
static struct drm_modeinfo mode0;          /* the one mode the connector offers */
static struct kms_st st;                   /* committed state */
static struct dfile *master;
static int nopen;
static uint32_t edid_blob, informats_blob[2];
static uint32_t vblank_seq;
static bool vblank_thread_started;
static struct { int32_t x, y, hot_x, hot_y; } lcur;   /* legacy cursor ioctl: pointer position, hot spot */
static bool reclaimed;                     /* the console took the display back (VT switch) */
static struct { bool on, zero_copy, hw_cursor; uint32_t res, fb, cur_fb; int32_t cx, cy; } shown;
static struct {                            /* nonblocking commit / page flip waiting for vblank */
    bool pending;
    struct kms_st st;
    bool event;
    uint32_t ev_type;
    uint64_t user_data;
    struct dfile *f;
} pend;
static struct wait_queue pend_wq;
static uint64_t stat_commits, stat_zero_copy, stat_copy;

/* WAIT_VBLANK events waiting for their target sequence */
struct vbl_wait { struct list_node node; struct dfile *f; uint32_t target; uint64_t user_data; };
static struct list_node vbl_waits = LIST_INIT(vbl_waits);
/* commit events delivered at the next vblank */
struct ev_wait { struct list_node node; struct dfile *f; uint32_t type; uint64_t user_data; };
static struct list_node ev_waits = LIST_INIT(ev_waits);

static struct dfb *fb_lookup(uint32_t id) {
    if (!id) return nullptr;
    for (int i = 0; i < MAX_FB; i++) if (fbs[i] && fbs[i]->id == id) return fbs[i];
    return nullptr;
}
static struct blob *blob_lookup(uint32_t id) {
    if (!id) return nullptr;
    for (int i = 0; i < MAX_BLOB; i++) if (blobs[i] && blobs[i]->id == id) return blobs[i];
    return nullptr;
}
static void blob_get(uint32_t id) { struct blob *b = blob_lookup(id); if (b) b->refs++; }
static void blob_put(uint32_t id) {
    struct blob *b = blob_lookup(id);
    if (!b || --b->refs > 0) return;
    for (int i = 0; i < MAX_BLOB; i++) if (blobs[i] == b) blobs[i] = nullptr;
    kfree(b->data); kfree(b);
}
static int blob_new(struct dfile *owner, const void *data, size_t len, uint32_t *id) {
    for (int i = 0; i < MAX_BLOB; i++) if (!blobs[i]) {
        struct blob *b = kzalloc(sizeof *b);
        uint8_t *d = kmalloc(len ? len : 1);
        if (!b || !d) { kfree(b); kfree(d); return -ENOMEM; }
        memcpy(d, data, len);
        *b = (struct blob){ next_id++, 1, owner, len, d };
        blobs[i] = b;
        *id = b->id;
        return 0;
    }
    return -ENOSPC;
}
static void st_get(const struct kms_st *s) { blob_get(s->mode_blob); blob_get(s->pl[0].damage); blob_get(s->pl[1].damage); }
static void st_put(const struct kms_st *s) { blob_put(s->mode_blob); blob_put(s->pl[0].damage); blob_put(s->pl[1].damage); }

static void gem_get(struct gem *g) { g->refs++; }
static void gem_put(struct gem *g) {
    if (--g->refs > 0) return;
    for (size_t i = 0; i < g->npages; i++) if (g->pages[i]) page_put_pa(g->pages[i]);
    list_del(&g->node);
    kfree(g->pages); kfree(g);
}
static uint32_t handle_of(struct dfile *f, struct gem *g) {     /* existing handle or 0 */
    for (int i = 0; i < MAX_HANDLES; i++) if (f->handles[i] == g) return i + 1;
    return 0;
}
static int handle_new(struct dfile *f, struct gem *g, uint32_t *h) {   /* takes a reference */
    uint32_t e = handle_of(f, g);
    if (e) { *h = e; return 0; }
    for (int i = 0; i < MAX_HANDLES; i++) if (!f->handles[i]) {
        f->handles[i] = g; gem_get(g); *h = i + 1;
        return 0;
    }
    return -ENOSPC;
}
static struct gem *handle_gem(struct dfile *f, uint32_t h) {
    return h && h <= MAX_HANDLES ? f->handles[h - 1] : nullptr;
}

/* ------------------------------------------------------------------ mode, EDID, IN_FORMATS */
static void make_mode(void) {
    uint32_t w = scan->width, h = scan->height;
    mode0 = (struct drm_modeinfo){ 0 };
    mode0.hdisplay = w; mode0.hsync_start = w + 16; mode0.hsync_end = w + 96; mode0.htotal = w + 160;
    mode0.vdisplay = h; mode0.vsync_start = h + 3; mode0.vsync_end = h + 9; mode0.vtotal = h + 30;
    mode0.vrefresh = 60;
    mode0.clock = (uint32_t)((uint64_t)mode0.htotal * mode0.vtotal * 60 / 1000);
    mode0.type = (1 << 3) | (1 << 6);            /* PREFERRED | DRIVER */
    mode0.flags = 0x5;                           /* PHSYNC | PVSYNC */
    snprintf(mode0.name, sizeof mode0.name, "%ux%u", w, h);
}
static uint32_t mm_w(void) { return scan->width * 254 / 960; }      /* 96 dpi */
static uint32_t mm_h(void) { return scan->height * 254 / 960; }

static void make_edid(uint8_t *e) {
    static const uint8_t hdr[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
    static const uint8_t chroma[10] = { 0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26, 0x0f, 0x50, 0x54 };
    const struct drm_modeinfo *m = &mode0;
    memset(e, 0, 128);
    memcpy(e, hdr, 8);
    uint16_t mfg = (('N' - '@') << 10) | (('O' - '@') << 5) | ('S' - '@');
    e[8] = mfg >> 8; e[9] = mfg & 0xff;
    e[10] = 0x36; e[11] = 0x09; e[12] = 1;                   /* product 0x0936, serial 1 */
    e[16] = 1; e[17] = 2026 - 1990; e[18] = 1; e[19] = 4;    /* week, year, EDID 1.4 */
    e[20] = 0x80;                                             /* digital */
    e[21] = (mm_w() + 5) / 10; e[22] = (mm_h() + 5) / 10;    /* cm */
    e[23] = 120;                                              /* gamma 2.2 */
    e[24] = 0x06;                                             /* sRGB, preferred timing */
    memcpy(e + 25, chroma, 10);
    for (int i = 38; i < 54; i++) e[i] = 1;                   /* no standard timings */
    uint8_t *d = e + 54;                                      /* detailed timing = mode0 */
    uint32_t pc = m->clock / 10, ha = m->hdisplay, hb = m->htotal - m->hdisplay, va = m->vdisplay, vb = m->vtotal - m->vdisplay;
    uint32_t hso = m->hsync_start - m->hdisplay, hsw = m->hsync_end - m->hsync_start;
    uint32_t vso = m->vsync_start - m->vdisplay, vsw = m->vsync_end - m->vsync_start;
    d[0] = pc & 0xff; d[1] = pc >> 8;
    d[2] = ha & 0xff; d[3] = hb & 0xff; d[4] = (uint8_t)((ha >> 8) << 4 | (hb >> 8));
    d[5] = va & 0xff; d[6] = vb & 0xff; d[7] = (uint8_t)((va >> 8) << 4 | (vb >> 8));
    d[8] = hso & 0xff; d[9] = hsw & 0xff; d[10] = (uint8_t)((vso & 15) << 4 | (vsw & 15));
    d[11] = (uint8_t)(((hso >> 8) & 3) << 6 | ((hsw >> 8) & 3) << 4 | ((vso >> 4) & 3) << 2 | ((vsw >> 4) & 3));
    d[12] = mm_w() & 0xff; d[13] = mm_h() & 0xff; d[14] = (uint8_t)((mm_w() >> 8) << 4 | (mm_h() >> 8));
    d[17] = 0x1e;                                             /* digital separate, +hsync +vsync */
    static const char *txt[2] = { "9os virtual", "9OS0001" };
    static const uint8_t tag[2] = { 0xfc, 0xff };             /* monitor name, serial string */
    for (int k = 0; k < 2; k++) {
        uint8_t *x = e + 72 + 36 * k;
        x[3] = tag[k];
        size_t l = strlen(txt[k]);
        for (int i = 0; i < 13; i++) x[5 + i] = i < (int)l ? txt[k][i] : i == (int)l ? '\n' : ' ';
    }
    uint8_t *r = e + 90;                                      /* range limits */
    r[3] = 0xfd; r[5] = 50; r[6] = 75; r[7] = 30; r[8] = 160; r[9] = 30; r[10] = 1; r[11] = '\n';
    for (int i = 12; i < 18; i++) r[i] = ' ';
    uint8_t sum = 0;
    for (int i = 0; i < 127; i++) sum += e[i];
    e[127] = (uint8_t)(0x100 - sum);
}

static uint32_t make_informats(const uint32_t *fmts, uint32_t n) {
    struct { uint32_t version, flags, count_formats, formats_offset, count_modifiers, modifiers_offset; } h =
        { 1, 0, n, 24, 1, 0 };
    h.modifiers_offset = ALIGN_UP(24 + 4 * n, 8);
    uint8_t buf[128] = { 0 };
    memcpy(buf, &h, sizeof h);
    memcpy(buf + 24, fmts, 4 * n);
    struct { uint64_t formats; uint32_t offset, pad; uint64_t modifier; } m = { (1ull << n) - 1, 0, 0, 0 };   /* LINEAR */
    memcpy(buf + h.modifiers_offset, &m, sizeof m);
    uint32_t id = 0;
    blob_new(nullptr, buf, h.modifiers_offset + sizeof m, &id);
    return id;
}

/* ------------------------------------------------------------------ properties */
enum {
    P_EDID, P_DPMS, P_LINK, P_NONDESKTOP, P_CRTC_ID, P_VRR_CAPABLE,     /* connector (CRTC_ID also planes) */
    P_ACTIVE, P_MODE_ID, P_VRR_ENABLED,                                  /* CRTC */
    P_TYPE, P_FB_ID, P_SRC_X, P_SRC_Y, P_SRC_W, P_SRC_H,                 /* planes */
    P_CRTC_X, P_CRTC_Y, P_CRTC_W, P_CRTC_H, P_IN_FORMATS, P_FB_DAMAGE_CLIPS,
    P_COUNT
};
#define PROP_ID(p) ((uint32_t)(p) + 1)
struct penum { uint64_t value; const char *name; };
struct propdef {
    const char *name;
    uint32_t flags;
    int64_t min, max;                /* ranges; OBJECT: min = object type */
    const struct penum *enums;
    int nenums;
};
static const struct penum dpms_enums[] = { { 0, "On" }, { 1, "Standby" }, { 2, "Suspend" }, { 3, "Off" } };
static const struct penum link_enums[] = { { 0, "Good" }, { 1, "Bad" } };
static const struct penum type_enums[] = { { 0, "Overlay" }, { 1, "Primary" }, { 2, "Cursor" } };
#define ENUMS(e) e, (int)(sizeof e / sizeof e[0])
static const struct propdef props[P_COUNT] = {
    [P_EDID] = { "EDID", PROP_BLOB | PROP_IMMUTABLE, 0, 0, nullptr, 0 },
    [P_DPMS] = { "DPMS", PROP_ENUM, 0, 0, ENUMS(dpms_enums) },
    [P_LINK] = { "link-status", PROP_ENUM, 0, 0, ENUMS(link_enums) },
    [P_NONDESKTOP] = { "non-desktop", PROP_RANGE | PROP_IMMUTABLE, 0, 1, nullptr, 0 },
    [P_CRTC_ID] = { "CRTC_ID", PROP_OBJECT | PROP_ATOMIC, OBJ_CRTC, 0, nullptr, 0 },
    [P_VRR_CAPABLE] = { "vrr_capable", PROP_RANGE | PROP_IMMUTABLE, 0, 1, nullptr, 0 },
    [P_ACTIVE] = { "ACTIVE", PROP_RANGE | PROP_ATOMIC, 0, 1, nullptr, 0 },
    [P_MODE_ID] = { "MODE_ID", PROP_BLOB | PROP_ATOMIC, 0, 0, nullptr, 0 },
    [P_VRR_ENABLED] = { "VRR_ENABLED", PROP_RANGE, 0, 1, nullptr, 0 },
    [P_TYPE] = { "type", PROP_ENUM | PROP_IMMUTABLE, 0, 0, ENUMS(type_enums) },
    [P_FB_ID] = { "FB_ID", PROP_OBJECT | PROP_ATOMIC, OBJ_FB, 0, nullptr, 0 },
    [P_SRC_X] = { "SRC_X", PROP_RANGE | PROP_ATOMIC, 0, UINT32_MAX, nullptr, 0 },
    [P_SRC_Y] = { "SRC_Y", PROP_RANGE | PROP_ATOMIC, 0, UINT32_MAX, nullptr, 0 },
    [P_SRC_W] = { "SRC_W", PROP_RANGE | PROP_ATOMIC, 0, UINT32_MAX, nullptr, 0 },
    [P_SRC_H] = { "SRC_H", PROP_RANGE | PROP_ATOMIC, 0, UINT32_MAX, nullptr, 0 },
    [P_CRTC_X] = { "CRTC_X", PROP_SIGNED_RANGE | PROP_ATOMIC, INT32_MIN, INT32_MAX, nullptr, 0 },
    [P_CRTC_Y] = { "CRTC_Y", PROP_SIGNED_RANGE | PROP_ATOMIC, INT32_MIN, INT32_MAX, nullptr, 0 },
    [P_CRTC_W] = { "CRTC_W", PROP_RANGE | PROP_ATOMIC, 0, INT32_MAX, nullptr, 0 },
    [P_CRTC_H] = { "CRTC_H", PROP_RANGE | PROP_ATOMIC, 0, INT32_MAX, nullptr, 0 },
    [P_IN_FORMATS] = { "IN_FORMATS", PROP_BLOB | PROP_IMMUTABLE, 0, 0, nullptr, 0 },
    [P_FB_DAMAGE_CLIPS] = { "FB_DAMAGE_CLIPS", PROP_BLOB | PROP_ATOMIC, 0, 0, nullptr, 0 },
};
static const uint8_t conn_props[] = { P_EDID, P_DPMS, P_LINK, P_NONDESKTOP, P_CRTC_ID, P_VRR_CAPABLE };
static const uint8_t crtc_props[] = { P_ACTIVE, P_MODE_ID, P_VRR_ENABLED };
static const uint8_t plane_props[] = { P_TYPE, P_FB_ID, P_CRTC_ID, P_SRC_X, P_SRC_Y, P_SRC_W, P_SRC_H,
                                       P_CRTC_X, P_CRTC_Y, P_CRTC_W, P_CRTC_H, P_IN_FORMATS, P_FB_DAMAGE_CLIPS };

static uint32_t obj_type(uint32_t id) {
    switch (id) {
    case ID_CONNECTOR: return OBJ_CONNECTOR;
    case ID_ENCODER: return OBJ_ENCODER;
    case ID_CRTC: return OBJ_CRTC;
    case ID_PRIMARY: case ID_CURSOR: return OBJ_PLANE;
    }
    if (id && id <= P_COUNT) return OBJ_PROPERTY;
    if (fb_lookup(id)) return OBJ_FB;
    if (blob_lookup(id)) return OBJ_BLOB;
    return 0;
}
static const uint8_t *obj_props(uint32_t id, int *n) {
    switch (id) {
    case ID_CONNECTOR: *n = sizeof conn_props; return conn_props;
    case ID_CRTC: *n = sizeof crtc_props; return crtc_props;
    case ID_PRIMARY: case ID_CURSOR: *n = sizeof plane_props; return plane_props;
    }
    *n = 0;
    return nullptr;
}
static uint64_t prop_value(const struct kms_st *s, uint32_t id, int p) {
    const struct plane_st *pl = &s->pl[id == ID_CURSOR];
    switch (p) {
    case P_EDID: return edid_blob;
    case P_DPMS: return s->dpms;
    case P_LINK: return s->link;
    case P_CRTC_ID: return id == ID_CONNECTOR ? s->conn_crtc : pl->crtc;
    case P_ACTIVE: return s->active;
    case P_MODE_ID: return s->mode_blob;
    case P_VRR_ENABLED: return s->vrr;
    case P_TYPE: return id == ID_PRIMARY ? 1 : 2;
    case P_FB_ID: return pl->fb;
    case P_SRC_X: return pl->src_x;
    case P_SRC_Y: return pl->src_y;
    case P_SRC_W: return pl->src_w;
    case P_SRC_H: return pl->src_h;
    case P_CRTC_X: return (uint64_t)(int64_t)pl->crtc_x;
    case P_CRTC_Y: return (uint64_t)(int64_t)pl->crtc_y;
    case P_CRTC_W: return pl->crtc_w;
    case P_CRTC_H: return pl->crtc_h;
    case P_IN_FORMATS: return informats_blob[id == ID_CURSOR];
    case P_FB_DAMAGE_CLIPS: return pl->damage;
    }
    return 0;
}
/* the property ids/values of an object as seen by f (atomic-only properties need the ATOMIC cap) */
static uint32_t list_props(struct dfile *f, uint32_t id, uint32_t *ids, uint64_t *vals) {
    int n;
    const uint8_t *pp = obj_props(id, &n);
    uint32_t k = 0;
    for (int i = 0; i < n; i++) {
        if ((props[pp[i]].flags & PROP_ATOMIC) && !f->atomic) continue;
        ids[k] = PROP_ID(pp[i]); vals[k] = prop_value(&st, id, pp[i]); k++;
    }
    return k;
}
static bool obj_has_prop(uint32_t id, int p) {
    int n;
    const uint8_t *pp = obj_props(id, &n);
    for (int i = 0; i < n; i++) if (pp[i] == p) return true;
    return false;
}

/* set a property in a state being built; legacy: through SETPROPERTY/OBJ_SETPROPERTY */
static int set_prop(struct kms_st *s, uint32_t id, int p, uint64_t v, bool legacy) {
    const struct propdef *d = &props[p];
    if (d->flags & PROP_IMMUTABLE) return -EINVAL;
    if (d->flags & PROP_ENUM) {
        bool ok = false;
        for (int i = 0; i < d->nenums; i++) if (d->enums[i].value == v) ok = true;
        if (!ok) return -EINVAL;
    } else if (d->flags & PROP_SIGNED_RANGE) {
        if ((int64_t)v < d->min || (int64_t)v > d->max) return -EINVAL;
    } else if (d->flags & PROP_RANGE) {
        if (v < (uint64_t)d->min || v > (uint64_t)d->max) return -EINVAL;
    }
    struct plane_st *pl = &s->pl[id == ID_CURSOR];
    switch (p) {
    case P_DPMS:
        if (!legacy) return -EINVAL;               /* as on Linux: not settable through atomic */
        s->active = v == 0 && s->enable;
        return 0;
    case P_LINK: s->link = (uint32_t)v; return 0;
    case P_CRTC_ID:
        if (v && v != ID_CRTC) return -EINVAL;
        if (id == ID_CONNECTOR) s->conn_crtc = (uint32_t)v;
        else { pl->crtc = (uint32_t)v; pl->touched = true; }
        return 0;
    case P_ACTIVE: s->active = v; return 0;
    case P_VRR_ENABLED: s->vrr = v; return 0;
    case P_MODE_ID: {
        if (!v) { blob_put(s->mode_blob); s->mode_blob = 0; s->enable = false; return 0; }
        struct blob *b = blob_lookup((uint32_t)v);
        if (!b || b->len != sizeof(struct drm_modeinfo)) return -EINVAL;
        blob_get(b->id); blob_put(s->mode_blob);
        s->mode_blob = b->id; s->enable = true;
        memcpy(&s->mode, b->data, sizeof s->mode);
        return 0;
    }
    case P_FB_ID:
        if (v && !fb_lookup((uint32_t)v)) return -EINVAL;
        pl->fb = (uint32_t)v; pl->touched = true;
        return 0;
    case P_SRC_X: pl->src_x = (uint32_t)v; pl->touched = true; return 0;
    case P_SRC_Y: pl->src_y = (uint32_t)v; pl->touched = true; return 0;
    case P_SRC_W: pl->src_w = (uint32_t)v; pl->touched = true; return 0;
    case P_SRC_H: pl->src_h = (uint32_t)v; pl->touched = true; return 0;
    case P_CRTC_X: pl->crtc_x = (int32_t)v; pl->touched = true; return 0;
    case P_CRTC_Y: pl->crtc_y = (int32_t)v; pl->touched = true; return 0;
    case P_CRTC_W: pl->crtc_w = (uint32_t)v; pl->touched = true; return 0;
    case P_CRTC_H: pl->crtc_h = (uint32_t)v; pl->touched = true; return 0;
    case P_FB_DAMAGE_CLIPS: {
        if (v) {
            struct blob *b = blob_lookup((uint32_t)v);
            if (!b || b->len % 16) return -EINVAL;
            blob_get(b->id);
        }
        blob_put(pl->damage);
        pl->damage = (uint32_t)v; pl->touched = true;
        return 0;
    }
    }
    return -EINVAL;
}

static void queue_event(struct dfile *f, uint32_t type, uint64_t user_data);

/* ------------------------------------------------------------------ atomic check */
static int check_plane(const struct kms_st *s, int i) {
    const struct plane_st *p = &s->pl[i];
    if (!p->fb && !p->crtc) return 0;
    if (!p->fb || !p->crtc) return -EINVAL;                  /* fb and CRTC go together */
    if (!s->enable) return -EINVAL;                          /* plane on a disabled CRTC */
    struct dfb *fb = fb_lookup(p->fb);
    if (!fb) return -EINVAL;
    if (i == 1 && fb->format != FMT_ARGB8888) return -EINVAL;
    if (!p->crtc_w || !p->crtc_h || !p->src_w || !p->src_h) return -EINVAL;
    if ((uint64_t)p->src_x + p->src_w > (uint64_t)fb->w << 16 || (uint64_t)p->src_y + p->src_h > (uint64_t)fb->h << 16)
        return -ENOSPC;
    if ((p->src_x | p->src_y | p->src_w | p->src_h) & 0xffff) return -ERANGE;     /* no subpixel sources */
    if (p->src_w >> 16 != p->crtc_w || p->src_h >> 16 != p->crtc_h) return -ERANGE; /* no scaling */
    if (i == 0 && (p->crtc_x || p->crtc_y || p->crtc_w != s->mode.hdisplay || p->crtc_h != s->mode.vdisplay))
        return -EINVAL;                                      /* the primary plane covers the CRTC */
    if (i == 1 && (p->crtc_w > CURSOR_SIZE || p->crtc_h > CURSOR_SIZE)) return -EINVAL;
    return 0;
}
static bool needs_modeset(const struct kms_st *a, const struct kms_st *b) {
    return a->enable != b->enable || a->active != b->active || a->conn_crtc != b->conn_crtc ||
           (b->enable && memcmp(&a->mode, &b->mode, sizeof a->mode));
}
static int check_state(const struct kms_st *s) {
    if (s->enable && (s->mode.hdisplay != mode0.hdisplay || s->mode.vdisplay != mode0.vdisplay)) return -EINVAL;
    if (s->active && !s->enable) return -EINVAL;
    if (s->enable != (s->conn_crtc == ID_CRTC)) return -EINVAL;     /* CRTC enabled <=> connector routed to it */
    for (int i = 0; i < 2; i++) { int r = check_plane(s, i); if (r) return r; }
    return 0;
}

/* ------------------------------------------------------------------ scanout */
struct rect { int32_t x0, y0, x1, y1; };
#define MAX_RECTS 20
struct dmg { bool full, cursor_update; int n; struct rect r[MAX_RECTS]; };
static void dmg_add(struct dmg *d, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    x0 = MAX(x0, 0); y0 = MAX(y0, 0);
    x1 = MIN(x1, (int32_t)scan->width); y1 = MIN(y1, (int32_t)scan->height);
    if (x0 >= x1 || y0 >= y1) return;
    if (d->n == MAX_RECTS) { d->full = true; return; }
    d->r[d->n++] = (struct rect){ x0, y0, x1, y1 };
}

/* premultiplied ARGB over an opaque pixel */
static uint32_t blend_px(uint32_t dst, uint32_t src) {
    uint32_t a = src >> 24;
    if (a == 255) return src;
    if (!a) return dst;
    uint32_t inv = 255 - a, out = 0xff000000u;
    for (int sh = 0; sh < 24; sh += 8) {
        uint32_t c = ((src >> sh) & 255) + ((((dst >> sh) & 255) * inv + 127) / 255);
        out |= MIN(c, 255u) << sh;
    }
    return out;
}
static uint32_t fb_px(const struct dfb *fb, uint32_t x, uint32_t y) {
    size_t off = fb->offset + (size_t)y * fb->pitch + (size_t)x * 4, pg = off / PAGE_SIZE;
    if (pg >= fb->bo->npages) return 0;
    return *(uint32_t *)((uint8_t *)PHYS_TO_VIRT(fb->bo->pages[pg]) + off % PAGE_SIZE);
}
static struct dfb *plane_fb(const struct kms_st *s, int i) { return s->pl[i].crtc ? fb_lookup(s->pl[i].fb) : nullptr; }

/* copy path: composite the primary and cursor planes of st into the system framebuffer */
static void compose_rect(const struct rect *r) {
    struct dfb *pf = plane_fb(&st, 0), *cf = plane_fb(&st, 1);
    const struct plane_st *p = &st.pl[0], *c = &st.pl[1];
    uint8_t *dst = scan->address;
    size_t rowb = (size_t)(r->x1 - r->x0) * 4;
    for (int32_t y = r->y0; y < r->y1; y++) {
        uint8_t *row = dst + (size_t)y * scan->pitch + (size_t)r->x0 * 4;
        if (pf) {
            size_t off = pf->offset + (size_t)((p->src_y >> 16) + y) * pf->pitch + ((p->src_x >> 16) + r->x0) * 4ul, done = 0;
            while (done < rowb) {
                size_t pg = (off + done) / PAGE_SIZE, po = (off + done) % PAGE_SIZE;
                size_t n = MIN(rowb - done, PAGE_SIZE - po);
                if (pg >= pf->bo->npages) { memset(row + done, 0, rowb - done); break; }
                memcpy(row + done, (uint8_t *)PHYS_TO_VIRT(pf->bo->pages[pg]) + po, n);
                done += n;
            }
        } else memset(row, 0, rowb);
        if (cf && y >= c->crtc_y && y < c->crtc_y + (int32_t)c->crtc_h) {
            int32_t x0 = MAX(r->x0, c->crtc_x), x1 = MIN(r->x1, c->crtc_x + (int32_t)c->crtc_w);
            uint32_t *px = (uint32_t *)(dst + (size_t)y * scan->pitch);
            for (int32_t x = x0; x < x1; x++)
                px[x] = blend_px(px[x], fb_px(cf, (c->src_x >> 16) + (uint32_t)(x - c->crtc_x), (c->src_y >> 16) + (uint32_t)(y - c->crtc_y)));
        }
    }
    fb_damage_rect((uint32_t)r->x0, (uint32_t)r->y0, (uint32_t)r->x1, (uint32_t)r->y1);
}

/* zero-copy needs a full-screen, tightly packed primary and (if any) a 64x64 cursor */
static bool zero_copy_ok(const struct kms_st *s, const struct dfb *pf, const struct dfb *cf) {
    if (!vgpu_present() || !pf) return false;
    if (pf->offset || pf->pitch != pf->w * 4 || pf->w != scan->width || pf->h != scan->height || s->pl[0].src_x || s->pl[0].src_y)
        return false;
    if (cf && (cf->offset || cf->w != CURSOR_SIZE || cf->h != CURSOR_SIZE || cf->pitch != CURSOR_SIZE * 4 ||
               s->pl[1].src_x || s->pl[1].src_y || s->pl[1].crtc_w != CURSOR_SIZE || s->pl[1].crtc_h != CURSOR_SIZE))
        return false;
    return true;
}
static bool ensure_vres(struct dfb *fb) {
    if (fb->vres) return true;
    size_t np = ((size_t)fb->pitch * fb->h + PAGE_SIZE - 1) / PAGE_SIZE;
    return vgpu_res_create(fb->format == FMT_ARGB8888, fb->w, fb->h, fb->bo->pages, MIN(np, fb->bo->npages), &fb->vres) == 0;
}
static void hide_hw_cursor(void) {
    if (shown.hw_cursor) vgpu_cursor(0, 0, 0, 0, 0, true);
    shown.hw_cursor = false; shown.cur_fb = 0;
}
static void show_console(void) {
    if (!shown.on) return;
    hide_hw_cursor();
    if (shown.zero_copy || (vgpu_present() && vgpu_scanout_res() != 1)) vgpu_show(0, 0, 0, 0, 0, 0);
    memset(&shown, 0, sizeof shown);
    fb_explicit_damage = false;
    fbcon_set_graphics(false);
}

static void scanout(const struct dmg *d) {
    if (reclaimed) return;
    if (!st.active) { show_console(); return; }
    struct dfb *pf = plane_fb(&st, 0), *cf = plane_fb(&st, 1);
    bool full = d->full || !shown.on;
    if (!shown.on) { fb_explicit_damage = true; fbcon_set_graphics(true); shown.on = true; }
    bool zc = zero_copy_ok(&st, pf, cf) && ensure_vres(pf) && (!cf || ensure_vres(cf));
    struct rect all = { 0, 0, (int32_t)scan->width, (int32_t)scan->height };
    if (zc) {
        if (!shown.zero_copy || shown.fb != pf->id) full = true;
        if (full) vgpu_show(pf->vres, pf->pitch, 0, 0, scan->width, scan->height);
        else for (int i = 0; i < d->n; i++)
            vgpu_show(pf->vres, pf->pitch, d->r[i].x0, d->r[i].y0, d->r[i].x1 - d->r[i].x0, d->r[i].y1 - d->r[i].y0);
        if (cf) {
            int32_t hx = cf->internal ? lcur.hot_x : 0, hy = cf->internal ? lcur.hot_y : 0;
            int32_t x = st.pl[1].crtc_x + hx, y = st.pl[1].crtc_y + hy;
            if (!shown.hw_cursor || shown.cur_fb != cf->id || d->cursor_update) vgpu_cursor(cf->vres, x, y, hx, hy, true);
            else if (shown.cx != x || shown.cy != y) vgpu_cursor(cf->vres, x, y, hx, hy, false);
            shown.hw_cursor = true; shown.cur_fb = cf->id; shown.cx = x; shown.cy = y;
        } else hide_hw_cursor();
        shown.zero_copy = true; shown.fb = pf->id; shown.res = pf->vres;
        stat_zero_copy++;
    } else {
        hide_hw_cursor();
        if (shown.zero_copy) full = true;
        if (full) compose_rect(&all);
        else for (int i = 0; i < d->n; i++) compose_rect(&d->r[i]);
        if (vgpu_present() && vgpu_scanout_res() != 1) vgpu_show(0, 0, 0, 0, 0, 0);
        shown.zero_copy = false; shown.fb = pf ? pf->id : 0; shown.res = 0; shown.cur_fb = cf ? cf->id : 0;
        stat_copy++;
    }
}

static bool cursor_rect(const struct kms_st *s, struct rect *r) {
    const struct plane_st *c = &s->pl[1];
    if (!c->fb || !c->crtc) { *r = (struct rect){ 0, 0, 0, 0 }; return false; }
    *r = (struct rect){ c->crtc_x, c->crtc_y, c->crtc_x + (int32_t)c->crtc_w, c->crtc_y + (int32_t)c->crtc_h };
    return true;
}
/* what changed on screen between the committed state o and n */
static void get_damage(const struct kms_st *o, const struct kms_st *n, struct dmg *d) {
    memset(d, 0, sizeof *d);
    const struct plane_st *op = &o->pl[0], *np = &n->pl[0];
    if (!shown.on || needs_modeset(o, n) || op->fb != np->fb || op->crtc != np->crtc ||
        op->src_x != np->src_x || op->src_y != np->src_y) d->full = true;
    else if (np->touched) {
        struct blob *b = blob_lookup(np->damage);
        if (!b) d->full = true;
        else {
            size_t cnt = b->len / 16;
            const int32_t *c = (const int32_t *)b->data;
            int32_t dx = np->crtc_x - (int32_t)(np->src_x >> 16), dy = np->crtc_y - (int32_t)(np->src_y >> 16);
            if (cnt > 16) {                                          /* many clips: their bounding box */
                struct rect bb = { INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN };
                for (size_t i = 0; i < cnt; i++) {
                    bb.x0 = MIN(bb.x0, c[4 * i]); bb.y0 = MIN(bb.y0, c[4 * i + 1]);
                    bb.x1 = MAX(bb.x1, c[4 * i + 2]); bb.y1 = MAX(bb.y1, c[4 * i + 3]);
                }
                dmg_add(d, bb.x0 + dx, bb.y0 + dy, bb.x1 + dx, bb.y1 + dy);
            } else
                for (size_t i = 0; i < cnt; i++) dmg_add(d, c[4 * i] + dx, c[4 * i + 1] + dy, c[4 * i + 2] + dx, c[4 * i + 3] + dy);
        }
    }
    struct rect orc, nrc;
    bool ov = cursor_rect(o, &orc), nv = cursor_rect(n, &nrc);
    if (ov != nv || o->pl[1].fb != n->pl[1].fb || memcmp(&orc, &nrc, sizeof orc)) {
        if (ov) dmg_add(d, orc.x0, orc.y0, orc.x1, orc.y1);
        if (nv) dmg_add(d, nrc.x0, nrc.y0, nrc.x1, nrc.y1);
        if (o->pl[1].fb != n->pl[1].fb) d->cursor_update = true;
    }
    if (n->pl[1].touched && nv) { d->cursor_update = true; dmg_add(d, nrc.x0, nrc.y0, nrc.x1, nrc.y1); }
}

static void fb_free(struct dfb *fb) {
    for (int i = 0; i < MAX_FB; i++) if (fbs[i] == fb) fbs[i] = nullptr;
    if (shown.cur_fb == fb->id) hide_hw_cursor();
    if (shown.fb == fb->id && shown.zero_copy) { vgpu_show(0, 0, 0, 0, 0, 0); shown.zero_copy = false; shown.fb = 0; shown.res = 0; }
    if (fb->vres) vgpu_res_destroy(fb->vres);
    gem_put(fb->bo);
    kfree(fb);
}
static bool st_uses_fb(const struct kms_st *s, uint32_t id) { return s->pl[0].fb == id || s->pl[1].fb == id; }
/* free framebuffers nobody owns any more (CLOSEFB, legacy cursor fbs) once no state uses them */
static void fb_gc(void) {
    for (int i = 0; i < MAX_FB; i++) {
        struct dfb *fb = fbs[i];
        if (fb && !fb->owner && !st_uses_fb(&st, fb->id) && !(pend.pending && st_uses_fb(&pend.st, fb->id))) fb_free(fb);
    }
}

/* make n (whose references we own) the committed state and show it */
static void apply(struct kms_st *n) {
    struct kms_st o = st;
    struct dmg d;
    get_damage(&o, n, &d);
    st = *n;
    for (int i = 0; i < 2; i++) { blob_put(st.pl[i].damage); st.pl[i].damage = 0; st.pl[i].touched = false; }
    scanout(&d);
    st_put(&o);
    fb_gc();
}
static void complete_pending(void) {
    if (!pend.pending) return;
    struct kms_st n = pend.st;
    pend.pending = false;
    apply(&n);
    if (pend.event && pend.f) queue_event(pend.f, pend.ev_type, pend.user_data);
    pend.f = nullptr;
    wake_up(&pend_wq);
}
/* blocking commits wait for a pending nonblocking one (with the lock dropped) */
static int wait_idle(bool nonblock) {
    while (pend.pending) {
        if (nonblock) return -EBUSY;
        mutex_unlock(&kms);
        int r = wait_until_sl(&pend_wq, !__atomic_load_n(&pend.pending, __ATOMIC_ACQUIRE));
        mutex_lock(&kms);
        if (r) return r;
    }
    return 0;
}
static struct kms_st st_dup(const struct kms_st *s) {
    struct kms_st n = *s;
    for (int i = 0; i < 2; i++) { n.pl[i].damage = 0; n.pl[i].touched = false; }
    st_get(&n);
    return n;
}

/* check and commit n (consumes its references); the caller waited with wait_idle unless TEST_ONLY */
static int commit(struct dfile *f, struct kms_st *n, uint32_t flags, uint64_t user_data) {
    const struct kms_st *base = pend.pending ? &pend.st : &st;
    int r = check_state(n);
    if (!r && needs_modeset(base, n) && !(flags & ATOMIC_ALLOW_MODESET)) r = -EINVAL;
    if (!r && (flags & ATOMIC_EVENT) && !n->active) r = -EINVAL;
    if (r || (flags & ATOMIC_TEST_ONLY)) { st_put(n); return r; }
    if (pend.pending) { st_put(n); return -EBUSY; }
    n->dpms = n->active ? 0 : 3;
    reclaimed = false;
    stat_commits++;
    if (flags & ATOMIC_NONBLOCK) {
        pend.st = *n; pend.event = flags & ATOMIC_EVENT; pend.ev_type = 2 /* FLIP_COMPLETE */;
        pend.user_data = user_data; pend.f = f;
        __atomic_store_n(&pend.pending, true, __ATOMIC_RELEASE);
        return 0;
    }
    apply(n);
    if (flags & ATOMIC_EVENT) {
        struct ev_wait *w = kzalloc(sizeof *w);
        if (w) { w->f = f; w->type = 2; w->user_data = user_data; list_add_tail(&ev_waits, &w->node); }
    }
    return 0;
}

/* RMFB: planes scanning out the fb are disabled; the primary takes the CRTC down with it */
static void fb_disable_users(uint32_t id) {
    wait_idle(false);
    if (!st_uses_fb(&st, id)) return;
    struct kms_st n = st_dup(&st);
    for (int i = 0; i < 2; i++) if (n.pl[i].fb == id) {
        n.pl[i] = (struct plane_st){ 0 };
        if (!i) {
            n.active = n.enable = false; n.conn_crtc = 0;
            blob_put(n.mode_blob); n.mode_blob = 0;
            n.pl[1] = (struct plane_st){ 0 };
        }
    }
    n.dpms = n.active ? 0 : 3;
    apply(&n);
}
static void fb_remove(uint32_t id) { fb_disable_users(id); struct dfb *fb = fb_lookup(id); if (fb) fb_free(fb); }

void drm_console_reclaim(void) {
    if (!scan) return;
    mutex_lock(&kms);
    if (shown.on) { show_console(); reclaimed = true; }
    mutex_unlock(&kms);
}
