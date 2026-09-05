/* PE entry point, called by firmware in long mode with boot services
 * alive. Gathers everything in boot_info, exits boot services, and
 * hands the machine to kmain. Never returns.
 */
#ifndef EFI_LOADER_H
#define EFI_LOADER_H

#include "efi/efi.h"

efi_status efi_main(efi_handle image, efi_system_table *systab);

#endif
