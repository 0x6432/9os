#pragma once
/* virtio-gpu scanout interface for DRM zero-copy scanout (M36); see virtio_gpu.c */
#include <kernel/types.h>

bool vgpu_present(void);
/* host resource (B8G8R8A8/X8, pitch w*4) backed by the given pages */
int vgpu_res_create(bool alpha, uint32_t w, uint32_t h, const paddr_t *pages, size_t npages, uint32_t *res);
void vgpu_res_destroy(uint32_t res);
/* scan out res (0: the console framebuffer) and push the damaged rect to the host */
int vgpu_show(uint32_t res, uint32_t pitch, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
uint32_t vgpu_scanout_res(void);
/* hardware cursor (64x64 resource); res 0 hides it, update=false only moves it */
int vgpu_cursor(uint32_t res, int32_t x, int32_t y, uint32_t hot_x, uint32_t hot_y, bool update);

/* drm.c: a VT switch to a text console takes the display back from KMS clients */
void drm_console_reclaim(void);
