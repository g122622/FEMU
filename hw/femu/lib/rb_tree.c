#include "qemu/osdep.h"
#include "rb_tree.h"

#include <assert.h>
#include "qemu/atomic.h"

#define RB_IS_RED(n)   ((n) && (n)->red)
#define RB_IS_BLACK(n) (!(n) || !(n)->red)

static inline FemuRbNode *rb_min(FemuRbNode *n)
{
    while (n && n->left) {
        n = n->left;
    }
    return n;
}

static void rb_rotate_left(FemuRbTree *t, FemuRbNode *x)
{
    FemuRbNode *y = x->right;

    assert(y);
    x->right = y->left;
    if (y->left) {
        y->left->parent = x;
    }

    y->parent = x->parent;
    if (!x->parent) {
        t->root = y;
    } else if (x == x->parent->left) {
        x->parent->left = y;
    } else {
        x->parent->right = y;
    }

    y->left = x;
    x->parent = y;
}

static void rb_rotate_right(FemuRbTree *t, FemuRbNode *x)
{
    FemuRbNode *y = x->left;

    assert(y);
    x->left = y->right;
    if (y->right) {
        y->right->parent = x;
    }

    y->parent = x->parent;
    if (!x->parent) {
        t->root = y;
    } else if (x == x->parent->right) {
        x->parent->right = y;
    } else {
        x->parent->left = y;
    }

    y->right = x;
    x->parent = y;
}

static void rb_insert_fixup(FemuRbTree *t, FemuRbNode *n)
{
    while (n->parent && n->parent->red) {
        FemuRbNode *p = n->parent;
        FemuRbNode *g = p->parent;

        assert(g);
        if (p == g->left) {
            FemuRbNode *u = g->right;

            if (RB_IS_RED(u)) {
                p->red = false;
                u->red = false;
                g->red = true;
                n = g;
                continue;
            }

            if (n == p->right) {
                n = p;
                rb_rotate_left(t, n);
                p = n->parent;
                g = p ? p->parent : NULL;
            }

            assert(p && g);
            p->red = false;
            g->red = true;
            rb_rotate_right(t, g);
        } else {
            FemuRbNode *u = g->left;

            if (RB_IS_RED(u)) {
                p->red = false;
                u->red = false;
                g->red = true;
                n = g;
                continue;
            }

            if (n == p->left) {
                n = p;
                rb_rotate_right(t, n);
                p = n->parent;
                g = p ? p->parent : NULL;
            }

            assert(p && g);
            p->red = false;
            g->red = true;
            rb_rotate_left(t, g);
        }
    }

    if (t->root) {
        t->root->red = false;
    }
}

static void rb_transplant(FemuRbTree *t, FemuRbNode *u, FemuRbNode *v)
{
    if (!u->parent) {
        t->root = v;
    } else if (u == u->parent->left) {
        u->parent->left = v;
    } else {
        u->parent->right = v;
    }

    if (v) {
        v->parent = u->parent;
    }
}

static void rb_delete_fixup(FemuRbTree *t, FemuRbNode *x, FemuRbNode *x_parent)
{
    while ((x != t->root) && RB_IS_BLACK(x)) {
        if (x_parent && x == x_parent->left) {
            FemuRbNode *w = x_parent->right;

            if (RB_IS_RED(w)) {
                w->red = false;
                x_parent->red = true;
                rb_rotate_left(t, x_parent);
                w = x_parent->right;
            }

            if (RB_IS_BLACK(w ? w->left : NULL) && RB_IS_BLACK(w ? w->right : NULL)) {
                if (w) {
                    w->red = true;
                }
                x = x_parent;
                x_parent = x ? x->parent : NULL;
            } else {
                if (RB_IS_BLACK(w ? w->right : NULL)) {
                    if (w && w->left) {
                        w->left->red = false;
                    }
                    if (w) {
                        w->red = true;
                        rb_rotate_right(t, w);
                    }
                    w = x_parent ? x_parent->right : NULL;
                }

                if (w) {
                    w->red = x_parent ? x_parent->red : false;
                }
                if (x_parent) {
                    x_parent->red = false;
                }
                if (w && w->right) {
                    w->right->red = false;
                }
                if (x_parent) {
                    rb_rotate_left(t, x_parent);
                }
                x = t->root;
                break;
            }
        } else {
            FemuRbNode *w = x_parent ? x_parent->left : NULL;

            if (RB_IS_RED(w)) {
                w->red = false;
                if (x_parent) {
                    x_parent->red = true;
                    rb_rotate_right(t, x_parent);
                }
                w = x_parent ? x_parent->left : NULL;
            }

            if (RB_IS_BLACK(w ? w->right : NULL) && RB_IS_BLACK(w ? w->left : NULL)) {
                if (w) {
                    w->red = true;
                }
                x = x_parent;
                x_parent = x ? x->parent : NULL;
            } else {
                if (RB_IS_BLACK(w ? w->left : NULL)) {
                    if (w && w->right) {
                        w->right->red = false;
                    }
                    if (w) {
                        w->red = true;
                        rb_rotate_left(t, w);
                    }
                    w = x_parent ? x_parent->left : NULL;
                }

                if (w) {
                    w->red = x_parent ? x_parent->red : false;
                }
                if (x_parent) {
                    x_parent->red = false;
                }
                if (w && w->left) {
                    w->left->red = false;
                }
                if (x_parent) {
                    rb_rotate_right(t, x_parent);
                }
                x = t->root;
                break;
            }
        }
    }

    if (x) {
        x->red = false;
    }
}

