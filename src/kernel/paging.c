#include "kernel/paging.h"

#include "console/fmt.h"
#include "kernel/alloc.h"

#define PTE_P (1ULL << 0)
#define PTE_W (1ULL << 1)
#define PTE_PS (1ULL << 7)

static uint64_t fw_cr3;
static uint64_t kernel_cr3;

uint64_t paging_firmware_cr3(void)
{
    return fw_cr3;
}

void paging_activate(void)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");
}

int paging_init(void)
{
    __asm__ volatile("mov %%cr3, %0" : "=r"(fw_cr3));
    kernel_cr3 = fw_cr3;

    uint32_t a, b, c, d;
    __asm__ volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(0x80000001), "c"(0));
    if (!(d & (1u << 26))) {
        kprintf("paging: no 1 GiB pages, keeping firmware tables\n");
        return -1;
    }

    /* One PML4 entry -> one PDPT of 512 huge pages = 512 GiB identity.
     * Covers all RAM (dataset included), the framebuffer aperture and
     * the LAPIC window on this class of machine; MTRRs still make MMIO
     * regions uncacheable, so plain WB entries are correct everywhere.
     * The tables themselves live in the identity map, so their virtual
     * and physical addresses agree by construction. */
    uint64_t *pml4 = kzalloc(4096, 4096);
    uint64_t *pdpt = kzalloc(4096, 4096);
    for (uint64_t i = 0; i < 512; i++)
        pdpt[i] = (i << 30) | PTE_P | PTE_W | PTE_PS;
    pml4[0] = (uint64_t)(uintptr_t)pdpt | PTE_P | PTE_W;

    kernel_cr3 = (uint64_t)(uintptr_t)pml4;
    paging_activate();
    kprintf("paging: 512 GiB identity map, 1 GiB pages\n");
    return 0;
}
