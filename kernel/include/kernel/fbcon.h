#pragma once
#include <kernel/types.h>
void fbcon_init(void);
void fbcon_write(const char *s, size_t n);
void fbcon_set_graphics(bool on);    /* pause text output while a client draws */
/* Display flushing for framebuffers that are not scanned out directly (virtio-gpu). */
extern void (*fb_flush_hook)(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);
void fb_damage(void);        /* mark the framebuffer dirty; flushed asynchronously */
void fb_damage_rect(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);   /* [x0,x1) x [y0,y1) */
/* set while a client (DRM master) reports its own damage, so idle frames are not re-sent */
extern bool fb_explicit_damage;
bool fb_graphics_active(void);
