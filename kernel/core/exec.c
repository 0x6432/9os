/* ELF64 loader and execve. */
#include <kernel/exec.h>
#include <kernel/elf.h>
#include <kernel/mm.h>
#include <kernel/process.h>
#include <kernel/vfs.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/time.h>
#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <arch/syscall.h>

#define PIE_BASE 0x400000ULL
#define MAX_ARG_BYTES (256 * 1024)

void arch_reset_fpu(struct thread *t);
void arch_set_tls(struct thread *t, uint64_t v);

uint64_t pc_stats_exec;   /* page-cache pages mapped by exec */

static int read_exact(struct file *f, void *buf, size_t n, off_t off) {
    ssize_t r = vfs_pread(f, buf, n, off);
    return r == (ssize_t)n ? 0 : (r < 0 ? (int)r : -ENOEXEC);
}

/* Load an ELF image. The main program (is_interp false) goes to its link address or PIE_BASE;
 * the dynamic linker (is_interp true) to a free range found by the mmap allocator. If the
 * program has PT_INTERP, its path is returned in *interp (kmalloc'd). */
static int load_elf(struct mm *mm, struct file *f, bool is_interp, uint64_t *entry, uint64_t *phdr_va,
                    uint16_t *phnum, uint64_t *brk, uint64_t *base_out, char **interp) {
    Elf64_Ehdr eh;
    int r = read_exact(f, &eh, sizeof eh, 0);
    if (r) return r;
    if (memcmp(eh.e_ident, "\x7f" "ELF", 4) || eh.e_ident[4] != 2 || eh.e_machine != ELF_MACHINE)
        return -ENOEXEC;
    if (eh.e_type != ET_EXEC && eh.e_type != ET_DYN) return -ENOEXEC;
    if (eh.e_phnum > 64 || eh.e_phentsize != sizeof(Elf64_Phdr)) return -ENOEXEC;
    Elf64_Phdr *ph = kmalloc(sizeof(Elf64_Phdr) * eh.e_phnum);
    r = read_exact(f, ph, sizeof(Elf64_Phdr) * eh.e_phnum, eh.e_phoff);
    if (r) goto out;
    uint64_t bias = eh.e_type == ET_DYN ? PIE_BASE : 0, top = 0;
    if (is_interp) {
        if (eh.e_type != ET_DYN) { r = -ENOEXEC; goto out; }
        uint64_t lo = UINT64_MAX, hi = 0;
        for (int i = 0; i < eh.e_phnum; i++) if (ph[i].p_type == PT_LOAD) {
            lo = MIN(lo, ALIGN_DOWN(ph[i].p_vaddr, PAGE_SIZE));
            hi = MAX(hi, ALIGN_UP(ph[i].p_vaddr + ph[i].p_memsz, PAGE_SIZE));
        }
        if (lo >= hi) { r = -ENOEXEC; goto out; }
        int64_t b = mm_map(mm, 0, hi - lo, VM_READ, VMA_ANON, false);   /* reserve a free range */
        if (b < 0) { r = (int)b; goto out; }
        mm_unmap(mm, b, hi - lo);
        bias = b - lo;
    }
    if (base_out) *base_out = bias;
    *phdr_va = 0;
    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type == PT_INTERP) {
            if (is_interp || !interp || ph[i].p_filesz < 2 || ph[i].p_filesz > 255) { r = -ENOEXEC; goto out; }
            char *ip = kzalloc(ph[i].p_filesz + 1);
            r = read_exact(f, ip, ph[i].p_filesz, ph[i].p_offset);
            if (r) { kfree(ip); goto out; }
            ip[ph[i].p_filesz] = 0;
            *interp = ip;
            continue;
        }
        if (ph[i].p_type == PT_PHDR) *phdr_va = ph[i].p_vaddr + bias;
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;
        uint64_t va = ph[i].p_vaddr + bias;
        uint64_t s = ALIGN_DOWN(va, PAGE_SIZE), e = ALIGN_UP(va + ph[i].p_memsz, PAGE_SIZE);
        unsigned prot = (ph[i].p_flags & PF_R ? VM_READ : 0) | (ph[i].p_flags & PF_W ? VM_WRITE : 0) |
                        (ph[i].p_flags & PF_X ? VM_EXEC : 0);
        /* segments may share a page: map only the uncovered part, widen protection otherwise */
        for (uint64_t p = s; p < e; ) {
            struct vma *v = vma_find(mm, p);
            if (v) { v->prot |= prot; p = v->end; continue; }
            uint64_t q = p;
            while (q < e && !vma_find(mm, q)) q += PAGE_SIZE;
            int64_t m = mm_map(mm, p, q - p, prot, VMA_ANON, true);
            if (m < 0) { r = (int)m; goto out; }
            p = q;
        }
        /* read-only segments come straight from the page cache (shared by every process
         * running this file); a page already present (shared with a previous segment) is
         * filled by copying, which breaks COW as usual */
        if (!(ph[i].p_flags & PF_W) && f->fops && f->fops->mmap_page && S_ISREG(f->inode->mode) &&
            (va - ph[i].p_offset) % PAGE_SIZE == 0) {
            uint64_t pva = ALIGN_DOWN(va, PAGE_SIZE), foff = ALIGN_DOWN(ph[i].p_offset, PAGE_SIZE);
            bool ok = true;
            for (; pva < va + ph[i].p_filesz; pva += PAGE_SIZE, foff += PAGE_SIZE) {
                paddr_t pa;
                if (vmm_query(mm->pt, pva, nullptr, nullptr) || f->fops->mmap_page(f, foff / PAGE_SIZE, &pa)) { ok = false; break; }
                page_ref_inc(phys_to_page(pa));
                if (vmm_map(mm->pt, pva, pa, (prot & ~VM_WRITE) | VM_USER)) { page_put_pa(pa); r = -ENOMEM; goto out; }
                pc_stats_exec++;
            }
            if (ok) goto mapped;
            /* fall back to copying the rest of the segment */
            uint64_t done = pva > va ? pva - va : 0;
            uint8_t *buf = kmalloc(PAGE_SIZE);
            for (uint64_t off = done; off < ph[i].p_filesz; off += PAGE_SIZE) {
                size_t n = MIN(PAGE_SIZE, ph[i].p_filesz - off);
                r = read_exact(f, buf, n, ph[i].p_offset + off);
                if (!r) r = mm_write(mm, va + off, buf, n);
                if (r) { kfree(buf); goto out; }
            }
            kfree(buf);
            goto mapped;
        }
        /* copy file contents in chunks */
        uint8_t *buf = kmalloc(PAGE_SIZE);
        for (uint64_t off = 0; off < ph[i].p_filesz; off += PAGE_SIZE) {
            size_t n = MIN(PAGE_SIZE, ph[i].p_filesz - off);
            r = read_exact(f, buf, n, ph[i].p_offset + off);
            if (!r) r = mm_write(mm, va + off, buf, n);
            if (r) { kfree(buf); goto out; }
        }
        kfree(buf);
    mapped:
        if (!*phdr_va && ph[i].p_offset == 0) *phdr_va = va + eh.e_phoff;
        top = MAX(top, e);
    }
    *entry = eh.e_entry + bias;
    *phnum = eh.e_phnum;
    *brk = top;
    r = 0;
