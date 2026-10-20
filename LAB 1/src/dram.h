// TASK 2: adding the DRAM
// Key idea: use a req. queue (static array) to store mem req coming from L2, 
// use a circular buffer to track if the bus is occupied (if so, how?) in future cycles.  

#ifndef _DRAM_H_
#define _DRAM_H_

#include <stdint.h>
#include "cache.h"

/* DRAM Constants */
#define DRAM_NUM_BANKS      8     
#define DRAM_CMD_CYCLES     4
#define DRAM_BANK_CYCLES    100    
#define DRAM_DATA_CYCLES    50     
#define DRAM_MAX_CMDS       3
// number of planned commands to access a row in case of row buff conflict (PRE (disconnect prev), ACT+R/W) 
#define DRAM_BUS_BUFFER_LEN        512
// to track if bus is occupied over the next N cycles
#define DRAM_REQ_QUEUE_SIZE 64     
// Size of the queue that tracks which commands should be issued

/* Address Decoding ([7:5] bank, [31:16] row) */
#define DRAM_BANK(addr)     (((addr) >> 5) & 0x7)
#define DRAM_ROW(addr)      (((addr) >> 16) & 0xFFFF)


/* Request Types & Bus Commands */
typedef enum {
    CMD_FREE = 0, // starts free initially
    CMD_ACT,   
    CMD_PRE,   
    CMD_READ,  
    CMD_WRITE, 
    CMD_REFRESH
} dram_cmd_t;

/* Info for each request on the bus */
typedef struct {
    dram_cmd_t cmd;    
    int mshr_id;       // who issued the req from L2?
    int bank;          // which bank is the req going to?
} bus_slot_t;

/* FCFS priority: Status & required commands  */
typedef enum {
    RB_HIT      = 1,   
    RB_MISS     = 2,   
    RB_CONFLICT = 3    
} rb_status_t;

/* DRAM bank State */
typedef struct {
    int      open_row; // -1 if closed, >= 0 index
    uint32_t free_at; 
} dram_bank_t;


/* Memory request complete info  */
typedef struct {
    dram_cmd_t  type;              
    uint32_t    addr;              
    int         mshr_id;           
    int         req_from_memstage; 
    uint32_t    arrival_cycle;
    uint32_t done_cycle;     
    uint64_t    req_id;
    int scheduled;        
} dram_req_t;

/* Complete DRAM system & Memory Controller structure */
typedef struct {
    dram_bank_t banks[DRAM_NUM_BANKS];
    bus_slot_t  cmd_bus[DRAM_BUS_BUFFER_LEN];
    bus_slot_t  data_bus[DRAM_BUS_BUFFER_LEN];
    dram_req_t  queue[DRAM_REQ_QUEUE_SIZE];
    int         num_reqs;           // number of req currently in queue
    uint64_t    next_req_id;        // serialization of multiple access in same cycle
} dram_t;

/* Create/destroy a DRAM */
dram_t *dram_new(void);
void    dram_destroy(dram_t *d);

/* Enqueue a read request from L2 (called 5 cycles after L2 miss) */
void dram_enqueue_read(dram_t *d, uint32_t addr, int mshr_id,
                       int req_from_memstage, uint32_t now);

/* Advance DRAM state by 1 cycle: check completions, schedule pending, clear past slot */
void dram_cycle(dram_t *d, l2_cache_t *l2c, uint32_t now);

/* Check Row Buffer status, handle schedulability */
rb_status_t dram_rb_status(dram_t *d, dram_req_t *r);
int dram_is_schedulable(dram_t *d, dram_req_t *r, uint32_t now);
void dram_schedule(dram_t *d, dram_req_t *r, uint32_t now);

/* Bus check and reservation */
int  dram_bus_isfree(bus_slot_t *bus, uint32_t start, int len);
void dram_bus_reserve(bus_slot_t *bus, uint32_t start, int len,
                      dram_cmd_t cmd, int mshr_id, int bank);

#ifdef DEBUG
void dram_dump_bus(dram_t *d, uint32_t now, int len);
#endif

#endif /* _DRAM_H_ */
