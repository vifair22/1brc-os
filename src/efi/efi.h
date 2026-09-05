/* Handmade UEFI definitions: just the types and protocols this project
 * touches, transcribed from the UEFI 2.10 specification. No gnu-efi, no
 * EDK2. Struct member order is ABI: every unused table entry is a void*
 * placeholder holding the slot so the members we do call line up.
 *
 * Naming: snake_case type names (project convention), CamelCase members
 * (spec convention, so fields can be cross-checked against the UEFI spec
 * verbatim).
 *
 * The whole tree compiles with --target=x86_64-unknown-windows, so the
 * MS x64 calling convention UEFI requires is the default; no EFIAPI
 * attribute is needed. Beware LLP64: long is 32-bit on this target,
 * which is why everything here uses stdint types. UINTN is uint64_t.
 */
#ifndef EFI_EFI_H
#define EFI_EFI_H

#include <stdint.h>

typedef uint64_t efi_status;
typedef void *efi_handle;
typedef uint16_t char16;

typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
} efi_guid;

/* Status codes: errors carry the high bit. */
#define EFI_SUCCESS 0ULL
#define EFI_ERROR_BIT 0x8000000000000000ULL
#define EFI_INVALID_PARAMETER (EFI_ERROR_BIT | 2)
#define EFI_BUFFER_TOO_SMALL (EFI_ERROR_BIT | 5)
#define EFI_NOT_FOUND (EFI_ERROR_BIT | 14)
#define EFI_IS_ERROR(s) (((s) & EFI_ERROR_BIT) != 0)

typedef struct {
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
} efi_table_header;

/* ---- Simple text output (pre-ExitBootServices prints only) ---- */

typedef struct efi_simple_text_output efi_simple_text_output;
struct efi_simple_text_output {
    void *Reset;
    efi_status (*OutputString)(efi_simple_text_output *This, char16 *String);
    void *TestString;
    void *QueryMode;
    void *SetMode;
    void *SetAttribute;
    efi_status (*ClearScreen)(efi_simple_text_output *This);
    void *SetCursorPosition;
    void *EnableCursor;
    void *Mode;
};

/* ---- Memory map ---- */

/* EFI_MEMORY_TYPE values this project cares about. */
#define EFI_LOADER_DATA 2
#define EFI_CONVENTIONAL_MEMORY 7

/* Iterate with the DescriptorSize returned by GetMemoryMap, never
 * sizeof: firmware may append fields beyond this definition. */
typedef struct {
    uint32_t Type;
    uint32_t Pad;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} efi_memory_descriptor;

/* EFI_ALLOCATE_TYPE */
#define EFI_ALLOCATE_ANY_PAGES 0
#define EFI_ALLOCATE_MAX_ADDRESS 1
#define EFI_ALLOCATE_ADDRESS 2

/* ---- Boot services ----
 * Full slot list from the spec; only the entries this project calls are
 * typed. */

typedef struct {
    efi_table_header Hdr;

    void *RaiseTPL;
    void *RestoreTPL;

    efi_status (*AllocatePages)(uint32_t Type, uint32_t MemoryType,
                                uint64_t Pages, uint64_t *Memory);
    efi_status (*FreePages)(uint64_t Memory, uint64_t Pages);
    efi_status (*GetMemoryMap)(uint64_t *MemoryMapSize, void *MemoryMap,
                               uint64_t *MapKey, uint64_t *DescriptorSize,
                               uint32_t *DescriptorVersion);
    efi_status (*AllocatePool)(uint32_t PoolType, uint64_t Size,
                               void **Buffer);
    efi_status (*FreePool)(void *Buffer);

    void *CreateEvent;
    void *SetTimer;
    void *WaitForEvent;
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;

    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    efi_status (*HandleProtocol)(efi_handle Handle, efi_guid *Protocol,
                                 void **Interface);
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;

    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    efi_status (*ExitBootServices)(efi_handle ImageHandle, uint64_t MapKey);

    void *GetNextMonotonicCount;
    efi_status (*Stall)(uint64_t Microseconds);
    efi_status (*SetWatchdogTimer)(uint64_t Timeout, uint64_t WatchdogCode,
                                   uint64_t DataSize, char16 *WatchdogData);

    void *ConnectController;
    void *DisconnectController;

    void *OpenProtocol;
    void *CloseProtocol;
    void *OpenProtocolInformation;

    void *ProtocolsPerHandle;
    efi_status (*LocateHandleBuffer)(uint32_t SearchType, efi_guid *Protocol,
                                     void *SearchKey, uint64_t *NoHandles,
                                     efi_handle **Buffer);
    efi_status (*LocateProtocol)(efi_guid *Protocol, void *Registration,
                                 void **Interface);
    void *InstallMultipleProtocolInterfaces;
    void *UninstallMultipleProtocolInterfaces;

    void *CalculateCrc32;

    void *CopyMem;
    void *SetMem;
    void *CreateEventEx;
} efi_boot_services;

