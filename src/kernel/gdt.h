/* Kernel GDT: the same three descriptors the AP trampoline uses (null,
 * code64 at 0x08, data at 0x10), owned by the kernel so the BSP stops
 * running on firmware descriptors and the IDT has a known selector.
 */
#ifndef KERNEL_GDT_H
#define KERNEL_GDT_H

#define GDT_SEL_CODE 0x08
#define GDT_SEL_DATA 0x10

/* Loads the kernel GDT and reloads CS/SS/DS on the calling core. */
void gdt_load(void);

#endif
