/* Minimal ACPI: walk RSDP -> XSDT -> MADT and enumerate the local
 * APICs. That is the only fact this OS needs from ACPI: which cores
 * exist and where the LAPIC lives.
 */
#ifndef KERNEL_ACPI_H
#define KERNEL_ACPI_H

#include <stdint.h>

#define ACPI_MAX_CPUS 64

typedef struct {
    uint32_t apic_ids[ACPI_MAX_CPUS]; /* enabled cores, MADT order */
    uint32_t count;
    uint64_t lapic_base; /* xAPIC MMIO base */
} cpu_topology;

/* Returns 0 and fills topo, or -1 on a missing/garbled table chain. */
int acpi_enumerate_cpus(const void *rsdp, cpu_topology *topo);

#endif
