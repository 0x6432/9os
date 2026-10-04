#pragma once
#include <kernel/types.h>
void fbcon_init(void);
void fbcon_write(const char *s, size_t n);
void fbcon_set_graphics(bool on);    /* pause text output while a client draws */
