/*
 * Computer Architecture - Professor Onur Mutlu
 *
 * MIPS pipeline timing simulator
 *
 * Chris Fallin, 2012
 */

#include "cache.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

//#define DEBUG
//#define TEST

#define log2(i) (__builtin_ffs( (i) ) - 1)

cache_t *cache_new(int sets, int ways, int block)
{
    cache_t *c = calloc(sizeof(cache_t), 1);

    c->sets = sets;
    c->ways = ways;
    c->log2sets = log2(sets);
    c->log2block = log2(block);

    c->present = calloc(sizeof(uint8_t), sets * ways);
    c->tag = calloc(sizeof(uint32_t), sets * ways);

    return c;
}

// L2 cache creation
l2_cache_t *l2_cache_new(int sets, int ways, int block)
{
    l2_cache_t *l2c = calloc(sizeof(l2_cache_t), 1);
    if (!l2c) return NULL; // null check

    l2c->data = cache_new(sets, ways, block); // data field creation, MSHR are 0 init by calloc
    return l2c;
}



void cache_destroy(cache_t *c)
{
    if (c) {
        free(c->present);
        free(c->tag);
        free(c);
    }
}

// L2 cache deallocation
void l2_cache_destroy(l2_cache_t *l2)
{
    // check if cache exist, destroy data first and then free the MSHR
    if (l2) {
        if (l2->data) {
            cache_destroy(l2->data);
        }
        free(l2);
    }
}

int cache_probe(cache_t *c, uint32_t addr)
{
    uint32_t block = (addr >> c->log2block);
    uint32_t set = block & ((1UL << c->log2sets) - 1);

    uint8_t *setpres = &c->present[c->ways * set];
    uint32_t *settag = &c->tag[c->ways * set];

    /* on a hit, promote the block to the MRU position (way 0) */
    for (int i = 0; i < c->ways; i++) {
        if (setpres[i] && (settag[i] == block)) {
            for (int j = i; j > 0; j--) {
                setpres[j] = setpres[j-1];
                settag[j] = settag[j-1];
            }
            setpres[0] = 1;
            settag[0] = block;
            return 1;
        }
    }

    return 0;
}

// Look for the first free MSHR, return index of first free, -1 if none is free

int l2_get_free_mshr(l2_cache_t *l2c)
{
    for (int i = 0; i < 16; i++) {
        if (!l2c->mshr[i].valid) {
            return i;
        }
    }
    return -1;
}


// Probe the L2 cache

int l2_probe(l2_cache_t *l2c, uint32_t addr, int* mshr_id, int req_from_memstage)
{
    int free_mshr = l2_get_free_mshr(l2c);
    // check MSHR availability
    if(free_mshr == -1) return -1; // can't probe if no MSHR is free
    
    // if MSHR is free, check hit/miss
    if(!cache_probe(l2c->data, addr))
    {// miss happened, allocate MSHR
        l2c->mshr[free_mshr].valid=1;
        l2c->mshr[free_mshr].done=0;
        l2c->mshr[free_mshr].addr=addr & ~31; // block address
        l2c->mshr[free_mshr].latency_cnt = 0; // miss triggered the cycle wait
        // before returning, save the MSHR id of the miss
        l2c->mshr[free_mshr].req_from_memstage = req_from_memstage;
        if (mshr_id) 
        {
            *mshr_id = free_mshr; // save index of first mshr to return it
        }

        return 0; // return 0 if you miss 
    } // otherwise  do nothing 
    return 1; // return 1 if you hit
}

int cache_update(cache_t *c, uint32_t addr)
{
    uint32_t block = (addr >> c->log2block);
    uint32_t set = block & ((1UL << c->log2sets) - 1);

    uint8_t *setpres = &c->present[c->ways * set];
    uint32_t *settag = &c->tag[c->ways * set];

    int hit = 0;

    int i;
    int firstempty = -1;
    for (i = 0; i < c->ways; i++) {
        if (setpres[i] && (settag[i] == block)) {
            int j;
            for (j = i; j > 0; j--) {
                setpres[j] = setpres[j-1];
                settag[j] = settag[j-1];
            }
            setpres[0] = 1;
            settag[0] = block;
            hit = 1;
            break;
        }
        if (!setpres[i] && firstempty == -1)
            firstempty = i;
    }

    if (!hit) {
        if (firstempty != -1) {
            for (i = firstempty; i > 0; i--) {
                setpres[i] = setpres[i-1];
                settag[i] = settag[i-1];
            }
            setpres[0] = 1;
            settag[0] = block;
            hit = 0;
        }
        else {
            for (i = c->ways - 1; i > 0; i--) {
                setpres[i] = setpres[i - 1];
                settag[i] = settag[i - 1];
            }
            setpres[0] = 1;
            settag[0] = block;
            hit = 0;
        }
    }

#ifdef DEBUG
    printf("Req (set %d) block %d: hit %d\nTags:", set, block, hit);
    for (i = 0; i < c->ways; i++)
        printf(" %d (pres %d)", settag[i], setpres[i]);
    printf("\n");
#endif

    return hit;
}


// L2 cache update and MSHR handling
int l2_cache_update(l2_cache_t *l2c, uint32_t addr, int mshr_id)
{
    // check if we hit on the L2 cache or not, update LRU and insert block in L2
    int hit = cache_update(l2c->data, addr);

    // free the MSR after the block is added (no more misses)
    if (mshr_id >= 0 && mshr_id < 16) 
    { // if MSHR id is valid, free up everything
        memset(&l2c->mshr[mshr_id], 0, sizeof(mshr_t));
    }
    return hit;
}

// DRAM notifies L2 cache that that it served MEMREQ, 
// reset cnt to 0 to track new 5 cycle latency
void l2_mem_fill_notify(l2_cache_t *l2c, int mshr_id)
{
    l2c->mshr[mshr_id].done = 1;
    l2c->mshr[mshr_id].latency_cnt = 0;
}

// this is only for handling the isolated cache, the cycle update of the cache
// is described as a function in the pipeline file

#ifdef TEST

int main()
{
    cache_t *c = cache_new(1, 4, 64);

    int seq[] = { 1, 2, 3, 4, 5, 1, 2, 3, 4, 5, 1, 2, 3, 4, 5, 2, 3, 4, 5 };

    int i;
    for (i = 0; i < sizeof(seq)/sizeof(int); i++) {
        cache_probe(c, 64 * seq[i]);
    }

    cache_destroy(c);
}

#endif


