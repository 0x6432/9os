#pragma once
#include <kernel/types.h>
void fbcon_init(void);
void fbcon_write(const char *s, size_t n);
