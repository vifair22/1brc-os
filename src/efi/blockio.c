#include "efi/blockio.h"

#include "console/fmt.h"
#include "kernel/cpu.h"
#include "lib/string.h"

/* Firmware transfer-size limits are undocumented and vary, so reads go
 * out in fixed chunks. 4 MiB is small enough for every firmware seen in
 * the wild and large enough that per-call overhead disappears. */
#define READ_CHUNK_BYTES (4u << 20)

/* SIMD parse tail slack; see boot_info.dataset. */
#define TAIL_SLACK 64

static efi_status probe_device(efi_boot_services *bs, efi_block_io *io,
                               dataset_header *hdr)
{
    uint64_t buf = 0;
    efi_status s;
    uint32_t bsz = io->Media->BlockSize;

    /* One page holds the header for any sector size up to 4K, and a
     * page allocation satisfies every IoAlign in practice. */
    if (bsz == 0 || bsz > DATASET_HEADER_BYTES)
        return EFI_NOT_FOUND;
    s = bs->AllocatePages(EFI_ALLOCATE_ANY_PAGES, EFI_LOADER_DATA, 1, &buf);
    if (EFI_IS_ERROR(s))
        return s;

    s = io->ReadBlocks(io, io->Media->MediaId, 0, DATASET_HEADER_BYTES,
                       (void *)buf);
    if (!EFI_IS_ERROR(s)) {
        memcpy(hdr, (const void *)buf, sizeof(*hdr));
        if (memcmp(hdr->magic, DATASET_MAGIC, sizeof(DATASET_MAGIC)) != 0 ||
            hdr->version != 1)
            s = EFI_NOT_FOUND;
    }
    bs->FreePages(buf, 1);
    return s;
}

static efi_status read_payload(efi_system_table *st, efi_block_io *io,
                               const dataset_header *hdr, uint8_t *dst,
                               uint64_t *ticks)
{
    uint32_t bsz = io->Media->BlockSize;
    uint64_t lba = DATASET_HEADER_BYTES / bsz;
    uint64_t left = (hdr->payload_bytes + bsz - 1) / bsz * bsz;
    uint64_t done = 0, next_dot = 1ULL << 30;
    uint64_t t0 = rdtsc();

    while (left) {
        uint64_t chunk = left < READ_CHUNK_BYTES ? left : READ_CHUNK_BYTES;
        efi_status s =
            io->ReadBlocks(io, io->Media->MediaId, lba, chunk, dst);
        if (EFI_IS_ERROR(s))
            return s;
        lba += chunk / bsz;
        dst += chunk;
        left -= chunk;
        done += chunk;
        /* A dot per GiB on the firmware console: a 13.8 GB load takes
         * tens of seconds and a frozen screen reads as a hang. */
        if (done >= next_dot) {
            st->ConOut->OutputString(st->ConOut, u".");
            next_dot += 1ULL << 30;
        }
    }
    *ticks = rdtsc() - t0;
    return EFI_SUCCESS;
}

efi_status blockio_load_dataset(efi_system_table *st, boot_info *bi)
{
    efi_boot_services *bs = st->BootServices;
    efi_guid bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    efi_handle *handles = 0;
    uint64_t nhandles = 0;
    efi_status s;

    s = bs->LocateHandleBuffer(EFI_BY_PROTOCOL, &bio_guid, 0, &nhandles,
                               &handles);
    if (EFI_IS_ERROR(s))
        return s;
    kprintf("blockio: %llu handles\n", nhandles);

    efi_block_io *found = 0;
    dataset_header hdr;
    for (uint64_t i = 0; i < nhandles && !found; i++) {
        efi_block_io *io;
        s = bs->HandleProtocol(handles[i], &bio_guid, (void **)&io);
        if (EFI_IS_ERROR(s) || !io->Media->MediaPresent)
            continue;
        if (!EFI_IS_ERROR(probe_device(bs, io, &hdr)))
            found = io;
    }
    bs->FreePool(handles);
    if (!found)
        return EFI_NOT_FOUND;

    kprintf("blockio: dataset %llu bytes, device bsz %u lastlba %llu\n",
            hdr.payload_bytes, found->Media->BlockSize,
            found->Media->LastBlock);

    /* Payload rounds up to whole blocks during the read, so the
     * allocation covers that overhang plus the zeroed SIMD slack. */
    uint32_t bsz = found->Media->BlockSize;
    uint64_t read_bytes = (hdr.payload_bytes + bsz - 1) / bsz * bsz;
    uint64_t pages = (read_bytes + TAIL_SLACK + 4095) / 4096;
    uint64_t base = 0;

    s = bs->AllocatePages(EFI_ALLOCATE_ANY_PAGES, EFI_LOADER_DATA, pages,
                          &base);
    if (EFI_IS_ERROR(s)) {
        kprintf("blockio: AllocatePages(%llu pages) failed %llx\n", pages, s);
        return s;
    }

    st->ConOut->OutputString(st->ConOut, u"loading dataset ");
    s = read_payload(st, found, &hdr, (uint8_t *)base, &bi->load_ticks);
    st->ConOut->OutputString(st->ConOut, u"\r\n");
    if (EFI_IS_ERROR(s)) {
        bs->FreePages(base, pages);
        return s;
    }

    memset((uint8_t *)base + hdr.payload_bytes, 0,
           (read_bytes - hdr.payload_bytes) + TAIL_SLACK);

    bi->dataset = (void *)base;
    bi->dataset_bytes = hdr.payload_bytes;
    bi->dataset_flags = hdr.flags;
    return EFI_SUCCESS;
}
