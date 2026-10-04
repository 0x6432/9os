/*
 * fbdemo: framebuffer demos for 9os (Linux fbdev API: /dev/fb0 + ioctl + mmap).
 *   fbdemo [-d seconds] [-j threads] [scene...]
 * scenes: lines circles mandel julia plasma fern sierpinski (default: all)
 * Mandelbrot and Julia are rendered in parallel by one pthread per CPU.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/fb.h>

static uint8_t *fbmem;
static uint32_t W, H, pitch;
static struct fb_var_screeninfo var;
static int nthreads = 1;

static uint32_t rgb(int r, int g, int b) {
    return ((uint32_t)(r & 255) << var.red.offset) | ((uint32_t)(g & 255) << var.green.offset) |
           ((uint32_t)(b & 255) << var.blue.offset);
}
static inline void put(int x, int y, uint32_t c) {
    if ((unsigned)x < W && (unsigned)y < H) *(volatile uint32_t *)(fbmem + (size_t)y * pitch + (size_t)x * 4) = c;
}
static void clear(uint32_t c) {
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) *(uint32_t *)(fbmem + (size_t)y * pitch + x * 4) = c;
}
static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static uint64_t rng = 88172645463325252ULL;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }

/* HSV-ish palette */
static uint32_t hue(double t) {
    double r = 0.5 + 0.5 * cos(6.28318 * (t + 0.00)), g = 0.5 + 0.5 * cos(6.28318 * (t + 0.33)),
           b = 0.5 + 0.5 * cos(6.28318 * (t + 0.67));
    return rgb((int)(r * 255), (int)(g * 255), (int)(b * 255));
}

/* ---- lines: Bresenham */
static void line(int x0, int y0, int x1, int y1, uint32_t c) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        put(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
static void scene_lines(void) {
    clear(rgb(0, 0, 0));
    int cx = W / 2, cy = H / 2, n = 180;
    for (int i = 0; i < n; i++) {
        double a = i * 6.28318 / n;
        line(cx, cy, cx + (int)(cos(a) * W), cy + (int)(sin(a) * H), hue((double)i / n));
    }
    /* string-art envelope in the corner */
    int s = H / 3;
    for (int i = 0; i <= 40; i++) line(10, 10 + i * s / 40, 10 + i * s / 40, 10 + s, rgb(255, 255, 255));
    for (int i = 0; i < 300; i++) line(rnd() % W, rnd() % H, rnd() % W, rnd() % H, hue((rnd() % 1000) / 1000.0));
}

/* ---- circles: midpoint algorithm */
static void circle(int cx, int cy, int r, uint32_t c) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        put(cx + x, cy + y, c); put(cx + y, cy + x, c); put(cx - y, cy + x, c); put(cx - x, cy + y, c);
        put(cx - x, cy - y, c); put(cx - y, cy - x, c); put(cx + y, cy - x, c); put(cx + x, cy - y, c);
        y++;
        if (err < 0) err += 2 * y + 1; else { x--; err += 2 * (y - x) + 1; }
    }
}
static void scene_circles(void) {
    clear(rgb(8, 8, 24));
    for (int r = 4; r < (int)H / 2; r += 4) circle(W / 2, H / 2, r, hue(r / (double)H));
    for (int i = 0; i < 200; i++) circle(rnd() % W, rnd() % H, 5 + rnd() % 60, hue((rnd() % 1000) / 1000.0));
}

/* ---- Mandelbrot / Julia (parallel rows) */
struct job { int kind, id; double cx, cy, scale, jr, ji; int maxit; };
static void *frac_worker(void *arg) {
    struct job *j = arg;
    for (uint32_t y = j->id; y < H; y += nthreads) {
        for (uint32_t x = 0; x < W; x++) {
            double zr, zi, cr, ci;
            double px = j->cx + ((double)x - W / 2.0) * j->scale, py = j->cy + ((double)y - H / 2.0) * j->scale;
            if (j->kind == 0) { zr = zi = 0; cr = px; ci = py; } else { zr = px; zi = py; cr = j->jr; ci = j->ji; }
            int i = 0;
            while (i < j->maxit && zr * zr + zi * zi < 16.0) {
                double t = zr * zr - zi * zi + cr;
                zi = 2 * zr * zi + ci; zr = t; i++;
            }
            if (i == j->maxit) put(x, y, rgb(0, 0, 0));
            else {
                double sm = i + 1 - log(log(sqrt(zr * zr + zi * zi))) / log(2.0);   /* smooth colouring */
                put(x, y, hue(0.6 + sm * 0.025));
            }
        }
    }
    return NULL;
}
static void fractal(int kind, double cx, double cy, double scale, double jr, double ji, int maxit) {
    pthread_t t[64];
    struct job jobs[64];
    double t0 = now();
    for (int i = 0; i < nthreads; i++) {
        jobs[i] = (struct job){ kind, i, cx, cy, scale, jr, ji, maxit };
        pthread_create(&t[i], NULL, frac_worker, &jobs[i]);
    }
    for (int i = 0; i < nthreads; i++) pthread_join(t[i], NULL);
    printf("  %s %ux%u, %d iterations max: %.3f s with %d thread(s)\n", kind ? "julia" : "mandelbrot", W, H, maxit,
           now() - t0, nthreads);
}
static void scene_mandel(void) { fractal(0, -0.6, 0.0, 3.2 / W, 0, 0, 256); }
static void scene_julia(void) { fractal(1, 0.0, 0.0, 3.0 / W, -0.8, 0.156, 256); }

