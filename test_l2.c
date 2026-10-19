/*
 * Unit tests for the L2 cache (Lab 1, Task 1) -- functional behavior only.
 *
 * Covers the "instantaneous" part of the spec (organization, set indexing,
 * MSHR allocation/free, MRU insertion, true LRU, mem-before-fetch ordering).
 * Timing (14-cycle hit, 5 + DRAM + 5 cycle miss) is NOT tested here: it
 * belongs to l2_cycle() / the pipeline, which do not exist yet.
 *
 * Build & run (from /home/fsapere/comparch_labs):
 *   gcc -g -Wall -I"LAB 1/src" test_l2.c "LAB 1/src/cache.c" -o test_l2 && ./test_l2
 *
 * Keep this file OUTSIDE src/: the Makefile compiles every .c in src/ and a second
 * main() would break the simulator build.


 REMEMBER TO CHECK THIS AFTER YOU INTEGRATE THE L2 CYCLE STUFF or TO REMOVE IT
 COMPLETELY AND CHECK EVERYTHING TOGETHER
 */

#include "cache.h"
#include <stdio.h>
#include <string.h>

#define SETS     512
#define WAYS     16
#define BLOCK    32
#define NUM_MSHR 16

/* ------------------------------------------------------------------ */
/* Minimal test framework: count failures instead of aborting          */
/* ------------------------------------------------------------------ */

static int g_checks = 0, g_failed_checks = 0;
static int g_tests = 0, g_failed_tests = 0;
static int t_failed = 0;

