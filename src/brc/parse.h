/* Hot parsing path, ported from 1brc-c's parse.h. Keep this diffable
 * against upstream: the hash, line-shape scan, temperature parse, probe
 * loop and stream structure are byte-for-byte the same logic. The full
 * "why" for every trick lives in the upstream file's comments; only
 * deltas are documented here. The deltas, all environmental:
 *
 *   - Allocation is the kernel bump allocator (kalloc/kzalloc); there
 *     is no free and no realloc. occupied_indices is sized to the full
 *     table up front, which deletes upstream's realloc grow path.
 *   - No tail special-case exists anywhere: the loader guarantees 64
 *     bytes of zeroed slack after the dataset, so the SIMD overreads
 *     are safe up to and including the last line.
 *   - libc includes replaced by kernel equivalents.
 */
#ifndef BRC_PARSE_H
#define BRC_PARSE_H

#include <immintrin.h>
#include <stdbool.h>
#include <stdint.h>

#include "kernel/alloc.h"
#include "lib/string.h"

#if !defined(__AVX2__)
#error "AVX2 required for this build."
#endif

#define FORCE_INLINE inline __attribute__((always_inline))

#define PRAGMA_STR(x) _Pragma(#x)
#define PRAGMA_EXPAND(x) PRAGMA_STR(x)
#define UNROLL_STREAMS PRAGMA_EXPAND(GCC unroll NSTREAMS)

/* -------------------- utility -------------------- */

static FORCE_INLINE unsigned ctz32(uint32_t v)
{
    return (unsigned)__builtin_ctz(v);
}

typedef struct {
    size_t start;
    size_t end;
} WorkerRange;

/* Hot hash slot, exactly 32 bytes; see upstream for the layout
 * rationale (key includes the ';', minmax packs {min10, -max10} so one
 * PMINSW updates both extremes). */
typedef struct {
    uint64_t key_a;
    uint64_t key_b;
    int64_t sum10;
    uint32_t count;
    int32_t minmax;
} Entry;

static FORCE_INLINE int32_t minmax_pack(int32_t temp10)
{
    return (int32_t)((uint32_t)(uint16_t)(int16_t)temp10 |
                     ((uint32_t)(uint16_t)(int16_t)(-temp10) << 16));
}

static FORCE_INLINE int32_t minmax_min(int32_t a)
{
    return (int16_t)(a & 0xFFFF);
}

static FORCE_INLINE int32_t minmax_max(int32_t a)
{
    return -(int32_t)(int16_t)(a >> 16);
}

/* Cold per-slot data, parallel to the Entry table. */
typedef struct {
    const char *city_ptr;
    uint32_t city_len;
} EntryMeta;

typedef struct {
    Entry *table;
    EntryMeta *meta;
    size_t table_size;
    size_t *occupied_indices;
    size_t occupied_count;
    size_t occupied_cap;
} WorkerReturn;

/* -------------------- hashing and scanning -------------------- */

/* BZHI key build + one multiply; identical to upstream. */
static FORCE_INLINE uint64_t hash_city(const char *ptr, uint32_t len,
                                       __m128i *key_out)
{
    uint32_t keylen = len + 1; /* include the ';' terminator in the key */
    uint32_t kk = (keylen < 16) ? keylen : 16;
    uint32_t tbits = kk * 8;
    uint64_t lo, hi;
    memcpy(&lo, ptr, 8);
    memcpy(&hi, ptr + 8, 8);
    int32_t t2 = (int32_t)tbits - 64;
    t2 &= ~(t2 >> 31);
    uint64_t a = _bzhi_u64(lo, tbits);
    uint64_t b = _bzhi_u64(hi, (uint32_t)t2);
    *key_out = _mm_set_epi64x((int64_t)b, (int64_t)a);
    uint64_t h = (a + b) * 0x9e3779b185ebca87ULL;
    return h >> 29;
}

