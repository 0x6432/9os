#include <kernel/cpu.h>
#include <kernel/mm.h>
#include <kernel/pmm.h>
#include <kernel/boot.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/process.h>

static const struct lock_class mm_class = { "mm", LR_MM, true };

static struct vma *vma_new(vaddr_t s, vaddr_t e, unsigned prot, unsigned flags) {
    struct vma *v = kzalloc(sizeof *v);
    if (!v) return nullptr;
    v->start = s; v->end = e; v->prot = prot; v->flags = flags;
    list_init(&v->node);
    return v;
}

static void vma_insert(struct mm *mm, struct vma *v) {
    list_for_each(it, &mm->vmas) {
        struct vma *o = list_entry(it, struct vma, node);
        if (o->start > v->start) { __list_add(&v->node, it->prev, it); return; }
    }
    list_add_tail(&mm->vmas, &v->node);
}

void mm_lock(struct mm *mm) {
    uint64_t f = arch_irq_save();
    int me = this_cpu()->id + 1;
    if (__atomic_load_n(&mm->lock_owner, __ATOMIC_RELAXED) == me) { mm->lock_depth++; arch_irq_restore(f); return; }
    spin_lock_ipi(&mm->lock);
    mm->lock_owner = me;
    mm->lock_depth = 1;
    mm->lock_flags = f;
}

void mm_unlock(struct mm *mm) {
    if (--mm->lock_depth > 0) return;
    uint64_t f = mm->lock_flags;
    __atomic_store_n(&mm->lock_owner, 0, __ATOMIC_RELAXED);
    spin_unlock(&mm->lock);
    arch_irq_restore(f);
}

struct mm *mm_create(void) {
    struct mm *mm = kzalloc(sizeof *mm);
    if (!mm) return nullptr;
    mm->pt = vmm_new_user_pagetable();
    if (!mm->pt.root) { kfree(mm); return nullptr; }
    list_init(&mm->vmas);
    spin_lock_init_class(&mm->lock, &mm_class);
    mm->refcount = 1;
    mm->mmap_hint = USER_MMAP_BASE;
    return mm;
}

struct cow_stats cow_stats;

static void free_pages_in(struct mm *mm, vaddr_t s, vaddr_t e, unsigned vflags) {
    for (vaddr_t va = s; va < e; va += PAGE_SIZE) {
        paddr_t pa = vmm_unmap(mm->pt, va);
        if (pa && !(vflags & VMA_PHYS)) {
            struct page *pg = phys_to_page(pa);
            page_put(pg);
        }
    }
}

void mm_put(struct mm *mm) {
    if (__atomic_sub_fetch(&mm->refcount, 1, __ATOMIC_ACQ_REL) > 0) return;
    mm_lock(mm);            /* a straggling lock-free fault/copy may still be inside */
    vmm_batch_begin();
    list_for_each_safe(it, tmp, &mm->vmas) {
        struct vma *v = list_entry(it, struct vma, node);
        free_pages_in(mm, v->start, v->end, v->flags);
        list_del(&v->node);
        kfree(v);
    }
    vmm_batch_end();
    mm_unlock(mm);
    vmm_free_user_pagetable(mm->pt);
    kfree(mm);
}

/*
 * fork: private writable pages become copy-on-write (read-only in both address spaces,
 * page refcount raised); MAP_SHARED and device mappings are shared outright.
 */
struct mm *mm_clone(struct mm *src) {
    struct mm *mm = mm_create();
    if (!mm) return nullptr;
    mm->brk_start = src->brk_start; mm->brk = src->brk; mm->mmap_hint = src->mmap_hint;
    mm->sigtramp = src->sigtramp;
    mm_lock(src);
    vmm_batch_begin();
    list_for_each(it, &src->vmas) {
        struct vma *v = list_entry(it, struct vma, node);
        struct vma *n = vma_new(v->start, v->end, v->prot, v->flags);
        if (!n) goto fail;
        list_add_tail(&mm->vmas, &n->node);
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
            if (vmm_map(mm->pt, va, pa, fl)) goto fail;
            page_ref_inc(phys_to_page(pa));
            cow_stats.shared++;
        }
    }
    vmm_batch_end();
    mm_unlock(src);
    return mm;
fail:
    vmm_batch_end();
    mm_unlock(src);
    mm_put(mm);
    return nullptr;
}

