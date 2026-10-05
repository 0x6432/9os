#pragma once
/* RISC-V (RV64, S-mode) CPU helpers. */
#include <kernel/types.h>

#define csr_read(csr) ({ uint64_t __v; __asm__ volatile("csrr %0, " #csr : "=r"(__v) :: "memory"); __v; })
#define csr_write(csr, v) __asm__ volatile("csrw " #csr ", %0" :: "rK"((uint64_t)(v)) : "memory")
#define csr_set(csr, v) __asm__ volatile("csrs " #csr ", %0" :: "rK"((uint64_t)(v)) : "memory")
#define csr_clear(csr, v) __asm__ volatile("csrc " #csr ", %0" :: "rK"((uint64_t)(v)) : "memory")

#define SSTATUS_SIE  (1UL << 1)
#define SSTATUS_SPIE (1UL << 5)
#define SSTATUS_SPP  (1UL << 8)
#define SSTATUS_FS   (3UL << 13)
#define SSTATUS_FS_INITIAL (1UL << 13)
#define SSTATUS_SUM  (1UL << 18)
#define SSTATUS_MXR  (1UL << 19)

#define SIE_SSIE (1UL << 1)
#define SIE_STIE (1UL << 5)
#define SIE_SEIE (1UL << 9)

static inline uint64_t rdtime(void) { uint64_t v; __asm__ volatile("rdtime %0" : "=r"(v)); return v; }
static inline void sfence_vma(uint64_t va) { __asm__ volatile("sfence.vma %0, zero" :: "r"(va) : "memory"); }
static inline void sfence_vma_all(void) { __asm__ volatile("sfence.vma" ::: "memory"); }

static inline uint64_t arch_irq_save(void) {
    uint64_t f;
    __asm__ volatile("csrrci %0, sstatus, 2" : "=r"(f) :: "memory");
    return f;
}
static inline void arch_irq_restore(uint64_t f) { if (f & SSTATUS_SIE) __asm__ volatile("csrsi sstatus, 2" ::: "memory"); }
static inline void arch_irq_enable(void) { __asm__ volatile("csrsi sstatus, 2" ::: "memory"); }
static inline void arch_irq_disable(void) { __asm__ volatile("csrci sstatus, 2" ::: "memory"); }
static inline bool arch_irq_enabled(void) { return csr_read(sstatus) & SSTATUS_SIE; }
static inline void arch_cpu_relax(void) { __asm__ volatile("nop"); }
/* wfi with interrupts enabled: a pending interrupt wakes the hart and is taken right after. */
/* WFI wakes for locally enabled pending IRQs even while global SIE is clear.
 * Keep SIE clear until after WFI, so an IPI cannot be handled and consumed in
 * the enable-to-WFI gap with the AP's timer disabled. */
static inline void arch_wait_for_interrupt(void) { __asm__ volatile("wfi; csrsi sstatus, 2" ::: "memory"); }

/* SBI calls */
struct sbiret { long error, value; };
static inline struct sbiret sbi_call(long ext, long fid, long a0, long a1, long a2) {
    register long r0 __asm__("a0") = a0, r1 __asm__("a1") = a1, r2 __asm__("a2") = a2;
    register long r6 __asm__("a6") = fid, r7 __asm__("a7") = ext;
    __asm__ volatile("ecall" : "+r"(r0), "+r"(r1) : "r"(r2), "r"(r6), "r"(r7) : "memory");
    return (struct sbiret){ r0, r1 };
}
static inline struct sbiret sbi_call4(long ext, long fid, long a0, long a1, long a2, long a3) {
    register long r0 __asm__("a0") = a0, r1 __asm__("a1") = a1, r2 __asm__("a2") = a2, r3 __asm__("a3") = a3;
    register long r6 __asm__("a6") = fid, r7 __asm__("a7") = ext;
    __asm__ volatile("ecall" : "+r"(r0), "+r"(r1) : "r"(r2), "r"(r3), "r"(r6), "r"(r7) : "memory");
    return (struct sbiret){ r0, r1 };
}
