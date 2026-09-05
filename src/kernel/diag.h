/* Boot-time hardware diagnostics. Rebooting real hardware is the most
 * expensive operation this project has, so every boot answers a full
 * matrix of questions: are the APs' MTRR/PAT views identical to the
 * BSP's, and is firmware SMM stealing time behind our backs.
 */
#ifndef KERNEL_DIAG_H
#define KERNEL_DIAG_H

#include <stdint.h>

#include "kernel/bootinfo.h"

/* Called by each AP during bringup to record its memory-typing state. */
void diag_ap_sample(uint32_t cpu_index);

/* BSP: compares samples across cores and runs the steal probe on all
 * of them. Prints two compact lines. */
void diag_report(const boot_info *bi);

#endif
