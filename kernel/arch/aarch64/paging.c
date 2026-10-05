/* aarch64 4-level, 4 KiB-granule paging. Kernel half in TTBR1 (kernel_pt), user half in TTBR0. */
#include <kernel/sched.h>
#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <kernel/boot.h>
#include <kernel/printk.h>
#include <kernel/string.h>
#include <kernel/spinlock.h>
#include <kernel/errno.h>
#include <arch/cpu.h>

#define D_VALID (1ULL << 0)
#define D_TABLE (1ULL << 1)        /* table (levels 0-2) or page (level 3) */
#define D_ATTR(i) ((uint64_t)(i) << 2)
#define D_AP_EL0 (1ULL << 6)
#define D_AP_RO  (1ULL << 7)
#define D_SH_IN  (3ULL << 8)
#define D_AF     (1ULL << 10)
#define D_NG     (1ULL << 11)
#define D_PXN    (1ULL << 53)
#define D_UXN    (1ULL << 54)
#define D_ADDR   0x0000fffffffff000ULL

pagetable_t kernel_pt;
static paddr_t empty_root;              /* TTBR0 for kernel threads */
static spinlock_t pt_lock = SPINLOCK_INIT;
static uint64_t pt_lock_irqsave(void) { uint64_t f = arch_irq_save(); spin_lock_ipi(&pt_lock); return f; }
static unsigned attr_normal, attr_device, attr_wc;

static uint64_t attrs(unsigned fl) {
    uint64_t e = D_VALID | D_AF;
    unsigned idx = (fl & VM_NOCACHE) ? attr_device : (fl & VM_WC) ? attr_wc : attr_normal;
    e |= D_ATTR(idx);
    if (idx == attr_normal) e |= D_SH_IN;
    if (!(fl & VM_WRITE)) e |= D_AP_RO;
    if (fl & VM_USER) { e |= D_AP_EL0 | D_NG | D_PXN; if (!(fl & VM_EXEC)) e |= D_UXN; }
    else { e |= D_UXN; if (!(fl & VM_EXEC)) e |= D_PXN; }
    return e;
}
static uint64_t to_pte(paddr_t pa, unsigned fl) { return (pa & D_ADDR) | attrs(fl) | D_TABLE; }

static inline paddr_t root_for(paddr_t root, vaddr_t va) { return (va >> 63) ? kernel_pt.root : root; }

static uint64_t *walk(paddr_t root, vaddr_t va, bool create) {
    uint64_t *table = PHYS_TO_VIRT(root_for(root, va));
    for (int level = 0; level < 3; level++) {
        unsigned idx = (va >> (39 - 9 * level)) & 511;
        uint64_t e = table[idx];
        if (!(e & D_VALID)) {
            if (!create) return nullptr;
            paddr_t n = pmm_alloc_zeroed(0);
            if (!n) return nullptr;
            e = n | D_VALID | D_TABLE;
            table[idx] = e;
        } else if (!(e & D_TABLE)) {
            return level == 2 ? &table[idx] : nullptr;    /* 2 MiB block */
        }
        table = PHYS_TO_VIRT(e & D_ADDR);
    }
    return &table[(va >> 12) & 511];
}

static inline void tlb_flush_va(vaddr_t va) {
    __asm__ volatile("dsb ishst; tlbi vaae1is, %0; dsb ish; isb" :: "r"((va >> 12) & 0xfffffffffffULL) : "memory");
}
static inline void tlb_flush_all(void) { __asm__ volatile("dsb ishst; tlbi vmalle1is; dsb ish; isb" ::: "memory"); }

void vmm_flush(vaddr_t va) { tlb_flush_va(va); }

int vmm_map(pagetable_t pt, vaddr_t va, paddr_t pa, unsigned flags) {
    uint64_t f = pt_lock_irqsave();
    uint64_t *pte = walk(pt.root, va, true);
    if (!pte) { spin_unlock_irqrestore(&pt_lock, f); return -ENOMEM; }
    *pte = to_pte(pa, flags);
    if (!tlb_batched(va)) tlb_flush_va(va);
    spin_unlock_irqrestore(&pt_lock, f);
    return 0;
}

