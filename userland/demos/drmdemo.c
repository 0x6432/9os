/*
 * drmdemo: KMS dumb-buffer client using raw DRM ioctls (what libdrm does underneath):
 * enumerate resources, create two dumb buffers, SETCRTC, then animate with PAGE_FLIP and
 * flip-complete events read from the card fd. usage: drmdemo [frames]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define IOWR(nr, sz) (0xC0000000u | ((sz) << 16) | ('d' << 8) | (nr))
#define IOW(nr, sz)  (0x40000000u | ((sz) << 16) | ('d' << 8) | (nr))

struct modeinfo { uint32_t clock; uint16_t hd, hss, hse, ht, hsk, vd, vss, vse, vt, vs; uint32_t vrefresh, flags, type; char name[32]; };
struct version { int32_t maj, min, patch, pad; uint64_t name_len, name, date_len, date, desc_len, desc; };
struct card_res { uint64_t fb, crtc, conn, enc; uint32_t nfb, ncrtc, nconn, nenc, minw, maxw, minh, maxh; };
struct get_conn { uint64_t enc_ptr, modes_ptr, props_ptr, pv_ptr; uint32_t nmodes, nprops, nenc, enc_id, conn_id, type, type_id, connection, mm_w, mm_h, subpixel, pad; };
struct get_enc { uint32_t id, type, crtc_id, possible_crtcs, possible_clones; };
struct mcrtc { uint64_t conn_ptr; uint32_t nconn, crtc_id, fb_id, x, y, gamma_size, mode_valid; struct modeinfo mode; };
struct create_dumb { uint32_t h, w, bpp, flags, handle, pitch; uint64_t size; };
struct map_dumb { uint32_t handle, pad; uint64_t offset; };
struct fb_cmd2 { uint32_t fb_id, w, h, fmt, flags, handles[4], pitches[4], offsets[4]; uint64_t mod[4]; };
struct page_flip { uint32_t crtc_id, fb_id, flags, reserved; uint64_t user_data; };
struct get_cap { uint64_t cap, value; };
struct ev_vblank { uint32_t type, length; uint64_t user_data; uint32_t sec, usec, seq, crtc; };

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("  FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

struct buf { uint32_t handle, pitch, fb; uint64_t size; uint32_t *px; };

static void draw(struct buf *b, int w, int h, int frame) {
    int bar = (frame * 8) % w;
    for (int y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)((char *)b->px + (size_t)y * b->pitch);
        for (int x = 0; x < w; x++) {
            uint32_t r = (x * 255 / w), g = (y * 255 / h), bl = (frame * 4) & 255;
            row[x] = (r << 16) | (g << 8) | bl;
        }
        for (int x = bar; x < bar + 24 && x < w; x++) row[x] = 0xffffff;
    }
}

int main(int argc, char **argv) {
    int frames = argc > 1 ? atoi(argv[1]) : 120;
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("drmdemo: /dev/dri/card0"); return 1; }
    char name[32] = {0}, desc[64] = {0};
    struct version v = { .name_len = sizeof name - 1, .name = (uintptr_t)name, .desc_len = sizeof desc - 1, .desc = (uintptr_t)desc };
    CHECK(ioctl(fd, IOWR(0x00, sizeof v), &v) == 0);
    struct get_cap cap = { 1, 0 };
    CHECK(ioctl(fd, IOWR(0x0c, sizeof cap), &cap) == 0 && cap.value == 1);
    ioctl(fd, 0x641e, 0);                                   /* SET_MASTER */

    struct card_res r = {0};
    CHECK(ioctl(fd, IOWR(0xA0, sizeof r), &r) == 0 && r.ncrtc == 1 && r.nconn >= 1);
    uint32_t crtcs[4], conns[4], encs[4];
    r.crtc = (uintptr_t)crtcs; r.conn = (uintptr_t)conns; r.enc = (uintptr_t)encs; r.nfb = 0;
    CHECK(ioctl(fd, IOWR(0xA0, sizeof r), &r) == 0);

    struct get_conn c = { .conn_id = conns[0] };
    CHECK(ioctl(fd, IOWR(0xA7, sizeof c), &c) == 0 && c.connection == 1 && c.nmodes >= 1);
    struct modeinfo modes[8]; uint32_t cenc[4];
    c.modes_ptr = (uintptr_t)modes; c.enc_ptr = (uintptr_t)cenc; c.nmodes = c.nmodes > 8 ? 8 : c.nmodes; c.nenc = 4; c.nprops = 0;
    CHECK(ioctl(fd, IOWR(0xA7, sizeof c), &c) == 0);
    struct get_enc e = { .id = c.enc_id };
    CHECK(ioctl(fd, IOWR(0xA6, sizeof e), &e) == 0 && (e.possible_crtcs & 1));
    struct modeinfo m = modes[0];
    printf("drmdemo: driver %s (%s), connector %u type %u, mode %s %ux%u@%u\n",
           name, desc, c.conn_id, c.type, m.name, m.hd, m.vd, m.vrefresh);
    struct mcrtc saved = { .crtc_id = crtcs[0] };
    CHECK(ioctl(fd, IOWR(0xA1, sizeof saved), &saved) == 0);

    struct buf b[2];
    for (int i = 0; i < 2; i++) {
        struct create_dumb cd = { .h = m.vd, .w = m.hd, .bpp = 32 };
        CHECK(ioctl(fd, IOWR(0xB2, sizeof cd), &cd) == 0 && cd.pitch >= m.hd * 4u);
        struct map_dumb md = { .handle = cd.handle };
        CHECK(ioctl(fd, IOWR(0xB3, sizeof md), &md) == 0);
        b[i].px = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
        CHECK(b[i].px != MAP_FAILED);
        if (b[i].px == MAP_FAILED) return 1;
        b[i].handle = cd.handle; b[i].pitch = cd.pitch; b[i].size = cd.size;
        struct fb_cmd2 f = { .w = m.hd, .h = m.vd, .fmt = 0x34325258 /* XR24 */, .handles = { cd.handle }, .pitches = { cd.pitch } };
        CHECK(ioctl(fd, IOWR(0xB8, sizeof f), &f) == 0);
        b[i].fb = f.fb_id;
    }
    draw(&b[0], m.hd, m.vd, 0);
    struct mcrtc set = { .conn_ptr = (uintptr_t)&c.conn_id, .nconn = 1, .crtc_id = crtcs[0], .fb_id = b[0].fb, .mode_valid = 1, .mode = m };
    CHECK(ioctl(fd, IOWR(0xA2, sizeof set), &set) == 0);
    struct mcrtc cur = { .crtc_id = crtcs[0] };
    CHECK(ioctl(fd, IOWR(0xA1, sizeof cur), &cur) == 0 && cur.fb_id == b[0].fb && cur.mode_valid);

    double t0 = now();
    int front = 0, got = 0; uint32_t last_seq = 0; int seq_ok = 1;
    for (int fr = 1; fr <= frames; fr++) {
        int back = front ^ 1;
        draw(&b[back], m.hd, m.vd, fr);
        struct page_flip pf = { crtcs[0], b[back].fb, 1 /* EVENT */, 0, (uint64_t)fr };
        if (ioctl(fd, IOWR(0xB0, sizeof pf), &pf) != 0) { CHECK(!"page flip"); break; }
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 1000) != 1) { CHECK(!"flip event timeout"); break; }
        struct ev_vblank ev;
        if (read(fd, &ev, sizeof ev) == sizeof ev && ev.type == 2 && ev.user_data == (uint64_t)fr) got++;
        if (ev.seq <= last_seq) seq_ok = 0;
        last_seq = ev.seq;
        front = back;
    }
    double dt = now() - t0;
    CHECK(got == frames);
    CHECK(seq_ok);
    printf("drmdemo: %d flips in %.2f s (%.1f fps)\n", got, dt, got / dt);
    /* flipping to a removed fb fails; restore the console */
    uint32_t id = b[1].fb;
    struct page_flip bad = { crtcs[0], 999999, 0, 0, 0 };
    CHECK(ioctl(fd, IOWR(0xB0, sizeof bad), &bad) != 0);
    CHECK(ioctl(fd, IOWR(0xAF, 4), &id) == 0);
    struct mcrtc off = { .crtc_id = crtcs[0] };
    CHECK(ioctl(fd, IOWR(0xA2, sizeof off), &off) == 0);
    for (int i = 0; i < 2; i++) { munmap(b[i].px, b[i].size); uint32_t h = b[i].handle; CHECK(ioctl(fd, IOWR(0xB4, 4), &h) == 0); }
    close(fd);
    printf("drmdemo: %s (%d checks, %d failures)\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails != 0;
}
