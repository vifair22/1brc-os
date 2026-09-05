#include "efi/loader.h"

#include "console/console.h"
#include "console/fmt.h"
#include "efi/blockio.h"
#include "kernel/bootinfo.h"
#include "kernel/cpu.h"
#include "lib/string.h"

/* kprintf works before console_init: bytes go to debugcon only, which
 * is exactly right for loader-phase logging (ConOut is for the one
 * sign-of-life line a human might see on a real screen).
 */

static efi_system_table *st;
static boot_info bi;

/* Takes non-const char16 because the spec's OutputString does; C's
 * u"" literals are non-const arrays, so callers pass them directly. */
static void efi_puts(char16 *s)
{
    st->ConOut->OutputString(st->ConOut, s);
}

_Noreturn static void fail(char16 *msg, efi_status status)
{
    efi_puts(u"loader: FAIL: ");
    efi_puts(msg);
    efi_puts(u"\r\n");
    kprintf("loader: FAIL status=%llx\n", status);
    for (;;)
        st->BootServices->Stall(1000000);
}

static efi_status find_gop(fb_info *fb)
{
    efi_guid gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    efi_graphics_output *gop;
    efi_status s;

    s = st->BootServices->LocateProtocol(&gop_guid, 0, (void **)&gop);
    if (EFI_IS_ERROR(s))
        return s;

    const efi_gop_mode_info *info = gop->Mode->Info;
    if (info->PixelFormat != EFI_PIXEL_RGB_RESERVED_8BPC &&
        info->PixelFormat != EFI_PIXEL_BGR_RESERVED_8BPC)
        return EFI_NOT_FOUND;

    fb->base = gop->Mode->FrameBufferBase;
    fb->width = info->HorizontalResolution;
    fb->height = info->VerticalResolution;
    fb->stride = info->PixelsPerScanLine;
    fb->format = info->PixelFormat;
    return EFI_SUCCESS;
}

/* Boot-services Stall is backed by a real timer, so bracketing one long
 * Stall with rdtsc gives the TSC frequency to within a few percent;
 * plenty for bandwidth and wall-clock reporting. */
static uint64_t calibrate_tsc(void)
{
    uint64_t t0 = rdtsc();
    st->BootServices->Stall(100000); /* 100 ms */
    return (rdtsc() - t0) * 10;
}

static void *find_acpi_rsdp(void)
{
    efi_guid acpi = EFI_ACPI_20_TABLE_GUID;

    for (uint64_t i = 0; i < st->NumberOfTableEntries; i++) {
        efi_configuration_table *t = &st->ConfigurationTable[i];
        if (memcmp(&t->VendorGuid, &acpi, sizeof(acpi)) == 0)
            return t->VendorTable;
    }
    return 0;
}

/* The GetMemoryMap/ExitBootServices dance. Between the two calls
 * nothing may allocate, and a failed ExitBootServices leaves only
 * GetMemoryMap and ExitBootServices callable, hence the retry loop
 * reusing the same (deliberately oversized) buffer.
 */
static efi_status exit_boot_services(efi_handle image)
{
    efi_boot_services *bs = st->BootServices;
    uint64_t size = 0, key = 0, dsize = 0;
    uint32_t dver = 0;
    void *buf = 0;
    efi_status s;

    s = bs->GetMemoryMap(&size, 0, &key, &dsize, &dver);
    if (s != EFI_BUFFER_TOO_SMALL)
        return s;
    size += 8 * dsize;
    s = bs->AllocatePool(EFI_LOADER_DATA, size, &buf);
    if (EFI_IS_ERROR(s))
        return s;

    for (;;) {
        uint64_t sz = size;
        s = bs->GetMemoryMap(&sz, buf, &key, &dsize, &dver);
        if (EFI_IS_ERROR(s))
            return s;
        s = bs->ExitBootServices(image, key);
        if (!EFI_IS_ERROR(s)) {
            bi.memmap = buf;
            bi.memmap_size = sz;
            bi.desc_size = dsize;
            bi.desc_ver = dver;
            return EFI_SUCCESS;
        }
        if (s != EFI_INVALID_PARAMETER)
            return s;
    }
}

static uint64_t count_conventional(void)
{
    uint64_t total = 0;

    for (uint64_t off = 0; off < bi.memmap_size; off += bi.desc_size) {
        const efi_memory_descriptor *d =
            (const efi_memory_descriptor *)((uintptr_t)bi.memmap + off);
        if (d->Type == EFI_CONVENTIONAL_MEMORY)
            total += d->NumberOfPages * 4096;
    }
    return total;
}

efi_status efi_main(efi_handle image, efi_system_table *systab)
{
    efi_status s;

    st = systab;

    /* Call number one: firmware arms a 5-minute watchdog by default,
     * and the dataset load will eventually outlive it. */
    st->BootServices->SetWatchdogTimer(0, 0, 0, 0);

    efi_puts(u"1brc-os: loader up\r\n");
    kprintf("loader: up\n");

    s = find_gop(&bi.fb);
    if (EFI_IS_ERROR(s))
        fail(u"no usable GOP framebuffer", s);
    kprintf("loader: gop %ux%u stride %u fmt %u base %llx\n", bi.fb.width,
            bi.fb.height, bi.fb.stride, bi.fb.format, bi.fb.base);

    bi.acpi_rsdp = find_acpi_rsdp();
    kprintf("loader: acpi rsdp %p\n", bi.acpi_rsdp);

    bi.tsc_hz = calibrate_tsc();
    kprintf("loader: tsc %llu MHz\n", bi.tsc_hz / 1000000);

    s = blockio_load_dataset(st, &bi);
    if (EFI_IS_ERROR(s))
        fail(u"dataset not found on any block device", s);
    kprintf("loader: dataset %llu bytes @ %p\n", bi.dataset_bytes,
            bi.dataset);

    s = exit_boot_services(image);
    if (EFI_IS_ERROR(s))
        fail(u"exit_boot_services", s);

    /* Firmware is gone. The machine is ours. */
    bi.conventional_bytes = count_conventional();
    kmain(&bi);
}