out:
    kfree(ph);
    return r;
}

static int push(struct mm *mm, uint64_t *sp, const void *data, size_t n) {
    *sp -= n;
    return mm_write(mm, *sp, data, n);
}

/* Build the System V initial process stack. */
static int setup_stack(struct mm *mm, char *const argv[], char *const envp[], uint64_t entry,
                       uint64_t phdr, uint16_t phnum, uint64_t interp_base, const char *execfn, uint64_t *out_sp) {
    int64_t r = mm_map(mm, USER_TOP - USER_STACK_SIZE, USER_STACK_SIZE, VM_READ | VM_WRITE,
                       VMA_ANON | VMA_STACK, true);
    if (r < 0) return (int)r;
    uint64_t sp = USER_TOP;
#ifdef ARCH_SIGTRAMP_CODE
    {   /* tiny "vDSO": rt_sigreturn trampoline used as the signal handler return address */
        static const uint32_t code[] = { ARCH_SIGTRAMP_CODE };
        vaddr_t va = USER_TOP - USER_STACK_SIZE - 0x10000;
        if (mm_map(mm, va, PAGE_SIZE, VM_READ | VM_EXEC, VMA_ANON, true) == (int64_t)va &&
            !mm_write(mm, va, code, sizeof code))
            mm->sigtramp = va;
    }
#endif
    int argc = 0, envc = 0;
    while (argv && argv[argc]) argc++;
    while (envp && envp[envc]) envc++;
    uint64_t *uargv = kmalloc(sizeof(uint64_t) * (argc + envc + 2));
    uint64_t *uenvp = uargv + argc + 1;
    push(mm, &sp, execfn, strlen(execfn) + 1);
    uint64_t execfn_va = sp;
    for (int i = envc - 1; i >= 0; i--) { push(mm, &sp, envp[i], strlen(envp[i]) + 1); uenvp[i] = sp; }
    for (int i = argc - 1; i >= 0; i--) { push(mm, &sp, argv[i], strlen(argv[i]) + 1); uargv[i] = sp; }
    uargv[argc] = 0; uenvp[envc] = 0;
    uint64_t rnd[2] = { time_ns() * 6364136223846793005ULL + 1442695040888963407ULL, (uint64_t)uargv ^ time_ns() };
    sp &= ~15ULL;
    push(mm, &sp, rnd, 16);
    uint64_t random_va = sp;
    const char *plat = ARCH_PLATFORM;
    push(mm, &sp, plat, strlen(plat) + 1);
    uint64_t plat_va = sp;
    uint64_t auxv[] = {
        AT_PHDR, phdr, AT_PHENT, sizeof(Elf64_Phdr), AT_PHNUM, phnum, AT_PAGESZ, PAGE_SIZE,
        AT_BASE, interp_base, AT_FLAGS, 0, AT_ENTRY, entry, AT_UID, 0, AT_EUID, 0, AT_GID, 0, AT_EGID, 0,
        AT_SECURE, 0, AT_RANDOM, random_va, AT_HWCAP, 0, AT_CLKTCK, 100, AT_PLATFORM, plat_va,
        AT_EXECFN, execfn_va, AT_NULL, 0,
    };
    size_t words = ARRAY_SIZE(auxv) + (envc + 1) + (argc + 1) + 1;
    sp = (sp - words * 8) & ~15ULL;
    uint64_t p = sp;
    uint64_t a = argc;
    mm_write(mm, p, &a, 8); p += 8;
    mm_write(mm, p, uargv, (argc + 1) * 8); p += (argc + 1) * 8;
    mm_write(mm, p, uenvp, (envc + 1) * 8); p += (envc + 1) * 8;
    mm_write(mm, p, auxv, sizeof auxv);
    kfree(uargv);
    *out_sp = sp;
    return 0;
}

