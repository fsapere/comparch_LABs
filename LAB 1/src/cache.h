/*
 * Computer Architecture - Professor Onur Mutlu
 *
 * MIPS pipeline timing simulator
 *
 * Chris Fallin, 2012
 */

#ifndef _CACHE_H_
#define _CACHE_H_

#include <stdint.h>

typedef struct
{
    int sets, ways;
    int log2sets;
    int log2block;

    uint8_t *present;
    uint8_t *dirty; // track dirty bits
    uint32_t *tag;
    // track status of last eviction, use after update (eviction happened? was it dirty?)
    int evicted_wasdirty; // 0 for no eviction or not dirty, 1 otherwise
    uint32_t evicted_addr;
} cache_t;


// MSHR registers

typedef struct
{
    int valid;
    uint32_t addr;
    int done;
    uint32_t latency_cnt; // Task 1, variable latency tracking (==THR -> fill)
    int req_from_memstage; // Task 2, where is the miss coming from? FETCH vs. MEM
} mshr_t;

// L2 cache struct definition
typedef struct
{
    cache_t *data;
    mshr_t mshr[16];
} l2_cache_t;

cache_t *cache_new(int sets, int ways, int block);
void cache_destroy(cache_t *c);

int cache_probe(cache_t *c, uint32_t addr);
int cache_update(cache_t *c, uint32_t addr);

// L2 cache creation/deallocation

l2_cache_t *l2_cache_new(int sets, int ways, int block);
void l2_cache_destroy(l2_cache_t *c);

// L2 get first free MSHR
int l2_get_free_mshr(l2_cache_t *l2c);

// L2 cache probe and update
int l2_probe(l2_cache_t *l2c, uint32_t addr, int *mshr_id, int req_from_memstage);
int l2_cache_update(l2_cache_t *l2c, uint32_t addr, int mshr_id);

// Function to implement pipe integration of the l2 cache
void l2_cycle(l2_cache_t *l2c);

// Function to refresh the L2  latency counter when the req has been served by DRAM
void l2_mem_fill_notify(l2_cache_t *l2c, int mshr_id);

// Function to mark a cache block as dirty
int cache_mark_dirty(cache_t *c, uint32_t addr); 

// Function to writeback L1->L2
void l1_to_l2_writeback(l2_cache_t *l2c, uint32_t evicted_addr);


#endif

