#ifndef CACHE_H
#define CACHE_H

#include "memory_class.h"

// PAGE
extern uint32_t PAGE_TABLE_LATENCY, SWAP_LATENCY;

// CACHE TYPE
#define IS_ITLB 0
#define IS_DTLB 1
#define IS_STLB 2
#define IS_L1I 3
#define IS_L1D 4
#define IS_L2C 5
#define IS_LLC 6

// INSTRUCTION TLB
#define ITLB_SET 16
#define ITLB_WAY 4
#define ITLB_RQ_SIZE 16
#define ITLB_WQ_SIZE 16
#define ITLB_PQ_SIZE 0
#define ITLB_MSHR_SIZE 8
#define ITLB_LATENCY 1

// DATA TLB
#define DTLB_SET 16
#define DTLB_WAY 4
#define DTLB_RQ_SIZE 16
#define DTLB_WQ_SIZE 16
#define DTLB_PQ_SIZE 0
#define DTLB_MSHR_SIZE 8
#define DTLB_LATENCY 1

// SECOND LEVEL TLB
#define STLB_SET 128
#define STLB_WAY 12
#define STLB_RQ_SIZE 32
#define STLB_WQ_SIZE 32
#define STLB_PQ_SIZE 0
#define STLB_MSHR_SIZE 16
#define STLB_LATENCY 8

// L1 INSTRUCTION CACHE
#define L1I_SET 64
#define L1I_WAY 8
#define L1I_RQ_SIZE 64
#define L1I_WQ_SIZE 64
#define L1I_PQ_SIZE 32
#define L1I_MSHR_SIZE 8
#define L1I_LATENCY 4

// L1 DATA CACHE
#define L1D_SET 64
#define L1D_WAY 12
#define L1D_RQ_SIZE 64
#define L1D_WQ_SIZE 64
#define L1D_PQ_SIZE 8
#define L1D_MSHR_SIZE 16
#define L1D_LATENCY 5

// L2 CACHE
#define L2C_SET 1024
#define L2C_WAY 8
#define L2C_RQ_SIZE 32
#define L2C_WQ_SIZE 32
#define L2C_PQ_SIZE 16
#define L2C_MSHR_SIZE 32
#define L2C_LATENCY 10 // 4/5 (L1I or L1D) + 10 = 14/15 cycles

// LAST LEVEL CACHE
#define LLC_SET NUM_CPUS * 2048
#define LLC_WAY 16
#define LLC_RQ_SIZE NUM_CPUS *L2C_MSHR_SIZE // 48
#define LLC_WQ_SIZE NUM_CPUS *L2C_MSHR_SIZE // 48
#define LLC_PQ_SIZE NUM_CPUS * 32
#define LLC_MSHR_SIZE NUM_CPUS * 64
#define LLC_LATENCY 20 // 4/5 (L1I or L1D) + 10 + 20 = 34/35 cycles

class CACHE : public MEMORY
{
public:
    uint32_t cpu;
    const string NAME;
    const uint32_t NUM_SET, NUM_WAY, NUM_LINE, WQ_SIZE, RQ_SIZE, PQ_SIZE, MSHR_SIZE;
    uint32_t LATENCY;
    BLOCK **block;
    BLOCK ***atd;
    vector<int> partitions;
    vector<vector<uint64_t>> hit_counts;
    int fill_level;
    uint32_t MAX_READ, MAX_FILL;
    uint32_t reads_available_this_cycle;
    uint8_t cache_type;

    // prefetch stats
    uint64_t pf_requested,
        pf_issued,
        pf_useful,
        pf_useless,
        pf_fill;

    // queues
    PACKET_QUEUE WQ{NAME + "_WQ", WQ_SIZE},       // write queue
        RQ{NAME + "_RQ", RQ_SIZE},                // read queue
        PQ{NAME + "_PQ", PQ_SIZE},                // prefetch queue
        MSHR{NAME + "_MSHR", MSHR_SIZE},          // MSHR
        PROCESSED{NAME + "_PROCESSED", ROB_SIZE}; // processed queue

    uint64_t sim_access[NUM_CPUS][NUM_TYPES],
        sim_hit[NUM_CPUS][NUM_TYPES],
        sim_miss[NUM_CPUS][NUM_TYPES],
        roi_access[NUM_CPUS][NUM_TYPES],
        roi_hit[NUM_CPUS][NUM_TYPES],
        roi_miss[NUM_CPUS][NUM_TYPES];

