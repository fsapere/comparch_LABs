/*
    DRAM integration in the MIPS pipeline simulator
*/

#include "cache.h"
#include "dram.h"
#include "pipe.h"
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

    // bank blocking: if the bank has a pending refresh request, block the scheduling
    // of any command to that bank
    if (r->type != CMD_REFRESH) {
        for (int i = 0; i < DRAM_REQ_QUEUE_SIZE; i++) {
            if (d->queue[i].valid && !d->queue[i].scheduled && 
                d->queue[i].type == CMD_REFRESH && DRAM_BANK(d->queue[i].addr) == bank) 
                {
                    return 0; // not schedulable
                }
        }
    }

    // read the row buffer status
    rb_status_t status = dram_rb_status(d, r);
    // check schedulability N cycles from now of the REFRESH commands
    if (r->type == CMD_REFRESH) {
        if (d->banks[bank].open_row == -1) {
            // CLOSED RB: ACT 'now'+4, PRE 'now + 100' for 4 cycles
            if (!dram_bus_isfree(d->cmd_bus, now, DRAM_CMD_CYCLES)) {
                return 0;
            }
            if (!dram_bus_isfree(d->cmd_bus, now + 100, DRAM_CMD_CYCLES)) {
                return 0;
            }
            return 1;
        } else {
            // OPEN RB: PRE 'now', ACT 'now + 100', PRE 'now + 200'
            if (!dram_bus_isfree(d->cmd_bus, now, DRAM_CMD_CYCLES)) {
                return 0;
            }
            if (!dram_bus_isfree(d->cmd_bus, now + 100, DRAM_CMD_CYCLES)) {
                return 0;
            }
            if (!dram_bus_isfree(d->cmd_bus, now + 200, DRAM_CMD_CYCLES)) {
                return 0;
            }
            return 1;
        }
    }


    // check schedulability N cycles from now of the R/W commands
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
    dram_cmd_t dram_cmd = r->type; // CMD_READ/WRITE/REFRESH

    if(dram_cmd == CMD_REFRESH)
    {// apply same policy you used to check for schedulability, now to reserve slots on the bus
        if (status == RB_MISS) 
        {// row buffer closed, ACT now, PRE now + 100
            dram_bus_reserve(d->cmd_bus, now, DRAM_CMD_CYCLES, CMD_ACT, -1, bank);
            dram_bus_reserve(d->cmd_bus, now + 100, DRAM_CMD_CYCLES, CMD_PRE, -1, bank);
            d->banks[bank].free_at = now + 200;
            r->done_cycle = now + 200 - 1;
        } else 
        {// rb open PRE now, ACT now + 100, PRE now + 200
            dram_bus_reserve(d->cmd_bus, now, DRAM_CMD_CYCLES, CMD_PRE, -1, bank);
            dram_bus_reserve(d->cmd_bus, now + 100, DRAM_CMD_CYCLES, CMD_ACT, -1, bank);
            dram_bus_reserve(d->cmd_bus, now + 200, DRAM_CMD_CYCLES, CMD_PRE, -1, bank);
            d->banks[bank].free_at = now + 300;
            r->done_cycle = now + 300 - 1;
        }
        d->banks[bank].open_row = -1; // row buffer sempre chiuso dopo refresh!
        r->scheduled = 1;
        return;
    }

    if (status == RB_HIT) 
    {
        // reserve slots both on data and cmd bus according to specs of the PDF based on RB
        dram_bus_reserve(d->cmd_bus, now, DRAM_CMD_CYCLES, dram_cmd, r->mshr_id, bank);
        dram_bus_reserve(d->data_bus, now + 100, DRAM_DATA_CYCLES, dram_cmd, r->mshr_id, bank);
        // set the bank as busy for the next 100 cycles
        d->banks[bank].free_at = now + 100;
        // update when request will be done
        r->done_cycle = now + 100 + DRAM_DATA_CYCLES - 1;
    } else if (status == RB_MISS) 
    {
        dram_bus_reserve(d->cmd_bus, now, DRAM_CMD_CYCLES, CMD_ACT, r->mshr_id, bank);
        dram_bus_reserve(d->cmd_bus, now + 100, DRAM_CMD_CYCLES, dram_cmd, r->mshr_id, bank);
        dram_bus_reserve(d->data_bus, now + 200, DRAM_DATA_CYCLES, dram_cmd, r->mshr_id, bank);
        d->banks[bank].free_at = now + 200;
        r->done_cycle = now + 200 + DRAM_DATA_CYCLES - 1;
    } else 
    { 
        dram_bus_reserve(d->cmd_bus, now, DRAM_CMD_CYCLES, CMD_PRE, r->mshr_id, bank);
        dram_bus_reserve(d->cmd_bus, now + 100, DRAM_CMD_CYCLES, CMD_ACT, r->mshr_id, bank);
        dram_bus_reserve(d->cmd_bus, now + 200, DRAM_CMD_CYCLES, dram_cmd, r->mshr_id, bank);
        dram_bus_reserve(d->data_bus, now + 300, DRAM_DATA_CYCLES, dram_cmd, r->mshr_id, bank);
        d->banks[bank].free_at = now + 300;
        r->done_cycle = now + 300 + DRAM_DATA_CYCLES - 1;
    }

    // Open-Row Policy: row is now open in row buffer
    d->banks[bank].open_row = row;
    if (r->type == CMD_WRITE) 
    {
        // Spec: A write request leaves the request queue when it is scheduled
        r->valid = 0;
    } else {
        r->scheduled = 1;
    }
}


