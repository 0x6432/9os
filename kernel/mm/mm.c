/*
 * User address spaces, VMM v2 (M26). See kernel/mm.h for the data structures.
 *
 * Locking: mm->lock (recursive, IRQs off, services TLB IPIs while spinning) covers the VMA
 * tree/list, the page tables of this mm, rss/locked_vm. Faults, user copies and the mm
 * syscalls run without the BKL. VMAs unlinked under the lock go to mm->dead and are released
 * (file reference dropped, freed) by the outermost mm_unlock, so ->release/iput never run
 * with the spinlock held. Page-cache pages are found by tmpfs' fault_page under the page
 * cache lock (mm -> page cache -> i_mmap -> LRU). Reclaim (fs/tmpfs.c) takes the page cache
 * lock, walks inode->i_mmap and unmaps the page from each mm with mm_trylock.
 *
 * Page references: every user PTE holds one reference (and one mapcount) on its page, the
 * page cache holds one on each cached page. A private page with refcount 1 is owned by the
 * PTE alone and may be written in place; anything else is copied on write (fork COW and
 * private file mappings).
 */
#include <kernel/cpu.h>
#include <kernel/mm.h>
#include <kernel/pmm.h>
#include <kernel/boot.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/process.h>
#include <kernel/signal.h>
#include <kernel/vfs.h>
#include <kernel/time.h>

static const struct lock_class mm_class = { "mm", LR_MM, true };
struct cow_stats cow_stats;
struct vm_stats vm_stats;
#define STAT(x) __atomic_fetch_add(&vm_stats.x, 1, __ATOMIC_RELAXED)
#define STATN(x, n) __atomic_fetch_add(&vm_stats.x, (n), __ATOMIC_RELAXED)

void pagecache_mark_dirty(struct page *pg);     /* fs/tmpfs.c */
void pagecache_mark_referenced(struct page *pg);
uint64_t tmpfs_reclaim(uint64_t want);

enum { FLT_OK = 0, FLT_SEGV, FLT_OOM };

/* ------------------------------------------------------------------ locking */

void mm_lock(struct mm *mm) {
    uint64_t f = arch_irq_save();
    int me = this_cpu()->id + 1;
    if (__atomic_load_n(&mm->lock_owner, __ATOMIC_RELAXED) == me) { mm->lock_depth++; arch_irq_restore(f); return; }
    spin_lock_ipi(&mm->lock);
    mm->lock_owner = me;
    mm->lock_depth = 1;
    mm->lock_flags = f;
}

bool mm_trylock(struct mm *mm) {
    uint64_t f = arch_irq_save();
    int me = this_cpu()->id + 1;
    if (__atomic_load_n(&mm->lock_owner, __ATOMIC_RELAXED) == me || !spin_trylock(&mm->lock)) {
        arch_irq_restore(f);
        return false;
    }
    mm->lock_owner = me;
    mm->lock_depth = 1;
    mm->lock_flags = f;
    return true;
}

static void vma_release(struct vma *v) {
    if (v->file) vfs_close(v->file);
    kfree(v);
}

void mm_unlock(struct mm *mm) {
    if (--mm->lock_depth > 0) return;
    struct list_node dead = LIST_INIT(dead);
    while (!list_empty(&mm->dead)) {
        struct list_node *n = mm->dead.next;
        list_del(n);
        list_add_tail(&dead, n);
    }
    uint64_t f = mm->lock_flags;
    __atomic_store_n(&mm->lock_owner, 0, __ATOMIC_RELAXED);
    spin_unlock(&mm->lock);
    arch_irq_restore(f);
    list_for_each_safe(it, tmp, &dead) vma_release(list_entry(it, struct vma, node));
}

/* ------------------------------------------------------------------ VMA tree */

#define VMA(n) rb_entry(n, struct vma, rb)

static vaddr_t vma_compute_max(struct vma *v) {
    vaddr_t m = v->gap;
    if (v->rb.left && VMA(v->rb.left)->max_gap > m) m = VMA(v->rb.left)->max_gap;
    if (v->rb.right && VMA(v->rb.right)->max_gap > m) m = VMA(v->rb.right)->max_gap;
    return m;
}
static void aug_propagate(struct rb_node *n, struct rb_node *stop) {
    for (; n != stop; n = n->parent) VMA(n)->max_gap = vma_compute_max(VMA(n));
}
static void aug_rotate(struct rb_node *old, struct rb_node *new_) {
    VMA(new_)->max_gap = VMA(old)->max_gap;
    VMA(old)->max_gap = vma_compute_max(VMA(old));
}
static const struct rb_aug vma_aug = { aug_propagate, aug_rotate };

static struct vma *vma_prev(struct mm *mm, struct vma *v) {
    return v->node.prev == &mm->vmas ? nullptr : list_entry(v->node.prev, struct vma, node);
}
static struct vma *vma_next(struct mm *mm, struct vma *v) {
    return v->node.next == &mm->vmas ? nullptr : list_entry(v->node.next, struct vma, node);
}
static void vma_set_gap(struct mm *mm, struct vma *v) {
    struct vma *p = vma_prev(mm, v);
    vaddr_t floor = p ? p->end : USER_MIN;
    v->gap = v->start > floor ? v->start - floor : 0;
    rb_propagate(&v->rb, &vma_aug);
}
/* after v's start/end changed */
static void vma_update_gaps(struct mm *mm, struct vma *v) {
    vma_set_gap(mm, v);
    struct vma *n = vma_next(mm, v);
    if (n) vma_set_gap(mm, n);
}

struct vma *vma_find(struct mm *mm, vaddr_t addr) {
    struct rb_node *n = mm->vma_tree.node;
    while (n) {
        struct vma *v = VMA(n);
        if (addr < v->start) n = n->left;
        else if (addr >= v->end) n = n->right;
        else return v;
    }
    return nullptr;
}

/* first VMA with end > addr */
static struct vma *vma_lower_bound(struct mm *mm, vaddr_t addr) {
    struct rb_node *n = mm->vma_tree.node;
    struct vma *best = nullptr;
    while (n) {
        struct vma *v = VMA(n);
        if (v->end > addr) { best = v; n = n->left; }
        else n = n->right;
    }
    return best;
}

