/*
 * kmstest: M36 DRM atomic/PRIME regression test through libdrm.
 * Master handling, caps, universal planes, properties (EDID, IN_FORMATS), atomic TEST_ONLY and
 * invalid states, modesets, nonblocking commits (EBUSY), legacy page flips, FB_DAMAGE_CLIPS, the
 * cursor plane (blending) and legacy cursor ioctls, GETFB2, DPMS, SETCRTC, PRIME export/import,
 * flink, VT reclaim, RMFB, a CRTC without a primary plane and lastclose.
 * Pixels are checked through /dev/fb0 when the driver composites into it (copy scanout).
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/vt.h>
#include <linux/dma-buf.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL kmstest line %d: ", __LINE__); printf(__VA_ARGS__); \
    printf(" (errno %d)\n", errno); } } while (0)
#define CHECK_ERR(expr, e, what) do { errno = 0; int _r = (expr); CHECK(_r < 0 && errno == (e), "%s: got %d, want errno %d", what, _r, e); } while (0)

static int W, H;
static uint32_t conn_id, crtc_id, primary_id, cursor_id;
static drmModeModeInfo mode;
static uint32_t mode_blob;
static uint8_t *fb0; static uint32_t fb0_pitch;
static int copy_mode;      /* scanout attr says "copy": /dev/fb0 shows the composited planes */

struct buf { uint32_t handle, pitch, fb; uint64_t size; uint32_t *map; int w, h; };

static int mkbuf(int fd, struct buf *b, int w, int h, uint32_t fmt) {
    struct drm_mode_create_dumb c = { .width = w, .height = h, .bpp = 32 };
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) return -1;
    b->handle = c.handle; b->pitch = c.pitch; b->size = c.size; b->w = w; b->h = h;
    struct drm_mode_map_dumb m = { .handle = c.handle };
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m)) return -1;
    b->map = mmap(0, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
    if (b->map == MAP_FAILED) return -1;
    uint32_t hs[4] = { c.handle }, ps[4] = { c.pitch }, os[4] = { 0 };
    return drmModeAddFB2(fd, w, h, fmt, hs, ps, os, &b->fb, 0);
}
static void fill(struct buf *b, int x0, int y0, int x1, int y1, uint32_t px) {
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) b->map[y * (b->pitch / 4) + x] = px;
}
static uint32_t pixel(int x, int y) { return fb0 ? *(uint32_t *)(fb0 + y * fb0_pitch + x * 4) & 0xffffff : 0; }
#define CHECK_PX(x, y, v) do { if (copy_mode) { uint32_t _p = pixel(x, y); CHECK(_p == (v), "pixel %d,%d = %06x, want %06x", x, y, _p, (unsigned)(v)); } } while (0)

static void readfile(const char *p, char *buf, size_t n) {
    buf[0] = 0;
    int f = open(p, O_RDONLY);
    if (f < 0) return;
    ssize_t r = read(f, buf, n - 1);
    buf[r > 0 ? r : 0] = 0;
    close(f);
}
static void scanout(char *buf) { readfile("/sys/class/drm/card0/scanout", buf, 256); }
static int scanout_is(const char *m) { char b[256]; scanout(b); return !strncmp(b, m, strlen(m)) && b[strlen(m)] == ' '; }

static uint32_t prop(int fd, uint32_t obj, uint32_t type, const char *name, uint64_t *val) {
    drmModeObjectProperties *p = drmModeObjectGetProperties(fd, obj, type);
    uint32_t id = 0;
    for (uint32_t i = 0; p && i < p->count_props && !id; i++) {
        drmModePropertyRes *r = drmModeGetProperty(fd, p->props[i]);
        if (r && !strcmp(r->name, name)) { id = r->prop_id; if (val) *val = p->prop_values[i]; }
        drmModeFreeProperty(r);
    }
    drmModeFreeObjectProperties(p);
    return id;
}
#define P(obj, type, name) prop(fd, obj, type, name, NULL)