static int map_2m(paddr_t root, vaddr_t va, paddr_t pa, unsigned flags) {
    uint64_t *table = PHYS_TO_VIRT(root_for(root, va));
    for (int level = 0; level < 2; level++) {
        unsigned idx = (va >> (39 - 9 * level)) & 511;
        if (!(table[idx] & D_VALID)) {
            paddr_t n = pmm_alloc_zeroed(0);
            if (!n) return -ENOMEM;
            table[idx] = n | D_VALID | D_TABLE;
        }
        table = PHYS_TO_VIRT(table[idx] & D_ADDR);
    }
    table[(va >> 21) & 511] = (pa & D_ADDR) | attrs(flags);   /* block descriptor */
    return 0;
}

int vmm_map_range(pagetable_t pt, vaddr_t va, paddr_t pa, size_t len, unsigned flags) {
    size_t off = 0;
    while (off < len) {
        if (!(flags & VM_USER) && ((va + off) & 0x1fffff) == 0 && ((pa + off) & 0x1fffff) == 0 &&
            len - off >= 0x200000) {
            if (map_2m(pt.root, va + off, pa + off, flags)) return -ENOMEM;
            off += 0x200000;
        } else {
            if (vmm_map(pt, va + off, pa + off, flags)) return -ENOMEM;
            off += PAGE_SIZE;
        }
    }
    tlb_flush_all();
    return 0;
}

paddr_t vmm_unmap(pagetable_t pt, vaddr_t va) {
    uint64_t f = pt_lock_irqsave();
    uint64_t *pte = walk(pt.root, va, false);
    paddr_t old = 0;
    if (pte && (*pte & D_VALID)) { old = *pte & D_ADDR; *pte = 0; if (!tlb_batched(va)) tlb_flush_va(va); }
    spin_unlock_irqrestore(&pt_lock, f);
    return old;
}

bool vmm_query(pagetable_t pt, vaddr_t va, paddr_t *pa, unsigned *flags) {
    uint64_t *pte = walk(pt.root, va, false);
    if (!pte || !(*pte & D_VALID)) return false;
    if (pa) *pa = (*pte & D_ADDR) | (va & 0xfff);
    if (flags) {
        unsigned fl = VM_READ;
        if (!(*pte & D_AP_RO)) fl |= VM_WRITE;
        if (*pte & D_AP_EL0) { fl |= VM_USER; if (!(*pte & D_UXN)) fl |= VM_EXEC; }
        else if (!(*pte & D_PXN)) fl |= VM_EXEC;
        *flags = fl;
    }
    return true;
}

int vmm_protect(pagetable_t pt, vaddr_t va, unsigned flags) {
    uint64_t *pte = walk(pt.root, va, false);
    if (!pte || !(*pte & D_VALID)) return -EFAULT;
    *pte = to_pte(*pte & D_ADDR, flags);
    if (!tlb_batched(va)) tlb_flush_va(va);
    return 0;
}

void *vmm_map_mmio(paddr_t pa, size_t len) {
    paddr_t base = ALIGN_DOWN(pa, PAGE_SIZE);
    size_t l = ALIGN_UP(pa + len, PAGE_SIZE) - base;
    for (size_t off = 0; off < l; off += PAGE_SIZE) {
        paddr_t cur;
        if (vmm_query(kernel_pt, (vaddr_t)PHYS_TO_VIRT(base + off), &cur, nullptr)) continue;
        vmm_map(kernel_pt, (vaddr_t)PHYS_TO_VIRT(base + off), base + off, VM_READ | VM_WRITE | VM_NOCACHE);
    }
    return PHYS_TO_VIRT(pa);
}

pagetable_t vmm_new_user_pagetable(void) { return (pagetable_t){ pmm_alloc_zeroed(0) }; }

static void free_level(paddr_t table, int level) {
    uint64_t *t = PHYS_TO_VIRT(table);
    for (int i = 0; i < 512; i++) {
        if (!(t[i] & D_VALID) || !(t[i] & D_TABLE)) continue;
        if (level < 2) free_level(t[i] & D_ADDR, level + 1);
        else pmm_free_pages(t[i] & D_ADDR, 0);      /* level-3 table page */
    }
    pmm_free_pages(table, 0);
}
void vmm_free_user_pagetable(pagetable_t pt) { free_level(pt.root, 0); }

void arch_tlb_flush_local(void) { __asm__ volatile("dsb ishst; tlbi vmalle1; dsb ish; isb" ::: "memory"); }
/* batched flush: one broadcast invalidation covers every CPU */
void arch_tlb_flush_all_cpus(void) { tlb_flush_all(); }
/* TLBI ...IS instructions already broadcast to every CPU in the inner-shareable domain */
void arch_tlb_remote(uint64_t mask, vaddr_t va) { if (va == ~0UL) tlb_flush_all(); }