static void vma_link(struct mm *mm, struct vma *v) {
    struct rb_node **link = &mm->vma_tree.node, *parent = nullptr;
    struct vma *prev = nullptr;
    while (*link) {
        parent = *link;
        if (v->start < VMA(parent)->start) link = &parent->left;
        else { prev = VMA(parent); link = &parent->right; }
    }
    if (prev) __list_add(&v->node, &prev->node, prev->node.next);
    else __list_add(&v->node, &mm->vmas, mm->vmas.next);
    vaddr_t floor = prev ? prev->end : USER_MIN;
    v->gap = v->start > floor ? v->start - floor : 0;
    v->max_gap = v->gap;
    rb_link(&v->rb, parent, link);
    rb_insert_color(&mm->vma_tree, &v->rb, &vma_aug);
    struct vma *n = vma_next(mm, v);
    if (n) vma_set_gap(mm, n);
    mm->nr_vmas++;
    if (v->file) {
        struct inode *ino = v->file->inode;
        uint64_t f = arch_irq_save();
        spin_lock_ipi(&ino->i_mmap_lock);
        list_add_tail(&ino->i_mmap, &v->fnode);
        spin_unlock(&ino->i_mmap_lock);
        arch_irq_restore(f);
    }
}

static void vma_unlink(struct mm *mm, struct vma *v) {
    struct vma *n = vma_next(mm, v);
    rb_erase(&mm->vma_tree, &v->rb, &vma_aug);
    list_del(&v->node);
    if (n) vma_set_gap(mm, n);
    mm->nr_vmas--;
    if (v->flags & VMA_LOCKED) mm->locked_vm -= v->end - v->start;
    if (v->file) {
        struct inode *ino = v->file->inode;
        uint64_t f = arch_irq_save();
        spin_lock_ipi(&ino->i_mmap_lock);
        list_del(&v->fnode);
        spin_unlock(&ino->i_mmap_lock);
        arch_irq_restore(f);
    }
    list_add_tail(&mm->dead, &v->node);
}

static struct vma *vma_alloc(struct mm *mm, vaddr_t s, vaddr_t e, unsigned prot, unsigned flags,
                             struct file *f, uint64_t pgoff) {
    struct vma *v = kzalloc(sizeof *v);
    if (!v) return nullptr;
    v->start = s; v->end = e; v->prot = prot; v->flags = flags; v->mm = mm;
    v->file = f ? file_get(f) : nullptr;
    v->pgoff = pgoff;
    list_init(&v->node);
    list_init(&v->fnode);
    if (flags & VMA_LOCKED) mm->locked_vm += e - s;
    return v;
}

static bool vma_mergeable(struct vma *a, struct vma *b) {
    if (a->end != b->start || a->prot != b->prot || a->flags != b->flags || a->file != b->file) return false;
    if (a->flags & (VMA_PHYS | VMA_STACK)) return false;
    return !a->file || a->pgoff + (a->end - a->start) / PAGE_SIZE == b->pgoff;
}

/* merge v with its neighbours where possible; returns the surviving VMA */
static struct vma *vma_merge(struct mm *mm, struct vma *v) {
    struct vma *p = vma_prev(mm, v);
    if (p && vma_mergeable(p, v)) {
        vaddr_t e = v->end;
        bool locked = v->flags & VMA_LOCKED;
        vma_unlink(mm, v);
        if (locked) mm->locked_vm += e - v->start;   /* p takes over the range */
        p->end = e;
        vma_update_gaps(mm, p);
        v = p;
    }
    struct vma *n = vma_next(mm, v);
    if (n && vma_mergeable(v, n)) {
        vaddr_t e = n->end;
        bool locked = n->flags & VMA_LOCKED;
        vma_unlink(mm, n);
        if (locked) mm->locked_vm += e - n->start;
        v->end = e;
        vma_update_gaps(mm, v);
    }
    return v;
}

/* Split so that a VMA boundary exists at addr. */
static int vma_split(struct mm *mm, vaddr_t addr) {
    struct vma *v = vma_find(mm, addr);
    if (!v || v->start == addr) return 0;
    struct vma *n = vma_alloc(mm, addr, v->end, v->prot, v->flags, v->file,
                              v->file ? v->pgoff + (addr - v->start) / PAGE_SIZE : 0);
    if (!n) return -ENOMEM;
    if (v->flags & VMA_LOCKED) mm->locked_vm -= v->end - addr;   /* counted again by vma_alloc */
    v->end = addr;
    vma_link(mm, n);
    return 0;
}

/* Top-down search for len bytes ending at or below hint. Guard gaps keep mappings away from
 * the bottom of stacks. Returns 0 if nothing fits. */
static vaddr_t gap_search_node(struct rb_node *rn, size_t len, vaddr_t hint) {
    if (!rn) return 0;
    struct vma *v = VMA(rn);
    if (v->max_gap < len) return 0;
    if (v->end < hint) {
        vaddr_t r = gap_search_node(rn->right, len, hint);
        if (r) return r;
    }
    vaddr_t lo = v->start - v->gap;
    vaddr_t hi = v->start;
    if (v->flags & VMA_STACK) hi = hi > lo + STACK_GUARD_GAP ? hi - STACK_GUARD_GAP : lo;
    if (hi > hint) hi = hint;
    if (hi > lo && hi - lo >= len) return hi - len;
    return gap_search_node(rn->left, len, hint);
}

static vaddr_t gap_search(struct mm *mm, size_t len, vaddr_t hint) {
    if (list_empty(&mm->vmas)) return hint >= USER_MIN + len ? hint - len : 0;
    struct vma *last = list_entry(mm->vmas.prev, struct vma, node);
    if (last->end <= hint && hint - last->end >= len) return hint - len;
    return gap_search_node(mm->vma_tree.node, len, hint);
}

static bool range_free_locked(struct mm *mm, vaddr_t s, vaddr_t e) {
    struct vma *v = vma_lower_bound(mm, s);
    return !v || v->start >= e;
}
static bool range_mapped_locked(struct mm *mm, vaddr_t s, vaddr_t e) {
    for (struct vma *v = vma_lower_bound(mm, s); s < e; v = vma_next(mm, v)) {
        if (!v || v->start > s) return false;
        s = v->end;
    }
    return true;
}

/* ------------------------------------------------------------------ PTEs */

static int pte_install(struct mm *mm, vaddr_t va, paddr_t pa, unsigned fl) {
    if (vmm_map(mm->pt, va, pa, fl)) return -ENOMEM;
    struct page *pg = phys_to_page(pa);
    __atomic_fetch_add(&pg->mapcount, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&mm->rss, 1, __ATOMIC_RELAXED);
    return 0;
}

/* Pages unmapped while TLB batching is active may still be cached in remote TLBs until the
 * batch flush, so they are only released after vmm_batch_end(). */
