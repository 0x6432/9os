#pragma once
#include <kernel/types.h>
#include <kernel/list.h>

struct thread {
    int tid;
};
extern struct thread *current;
void sched_yield(void);
