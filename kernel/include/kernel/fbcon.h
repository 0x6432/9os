#pragma once
#include <kernel/types.h>
void fbcon_init(void);
void fbcon_write(const char *s, size_t n);
void fbcon_set_graphics(bool on);    /* pause text output while a client draws */
/* Display flushing for framebuffers that are not scanned out directly (virtio-gpu). */
extern void (*fb_flush_hook)(void);
void fb_damage(void);        /* mark the framebuffer dirty; flushed asynchronously */
bool fb_graphics_active(void);
