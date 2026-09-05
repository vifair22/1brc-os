#include "kernel/alloc.h"

#include "console/fmt.h"
#include "efi/efi.h"
#include "lib/string.h"

static struct {
    uint8_t *base;
    uint64_t size;
    uint64_t off;
} pool;

void alloc_init(const boot_info *bi)
{
    uint64_t best_base = 0, best_size = 0;

    for (uint64_t off = 0; off < bi->memmap_size; off += bi->desc_size) {
        const efi_memory_descriptor *d =
            (const efi_memory_descriptor *)((uintptr_t)bi->memmap + off);
        uint64_t bytes = d->NumberOfPages * 4096;
        /* Below 1 MiB stays free for the SMP trampoline's real-mode
         * startup page. */
        if (d->Type != EFI_CONVENTIONAL_MEMORY || d->PhysicalStart < 0x100000)
            continue;
        if (bytes > best_size) {
            best_base = d->PhysicalStart;
            best_size = bytes;
        }
    }

    pool.base = (uint8_t *)best_base;
    pool.size = best_size;
    pool.off = 0;
    kprintf("alloc: pool %llu MiB @ %p\n", best_size >> 20, pool.base);
}

void *kalloc(uint64_t size, uint64_t align)
{
    uint64_t off = (pool.off + align - 1) & ~(align - 1);

    if (off + size > pool.size) {
        kprintf("alloc: OOM (%llu wanted, %llu left)\n", size,
                pool.size - pool.off);
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    pool.off = off + size;
    return pool.base + off;
}

void *kzalloc(uint64_t size, uint64_t align)
{
    void *p = kalloc(size, align);
    memset(p, 0, size);
    return p;
}

uint64_t alloc_remaining(void)
{
    return pool.size - pool.off;
}