    uint64_t total_miss_latency;

    // constructor
    CACHE(string v1, uint32_t v2, int v3, uint32_t v4, uint32_t v5, uint32_t v6, uint32_t v7, uint32_t v8)
        : NAME(v1), NUM_SET(v2), NUM_WAY(v3), NUM_LINE(v4), WQ_SIZE(v5), RQ_SIZE(v6), PQ_SIZE(v7), MSHR_SIZE(v8)
    {

        LATENCY = 0;

        block = new BLOCK *[NUM_SET];
        // cache block
        for (uint32_t i = 0; i < NUM_SET; i++)
        {
            block[i] = new BLOCK[NUM_WAY];
            for (uint32_t j = 0; j < NUM_WAY; j++)
            {
                block[i][j].lru = j;
            }
        }

        if (v1 == "LLC") // Checking if the cache is LLC, to create ATD for each core's LLC only.
        {
            /*
                We allocate CPU equally to the LLC, in this order 0,1,2---,NUM_CPUS-1
                in each CPU, allocate the LRU value from MRU to LRU.
                Example, let say ,we have 4 cores and 64 ways,

                then core of blocks will be 0,0,---,0,1,1----1,2,2,-----2,3,3-----,3
                each core number occurs 8 times

                LRU Values are assigned : 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,0,1,2---
            */
            for (uint32_t i = 0; i < NUM_SET; i++)
            {
                for (uint32_t j = 0; j < NUM_WAY; j++)
                {
                    block[i][j].lru = j % (NUM_WAY / NUM_CPUS);
                    block[i][j].cpu = j / (NUM_WAY / NUM_CPUS);
                }
            }

            /* Equal number of ways are being allocated to each core initially
            then afterwards partitioning algo will decide the distribution*/

            for (uint32_t i = 0; i < NUM_CPUS; i++)
            {
                partitions.push_back(NUM_WAY / NUM_CPUS);
            }

            // Creating an array of ATD
            atd = new BLOCK **[NUM_CPUS];

            // for each CPU, create an ATD of 32 sets for Dynamic Set Sampling ( DSS )
            for (uint32_t i = 0; i < NUM_CPUS; i++)
            {
                atd[i] = new BLOCK *[32];
                for (uint32_t j = 0; j < 32; j++)
                {
                    // for each set in ATD , Blocks are being assigned in the set with LRU value starting from MRU ( 0 ) to LRU ( NUM_WAY-1 ).
                    atd[i][j] = new BLOCK[NUM_WAY];
                    for (uint32_t k = 0; k < NUM_WAY; k++)
                    {
                        // Assiging LRU Value
                        atd[i][j][k].lru = k;
                    }
                }
            }

            // Hit counters is created for each way in an ATD, initalized with 0 hits
            hit_counts.resize(NUM_CPUS);
            for (uint32_t i = 0; i < NUM_CPUS; i++)
            {
                for (uint32_t j = 0; j < NUM_WAY; j++)
                {
                    hit_counts[i].push_back(0);
                }
            }
        }

        for (uint32_t i = 0; i < NUM_CPUS; i++)
        {
            upper_level_icache[i] = NULL;
            upper_level_dcache[i] = NULL;

            for (uint32_t j = 0; j < NUM_TYPES; j++)
            {
                sim_access[i][j] = 0;
                sim_hit[i][j] = 0;
                sim_miss[i][j] = 0;
                roi_access[i][j] = 0;
                roi_hit[i][j] = 0;
                roi_miss[i][j] = 0;
            }
        }

        total_miss_latency = 0;

        lower_level = NULL;
        extra_interface = NULL;
        fill_level = -1;
        MAX_READ = 1;
        MAX_FILL = 1;

        pf_requested = 0;
        pf_issued = 0;
        pf_useful = 0;
        pf_useless = 0;
        pf_fill = 0;
    };

    // destructor
    ~CACHE()
    {
        for (uint32_t i = 0; i < NUM_SET; i++)
            delete[] block[i];
        delete[] block;
    };

    // functions
    int add_rq(PACKET *packet),
        add_wq(PACKET *packet),
        add_pq(PACKET *packet);

    void return_data(PACKET *packet),
        operate(),
        increment_WQ_FULL(uint64_t address);

    uint32_t get_occupancy(uint8_t queue_type, uint64_t address),
        get_size(uint8_t queue_type, uint64_t address);