void files_close_on_exec(struct process *p);
void signals_reset_on_exec(struct process *p);

int do_execve(const char *path, char *const argv[], char *const envp[], struct trap_frame *frame) {
    struct file *f;
    int r = vfs_open(path, O_RDONLY, 0, &f);
    if (r) return r;
    if (!S_ISREG(f->inode->mode)) { vfs_close(f); return -EACCES; }
    if (!(f->inode->mode & 0111)) { vfs_close(f); return -EACCES; }

    /* #! scripts */
    char hdr[128];
    ssize_t n = vfs_pread(f, hdr, sizeof hdr - 1, 0);
    if (n >= 2 && hdr[0] == '#' && hdr[1] == '!') {
        vfs_close(f);
        hdr[n] = 0;
        char *nl = strchr(hdr, '\n');
        if (nl) *nl = 0;
        char *interp = hdr + 2;
        while (*interp == ' ' || *interp == '\t') interp++;
        char *arg = interp;
        while (*arg && *arg != ' ' && *arg != '\t') arg++;
        if (*arg) { *arg++ = 0; while (*arg == ' ' || *arg == '\t') arg++; }
        int argc = 0;
        while (argv[argc]) argc++;
        char **nargv = kmalloc(sizeof(char *) * (argc + 4));
        int k = 0;
        nargv[k++] = interp;
        if (*arg) nargv[k++] = arg;
        nargv[k++] = (char *)path;
        for (int i = 1; i < argc; i++) nargv[k++] = argv[i];
        nargv[k] = nullptr;
        r = do_execve(interp, nargv, envp, frame);
        kfree(nargv);
        return r;
    }

    struct mm *mm = mm_create();
    if (!mm) { vfs_close(f); return -ENOMEM; }
    uint64_t entry, phdr, brk, sp;
    uint16_t phnum;
    char *interp = nullptr;
    uint64_t start, ibase = 0;
    r = load_elf(mm, f, false, &entry, &phdr, &phnum, &brk, nullptr, &interp);
    vfs_close(f);
    start = entry;
    if (!r && interp) {          /* dynamic executable: load ld.so and start there */
        struct file *fi;
        r = vfs_open(interp, O_RDONLY, 0, &fi);
        if (!r) {
            uint64_t ient, iphdr, ibrk; uint16_t iphnum;
            r = load_elf(mm, fi, true, &ient, &iphdr, &iphnum, &ibrk, &ibase, nullptr);
            vfs_close(fi);
            start = ient;
        }
        kfree(interp);
    }
    if (!r) r = setup_stack(mm, argv, envp, entry, phdr, phnum, ibase, path, &sp);
    if (r) { mm_put(mm); return r; }
    mm->brk_start = mm->brk = brk;

    /* point of no return */
    struct process *p = curproc;
    struct mm *old = p->mm;
    p->mm = mm;
    vmm_switch(mm->pt);
    if (old) mm_put(old);
    const char *base = strrchr(path, '/');
    strlcpy(p->name, base ? base + 1 : path, sizeof p->name);
    strlcpy(current->name, p->name, sizeof current->name);
    kfree(p->cmdline);
    size_t cl = 0;
    for (int i = 0; argv && argv[i]; i++) cl += strlen(argv[i]) + 1;
    p->cmdline = kmalloc(cl ? cl : 1);
    p->cmdline_len = cl;
    for (int i = 0, o = 0; argv && argv[i]; i++) { size_t l = strlen(argv[i]) + 1; memcpy(p->cmdline + o, argv[i], l); o += l; }
    kfree(p->exe);
    p->exe = strdup(path);
    files_close_on_exec(p);
    signals_reset_on_exec(p);
    arch_reset_fpu(current);
    arch_set_tls(current, 0);
    frame_init_user(frame, start, sp);
    if (p->vfork) { p->vfork->done = true; wake_up(&p->vfork->wq); p->vfork = nullptr; }
    return 0;
}
