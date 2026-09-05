/* Dataset load over EFI_BLOCK_IO_PROTOCOL. The dataset lives raw on a
 * block device (partition or whole disk) behind a one-block header, so
 * no filesystem is involved on either side; tools/mkdataimg.py writes
 * the same layout from the host.
 */
#ifndef EFI_BLOCKIO_H
#define EFI_BLOCKIO_H

#include "efi/efi.h"
#include "kernel/bootinfo.h"

#define DATASET_MAGIC "1BRC-OS-DATASET"

/* Header occupies the first 4096 bytes of the device so the payload
 * starts LBA-aligned for both 512- and 4096-byte sectors. */
#define DATASET_HEADER_BYTES 4096

/* flags bit 0: quiet - suppress the full challenge line at the end and
 * print its length + FNV-1a hash instead. Set by mkdataimg.py --quiet;
 * meant for big datasets where 10k stations of output is just noise. */
#define DATASET_FLAG_QUIET 1u

typedef struct {
    char magic[16]; /* DATASET_MAGIC, NUL-padded */
    uint32_t version;
    uint32_t flags;
    uint64_t payload_bytes;
} dataset_header;

/* Scans all block devices for the header, loads the payload into
 * allocated pages, and fills bi->dataset*, bi->load_ticks. */
efi_status blockio_load_dataset(efi_system_table *st, boot_info *bi);

#endif
