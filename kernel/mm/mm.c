#include <kernel/mm.h>
#include <kernel/pmm.h>
#include <kernel/boot.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/process.h>

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

struct mm *mm_create(void) {
    struct mm *mm = kzalloc(sizeof *mm);
    if (!mm) return nullptr;
    mm->pt = vmm_new_user_pagetable();
    if (!mm->pt.root) { kfree(mm); return nullptr; }
    list_init(&mm->vmas);
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
            if (--pg->refcount <= 0) page_free(pg, 0);
        }
    }
}

void mm_put(struct mm *mm) {
    if (--mm->refcount > 0) return;
    list_for_each_safe(it, tmp, &mm->vmas) {
        struct vma *v = list_entry(it, struct vma, node);
        free_pages_in(mm, v->start, v->end, v->flags);
        list_del(&v->node);
        kfree(v);
    }
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
            phys_to_page(pa)->refcount++;
            cow_stats.shared++;
        }
    }
    return mm;
fail:
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
    if ((v->flags & (VMA_SHARED | VMA_PHYS)) || pg->refcount <= 1) return pa;
    paddr_t np = pmm_alloc_pages(0);
    if (!np) return 0;
    memcpy(PHYS_TO_VIRT(np), PHYS_TO_VIRT(pa), PAGE_SIZE);
    vmm_unmap(mm->pt, va);
    if (vmm_map(mm->pt, va, np, fl)) { pmm_free_pages(np, 0); return 0; }
    if (--pg->refcount <= 0) page_free(pg, 0);
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
    if (pg->refcount <= 1 || (v->flags & VMA_SHARED)) {        /* sole owner: just re-enable writes */
        vmm_protect(mm->pt, va, v->prot | VM_USER);
        cow_stats.reused++;
        return pa;
    }
    paddr_t np = pmm_alloc_pages(0);
    if (!np) return 0;
    memcpy(PHYS_TO_VIRT(np), PHYS_TO_VIRT(pa), PAGE_SIZE);
    vmm_unmap(mm->pt, va);
    if (vmm_map(mm->pt, va, np, v->prot | VM_USER)) { pmm_free_pages(np, 0); return 0; }
    if (--pg->refcount <= 0) page_free(pg, 0);
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

int mm_unmap(struct mm *mm, vaddr_t addr, size_t len) {
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    vaddr_t end = ALIGN_UP(addr + len, PAGE_SIZE);
    if (vma_split(mm, addr) || vma_split(mm, end)) return -ENOMEM;
    list_for_each_safe(it, tmp, &mm->vmas) {
        struct vma *v = list_entry(it, struct vma, node);
        if (v->start >= addr && v->end <= end) {
            free_pages_in(mm, v->start, v->end, v->flags);
            list_del(&v->node);
            kfree(v);
        }
    }
    return 0;
}

static bool range_free(struct mm *mm, vaddr_t s, vaddr_t e) {
    list_for_each(it, &mm->vmas) {
        struct vma *v = list_entry(it, struct vma, node);
        if (v->start < e && v->end > s) return false;
    }
    return true;
}

int64_t mm_map(struct mm *mm, vaddr_t addr, size_t len, unsigned prot, unsigned flags, bool fixed) {
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

int mm_protect(struct mm *mm, vaddr_t addr, size_t len, unsigned prot) {
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
                if (!(v->flags & (VMA_SHARED | VMA_PHYS)) && phys_to_page(ALIGN_DOWN(pa, PAGE_SIZE))->refcount > 1)
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

bool mm_handle_fault(struct mm *mm, vaddr_t addr, bool write, bool exec) {
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
int mm_write(struct mm *mm, vaddr_t dst, const void *src, size_t n) {
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

bool user_range_ok(const void *uaddr, size_t n, bool write) {
    vaddr_t a = (vaddr_t)uaddr;
    if (a + n < a || a + n > USER_TOP) return false;
    if (!current || !current->proc) return false;
    struct mm *mm = current->proc->mm;
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

int copy_from_user(void *dst, const void *usrc, size_t n) {
    if (!n) return 0;
    if (!user_range_ok(usrc, n, false)) return -EFAULT;
    memcpy(dst, usrc, n);
    return 0;
}

int copy_to_user(void *udst, const void *src, size_t n) {
    if (!n) return 0;
    if (!user_range_ok(udst, n, true)) return -EFAULT;
    memcpy(udst, src, n);
    return 0;
}

int64_t strncpy_from_user(char *dst, const char *usrc, size_t max) {
    for (size_t i = 0; i < max; i++) {
        if (((vaddr_t)(usrc + i) & (PAGE_SIZE - 1)) == 0 || i == 0)
            if (!user_range_ok(usrc + i, 1, false)) return -EFAULT;
        dst[i] = usrc[i];
        if (!dst[i]) return (int64_t)i;
    }
    return -ENAMETOOLONG;
}
