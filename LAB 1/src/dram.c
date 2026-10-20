/*
    DRAM integration in the MIPS pipeline simulator
*/

#include "cache.h"
#include "dram.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// create/destroy DRAM

dram_t *dram_new()
{
    dram_t *d = calloc(1, sizeof(dram_t));
    if(!d) return NULL; 
    // close all rows by default
    for(int i=0;i<DRAM_NUM_BANKS;i++)
    {
        d->banks[i].open_row=-1;
    }
    return d;
}

void dram_destroy(dram_t *d)
{
    if(d)
    {
        free(d);
    }
}

// Check if you can schedule something on the bus for the given cycles

int dram_bus_isfree(bus_slot_t *bus, uint32_t start, int len)
{
    for(int i=0;i<len;i++)
    {
        int idx = (start + i) % DRAM_BUS_BUFFER_LEN; // circular buffer
        if(bus[idx].cmd != CMD_FREE)
        {
            return 0;
        }
    }
    return 1; 
}

// populate the slots of the buffer

void dram_bus_reserve(bus_slot_t *bus, uint32_t start, int len,
                      dram_cmd_t cmd, int mshr_id, int bank)
{
    for (int i = 0; i < len; i++)
    {
        int idx = (start + i) % DRAM_BUS_BUFFER_LEN;
        bus[idx].cmd     = cmd;
        bus[idx].mshr_id = mshr_id;
        bus[idx].bank    = bank;
    }
}


// return row buffer status
rb_status_t dram_rb_status(dram_t *d, dram_req_t *r)
{
    int bank = DRAM_BANK(r->addr);
    int row  = DRAM_ROW(r->addr);
    int open_row = d->banks[bank].open_row;
    if(open_row == -1){
        return RB_MISS;
    } else if(open_row == row){
        return RB_HIT;
    } else{
        return RB_CONFLICT;
    }
}

// check if the req towards DRAM is schedulable according to the BUS UTIL buffer
// policy from PDF -> based on RB status, check if you can occupy bus in M cycles for a duration of N
int dram_is_schedulable(dram_t *d, dram_req_t *r, uint32_t now)
{
    int bank = DRAM_BANK(r->addr);
    // if the bank is not free now, we cannot schedule anything
    if (now < d->banks[bank].free_at) {
        return 0;
    }

    // read the row buffer status
    rb_status_t status = dram_rb_status(d, r);

    if (status == RB_HIT) {
        // READ at 'now' (4 cycles on cmd bus)
        if (!dram_bus_isfree(d->cmd_bus, now, DRAM_CMD_CYCLES)) {
            return 0;
        }
        // Data transfer starts 100 cycles after READ (50 cycles on data bus)
        if (!dram_bus_isfree(d->data_bus, now + 100, DRAM_DATA_CYCLES)) {
            return 0;
        }
        return 1;
    } else if (status == RB_MISS) {
        // ACTIVATE at 'now' (4 cycles on cmd bus)
        if (!dram_bus_isfree(d->cmd_bus, now, DRAM_CMD_CYCLES)) {
            return 0;
        }
        // READ at 'now + 100' (4 cycles on cmd bus)
        if (!dram_bus_isfree(d->cmd_bus, now + 100, DRAM_CMD_CYCLES)) {
            return 0;
        }
        // Data transfer starts 100 cycles after READ -> 'now + 200' (50 cycles on data bus)
        if (!dram_bus_isfree(d->data_bus, now + 200, DRAM_DATA_CYCLES)) {
            return 0;
        }
        return 1;
    } else { // RB_CONFLICT
        // PRECHARGE at 'now' (4 cycles on cmd bus)
        if (!dram_bus_isfree(d->cmd_bus, now, DRAM_CMD_CYCLES)) {
            return 0;
        }
        // ACTIVATE at 'now + 100' (4 cycles on cmd bus)
        if (!dram_bus_isfree(d->cmd_bus, now + 100, DRAM_CMD_CYCLES)) {
            return 0;
        }
        // READ at 'now + 200' (4 cycles on cmd bus)
        if (!dram_bus_isfree(d->cmd_bus, now + 200, DRAM_CMD_CYCLES)) {
            return 0;
        }
        // Data transfer starts 100 cycles after READ -> 'now + 300' (50 cycles on data bus)
        if (!dram_bus_isfree(d->data_bus, now + 300, DRAM_DATA_CYCLES)) {
            return 0;
        }
        return 1;
    }
}

