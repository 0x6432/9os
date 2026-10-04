/*
 * /dev/fb0: Linux fbdev ABI on top of the bootloader framebuffer.
 * Supports FBIOGET_VSCREENINFO / FBIOPUT_VSCREENINFO (no mode changes) / FBIOGET_FSCREENINFO /
 * FBIOPAN_DISPLAY / FBIOBLANK, read/write at offsets and mmap of the pixel memory.
 * The text console pauses while the device is open.
 */
#include <kernel/vfs.h>
#include <kernel/boot.h>
#include <kernel/fbcon.h>
#include <kernel/mm.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/printk.h>

struct fb_bitfield { uint32_t offset, length, msb_right; };
struct fb_var_screeninfo {
    uint32_t xres, yres, xres_virtual, yres_virtual, xoffset, yoffset, bits_per_pixel, grayscale;
    struct fb_bitfield red, green, blue, transp;
    uint32_t nonstd, activate, height, width, accel_flags, pixclock, left_margin, right_margin,
             upper_margin, lower_margin, hsync_len, vsync_len, sync, vmode, rotate, colorspace, reserved[4];
};
struct fb_fix_screeninfo {
    char id[16];
    unsigned long smem_start;
    uint32_t smem_len, type, type_aux, visual;
    uint16_t xpanstep, ypanstep, ywrapstep;
    uint32_t line_length;
    unsigned long mmio_start;
    uint32_t mmio_len, accel;
    uint16_t capabilities, reserved[2];
};
_Static_assert(sizeof(struct fb_var_screeninfo) == 160, "fb_var_screeninfo ABI");
_Static_assert(sizeof(struct fb_fix_screeninfo) == 80, "fb_fix_screeninfo ABI");

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOPAN_DISPLAY     0x4606
#define FBIOBLANK           0x4611

static struct limine_framebuffer *fb;
static uint64_t fb_size;
static int opens;

static void fill_var(struct fb_var_screeninfo *v) {
    memset(v, 0, sizeof *v);
    v->xres = v->xres_virtual = fb->width;
    v->yres = v->yres_virtual = fb->height;
    v->bits_per_pixel = fb->bpp;
    v->red = (struct fb_bitfield){ fb->red_mask_shift, fb->red_mask_size, 0 };
    v->green = (struct fb_bitfield){ fb->green_mask_shift, fb->green_mask_size, 0 };
    v->blue = (struct fb_bitfield){ fb->blue_mask_shift, fb->blue_mask_size, 0 };
    v->height = v->width = (uint32_t)-1;
    v->pixclock = 10000;
}

static int fb_ioctl(struct file *f, uint64_t cmd, uint64_t arg) {
    switch (cmd) {
    case FBIOGET_VSCREENINFO: case FBIOPUT_VSCREENINFO: {
        struct fb_var_screeninfo v;
        fill_var(&v);
        return copy_to_user((void *)arg, &v, sizeof v) ? -EFAULT : 0;    /* PUT: mode is fixed */
    }
    case FBIOGET_FSCREENINFO: {
        struct fb_fix_screeninfo x;
        memset(&x, 0, sizeof x);
        strlcpy(x.id, "9os-liminefb", sizeof x.id);
        x.smem_start = VIRT_TO_PHYS(fb->address);
        x.smem_len = fb_size;
        x.visual = 2;            /* FB_VISUAL_TRUECOLOR */
        x.line_length = fb->pitch;
        return copy_to_user((void *)arg, &x, sizeof x) ? -EFAULT : 0;
    }
    case FBIOPAN_DISPLAY: case FBIOBLANK: return 0;
    default: return -ENOTTY;
    }
}

static ssize_t fb_read(struct file *f, void *buf, size_t n, off_t *off) {
    if ((uint64_t)*off >= fb_size) return 0;
    n = MIN(n, fb_size - *off);
    memcpy(buf, (uint8_t *)fb->address + *off, n);
    *off += n;
    return n;
}

static ssize_t fb_write(struct file *f, const void *buf, size_t n, off_t *off) {
    if ((uint64_t)*off >= fb_size) return -ENOSPC;
    n = MIN(n, fb_size - *off);
    memcpy((uint8_t *)fb->address + *off, buf, n);
    *off += n;
    return n;
}

static int fb_mmap(struct file *f, uint64_t off, size_t len, paddr_t *pa) {
    if (off + len > ALIGN_UP(fb_size, PAGE_SIZE)) return -EINVAL;
    *pa = VIRT_TO_PHYS(fb->address) + off;
    return 0;
}

static int fb_open(struct inode *ino, struct file *f) {
    if (opens++ == 0) fbcon_set_graphics(true);
    return 0;
}
static void fb_release(struct file *f) {
    if (--opens == 0) fbcon_set_graphics(false);
}

static const struct file_ops fb_ops = {
    .open = fb_open, .read = fb_read, .write = fb_write, .ioctl = fb_ioctl,
    .release = fb_release, .mmap = fb_mmap,
};

void chrdev_register(unsigned major, unsigned minor, const struct file_ops *ops);

void fbdev_init(void) {
    fb = boot_framebuffer();
    if (!fb || fb->bpp != 32) { pr_info("fbdev: no 32bpp framebuffer\n"); return; }
    fb_size = fb->pitch * fb->height;
    chrdev_register(29, 0, &fb_ops);
    vfs_mknod_at(nullptr, "/dev/fb0", S_IFCHR | 0660, MKDEV(29, 0));
    pr_info("fbdev: /dev/fb0 %lux%lu, %u bpp, pitch %lu\n", fb->width, fb->height, fb->bpp, fb->pitch);
}
