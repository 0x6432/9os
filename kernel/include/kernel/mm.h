#pragma once
/*
 * User address spaces (VMM v2, M26): VMAs in an augmented red-black tree (largest free gap
 * per subtree, for O(log n) top-down placement) plus an address-ordered list, demand-zero
 * anonymous memory with fork COW, demand-faulted file mappings (page cache via
 * file_ops.fault_page, reverse-mapped through inode->i_mmap), per-mm RSS, mlock, and the
 * page-cache reclaim / OOM machinery (mm/mm.c, fs/tmpfs.c).
 */
#include <kernel/spinlock.h>
#include <kernel/types.h>
#include <kernel/list.h>
#include <kernel/rbtree.h>
#include <kernel/vmm.h>

#define USER_TOP        0x00007ffffffff000ULL
#define USER_STACK_SIZE (8ULL << 20)
#define USER_MMAP_BASE  0x00007f0000000000ULL
#define USER_MIN        0x10000ULL
#define STACK_GUARD_GAP (1ULL << 20)   /* non-fixed mappings keep this far below a stack */

enum {
    VMA_ANON   = 1,     /* anonymous memory (no file) */
    VMA_SHARED = 2,     /* MAP_SHARED: writes reach the file / other mappers; no COW */
    VMA_STACK  = 4,
    VMA_PHYS   = 8,     /* device memory: never freed, shared on fork */
    VMA_LOCKED = 16,    /* mlock: populated, its pages are never reclaimed */
    VMA_HEAP   = 32,    /* brk area */
};

struct file;
struct page;
struct mm;
struct vma {
    struct rb_node rb;          /* mm->vma_tree, keyed by start */
    struct list_node node;      /* mm->vmas, sorted by start */
    vaddr_t start, end;
    vaddr_t gap, max_gap;       /* free space below start (to the previous VMA); subtree max */
    unsigned prot;              /* VM_READ | VM_WRITE | VM_EXEC */
    unsigned flags;             /* VMA_* */
    struct mm *mm;
    struct file *file;          /* file mapping (holds a reference), else nullptr */
    uint64_t pgoff;             /* file page index of start */
    struct list_node fnode;     /* file->inode->i_mmap */
};

struct mm {
    pagetable_t pt;
    struct rb_root vma_tree;
    struct list_node vmas;      /* sorted by start */
    int nr_vmas;
    vaddr_t brk_start, brk;
    vaddr_t mmap_hint;
    int refcount;
    vaddr_t sigtramp;           /* user sigreturn trampoline (archs without SA_RESTORER) */
    unsigned def_flags;         /* mlockall(MCL_FUTURE): VMA_LOCKED for new mappings */
    int64_t rss;                /* resident user pages (atomic) */
    uint64_t locked_vm;         /* bytes in VMA_LOCKED mappings */
    struct list_node dead;      /* unlinked VMAs, released after the outermost unlock */
    /* Recursive IRQ-off lock protecting the VMAs and this mm's page-table entries against
     * lock-free faults, user copies and mm syscalls. Order: BKL -> mutexes -> mm->lock ->
     * page cache -> i_mmap -> LRU -> page tables/slab/buddy -> sched. Never sleep under it. */
    spinlock_t lock;
    int lock_owner;             /* cpu id + 1, 0 = free */
    int lock_depth;
    uint64_t lock_flags;
};

void mm_lock(struct mm *mm);
bool mm_trylock(struct mm *mm);   /* fails if another CPU holds it (reclaim) */
void mm_unlock(struct mm *mm);

struct cow_stats { uint64_t shared, copied, reused; };   /* fork COW counters (/proc/vmstat) */
extern struct cow_stats cow_stats;
struct vm_stats {
    uint64_t file_faults, anon_faults, zero_eof_faults, cow_faults, oom_retries, oom_kills;
    uint64_t reclaim_scanned, reclaim_freed, reclaim_runs, kswapd_wakeups, rmap_unmapped;
    uint64_t madv_zapped, mremap_moved, populated, copy_slowpath;
};
extern struct vm_stats vm_stats;

struct mm *mm_create(void);
void mm_put(struct mm *mm);
struct mm *mm_clone(struct mm *mm);
struct vma *vma_find(struct mm *mm, vaddr_t addr);     /* caller holds mm->lock (or owns mm) */
/* Map [addr, addr+len). If fixed is false, addr is a hint. Returns address or -errno. */
int64_t mm_map(struct mm *mm, vaddr_t addr, size_t len, unsigned prot, unsigned flags, bool fixed);
/* file mapping: takes its own reference on f; pgoff is the file page index of addr */
int64_t mm_map_file(struct mm *mm, vaddr_t addr, size_t len, unsigned prot, unsigned flags, bool fixed,
                    struct file *f, uint64_t pgoff);
int mm_unmap(struct mm *mm, vaddr_t addr, size_t len);
int mm_protect(struct mm *mm, vaddr_t addr, size_t len, unsigned prot);
bool mm_range_mapped(struct mm *mm, vaddr_t addr, size_t len);   /* fully covered by VMAs */
bool mm_range_free(struct mm *mm, vaddr_t addr, size_t len);     /* no VMA overlaps */
int64_t mm_remap(struct mm *mm, vaddr_t old, size_t olen, size_t nlen, int flags, vaddr_t naddr);
int mm_advise(struct mm *mm, vaddr_t addr, size_t len, int advice);
int mm_mlock(struct mm *mm, vaddr_t addr, size_t len, bool lock, bool populate);
int mm_mlockall(struct mm *mm, int flags);
int mm_munlockall(struct mm *mm);
int mm_msync(struct mm *mm, vaddr_t addr, size_t len, int flags);
int mm_mincore(struct mm *mm, vaddr_t addr, size_t len, uint8_t *vec);   /* vec: kernel buffer */
int64_t mm_brk(struct mm *mm, vaddr_t addr);
int mm_populate(struct mm *mm, vaddr_t addr, size_t len, bool write);
/* install a page (caller's reference is consumed) at va, unless something is mapped there */
int mm_install_page(struct mm *mm, vaddr_t va, paddr_t pa, unsigned vmflags);
bool mm_handle_fault(struct mm *mm, vaddr_t addr, bool write, bool exec);
/* Access memory of a (possibly inactive) address space. */
int mm_write(struct mm *mm, vaddr_t dst, const void *src, size_t n);
int mm_zero(struct mm *mm, vaddr_t dst, size_t n);
/* reverse map: unmap page-cache page pg from every mapping; false if one could not be
 * locked or the page is mlocked (called by page reclaim with the page-cache lock held) */
bool rmap_unmap_file_page(struct page *pg);
void mm_pressure_init(void);      /* start kswapd */
uint64_t mm_reclaim(uint64_t want);   /* free up to want page-cache pages, returns freed */

/* Access the current process's memory; return 0 or -EFAULT. */
int copy_from_user(void *dst, const void *usrc, size_t n);
int copy_to_user(void *udst, const void *src, size_t n);
int64_t strncpy_from_user(char *dst, const char *usrc, size_t max);
bool user_range_ok(const void *uaddr, size_t n, bool write);