static uint64_t ev_data; static int ev_count;
static void flip_handler(int fd, unsigned seq, unsigned s, unsigned us, unsigned crtc, void *data) {
    (void)fd; (void)seq; (void)s; (void)us; (void)crtc; ev_data = (uint64_t)(uintptr_t)data; ev_count++;
}
static int wait_event(int fd) {
    drmEventContext ev = { .version = 3, .page_flip_handler2 = flip_handler };
    int before = ev_count;
    for (int i = 0; i < 50 && ev_count == before; i++) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 20) > 0) drmHandleEvent(fd, &ev);
    }
    return ev_count > before;
}

static void plane_full(drmModeAtomicReq *r, int fd, uint32_t plane, uint32_t fb, int w, int h, int x, int y) {
    drmModeAtomicAddProperty(r, plane, P(plane, DRM_MODE_OBJECT_PLANE, "FB_ID"), fb);
    drmModeAtomicAddProperty(r, plane, P(plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID"), fb ? crtc_id : 0);
    drmModeAtomicAddProperty(r, plane, P(plane, DRM_MODE_OBJECT_PLANE, "SRC_X"), 0);
    drmModeAtomicAddProperty(r, plane, P(plane, DRM_MODE_OBJECT_PLANE, "SRC_Y"), 0);
    drmModeAtomicAddProperty(r, plane, P(plane, DRM_MODE_OBJECT_PLANE, "SRC_W"), (uint64_t)w << 16);
    drmModeAtomicAddProperty(r, plane, P(plane, DRM_MODE_OBJECT_PLANE, "SRC_H"), (uint64_t)h << 16);
    drmModeAtomicAddProperty(r, plane, P(plane, DRM_MODE_OBJECT_PLANE, "CRTC_X"), (uint64_t)(int64_t)x);
    drmModeAtomicAddProperty(r, plane, P(plane, DRM_MODE_OBJECT_PLANE, "CRTC_Y"), (uint64_t)(int64_t)y);
    drmModeAtomicAddProperty(r, plane, P(plane, DRM_MODE_OBJECT_PLANE, "CRTC_W"), w);
    drmModeAtomicAddProperty(r, plane, P(plane, DRM_MODE_OBJECT_PLANE, "CRTC_H"), h);
}
static void modeset_props(drmModeAtomicReq *r, int fd, int active) {
    drmModeAtomicAddProperty(r, conn_id, P(conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID"), active ? crtc_id : 0);
    drmModeAtomicAddProperty(r, crtc_id, P(crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID"), active ? mode_blob : 0);
    drmModeAtomicAddProperty(r, crtc_id, P(crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE"), active);
}
/* one-property commit */
static int set1(int fd, uint32_t obj, uint32_t type, const char *name, uint64_t v, uint32_t flags) {
    drmModeAtomicReq *r = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(r, obj, prop(fd, obj, type, name, NULL), v);
    int ret = drmModeAtomicCommit(fd, r, flags, NULL);
    drmModeAtomicFree(r);
    return ret;
}
static int flip_to(int fd, uint32_t fb, uint32_t damage, uint32_t flags, void *ud) {
    drmModeAtomicReq *r = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(r, primary_id, P(primary_id, DRM_MODE_OBJECT_PLANE, "FB_ID"), fb);
    if (damage) drmModeAtomicAddProperty(r, primary_id, P(primary_id, DRM_MODE_OBJECT_PLANE, "FB_DAMAGE_CLIPS"), damage);
    int ret = drmModeAtomicCommit(fd, r, flags, ud);
    drmModeAtomicFree(r);
    return ret;
}

int main(void) {
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { printf("FAIL kmstest: open card0: %s\n", strerror(errno)); return 1; }
    CHECK(drmIsMaster(fd), "first opener is master");
    int fd2 = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    CHECK(fd2 >= 0 && !drmIsMaster(fd2), "second opener is not master");
    CHECK_ERR(drmSetMaster(fd2), EBUSY, "SET_MASTER with a master present");

    drmVersion *v = drmGetVersion(fd);
    CHECK(v && !strcmp(v->name, "9os") && v->version_minor >= 1, "version");
    drmFreeVersion(v);
    uint64_t cap = 0;
    CHECK(!drmGetCap(fd, DRM_CAP_PRIME, &cap) && cap == (DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT), "PRIME cap");
    CHECK(!drmGetCap(fd, DRM_CAP_ADDFB2_MODIFIERS, &cap) && cap == 1, "ADDFB2_MODIFIERS cap");
    CHECK(!drmGetCap(fd, DRM_CAP_CURSOR_WIDTH, &cap) && cap == 64, "cursor width cap");
    drmModePlaneRes *pr = drmModeGetPlaneResources(fd);
    CHECK(pr && pr->count_planes == 0, "no planes without UNIVERSAL_PLANES");
    drmModeFreePlaneResources(pr);
    drmModeAtomicReq *r = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(r, crtc_id ? crtc_id : 33, 7, 0);
    CHECK_ERR(drmModeAtomicCommit(fd, r, DRM_MODE_ATOMIC_TEST_ONLY, NULL), EINVAL, "atomic without the cap");
    drmModeAtomicFree(r);
    CHECK(!drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1), "ATOMIC client cap");
    CHECK(!drmSetClientCap(fd2, DRM_CLIENT_CAP_ATOMIC, 1), "ATOMIC client cap (fd2)");
    pr = drmModeGetPlaneResources(fd);
    CHECK(pr && pr->count_planes == 2, "primary + cursor plane");
    for (uint32_t i = 0; pr && i < pr->count_planes; i++) {
        uint64_t t = 9;
        prop(fd, pr->planes[i], DRM_MODE_OBJECT_PLANE, "type", &t);
        if (t == DRM_PLANE_TYPE_PRIMARY) primary_id = pr->planes[i];
        if (t == DRM_PLANE_TYPE_CURSOR) cursor_id = pr->planes[i];
    }
    drmModeFreePlaneResources(pr);
    CHECK(primary_id && cursor_id, "plane types");

    drmModeRes *res = drmModeGetResources(fd);
    if (!res || !res->count_connectors || !res->count_crtcs) { printf("FAIL kmstest: resources\n"); return 1; }
    conn_id = res->connectors[0]; crtc_id = res->crtcs[0];
    drmModeFreeResources(res);
    drmModeConnector *conn = drmModeGetConnector(fd, conn_id);
    if (!conn || !conn->count_modes) { printf("FAIL kmstest: connector\n"); return 1; }
    mode = conn->modes[0]; W = mode.hdisplay; H = mode.vdisplay;
    drmModeFreeConnector(conn);

    /* EDID: blob + sysfs, checksum */
    uint64_t edid_id = 0;
    CHECK(prop(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR, "EDID", &edid_id) && edid_id, "EDID property");
    drmModePropertyBlobRes *eb = drmModeGetPropertyBlob(fd, (uint32_t)edid_id);
    CHECK(eb && eb->length == 128, "EDID blob");
    if (eb && eb->length == 128) {
        const uint8_t *e = eb->data;
        uint8_t sum = 0;
        for (int i = 0; i < 128; i++) sum += e[i];
        CHECK(e[0] == 0 && e[1] == 0xff && e[7] == 0 && sum == 0, "EDID header/checksum");
        CHECK((e[56] | (e[58] >> 4) << 8) == W, "EDID detailed timing width");
        char sb[256];
        int sf = open("/sys/class/drm/card0-Virtual-1/edid", O_RDONLY);
        CHECK(sf >= 0 && read(sf, sb, sizeof sb) == 128 && !memcmp(sb, e, 128), "sysfs edid");
        if (sf >= 0) close(sf);
    }
    drmModeFreePropertyBlob(eb);
    char sbuf[256];
    readfile("/sys/class/drm/card0-Virtual-1/status", sbuf, sizeof sbuf);
    CHECK(!strcmp(sbuf, "connected\n"), "sysfs status");

    /* IN_FORMATS */
    for (int pl = 0; pl < 2; pl++) {
        uint64_t bid = 0;
        uint32_t plane = pl ? cursor_id : primary_id;
        prop(fd, plane, DRM_MODE_OBJECT_PLANE, "IN_FORMATS", &bid);
        drmModePropertyBlobRes *b = bid ? drmModeGetPropertyBlob(fd, (uint32_t)bid) : NULL;
        CHECK(b, "IN_FORMATS blob");
        if (!b) continue;
        drmModeFormatModifierIterator it = { 0 };
        int xr = 0, ar = 0, other = 0;
        while (drmModeFormatModifierBlobIterNext(b, &it)) {
            if (it.mod != DRM_FORMAT_MOD_LINEAR) other++;
            else if (it.fmt == DRM_FORMAT_XRGB8888) xr++;
            else if (it.fmt == DRM_FORMAT_ARGB8888) ar++;
            else other++;
        }
        CHECK(ar == 1 && xr == !pl && !other, "IN_FORMATS plane %d: xr %d ar %d other %d", pl, xr, ar, other);
        drmModeFreePropertyBlob(b);
    }
    CHECK(P(conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID") != 0, "atomic CRTC_ID visible");

    /* framebuffers */
    struct buf A, B, C, X;
    CHECK(!mkbuf(fd, &A, W, H, DRM_FORMAT_XRGB8888) && !mkbuf(fd, &B, W, H, DRM_FORMAT_XRGB8888), "dumb fbs");
    CHECK(!mkbuf(fd, &C, 64, 64, DRM_FORMAT_ARGB8888) && !mkbuf(fd, &X, 64, 64, DRM_FORMAT_XRGB8888), "cursor fbs");
    fill(&A, 0, 0, W, H, 0x0000ff); fill(&B, 0, 0, W, H, 0x00ff00); fill(&C, 0, 0, 64, 64, 0x80808080u);
    CHECK(!drmModeCreatePropertyBlob(fd, &mode, sizeof mode, &mode_blob), "mode blob");
    int fbf = open("/dev/fb0", O_RDWR);
    struct fb_fix_screeninfo fix;
    if (fbf >= 0 && !ioctl(fbf, FBIOGET_FSCREENINFO, &fix)) {
        fb0_pitch = fix.line_length;
        fb0 = mmap(0, fix.smem_len, PROT_READ, MAP_SHARED, fbf, 0);
        if (fb0 == MAP_FAILED) fb0 = NULL;
    }

    /* atomic checks */
    r = drmModeAtomicAlloc();
    modeset_props(r, fd, 1); plane_full(r, fd, primary_id, A.fb, W, H, 0, 0);
    CHECK(!drmModeAtomicCommit(fd, r, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL), "TEST_ONLY modeset");
    CHECK_ERR(drmModeAtomicCommit(fd, r, DRM_MODE_ATOMIC_TEST_ONLY, NULL), EINVAL, "modeset without ALLOW_MODESET");
    CHECK_ERR(drmModeAtomicCommit(fd, r, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_PAGE_FLIP_EVENT, NULL), EINVAL, "TEST_ONLY + EVENT");
    CHECK_ERR(drmModeAtomicCommit(fd2, r, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL), EACCES, "atomic from non-master");
    drmModeAtomicReq *bad = drmModeAtomicDuplicate(r);
    drmModeAtomicAddProperty(bad, primary_id, P(primary_id, DRM_MODE_OBJECT_PLANE, "SRC_W"), (uint64_t)(W + 1) << 16);
    CHECK_ERR(drmModeAtomicCommit(fd, bad, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL), ENOSPC, "source outside the fb");
    drmModeAtomicFree(bad);
    bad = drmModeAtomicDuplicate(r);
    drmModeAtomicAddProperty(bad, primary_id, P(primary_id, DRM_MODE_OBJECT_PLANE, "SRC_W"), (uint64_t)(W / 2) << 16);
    CHECK_ERR(drmModeAtomicCommit(fd, bad, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL), ERANGE, "scaling");
    drmModeAtomicAddProperty(bad, primary_id, P(primary_id, DRM_MODE_OBJECT_PLANE, "CRTC_W"), W / 2);
    CHECK_ERR(drmModeAtomicCommit(fd, bad, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL), EINVAL, "primary not covering");
    drmModeAtomicFree(bad);
    bad = drmModeAtomicDuplicate(r);
    drmModeAtomicAddProperty(bad, conn_id, P(conn_id, DRM_MODE_OBJECT_CONNECTOR, "DPMS"), 3);
    CHECK_ERR(drmModeAtomicCommit(fd, bad, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL), EINVAL, "DPMS through atomic");
    drmModeAtomicFree(bad);
    bad = drmModeAtomicDuplicate(r);
    drmModeAtomicAddProperty(bad, conn_id, P(conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID"), 0);
    CHECK_ERR(drmModeAtomicCommit(fd, bad, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL), EINVAL, "CRTC without connector");
    drmModeAtomicFree(bad);
    bad = drmModeAtomicDuplicate(r);
    drmModeAtomicAddProperty(bad, cursor_id, P(cursor_id, DRM_MODE_OBJECT_PLANE, "FB_ID"), X.fb);
    drmModeAtomicAddProperty(bad, cursor_id, P(cursor_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID"), crtc_id);
    CHECK_ERR(drmModeAtomicCommit(fd, bad, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL), EINVAL, "XRGB cursor");
    drmModeAtomicFree(bad);
    bad = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(bad, 12345, 1, 0);
    CHECK_ERR(drmModeAtomicCommit(fd, bad, DRM_MODE_ATOMIC_TEST_ONLY, NULL), ENOENT, "unknown object");
    drmModeAtomicFree(bad);

    /* modeset */
    CHECK(!drmModeAtomicCommit(fd, r, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL), "modeset");
    drmModeAtomicFree(r);
    char so[256];
    scanout(so);
    copy_mode = scanout_is("copy");
    int zero_copy = scanout_is("zero-copy");
    CHECK(copy_mode || zero_copy, "scanout after modeset: %s", so);
    char dev[256] = "";
    ssize_t dl = readlink("/sys/class/drm/card0/device", dev, sizeof dev - 1);
    if (dl > 0) dev[dl] = 0;
    printf("kmstest: device %s, scanout %s", dev, so);
    if (strstr(dev, "virtio")) CHECK(zero_copy, "virtio-gpu: zero-copy scanout (%s)", so);
    CHECK_PX(10, 10, 0x0000ff);
    uint64_t act = 0;
    prop(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", &act);
    CHECK(act == 1, "ACTIVE reads back");
    readfile("/sys/class/drm/card0-Virtual-1/dpms", sbuf, sizeof sbuf);
    CHECK(!strcmp(sbuf, "On\n"), "sysfs dpms On");

    /* nonblocking commit + event, EBUSY while pending */
    CHECK(!flip_to(fd, B.fb, 0, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, (void *)0x1234), "nonblocking flip");
    CHECK_ERR(flip_to(fd, A.fb, 0, DRM_MODE_ATOMIC_NONBLOCK, NULL), EBUSY, "second nonblocking commit");
    CHECK(wait_event(fd) && ev_data == 0x1234, "flip event");
    CHECK_PX(10, 10, 0x00ff00);
    /* blocking commit with event */
    CHECK(!flip_to(fd, A.fb, 0, DRM_MODE_PAGE_FLIP_EVENT, (void *)0x55), "blocking commit with event");
    CHECK_PX(10, 10, 0x0000ff);
    CHECK(wait_event(fd) && ev_data == 0x55, "blocking commit event");
    /* legacy page flip */
    CHECK(!drmModePageFlip(fd, crtc_id, B.fb, DRM_MODE_PAGE_FLIP_EVENT, (void *)0x77), "legacy page flip");
    CHECK(wait_event(fd) && ev_data == 0x77, "legacy flip event");
    CHECK_PX(10, 10, 0x00ff00);
    CHECK_ERR(drmModePageFlip(fd2, crtc_id, A.fb, 0, NULL), EACCES, "page flip from non-master");
    CHECK(!flip_to(fd, A.fb, 0, 0, NULL), "back to A");

    /* FB_DAMAGE_CLIPS */
    fill(&A, 0, 0, 32, 32, 0xff0000); fill(&A, 100, 100, 140, 140, 0xff0000);
    struct drm_mode_rect clip = { 0, 0, 32, 32 };
    uint32_t dmg = 0;
    CHECK(!drmModeCreatePropertyBlob(fd, &clip, sizeof clip, &dmg), "damage blob");
    CHECK(!flip_to(fd, A.fb, dmg, 0, NULL), "commit with damage");
    CHECK_PX(10, 10, 0xff0000);
    CHECK_PX(110, 110, 0x0000ff);                 /* outside the damage: not updated */
    uint32_t badblob = 0;
    drmModeCreatePropertyBlob(fd, &clip, 10, &badblob);
    CHECK_ERR(flip_to(fd, A.fb, badblob, DRM_MODE_ATOMIC_TEST_ONLY, NULL), EINVAL, "bad damage blob size");
    CHECK(!flip_to(fd, A.fb, 0, 0, NULL), "commit without damage");
    CHECK_PX(110, 110, 0xff0000);
    CHECK_ERR(drmModeDestroyPropertyBlob(fd2, dmg), EPERM, "destroy someone else's blob");
    CHECK(!drmModeDestroyPropertyBlob(fd, dmg) && !drmModeDestroyPropertyBlob(fd, badblob), "destroy blobs");
    fill(&A, 0, 0, W, H, 0x0000ff);
    CHECK(!flip_to(fd, A.fb, 0, 0, NULL), "restore A");

    /* cursor plane */
    r = drmModeAtomicAlloc();
    plane_full(r, fd, cursor_id, C.fb, 64, 64, 200, 200);
    CHECK(!drmModeAtomicCommit(fd, r, 0, NULL), "cursor plane on");
    drmModeAtomicFree(r);
    CHECK_PX(210, 210, 0x8080ff);
    CHECK_PX(190, 190, 0x0000ff);
    if (zero_copy) { scanout(so); CHECK(strstr(so, "cursor hw"), "hardware cursor: %s", so); }
    CHECK(!set1(fd, cursor_id, DRM_MODE_OBJECT_PLANE, "CRTC_X", 300, 0), "cursor move");
    CHECK_PX(210, 210, 0x0000ff);
    CHECK_PX(310, 210, 0x8080ff);
    r = drmModeAtomicAlloc();
    plane_full(r, fd, cursor_id, 0, 0, 0, 0, 0);
    CHECK(!drmModeAtomicCommit(fd, r, 0, NULL), "cursor plane off");
    drmModeAtomicFree(r);
    CHECK_PX(310, 210, 0x0000ff);

    /* legacy cursor */
    CHECK(!drmModeSetCursor(fd, crtc_id, C.handle, 64, 64), "legacy cursor");
    CHECK(!drmModeMoveCursor(fd, crtc_id, 50, 50), "legacy cursor move");
    CHECK_PX(60, 60, 0x8080ff);
    CHECK(!drmModeSetCursor2(fd, crtc_id, C.handle, 64, 64, 10, 10), "cursor2 with hot spot");
    CHECK(!drmModeMoveCursor(fd, crtc_id, 50, 50), "cursor2 move");
    CHECK_PX(45, 45, 0x8080ff);
    CHECK_PX(112, 112, 0x0000ff);
    CHECK(!drmModeSetCursor(fd, crtc_id, 0, 0, 0), "cursor off");
    CHECK_PX(60, 60, 0x0000ff);

    /* GETFB2 */
    drmModeFB2 *f2 = drmModeGetFB2(fd, A.fb);
    CHECK(f2 && f2->width == (uint32_t)W && f2->pixel_format == DRM_FORMAT_XRGB8888 && f2->handles[0] && f2->pitches[0] == A.pitch, "GETFB2");
    drmModeFreeFB2(f2);

    /* DPMS */
    uint32_t dpms = P(conn_id, DRM_MODE_OBJECT_CONNECTOR, "DPMS");
    CHECK(!drmModeConnectorSetProperty(fd, conn_id, dpms, DRM_MODE_DPMS_OFF), "DPMS off");
    CHECK(scanout_is("console"), "DPMS off shows the console");
    readfile("/sys/class/drm/card0-Virtual-1/dpms", sbuf, sizeof sbuf);
    CHECK(!strcmp(sbuf, "Off\n"), "sysfs dpms Off");
    CHECK(!drmModeConnectorSetProperty(fd, conn_id, dpms, DRM_MODE_DPMS_ON), "DPMS on");
    CHECK(!scanout_is("console"), "DPMS on");
    CHECK_PX(10, 10, 0x0000ff);

    /* legacy SETCRTC */
    CHECK(!drmModeSetCrtc(fd, crtc_id, B.fb, 0, 0, &conn_id, 1, &mode), "SETCRTC");
    drmModeCrtc *c = drmModeGetCrtc(fd, crtc_id);
    CHECK(c && c->buffer_id == B.fb && c->mode_valid, "GETCRTC");
    drmModeFreeCrtc(c);
    CHECK_PX(10, 10, 0x00ff00);
    CHECK_ERR(drmModeSetCrtc(fd2, crtc_id, B.fb, 0, 0, &conn_id, 1, &mode), EACCES, "SETCRTC from non-master");

    /* PRIME and flink */
    int pfd = -1;
    CHECK(!drmPrimeHandleToFD(fd, A.handle, DRM_CLOEXEC | DRM_RDWR, &pfd) && pfd >= 0, "PRIME export");
    if (pfd >= 0) {
        CHECK(lseek(pfd, 0, SEEK_END) == (off_t)A.size, "dma-buf size");
        uint32_t *m = mmap(0, A.size, PROT_READ | PROT_WRITE, MAP_SHARED, pfd, 0);
        CHECK(m != MAP_FAILED && m[5] == 0x0000ff, "dma-buf mmap");
        struct dma_buf_sync s = { DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW };
        CHECK(!ioctl(pfd, DMA_BUF_IOCTL_SYNC, &s), "DMA_BUF_IOCTL_SYNC");
        if (m != MAP_FAILED) { m[7] = 0xabcdef; munmap(m, A.size); }
        s.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
        CHECK(!ioctl(pfd, DMA_BUF_IOCTL_SYNC, &s), "DMA_BUF_IOCTL_SYNC end");
        s.flags = 0x100;
        CHECK_ERR(ioctl(pfd, DMA_BUF_IOCTL_SYNC, &s), EINVAL, "bad sync flags");
        CHECK(A.map[7] == 0xabcdef, "dma-buf shares the pages");
        uint32_t h = 0, h2 = 0;
        CHECK(!drmPrimeFDToHandle(fd, pfd, &h) && h == A.handle, "self import gives the same handle");
        CHECK(!drmPrimeFDToHandle(fd2, pfd, &h2) && h2, "PRIME import");
        uint32_t hs[4] = { h2 }, ps[4] = { A.pitch }, os[4] = { 0 }, fb2 = 0;
        CHECK(!drmModeAddFB2(fd2, W, H, DRM_FORMAT_XRGB8888, hs, ps, os, &fb2, 0), "ADDFB2 on an imported buffer");
        if (fb2) CHECK(!drmModeRmFB(fd2, fb2), "RMFB imported");
        int pfd2 = -1;
        CHECK(!drmPrimeHandleToFD(fd, A.handle, 0, &pfd2), "second export");
        if (pfd2 >= 0) close(pfd2);
        close(pfd);
        struct drm_gem_flink fl = { .handle = B.handle };
        CHECK(!drmIoctl(fd, DRM_IOCTL_GEM_FLINK, &fl) && fl.name, "GEM_FLINK");
        struct drm_gem_open op = { .name = fl.name };
        CHECK(!drmIoctl(fd2, DRM_IOCTL_GEM_OPEN, &op) && op.handle && op.size == B.size, "GEM_OPEN");
        struct drm_gem_close gc = { .handle = op.handle };
        CHECK(!drmIoctl(fd2, DRM_IOCTL_GEM_CLOSE, &gc), "GEM_CLOSE");
        CHECK_ERR(drmIoctl(fd2, DRM_IOCTL_GEM_CLOSE, &gc), EINVAL, "GEM_CLOSE twice");
    }

    /* VT switch to a text console takes the display back; the next commit shows the planes again */
    int tty = open("/dev/tty0", O_RDWR);
    struct vt_stat vs;
    if (tty >= 0 && !ioctl(tty, VT_GETSTATE, &vs)) {
        CHECK(!ioctl(tty, VT_ACTIVATE, 2) && !ioctl(tty, VT_WAITACTIVE, 2), "VT_ACTIVATE 2");
        CHECK(scanout_is("console"), "VT switch reclaims the display");
        CHECK(!ioctl(tty, VT_ACTIVATE, vs.v_active) && !ioctl(tty, VT_WAITACTIVE, vs.v_active), "VT back");
        CHECK(!flip_to(fd, A.fb, 0, 0, NULL), "commit after VT switch");
        CHECK(!scanout_is("console"), "planes shown again");
        CHECK_PX(10, 10, 0x0000ff);
        close(tty);
    } else CHECK(0, "open /dev/tty0");

    /* RMFB of the scanned-out fb disables the CRTC */
    CHECK(!drmModeRmFB(fd, A.fb), "RMFB");
    c = drmModeGetCrtc(fd, crtc_id);
    CHECK(c && !c->mode_valid && !c->buffer_id, "RMFB disabled the CRTC");
    drmModeFreeCrtc(c);
    CHECK(scanout_is("console"), "console after RMFB");
    CHECK_ERR(drmModeRmFB(fd, A.fb), ENOENT, "RMFB twice");

    /* an active CRTC without a primary plane scans out black */
    r = drmModeAtomicAlloc();
    modeset_props(r, fd, 1);
    CHECK(!drmModeAtomicCommit(fd, r, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL), "CRTC without primary");
    drmModeAtomicFree(r);
    CHECK(scanout_is("copy"), "black CRTC is composited");
    CHECK_PX(10, 10, 0x000000);

    /* CLOSEFB keeps the fb on screen until it is replaced */
    r = drmModeAtomicAlloc();
    plane_full(r, fd, primary_id, B.fb, W, H, 0, 0);
    CHECK(!drmModeAtomicCommit(fd, r, 0, NULL), "primary B");
    drmModeAtomicFree(r);
    CHECK(!drmModeCloseFB(fd, B.fb), "CLOSEFB");
    c = drmModeGetCrtc(fd, crtc_id);
    CHECK(c && c->buffer_id == B.fb && c->mode_valid, "CLOSEFB keeps scanning out");
    drmModeFreeCrtc(c);
    CHECK_PX(10, 10, 0x00ff00);

    /* lastclose restores the console; the next opener is master again */
    close(fd2);
    CHECK(!scanout_is("console"), "display kept while a file is open");
    close(fd);
    CHECK(scanout_is("console"), "lastclose restores the console");
    fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0 && drmIsMaster(fd), "reopen is master");
    close(fd);
    scanout(so);
    printf("kmstest: %s (%s)", fails ? "FAILED" : "PASSED", so);
    return fails ? 1 : 0;
}
