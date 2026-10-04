#pragma once
#include <kernel/types.h>
struct trap_frame;
typedef void (*irq_handler_t)(struct trap_frame *f, void *ctx);
void irq_register_vector(int vector, irq_handler_t h, void *ctx);
/* Route a legacy/global interrupt line to a handler. Returns the vector used. */
int irq_install(int gsi, irq_handler_t h, void *ctx);
void irq_eoi(void);
