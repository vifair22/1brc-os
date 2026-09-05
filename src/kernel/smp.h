/* SMP bringup and work dispatch. There is no scheduler: every AP boots
 * into a spin loop and smp_run hands all cores (BSP included) the same
 * function to run to completion. That is the entire threading model of
 * this OS.
 */
#ifndef KERNEL_SMP_H
#define KERNEL_SMP_H

#include <stdint.h>

#include "kernel/bootinfo.h"

typedef void (*smp_fn)(uint32_t cpu_index, void *arg);

/* Wakes every AP via INIT-SIPI-SIPI and parks them. Returns the number
 * of cores online including the BSP (>= 1; falls back to BSP-only if
 * ACPI or bringup fails). */
uint32_t smp_start_all(const boot_info *bi);

uint32_t smp_cpu_count(void);

/* APIC id of a given cpu_index (0 = BSP). On SMT parts consecutive
 * APIC ids are sibling threads of one core, so an even id selects one
 * thread per physical core. */
uint32_t smp_apic_id(uint32_t cpu_index);

/* Runs fn(cpu_index, arg) on every online core; cpu_index 0 is the
 * calling BSP. Returns when all cores have finished. */
void smp_run(smp_fn fn, void *arg);

#endif
