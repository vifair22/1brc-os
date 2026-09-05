/* The handoff contract between the efi/ phase and the kernel/ phase.
 * Everything the kernel needs to own the machine is gathered here by the
 * loader before ExitBootServices; after that call this struct is the
 * only source of truth about the hardware.
 */
#ifndef KERNEL_BOOTINFO_H
#define KERNEL_BOOTINFO_H

#include <stdint.h>

/* format matches EFI_GRAPHICS_PIXEL_FORMAT: 0 = red in byte 0,
 * 1 = blue in byte 0. Always 32 bits per pixel. */
typedef struct {
    uint64_t base;
    uint32_t width;
    uint32_t height;
    uint32_t stride; /* pixels per scanline, >= width */
    uint32_t format;
} fb_info;

typedef struct {
    fb_info fb;

    /* Final UEFI memory map, still in firmware layout: iterate by
     * desc_size, not sizeof(efi_memory_descriptor). */
    void *memmap;
    uint64_t memmap_size;
    uint64_t desc_size;
    uint32_t desc_ver;

    uint64_t conventional_bytes; /* total EfiConventionalMemory */

    /* ACPI 2.0 RSDP from the EFI configuration table, for MADT walks. */
    void *acpi_rsdp;

    /* Dataset payload, loaded whole into RAM by the loader. The 64
     * bytes after dataset_bytes are zeroed slack so SIMD tail reads
     * never leave mapped memory. */
    void *dataset;
    uint64_t dataset_bytes;
    uint32_t dataset_flags; /* DATASET_FLAG_* from the image header */

    uint64_t load_ticks; /* TSC ticks spent in the BlockIo read loop */
    uint64_t tsc_hz;     /* Stall-calibrated, good to a few percent */
} boot_info;

_Noreturn void kmain(boot_info *bi);

#endif
