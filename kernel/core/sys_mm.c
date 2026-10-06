/*
 * Memory-management system calls (M26). All of them run without the BKL (lockfree_names in
 * syscall.c.in): the address space is protected by mm->lock (mm/mm.c). Device mappings
 * (file_ops.mmap / mmap_page, e.g. fbdev and DRM) still call into their drivers under the BKL.
 */
#include <kernel/uaccess.h>
#include <kernel/pmm.h>
#include <kernel/syscall.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>

#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define PROT_GROWSDOWN 0x01000000
#define PROT_GROWSUP 0x02000000
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_SHARED_VALIDATE 3
#define MAP_TYPE 0xf
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_GROWSDOWN 0x100
#define MAP_DENYWRITE 0x800
#define MAP_EXECUTABLE 0x1000
#define MAP_LOCKED 0x2000
#define MAP_NORESERVE 0x4000
#define MAP_POPULATE 0x8000
#define MAP_NONBLOCK 0x10000
#define MAP_STACK 0x20000
#define MAP_HUGETLB 0x40000
#define MAP_SYNC 0x80000
#define MAP_FIXED_NOREPLACE 0x100000

struct inode *tmpfs_create_anon(uint32_t mode);

static unsigned prot_to_vm(int prot) {
    return (prot & PROT_READ ? VM_READ : 0) | (prot & PROT_WRITE ? VM_WRITE : 0) | (prot & PROT_EXEC ? VM_EXEC : 0);
}

int64_t sys_brk(uint64_t addr) { return mm_brk(curproc->mm, addr); }

uint64_t pc_stats_mapped;   /* file pages mapped through eager mmap_page (DRM) */

/* device memory (file_ops.mmap): one physically contiguous range, prefaulted */
static int64_t map_device(struct file *f, uint64_t addr, size_t len, int prot, int flags, off_t off) {
    struct mm *mm = curproc->mm;
    paddr_t pa;
    bkl_enter();
    int e = f->fops->mmap(f, off, len, &pa);
    bkl_exit();
    if (e) return e;
    int64_t r = mm_map(mm, addr, len, prot_to_vm(prot), VMA_PHYS | VMA_SHARED, flags & MAP_FIXED);
    if (r < 0) return r;
    mm_lock(mm);
    for (size_t o = 0; o < len; o += PAGE_SIZE)
        vmm_map(mm->pt, r + o, pa + o, prot_to_vm(prot) | VM_USER | VM_WC);
    mm_unlock(mm);
    return r;
}

/* drivers that hand out pages one by one (DRM dumb buffers): mapped eagerly */
static int64_t map_driver_pages(struct file *f, uint64_t addr, size_t len, int prot, int flags, off_t off) {
    struct mm *mm = curproc->mm;
    int64_t r = mm_map(mm, addr, len, prot_to_vm(prot), VMA_SHARED, flags & MAP_FIXED);
    if (r < 0) return r;
    for (size_t o = 0; o < len; o += PAGE_SIZE) {
        paddr_t pa;
        bkl_enter();
        int e = f->fops->mmap_page(f, (off + o) / PAGE_SIZE, &pa);
        bkl_exit();
        if (e) { mm_unmap(mm, r, len); return e; }
        page_ref_inc(phys_to_page(pa));          /* the PTE's reference */
        int ir = mm_install_page(mm, r + o, pa, prot_to_vm(prot) | VM_USER);
        if (ir < 0) { mm_unmap(mm, r, len); return -ENOMEM; }
        if (!ir) pc_stats_mapped++;
    }
    return r;
}

/* MAP_SHARED|MAP_ANONYMOUS and MAP_SHARED of /dev/zero: an unlinked tmpfs file (shmem) */
static struct file *shmem_file(size_t len) {
    struct inode *i = tmpfs_create_anon(S_IFREG | 0600);
    if (!i) return nullptr;
    int e = i->iops->truncate(i, len);
    struct file *f = e ? nullptr : file_open_inode(i, O_RDWR);
    iput(i);
    return f;
}

