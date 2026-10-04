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
#include <arch/syscall.h>

#define PIE_BASE 0x400000ULL
#define MAX_ARG_BYTES (256 * 1024)

void arch_reset_fpu(struct thread *t);
void arch_set_tls(struct thread *t, uint64_t v);

static int read_exact(struct file *f, void *buf, size_t n, off_t off) {
    ssize_t r = vfs_pread(f, buf, n, off);
    return r == (ssize_t)n ? 0 : (r < 0 ? (int)r : -ENOEXEC);
}

static int load_elf(struct mm *mm, struct file *f, uint64_t *entry, uint64_t *phdr_va,
                    uint16_t *phnum, uint64_t *brk) {
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
    *phdr_va = 0;
    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type == PT_INTERP) { r = -ENOEXEC; goto out; }   /* static binaries only */
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
        /* copy file contents in chunks */
        uint8_t *buf = kmalloc(PAGE_SIZE);
        for (uint64_t off = 0; off < ph[i].p_filesz; off += PAGE_SIZE) {
            size_t n = MIN(PAGE_SIZE, ph[i].p_filesz - off);
            r = read_exact(f, buf, n, ph[i].p_offset + off);
            if (!r) r = mm_write(mm, va + off, buf, n);
            if (r) { kfree(buf); goto out; }
        }
        kfree(buf);
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
                       uint64_t phdr, uint16_t phnum, const char *execfn, uint64_t *out_sp) {
    int64_t r = mm_map(mm, USER_TOP - USER_STACK_SIZE, USER_STACK_SIZE, VM_READ | VM_WRITE,
                       VMA_ANON | VMA_STACK, true);
    if (r < 0) return (int)r;
    uint64_t sp = USER_TOP;
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
    const char *plat = "x86_64";
    push(mm, &sp, plat, 7);
    uint64_t plat_va = sp;
    uint64_t auxv[] = {
        AT_PHDR, phdr, AT_PHENT, sizeof(Elf64_Phdr), AT_PHNUM, phnum, AT_PAGESZ, PAGE_SIZE,
        AT_BASE, 0, AT_FLAGS, 0, AT_ENTRY, entry, AT_UID, 0, AT_EUID, 0, AT_GID, 0, AT_EGID, 0,
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
    r = load_elf(mm, f, &entry, &phdr, &phnum, &brk);
    vfs_close(f);
    if (!r) r = setup_stack(mm, argv, envp, entry, phdr, phnum, path, &sp);
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
    files_close_on_exec(p);
    signals_reset_on_exec(p);
    arch_reset_fpu(current);
    arch_set_tls(current, 0);
    frame_init_user(frame, entry, sp);
    if (p->vfork) { p->vfork->done = true; wake_up(&p->vfork->wq); p->vfork = nullptr; }
    return 0;
}