struct gather { struct page *pg[64]; int n; bool batch; };
static void gather_flush(struct gather *g) {
    if (!g->n) return;
    vmm_batch_end();
    for (int i = 0; i < g->n; i++) page_put(g->pg[i]);
    g->n = 0;
    vmm_batch_begin();
}
static void gather_begin(struct gather *g, bool batch) { g->n = 0; g->batch = batch; if (batch) vmm_batch_begin(); }
static void gather_end(struct gather *g) {
    if (!g->batch) return;
    vmm_batch_end();
    for (int i = 0; i < g->n; i++) page_put(g->pg[i]);
    g->n = 0;
}

static void pte_zap(struct mm *mm, struct vma *v, vaddr_t va, struct gather *g) {
    paddr_t pa = vmm_unmap(mm->pt, va);
    if (!pa || (v->flags & VMA_PHYS)) return;
    struct page *pg = phys_to_page(ALIGN_DOWN(pa, PAGE_SIZE));
    __atomic_fetch_sub(&pg->mapcount, 1, __ATOMIC_RELAXED);
    __atomic_fetch_sub(&mm->rss, 1, __ATOMIC_RELAXED);
    if (!g->batch) { page_put(pg); return; }
    if (g->n == (int)ARRAY_SIZE(g->pg)) gather_flush(g);
    g->pg[g->n++] = pg;
}

static void zap_range(struct mm *mm, struct vma *v, vaddr_t s, vaddr_t e, struct gather *g) {
    for (vaddr_t va = s; va < e; va += PAGE_SIZE) pte_zap(mm, v, va, g);
}

/* ------------------------------------------------------------------ faults */

static int anon_fault(struct mm *mm, struct vma *v, vaddr_t va) {
    paddr_t np = pmm_alloc_zeroed(0);
    if (!np) return FLT_OOM;
    if (pte_install(mm, va, np, v->prot | VM_USER)) { pmm_free_pages(np, 0); return FLT_OOM; }
    STAT(anon_faults);
    return FLT_OK;
}

/* Copy-on-write (or re-enable writes) for a present, read-only PTE. force: kernel write
 * ignoring the VMA protection (mm_write); keeps the PTE protection. */
static int cow_break(struct mm *mm, struct vma *v, vaddr_t va, bool force, paddr_t *out) {
    paddr_t pa; unsigned fl;
    if (!vmm_query(mm->pt, va, &pa, &fl)) return FLT_SEGV;
    pa = ALIGN_DOWN(pa, PAGE_SIZE);
    if (out) *out = pa;
    if (fl & VM_WRITE) return FLT_OK;
    if (v->flags & VMA_PHYS) return force ? FLT_OK : FLT_SEGV;
    if (!force && !(v->prot & VM_WRITE)) return FLT_SEGV;
    struct page *pg = phys_to_page(pa);
    unsigned nfl = force ? fl : v->prot | VM_USER;
    if (v->flags & VMA_SHARED) {
        if (page_uflag_test(pg, PGU_CACHE)) pagecache_mark_dirty(pg);
        if (!force) vmm_protect(mm->pt, va, nfl);
        cow_stats.reused++;
        return FLT_OK;
    }
    if (page_ref_read(pg) <= 1) {         /* sole owner: just re-enable writes */
        if (!force) vmm_protect(mm->pt, va, nfl);
        cow_stats.reused++;
        return FLT_OK;
    }
    paddr_t np = pmm_alloc_pages(0);
    if (!np) return FLT_OOM;
    memcpy(PHYS_TO_VIRT(np), PHYS_TO_VIRT(pa), PAGE_SIZE);
    vmm_unmap(mm->pt, va);                /* flushes every CPU before the old page can go */
    __atomic_fetch_sub(&pg->mapcount, 1, __ATOMIC_RELAXED);
    __atomic_fetch_sub(&mm->rss, 1, __ATOMIC_RELAXED);
    page_put(pg);
    if (pte_install(mm, va, np, nfl)) { pmm_free_pages(np, 0); return FLT_OOM; }
    cow_stats.copied++;
    STAT(cow_faults);
    if (out) *out = np;
    return FLT_OK;
}

static int file_fault(struct mm *mm, struct vma *v, vaddr_t va, bool write) {
    struct file *f = v->file;
    bool shared = v->flags & VMA_SHARED;
    uint64_t idx = v->pgoff + (va - v->start) / PAGE_SIZE;
    paddr_t pa;
    int e = f->fops && f->fops->fault_page ? f->fops->fault_page(f->inode, idx, shared, &pa) : -ENXIO;
    if (e == -ENXIO && !shared) { STAT(zero_eof_faults); return anon_fault(mm, v, va); }   /* past EOF */
    if (e == -ENOMEM) return FLT_OOM;
    if (e) return FLT_SEGV;
    struct page *pg = phys_to_page(pa);
    unsigned fl = v->prot | VM_USER;
    if (!shared) fl &= ~VM_WRITE;                     /* private: COW on the first write */
    else if (fl & VM_WRITE) pagecache_mark_dirty(pg); /* writable shared: assume written */
    if (pte_install(mm, va, pa, fl)) { page_put(pg); return FLT_OOM; }
    STAT(file_faults);
    if (write && !shared) return cow_break(mm, v, va, false, nullptr);
    return FLT_OK;
}

/* make va present (and writable for write); caller checked the VMA protection */
static int fault_page_locked(struct mm *mm, struct vma *v, vaddr_t va, bool write) {
    paddr_t pa; unsigned fl;
    if (vmm_query(mm->pt, va, &pa, &fl)) {
        if (!write || (fl & VM_WRITE)) return FLT_OK;
        return cow_break(mm, v, va, false, nullptr);
    }
    if (v->flags & VMA_PHYS) return FLT_SEGV;          /* device mappings are prefaulted */
    if (v->file) return file_fault(mm, v, va, write);
    return anon_fault(mm, v, va);
}

static bool vma_allows(struct vma *v, bool write, bool exec) {
    if (write && !(v->prot & VM_WRITE)) return false;
    if (exec && !(v->prot & VM_EXEC)) return false;
    return v->prot & (VM_READ | VM_WRITE | VM_EXEC);
}

/* ------------------------------------------------------------------ OOM */

static int oom_victim_pid;
static uint64_t oom_victim_ns;

struct oom_pick { struct process *best; int64_t rss; };
static void oom_consider(struct process *p, void *arg) {
    struct oom_pick *o = arg;
    if (p->pid <= 1 || p->state == P_ZOMBIE || !p->mm) return;
    int64_t r = __atomic_load_n(&p->mm->rss, __ATOMIC_RELAXED);
    if (r > o->rss) { o->best = p; o->rss = r; }
}

