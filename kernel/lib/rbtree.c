/* Red-black tree (CLRS insert/delete) with augmentation callbacks. See kernel/rbtree.h. */
#include <kernel/rbtree.h>

static void change_child(struct rb_root *root, struct rb_node *parent, struct rb_node *old, struct rb_node *new_) {
    if (!parent) root->node = new_;
    else if (parent->left == old) parent->left = new_;
    else parent->right = new_;
}

/* x's right child y takes x's place */
static void rotate_left(struct rb_root *root, struct rb_node *x, const struct rb_aug *aug) {
    struct rb_node *y = x->right;
    x->right = y->left;
    if (y->left) y->left->parent = x;
    y->parent = x->parent;
    change_child(root, x->parent, x, y);
    y->left = x;
    x->parent = y;
    if (aug) aug->rotate(x, y);
}

static void rotate_right(struct rb_root *root, struct rb_node *x, const struct rb_aug *aug) {
    struct rb_node *y = x->left;
    x->left = y->right;
    if (y->right) y->right->parent = x;
    y->parent = x->parent;
    change_child(root, x->parent, x, y);
    y->right = x;
    x->parent = y;
    if (aug) aug->rotate(x, y);
}

void rb_insert_color(struct rb_root *root, struct rb_node *n, const struct rb_aug *aug) {
    if (aug) aug->propagate(n, nullptr);
    while (n->parent && n->parent->red) {
        struct rb_node *p = n->parent, *g = p->parent;
        if (p == g->left) {
            struct rb_node *u = g->right;
            if (u && u->red) { p->red = u->red = false; g->red = true; n = g; continue; }
            if (n == p->right) { rotate_left(root, p, aug); n = p; p = n->parent; }
            p->red = false; g->red = true;
            rotate_right(root, g, aug);
        } else {
            struct rb_node *u = g->left;
            if (u && u->red) { p->red = u->red = false; g->red = true; n = g; continue; }
            if (n == p->left) { rotate_right(root, p, aug); n = p; p = n->parent; }
            p->red = false; g->red = true;
            rotate_left(root, g, aug);
        }
    }
    root->node->red = false;
}

static void erase_fixup(struct rb_root *root, struct rb_node *x, struct rb_node *xp, const struct rb_aug *aug) {
    while (x != root->node && (!x || !x->red)) {
        if (x == xp->left) {
            struct rb_node *w = xp->right;
            if (w->red) { w->red = false; xp->red = true; rotate_left(root, xp, aug); w = xp->right; }
            if ((!w->left || !w->left->red) && (!w->right || !w->right->red)) {
                w->red = true; x = xp; xp = x->parent;
            } else {
                if (!w->right || !w->right->red) { w->left->red = false; w->red = true; rotate_right(root, w, aug); w = xp->right; }
                w->red = xp->red; xp->red = false;
                if (w->right) w->right->red = false;
                rotate_left(root, xp, aug);
                x = root->node; xp = nullptr;
            }
        } else {
            struct rb_node *w = xp->left;
            if (w->red) { w->red = false; xp->red = true; rotate_right(root, xp, aug); w = xp->left; }
            if ((!w->left || !w->left->red) && (!w->right || !w->right->red)) {
                w->red = true; x = xp; xp = x->parent;
            } else {
                if (!w->left || !w->left->red) { w->right->red = false; w->red = true; rotate_left(root, w, aug); w = xp->left; }
                w->red = xp->red; xp->red = false;
                if (w->left) w->left->red = false;
                rotate_right(root, xp, aug);
                x = root->node; xp = nullptr;
            }
        }
    }
    if (x) x->red = false;
}

void rb_erase(struct rb_root *root, struct rb_node *z, const struct rb_aug *aug) {
    struct rb_node *x, *xp, *fix;          /* fix: lowest node whose subtree changed */
    bool removed_red;
    if (!z->left || !z->right) {
        x = z->left ? z->left : z->right;
        xp = z->parent;
        removed_red = z->red;
        if (x) x->parent = xp;
        change_child(root, xp, z, x);
        fix = xp;
    } else {
        struct rb_node *y = z->right;
        while (y->left) y = y->left;       /* successor */
        removed_red = y->red;
        x = y->right;
        if (y->parent == z) {
            xp = y;
        } else {
            xp = y->parent;
            xp->left = x;
            if (x) x->parent = xp;
            y->right = z->right;
            y->right->parent = y;
        }
        y->left = z->left;
        y->left->parent = y;
        y->parent = z->parent;
        y->red = z->red;
        change_child(root, z->parent, z, y);
        fix = xp;
    }
    if (aug && fix) aug->propagate(fix, nullptr);
    if (!removed_red) erase_fixup(root, x, xp, aug);
}

struct rb_node *rb_first(const struct rb_root *root) {
    struct rb_node *n = root->node;
    if (n) while (n->left) n = n->left;
    return n;
}
struct rb_node *rb_last(const struct rb_root *root) {
    struct rb_node *n = root->node;
    if (n) while (n->right) n = n->right;
    return n;
}
struct rb_node *rb_next(const struct rb_node *n) {
    if (n->right) { n = n->right; while (n->left) n = n->left; return (struct rb_node *)n; }
    while (n->parent && n == n->parent->right) n = n->parent;
    return n->parent;
}
struct rb_node *rb_prev(const struct rb_node *n) {
    if (n->left) { n = n->left; while (n->right) n = n->right; return (struct rb_node *)n; }
    while (n->parent && n == n->parent->left) n = n->parent;
    return n->parent;
}
