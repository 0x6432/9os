/* Memory-management system calls. */
#include <kernel/pmm.h>
#include <kernel/syscall.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>

#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_FIXED_NOREPLACE 0x100000

static unsigned prot_to_vm(int prot) {
    return (prot & PROT_READ ? VM_READ : 0) | (prot & PROT_WRITE ? VM_WRITE : 0) | (prot & PROT_EXEC ? VM_EXEC : 0);
}

int64_t sys_brk(uint64_t addr) {
    struct mm *mm = curproc->mm;
    if (addr < mm->brk_start) return mm->brk;
    uint64_t old_end = ALIGN_UP(mm->brk, PAGE_SIZE), new_end = ALIGN_UP(addr, PAGE_SIZE);
    if (new_end > old_end) {
        for (uint64_t a = old_end; a < new_end; a += PAGE_SIZE)
            if (vma_find(mm, a)) return mm->brk;
        int64_t r = mm_map(mm, old_end, new_end - old_end, VM_READ | VM_WRITE, VMA_ANON, true);
        if (r < 0) return mm->brk;
    } else if (new_end < old_end) {
        mm_unmap(mm, new_end, old_end - new_end);
    }
    mm->brk = addr;
    return addr;
}

uint64_t pc_stats_mapped;   /* page-cache pages mapped privately (shown in /proc/vmstat) */

int64_t sys_mmap(uint64_t addr, size_t len, int prot, int flags, int fd, off_t off) {
    struct mm *mm = curproc->mm;
    if (!len) return -EINVAL;
    if (off & (PAGE_SIZE - 1)) return -EINVAL;
    struct file *f = nullptr;
    if (!(flags & MAP_ANONYMOUS)) {
        f = fd_get(fd);
        if (!f) return -EBADF;
        if (f->fops && f->fops->mmap) {          /* device memory (e.g. /dev/fb0) */
            paddr_t pa;
            int e = f->fops->mmap(f, off, ALIGN_UP(len, PAGE_SIZE), &pa);
            if (e) return e;
            bool fx = flags & (MAP_FIXED | MAP_FIXED_NOREPLACE);
            int64_t r = mm_map(curproc->mm, addr, len, prot_to_vm(prot), VMA_PHYS | VMA_SHARED, fx);
            if (r < 0) return r;
            for (size_t o = 0; o < ALIGN_UP(len, PAGE_SIZE); o += PAGE_SIZE)
                vmm_map(mm->pt, r + o, pa + o, prot_to_vm(prot) | VM_USER | VM_WC);
            return r;
        }
        if ((flags & MAP_SHARED) && f->fops && f->fops->mmap_page) {   /* shared file pages (tmpfs/memfd) */
            bool fx = flags & (MAP_FIXED | MAP_FIXED_NOREPLACE);
            int64_t r = mm_map(mm, addr, len, prot_to_vm(prot), VMA_SHARED, fx);
            if (r < 0) return r;
            for (size_t o = 0; o < ALIGN_UP(len, PAGE_SIZE); o += PAGE_SIZE) {
                paddr_t pa;
                int e = f->fops->mmap_page(f, (off + o) / PAGE_SIZE, &pa);
                if (e) { mm_unmap(mm, r, len); return e; }
                phys_to_page(pa)->refcount++;
                if (vmm_map(mm->pt, r + o, pa, prot_to_vm(prot) | VM_USER)) { page_put_pa(pa); mm_unmap(mm, r, len); return -ENOMEM; }
            }
            return r;
        }
        if (!(flags & MAP_SHARED) && f->fops && f->fops->mmap_page && S_ISREG(f->inode->mode)) {
            /* private file mapping from the page cache: map the file's pages read-only and let
             * the COW fault path copy a page on the first write (the cache holds a reference,
             * so the page is never written in place). Pages past EOF are demand-zero. */
            bool fx = flags & (MAP_FIXED | MAP_FIXED_NOREPLACE);
            if ((flags & MAP_FIXED_NOREPLACE)) {
                for (uint64_t a = addr; a < addr + len; a += PAGE_SIZE) if (vma_find(mm, a)) return -EEXIST;
            }
            unsigned vp = prot_to_vm(prot);
            int64_t r = mm_map(mm, addr, len, vp, VMA_ANON, fx);
            if (r < 0) return r;
            uint64_t fsize = f->inode->size;
            for (size_t o = 0; o < ALIGN_UP(len, PAGE_SIZE) && off + o < fsize; o += PAGE_SIZE) {
                paddr_t pa;
                if (f->fops->mmap_page(f, (off + o) / PAGE_SIZE, &pa)) break;
                phys_to_page(pa)->refcount++;
                if (vmm_map(mm->pt, r + o, pa, (vp & ~VM_WRITE) | VM_USER)) { page_put_pa(pa); mm_unmap(mm, r, len); return -ENOMEM; }
                pc_stats_mapped++;
            }
            return r;
        }
        if (!f->fops || !f->fops->read) return -ENODEV;
    }
    bool fixed = flags & (MAP_FIXED | MAP_FIXED_NOREPLACE);
    if ((flags & MAP_FIXED_NOREPLACE)) {
        for (uint64_t a = addr; a < addr + len; a += PAGE_SIZE) if (vma_find(mm, a)) return -EEXIST;
    }
    int64_t r = mm_map(mm, addr, len, prot_to_vm(prot), VMA_ANON | (flags & MAP_SHARED ? VMA_SHARED : 0), fixed);
    if (r < 0) return r;
    if (f) {
        /* private snapshot of the file contents */
        uint8_t *buf = kmalloc(PAGE_SIZE);
        for (size_t done = 0; done < len; done += PAGE_SIZE) {
            ssize_t n = vfs_pread(f, buf, PAGE_SIZE, off + done);
            if (n <= 0) break;
            mm_write(mm, r + done, buf, n);
            if (n < (ssize_t)PAGE_SIZE) break;
        }
        kfree(buf);
    }
    return r;
}

