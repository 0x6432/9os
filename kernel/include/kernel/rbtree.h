#pragma once
/*
 * Intrusive red-black tree with optional augmentation (M26, used for the VMA tree).
 * The caller links a new node with rb_link() at the leaf position found by its own search,
 * then calls rb_insert_color(). If the tree is augmented, aug->propagate(node, stop) must
 * recompute the augmented value from node up to (excluding) stop, and aug->rotate(old, new)
 * is called after a rotation where new took old's place (new inherits old's subtree value,
 * old must be recomputed). Callers update augmented values after changing a node's key data
 * with rb_propagate(node).
 */
#include <kernel/types.h>

struct rb_node { struct rb_node *parent, *left, *right; bool red; };
struct rb_root { struct rb_node *node; };

struct rb_aug {
    void (*propagate)(struct rb_node *n, struct rb_node *stop);
    void (*rotate)(struct rb_node *old, struct rb_node *new_);
};

static inline void rb_link(struct rb_node *n, struct rb_node *parent, struct rb_node **link) {
    n->parent = parent; n->left = n->right = nullptr; n->red = true;
    *link = n;
}
void rb_insert_color(struct rb_root *root, struct rb_node *n, const struct rb_aug *aug);
void rb_erase(struct rb_root *root, struct rb_node *n, const struct rb_aug *aug);
static inline void rb_propagate(struct rb_node *n, const struct rb_aug *aug) { if (aug) aug->propagate(n, nullptr); }
struct rb_node *rb_first(const struct rb_root *root);
struct rb_node *rb_last(const struct rb_root *root);
struct rb_node *rb_next(const struct rb_node *n);
struct rb_node *rb_prev(const struct rb_node *n);
#define rb_entry(ptr, type, member) container_of(ptr, type, member)
#define rb_entry_safe(ptr, type, member) ({ struct rb_node *__p = (ptr); __p ? rb_entry(__p, type, member) : nullptr; })
