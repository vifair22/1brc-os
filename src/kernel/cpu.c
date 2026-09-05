#include "kernel/cpu.h"

/* This translation unit is compiled WITHOUT -mavx2: it must execute
 * before XCR0 is programmed, so nothing here may emit VEX instructions.
 */

#define MSR_AMD_HWCR 0xc0010015
#define HWCR_CPB_DIS (1ULL << 25)
#define MSR_AMD_PSTATE_CTL 0xc0010062
#define MSR_IA32_MPERF 0xe7
#define MSR_IA32_APERF 0xe8

static inline void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a,
                         uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(sub));
}

int cpu_enable_simd(void)
{
    uint32_t a, b, c, d;

    cpuid(1, 0, &a, &b, &c, &d);
    if (!(c & (1u << 26)) || !(c & (1u << 28)))
        return -1; /* no XSAVE or no AVX */
    cpuid(7, 0, &a, &b, &c, &d);
    if (!(b & (1u << 5)))
        return -1; /* no AVX2 */

    /* CR4: OSFXSR + OSXMMEXCPT (SSE state, firmware usually did this
     * already) and OSXSAVE (unlocks XSETBV). Then XCR0 x87|SSE|AVX so
     * VEX-encoded instructions stop faulting #UD. Runs per core: APs
     * arrive with pristine control registers. */
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1u << 9) | (1u << 10) | (1u << 18);
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
    __asm__ volatile("xsetbv" : : "a"(7), "d"(0), "c"(0));
    return 0;
}

static int is_amd_bare_metal(void)
{
    uint32_t a, b, c, d;

    cpuid(1, 0, &a, &b, &c, &d);
    if (c & (1u << 31)) /* hypervisor present */
        return 0;
    cpuid(0, 0, &a, &b, &c, &d);
    return b == 0x68747541 && d == 0x69746e65 && c == 0x444d4163;
}

void cpu_boost(void)
{
    if (!is_amd_bare_metal())
        return;
    uint64_t hwcr = rdmsr(MSR_AMD_HWCR);
    if (hwcr & HWCR_CPB_DIS)
        wrmsr(MSR_AMD_HWCR, hwcr & ~HWCR_CPB_DIS);
    wrmsr(MSR_AMD_PSTATE_CTL, 0); /* request P0 */
}

int cpu_has_aperf(void)
{
    uint32_t a, b, c, d;
    cpuid(6, 0, &a, &b, &c, &d);
    return (int)(c & 1);
}

uint64_t cpu_aperf(void)
{
    return rdmsr(MSR_IA32_APERF);
}

uint64_t cpu_mperf(void)
{
    return rdmsr(MSR_IA32_MPERF);
}
