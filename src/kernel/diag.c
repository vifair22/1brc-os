#include "kernel/diag.h"

#include "console/fmt.h"
#include "kernel/alloc.h"
#include "kernel/cpu.h"
#include "kernel/smp.h"

#define DIAG_MAX_CPUS 64

#define MSR_MTRRCAP 0xfe
#define MSR_PAT 0x277
#define MSR_MTRR_PHYSBASE0 0x200
#define MSR_MTRR_DEFTYPE 0x2ff

static uint64_t mtrr_sig[DIAG_MAX_CPUS];
static uint64_t pat_val[DIAG_MAX_CPUS];

static uint64_t rol64(uint64_t v, unsigned r)
{
    return (v << r) | (v >> (64 - r));
}

/* One value summarizing everything that decides this core's view of
 * memory types: default type, every variable range pair, every fixed
 * range. Equal signatures = identical cacheability. */
static uint64_t mtrr_signature(void)
{
    static const uint32_t fixed[] = { 0x250, 0x258, 0x259, 0x268, 0x269,
                                      0x26a, 0x26b, 0x26c, 0x26d, 0x26e,
                                      0x26f };
    uint64_t cap = rdmsr(MSR_MTRRCAP);
    uint64_t sig = rdmsr(MSR_MTRR_DEFTYPE);
    uint32_t vcnt = cap & 0xff;

    if (vcnt > 16)
        vcnt = 16;
    for (uint32_t i = 0; i < vcnt; i++)
        sig ^= rol64(rdmsr(MSR_MTRR_PHYSBASE0 + 2 * i), i + 1) ^
               rol64(rdmsr(MSR_MTRR_PHYSBASE0 + 2 * i + 1), i + 33);
    if (cap & (1u << 8))
        for (uint32_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++)
            sig ^= rol64(rdmsr(fixed[i]), 40 + i);
    return sig;
}

void diag_ap_sample(uint32_t cpu_index)
{
    if (cpu_index < DIAG_MAX_CPUS) {
        mtrr_sig[cpu_index] = mtrr_signature();
        pat_val[cpu_index] = rdmsr(MSR_PAT);
    }
}

/* -------- steal probe: is anything invisible taking our cycles? --------
 * A register-only rdtsc loop never legitimately gaps by a microsecond;
 * every gap above that is time the core spent elsewhere - which, with
 * interrupts off and no OS, means SMM. */

static struct {
    uint64_t tsc_hz;
    uint64_t gaps[DIAG_MAX_CPUS];
    uint64_t worst[DIAG_MAX_CPUS];
    uint64_t stolen[DIAG_MAX_CPUS];
} steal;

static void steal_probe(uint32_t cpu, void *arg)
{
    (void)arg;
    uint64_t thresh = steal.tsc_hz / 1000000; /* 1 us */
    uint64_t prev = rdtsc();
    uint64_t end = prev + steal.tsc_hz / 20; /* 50 ms */
    uint64_t gaps = 0, worst = 0, stolen = 0;

    for (;;) {
        uint64_t now = rdtsc();
        uint64_t d = now - prev;
        prev = now;
        if (d > thresh) {
            gaps++;
            stolen += d;
            if (d > worst)
                worst = d;
        }
        if (now >= end)
            break;
    }
    if (cpu < DIAG_MAX_CPUS) {
        steal.gaps[cpu] = gaps;
        steal.worst[cpu] = worst;
        steal.stolen[cpu] = stolen;
    }
}

/* -------- raw read bandwidth: is the memory path itself healthy? ----
 * Pure 64-bit streaming loads, no parsing. Solo answers "can one core
 * pull DRAM at normal speed in this environment"; all-cores answers
 * "does it scale to the socket". */

static struct {
    const uint64_t *buf;
    uint64_t words;
    int solo;
    uint64_t ticks[DIAG_MAX_CPUS];
} bw;

static void bw_probe(uint32_t cpu, void *arg)
{
    (void)arg;
    if (bw.solo && cpu != 0)
        return;
    const uint64_t *p = bw.buf;
    uint64_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    uint64_t t0 = rdtsc();
    for (uint64_t i = 0; i + 8 <= bw.words; i += 8) {
        s0 += p[i];
        s1 += p[i + 2];
        s2 += p[i + 4];
        s3 += p[i + 6];
    }
    __asm__ volatile("" : : "r"(s0 + s1 + s2 + s3));
    if (cpu < DIAG_MAX_CPUS)
        bw.ticks[cpu] = rdtsc() - t0;
}

static uint64_t mibps(uint64_t bytes, uint64_t ticks, uint64_t hz)
{
    return ticks ? bytes / 1024 / 1024 * hz / ticks : 0;
}

static void bandwidth_report(const boot_info *bi)
{
    uint32_t n = smp_cpu_count();

    /* Probe the dataset itself: real written memory (a fresh
     * allocation reads back as shared zero pages under a hypervisor
     * and produces fantasy numbers), and exactly the bytes the parse
     * streams. */
    uint64_t bytes = bi->dataset_bytes < (1ULL << 30)
                         ? bi->dataset_bytes & ~63ULL
                         : (1ULL << 30);
    bw.buf = bi->dataset;
    bw.words = bytes / 8;

    bw.solo = 1;
    smp_run(bw_probe, 0);
    uint64_t solo = mibps(bytes, bw.ticks[0], bi->tsc_hz);

    bw.solo = 0;
    smp_run(bw_probe, 0);
    uint64_t worst = 0;
    for (uint32_t i = 0; i < n && i < DIAG_MAX_CPUS; i++)
        if (bw.ticks[i] > worst)
            worst = bw.ticks[i];
    uint64_t agg = mibps(bytes * n, worst, bi->tsc_hz);

    kprintf("diag: read bw solo %llu.%llu GB/s, %u-core %llu.%llu GB/s\n",
            solo / 1024, solo % 1024 * 10 / 1024, n, agg / 1024,
            agg % 1024 * 10 / 1024);
}

void diag_report(const boot_info *bi)
{
    uint32_t n = smp_cpu_count();

    diag_ap_sample(0); /* the BSP's own view is the reference */
    uint32_t mtrr_bad = 0, pat_bad = 0;
    for (uint32_t i = 1; i < n && i < DIAG_MAX_CPUS; i++) {
        if (mtrr_sig[i] != mtrr_sig[0])
            mtrr_bad++;
        if (pat_val[i] != pat_val[0])
            pat_bad++;
    }
    kprintf("diag: mtrr def %llx %s (%u/%u differ), pat %llx %s\n",
            rdmsr(MSR_MTRR_DEFTYPE), mtrr_bad ? "MISMATCH" : "uniform",
            mtrr_bad, n - 1, pat_val[0],
            pat_bad ? "MISMATCH" : "uniform");

    bandwidth_report(bi);

    steal.tsc_hz = bi->tsc_hz;
    smp_run(steal_probe, 0);
    uint64_t gaps = 0, worst = 0, stolen = 0;
    for (uint32_t i = 0; i < n && i < DIAG_MAX_CPUS; i++) {
        gaps += steal.gaps[i];
        stolen += steal.stolen[i];
        if (steal.worst[i] > worst)
            worst = steal.worst[i];
    }
    /* percent x10 of the total probe time across all cores */
    uint64_t window = steal.tsc_hz / 20 * n;
    uint64_t pml = stolen * 1000 / window;
    kprintf("diag: steal %llu gaps, worst %llu us, %llu.%llu%% of "
            "50 ms x %u cores\n",
            gaps, worst * 1000000 / steal.tsc_hz, pml / 10, pml % 10, n);
}