// unified queue insertion helper
static void dram_enqueue(dram_t *d, dram_cmd_t type, uint32_t addr,
                         int mshr_id, int req_from_memstage, uint32_t arrival_cycle)
{
    for (int i = 0; i < DRAM_REQ_QUEUE_SIZE; i++) {
        if (!d->queue[i].valid) {
            dram_req_t *req = &d->queue[i];
            req->valid             = 1;
            req->type              = type;
            req->addr              = addr;
            req->mshr_id           = mshr_id;
            req->req_from_memstage = req_from_memstage;
            req->arrival_cycle     = arrival_cycle;
            req->req_id            = d->next_req_id++; // tie-break for multiple requests in same cycle
            req->scheduled         = 0;
            req->done_cycle        = 0;
            return;
        }
    }
}

// add a read to the request queue
void dram_enqueue_read(dram_t *d, uint32_t addr, int mshr_id,
                       int req_from_memstage, uint32_t now)
{
    dram_enqueue(d, CMD_READ, addr, mshr_id, req_from_memstage, now);
}

// add a write to the request queue (from L2 writeback)
void dram_enqueue_write(dram_t *d, uint32_t addr, uint32_t arrival_cycle)
{
    dram_enqueue(d, CMD_WRITE, addr, -1, 1, arrival_cycle);
}

// add a refresgh to the request queue
void dram_enqueue_refresh(dram_t *d, uint32_t addr, uint32_t now)
{
    dram_enqueue(d, CMD_REFRESH, addr, -1, 0, now);
}


// FR-FCFS policy comparison for two candiate instructions (1 if cand higher priority than current best, 0 otherwise)
static int dram_req_better(dram_t *d, dram_req_t *cand, dram_req_t *best)
{// refreshes must be prioritized over everything else
    if (cand->type == CMD_REFRESH && best->type != CMD_REFRESH) {
        return 1; // cand is refresh, best isn't, so cand wins
    }
    if (cand->type != CMD_REFRESH && best->type == CMD_REFRESH) {
        return 0;
    }
    // if both are refreshes in the queue, FIFO
    if (cand->type == CMD_REFRESH && best->type == CMD_REFRESH) {
        return cand->req_id < best->req_id;
    }

    // check row buffer hits
    int cand_hit = (dram_rb_status(d, cand) == RB_HIT);
    int best_hit = (dram_rb_status(d, best) == RB_HIT);
    if (cand_hit != best_hit) {
        return cand_hit > best_hit;
    }

    // check arrival cycle
    if (cand->arrival_cycle != best->arrival_cycle) {
        return cand->arrival_cycle < best->arrival_cycle;
    }

    // Rule 3: read requests prioritized over write requests
    if ((cand->type == CMD_READ) != (best->type == CMD_READ)) {
        return cand->type == CMD_READ;
    }

    // Rule 4: MEM stage higher priority than FETCH
    if (cand->req_from_memstage != best->req_from_memstage) {
        return cand->req_from_memstage > best->req_from_memstage;
    }

    // last tie break, id request (when were they created within the same cycle)
    return cand->req_id < best->req_id;
}

// Simulate one cycle of DRAM operation
void dram_cycle(dram_t *d, l2_cache_t *l2c, uint32_t now)
{
    // notify L2 of completed transfer and clear
    for (int i = 0; i < DRAM_REQ_QUEUE_SIZE; i++) {
        dram_req_t *req = &d->queue[i];
        if (!req->valid) continue;
        if (req->scheduled && req->done_cycle == now) {// can be a read or a ref, writes are
        // removed from the queue as soon as they are scheduled, while REF/RD once completed
            if(req->type == CMD_READ)
            {// it is a read, notify the l2 by freeing the mshr
                l2_mem_fill_notify(l2c, req->mshr_id);
            }// for both reads and refreshes, clear the slot in the queue once done
            req->valid = 0;
        }
    }

    // periodically inject refresh operations in the queue
#if REFRESH_INTERVAL > 0
    if (now >= REFRESH_INTERVAL && (now % REFRESH_INTERVAL == 0)) {
        uint32_t k = d->refresh_k++;
        int bank = k % DRAM_NUM_BANKS;
        int row  = (k / DRAM_NUM_BANKS) % 65536;
        uint32_t addr = (row << 16) | (bank << 5);
        dram_enqueue_refresh(d, addr, now);
    }
#endif
    // FR-FCFS scheduling scanning the queue
    int best_idx = -1;
    for (int i = 0; i < DRAM_REQ_QUEUE_SIZE; i++) {
        dram_req_t *req = &d->queue[i];
        if (!req->valid || req->scheduled) continue;
        if (now < req->arrival_cycle) continue; // not arrived at controller yet

        if (!dram_is_schedulable(d, req, now)) continue;

        if (best_idx == -1 || dram_req_better(d, req, &d->queue[best_idx])) {
            best_idx = i;
        }
    }

    // If a request is schedulable and won the FR-FCFS arbitration, schedule it
    if (best_idx != -1) {
        dram_schedule(d, &d->queue[best_idx], now);
    }

    // clear bus slots at the now cycle
    d->cmd_bus[now % DRAM_BUS_BUFFER_LEN] = (bus_slot_t){ CMD_FREE, -1, -1 };
    d->data_bus[now % DRAM_BUS_BUFFER_LEN] = (bus_slot_t){ CMD_FREE, -1, -1 };
}