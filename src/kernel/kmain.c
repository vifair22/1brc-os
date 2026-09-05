#include "kernel/bootinfo.h"

#include "brc/worker.h"
#include "console/console.h"
#include "console/fmt.h"
#include "kernel/alloc.h"
#include "kernel/cpu.h"
#include "kernel/diag.h"
#include "kernel/gdt.h"
#include "kernel/idt.h"
#include "kernel/paging.h"
#include "kernel/smp.h"

_Noreturn void kmain(boot_info *bi)
{
    console_init(&bi->fb);

    kprintf("1brc-os 0.1.0\n");
    kprintf("post-ExitBootServices; firmware is gone\n\n");
    kprintf("framebuffer  %ux%u stride %u fmt %s @ 0x%llx\n", bi->fb.width,
            bi->fb.height, bi->fb.stride, bi->fb.format ? "bgr" : "rgb",
            bi->fb.base);
    kprintf("memory map   %llu bytes, %llu descriptors\n", bi->memmap_size,
            bi->memmap_size / bi->desc_size);
    kprintf("conventional %llu MiB\n", bi->conventional_bytes >> 20);
    kprintf("acpi rsdp    %p\n", bi->acpi_rsdp);
    kprintf("tsc          %llu MHz\n\n", bi->tsc_hz / 1000000);

    /* Dataset sanity: firmware handed us these bytes, so show enough to
     * eyeball that they are really measurement rows. */
    const uint8_t *d = bi->dataset;
    uint64_t n = bi->dataset_bytes;
    /* Ordered to stay in 64 bits: no compiler-rt, so no 128-bit
     * division. (n/1e6) * tsc_hz tops out around 2^56 for a 16 GB
     * dataset on a 5 GHz TSC. */
    uint64_t load_ms = bi->load_ticks / (bi->tsc_hz / 1000);
    uint64_t mbps = n / 1000000 * bi->tsc_hz / bi->load_ticks;

    kprintf("dataset      %llu bytes @ %p\n", n, bi->dataset);
    kprintf("loaded in    %llu ms (%llu MB/s via firmware BlockIo)\n",
            load_ms, mbps);
    kprintf("first row    ");
    for (uint64_t i = 0; i < 48 && i < n && d[i] != '\n'; i++)
        console_putc((char)d[i]);
    kprintf("\nlast byte    %s\n", n && d[n - 1] == '\n' ? "newline (ok)"
                                                         : "NOT NEWLINE");
    kprintf("tail slack   %s\n\n",
            d[n] == 0 && d[n + 63] == 0 ? "zeroed (ok)" : "DIRTY");

    alloc_init(bi);
    gdt_load();
    idt_load();
    paging_init();
    if (cpu_enable_simd() != 0) {
        kprintf("cpu: no AVX2, cannot run\n");
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    cpu_boost();
    kprintf("cpu: AVX2 enabled, boost requested\n");

    uint64_t t_smp = rdtsc();
    uint32_t cores = smp_start_all(bi);
    uint64_t smp_ticks = rdtsc() - t_smp;
    kprintf("smp: %u cores ready\n", cores);

    diag_report(bi);
    brc_diag(bi);
    kprintf("\n");

    /* APERF/MPERF around the benchmark give the BSP's true average
     * clock, the direct readout of whether the boost request stuck. */
    int have_aperf = cpu_has_aperf();
    uint64_t ap0 = have_aperf ? cpu_aperf() : 0;
    uint64_t mp0 = have_aperf ? cpu_mperf() : 0;

    brc_result r;
    if (brc_run(bi, &r) != 0)
        for (;;)
            __asm__ volatile("cli; hlt");

    uint64_t eff_mhz = 0;
    if (have_aperf) {
        uint64_t da = cpu_aperf() - ap0;
        uint64_t dm = cpu_mperf() - mp0;
        if (dm)
            eff_mhz = bi->tsc_hz / 1000000 * da / dm;
    }

    /* Stage summary. "power-on to answer" is rdtsc-since-reset, so it
     * includes the firmware's own boot time. */
    uint64_t khz = bi->tsc_hz / 1000;
    uint64_t smp_ms = smp_ticks / khz;
    uint64_t bench_ms = r.avg_ticks / khz;
    uint64_t cyc_x10 = r.rows ? r.avg_ticks * 10 / r.rows : 0;
    uint64_t total_cs = rdtsc() / (bi->tsc_hz / 100);

    kprintf("\nstages\n");
    kprintf("  load   %6llu ms   %llu MB/s firmware BlockIo\n", load_ms,
            mbps);
    kprintf("  smp    %6llu ms   %u cores via INIT-SIPI-SIPI\n", smp_ms,
            cores);
    kprintf("  bench  %6llu ms   trimmed mean of %u runs "
            "(best %llu / worst %llu)\n",
            bench_ms, BRC_BENCH_RUNS, r.best_ticks / khz,
            r.worst_ticks / khz);
    kprintf("  rate   %llu.%llu cycles/row wall, %llu rows, "
            "%llu stations\n",
            cyc_x10 / 10, cyc_x10 % 10, r.rows, r.stations);
    if (eff_mhz)
        kprintf("  clock  %llu.%02llu GHz effective on BSP "
                "(aperf/mperf)\n",
                eff_mhz / 1000, eff_mhz % 1000 / 10);
    kprintf("finished: compute %llu ms (excl. load), "
            "power-on to answer %llu.%02llu s\n",
            smp_ms + bench_ms, total_cs / 100, total_cs % 100);

    for (;;)
        __asm__ volatile("cli; hlt");
}
