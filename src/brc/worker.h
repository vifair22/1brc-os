/* Challenge orchestration: run the parse over the resident dataset on
 * every online core, aggregate, and emit the official output line. The
 * caller gets the stage timings back for the boot summary.
 */
#ifndef BRC_WORKER_H
#define BRC_WORKER_H

#include <stdint.h>

#include "kernel/bootinfo.h"

/* Official 1BRC protocol: 5 timed runs over the resident dataset,
 * discard best and worst, average the middle three. Each run covers
 * table reset, parse, merge, and line build. */
#define BRC_BENCH_RUNS 5

typedef struct {
    uint64_t avg_ticks;  /* trimmed mean of the runs */
    uint64_t best_ticks;
    uint64_t worst_ticks;
    uint64_t rows;
    uint64_t stations;
    uint64_t out_bytes; /* challenge line length incl. newline */
    uint64_t out_hash;  /* FNV-1a 64 of the line incl. newline */
    int hash_stable;    /* all runs produced identical output */
} brc_result;

/* Per-boot experiment matrix (solo core speeds, physical-cores-only
 * pass); prints its findings. Run before brc_run. */
void brc_diag(const boot_info *bi);

int brc_run(const boot_info *bi, brc_result *res);

#endif
