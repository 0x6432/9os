/* host stand-in for kernel/types.h */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#define nullptr NULL
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