static FORCE_INLINE bool city_long_eq(const EntryMeta *m, const char *ptr,
                                      uint32_t len)
{
    return m->city_len == len &&
           (len <= 16 || memcmp(m->city_ptr + 16, ptr + 16, len - 16) == 0);
}

static const char *find_next_newline(const char *p, const char *end)
{
    const __m256i nl = _mm256_set1_epi8('\n');
    while (p + 32 <= end) {
        __m256i chunk = _mm256_loadu_si256((const __m256i *)p);
        uint32_t mask =
            (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, nl));
        if (mask)
            return p + ctz32(mask);
        p += 32;
    }
    while (p < end) {
        if (*p == '\n')
            return p;
        ++p;
    }
    return end;
}

/* Both delimiter offsets from one 32-byte load; scalar fallback for
 * pathological lines. Identical to upstream. */
typedef struct {
    uint32_t city_len;
    uint32_t line_len;
} LineShape;

static FORCE_INLINE LineShape find_shape(const char *p)
{
    const __m256i semi = _mm256_set1_epi8(';');
    const __m256i newline = _mm256_set1_epi8('\n');
    __m256i chunk = _mm256_loadu_si256((const __m256i *)p);
    uint32_t smask =
        (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, semi));
    uint32_t nmask =
        (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, newline));

    LineShape s;
    if (__builtin_expect(smask != 0 && nmask != 0, 1)) {
        s.city_len = ctz32(smask);
        s.line_len = ctz32(nmask);
        return s;
    }

    const char *q = p;
    while (*q != ';')
        ++q;
    s.city_len = (uint32_t)(q - p);
    while (*q != '\n')
        ++q;
    s.line_len = (uint32_t)(q - p);
    return s;
}

/* -------------------- worker parsing -------------------- */

static int worker_return_init(WorkerReturn *out, size_t table_size)
{
    memset(out, 0, sizeof(*out));
    out->table = (Entry *)kzalloc(table_size * sizeof(Entry), 64);
    out->meta = (EntryMeta *)kzalloc(table_size * sizeof(EntryMeta), 64);
    out->table_size = table_size;
    /* Full-table capacity up front: 256 KiB per worker from the bump
     * pool buys deleting the realloc path from the insert branch. */
    out->occupied_cap = table_size;
    out->occupied_indices =
        (size_t *)kalloc(table_size * sizeof(size_t), 64);
    out->occupied_count = 0;
    return 0;
}

/* Benchmark-loop reset: emptiness is decided by key_a alone, so wiping
 * the Entry table and the occupied list is a full reset; meta can stay
 * stale because it is only read for slots the probe already matched. */
static void worker_return_reset(WorkerReturn *out)
{
    memset(out->table, 0, out->table_size * sizeof(Entry));
    out->occupied_count = 0;
}

/* Parse one line's temperature+probe+update; identical to upstream
 * minus the realloc grow branch (capacity is the whole table). */