/* Kernel writes that ignore VMA protection (mm_write): never scribble on a shared COW page. */
static paddr_t cow_break(struct mm *mm, struct vma *v, vaddr_t va);
static paddr_t cow_break_any(struct mm *mm, struct vma *v, vaddr_t va) {
    if (v->prot & VM_WRITE) return cow_break(mm, v, va);
    paddr_t pa; unsigned fl;
    if (!vmm_query(mm->pt, va, &pa, &fl)) return 0;
    pa = ALIGN_DOWN(pa, PAGE_SIZE);
    struct page *pg = phys_to_page(pa);
    if ((v->flags & (VMA_SHARED | VMA_PHYS)) || page_ref_read(pg) <= 1) return pa;
    paddr_t np = pmm_alloc_pages(0);
    if (!np) return 0;
    memcpy(PHYS_TO_VIRT(np), PHYS_TO_VIRT(pa), PAGE_SIZE);
    vmm_unmap(mm->pt, va);
    if (vmm_map(mm->pt, va, np, fl)) { pmm_free_pages(np, 0); return 0; }
    page_put(pg);
    cow_stats.copied++;
    return np;
}

/* Make a present page writable, copying it first if another address space shares it. */
static paddr_t cow_break(struct mm *mm, struct vma *v, vaddr_t va) {
    paddr_t pa; unsigned fl;
    if (!vmm_query(mm->pt, va, &pa, &fl)) return 0;
    pa = ALIGN_DOWN(pa, PAGE_SIZE);
    if (fl & VM_WRITE) return pa;
    if (!(v->prot & VM_WRITE) || (v->flags & VMA_PHYS)) return 0;
    struct page *pg = phys_to_page(pa);
    if (page_ref_read(pg) <= 1 || (v->flags & VMA_SHARED)) {        /* sole owner: just re-enable writes */
        vmm_protect(mm->pt, va, v->prot | VM_USER);
        cow_stats.reused++;
        return pa;
    }
    paddr_t np = pmm_alloc_pages(0);
    if (!np) return 0;
    memcpy(PHYS_TO_VIRT(np), PHYS_TO_VIRT(pa), PAGE_SIZE);
    vmm_unmap(mm->pt, va);
    if (vmm_map(mm->pt, va, np, v->prot | VM_USER)) { pmm_free_pages(np, 0); return 0; }
    page_put(pg);
    cow_stats.copied++;
    return np;
}

struct vma *vma_find(struct mm *mm, vaddr_t addr) {
    list_for_each(it, &mm->vmas) {
        struct vma *v = list_entry(it, struct vma, node);
        if (addr >= v->start && addr < v->end) return v;
        if (v->start > addr) break;
    }
    return nullptr;
}

/* Split so that a VMA boundary exists at addr. */
static int vma_split(struct mm *mm, vaddr_t addr) {
    struct vma *v = vma_find(mm, addr);
    if (!v || v->start == addr) return 0;
    struct vma *n = vma_new(addr, v->end, v->prot, v->flags);
    if (!n) return -ENOMEM;
    v->end = addr;
    __list_add(&n->node, &v->node, v->node.next);
    return 0;
}

static int mm_unmap_locked(struct mm *mm, vaddr_t addr, size_t len) {
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    vaddr_t end = ALIGN_UP(addr + len, PAGE_SIZE);
    if (vma_split(mm, addr) || vma_split(mm, end)) return -ENOMEM;
    bool batch = end - addr > 16 * PAGE_SIZE;
    if (batch) vmm_batch_begin();
    list_for_each_safe(it, tmp, &mm->vmas) {
        struct vma *v = list_entry(it, struct vma, node);
        if (v->start >= addr && v->end <= end) {
            free_pages_in(mm, v->start, v->end, v->flags);
            list_del(&v->node);
            kfree(v);
        }
    }
    if (batch) vmm_batch_end();
    return 0;
}

static bool range_free(struct mm *mm, vaddr_t s, vaddr_t e) {
    list_for_each(it, &mm->vmas) {
        struct vma *v = list_entry(it, struct vma, node);
        if (v->start < e && v->end > s) return false;
    }
    return true;
}

