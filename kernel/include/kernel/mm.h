#pragma once
/* User address spaces: VMAs + demand-zero paging. */
#include <kernel/types.h>
#include <kernel/list.h>
#include <kernel/vmm.h>

#define USER_TOP        0x00007ffffffff000ULL
#define USER_STACK_SIZE (8ULL << 20)
#define USER_MMAP_BASE  0x00007f0000000000ULL
#define USER_MIN        0x10000ULL

enum { VMA_ANON = 1, VMA_SHARED = 2, VMA_STACK = 4 };

struct vma {
    struct list_node node;
    vaddr_t start, end;
    unsigned prot;          /* VM_READ | VM_WRITE | VM_EXEC */
    unsigned flags;
};

struct mm {
    pagetable_t pt;
    struct list_node vmas;  /* sorted by start */
    vaddr_t brk_start, brk;
    vaddr_t mmap_hint;
    int refcount;
};

struct mm *mm_create(void);
void mm_put(struct mm *mm);
struct mm *mm_clone(struct mm *mm);
struct vma *vma_find(struct mm *mm, vaddr_t addr);
/* Map [addr, addr+len). If fixed is false, addr is a hint. Returns address or -errno. */
int64_t mm_map(struct mm *mm, vaddr_t addr, size_t len, unsigned prot, unsigned flags, bool fixed);
int mm_unmap(struct mm *mm, vaddr_t addr, size_t len);
int mm_protect(struct mm *mm, vaddr_t addr, size_t len, unsigned prot);
bool mm_handle_fault(struct mm *mm, vaddr_t addr, bool write, bool exec);
/* Access memory of a (possibly inactive) address space. */
int mm_write(struct mm *mm, vaddr_t dst, const void *src, size_t n);
int mm_zero(struct mm *mm, vaddr_t dst, size_t n);

/* Access the current process's memory; return 0 or -EFAULT. */
int copy_from_user(void *dst, const void *usrc, size_t n);
int copy_to_user(void *udst, const void *src, size_t n);
int64_t strncpy_from_user(char *dst, const char *usrc, size_t max);
bool user_range_ok(const void *uaddr, size_t n, bool write);