int64_t sys_munmap(uint64_t addr, size_t len) {
    if (addr & (PAGE_SIZE - 1) || !len) return -EINVAL;
    return mm_unmap(curproc->mm, addr, len);
}

int64_t sys_mprotect(uint64_t addr, size_t len, int prot) {
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    if (!len) return 0;
    return mm_protect(curproc->mm, addr, len, prot_to_vm(prot));
}

int64_t sys_mremap(uint64_t old, size_t olen, size_t nlen, int flags, uint64_t naddr) {
    struct mm *mm = curproc->mm;
    if (old & (PAGE_SIZE - 1)) return -EINVAL;
    olen = ALIGN_UP(olen, PAGE_SIZE); nlen = ALIGN_UP(nlen, PAGE_SIZE);
    struct vma *v = vma_find(mm, old);
    if (!v) return -EFAULT;
    if (nlen <= olen) {
        if (nlen < olen) mm_unmap(mm, old + nlen, olen - nlen);
        return old;
    }
    /* try to grow in place */
    bool free_after = true;
    for (uint64_t a = old + olen; a < old + nlen; a += PAGE_SIZE) if (vma_find(mm, a)) { free_after = false; break; }
    if (free_after && v->end == old + olen) {
        v->end = old + nlen;
        return old;
    }
    if (!(flags & 1)) return -ENOMEM;   /* MREMAP_MAYMOVE */
    int64_t n = mm_map(mm, 0, nlen, v->prot, v->flags, false);
    if (n < 0) return n;
    /* move pages */
    for (uint64_t off = 0; off < olen; off += PAGE_SIZE) {
        paddr_t pa; unsigned fl;
        if (vmm_query(mm->pt, old + off, &pa, &fl)) {
            vmm_unmap(mm->pt, old + off);
            vmm_map(mm->pt, n + off, ALIGN_DOWN(pa, PAGE_SIZE), fl);
        }
    }
    mm_unmap(mm, old, olen);
    return n;
}