static int64_t mm_map_locked(struct mm *mm, vaddr_t addr, size_t len, unsigned prot, unsigned flags, bool fixed) {
    len = ALIGN_UP(len, PAGE_SIZE);
    if (!len) return -EINVAL;
    if (fixed) {
        if (addr & (PAGE_SIZE - 1) || addr < USER_MIN || addr + len > USER_TOP) return -EINVAL;
        int r = mm_unmap(mm, addr, len);
        if (r) return r;
    } else {
        /* top-down search below the hint */
        vaddr_t cand = 0;
        if (addr && addr >= USER_MIN && addr + len <= USER_TOP && !(addr & (PAGE_SIZE - 1)) &&
            range_free(mm, addr, addr + len))
            cand = addr;
        for (vaddr_t top = mm->mmap_hint; !cand && top > USER_MIN + len; ) {
            vaddr_t s = top - len;
            bool ok = true;
            list_for_each(it, &mm->vmas) {
                struct vma *v = list_entry(it, struct vma, node);
                if (v->start < top && v->end > s) { top = v->start; ok = false; break; }
            }
            if (ok) cand = s;
        }
        if (!cand) return -ENOMEM;
        addr = cand;
    }
    struct vma *v = vma_new(addr, addr + len, prot, flags);
    if (!v) return -ENOMEM;
    vma_insert(mm, v);
    return (int64_t)addr;
}

static int mm_protect_locked(struct mm *mm, vaddr_t addr, size_t len, unsigned prot) {
    vaddr_t end = ALIGN_UP(addr + len, PAGE_SIZE);
    if (vma_split(mm, addr) || vma_split(mm, end)) return -ENOMEM;
    for (vaddr_t a = addr; a < end; a += PAGE_SIZE)
        if (!vma_find(mm, a)) return -ENOMEM;
    list_for_each(it, &mm->vmas) {
        struct vma *v = list_entry(it, struct vma, node);
        if (v->start >= addr && v->end <= end) {
            v->prot = prot;
            for (vaddr_t va = v->start; va < v->end; va += PAGE_SIZE) {
                paddr_t pa;
                if (!vmm_query(mm->pt, va, &pa, nullptr)) continue;
                unsigned p = prot | VM_USER;
                if (!(v->flags & (VMA_SHARED | VMA_PHYS)) && page_ref_read(phys_to_page(ALIGN_DOWN(pa, PAGE_SIZE))) > 1)
                    p &= ~VM_WRITE;            /* still copy-on-write */
                if (v->flags & VMA_PHYS) p |= VM_WC;
                vmm_protect(mm->pt, va, p);
            }
        }
    }
    return 0;
}

static paddr_t fault_in(struct mm *mm, struct vma *v, vaddr_t va) {
    paddr_t pa;
    if (vmm_query(mm->pt, va, &pa, nullptr)) return ALIGN_DOWN(pa, PAGE_SIZE);
    pa = pmm_alloc_zeroed(0);
    if (!pa) return 0;
    if (vmm_map(mm->pt, va, pa, v->prot | VM_USER)) { pmm_free_pages(pa, 0); return 0; }
    return pa;
}

static bool mm_handle_fault_locked(struct mm *mm, vaddr_t addr, bool write, bool exec) {
    struct vma *v = vma_find(mm, addr);
    if (!v) return false;
    if (write && !(v->prot & VM_WRITE)) return false;
    if (exec && !(v->prot & VM_EXEC)) return false;
    if (!(v->prot & (VM_READ | VM_WRITE | VM_EXEC))) return false;
    if (vmm_query(mm->pt, addr, nullptr, nullptr))                  /* present: protection fault */
        return write && cow_break(mm, v, ALIGN_DOWN(addr, PAGE_SIZE)) != 0;
    if (v->flags & VMA_PHYS) return false;                         /* device mappings are prefaulted */
    return fault_in(mm, v, ALIGN_DOWN(addr, PAGE_SIZE)) != 0;
}

/* Writes through the HHDM, faulting pages in regardless of VMA protection. */
static int mm_write_locked(struct mm *mm, vaddr_t dst, const void *src, size_t n) {
    const uint8_t *s = src;
    while (n) {
        struct vma *v = vma_find(mm, dst);
        if (!v) return -EFAULT;
        paddr_t pa = fault_in(mm, v, ALIGN_DOWN(dst, PAGE_SIZE));
        if (pa) { paddr_t w = cow_break_any(mm, v, ALIGN_DOWN(dst, PAGE_SIZE)); if (w) pa = w; }
        if (!pa) return -ENOMEM;
        size_t off = dst & (PAGE_SIZE - 1), chunk = MIN(n, PAGE_SIZE - off);
        if (s) { memcpy((uint8_t *)PHYS_TO_VIRT(pa) + off, s, chunk); s += chunk; }
        else memset((uint8_t *)PHYS_TO_VIRT(pa) + off, 0, chunk);
        dst += chunk; n -= chunk;
    }
    return 0;
}
int mm_zero(struct mm *mm, vaddr_t dst, size_t n) { return mm_write(mm, dst, nullptr, n); }

