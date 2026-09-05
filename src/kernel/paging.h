/* Kernel page tables: a 512 GiB identity map built from 1 GiB huge
 * pages. Firmware tables are functionally fine but may map RAM with
 * small pages; 32 cores streaming 13.8 GB through 4 KiB TLB entries is
 * measurable pain, and one PDPT of huge pages ends it.
 */
#ifndef KERNEL_PAGING_H
#define KERNEL_PAGING_H

#include <stdint.h>

/* BSP: saves the firmware CR3, builds the map, switches. Falls back to
 * staying on firmware tables (returns -1) if the CPU lacks 1 GiB
 * pages. */
int paging_init(void);

/* APs: switch to whatever paging_init decided (kernel tables, or
 * firmware's on fallback). */
void paging_activate(void);

/* The CR3 the firmware handed us; the AP trampoline must start on
 * this, because it loads CR3 as a 32-bit value and the kernel tables
 * may sit above 4 GiB. */
uint64_t paging_firmware_cr3(void);

#endif