/* EFI_LOCATE_SEARCH_TYPE for LocateHandleBuffer */
#define EFI_BY_PROTOCOL 2

/* ---- Configuration table (ACPI RSDP lives here) ---- */

typedef struct {
    efi_guid VendorGuid;
    void *VendorTable;
} efi_configuration_table;

#define EFI_ACPI_20_TABLE_GUID \
    { 0x8868e871, 0xe4f1, 0x11d3, \
      { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } }

/* ---- System table ---- */

typedef struct {
    efi_table_header Hdr;
    char16 *FirmwareVendor;
    uint32_t FirmwareRevision;
    efi_handle ConsoleInHandle;
    void *ConIn;
    efi_handle ConsoleOutHandle;
    efi_simple_text_output *ConOut;
    efi_handle StandardErrorHandle;
    efi_simple_text_output *StdErr;
    void *RuntimeServices;
    efi_boot_services *BootServices;
    uint64_t NumberOfTableEntries;
    efi_configuration_table *ConfigurationTable;
} efi_system_table;

/* ---- Block I/O protocol ---- */

#define EFI_BLOCK_IO_PROTOCOL_GUID \
    { 0x964e5b21, 0x6459, 0x11d2, \
      { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

/* Natural C alignment reproduces the spec layout here (BlockSize at
 * offset 12, LastBlock at 24). Revision 2/3 trailing fields are omitted;
 * nothing dereferences past LastBlock. */
typedef struct {
    uint32_t MediaId;
    uint8_t RemovableMedia;
    uint8_t MediaPresent;
    uint8_t LogicalPartition;
    uint8_t ReadOnly;
    uint8_t WriteCaching;
    uint32_t BlockSize;
    uint32_t IoAlign;
    uint64_t LastBlock;
} efi_block_io_media;

typedef struct efi_block_io efi_block_io;
struct efi_block_io {
    uint64_t Revision;
    efi_block_io_media *Media;
    void *Reset;
    efi_status (*ReadBlocks)(efi_block_io *This, uint32_t MediaId,
                             uint64_t LBA, uint64_t BufferSize, void *Buffer);
    void *WriteBlocks;
    void *FlushBlocks;
};

/* ---- Graphics output protocol ---- */

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, \
      { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }

/* EFI_GRAPHICS_PIXEL_FORMAT */
#define EFI_PIXEL_RGB_RESERVED_8BPC 0
#define EFI_PIXEL_BGR_RESERVED_8BPC 1

typedef struct {
    uint32_t Version;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    uint32_t PixelFormat;
    struct {
        uint32_t RedMask;
        uint32_t GreenMask;
        uint32_t BlueMask;
        uint32_t ReservedMask;
    } PixelInformation;
    uint32_t PixelsPerScanLine;
} efi_gop_mode_info;

typedef struct {
    uint32_t MaxMode;
    uint32_t Mode;
    efi_gop_mode_info *Info;
    uint64_t SizeOfInfo;
    uint64_t FrameBufferBase;
    uint64_t FrameBufferSize;
} efi_gop_mode;

typedef struct {
    void *QueryMode;
    void *SetMode;
    void *Blt;
    efi_gop_mode *Mode;
} efi_graphics_output;

#endif