// schedule the request (just operation, the policy that calls this is handled in the "cycle" funct)
void dram_schedule(dram_t *d, dram_req_t *r, uint32_t now)
{
    int bank = DRAM_BANK(r->addr);
    int row  = DRAM_ROW(r->addr);
    rb_status_t status = dram_rb_status(d, r);

    if (status == RB_HIT) {
        // reserve slots both on data and cmd bus according to specs of the PDF based on RB
        dram_bus_reserve(d->cmd_bus, now, DRAM_CMD_CYCLES, CMD_READ, r->mshr_id, bank);
        dram_bus_reserve(d->data_bus, now + 100, DRAM_DATA_CYCLES, CMD_READ, r->mshr_id, bank);
        // set the bank as busy for the next 100 cycles
        d->banks[bank].free_at = now + 100;
        // update when request will be done
        r->done_cycle = now + 100 + DRAM_DATA_CYCLES - 1;
    } else if (status == RB_MISS) {
        dram_bus_reserve(d->cmd_bus, now, DRAM_CMD_CYCLES, CMD_ACT, r->mshr_id, bank);
        dram_bus_reserve(d->cmd_bus, now + 100, DRAM_CMD_CYCLES, CMD_READ, r->mshr_id, bank);
        dram_bus_reserve(d->data_bus, now + 200, DRAM_DATA_CYCLES, CMD_READ, r->mshr_id, bank);
        d->banks[bank].free_at = now + 200;
        r->done_cycle = now + 200 + DRAM_DATA_CYCLES - 1;
    } else { 
        dram_bus_reserve(d->cmd_bus, now, DRAM_CMD_CYCLES, CMD_PRE, r->mshr_id, bank);
        dram_bus_reserve(d->cmd_bus, now + 100, DRAM_CMD_CYCLES, CMD_ACT, r->mshr_id, bank);
        dram_bus_reserve(d->cmd_bus, now + 200, DRAM_CMD_CYCLES, CMD_READ, r->mshr_id, bank);
        dram_bus_reserve(d->data_bus, now + 300, DRAM_DATA_CYCLES, CMD_READ, r->mshr_id, bank);
        d->banks[bank].free_at = now + 300;
        r->done_cycle = now + 300 + DRAM_DATA_CYCLES - 1;
    }

    // Open-Row Policy: row is now open in row buffer
    d->banks[bank].open_row = row;
    r->scheduled = 1;
}


// add a read to the request queue
void dram_enqueue_read(dram_t *d, uint32_t addr, int mshr_id,
                       int req_from_memstage, uint32_t now)
{
    for (int i = 0; i < DRAM_REQ_QUEUE_SIZE; i++) {
        if (!d->queue[i].valid) {
            dram_req_t *req = &d->queue[i];
            req->valid             = 1;
            req->type              = CMD_READ;
            req->addr              = addr;
            req->mshr_id           = mshr_id;
            req->req_from_memstage = req_from_memstage;
            req->arrival_cycle     = now;
            req->req_id            = d->next_req_id++; // tie-break for multiple requests in same cycle
            req->scheduled         = 0;
            req->done_cycle        = 0;
            return;
        }
    }
}

// Compare two candidates according to FR-FCFS policy (PDF Section 5)
// Returns 1 if 'cand' has higher priority than 'best', 0 otherwise
static int dram_req_better(dram_t *d, dram_req_t *cand, dram_req_t *best)
{
    // Rule 1: Row buffer hits prioritized over others
    int cand_hit = (dram_rb_status(d, cand) == RB_HIT);
    int best_hit = (dram_rb_status(d, best) == RB_HIT);
    if (cand_hit != best_hit) {
        return cand_hit > best_hit;
    }

    // Rule 2: Earlier arrival cycle
    if (cand->arrival_cycle != best->arrival_cycle) {
        return cand->arrival_cycle < best->arrival_cycle;
    }

    // Rule 3: Memory stage prioritized over fetch stage
    if (cand->req_from_memstage != best->req_from_memstage) {
        return cand->req_from_memstage > best->req_from_memstage;
    }

    // Rule 5: Tie-break on creation order (lower req_id)
    return cand->req_id < best->req_id;
}

// Simulate one cycle of DRAM operation
void dram_cycle(dram_t *d, l2_cache_t *l2c, uint32_t now)
{
    // 1. Completion: Notify L2 if data transfer completed at cycle 'now', then clear
    for (int i = 0; i < DRAM_REQ_QUEUE_SIZE; i++) {
        dram_req_t *req = &d->queue[i];
        if (!req->valid) continue;

        if (req->scheduled && req->done_cycle == now) {
            l2_mem_fill_notify(l2c, req->mshr_id);
            req->valid = 0; // cleared without copying or shifting
        }
    }

    // 2. Scheduling: Find the best schedulable request using FR-FCFS
    int best_idx = -1;
    for (int i = 0; i < DRAM_REQ_QUEUE_SIZE; i++) {
        dram_req_t *req = &d->queue[i];
        if (!req->valid || req->scheduled) continue;

        if (!dram_is_schedulable(d, req, now)) continue;

        if (best_idx == -1 || dram_req_better(d, req, &d->queue[best_idx])) {
            best_idx = i;
        }
    }

    // If a request is schedulable and won the FR-FCFS arbitration, schedule it!
    if (best_idx != -1) {
        dram_schedule(d, &d->queue[best_idx], now);
    }

    // 3. Clear ring buffer slots of the elapsed cycle 'now'
    d->cmd_bus[now % DRAM_BUS_BUFFER_LEN] = (bus_slot_t){ CMD_FREE, -1, -1 };
    d->data_bus[now % DRAM_BUS_BUFFER_LEN] = (bus_slot_t){ CMD_FREE, -1, -1 };
}