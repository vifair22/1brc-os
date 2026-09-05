#include "brc/worker.h"

#include "brc/parse.h"
#include "console/console.h"
#include "console/fmt.h"
#include "efi/blockio.h" /* DATASET_FLAG_QUIET */
#include "kernel/alloc.h"
#include "kernel/cpu.h"
#include "kernel/smp.h"
#include "lib/sort.h"
#include "lib/string.h"

/* Dynamic work distribution, ported from 1brc.c: one shared cursor
 * hands out fixed-size chunks and every core parses into its own table
 * until the file runs out. Chunk boundaries are line-aligned by the
 * same rule everywhere, so the chunks partition the file exactly. */
#ifndef CHUNK_BYTES
#define CHUNK_BYTES (8ULL << 20)
#endif

static struct {
    const char *base;
    uint64_t bulk_end;
    uint64_t cursor;
    WorkerReturn *wrs;
    volatile int failed;
    /* Experiment knobs: cap the worker count, pin to a single cpu, or
     * take only even APIC ids (one thread per physical core on SMT). */
    uint32_t limit;
    int only_cpu;
    int even_only;
} job;

static uint64_t line_start_at_or_after(const char *base, uint64_t off,
                                       uint64_t limit)
{
    if (off == 0)
        return 0;
    if (off >= limit)
        return limit;
    const char *nl = find_next_newline(base + off - 1, base + limit);
    uint64_t start = (uint64_t)(nl - base) + 1;
    return start < limit ? start : limit;
}

/* The hot body carries the SysV calling convention even in the win64
 * PE build: under the MS convention (rdi/rsi and xmm6-15 all
 * callee-saved) clang spills the hash chain's operands to the stack
 * and the parse runs 4x slower - measured, same TU, only the ABI
 * differing. All of parse.h inlines into this function, so its CC
 * decides the register regime for the entire hot loop. */
__attribute__((sysv_abi, noinline)) static void
chunk_worker_hot(uint32_t cpu_index)
{
    WorkerReturn *res = &job.wrs[cpu_index];

    for (;;) {
        uint64_t off = __atomic_fetch_add(&job.cursor, CHUNK_BYTES,
                                          __ATOMIC_RELAXED);
        if (off >= job.bulk_end)
            break;
        WorkerRange r;
        r.start = line_start_at_or_after(job.base, off, job.bulk_end);
        r.end =
            line_start_at_or_after(job.base, off + CHUNK_BYTES, job.bulk_end);
        if (r.start >= r.end)
            continue;
        if (parse_range(job.base, &r, res) != 0) {
            job.failed = 1;
            break;
        }
    }
}

/* MS-ABI shim: smp_run's dispatch calls this with the platform CC. */
static void chunk_worker(uint32_t cpu_index, void *arg)
{
    (void)arg;
    if (job.only_cpu >= 0 && cpu_index != (uint32_t)job.only_cpu)
        return;
    if (job.limit && cpu_index >= job.limit)
        return;
    if (job.even_only && (smp_apic_id(cpu_index) & 1))
        return;
    chunk_worker_hot(cpu_index);
}

/* -------------------- aggregation (ported from 1brc.c) -------------- */

typedef struct {
    const char *city_ptr;
    uint32_t city_len;
    uint64_t key_a;
    uint64_t key_b;
    uint64_t count;
    int64_t sum10;
    int32_t min10;
    int32_t max10;
} AggEntry;

typedef struct {
    AggEntry *table;
    size_t mask;
    size_t size;
    size_t occupied;
} AggregateTable;

static int aggregate_init(AggregateTable *agg, size_t size_pow2)
{
    agg->table = (AggEntry *)kzalloc(size_pow2 * sizeof(AggEntry), 64);
    agg->size = size_pow2;
    agg->mask = size_pow2 - 1;
    agg->occupied = 0;
    return 0;
}

/* Same insert-or-merge as upstream: workers see disjoint slices, so the
 * union of their keys needs its own capacity guard. */
