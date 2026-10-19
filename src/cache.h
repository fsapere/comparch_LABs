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
    uint32_t *tag;
} cache_t;


// MSHR registers

typedef struct
{
    int valid;
    uint32_t addr;
    int done;
    uint32_t latency_cnt; // counter for variable latency in misses
    int req_from_memstage; // flag to see who sent the request 
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
int l2_probe(l2_cache_t *l2c, uint32_t addr, int *mshr_id);
int l2_cache_update(l2_cache_t *l2c, uint32_t addr, int mshr_id);

#endif
