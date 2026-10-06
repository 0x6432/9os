/* Host test for kernel/lib/rbtree.c: random inserts/erases checked against a reference array,
 * red-black invariants and an augmented subtree-maximum value. */
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "../../kernel/include/kernel/rbtree.h"
#include "../../kernel/lib/rbtree.c"

struct item { struct rb_node rb; long key, val, max; };
#define IT(n) rb_entry(n, struct item, rb)

static long compute(struct item *it) {
    long m = it->val;
    if (it->rb.left && IT(it->rb.left)->max > m) m = IT(it->rb.left)->max;
    if (it->rb.right && IT(it->rb.right)->max > m) m = IT(it->rb.right)->max;
    return m;
}
static void prop(struct rb_node *n, struct rb_node *stop) {
    for (; n != stop; n = n->parent) {
        long m = compute(IT(n));
        IT(n)->max = m;
    }
}
static void rot(struct rb_node *old, struct rb_node *new_) {
    IT(new_)->max = IT(old)->max;
    IT(old)->max = compute(IT(old));
}
static const struct rb_aug aug = { prop, rot };

static int fail(const char *m) { fprintf(stderr, "rbtest: FAIL: %s\n", m); exit(1); }

static int check(struct rb_node *n, struct rb_node *parent, long lo, long hi) {
    if (!n) return 1;
    if (n->parent != parent) fail("parent link");
    if (IT(n)->key < lo || IT(n)->key > hi) fail("order");
    if (n->red && ((n->left && n->left->red) || (n->right && n->right->red))) fail("red-red");
    if (IT(n)->max != compute(IT(n))) fail("augmented value");
    int l = check(n->left, n, lo, IT(n)->key), r = check(n->right, n, IT(n)->key, hi);
    if (l != r) fail("black height");
    return l + !n->red;
}

static void insert(struct rb_root *root, struct item *it) {
    struct rb_node **link = &root->node, *parent = nullptr;
    while (*link) {
        parent = *link;
        link = it->key < IT(parent)->key ? &parent->left : &parent->right;
    }
    rb_link(&it->rb, parent, link);
    rb_insert_color(root, &it->rb, &aug);
}

int main(void) {
    enum { N = 2000, ROUNDS = 200000 };
    static struct item items[N];
    static bool in[N];
    struct rb_root root = { nullptr };
    srand(12345);
    int count = 0;
    for (int r = 0; r < ROUNDS; r++) {
        int i = rand() % N;
        if (in[i]) { rb_erase(&root, &items[i].rb, &aug); in[i] = false; count--; }
        else {
            items[i].key = rand() % 5000; items[i].val = rand() % 100000;
            insert(&root, &items[i]); in[i] = true; count++;
        }
        if (r % 97 == 0 || r == ROUNDS - 1) {
            if (root.node && root.node->red) fail("red root");
            check(root.node, nullptr, -1, 1L << 40);
            long expect = -1;
            for (int k = 0; k < N; k++) if (in[k] && items[k].val > expect) expect = items[k].val;
            if ((root.node ? IT(root.node)->max : -1) != expect) fail("root max");
            int c = 0; long prev = -1;
            for (struct rb_node *n = rb_first(&root); n; n = rb_next(n), c++) {
                if (IT(n)->key < prev) fail("iteration order");
                prev = IT(n)->key;
            }
            if (c != count) fail("count");
            c = 0;
            for (struct rb_node *n = rb_last(&root); n; n = rb_prev(n)) c++;
            if (c != count) fail("reverse count");
        }
        if (r % 1000 == 0) {          /* change a value in place and propagate */
            for (int k = 0; k < N; k++) if (in[k]) { items[k].val = rand() % 100000; rb_propagate(&items[k].rb, &aug); break; }
        }
    }
    printf("rbtree host tests: %d rounds passed\n", ROUNDS);
    return 0;
}