int64_t sys_mmap(uint64_t addr, size_t len, int prot, int flags, int fd, off_t off) {
    struct mm *mm = curproc->mm;
    if (!len || off & (PAGE_SIZE - 1) || prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC | PROT_GROWSDOWN | PROT_GROWSUP))
        return -EINVAL;
    int type = flags & MAP_TYPE;
    if (type != MAP_SHARED && type != MAP_PRIVATE && type != MAP_SHARED_VALIDATE) return -EINVAL;
    if (type == MAP_SHARED_VALIDATE &&
        flags & ~(MAP_TYPE | MAP_FIXED | MAP_ANONYMOUS | MAP_GROWSDOWN | MAP_DENYWRITE | MAP_EXECUTABLE | MAP_LOCKED |
                  MAP_NORESERVE | MAP_POPULATE | MAP_NONBLOCK | MAP_STACK | MAP_HUGETLB | MAP_FIXED_NOREPLACE))
        return -EOPNOTSUPP;
    bool shared = type != MAP_PRIVATE;
    len = ALIGN_UP(len, PAGE_SIZE);
    if (!len || len > USER_TOP) return -ENOMEM;
    if (flags & MAP_FIXED_NOREPLACE) {
        flags |= MAP_FIXED;
        if (addr & (PAGE_SIZE - 1)) return -EINVAL;
        if (!mm_range_free(mm, addr, len)) return -EEXIST;
    }
    if ((flags & MAP_FIXED) && (addr & (PAGE_SIZE - 1))) return -EINVAL;
    unsigned vp = prot_to_vm(prot), vflags = (shared ? VMA_SHARED : 0) | (flags & MAP_LOCKED ? VMA_LOCKED : 0);
    int wx = wx_check(vp);
    if (wx) return wx;
    struct file *f = nullptr, *own = nullptr;
    if (!(flags & MAP_ANONYMOUS)) {
        f = fd_get(fd);
        if (!f) return -EBADF;
        unsigned acc = f->flags & O_ACCMODE;
        if (acc == O_WRONLY) return -EACCES;
        if (shared && (prot & PROT_WRITE) && acc != O_RDWR) return -EACCES;
        if (f->fops && f->fops->mmap) return map_device(f, addr, len, prot, flags, off);
        if (shared && f->fops && f->fops->mmap_page && !f->fops->fault_page) return map_driver_pages(f, addr, len, prot, flags, off);
        if (!f->fops || !f->fops->fault_page) {
            /* character devices without mmap support: /dev/zero behaves like anonymous memory */
            if (!S_ISCHR(f->inode->mode) || !f->fops || !f->fops->read) return -ENODEV;
            f = nullptr;
        }
    }
    if (!f && shared) {
        own = f = shmem_file(len);
        if (!f) return -ENOMEM;
        off = 0;
    }
    int64_t r = f ? mm_map_file(mm, addr, len, vp, vflags, flags & MAP_FIXED, f, off / PAGE_SIZE)
                  : mm_map(mm, addr, len, vp, VMA_ANON | vflags, flags & MAP_FIXED);
    if (own) vfs_close(own);           /* the VMA holds its own reference */
    if (r < 0) return r;
    if (flags & (MAP_POPULATE | MAP_LOCKED)) mm_populate(mm, r, len, false);
    return r;
}

int64_t sys_munmap(uint64_t addr, size_t len) {
    if (addr & (PAGE_SIZE - 1) || !len) return -EINVAL;
    return mm_unmap(curproc->mm, addr, len);
}

int64_t sys_mprotect(uint64_t addr, size_t len, int prot) {
    if (addr & (PAGE_SIZE - 1) || prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC | PROT_GROWSDOWN | PROT_GROWSUP)) return -EINVAL;
    if (!len) return 0;
    int wx = wx_check(prot_to_vm(prot));
    if (wx) return wx;
    return mm_protect(curproc->mm, addr, len, prot_to_vm(prot));
}

int64_t sys_mremap(uint64_t old, size_t olen, size_t nlen, int flags, uint64_t naddr) {
    return mm_remap(curproc->mm, old, olen, nlen, flags, naddr);
}

int64_t sys_madvise(uint64_t addr, size_t len, int advice) { return mm_advise(curproc->mm, addr, len, advice); }

#define MLOCK_ONFAULT 1
int64_t sys_mlock(uint64_t addr, size_t len) { return mm_mlock(curproc->mm, addr, len, true, true); }
int64_t sys_mlock2(uint64_t addr, size_t len, unsigned flags) {
    if (flags & ~MLOCK_ONFAULT) return -EINVAL;
    return mm_mlock(curproc->mm, addr, len, true, !(flags & MLOCK_ONFAULT));
}
int64_t sys_munlock(uint64_t addr, size_t len) { return mm_mlock(curproc->mm, addr, len, false, false); }
int64_t sys_mlockall(int flags) { return mm_mlockall(curproc->mm, flags); }
int64_t sys_munlockall(void) { return mm_munlockall(curproc->mm); }
int64_t sys_msync(uint64_t addr, size_t len, int flags) { return mm_msync(curproc->mm, addr, len, flags); }

int64_t sys_mincore(uint64_t addr, size_t len, uint8_t *uvec) {
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    if (addr + len < addr || addr + len > USER_TOP) return -ENOMEM;
    size_t pages = ALIGN_UP(len, PAGE_SIZE) / PAGE_SIZE;
    uint8_t buf[512];
    for (size_t done = 0; done < pages;) {
        size_t n = MIN(pages - done, sizeof buf);
        int r = mm_mincore(curproc->mm, addr + done * PAGE_SIZE, n * PAGE_SIZE, buf);
        if (r) return r;
        if (copy_to_user(uvec + done, buf, n)) return -EFAULT;
        done += n;
    }
    return 0;
}
