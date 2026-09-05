#include "kernel/gdt.h"

#include <stdint.h>

static const uint64_t gdt[3] __attribute__((aligned(16))) = {
    0x0000000000000000ULL, /* null */
    0x00209a0000000000ULL, /* 0x08: 64-bit code */
    0x0000920000000000ULL, /* 0x10: data */
};

void gdt_load(void)
{
    struct __attribute__((packed)) {
        uint16_t limit;
        uint64_t base;
    } gdtr = { sizeof(gdt) - 1, (uint64_t)(uintptr_t)gdt };

    /* CS can only change via a far transfer; push the new selector and
     * a rip and lretq into it. Data segments reload normally. */
    __asm__ volatile("lgdt %0\n\t"
                     "pushq %1\n\t"
                     "leaq 1f(%%rip), %%rax\n\t"
                     "pushq %%rax\n\t"
                     "lretq\n\t"
                     "1:\n\t"
                     "movl %2, %%eax\n\t"
                     "movl %%eax, %%ds\n\t"
                     "movl %%eax, %%es\n\t"
                     "movl %%eax, %%ss\n\t"
                     :
                     : "m"(gdtr), "i"((uint64_t)GDT_SEL_CODE),
                       "i"(GDT_SEL_DATA)
                     : "rax", "memory");
}