/* ---- current-process user access ---- */

static bool user_range_ok_locked(struct mm *mm, const void *uaddr, size_t n, bool write) {
    vaddr_t a = (vaddr_t)uaddr;
    if (a + n < a || a + n > USER_TOP) return false;
    vaddr_t end = a + n;
    for (vaddr_t p = ALIGN_DOWN(a, PAGE_SIZE); p < end; p += PAGE_SIZE) {
        struct vma *v = vma_find(mm, p);
        if (!v) return false;
        if (write && !(v->prot & VM_WRITE)) return false;
        if (!write && !(v->prot & (VM_READ | VM_WRITE))) return false;
        if (!fault_in(mm, v, p)) return false;
        if (write && !cow_break(mm, v, p)) return false;
    }
    return true;
}

bool user_range_ok(const void *uaddr, size_t n, bool write) {
    if (!current || !current->proc) return false;
    struct mm *mm = current->proc->mm;
    mm_lock(mm);
    bool ok = user_range_ok_locked(mm, uaddr, n, write);
    mm_unlock(mm);
    return ok;
}

/*
 * User copies hold the mm lock (IRQs off) from the range check through the memcpy, so a
 * concurrent munmap/mprotect by another thread cannot pull the pages out from under it and
 * no fault can occur during the copy. No BKL needed. Copies are chunked so IRQs are not held
 * off for long stretches on big transfers.
 */
#define COPY_CHUNK (64 * 1024)
static int user_copy(void *dst, const void *src, vaddr_t uaddr, size_t n, bool write) {
    if (!current || !current->proc) return -EFAULT;
    struct mm *mm = current->proc->mm;
    while (n) {
        size_t c = MIN(n, (size_t)COPY_CHUNK);
        mm_lock(mm);
        bool ok = user_range_ok_locked(mm, (const void *)uaddr, c, write);
        if (ok) memcpy(dst, src, c);
        mm_unlock(mm);
        if (!ok) return -EFAULT;
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
    while (i < max) {
        /* one page at a time under the lock */
        size_t lim = MIN(max, i + (PAGE_SIZE - ((vaddr_t)(usrc + i) & (PAGE_SIZE - 1))));
        mm_lock(mm);
        if (!user_range_ok_locked(mm, usrc + i, 1, false)) { mm_unlock(mm); return -EFAULT; }
        for (; i < lim; i++) {
            dst[i] = usrc[i];
            if (!dst[i]) { mm_unlock(mm); return (int64_t)i; }
        }
        mm_unlock(mm);
    }
    return -ENAMETOOLONG;
}

int mm_unmap(struct mm *mm, vaddr_t addr, size_t len) {
    mm_lock(mm);
    int r = mm_unmap_locked(mm, addr, len);
    mm_unlock(mm);
    return r;
}

int64_t mm_map(struct mm *mm, vaddr_t addr, size_t len, unsigned prot, unsigned flags, bool fixed) {
    mm_lock(mm);
    int64_t r = mm_map_locked(mm, addr, len, prot, flags, fixed);
    mm_unlock(mm);
    return r;
}

int mm_protect(struct mm *mm, vaddr_t addr, size_t len, unsigned prot) {
    mm_lock(mm);
    int r = mm_protect_locked(mm, addr, len, prot);
    mm_unlock(mm);
    return r;
}

bool mm_handle_fault(struct mm *mm, vaddr_t addr, bool write, bool exec) {
    mm_lock(mm);
    bool r = mm_handle_fault_locked(mm, addr, write, exec);
    mm_unlock(mm);
    if (r && current && current->proc && current->proc->mm == mm)
        __atomic_fetch_add(&current->proc->min_flt, 1, __ATOMIC_RELAXED);
    return r;
}

int mm_write(struct mm *mm, vaddr_t dst, const void *src, size_t n) {
    mm_lock(mm);
    int r = mm_write_locked(mm, dst, src, n);
    mm_unlock(mm);
    return r;
}