static uint64_t bsp_mair, bsp_tcr;
/* AP: adopt the boot CPU's translation setup */
void a64_ap_mmu_init(void) {
    sysreg_write(mair_el1, bsp_mair);
    sysreg_write(tcr_el1, bsp_tcr);
    isb();
    __asm__ volatile("dsb ishst" ::: "memory");
    sysreg_write(ttbr1_el1, kernel_pt.root);
    sysreg_write(ttbr0_el1, empty_root);
    isb();
    __asm__ volatile("tlbi vmalle1; dsb nsh; isb" ::: "memory");
    this_cpu()->active_root = kernel_pt.root;
}

void vmm_switch(pagetable_t pt) {
    this_cpu()->active_root = pt.root;
    paddr_t root = pt.root == kernel_pt.root ? empty_root : pt.root;
    if ((sysreg_read(ttbr0_el1) & D_ADDR) != root) {
        sysreg_write(ttbr0_el1, root);
        isb();
        tlb_flush_all();
    }
}

extern char __kernel_start[], __text_start[], __text_end[], __rodata_start[], __rodata_end[],
            __data_start[], __kernel_end[];

static int find_attr(uint64_t mair, uint8_t want) {
    for (int i = 0; i < 8; i++) if (((mair >> (8 * i)) & 0xff) == want) return i;
    return -1;
}

void vmm_init(void) {
    /* Base revision >= 4: Attr0 = normal WB, Attr1 = framebuffer type, rest unused.
       Program device (nGnRnE) and normal non-cacheable into free slots. */
    uint64_t mair = sysreg_read(mair_el1);
    int n = find_attr(mair, 0xff);
    if (n < 0) panic("vmm: MAIR %lx has no normal WB attribute", mair);
    attr_normal = n;
    attr_device = 6; attr_wc = 7;
    mair = (mair & ~(0xffffULL << 48)) | (0x00ULL << 48) | (0x44ULL << 56);
    sysreg_write(mair_el1, mair);
    uint64_t tcr = sysreg_read(tcr_el1);
    sysreg_write(tcr_el1, tcr & ~(1ULL << 7));               /* make sure TTBR0 walks are enabled */
    isb();

    kernel_pt.root = pmm_alloc_zeroed(0);
    empty_root = pmm_alloc_zeroed(0);
    uint64_t *root = PHYS_TO_VIRT(kernel_pt.root);
    for (int i = 256; i < 512; i++) root[i] = pmm_alloc_zeroed(0) | D_VALID | D_TABLE;

    struct limine_memmap_response *mm = boot_memmap();
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_BAD_MEMORY || e->type == LIMINE_MEMMAP_RESERVED) continue;
        paddr_t base = ALIGN_DOWN(e->base, PAGE_SIZE);
        paddr_t end = ALIGN_UP(e->base + e->length, PAGE_SIZE);
        unsigned fl = VM_READ | VM_WRITE;
        if (e->type == LIMINE_MEMMAP_FRAMEBUFFER) fl |= VM_WC;
        vmm_map_range(kernel_pt, (vaddr_t)PHYS_TO_VIRT(base), base, end - base, fl);
    }

    struct limine_executable_address_response *ka = boot_kernel_address();
    uint64_t vbase = ka->virtual_base, pbase = ka->physical_base;
#define KMAP(s, e, fl) vmm_map_range(kernel_pt, (vaddr_t)(s), (paddr_t)(s) - vbase + pbase, \
                                     ALIGN_UP((uint64_t)(e), PAGE_SIZE) - (uint64_t)(s), fl)
    KMAP(__kernel_start, __text_start, VM_READ);
    KMAP(__text_start, __text_end, VM_READ | VM_EXEC);
    KMAP(__rodata_start, __rodata_end, VM_READ);
    KMAP(__data_start, __kernel_end, VM_READ | VM_WRITE);

    __asm__ volatile("dsb ishst" ::: "memory");
    sysreg_write(ttbr1_el1, kernel_pt.root);
    sysreg_write(ttbr0_el1, empty_root);
    isb();
    tlb_flush_all();
    bsp_mair = sysreg_read(mair_el1);
    bsp_tcr = sysreg_read(tcr_el1);
    pr_info("vmm: TTBR1 kernel tables active (root %lx), MAIR %lx\n", kernel_pt.root, mair);
}