/* Kill the process with the largest resident set (never init) unless a recent victim is
 * still exiting. Takes the BKL for the process list; no spinlocks may be held. */
static void oom_kill(void) {
    bool took = !bkl_held();
    if (took) bkl_enter();
    struct process *prev = oom_victim_pid ? process_find(oom_victim_pid) : nullptr;
    if (prev && prev->state != P_ZOMBIE && time_ns() - oom_victim_ns < 2000000000ull) goto out;
    struct oom_pick o = { nullptr, -1 };
    process_list(oom_consider, &o);
    if (o.best) {
        uint64_t fp, tp;
        pmm_stats(&fp, &tp);
        printk("oom: out of memory (%lu of %lu pages free): killed pid %d (%s), rss %ld KiB\n",
               fp, tp, o.best->pid, o.best->name, (long)(o.rss * (PAGE_SIZE / 1024)));
        oom_victim_pid = o.best->pid;
        oom_victim_ns = time_ns();
        STAT(oom_kills);
        signal_send(o.best, SIGKILL);
    }
out:
    if (took) bkl_exit();
}

/* Called with no spinlocks held after an allocation failed under mm->lock. Returns true if
 * the operation should be retried. */
static bool oom_retry(int tries, bool from_fault) {
    STAT(oom_retries);
    if (mm_reclaim(64) && tries < 8) return true;
    pmm_drain_cpu_caches();
    if (tries < 2) return true;
    oom_kill();
    return from_fault;      /* faults return to user and retry (or die); copies fail */
}

bool mm_handle_fault(struct mm *mm, vaddr_t addr, bool write, bool exec) {
    for (int tries = 0;; tries++) {
        mm_lock(mm);
        struct vma *v = vma_find(mm, addr);
        int r = !v || !vma_allows(v, write, exec) ? FLT_SEGV : fault_page_locked(mm, v, ALIGN_DOWN(addr, PAGE_SIZE), write);
        mm_unlock(mm);
        if (r == FLT_OK) {
            if (current && current->proc && current->proc->mm == mm)
                __atomic_fetch_add(&current->proc->min_flt, 1, __ATOMIC_RELAXED);
            return true;
        }
        if (r == FLT_SEGV) return false;
        if (!oom_retry(tries, true)) return false;
        if (tries >= 2) return true;      /* OOM killer ran: let the fault happen again */
    }
}

/* ------------------------------------------------------------------ create/clone/destroy */

struct mm *mm_create(void) {
    struct mm *mm = kzalloc(sizeof *mm);
    if (!mm) return nullptr;
    mm->pt = vmm_new_user_pagetable();
    if (!mm->pt.root) { kfree(mm); return nullptr; }
    list_init(&mm->vmas);
    list_init(&mm->dead);
    spin_lock_init_class(&mm->lock, &mm_class);
    mm->refcount = 1;
    mm->mmap_hint = USER_MMAP_BASE;
    return mm;
}

void mm_put(struct mm *mm) {
    if (__atomic_sub_fetch(&mm->refcount, 1, __ATOMIC_ACQ_REL) > 0) return;
    mm_lock(mm);            /* a straggling lock-free fault/copy or reclaim may still be inside */
    struct gather g;
    gather_begin(&g, true);
    while (!list_empty(&mm->vmas)) {
        struct vma *v = list_first(&mm->vmas, struct vma, node);
        zap_range(mm, v, v->start, v->end, &g);
        vma_unlink(mm, v);
    }
    gather_end(&g);
    mm_unlock(mm);
    vmm_free_user_pagetable(mm->pt);
    kfree(mm);
}

/*
 * fork: private writable pages become copy-on-write (read-only in both address spaces,
 * page refcount raised); MAP_SHARED and device mappings are shared outright. mlock and
 * mlockall(MCL_FUTURE) are not inherited.
 */
struct mm *mm_clone(struct mm *src) {
    struct mm *mm = mm_create();
    if (!mm) return nullptr;
    mm_lock(src);
    mm_lock(mm);            /* visible to reclaim through i_mmap as soon as a file VMA is linked */
    mm->brk_start = src->brk_start; mm->brk = src->brk; mm->mmap_hint = src->mmap_hint;
    mm->sigtramp = src->sigtramp;
    vmm_batch_begin();
    list_for_each(it, &src->vmas) {
        struct vma *v = list_entry(it, struct vma, node);
        struct vma *n = vma_alloc(mm, v->start, v->end, v->prot, v->flags & ~VMA_LOCKED, v->file, v->pgoff);
        if (!n) goto fail;
        vma_link(mm, n);
        for (vaddr_t va = v->start; va < v->end; va += PAGE_SIZE) {
            paddr_t pa; unsigned fl;
            if (!vmm_query(src->pt, va, &pa, &fl)) continue;
            pa = ALIGN_DOWN(pa, PAGE_SIZE);
            if (v->flags & VMA_PHYS) {
                if (vmm_map(mm->pt, va, pa, fl | VM_WC)) goto fail;
                continue;
            }
            if (!(v->flags & VMA_SHARED) && (fl & VM_WRITE)) {
                fl &= ~VM_WRITE;
                vmm_protect(src->pt, va, fl);
            }
            page_ref_inc(phys_to_page(pa));
            if (pte_install(mm, va, pa, fl)) { page_put_pa(pa); goto fail; }
            cow_stats.shared++;
        }
    }
    vmm_batch_end();
    mm_unlock(mm);
    mm_unlock(src);
    return mm;
fail:
    vmm_batch_end();
    mm_unlock(mm);
    mm_unlock(src);
    mm_put(mm);
    return nullptr;
}

/* ------------------------------------------------------------------ map/unmap/protect */

static int mm_unmap_locked(struct mm *mm, vaddr_t addr, size_t len) {
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    vaddr_t end = ALIGN_UP(addr + len, PAGE_SIZE);
    if (end <= addr) return -EINVAL;
    if (vma_split(mm, addr) || vma_split(mm, end)) return -ENOMEM;
    struct gather g;
    gather_begin(&g, end - addr > 16 * PAGE_SIZE);
    for (struct vma *v = vma_lower_bound(mm, addr), *n; v && v->start < end; v = n) {
        n = vma_next(mm, v);
        zap_range(mm, v, v->start, v->end, &g);
        vma_unlink(mm, v);
    }
    gather_end(&g);
    return 0;
}

