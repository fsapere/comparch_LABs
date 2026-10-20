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
    dram_req_t *req = &d->queue[d->num_reqs];
    req->addr              = addr;
    req->mshr_id           = mshr_id;
    req->req_from_memstage = req_from_memstage;
    req->arrival_cycle     = now;
    req->req_id            = d->next_req_id++; // handle multiple req coming in same cycle
    req->scheduled         = 0;
    req->done_cycle        = 0;
    d->num_reqs++; // update index for the next enqueue
}

// FCFS policy scheduling, chooses which r is passed to the scheduling
// function according to the conditions