#pragma once
#include <kernel/types.h>
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void *krealloc(void *p, size_t size);
void kfree(void *p);
