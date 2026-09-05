/* Bump allocator over the largest EfiConventionalMemory region. There
 * is no free: everything allocated lives until the machine halts, which
 * is the lifetime of everything in this OS anyway.
 *
 * Only type-7 (conventional) memory is used. BootServicesData is
 * technically reclaimable after ExitBootServices but the BSP is still
 * running on a firmware-allocated stack that lives there, so it stays
 * off limits.
 */
#ifndef KERNEL_ALLOC_H
#define KERNEL_ALLOC_H

#include <stdint.h>

#include "kernel/bootinfo.h"

void alloc_init(const boot_info *bi);
void *kalloc(uint64_t size, uint64_t align);
void *kzalloc(uint64_t size, uint64_t align); /* zeroed */
uint64_t alloc_remaining(void);

#endif