static int64_t mm_map_locked(struct mm *mm, vaddr_t addr, size_t len, unsigned prot, unsigned flags, bool fixed,
                             struct file *f, uint64_t pgoff) {
    len = ALIGN_UP(len, PAGE_SIZE);
    if (!len) return -EINVAL;
    if (fixed) {
        if (addr & (PAGE_SIZE - 1) || addr < USER_MIN || addr + len > USER_TOP || addr + len < addr) return -EINVAL;
        int r = mm_unmap_locked(mm, addr, len);
        if (r) return r;
    } else {
        vaddr_t cand = 0;
        if (addr && addr >= USER_MIN && addr + len <= USER_TOP && addr + len > addr && !(addr & (PAGE_SIZE - 1)) &&
            range_free_locked(mm, addr, addr + len))
            cand = addr;
        if (!cand) cand = gap_search(mm, len, mm->mmap_hint);
        if (!cand) cand = gap_search(mm, len, USER_TOP);
        if (!cand) return -ENOMEM;
        addr = cand;
    }
    if (!(flags & VMA_PHYS)) flags |= mm->def_flags;
    struct vma *v = vma_alloc(mm, addr, addr + len, prot, flags, f, pgoff);
    if (!v) return -ENOMEM;
    vma_link(mm, v);
    vma_merge(mm, v);
    return (int64_t)addr;
}

static int mm_protect_locked(struct mm *mm, vaddr_t addr, size_t len, unsigned prot) {
    vaddr_t end = ALIGN_UP(addr + len, PAGE_SIZE);
    if (!range_mapped_locked(mm, addr, end)) return -ENOMEM;
    if (vma_split(mm, addr) || vma_split(mm, end)) return -ENOMEM;
    for (struct vma *v = vma_lower_bound(mm, addr), *n; v && v->start < end; v = n) {
        n = vma_next(mm, v);
        v->prot = prot;
        for (vaddr_t va = v->start; va < v->end; va += PAGE_SIZE) {
            paddr_t pa;
            if (!vmm_query(mm->pt, va, &pa, nullptr)) continue;
            struct page *pg = phys_to_page(ALIGN_DOWN(pa, PAGE_SIZE));
            unsigned p = prot | VM_USER;
            if (v->flags & VMA_PHYS) p |= VM_WC;
            else if (!(v->flags & VMA_SHARED) && page_ref_read(pg) > 1) p &= ~VM_WRITE;   /* still COW */
            else if ((v->flags & VMA_SHARED) && (p & VM_WRITE) && page_uflag_test(pg, PGU_CACHE)) pagecache_mark_dirty(pg);
            vmm_protect(mm->pt, va, p);
        }
        n = vma_next(mm, vma_merge(mm, v));
    }
    return 0;
}

int mm_unmap(struct mm *mm, vaddr_t addr, size_t len) {
    mm_lock(mm);
    int r = mm_unmap_locked(mm, addr, len);
    mm_unlock(mm);
    return r;
}

int64_t mm_map_file(struct mm *mm, vaddr_t addr, size_t len, unsigned prot, unsigned flags, bool fixed,
                    struct file *f, uint64_t pgoff) {
    mm_lock(mm);
    int64_t r = mm_map_locked(mm, addr, len, prot, flags, fixed, f, pgoff);
    bool populate = r >= 0 && (mm->def_flags & VMA_LOCKED) && !(flags & VMA_PHYS);
    mm_unlock(mm);
    if (populate) mm_populate(mm, r, len, false);
    return r;
}

int64_t mm_map(struct mm *mm, vaddr_t addr, size_t len, unsigned prot, unsigned flags, bool fixed) {
    return mm_map_file(mm, addr, len, prot, flags, fixed, nullptr, 0);
}

int mm_protect(struct mm *mm, vaddr_t addr, size_t len, unsigned prot) {
    mm_lock(mm);
    int r = mm_protect_locked(mm, addr, len, prot);
    mm_unlock(mm);
    return r;
}

bool mm_range_mapped(struct mm *mm, vaddr_t addr, size_t len) {
    mm_lock(mm);
    bool r = range_mapped_locked(mm, addr, ALIGN_UP(addr + len, PAGE_SIZE));
    mm_unlock(mm);
    return r;
}

bool mm_range_free(struct mm *mm, vaddr_t addr, size_t len) {
    mm_lock(mm);
    bool r = range_free_locked(mm, addr, ALIGN_UP(addr + len, PAGE_SIZE));
    mm_unlock(mm);
    return r;
}

int mm_install_page(struct mm *mm, vaddr_t va, paddr_t pa, unsigned vmflags) {
    mm_lock(mm);
    int r = vmm_query(mm->pt, va, nullptr, nullptr) ? 1 : pte_install(mm, va, pa, vmflags);
    mm_unlock(mm);
    if (r) page_put_pa(pa);
    return r;
}

int64_t mm_brk(struct mm *mm, vaddr_t addr) {
    mm_lock(mm);
    vaddr_t cur = mm->brk;
    if (addr < mm->brk_start || addr >= USER_MMAP_BASE) goto out;
    vaddr_t old_end = ALIGN_UP(mm->brk, PAGE_SIZE), new_end = ALIGN_UP(addr, PAGE_SIZE);
    if (new_end > old_end) {
        if (!range_free_locked(mm, old_end, new_end)) goto out;
        if (mm_map_locked(mm, old_end, new_end - old_end, VM_READ | VM_WRITE, VMA_ANON | VMA_HEAP, true, nullptr, 0) < 0) goto out;
    } else if (new_end < old_end) {
        mm_unmap_locked(mm, new_end, old_end - new_end);
    }
    mm->brk = cur = addr;
out:
    mm_unlock(mm);
    return cur;
}

/* fault in [addr, addr+len): mode 0 read, 1 write, 2 write where the VMA is writable */
static int populate(struct mm *mm, vaddr_t addr, size_t len, int mode) {
    vaddr_t end = ALIGN_UP(addr + len, PAGE_SIZE);
    int tries = 0;
    for (vaddr_t va = ALIGN_DOWN(addr, PAGE_SIZE); va < end;) {
        mm_lock(mm);
        int r = FLT_OK, n = 0;
        for (; va < end && n < 64 && r == FLT_OK; n++) {
            struct vma *v = vma_find(mm, va);
            if (!v) { mm_unlock(mm); return -ENOMEM; }
            if ((v->flags & VMA_PHYS) || !(v->prot & (VM_READ | VM_WRITE | VM_EXEC))) { va = v->end; continue; }
            bool w = mode == 1 ? true : mode == 2 ? (v->prot & VM_WRITE) && !(v->flags & VMA_SHARED) : false;
            if (w && !(v->prot & VM_WRITE)) { r = FLT_SEGV; break; }
            r = fault_page_locked(mm, v, va, w);
            if (r == FLT_OK) { va += PAGE_SIZE; STAT(populated); }
        }
        mm_unlock(mm);
        if (r == FLT_SEGV) return -EFAULT;
        if (r == FLT_OOM && !oom_retry(tries++, false)) return -ENOMEM;
    }
    return 0;
}
int mm_populate(struct mm *mm, vaddr_t addr, size_t len, bool write) { return populate(mm, addr, len, write ? 1 : 0); }