    int check_hit(PACKET *packet),
        check_hit_atd(PACKET *packet),
        invalidate_entry(uint64_t inval_addr),
        check_mshr(PACKET *packet),
        prefetch_line(uint64_t ip, uint64_t base_addr, uint64_t pf_addr, int prefetch_fill_level, uint32_t prefetch_metadata),
        kpc_prefetch_line(uint64_t base_addr, uint64_t pf_addr, int prefetch_fill_level, int delta, int depth, int signature, int confidence, uint32_t prefetch_metadata);

    void handle_fill(),
        handle_writeback(),
        handle_read(),
        handle_prefetch();

    void add_mshr(PACKET *packet),
        update_fill_cycle(),
        llc_initialize_replacement(),
        update_replacement_state(uint32_t cpu, uint32_t set, uint32_t way, uint64_t full_addr, uint64_t ip, uint64_t victim_addr, uint32_t type, uint8_t hit),
        llc_update_replacement_state(uint32_t cpu, uint32_t set, uint32_t way, uint64_t full_addr, uint64_t ip, uint64_t victim_addr, uint32_t type, uint8_t hit),
        lru_update(uint32_t set, uint32_t way),
        llc_lru_update(uint32_t set, uint32_t way, uint32_t cpu),
        atd_lru_update(uint32_t set, uint32_t way, uint32_t cpu),
        fill_cache(uint32_t set, uint32_t way, PACKET *packet),
        fill_atd(uint32_t set, uint32_t way, PACKET *packet),
        replacement_final_stats(),
        llc_replacement_final_stats(),
        // prefetcher_initialize(),
        l1d_prefetcher_initialize(),
        l2c_prefetcher_initialize(),
        llc_prefetcher_initialize(),
        prefetcher_operate(uint64_t addr, uint64_t ip, uint8_t cache_hit, uint8_t type),
        l1d_prefetcher_operate(uint64_t addr, uint64_t ip, uint8_t cache_hit, uint8_t type),
        prefetcher_cache_fill(uint64_t addr, uint32_t set, uint32_t way, uint8_t prefetch, uint64_t evicted_addr),
        l1d_prefetcher_cache_fill(uint64_t addr, uint32_t set, uint32_t way, uint8_t prefetch, uint64_t evicted_addr, uint32_t metadata_in),
        // prefetcher_final_stats(),
        l1d_prefetcher_final_stats(),
        l2c_prefetcher_final_stats(),
        llc_prefetcher_final_stats();
    void (*l1i_prefetcher_cache_operate)(uint32_t, uint64_t, uint8_t, uint8_t);
    void (*l1i_prefetcher_cache_fill)(uint32_t, uint64_t, uint32_t, uint32_t, uint8_t, uint64_t);

    uint32_t l2c_prefetcher_operate(uint64_t addr, uint64_t ip, uint8_t cache_hit, uint8_t type, uint32_t metadata_in),
        llc_prefetcher_operate(uint64_t addr, uint64_t ip, uint8_t cache_hit, uint8_t type, uint32_t metadata_in),
        l2c_prefetcher_cache_fill(uint64_t addr, uint32_t set, uint32_t way, uint8_t prefetch, uint64_t evicted_addr, uint32_t metadata_in),
        llc_prefetcher_cache_fill(uint64_t addr, uint32_t set, uint32_t way, uint8_t prefetch, uint64_t evicted_addr, uint32_t metadata_in);

    uint32_t get_set(uint64_t address),
        atd_lru_victim(uint32_t cpu, uint32_t set),
        get_way(uint64_t address, uint32_t set),
        find_victim(uint32_t cpu, uint64_t instr_id, uint32_t set, const BLOCK *current_set, uint64_t ip, uint64_t full_addr, uint32_t type),
        llc_find_victim(uint32_t cpu, uint64_t instr_id, uint32_t set, const BLOCK *current_set, uint64_t ip, uint64_t full_addr, uint32_t type),
        lru_victim(uint32_t cpu, uint64_t instr_id, uint32_t set, const BLOCK *current_set, uint64_t ip, uint64_t full_addr, uint32_t type),
        llc_lru_victim(uint32_t cpu, uint64_t instr_id, uint32_t set, const BLOCK *current_set, uint64_t ip, uint64_t full_addr, uint32_t type);

    vector<uint32_t> partition_algorithm();
    pair<float, uint32_t> get_max_mu(uint32_t core, uint32_t alloc, uint32_t balance);
    float get_mu_value(uint32_t core, uint32_t a, uint32_t b);
};

#endif