static FORCE_INLINE int process_line_at(const char *ptr, uint32_t city_len,
                                        WorkerReturn *out, size_t mask)
{
    const char *value_ptr = ptr + city_len + 1;

    int32_t temp10;
    int32_t neg;
    uint32_t dot;
    {
        uint64_t word;
        memcpy(&word, value_ptr, 8);

        neg = (int32_t)((word & 0xFF) == '-');
        word >>= (neg << 3);

        dot = (uint32_t)__builtin_ctzll(~word & 0x10101000ULL); /* 12 or 20 */
        uint64_t shifted = word << (28 - dot);
        uint64_t digits =
            (((shifted & 0x0F000F0F00ULL) * 0x640a0001ULL) >> 32) & 0x3FF;

        temp10 = ((int32_t)digits ^ -neg) + neg;
    }

    const int32_t mmpack = minmax_pack(temp10);
    const __m128i mmvec = _mm_shuffle_epi32(_mm_cvtsi32_si128(mmpack), 0);
    const __m128i inc = _mm_or_si128(_mm_cvtsi64_si128((int64_t)temp10),
                                     _mm_set_epi64x(1, 0));

    __m128i key;
    uint64_t h_city = hash_city(ptr, city_len, &key);
    size_t idx = (size_t)h_city & mask;
    for (;;) {
        Entry *e = &out->table[idx];
        __m128i ekey = _mm_load_si128((const __m128i *)&e->key_a);
        __m128i diff = _mm_xor_si128(ekey, key);
        if (_mm_test_all_zeros(diff, diff) &&
            (__builtin_expect(city_len <= 15, 1) ||
             city_long_eq(&out->meta[idx], ptr, city_len))) {
            __m128i stats = _mm_load_si128((const __m128i *)&e->sum10);
            stats = _mm_add_epi64(stats, inc);
            stats = _mm_blend_epi16(stats, _mm_min_epi16(stats, mmvec), 0xC0);
            _mm_store_si128((__m128i *)&e->sum10, stats);
            break;
        } else if (!e->key_a) {
            if (__builtin_expect(out->occupied_count + 1 >= out->table_size,
                                 0))
                return -1;
            _mm_store_si128((__m128i *)&e->key_a, key);
            e->count = 1;
            e->sum10 = temp10;
            e->minmax = mmpack;
            out->meta[idx].city_ptr = ptr;
            out->meta[idx].city_len = city_len;
            out->occupied_indices[out->occupied_count++] = idx;
            break;
        }
        idx = (idx + 1) & mask;
    }

    return 0;
}

static FORCE_INLINE const char *process_line(const char *ptr,
                                             WorkerReturn *out, size_t mask)
{
    const LineShape shape = find_shape(ptr);
    const char *const next_line = ptr + shape.line_len + 1;
    if (process_line_at(ptr, shape.city_len, out, mask) != 0)
        return NULL;
    return next_line;
}

/* Interleaved line streams per worker; 1 is the measured optimum since
 * the newline-mask line advance collapsed the loop-carried chain. See
 * upstream for the sweep table. */
#ifndef NSTREAMS
#define NSTREAMS 1
#endif

static int parse_range(const char *base, const WorkerRange *range,
                       WorkerReturn *out)
{
    const size_t mask = out->table_size - 1;

    const char *range_end = base + range->end;
    const char *cur[NSTREAMS];
    const char *stop[NSTREAMS];
    size_t len = range->end - range->start;
    cur[0] = base + range->start;
    for (int k = 1; k < NSTREAMS; ++k) {
        const char *guess = base + range->start + (len * (size_t)k) / NSTREAMS;
        const char *nl = (guess < range_end)
                             ? find_next_newline(guess, range_end)
                             : range_end;
        cur[k] = (nl < range_end) ? nl + 1 : range_end;
        stop[k - 1] = cur[k];
    }
    stop[NSTREAMS - 1] = range_end;

    for (;;) {
        bool all_live = true;
        UNROLL_STREAMS
        for (int k = 0; k < NSTREAMS; ++k)
            all_live &= (cur[k] < stop[k]);
        if (!all_live)
            break;
        UNROLL_STREAMS
        for (int k = 0; k < NSTREAMS; ++k)
            cur[k] = process_line(cur[k], out, mask);
        UNROLL_STREAMS
        for (int k = 0; k < NSTREAMS; ++k)
            if (!cur[k])
                return -3;
    }

    for (int k = 0; k < NSTREAMS; ++k) {
        while (cur[k] < stop[k]) {
            cur[k] = process_line(cur[k], out, mask);
            if (!cur[k])
                return -3;
        }
    }

    return 0;
}

/* Slots per worker table; power of two. 32768 holds the challenge's
 * 10,000-station ceiling at 30% load; see upstream for the sweep. */
#ifndef TABLE_SLOTS
#define TABLE_SLOTS 32768
#endif

#endif /* BRC_PARSE_H */