/* ------------------------------------------------------------------ mremap/madvise/mlock/msync/mincore */

#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED 2
#define MREMAP_DONTUNMAP 4

static int64_t mm_remap_locked(struct mm *mm, vaddr_t old, size_t olen, size_t nlen, int flags, vaddr_t naddr) {
    struct vma *v = vma_find(mm, old);
    if (!v || old + olen > v->end) return -EFAULT;
    if (!(flags & (MREMAP_FIXED | MREMAP_DONTUNMAP))) {
        if (nlen <= olen) {
            if (nlen < olen) mm_unmap_locked(mm, old + nlen, olen - nlen);
            return old;
        }
        if (old + olen == v->end && !(v->flags & (VMA_STACK | VMA_PHYS)) && old + nlen <= USER_TOP &&
            old + nlen > old && range_free_locked(mm, v->end, old + nlen)) {
            if (v->flags & VMA_LOCKED) mm->locked_vm += old + nlen - v->end;
            v->end = old + nlen;
            vma_update_gaps(mm, v);
            return old;
        }
    }
    if (!(flags & MREMAP_MAYMOVE)) return -ENOMEM;
    if ((flags & MREMAP_DONTUNMAP) && ((v->flags & (VMA_SHARED | VMA_PHYS)) || v->file)) return -EINVAL;
    vaddr_t dest;
    if (flags & MREMAP_FIXED) {
        if (naddr & (PAGE_SIZE - 1) || naddr < USER_MIN || naddr + nlen > USER_TOP || naddr + nlen < naddr) return -EINVAL;
        if (naddr < old + olen && old < naddr + nlen) return -EINVAL;
        int r = mm_unmap_locked(mm, naddr, nlen);
        if (r) return r;
        dest = naddr;
    } else {
        dest = gap_search(mm, nlen, mm->mmap_hint);
        if (!dest) dest = gap_search(mm, nlen, USER_TOP);
        if (!dest) return -ENOMEM;
    }
    if (vma_split(mm, old) || vma_split(mm, old + olen)) return -ENOMEM;
    v = vma_find(mm, old);
    struct vma *n = vma_alloc(mm, dest, dest + nlen, v->prot, v->flags, v->file, v->pgoff);
    if (!n) return -ENOMEM;
    vma_link(mm, n);
    size_t move = MIN(olen, nlen);
    for (size_t off = 0; off < move; off += PAGE_SIZE) {
        paddr_t pa; unsigned fl;
        if (!vmm_query(mm->pt, old + off, &pa, &fl)) continue;
        vmm_unmap(mm->pt, old + off);
        if (vmm_map(mm->pt, dest + off, ALIGN_DOWN(pa, PAGE_SIZE), fl)) {
            /* out of page-table memory: drop the page (contents lost, refaults zero) */
            struct page *pg = phys_to_page(ALIGN_DOWN(pa, PAGE_SIZE));
            if (!(v->flags & VMA_PHYS)) {
                __atomic_fetch_sub(&pg->mapcount, 1, __ATOMIC_RELAXED);
                __atomic_fetch_sub(&mm->rss, 1, __ATOMIC_RELAXED);
                page_put(pg);
            }
        }
        STAT(mremap_moved);
    }
    if (flags & MREMAP_DONTUNMAP) return dest;
    struct gather g;
    gather_begin(&g, false);
    if (olen > move) zap_range(mm, v, old + move, old + olen, &g);
    vma_unlink(mm, v);
    vma_merge(mm, n);
    return dest;
}

int64_t mm_remap(struct mm *mm, vaddr_t old, size_t olen, size_t nlen, int flags, vaddr_t naddr) {
    if (old & (PAGE_SIZE - 1) || flags & ~(MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP)) return -EINVAL;
    if ((flags & (MREMAP_FIXED | MREMAP_DONTUNMAP)) && !(flags & MREMAP_MAYMOVE)) return -EINVAL;
    olen = ALIGN_UP(olen, PAGE_SIZE); nlen = ALIGN_UP(nlen, PAGE_SIZE);
    if (!nlen || !olen) return -EINVAL;
    if ((flags & MREMAP_DONTUNMAP) && olen != nlen) return -EINVAL;
    mm_lock(mm);
    int64_t r = mm_remap_locked(mm, old, olen, nlen, flags, naddr);
    bool locked = false;
    if (r >= 0) { struct vma *v = vma_find(mm, r); locked = v && (v->flags & VMA_LOCKED); }
    mm_unlock(mm);
    if (locked) populate(mm, r, nlen, 2);
    return r;
}

enum {
    MADV_NORMAL = 0, MADV_RANDOM, MADV_SEQUENTIAL, MADV_WILLNEED, MADV_DONTNEED, MADV_FREE = 8,
    MADV_REMOVE, MADV_DONTFORK, MADV_DOFORK, MADV_MERGEABLE, MADV_UNMERGEABLE, MADV_HUGEPAGE,
    MADV_NOHUGEPAGE, MADV_DONTDUMP, MADV_DODUMP, MADV_WIPEONFORK, MADV_KEEPONFORK, MADV_COLD,
    MADV_PAGEOUT, MADV_POPULATE_READ, MADV_POPULATE_WRITE,
};