/* ---- plasma (animated) */
static void scene_plasma(double secs) {
    double t0 = now();
    int frames = 0;
    int step = W > 800 ? 2 : 1;
    while (now() - t0 < secs) {
        double t = frames * 0.15;
        for (uint32_t y = 0; y < H; y += step)
            for (uint32_t x = 0; x < W; x += step) {
                double v = sin(x * 0.02 + t) + sin(y * 0.03 + t * 1.3) + sin((x + y) * 0.015 + t * 0.7) +
                           sin(sqrt((double)(x - W / 2) * (x - W / 2) + (double)(y - H / 2) * (y - H / 2)) * 0.03 - t);
                uint32_t c = hue(v * 0.125);
                put(x, y, c);
                if (step == 2) { put(x + 1, y, c); put(x, y + 1, c); put(x + 1, y + 1, c); }
            }
        frames++;
    }
    printf("  plasma: %d frames in %.1f s (%.1f fps)\n", frames, now() - t0, frames / (now() - t0));
}

/* ---- Barnsley fern (IFS) */
static void scene_fern(void) {
    clear(rgb(0, 0, 0));
    double x = 0, y = 0;
    for (int i = 0; i < 400000; i++) {
        uint32_t r = rnd() % 100;
        double nx, ny;
        if (r < 1) { nx = 0; ny = 0.16 * y; }
        else if (r < 86) { nx = 0.85 * x + 0.04 * y; ny = -0.04 * x + 0.85 * y + 1.6; }
        else if (r < 93) { nx = 0.2 * x - 0.26 * y; ny = 0.23 * x + 0.22 * y + 1.6; }
        else { nx = -0.15 * x + 0.28 * y; ny = 0.26 * x + 0.24 * y + 0.44; }
        x = nx; y = ny;
        put(W / 2 + (int)(x * H / 11), H - (int)(y * H / 10.5), rgb(40, 200 + (int)(y * 5), 60));
    }
}

/* ---- Sierpinski triangle (chaos game) */
static void scene_sierpinski(void) {
    clear(rgb(255, 255, 255));
    int vx[3] = { (int)W / 2, (int)W / 10, (int)W * 9 / 10 }, vy[3] = { (int)H / 20, (int)H * 19 / 20, (int)H * 19 / 20 };
    double x = W / 2, y = H / 2;
    for (int i = 0; i < 300000; i++) {
        int k = rnd() % 3;
        x = (x + vx[k]) / 2; y = (y + vy[k]) / 2;
        put((int)x, (int)y, k == 0 ? rgb(200, 0, 0) : k == 1 ? rgb(0, 150, 0) : rgb(0, 0, 200));
    }
}

int main(int argc, char **argv) {
    double delay = 2.0;
    int opt;
    nthreads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    while ((opt = getopt(argc, argv, "d:j:h")) != -1) {
        if (opt == 'd') delay = atof(optarg);
        else if (opt == 'j') nthreads = atoi(optarg);
        else { fprintf(stderr, "usage: fbdemo [-d secs] [-j threads] [lines|circles|mandel|julia|plasma|fern|sierpinski]...\n"); return 1; }
    }
    if (nthreads < 1) nthreads = 1;
    if (nthreads > 64) nthreads = 64;
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) { perror("fbdemo: /dev/fb0"); return 1; }
    struct fb_fix_screeninfo fix;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) || ioctl(fd, FBIOGET_FSCREENINFO, &fix)) { perror("fbdemo: ioctl"); return 1; }
    W = var.xres; H = var.yres; pitch = fix.line_length;
    fbmem = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fbmem == MAP_FAILED) { perror("fbdemo: mmap"); return 1; }
    printf("fbdemo: %s %ux%u %ubpp pitch %u, %d threads\n", fix.id, W, H, var.bits_per_pixel, pitch, nthreads);
    const char *all[] = { "lines", "circles", "mandel", "julia", "plasma", "fern", "sierpinski" };
    int n = argc - optind;
    const char **scenes = n ? (const char **)argv + optind : all;
    if (!n) n = 7;
    for (int i = 0; i < n; i++) {
        const char *s = scenes[i];
        printf("scene: %s\n", s);
        if (!strcmp(s, "lines")) scene_lines();
        else if (!strcmp(s, "circles")) scene_circles();
        else if (!strcmp(s, "mandel")) scene_mandel();
        else if (!strcmp(s, "julia")) scene_julia();
        else if (!strcmp(s, "plasma")) { scene_plasma(delay); continue; }
        else if (!strcmp(s, "fern")) scene_fern();
        else if (!strcmp(s, "sierpinski")) scene_sierpinski();
        else { printf("  unknown scene\n"); continue; }
        usleep((useconds_t)(delay * 1e6));
    }
    munmap(fbmem, fix.smem_len);
    close(fd);
    return 0;
}
