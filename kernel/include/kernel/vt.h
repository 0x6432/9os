#pragma once
/* M35: virtual terminals tty1..tty6 (major 4, minor 1..6; tty0 = the foreground VT). tty1 is
 * the kernel console (serial input and printk); the others are text consoles on the framebuffer. */
#include <kernel/types.h>
#define NR_VT 6
struct tty;
int vt_fg(void);                       /* 0-based foreground VT */
struct tty *vt_fg_tty(void);
int vt_kbmode(void);                   /* keyboard mode of the foreground VT */
void vt_activate(int vt);              /* 0-based; from keyboard (Alt+Fn) or VT_ACTIVATE */
int vt_index(struct tty *t);           /* -1: not a VT */
int vt_ioctl(struct tty *t, uint64_t cmd, uint64_t arg);   /* -ENOTTY: not a VT or not a VT ioctl */
void vt_kbd_switch(int vt);            /* keyboard request (any context) */
void vt_late_init(void);
void vt_open(struct tty *t);
void vt_close(struct tty *t);
void vt_register_devices(void);

/* fbcon side */
void fbcon_vt_init(void);
void fbcon_vt_write(int vt, const char *s, size_t n);
void fbcon_vt_switch(int vt);
void fbcon_vt_set_kd_graphics(int vt, bool on);
size_t fbcon_vcs_read(int vt, char *buf, size_t n, uint64_t off);   /* vt -1: foreground */
size_t fbcon_vcs_size(void);
void fbcon_get_size(int *cols, int *rows);