void femu_rb_tree_init(FemuRbTree *t)
{
    t->root = NULL;
    t->count = 0;
}

void femu_rb_node_init(FemuRbNode *n, uint64_t lpn, uint64_t hmb_off,
                       uint32_t len, uint8_t state)
{
    n->parent = NULL;
    n->left = NULL;
    n->right = NULL;
    n->red = true;

    n->lpn = lpn;
    n->hmb_off = hmb_off;
    n->len = len;
    n->state = state;

    qatomic_set(&n->refcnt, 0);
}

FemuRbNode *femu_rb_find(FemuRbTree *t, uint64_t lpn)
{
    FemuRbNode *cur = t->root;

    while (cur) {
        if (lpn < cur->lpn) {
            cur = cur->left;
        } else if (lpn > cur->lpn) {
            cur = cur->right;
        } else {
            return cur;
        }
    }

    return NULL;
}

FemuRbNode *femu_rb_find_ge(FemuRbTree *t, uint64_t lpn)
{
    FemuRbNode *cur = t->root;
    FemuRbNode *best = NULL;

    while (cur) {
        if (lpn <= cur->lpn) {
            best = cur;
            cur = cur->left;
        } else {
            cur = cur->right;
        }
    }

    return best;
}

bool femu_rb_insert(FemuRbTree *t, FemuRbNode *n)
{
    FemuRbNode *parent = NULL;
    FemuRbNode *cur = t->root;

    assert(n);
    assert(!n->parent && !n->left && !n->right);

    while (cur) {
        parent = cur;
        if (n->lpn < cur->lpn) {
            cur = cur->left;
        } else if (n->lpn > cur->lpn) {
            cur = cur->right;
        } else {
            return false;
        }
    }

    n->parent = parent;
    n->left = NULL;
    n->right = NULL;
    n->red = true;

    if (!parent) {
        t->root = n;
    } else if (n->lpn < parent->lpn) {
        parent->left = n;
    } else {
        parent->right = n;
    }

    rb_insert_fixup(t, n);
    t->count++;
    return true;
}

FemuRbNode *femu_rb_upsert(FemuRbTree *t, FemuRbNode *n, bool *inserted)
{
    FemuRbNode *old = femu_rb_find(t, n->lpn);

    if (old) {
        old->hmb_off = n->hmb_off;
        old->len = n->len;
        old->state = n->state;
        if (inserted) {
            *inserted = false;
        }
        return old;
    }

    if (inserted) {
        *inserted = femu_rb_insert(t, n);
    } else {
        (void)femu_rb_insert(t, n);
    }

    return n;
}

void femu_rb_remove(FemuRbTree *t, FemuRbNode *z)
{
    FemuRbNode *y = z;
    FemuRbNode *x;
    FemuRbNode *x_parent;
    bool y_red = y->red;

    assert(t);
    assert(z);

    if (!z->left) {
        x = z->right;
        x_parent = z->parent;
        rb_transplant(t, z, z->right);
    } else if (!z->right) {
        x = z->left;
        x_parent = z->parent;
        rb_transplant(t, z, z->left);
    } else {
        y = rb_min(z->right);
        y_red = y->red;
        x = y->right;

        if (y->parent == z) {
            x_parent = y;
        } else {
            x_parent = y->parent;
            rb_transplant(t, y, y->right);
            y->right = z->right;
            if (y->right) {
                y->right->parent = y;
            }
        }

        rb_transplant(t, z, y);
        y->left = z->left;
        if (y->left) {
            y->left->parent = y;
        }
        y->red = z->red;
    }

    if (!y_red) {
        rb_delete_fixup(t, x, x_parent);
    }

    z->parent = NULL;
    z->left = NULL;
    z->right = NULL;
    z->red = true;

    assert(t->count > 0);
    t->count--;
}

FemuRbNode *femu_rb_erase_by_lpn(FemuRbTree *t, uint64_t lpn)
{
    FemuRbNode *n = femu_rb_find(t, lpn);

    if (!n) {
        return NULL;
    }

    femu_rb_remove(t, n);
    return n;
}

FemuRbNode *femu_rb_first(FemuRbTree *t)
{
    return rb_min(t->root);
}

FemuRbNode *femu_rb_next(FemuRbNode *n)
{
    FemuRbNode *p;

    if (!n) {
        return NULL;
    }

    if (n->right) {
        return rb_min(n->right);
    }

    p = n->parent;
    while (p && n == p->right) {
        n = p;
        p = p->parent;
    }

    return p;
}

int32_t femu_rb_refcnt_read(FemuRbNode *n)
{
    return qatomic_read(&n->refcnt);
}

void femu_rb_refcnt_set(FemuRbNode *n, int32_t v)
{
    qatomic_set(&n->refcnt, v);
}

int32_t femu_rb_refcnt_inc(FemuRbNode *n)
{
    return qatomic_inc_fetch(&n->refcnt);
}

int32_t femu_rb_refcnt_dec(FemuRbNode *n)
{
    return qatomic_dec_fetch(&n->refcnt);
}