static int aggregate_add(AggregateTable *agg, const Entry *src,
                         const EntryMeta *meta)
{
    const int32_t src_min = minmax_min(src->minmax);
    const int32_t src_max = minmax_max(src->minmax);
    size_t idx =
        (size_t)(((src->key_a + src->key_b) * 0x9e3779b185ebca87ULL) >> 29) &
        agg->mask;
    for (;;) {
        AggEntry *dst = &agg->table[idx];
        if (!dst->city_ptr) {
            if (agg->occupied + 1 >= agg->size)
                return -1;
            agg->occupied++;
            dst->city_ptr = meta->city_ptr;
            dst->city_len = meta->city_len;
            dst->key_a = src->key_a;
            dst->key_b = src->key_b;
            dst->count = src->count;
            dst->sum10 = src->sum10;
            dst->min10 = src_min;
            dst->max10 = src_max;
            return 0;
        } else if (((dst->key_a ^ src->key_a) | (dst->key_b ^ src->key_b)) ==
                       0 &&
                   (meta->city_len <= 15 ||
                    (dst->city_len == meta->city_len &&
                     (meta->city_len <= 16 ||
                      memcmp(dst->city_ptr + 16, meta->city_ptr + 16,
                             meta->city_len - 16) == 0)))) {
            dst->count += src->count;
            dst->sum10 += src->sum10;
            if (src_min < dst->min10)
                dst->min10 = src_min;
            if (src_max > dst->max10)
                dst->max10 = src_max;
            return 0;
        }
        idx = (idx + 1) & agg->mask;
    }
}

/* -------------------- challenge output -------------------- */

/* The MS ABI makes any floating-point-using object reference this
 * marker symbol; normally the CRT provides it, here we are the CRT. */
int _fltused = 0;

/* llround stand-in (no libm): round half away from zero, which is what
 * the mean's double-arithmetic ground truth requires. */
static int64_t kllround(double x)
{
    return x >= 0.0 ? (int64_t)(x + 0.5) : (int64_t)(x - 0.5);
}

/* Mean deliberately via doubles, matching the Java reference's rounding
 * behavior; see upstream 1brc.c for the full story. */
static int64_t mean_tenths(int64_t sum10, uint64_t count)
{
    double mean = (double)sum10 / 10.0 / (double)count;
    return kllround(mean * 10.0);
}

/* The challenge line is built into a buffer instead of streaming to the
 * console: quiet mode wants only its hash, and hashing needs the bytes
 * anyway. */
static struct {
    char *buf;
    uint64_t len;
} out;

static void emitc(char c)
{
    out.buf[out.len++] = c;
}

static void emit_u64(uint64_t v)
{
    char tmp[20];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        emitc(tmp[--n]);
}

static void emit_tenths(int64_t tenths)
{
    if (tenths < 0) {
        emitc('-');
        tenths = -tenths;
    }
    emit_u64((uint64_t)tenths / 10);
    emitc('.');
    emit_u64((uint64_t)tenths % 10);
}

