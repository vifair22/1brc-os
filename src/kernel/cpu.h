/* CPU primitives shared by both phases. Grows per-core AVX/XCR0 setup
 * when SMP lands.
 */
#ifndef KERNEL_CPU_H
#define KERNEL_CPU_H

#include <stdint.h>

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t v)
{
    __asm__ volatile("wrmsr"
                     :
                     : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static inline void cpu_pause(void)
{
    __asm__ volatile("pause");
}

static inline void tsc_delay_us(uint64_t us, uint64_t tsc_hz)
{
    uint64_t end = rdtsc() + us * (tsc_hz / 1000000);
    while (rdtsc() < end)
        cpu_pause();
}

/* Enables SSE+AVX state on the calling core (CR4 + XCR0). Must run on
 * every core before it touches AVX2 code. Returns -1 if the CPU lacks
 * XSAVE/AVX/AVX2. */
int cpu_enable_simd(void);

/* Asks the calling core for full speed: enables Core Performance Boost
 * (HWCR.CpbDis = 0) and requests P-state 0. There is no OS governor
 * here, so nobody else will. AMD-only, skipped under a hypervisor
 * (KVM #GPs on these MSRs). */
void cpu_boost(void);

/* Effective-frequency accounting via APERF/MPERF, when the CPU exposes
 * it (CPUID.6 ECX[0]; hypervisors usually hide it). Returns 0 when
 * unsupported. */
int cpu_has_aperf(void);
uint64_t cpu_aperf(void);
uint64_t cpu_mperf(void);

#endif