#define CHECK(cond, ...)                                                  \
    do {                                                                  \
        g_checks++;                                                       \
        if (!(cond)) {                                                    \
            g_failed_checks++;                                            \
            t_failed++;                                                   \
            printf("    FAIL (line %d): %s -- ", __LINE__, #cond);        \
            printf(__VA_ARGS__);                                          \
            printf("\n");                                                 \
        }                                                                 \
    } while (0)

#define RUN(fn)                                                           \
    do {                                                                  \
        t_failed = 0;                                                     \
        l2_cache_t *l2_ = l2_cache_new(SETS, WAYS, BLOCK);                \
        if (!l2_) { printf("[FAIL] %s: l2_cache_new returned NULL\n", #fn); \
                    g_tests++; g_failed_tests++; break; }                 \
        fn(l2_);                                                          \
        l2_cache_destroy(l2_);                                            \
        g_tests++;                                                        \
        if (t_failed) g_failed_tests++;                                   \
        printf("[%s] %s\n", t_failed ? "FAIL" : " OK ", #fn);             \
    } while (0)

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Build an address with a given set index (bits [13:5]) and tag (bits [31:14]) */
static uint32_t mk_addr(uint32_t set, uint32_t tag)
{
    return (tag << 14) | (set << 5);
}

/* Way (LRU position: 0 = MRU) of the block containing addr, or -1 if absent.
 * Reads the tag array directly so it has NO side effects on LRU state. */
static int way_of(l2_cache_t *l2, uint32_t addr)
{
    uint32_t block = addr >> 5;
    uint32_t set = block & (SETS - 1);
    for (int w = 0; w < WAYS; w++) {
        int idx = set * WAYS + w;
        if (l2->data->present[idx] && l2->data->tag[idx] == block)
            return w;
    }
    return -1;
}

/* Number of valid blocks in a set */
static int count_present(l2_cache_t *l2, uint32_t set)
{
    int n = 0;
    for (int w = 0; w < WAYS; w++)
        n += l2->data->present[set * WAYS + w] ? 1 : 0;
    return n;
}

/* Insert a block without touching MSHRs (models a completed fill) */
static void fill(l2_cache_t *l2, uint32_t addr)
{
    l2_cache_update(l2, addr, -1);
}

static int mshr_is_zero(const mshr_t *m)
{
    mshr_t z;
    memset(&z, 0, sizeof(z));
    return memcmp(m, &z, sizeof(z)) == 0;
}

static int count_valid_mshr(l2_cache_t *l2)
{
    int n = 0;
    for (int i = 0; i < NUM_MSHR; i++)
        n += l2->mshr[i].valid ? 1 : 0;
    return n;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/* 256KB, 16-way, 32B blocks -> 512 sets; cache empty, MSHRs zeroed */
static void test_init(l2_cache_t *l2)
{
    CHECK(l2->data != NULL, "data array not allocated");
    CHECK(l2->data->sets == SETS, "sets = %d", l2->data->sets);
    CHECK(l2->data->ways == WAYS, "ways = %d", l2->data->ways);
    CHECK(l2->data->log2sets == 9, "log2sets = %d", l2->data->log2sets);
    CHECK(l2->data->log2block == 5, "log2block = %d", l2->data->log2block);

    int any_present = 0;
    for (int i = 0; i < SETS * WAYS; i++)
        any_present |= l2->data->present[i];
    CHECK(!any_present, "cache must start empty");

    for (int i = 0; i < NUM_MSHR; i++)
        CHECK(mshr_is_zero(&l2->mshr[i]), "MSHR %d not zero-initialized", i);

    CHECK(l2_get_free_mshr(l2) == 0, "first free MSHR = %d", l2_get_free_mshr(l2));
}

/* Set index = addr[13:5]; offset bits [4:0] ignored */
static void test_set_index_bits_13_5(l2_cache_t *l2)
{
    uint32_t a = 0x00003FE0; /* bits 13:5 all ones -> set 511, tag 0 */
    fill(l2, a);
    CHECK(l2->data->present[511 * WAYS + 0], "0x3FE0 should map to set 511");

    uint32_t b = 0x00004000; /* bit 14 set, bits 13:5 zero -> set 0, tag 1 */
    fill(l2, b);
    CHECK(l2->data->present[0 * WAYS + 0], "0x4000 should map to set 0");

    CHECK(l2_probe(l2, a | 0x1F, NULL, 0) == 1, "same block, last byte must hit");
    CHECK(l2_probe(l2, a | 0x04, NULL, 0) == 1, "same block, other word must hit");
    CHECK(l2_probe(l2, a + 0x4000, NULL, 0) == 0, "same set, different tag must miss");
}

/* Miss: return 0, allocate first free MSHR {valid=1, block addr, done=0}.
 * The block is NOT inserted at probe time (only when the fill completes). */
static void test_miss_allocates_mshr(l2_cache_t *l2)
{
    uint32_t addr = 0x10000004;
    int id = -77;
    int r = l2_probe(l2, addr, &id, 1);

    CHECK(r == 0, "probe on empty cache returned %d (expected 0 = miss)", r);
    CHECK(id == 0, "mshr_id = %d (expected 0)", id);
    CHECK(l2->mshr[0].valid == 1, "MSHR valid bit not set");
    CHECK(l2->mshr[0].done == 0, "MSHR done bit must start at 0");
    CHECK(l2->mshr[0].addr == 0x10000000, "MSHR addr = 0x%08x (expected block-aligned 0x10000000)",
          l2->mshr[0].addr);
    CHECK(l2->mshr[0].latency_cnt == 0, "latency_cnt = %u", l2->mshr[0].latency_cnt);
    CHECK(l2->mshr[0].req_from_memstage == 1, "req_from_memstage = %d", l2->mshr[0].req_from_memstage);
    CHECK(way_of(l2, addr) == -1, "block must not be inserted on probe");
    CHECK(l2_get_free_mshr(l2) == 1, "next free MSHR = %d", l2_get_free_mshr(l2));

    for (int i = 1; i < NUM_MSHR; i++)
        CHECK(mshr_is_zero(&l2->mshr[i]), "MSHR %d touched by a single miss", i);
}

/* Hit: return 1, NO MSHR allocated, mshr_id untouched */
static void test_hit_does_not_allocate_mshr(l2_cache_t *l2)
{
    uint32_t a = mk_addr(10, 1);
    fill(l2, a);

    int id = -77;
    int r = l2_probe(l2, a, &id, 0);
    CHECK(r == 1, "probe returned %d (expected 1 = hit)", r);
    CHECK(id == -77, "mshr_id modified on hit (%d)", id);
    CHECK(count_valid_mshr(l2) == 0, "%d MSHRs valid after a hit", count_valid_mshr(l2));

    /* NULL mshr_id must be tolerated on both hit and miss */
    CHECK(l2_probe(l2, a, NULL, 0) == 1, "hit with NULL mshr_id");
    CHECK(l2_probe(l2, mk_addr(10, 2), NULL, 0) == 0, "miss with NULL mshr_id");
    CHECK(l2->mshr[0].valid == 1, "miss with NULL mshr_id must still allocate");
}

/* Fill completion: block inserted at MRU, its MSHR freed (all fields 0) */
static void test_fill_inserts_mru_and_frees_mshr(l2_cache_t *l2)
{
    uint32_t old = mk_addr(5, 1);
    uint32_t a = mk_addr(5, 2);
    fill(l2, old);

    int id = -1;
    l2_probe(l2, a, &id, 0);
    CHECK(id >= 0, "miss did not allocate");
    if (id < 0) return;

    l2_cache_update(l2, a, id);
    CHECK(way_of(l2, a) == 0, "new block at way %d (expected MRU = 0)", way_of(l2, a));
    CHECK(way_of(l2, old) == 1, "older block at way %d (expected 1)", way_of(l2, old));
    CHECK(mshr_is_zero(&l2->mshr[id]), "MSHR %d not cleared after fill", id);
    CHECK(l2_get_free_mshr(l2) == id, "freed MSHR not reusable");
    CHECK(l2_probe(l2, a, NULL, 0) == 1, "block must hit after fill");
}

/* update with mshr_id = -1 must not touch any MSHR */
static void test_update_without_mshr(l2_cache_t *l2)
{
    int id = -1;
    l2_probe(l2, mk_addr(1, 1), &id, 0);
    fill(l2, mk_addr(2, 1));
    CHECK(id == 0 && l2->mshr[0].valid == 1, "unrelated MSHR freed by update(-1)");
    CHECK(count_valid_mshr(l2) == 1, "valid MSHRs = %d", count_valid_mshr(l2));
}

/* Fetch and mem miss at the same time: two MSHRs; they can complete out of order */
static void test_two_outstanding_misses(l2_cache_t *l2)
{
    uint32_t a = mk_addr(30, 1); /* memory stage */
    uint32_t b = mk_addr(31, 1); /* fetch stage  */
    int id_a = -1, id_b = -1;

    CHECK(l2_probe(l2, a, &id_a, 1) == 0, "mem miss");
    CHECK(l2_probe(l2, b, &id_b, 0) == 0, "fetch miss");
    CHECK(id_a == 0 && id_b == 1, "ids = %d, %d (expected 0, 1)", id_a, id_b);
    CHECK(l2->mshr[0].addr == a && l2->mshr[1].addr == b, "MSHR addresses mixed up");
    CHECK(l2->mshr[0].req_from_memstage == 1, "mem stage flag not set");
    CHECK(l2->mshr[1].req_from_memstage == 0, "fetch stage flag not set");

    /* fetch's request completes first */
    l2_cache_update(l2, b, id_b);
    CHECK(l2->mshr[id_b].valid == 0, "fetch MSHR not freed");
    CHECK(l2->mshr[id_a].valid == 1, "mem MSHR freed by the wrong fill");

    int id_c = -1;
    l2_probe(l2, mk_addr(32, 1), &id_c, 0);
    CHECK(id_c == id_b, "new miss got MSHR %d (expected the freed one, %d)", id_c, id_b);
}

/* No free MSHR -> cannot even probe: return -1, even if it would hit,
 * and the LRU state must not change */
static void test_mshr_full_blocks_probe(l2_cache_t *l2)
{
    uint32_t h = mk_addr(7, 1);
    uint32_t g = mk_addr(7, 2);
    fill(l2, h);
    fill(l2, g); /* g = MRU, h = way 1 */

    for (int i = 0; i < NUM_MSHR; i++) {
        int id = -1;
        int r = l2_probe(l2, mk_addr(100 + i, 1), &id, 0);
        CHECK(r == 0 && id == i, "miss #%d: r=%d id=%d", i, r, id);
    }
    CHECK(l2_get_free_mshr(l2) == -1, "all MSHRs should be busy");

    int id = -77;
    int r = l2_probe(l2, h, &id, 0);
    CHECK(r == -1, "probe of resident block with full MSHRs returned %d (expected -1)", r);
    CHECK(id == -77, "mshr_id modified on refused probe");
    CHECK(way_of(l2, h) == 1, "refused probe promoted the block (way %d)", way_of(l2, h));

    r = l2_probe(l2, mk_addr(200, 1), &id, 0);
    CHECK(r == -1, "probe of absent block with full MSHRs returned %d (expected -1)", r);
    CHECK(count_valid_mshr(l2) == NUM_MSHR, "MSHR count changed on refused probe");
}

/* A freed MSHR can be re-allocated in the same cycle */
static void test_freed_mshr_reused_same_cycle(l2_cache_t *l2)
{
    for (int i = 0; i < NUM_MSHR; i++)
        l2_probe(l2, mk_addr(100 + i, 1), NULL, 0);

    l2_cache_update(l2, l2->mshr[7].addr, 7);
    CHECK(l2_get_free_mshr(l2) == 7, "free MSHR = %d (expected 7)", l2_get_free_mshr(l2));

    int id = -1;
    uint32_t n = mk_addr(300 & (SETS - 1), 5);
    int r = l2_probe(l2, n, &id, 0);
    CHECK(r == 0 && id == 7, "r=%d id=%d (expected miss in MSHR 7)", r, id);
    CHECK(l2->mshr[7].addr == (n & ~31u), "MSHR 7 addr = 0x%08x", l2->mshr[7].addr);
}

/* Insert at MRU, promote to MRU on hit */
static void test_mru_insertion_and_promotion(l2_cache_t *l2)
{
    uint32_t a = mk_addr(42, 1), b = mk_addr(42, 2), c = mk_addr(42, 3);
    fill(l2, a);
    fill(l2, b);
    fill(l2, c);
    CHECK(way_of(l2, c) == 0 && way_of(l2, b) == 1 && way_of(l2, a) == 2,
          "after A,B,C: C=%d B=%d A=%d (expected 0,1,2)", way_of(l2, c), way_of(l2, b), way_of(l2, a));

    l2_probe(l2, a, NULL, 0);
    CHECK(way_of(l2, a) == 0 && way_of(l2, c) == 1 && way_of(l2, b) == 2,
          "after hit A: A=%d C=%d B=%d (expected 0,1,2)", way_of(l2, a), way_of(l2, c), way_of(l2, b));

    l2_probe(l2, b, NULL, 0);
    CHECK(way_of(l2, b) == 0 && way_of(l2, a) == 1 && way_of(l2, c) == 2,
          "after hit B: B=%d A=%d C=%d (expected 0,1,2)", way_of(l2, b), way_of(l2, a), way_of(l2, c));
}

/* True LRU: 17th block evicts the least recently used one */
static void test_true_lru_eviction(l2_cache_t *l2)
{
    const uint32_t s = 9;
    for (uint32_t t = 0; t < WAYS; t++)
        fill(l2, mk_addr(s, t));

    for (uint32_t t = 0; t < WAYS; t++)
        CHECK(way_of(l2, mk_addr(s, t)) == (int)(WAYS - 1 - t),
              "tag %u at way %d (expected %u)", t, way_of(l2, mk_addr(s, t)), WAYS - 1 - t);

    fill(l2, mk_addr(s, 16));
    CHECK(way_of(l2, mk_addr(s, 0)) == -1, "LRU block (tag 0) not evicted");
    CHECK(way_of(l2, mk_addr(s, 16)) == 0, "new block not at MRU");
    CHECK(way_of(l2, mk_addr(s, 1)) == WAYS - 1, "tag 1 should now be LRU");
    CHECK(count_present(l2, s) == WAYS, "set has %d blocks", count_present(l2, s));

    /* touching the LRU block saves it: the next one gets evicted instead */
    l2_probe(l2, mk_addr(s, 1), NULL, 0);
    fill(l2, mk_addr(s, 17));
    CHECK(way_of(l2, mk_addr(s, 1)) >= 0, "recently used block (tag 1) was evicted");
    CHECK(way_of(l2, mk_addr(s, 2)) == -1, "tag 2 should have been evicted");
}

/* Same-cycle hits in the same set: mem first, then fetch
 * -> fetch block at MRU, mem block at MRU-1 */
static void test_same_cycle_mem_then_fetch(l2_cache_t *l2)
{
    uint32_t x = mk_addr(20, 1), y = mk_addr(20, 2), z = mk_addr(20, 3);
    fill(l2, x);
    fill(l2, y);
    fill(l2, z); /* order: Z, Y, X */

    CHECK(l2_probe(l2, x, NULL, 1) == 1, "mem-stage hit"); /* memory stage first */
    CHECK(l2_probe(l2, y, NULL, 0) == 1, "fetch-stage hit"); /* then fetch stage  */

    CHECK(way_of(l2, y) == 0, "fetch block at way %d (expected MRU = 0)", way_of(l2, y));
    CHECK(way_of(l2, x) == 1, "mem block at way %d (expected MRU-1 = 1)", way_of(l2, x));
    CHECK(way_of(l2, z) == 2, "untouched block at way %d (expected 2)", way_of(l2, z));
}

/* Activity in one set must not affect other sets */
static void test_sets_independent(l2_cache_t *l2)
{
    uint32_t other = mk_addr(4, 99);
    fill(l2, other);
    for (uint32_t t = 0; t < WAYS + 1; t++)
        fill(l2, mk_addr(3, t));

    CHECK(way_of(l2, other) == 0, "block in set 4 disturbed (way %d)", way_of(l2, other));
    CHECK(count_present(l2, 3) == WAYS, "set 3 has %d blocks", count_present(l2, 3));
    CHECK(count_present(l2, 4) == 1, "set 4 has %d blocks", count_present(l2, 4));
}

/* Two fills of the same block (e.g. fetch and mem missed on it) must not
 * create duplicate copies in the set */
static void test_fill_same_block_twice_no_duplicate(l2_cache_t *l2)
{
    uint32_t a = mk_addr(50, 1);
    int id1 = -1, id2 = -1;
    l2_probe(l2, a, &id1, 0);
    l2_probe(l2, a, &id2, 0);

    if (id1 >= 0) l2_cache_update(l2, a, id1);
    if (id2 >= 0) l2_cache_update(l2, a, id2);

    CHECK(count_present(l2, 50) == 1, "set holds %d blocks (expected 1)", count_present(l2, 50));
    CHECK(count_valid_mshr(l2) == 0, "%d MSHRs still valid", count_valid_mshr(l2));
}

/* ------------------------------------------------------------------ */

int main(void)
{
    printf("=== L2 cache unit tests ===\n");

    RUN(test_init);
    RUN(test_set_index_bits_13_5);
    RUN(test_miss_allocates_mshr);
    RUN(test_hit_does_not_allocate_mshr);
    RUN(test_fill_inserts_mru_and_frees_mshr);
    RUN(test_update_without_mshr);
    RUN(test_two_outstanding_misses);
    RUN(test_mshr_full_blocks_probe);
    RUN(test_freed_mshr_reused_same_cycle);
    RUN(test_mru_insertion_and_promotion);
    RUN(test_true_lru_eviction);
    RUN(test_same_cycle_mem_then_fetch);
    RUN(test_sets_independent);
    RUN(test_fill_same_block_twice_no_duplicate);

    printf("\n%d/%d tests passed (%d/%d checks)\n",
           g_tests - g_failed_tests, g_tests,
           g_checks - g_failed_checks, g_checks);
    return g_failed_tests ? 1 : 0;
}