static uint64_t fnv1a64(const char *p, uint64_t n)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (uint64_t i = 0; i < n; i++) {
        h ^= (uint8_t)p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static int agg_entry_name_cmp(const void *pa, const void *pb)
{
    const AggEntry *a = (const AggEntry *)pa;
    const AggEntry *b = (const AggEntry *)pb;
    uint32_t n = a->city_len < b->city_len ? a->city_len : b->city_len;
    int c = memcmp(a->city_ptr, b->city_ptr, n);
    if (c != 0)
        return c;
    return (a->city_len > b->city_len) - (a->city_len < b->city_len);
}

static void build_challenge_line(const AggregateTable *totals,
                                 const AggEntry **list)
{
    size_t n = 0;
    for (size_t i = 0; i < totals->size; ++i)
        if (totals->table[i].city_ptr)
            list[n++] = &totals->table[i];
    ksort_ptrs((const void **)list, n, agg_entry_name_cmp);

    out.len = 0;
    emitc('{');
    for (size_t i = 0; i < n; ++i) {
        const AggEntry *e = list[i];
        if (i) {
            emitc(',');
            emitc(' ');
        }
        for (uint32_t j = 0; j < e->city_len; ++j)
            emitc(e->city_ptr[j]);
        emitc('=');
        emit_tenths(e->min10);
        emitc('/');
        emit_tenths(mean_tenths(e->sum10, e->count));
        emitc('/');
        emit_tenths(e->max10);
    }
    emitc('}');
    emitc('\n');
}

/* -------------------- run -------------------- */

/* One-time setup shared by the diag experiments and the benchmark: all
 * allocations happen here, on the BSP, before the first timed run.
 * parse_range itself never allocates, so the bump pool needs no locking
 * anywhere and every run reuses everything. */
static AggregateTable totals;
static const AggEntry **agg_list;

static void brc_init(const boot_info *bi)
{
    if (job.wrs)
        return;
    uint32_t ncpu = smp_cpu_count();
    job.base = (const char *)bi->dataset;
    job.wrs = (WorkerReturn *)kalloc(ncpu * sizeof(WorkerReturn), 64);
    for (uint32_t i = 0; i < ncpu; i++)
        worker_return_init(&job.wrs[i], TABLE_SLOTS);
    aggregate_init(&totals, TABLE_SLOTS);
    agg_list = (const AggEntry **)kalloc(totals.size * sizeof(*agg_list), 8);
    /* Worst case: 10k stations x (100-byte name + 3 numbers + syntax);
     * 4 MiB is an order of magnitude of headroom. */
    out.buf = kalloc(4ULL << 20, 64);
}

/* One timed pass under the given experiment knobs; returns ticks and
 * the row count actually parsed. */
static uint64_t bench_once(uint64_t bytes, uint32_t limit, int only_cpu,
                           int even_only, uint64_t *rows_out)
{
    uint32_t ncpu = smp_cpu_count();
    uint64_t t0 = rdtsc();

    for (uint32_t i = 0; i < ncpu; i++)
        worker_return_reset(&job.wrs[i]);
    job.bulk_end = bytes;
    job.cursor = 0;
    job.failed = 0;
    job.limit = limit;
    job.only_cpu = only_cpu;
    job.even_only = even_only;
    smp_run(chunk_worker, 0);
    uint64_t dt = rdtsc() - t0;

    uint64_t rows = 0;
    for (uint32_t w = 0; w < ncpu; w++)
        for (size_t i = 0; i < job.wrs[w].occupied_count; ++i)
            rows += job.wrs[w].table[job.wrs[w].occupied_indices[i]].count;
    *rows_out = rows;
    return dt;
}

/* Per-boot experiment matrix: solo per-core speed (BSP vs one AP) and a
 * one-thread-per-physical-core pass. Splits "per-core state problem"
 * from "global tax" from "SMT/bandwidth ceiling" in a single boot. */
void brc_diag(const boot_info *bi)
{
    brc_init(bi);

    uint64_t slice = bi->dataset_bytes < (1ULL << 30) ? bi->dataset_bytes
                                                      : (1ULL << 30);
    slice = line_start_at_or_after(job.base, slice, bi->dataset_bytes);
    uint64_t rows, dt;

    dt = bench_once(slice, 1, -1, 0, &rows);
    uint64_t bsp_x10 = rows ? dt * 10 / rows : 0;
    dt = bench_once(slice, 0, 1, 0, &rows);
    uint64_t ap_x10 = rows ? dt * 10 / rows : 0;
    kprintf("diag: solo parse bsp %llu.%llu c/r, ap1 %llu.%llu c/r "
            "(1 GiB slice)\n",
            bsp_x10 / 10, bsp_x10 % 10, ap_x10 / 10, ap_x10 % 10);

    dt = bench_once(bi->dataset_bytes, 0, -1, 1, &rows);
    uint64_t phys_x10 = rows ? dt * 10 / rows : 0;
    kprintf("diag: physical cores only, full set: %llu ms "
            "(%llu.%llu c/r wall)\n",
            dt / (bi->tsc_hz / 1000), phys_x10 / 10, phys_x10 % 10);
}

int brc_run(const boot_info *bi, brc_result *res)
{
    uint32_t ncpu = smp_cpu_count();

    brc_init(bi);
    job.limit = 0;
    job.only_cpu = -1;
    job.even_only = 0;

    uint64_t run_ticks[BRC_BENCH_RUNS];
    uint64_t run_hash[BRC_BENCH_RUNS];
    uint64_t rows = 0;

    for (uint32_t r = 0; r < BRC_BENCH_RUNS; r++) {
        uint64_t t0 = rdtsc();

        for (uint32_t i = 0; i < ncpu; i++)
            worker_return_reset(&job.wrs[i]);
        memset(totals.table, 0, totals.size * sizeof(AggEntry));
        totals.occupied = 0;
        job.bulk_end = bi->dataset_bytes;
        job.cursor = 0;
        job.failed = 0;

        smp_run(chunk_worker, 0);
        if (job.failed) {
            kprintf("brc: parse failed (run %u)\n", r + 1);
            return -1;
        }

        rows = 0;
        for (uint32_t w = 0; w < ncpu; w++) {
            const WorkerReturn *wr = &job.wrs[w];
            for (size_t i = 0; i < wr->occupied_count; ++i) {
                size_t idx = wr->occupied_indices[i];
                rows += wr->table[idx].count;
                if (aggregate_add(&totals, &wr->table[idx],
                                  &wr->meta[idx]) != 0) {
                    kprintf("brc: aggregate table full\n");
                    return -1;
                }
            }
        }
        build_challenge_line(&totals, agg_list);

        run_ticks[r] = rdtsc() - t0;
        run_hash[r] = fnv1a64(out.buf, out.len);
        kprintf("run %u/%u: %llu ms\n", r + 1, BRC_BENCH_RUNS,
                run_ticks[r] / (bi->tsc_hz / 1000));
    }

    /* Discard best and worst, average the rest; identical outputs
     * across runs double as an SMP-race canary. */
    uint32_t lo = 0, hi = 0;
    res->hash_stable = 1;
    for (uint32_t r = 1; r < BRC_BENCH_RUNS; r++) {
        if (run_ticks[r] < run_ticks[lo])
            lo = r;
        if (run_ticks[r] > run_ticks[hi])
            hi = r;
        if (run_hash[r] != run_hash[0])
            res->hash_stable = 0;
    }
    if (hi == lo) /* all runs tied: still drop exactly two */
        hi = (lo + 1) % BRC_BENCH_RUNS;
    uint64_t sum = 0;
    uint32_t kept = 0;
    for (uint32_t r = 0; r < BRC_BENCH_RUNS; r++) {
        if (r == lo || r == hi)
            continue;
        sum += run_ticks[r];
        kept++;
    }
    res->avg_ticks = sum / kept;
    res->best_ticks = run_ticks[lo];
    res->worst_ticks = run_ticks[hi];
    res->rows = rows;
    res->stations = totals.occupied;
    res->out_bytes = out.len;
    res->out_hash = run_hash[BRC_BENCH_RUNS - 1];

    if (!res->hash_stable)
        kprintf("brc: WARNING: runs disagree on output, SMP race?\n");
    if (bi->dataset_flags & DATASET_FLAG_QUIET) {
        kprintf("output: %llu bytes, %llu stations, fnv1a %llx (quiet)\n",
                out.len, (uint64_t)totals.occupied, res->out_hash);
    } else {
        for (uint64_t i = 0; i < out.len; i++)
            console_putc(out.buf[i]);
    }
    return 0;
}