int mm_advise(struct mm *mm, vaddr_t addr, size_t len, int advice) {
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    vaddr_t end = ALIGN_UP(addr + len, PAGE_SIZE);
    if (end < addr) return -EINVAL;
    if (end == addr) return 0;
    switch (advice) {
    case MADV_DONTNEED: case MADV_FREE: case MADV_REMOVE: {
        mm_lock(mm);
        bool mapped = range_mapped_locked(mm, addr, end);
        int r = 0;
        for (struct vma *v = vma_lower_bound(mm, addr); v && v->start < end; v = vma_next(mm, v))
            if (v->flags & (VMA_LOCKED | VMA_PHYS)) r = -EINVAL;
        if (!r) {
            struct gather g;
            gather_begin(&g, end - addr > 16 * PAGE_SIZE);
            for (struct vma *v = vma_lower_bound(mm, addr); v && v->start < end; v = vma_next(mm, v)) {
                vaddr_t s = MAX(v->start, addr), e = MIN(v->end, end);
                zap_range(mm, v, s, e, &g);
                STATN(madv_zapped, (e - s) / PAGE_SIZE);
            }
            gather_end(&g);
        }
        mm_unlock(mm);
        return r ? r : mapped ? 0 : -ENOMEM;
    }
    case MADV_WILLNEED: case MADV_POPULATE_READ: case MADV_POPULATE_WRITE: {
        if (!mm_range_mapped(mm, addr, end - addr)) return -ENOMEM;
        int r = populate(mm, addr, end - addr, advice == MADV_POPULATE_WRITE ? 1 : 0);
        return advice == MADV_WILLNEED ? 0 : r;
    }
    case MADV_NORMAL: case MADV_RANDOM: case MADV_SEQUENTIAL: case MADV_DONTFORK: case MADV_DOFORK:
    case MADV_MERGEABLE: case MADV_UNMERGEABLE: case MADV_HUGEPAGE: case MADV_NOHUGEPAGE:
    case MADV_DONTDUMP: case MADV_DODUMP: case MADV_WIPEONFORK: case MADV_KEEPONFORK:
    case MADV_COLD: case MADV_PAGEOUT:
        return mm_range_mapped(mm, addr, end - addr) ? 0 : -ENOMEM;
    default:
        return -EINVAL;
    }
}

static void set_locked(struct mm *mm, struct vma *v, bool lock) {
    if (v->flags & VMA_PHYS) return;
    if (lock && !(v->flags & VMA_LOCKED)) { v->flags |= VMA_LOCKED; mm->locked_vm += v->end - v->start; }
    if (!lock && (v->flags & VMA_LOCKED)) { v->flags &= ~VMA_LOCKED; mm->locked_vm -= v->end - v->start; }
}

int mm_mlock(struct mm *mm, vaddr_t addr, size_t len, bool lock, bool do_populate) {
    vaddr_t s = ALIGN_DOWN(addr, PAGE_SIZE), end = ALIGN_UP(addr + len, PAGE_SIZE);
    if (end < s) return -EINVAL;
    if (end == s) return 0;
    mm_lock(mm);
    int r = 0;
    if (!range_mapped_locked(mm, s, end)) r = -ENOMEM;
    else if (vma_split(mm, s) || vma_split(mm, end)) r = -EAGAIN;
    else
        for (struct vma *v = vma_lower_bound(mm, s), *n; v && v->start < end; v = n) {
            set_locked(mm, v, lock);
            n = vma_next(mm, vma_merge(mm, v));
        }
    mm_unlock(mm);
    if (!r && lock && do_populate && populate(mm, s, end - s, 2) == -ENOMEM) r = -EAGAIN;
    return r;
}

#define MCL_CURRENT 1
#define MCL_FUTURE 2
#define MCL_ONFAULT 4
int mm_mlockall(struct mm *mm, int flags) {
    if (!flags || flags & ~(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT) || flags == MCL_ONFAULT) return -EINVAL;
    mm_lock(mm);
    if (flags & MCL_FUTURE) mm->def_flags |= VMA_LOCKED;
    if (flags & MCL_CURRENT) list_for_each(it, &mm->vmas) set_locked(mm, list_entry(it, struct vma, node), true);
    mm_unlock(mm);
    if ((flags & MCL_CURRENT) && !(flags & MCL_ONFAULT)) {
        /* populate VMA by VMA (unmapped holes in between are not an error) */
        for (vaddr_t a = USER_MIN;;) {
            mm_lock(mm);
            struct vma *v = vma_lower_bound(mm, a);
            vaddr_t s = v ? MAX(v->start, a) : 0, e = v ? v->end : 0;
            bool skip = v && (v->flags & VMA_PHYS);
            mm_unlock(mm);
            if (!v) break;
            if (!skip && populate(mm, s, e - s, 2) == -ENOMEM) return -EAGAIN;
            a = e;
        }
    }
    return 0;
}

int mm_munlockall(struct mm *mm) {
    mm_lock(mm);
    mm->def_flags &= ~VMA_LOCKED;
    list_for_each(it, &mm->vmas) set_locked(mm, list_entry(it, struct vma, node), false);
    mm_unlock(mm);
    return 0;
}

#define MS_ASYNC 1
#define MS_INVALIDATE 2
#define MS_SYNC 4
/* tmpfs has no backing store to write to: msync only validates (shared mappings are coherent
 * with read/write because they map the page-cache pages themselves) */
int mm_msync(struct mm *mm, vaddr_t addr, size_t len, int flags) {
    if (addr & (PAGE_SIZE - 1) || flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC)) return -EINVAL;
    if ((flags & MS_ASYNC) && (flags & MS_SYNC)) return -EINVAL;
    vaddr_t end = ALIGN_UP(addr + len, PAGE_SIZE);
    if (end < addr) return -ENOMEM;
    if (end == addr) return 0;
    mm_lock(mm);
    int r = range_mapped_locked(mm, addr, end) ? 0 : -ENOMEM;
    if (!r && (flags & MS_INVALIDATE))
        for (struct vma *v = vma_lower_bound(mm, addr); v && v->start < end; v = vma_next(mm, v))
            if (v->flags & VMA_LOCKED) r = -EBUSY;
    mm_unlock(mm);
    return r;
}

int mm_mincore(struct mm *mm, vaddr_t addr, size_t len, uint8_t *vec) {
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    vaddr_t end = ALIGN_UP(addr + len, PAGE_SIZE);
    mm_lock(mm);
    int r = range_mapped_locked(mm, addr, end) ? 0 : -ENOMEM;
    for (vaddr_t va = addr; !r && va < end; va += PAGE_SIZE)
        vec[(va - addr) / PAGE_SIZE] = vmm_query(mm->pt, va, nullptr, nullptr);
    mm_unlock(mm);
    return r;
}

/* ------------------------------------------------------------------ reverse map / reclaim */

