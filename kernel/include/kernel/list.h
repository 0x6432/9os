#pragma once
#include <kernel/types.h>

struct list_node { struct list_node *next, *prev; };
#define LIST_INIT(name) { &(name), &(name) }
static inline void list_init(struct list_node *l) { l->next = l->prev = l; }
static inline bool list_empty(const struct list_node *l) { return l->next == l; }
static inline void __list_add(struct list_node *n, struct list_node *p, struct list_node *nx) {
    nx->prev = n; n->next = nx; n->prev = p; p->next = n;
}
static inline void list_add(struct list_node *l, struct list_node *n) { __list_add(n, l, l->next); }
static inline void list_add_tail(struct list_node *l, struct list_node *n) { __list_add(n, l->prev, l); }
static inline void list_del(struct list_node *n) {
    n->prev->next = n->next; n->next->prev = n->prev; n->next = n->prev = n;
}
#define list_entry(ptr, type, member) container_of(ptr, type, member)
#define list_first(l, type, member) list_entry((l)->next, type, member)
#define list_for_each(it, l) for (struct list_node *it = (l)->next; it != (l); it = it->next)
#define list_for_each_safe(it, tmp, l) \
    for (struct list_node *it = (l)->next, *tmp = it->next; it != (l); it = tmp, tmp = it->next)
