#include "kernel/acpi.h"

#include "console/fmt.h"
#include "lib/string.h"

/* ACPI 2.0+ RSDP; only the XSDT pointer matters. */
typedef struct {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} __attribute__((packed)) acpi_rsdp;

typedef struct {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_header;

/* MADT interrupt-controller entry types this walk understands. */
#define MADT_LAPIC 0
#define MADT_LAPIC_ADDR_OVERRIDE 5
#define MADT_X2APIC 9

#define MADT_CPU_ENABLED 1u

int acpi_enumerate_cpus(const void *rsdp_ptr, cpu_topology *topo)
{
    const acpi_rsdp *rsdp = rsdp_ptr;

    memset(topo, 0, sizeof(*topo));
    if (!rsdp || memcmp(rsdp->signature, "RSD PTR ", 8) != 0 ||
        rsdp->revision < 2 || !rsdp->xsdt_address)
        return -1;

    const acpi_sdt_header *xsdt =
        (const acpi_sdt_header *)(uintptr_t)rsdp->xsdt_address;
    if (memcmp(xsdt->signature, "XSDT", 4) != 0)
        return -1;

    const acpi_sdt_header *madt = 0;
    uint32_t entries = (xsdt->length - (uint32_t)sizeof(*xsdt)) / 8;
    for (uint32_t i = 0; i < entries; i++) {
        uint64_t addr;
        memcpy(&addr, (const uint8_t *)(xsdt + 1) + i * 8, 8);
        const acpi_sdt_header *h = (const acpi_sdt_header *)(uintptr_t)addr;
        if (memcmp(h->signature, "APIC", 4) == 0) {
            madt = h;
            break;
        }
    }
    if (!madt)
        return -1;

    const uint8_t *body = (const uint8_t *)madt;
    uint32_t lapic32;
    memcpy(&lapic32, body + 36, 4);
    topo->lapic_base = lapic32;

    /* Entry stream: [type u8][len u8][payload], len includes both. */
    uint32_t off = 44;
    while (off + 2 <= madt->length) {
        uint8_t type = body[off];
        uint8_t len = body[off + 1];
        if (len < 2 || off + len > madt->length)
            break;

        if (type == MADT_LAPIC && len >= 8) {
            uint32_t flags;
            memcpy(&flags, body + off + 4, 4);
            if ((flags & MADT_CPU_ENABLED) &&
                topo->count < ACPI_MAX_CPUS)
                topo->apic_ids[topo->count++] = body[off + 3];
        } else if (type == MADT_X2APIC && len >= 16) {
            uint32_t id, flags;
            memcpy(&id, body + off + 4, 4);
            memcpy(&flags, body + off + 8, 4);
            if ((flags & MADT_CPU_ENABLED) &&
                topo->count < ACPI_MAX_CPUS)
                topo->apic_ids[topo->count++] = id;
        } else if (type == MADT_LAPIC_ADDR_OVERRIDE && len >= 12) {
            memcpy(&topo->lapic_base, body + off + 4, 8);
        }
        off += len;
    }

    kprintf("acpi: %u cpus, lapic @ %llx\n", topo->count, topo->lapic_base);
    return topo->count ? 0 : -1;
}