bool rmap_unmap_file_page(struct page *pg) {
    struct inode *ino = pg->mapping;
    uint64_t idx = pg->index;
    paddr_t want = page_to_phys(pg);
    bool ok = true;
    uint64_t f = arch_irq_save();
    spin_lock_ipi(&ino->i_mmap_lock);
    list_for_each(it, &ino->i_mmap) {
        struct vma *v = list_entry(it, struct vma, fnode);
        uint64_t npg = (v->end - v->start) / PAGE_SIZE;
        if (idx < v->pgoff || idx >= v->pgoff + npg) continue;
        if (v->flags & VMA_LOCKED) { ok = false; break; }
        if (!mm_trylock(v->mm)) { ok = false; break; }
        vaddr_t va = v->start + (idx - v->pgoff) * PAGE_SIZE;
        paddr_t pa;
        if (vmm_query(v->mm->pt, va, &pa, nullptr) && ALIGN_DOWN(pa, PAGE_SIZE) == want) {
            vmm_unmap(v->mm->pt, va);
            __atomic_fetch_sub(&pg->mapcount, 1, __ATOMIC_RELAXED);
            __atomic_fetch_sub(&v->mm->rss, 1, __ATOMIC_RELAXED);
            page_put(pg);                  /* the page cache still holds a reference */
            STAT(rmap_unmapped);
        }
        mm_unlock(v->mm);
    }
    spin_unlock(&ino->i_mmap_lock);
    arch_irq_restore(f);
    return ok;
}

uint64_t mm_reclaim(uint64_t want) {
    STAT(reclaim_runs);
    uint64_t n = tmpfs_reclaim(want);
    STATN(reclaim_freed, n);
    return n;
}

static void kswapd(void *arg) {
    for (;;) {
        sleep_ns(50000000ull);
        uint64_t fp, tp;
        pmm_stats(&fp, &tp);
        uint64_t low = tp / 64, high = tp / 32;
        if (fp >= low) continue;
        STAT(kswapd_wakeups);
        mm_reclaim(high - fp);
    }
}

void mm_pressure_init(void) { thread_create("kswapd", kswapd, nullptr); }

/* ------------------------------------------------------------------ user access */

/* Writes through the HHDM, faulting pages in regardless of VMA protection. */
static int mm_write_locked(struct mm *mm, vaddr_t dst, const void *src, size_t n, int *flt) {
    const uint8_t *s = src;
    while (n) {
        struct vma *v = vma_find(mm, dst);
        if (!v) return -EFAULT;
        vaddr_t va = ALIGN_DOWN(dst, PAGE_SIZE);
        int r = fault_page_locked(mm, v, va, false);
        paddr_t pa = 0;
        if (r == FLT_OK) r = cow_break(mm, v, va, true, &pa);
        if (r) { *flt = r; return r == FLT_OOM ? -ENOMEM : -EFAULT; }
        size_t off = dst & (PAGE_SIZE - 1), chunk = MIN(n, PAGE_SIZE - off);
        if (s) { memcpy((uint8_t *)PHYS_TO_VIRT(pa) + off, s, chunk); s += chunk; }
        else memset((uint8_t *)PHYS_TO_VIRT(pa) + off, 0, chunk);
        dst += chunk; n -= chunk;
    }
    return 0;
}

int mm_write(struct mm *mm, vaddr_t dst, const void *src, size_t n) {
    for (int tries = 0;; tries++) {
        int flt = 0;
        mm_lock(mm);
        int r = mm_write_locked(mm, dst, src, n, &flt);
        mm_unlock(mm);
        if (flt != FLT_OOM || !oom_retry(tries, false)) return r;
    }
}
int mm_zero(struct mm *mm, vaddr_t dst, size_t n) { return mm_write(mm, dst, nullptr, n); }

static int range_fault_locked(struct mm *mm, vaddr_t a, size_t n, bool write) {
    if (a + n < a || a + n > USER_TOP) return FLT_SEGV;
    vaddr_t end = a + n;
    struct vma *v = nullptr;
    for (vaddr_t p = ALIGN_DOWN(a, PAGE_SIZE); p < end; p += PAGE_SIZE) {
        if (!v || p >= v->end) {
            v = vma_find(mm, p);
            if (!v) return FLT_SEGV;
            if (write && !(v->prot & VM_WRITE)) return FLT_SEGV;
            if (!write && !(v->prot & (VM_READ | VM_WRITE))) return FLT_SEGV;
        }
        int r = fault_page_locked(mm, v, p, write);
        if (r) return r;
    }
    return FLT_OK;
}

bool user_range_ok(const void *uaddr, size_t n, bool write) {
    if (!current || !current->proc) return false;
    struct mm *mm = current->proc->mm;
    for (int tries = 0;; tries++) {
        mm_lock(mm);
        int r = range_fault_locked(mm, (vaddr_t)uaddr, n, write);
        mm_unlock(mm);
        if (r == FLT_OK) return true;
        if (r == FLT_SEGV || !oom_retry(tries, false)) return false;
    }
}

/*
 * User copies hold the mm lock (IRQs off) from the range check through the memcpy, so a
 * concurrent munmap/mprotect/reclaim cannot pull the pages out from under it and no fault
 * can occur during the copy. Chunked so IRQs are not held off for long on big transfers.
 */
#define COPY_CHUNK (64 * 1024)
static int user_copy(void *dst, const void *src, vaddr_t uaddr, size_t n, bool write) {
    if (!current || !current->proc) return -EFAULT;
    struct mm *mm = current->proc->mm;
    int tries = 0;
    while (n) {
        size_t c = MIN(n, (size_t)COPY_CHUNK);
        mm_lock(mm);
        int r = range_fault_locked(mm, uaddr, c, write);
        if (r == FLT_OK) memcpy(dst, src, c);
        mm_unlock(mm);
        if (r == FLT_OOM && oom_retry(tries++, false)) continue;
        if (r) return -EFAULT;
        dst = (uint8_t *)dst + c; src = (const uint8_t *)src + c; uaddr += c; n -= c;
    }
    return 0;
}

int copy_from_user(void *dst, const void *usrc, size_t n) {
    return n ? user_copy(dst, usrc, (vaddr_t)usrc, n, false) : 0;
}

int copy_to_user(void *udst, const void *src, size_t n) {
    return n ? user_copy(udst, src, (vaddr_t)udst, n, true) : 0;
}

int64_t strncpy_from_user(char *dst, const char *usrc, size_t max) {
    if (!current || !current->proc) return -EFAULT;
    struct mm *mm = current->proc->mm;
    size_t i = 0;
    int tries = 0;
    while (i < max) {
        /* one page at a time under the lock */
        size_t lim = MIN(max, i + (PAGE_SIZE - ((vaddr_t)(usrc + i) & (PAGE_SIZE - 1))));
        mm_lock(mm);
        int r = range_fault_locked(mm, (vaddr_t)(usrc + i), 1, false);
        if (r) {
            mm_unlock(mm);
            if (r == FLT_OOM && oom_retry(tries++, false)) continue;
            return -EFAULT;
        }
        for (; i < lim; i++) {
            dst[i] = usrc[i];
            if (!dst[i]) { mm_unlock(mm); return (int64_t)i; }
        }
        mm_unlock(mm);
    }
    return -ENAMETOOLONG;
}
