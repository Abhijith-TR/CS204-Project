#include "cache.h"
#include "set.h"

uint64_t l2pf_access = 0;

uint64_t partition_count = 0; // This variable is used for call paritition only on 5 million cycles. We might make inside

void CACHE::handle_fill()
{
  // handle fill
  uint32_t fill_cpu = (MSHR.next_fill_index == MSHR_SIZE) ? NUM_CPUS : MSHR.entry[MSHR.next_fill_index].cpu;
  if (fill_cpu == NUM_CPUS)
    return;

  if (MSHR.next_fill_cycle <= current_core_cycle[fill_cpu])
  {

#ifdef SANITY_CHECK
    if (MSHR.next_fill_index >= MSHR.SIZE)
      assert(0);
#endif

    uint32_t mshr_index = MSHR.next_fill_index;

    // find victim
    uint32_t set = get_set(MSHR.entry[mshr_index].address), way;
    if (cache_type == IS_LLC)
    {
      way = llc_find_victim(fill_cpu, MSHR.entry[mshr_index].instr_id, set, block[set], MSHR.entry[mshr_index].ip, MSHR.entry[mshr_index].full_addr, MSHR.entry[mshr_index].type);
    }
    else
      way = find_victim(fill_cpu, MSHR.entry[mshr_index].instr_id, set, block[set], MSHR.entry[mshr_index].ip, MSHR.entry[mshr_index].full_addr, MSHR.entry[mshr_index].type);

#ifdef LLC_BYPASS
    if ((cache_type == IS_LLC) && (way == LLC_WAY))
    { // this is a bypass that does not fill the LLC

      // update replacement policy
      if (cache_type == IS_LLC)
      {
        llc_update_replacement_state(fill_cpu, set, way, MSHR.entry[mshr_index].full_addr, MSHR.entry[mshr_index].ip, 0, MSHR.entry[mshr_index].type, 0);
      }
      else
        update_replacement_state(fill_cpu, set, way, MSHR.entry[mshr_index].full_addr, MSHR.entry[mshr_index].ip, 0, MSHR.entry[mshr_index].type, 0);

      // COLLECT STATS
      sim_miss[fill_cpu][MSHR.entry[mshr_index].type]++;
      sim_access[fill_cpu][MSHR.entry[mshr_index].type]++;

      // check fill level
      if (MSHR.entry[mshr_index].fill_level < fill_level)
      {

        if (fill_level == FILL_L2)
        {
          if (MSHR.entry[mshr_index].fill_l1i)
          {
            upper_level_icache[fill_cpu]->return_data(&MSHR.entry[mshr_index]);
          }
          if (MSHR.entry[mshr_index].fill_l1d)
          {
            upper_level_dcache[fill_cpu]->return_data(&MSHR.entry[mshr_index]);
          }
        }
        else
        {
          if (MSHR.entry[mshr_index].instruction)
            upper_level_icache[fill_cpu]->return_data(&MSHR.entry[mshr_index]);
          if (MSHR.entry[mshr_index].is_data)
            upper_level_dcache[fill_cpu]->return_data(&MSHR.entry[mshr_index]);
        }
      }

      if (warmup_complete[fill_cpu] && (MSHR.entry[mshr_index].cycle_enqueued != 0))
      {
        uint64_t current_miss_latency = (current_core_cycle[fill_cpu] - MSHR.entry[mshr_index].cycle_enqueued);
        total_miss_latency += current_miss_latency;
      }

      MSHR.remove_queue(&MSHR.entry[mshr_index]);
      MSHR.num_returned--;

      update_fill_cycle();

      return; // return here, no need to process further in this function
    }
#endif

    uint8_t do_fill = 1;

    // is this dirty?
    if (block[set][way].dirty)
    {

      // check if the lower level WQ has enough room to keep this writeback request
      if (lower_level)
      {
        if (lower_level->get_occupancy(2, block[set][way].address) == lower_level->get_size(2, block[set][way].address))
        {

          // lower level WQ is full, cannot replace this victim
          do_fill = 0;
          lower_level->increment_WQ_FULL(block[set][way].address);
          STALL[MSHR.entry[mshr_index].type]++;

          DP(if (warmup_complete[fill_cpu]) {
                    cout << "[" << NAME << "] " << __func__ << "do_fill: " << +do_fill;
                    cout << " lower level wq is full!" << " fill_addr: " << hex << MSHR.entry[mshr_index].address;
                    cout << " victim_addr: " << block[set][way].tag << dec << endl; });
        }
        else
        {
          PACKET writeback_packet;

          writeback_packet.fill_level = fill_level << 1;
          writeback_packet.cpu = fill_cpu;
          writeback_packet.address = block[set][way].address;
          writeback_packet.full_addr = block[set][way].full_addr;
          writeback_packet.data = block[set][way].data;
          writeback_packet.instr_id = MSHR.entry[mshr_index].instr_id;
          writeback_packet.ip = 0; // writeback does not have ip
          writeback_packet.type = WRITEBACK;
          writeback_packet.event_cycle = current_core_cycle[fill_cpu];

          lower_level->add_wq(&writeback_packet);
        }
      }
#ifdef SANITY_CHECK
      else
      {
        // sanity check
        if (cache_type != IS_STLB)
          assert(0);
      }
#endif
    }

    if (do_fill)
    {
      // update prefetcher
      if (cache_type == IS_L1I)
        l1i_prefetcher_cache_fill(fill_cpu, ((MSHR.entry[mshr_index].ip) >> LOG2_BLOCK_SIZE) << LOG2_BLOCK_SIZE, set, way, (MSHR.entry[mshr_index].type == PREFETCH) ? 1 : 0, ((block[set][way].ip) >> LOG2_BLOCK_SIZE) << LOG2_BLOCK_SIZE);
      if (cache_type == IS_L1D)
        l1d_prefetcher_cache_fill(MSHR.entry[mshr_index].full_addr, set, way, (MSHR.entry[mshr_index].type == PREFETCH) ? 1 : 0, block[set][way].address << LOG2_BLOCK_SIZE,
                                  MSHR.entry[mshr_index].pf_metadata);
      if (cache_type == IS_L2C)
        MSHR.entry[mshr_index].pf_metadata = l2c_prefetcher_cache_fill(MSHR.entry[mshr_index].address << LOG2_BLOCK_SIZE, set, way, (MSHR.entry[mshr_index].type == PREFETCH) ? 1 : 0,
                                                                       block[set][way].address << LOG2_BLOCK_SIZE, MSHR.entry[mshr_index].pf_metadata);
      if (cache_type == IS_LLC)
      {
        cpu = fill_cpu;
        MSHR.entry[mshr_index].pf_metadata = llc_prefetcher_cache_fill(MSHR.entry[mshr_index].address << LOG2_BLOCK_SIZE, set, way, (MSHR.entry[mshr_index].type == PREFETCH) ? 1 : 0,
                                                                       block[set][way].address << LOG2_BLOCK_SIZE, MSHR.entry[mshr_index].pf_metadata);
        cpu = 0;
      }

      // update replacement policy
      if (cache_type == IS_LLC)
      {
        llc_update_replacement_state(fill_cpu, set, way, MSHR.entry[mshr_index].full_addr, MSHR.entry[mshr_index].ip, block[set][way].full_addr, MSHR.entry[mshr_index].type, 0);
      }
      else
        update_replacement_state(fill_cpu, set, way, MSHR.entry[mshr_index].full_addr, MSHR.entry[mshr_index].ip, block[set][way].full_addr, MSHR.entry[mshr_index].type, 0);

      // COLLECT STATS
      sim_miss[fill_cpu][MSHR.entry[mshr_index].type]++;
      sim_access[fill_cpu][MSHR.entry[mshr_index].type]++;

      fill_cache(set, way, &MSHR.entry[mshr_index]);

      // RFO marks cache line dirty
      if (cache_type == IS_L1D)
      {
        if (MSHR.entry[mshr_index].type == RFO)
          block[set][way].dirty = 1;
      }

      // check fill level
      if (MSHR.entry[mshr_index].fill_level < fill_level)
      {

        if (fill_level == FILL_L2)
        {
          if (MSHR.entry[mshr_index].fill_l1i)
          {
            upper_level_icache[fill_cpu]->return_data(&MSHR.entry[mshr_index]);
          }
          if (MSHR.entry[mshr_index].fill_l1d)
          {
            upper_level_dcache[fill_cpu]->return_data(&MSHR.entry[mshr_index]);
          }
        }
        else
        {
          if (MSHR.entry[mshr_index].instruction)
            upper_level_icache[fill_cpu]->return_data(&MSHR.entry[mshr_index]);
          if (MSHR.entry[mshr_index].is_data)
            upper_level_dcache[fill_cpu]->return_data(&MSHR.entry[mshr_index]);
        }
      }

      // update processed packets
      if (cache_type == IS_ITLB)
      {
        MSHR.entry[mshr_index].instruction_pa = block[set][way].data;
        if (PROCESSED.occupancy < PROCESSED.SIZE)
          PROCESSED.add_queue(&MSHR.entry[mshr_index]);
      }
      else if (cache_type == IS_DTLB)
      {
        MSHR.entry[mshr_index].data_pa = block[set][way].data;
        if (PROCESSED.occupancy < PROCESSED.SIZE)
          PROCESSED.add_queue(&MSHR.entry[mshr_index]);
      }
      else if (cache_type == IS_L1I)
      {
        if (PROCESSED.occupancy < PROCESSED.SIZE)
          PROCESSED.add_queue(&MSHR.entry[mshr_index]);
      }
      // else if (cache_type == IS_L1D) {
      else if ((cache_type == IS_L1D) && (MSHR.entry[mshr_index].type != PREFETCH))
      {
        if (PROCESSED.occupancy < PROCESSED.SIZE)
          PROCESSED.add_queue(&MSHR.entry[mshr_index]);
      }

      if (warmup_complete[fill_cpu] && (MSHR.entry[mshr_index].cycle_enqueued != 0))
      {
        uint64_t current_miss_latency = (current_core_cycle[fill_cpu] - MSHR.entry[mshr_index].cycle_enqueued);
        /*
        if(cache_type == IS_L1D)
          {
            cout << current_core_cycle[fill_cpu] << " - " << MSHR.entry[mshr_index].cycle_enqueued << " = " << current_miss_latency << " MSHR index: " << mshr_index << endl;
          }
        */
        total_miss_latency += current_miss_latency;
      }

      MSHR.remove_queue(&MSHR.entry[mshr_index]);
      MSHR.num_returned--;

      update_fill_cycle();
    }
  }
}

void CACHE::handle_writeback()
{
  // handle write
  uint32_t writeback_cpu = WQ.entry[WQ.head].cpu;
  if (writeback_cpu == NUM_CPUS)
    return;

  // handle the oldest entry
  if ((WQ.entry[WQ.head].event_cycle <= current_core_cycle[writeback_cpu]) && (WQ.occupancy > 0))
  {
    int index = WQ.head;

    // access cache
    uint32_t set = get_set(WQ.entry[index].address);
    int way = check_hit(&WQ.entry[index]);

    // Checking for hits in the ATD whenever we check for hit in the LLC
    if (cache_type == IS_LLC && set % (NUM_SET / 32) == 0)
    {
      int atd_way = check_hit_atd(&WQ.entry[index]); // Checking for hit in the ATD
      if (atd_way == -1)
      {
        int way_r = atd_lru_victim(WQ.entry[index].cpu, set / (NUM_SET / 32)); // Finding way to be replaced in ATD
        fill_atd(set / (NUM_SET / 32), way_r, &WQ.entry[index]);               // Placing the the requested packet in cpu
        atd_lru_update(set / (NUM_SET / 32), way_r, WQ.entry[index].cpu);      // Updating the lru values in ATD
      }
    }

    if (way >= 0)
    { // writeback hit (or RFO hit for L1D)

      if (cache_type == IS_LLC)
      {
        llc_update_replacement_state(writeback_cpu, set, way, block[set][way].full_addr, WQ.entry[index].ip, 0, WQ.entry[index].type, 1);
      }
      else
        update_replacement_state(writeback_cpu, set, way, block[set][way].full_addr, WQ.entry[index].ip, 0, WQ.entry[index].type, 1);

      // COLLECT STATS
      sim_hit[writeback_cpu][WQ.entry[index].type]++;
      sim_access[writeback_cpu][WQ.entry[index].type]++;

      // mark dirty
      block[set][way].dirty = 1;

      if (cache_type == IS_ITLB)
        WQ.entry[index].instruction_pa = block[set][way].data;
      else if (cache_type == IS_DTLB)
        WQ.entry[index].data_pa = block[set][way].data;
      else if (cache_type == IS_STLB)
        WQ.entry[index].data = block[set][way].data;

      // check fill level
      if (WQ.entry[index].fill_level < fill_level)
      {

        if (fill_level == FILL_L2)
        {
          if (WQ.entry[index].fill_l1i)
          {
            upper_level_icache[writeback_cpu]->return_data(&WQ.entry[index]);
          }
          if (WQ.entry[index].fill_l1d)
          {
            upper_level_dcache[writeback_cpu]->return_data(&WQ.entry[index]);
          }
        }
        else
        {
          if (WQ.entry[index].instruction)
            upper_level_icache[writeback_cpu]->return_data(&WQ.entry[index]);
          if (WQ.entry[index].is_data)
            upper_level_dcache[writeback_cpu]->return_data(&WQ.entry[index]);
        }
      }

      HIT[WQ.entry[index].type]++;
      ACCESS[WQ.entry[index].type]++;

      // remove this entry from WQ
      WQ.remove_queue(&WQ.entry[index]);
    }
    else
    { // writeback miss (or RFO miss for L1D)

      DP(if (warmup_complete[writeback_cpu]) {
            cout << "[" << NAME << "] " << __func__ << " type: " << +WQ.entry[index].type << " miss";
            cout << " instr_id: " << WQ.entry[index].instr_id << " address: " << hex << WQ.entry[index].address;
            cout << " full_addr: " << WQ.entry[index].full_addr << dec;
            cout << " cycle: " << WQ.entry[index].event_cycle << endl; });

      if (cache_type == IS_L1D)
      { // RFO miss

        // check mshr
        uint8_t miss_handled = 1;
        int mshr_index = check_mshr(&WQ.entry[index]);

        if (mshr_index == -2)
        {
          // this is a data/instruction collision in the MSHR, so we have to wait before we can allocate this miss
          miss_handled = 0;
        }
        else if ((mshr_index == -1) && (MSHR.occupancy < MSHR_SIZE))
        { // this is a new miss

          if (cache_type == IS_LLC)
          {
            // check to make sure the DRAM RQ has room for this LLC RFO miss
            if (lower_level->get_occupancy(1, WQ.entry[index].address) == lower_level->get_size(1, WQ.entry[index].address))
            {
              miss_handled = 0;
            }
            else
            {
              add_mshr(&WQ.entry[index]);
              lower_level->add_rq(&WQ.entry[index]);
            }
          }
          else
          {
            // add it to mshr (RFO miss)
            add_mshr(&WQ.entry[index]);

            // add it to the next level's read queue
            // if (lower_level) // L1D always has a lower level cache
            lower_level->add_rq(&WQ.entry[index]);
          }
        }
        else
        {
          if ((mshr_index == -1) && (MSHR.occupancy == MSHR_SIZE))
          { // not enough MSHR resource

            // cannot handle miss request until one of MSHRs is available
            miss_handled = 0;
            STALL[WQ.entry[index].type]++;
          }
          else if (mshr_index != -1)
          { // already in-flight miss

            // update fill_level
            if (WQ.entry[index].fill_level < MSHR.entry[mshr_index].fill_level)
              MSHR.entry[mshr_index].fill_level = WQ.entry[index].fill_level;

            if ((WQ.entry[index].fill_l1i) && (MSHR.entry[mshr_index].fill_l1i != 1))
            {
              MSHR.entry[mshr_index].fill_l1i = 1;
            }
            if ((WQ.entry[index].fill_l1d) && (MSHR.entry[mshr_index].fill_l1d != 1))
            {
              MSHR.entry[mshr_index].fill_l1d = 1;
            }

            // update request
            if (MSHR.entry[mshr_index].type == PREFETCH)
            {
              uint8_t prior_returned = MSHR.entry[mshr_index].returned;
              uint64_t prior_event_cycle = MSHR.entry[mshr_index].event_cycle;
              MSHR.entry[mshr_index] = WQ.entry[index];

              // in case request is already returned, we should keep event_cycle and retunred variables
              MSHR.entry[mshr_index].returned = prior_returned;
              MSHR.entry[mshr_index].event_cycle = prior_event_cycle;
            }

            MSHR_MERGED[WQ.entry[index].type]++;

            DP(if (warmup_complete[writeback_cpu]) {
                        cout << "[" << NAME << "] " << __func__ << " mshr merged";
                        cout << " instr_id: " << WQ.entry[index].instr_id << " prior_id: " << MSHR.entry[mshr_index].instr_id; 
                        cout << " address: " << hex << WQ.entry[index].address;
                        cout << " full_addr: " << WQ.entry[index].full_addr << dec;
                        cout << " cycle: " << WQ.entry[index].event_cycle << endl; });
          }
          else
          { // WE SHOULD NOT REACH HERE
            cerr << "[" << NAME << "] MSHR errors" << endl;
            assert(0);
          }
        }

        if (miss_handled)
        {

          MISS[WQ.entry[index].type]++;
          ACCESS[WQ.entry[index].type]++;

          // remove this entry from WQ
          WQ.remove_queue(&WQ.entry[index]);
        }
      }
      else
      {
        // find victim
        uint32_t set = get_set(WQ.entry[index].address), way;
        if (cache_type == IS_LLC)
        {
          way = llc_find_victim(writeback_cpu, WQ.entry[index].instr_id, set, block[set], WQ.entry[index].ip, WQ.entry[index].full_addr, WQ.entry[index].type);
        }
        else
          way = find_victim(writeback_cpu, WQ.entry[index].instr_id, set, block[set], WQ.entry[index].ip, WQ.entry[index].full_addr, WQ.entry[index].type);

#ifdef LLC_BYPASS
        if ((cache_type == IS_LLC) && (way == LLC_WAY))
        {
          cerr << "LLC bypassing for writebacks is not allowed!" << endl;
          assert(0);
        }
#endif

        uint8_t do_fill = 1;

        // is this dirty?
        if (block[set][way].dirty)
        {

          // check if the lower level WQ has enough room to keep this writeback request
          if (lower_level)
          {
            if (lower_level->get_occupancy(2, block[set][way].address) == lower_level->get_size(2, block[set][way].address))
            {

              // lower level WQ is full, cannot replace this victim
              do_fill = 0;
              lower_level->increment_WQ_FULL(block[set][way].address);
              STALL[WQ.entry[index].type]++;

              DP(if (warmup_complete[writeback_cpu]) {
                            cout << "[" << NAME << "] " << __func__ << "do_fill: " << +do_fill;
                            cout << " lower level wq is full!" << " fill_addr: " << hex << WQ.entry[index].address;
                            cout << " victim_addr: " << block[set][way].tag << dec << endl; });
            }
            else
            {
              PACKET writeback_packet;

              writeback_packet.fill_level = fill_level << 1;
              writeback_packet.cpu = writeback_cpu;
              writeback_packet.address = block[set][way].address;
              writeback_packet.full_addr = block[set][way].full_addr;
              writeback_packet.data = block[set][way].data;
              writeback_packet.instr_id = WQ.entry[index].instr_id;
              writeback_packet.ip = 0;
              writeback_packet.type = WRITEBACK;
              writeback_packet.event_cycle = current_core_cycle[writeback_cpu];

              lower_level->add_wq(&writeback_packet);
            }
          }
#ifdef SANITY_CHECK
          else
          {
            // sanity check
            if (cache_type != IS_STLB)
              assert(0);
          }
#endif
        }

        if (do_fill)
        {
          // update prefetcher
          if (cache_type == IS_L1I)
            l1i_prefetcher_cache_fill(writeback_cpu, ((WQ.entry[index].ip) >> LOG2_BLOCK_SIZE) << LOG2_BLOCK_SIZE, set, way, 0, ((block[set][way].ip) >> LOG2_BLOCK_SIZE) << LOG2_BLOCK_SIZE);
          if (cache_type == IS_L1D)
            l1d_prefetcher_cache_fill(WQ.entry[index].full_addr, set, way, 0, block[set][way].address << LOG2_BLOCK_SIZE, WQ.entry[index].pf_metadata);
          else if (cache_type == IS_L2C)
            WQ.entry[index].pf_metadata = l2c_prefetcher_cache_fill(WQ.entry[index].address << LOG2_BLOCK_SIZE, set, way, 0,
                                                                    block[set][way].address << LOG2_BLOCK_SIZE, WQ.entry[index].pf_metadata);
          if (cache_type == IS_LLC)
          {
            cpu = writeback_cpu;
            WQ.entry[index].pf_metadata = llc_prefetcher_cache_fill(WQ.entry[index].address << LOG2_BLOCK_SIZE, set, way, 0,
                                                                    block[set][way].address << LOG2_BLOCK_SIZE, WQ.entry[index].pf_metadata);
            cpu = 0;
          }

          // update replacement policy
          if (cache_type == IS_LLC)
          {
            llc_update_replacement_state(writeback_cpu, set, way, WQ.entry[index].full_addr, WQ.entry[index].ip, block[set][way].full_addr, WQ.entry[index].type, 0);
          }
          else
            update_replacement_state(writeback_cpu, set, way, WQ.entry[index].full_addr, WQ.entry[index].ip, block[set][way].full_addr, WQ.entry[index].type, 0);

          // COLLECT STATS
          sim_miss[writeback_cpu][WQ.entry[index].type]++;
          sim_access[writeback_cpu][WQ.entry[index].type]++;

          fill_cache(set, way, &WQ.entry[index]);

          // mark dirty
          block[set][way].dirty = 1;

          // check fill level
          if (WQ.entry[index].fill_level < fill_level)
          {

            if (fill_level == FILL_L2)
            {
              if (WQ.entry[index].fill_l1i)
              {
                upper_level_icache[writeback_cpu]->return_data(&WQ.entry[index]);
              }
              if (WQ.entry[index].fill_l1d)
              {
                upper_level_dcache[writeback_cpu]->return_data(&WQ.entry[index]);
              }
            }
            else
            {
              if (WQ.entry[index].instruction)
                upper_level_icache[writeback_cpu]->return_data(&WQ.entry[index]);
              if (WQ.entry[index].is_data)
                upper_level_dcache[writeback_cpu]->return_data(&WQ.entry[index]);
            }
          }

          MISS[WQ.entry[index].type]++;
          ACCESS[WQ.entry[index].type]++;

          // remove this entry from WQ
          WQ.remove_queue(&WQ.entry[index]);
        }
      }
    }
  }
}

void CACHE::handle_read()
{
  // handle read
  for (uint32_t i = 0; i < MAX_READ; i++)
  {

    uint32_t read_cpu = RQ.entry[RQ.head].cpu;
    if (read_cpu == NUM_CPUS)
      return;

    // handle the oldest entry
    if ((RQ.entry[RQ.head].event_cycle <= current_core_cycle[read_cpu]) && (RQ.occupancy > 0))
    {
      int index = RQ.head;

      // access cache
      uint32_t set = get_set(RQ.entry[index].address);
      int way = check_hit(&RQ.entry[index]);

      // Checking for hits in the ATD whenever we check for hit in the LLC
      if (cache_type == IS_LLC && set % (NUM_SET / 32) == 0)
      {
        int atd_way = check_hit_atd(&RQ.entry[index]);
        if (atd_way == -1)
        {
          int way_r = atd_lru_victim(RQ.entry[index].cpu, set / (NUM_SET / 32)); // Finding way to be replaced in ATD
          fill_atd(set / (NUM_SET / 32), way_r, &RQ.entry[index]);               // Placing the the requested packet in cpu
          atd_lru_update(set / (NUM_SET / 32), way_r, RQ.entry[index].cpu);      // Updating the lru values in ATD
        }
        else
        {
          atd_lru_update(set / (NUM_SET / 32), atd_way, RQ.entry[index].cpu); // Updating the lru values in ATD
        }
      }

      if (way >= 0)
      { // read hit

        if (cache_type == IS_ITLB)
        {
          RQ.entry[index].instruction_pa = block[set][way].data;
          if (PROCESSED.occupancy < PROCESSED.SIZE)
            PROCESSED.add_queue(&RQ.entry[index]);
        }
        else if (cache_type == IS_DTLB)
        {
          RQ.entry[index].data_pa = block[set][way].data;
          if (PROCESSED.occupancy < PROCESSED.SIZE)
            PROCESSED.add_queue(&RQ.entry[index]);
        }
        else if (cache_type == IS_STLB)
          RQ.entry[index].data = block[set][way].data;
        else if (cache_type == IS_L1I)
        {
          if (PROCESSED.occupancy < PROCESSED.SIZE)
            PROCESSED.add_queue(&RQ.entry[index]);
        }
        // else if (cache_type == IS_L1D) {
        else if ((cache_type == IS_L1D) && (RQ.entry[index].type != PREFETCH))
        {
          if (PROCESSED.occupancy < PROCESSED.SIZE)
            PROCESSED.add_queue(&RQ.entry[index]);
        }

        // update prefetcher on load instruction
        if (RQ.entry[index].type == LOAD)
        {
          if (cache_type == IS_L1I)
            l1i_prefetcher_cache_operate(read_cpu, RQ.entry[index].ip, 1, block[set][way].prefetch);
          if (cache_type == IS_L1D)
            l1d_prefetcher_operate(RQ.entry[index].full_addr, RQ.entry[index].ip, 1, RQ.entry[index].type);
          else if (cache_type == IS_L2C)
            l2c_prefetcher_operate(block[set][way].address << LOG2_BLOCK_SIZE, RQ.entry[index].ip, 1, RQ.entry[index].type, 0);
          else if (cache_type == IS_LLC)
          {
            cpu = read_cpu;
            llc_prefetcher_operate(block[set][way].address << LOG2_BLOCK_SIZE, RQ.entry[index].ip, 1, RQ.entry[index].type, 0);
            cpu = 0;
          }
        }

        // update replacement policy
        if (cache_type == IS_LLC)
        {
          llc_update_replacement_state(read_cpu, set, way, block[set][way].full_addr, RQ.entry[index].ip, 0, RQ.entry[index].type, 1);
        }
        else
          update_replacement_state(read_cpu, set, way, block[set][way].full_addr, RQ.entry[index].ip, 0, RQ.entry[index].type, 1);

        // COLLECT STATS
        sim_hit[read_cpu][RQ.entry[index].type]++;
        sim_access[read_cpu][RQ.entry[index].type]++;

        // check fill level
        if (RQ.entry[index].fill_level < fill_level)
        {

          if (fill_level == FILL_L2)
          {
            if (RQ.entry[index].fill_l1i)
            {
              upper_level_icache[read_cpu]->return_data(&RQ.entry[index]);
            }
            if (RQ.entry[index].fill_l1d)
            {
              upper_level_dcache[read_cpu]->return_data(&RQ.entry[index]);
            }
          }
          else
          {
            if (RQ.entry[index].instruction)
              upper_level_icache[read_cpu]->return_data(&RQ.entry[index]);
            if (RQ.entry[index].is_data)
              upper_level_dcache[read_cpu]->return_data(&RQ.entry[index]);
          }
        }

        // update prefetch stats and reset prefetch bit
        if (block[set][way].prefetch)
        {
          pf_useful++;
          block[set][way].prefetch = 0;
        }
        block[set][way].used = 1;

        HIT[RQ.entry[index].type]++;
        ACCESS[RQ.entry[index].type]++;

        // remove this entry from RQ
        RQ.remove_queue(&RQ.entry[index]);
        reads_available_this_cycle--;
      }
      else
      { // read miss

        DP(if (warmup_complete[read_cpu]) {
                cout << "[" << NAME << "] " << __func__ << " read miss";
                cout << " instr_id: " << RQ.entry[index].instr_id << " address: " << hex << RQ.entry[index].address;
                cout << " full_addr: " << RQ.entry[index].full_addr << dec;
                cout << " cycle: " << RQ.entry[index].event_cycle << endl; });

        // check mshr
        uint8_t miss_handled = 1;
        int mshr_index = check_mshr(&RQ.entry[index]);

        if (mshr_index == -2)
        {
          // this is a data/instruction collision in the MSHR, so we have to wait before we can allocate this miss
          miss_handled = 0;
        }
        else if ((mshr_index == -1) && (MSHR.occupancy < MSHR_SIZE))
        { // this is a new miss

          if (cache_type == IS_LLC)
          {
            // check to make sure the DRAM RQ has room for this LLC read miss
            if (lower_level->get_occupancy(1, RQ.entry[index].address) == lower_level->get_size(1, RQ.entry[index].address))
            {
              miss_handled = 0;
            }
            else
            {
              add_mshr(&RQ.entry[index]);
              if (lower_level)
              {
                lower_level->add_rq(&RQ.entry[index]);
              }
            }
          }
          else
          {
            // add it to mshr (read miss)
            add_mshr(&RQ.entry[index]);

            // add it to the next level's read queue
            if (lower_level)
              lower_level->add_rq(&RQ.entry[index]);
            else
            { // this is the last level
              if (cache_type == IS_STLB)
              {
                // TODO: need to differentiate page table walk and actual swap

                // emulate page table walk
                uint64_t pa = va_to_pa(read_cpu, RQ.entry[index].instr_id, RQ.entry[index].full_addr, RQ.entry[index].address, 0);

                RQ.entry[index].data = pa >> LOG2_PAGE_SIZE;
                RQ.entry[index].event_cycle = current_core_cycle[read_cpu];
                return_data(&RQ.entry[index]);
              }
            }
          }
        }
        else
        {
          if ((mshr_index == -1) && (MSHR.occupancy == MSHR_SIZE))
          { // not enough MSHR resource

            // cannot handle miss request until one of MSHRs is available
            miss_handled = 0;
            STALL[RQ.entry[index].type]++;
          }
          else if (mshr_index != -1)
          { // already in-flight miss

            // mark merged consumer
            if (RQ.entry[index].type == RFO)
            {

              if (RQ.entry[index].tlb_access)
              {
                uint32_t sq_index = RQ.entry[index].sq_index;
                MSHR.entry[mshr_index].store_merged = 1;
                MSHR.entry[mshr_index].sq_index_depend_on_me.insert(sq_index);
                MSHR.entry[mshr_index].sq_index_depend_on_me.join(RQ.entry[index].sq_index_depend_on_me, SQ_SIZE);
              }

              if (RQ.entry[index].load_merged)
              {
                // uint32_t lq_index = RQ.entry[index].lq_index;
                MSHR.entry[mshr_index].load_merged = 1;
                // MSHR.entry[mshr_index].lq_index_depend_on_me[lq_index] = 1;
                MSHR.entry[mshr_index].lq_index_depend_on_me.join(RQ.entry[index].lq_index_depend_on_me, LQ_SIZE);
              }
            }
            else
            {
              if (RQ.entry[index].instruction)
              {
                uint32_t rob_index = RQ.entry[index].rob_index;
                MSHR.entry[mshr_index].instruction = 1; // add as instruction type
                MSHR.entry[mshr_index].instr_merged = 1;
                MSHR.entry[mshr_index].rob_index_depend_on_me.insert(rob_index);

                DP(if (warmup_complete[MSHR.entry[mshr_index].cpu]) {
                                cout << "[INSTR_MERGED] " << __func__ << " cpu: " << MSHR.entry[mshr_index].cpu << " instr_id: " << MSHR.entry[mshr_index].instr_id;
                                cout << " merged rob_index: " << rob_index << " instr_id: " << RQ.entry[index].instr_id << endl; });

                if (RQ.entry[index].instr_merged)
                {
                  MSHR.entry[mshr_index].rob_index_depend_on_me.join(RQ.entry[index].rob_index_depend_on_me, ROB_SIZE);
                  DP(if (warmup_complete[MSHR.entry[mshr_index].cpu]) {
                                    cout << "[INSTR_MERGED] " << __func__ << " cpu: " << MSHR.entry[mshr_index].cpu << " instr_id: " << MSHR.entry[mshr_index].instr_id;
                                    cout << " merged rob_index: " << i << " instr_id: N/A" << endl; });
                }
              }
              else
              {
                uint32_t lq_index = RQ.entry[index].lq_index;
                MSHR.entry[mshr_index].is_data = 1; // add as data type
                MSHR.entry[mshr_index].load_merged = 1;
                MSHR.entry[mshr_index].lq_index_depend_on_me.insert(lq_index);

                DP(if (warmup_complete[read_cpu]) {
                                cout << "[DATA_MERGED] " << __func__ << " cpu: " << read_cpu << " instr_id: " << RQ.entry[index].instr_id;
                                cout << " merged rob_index: " << RQ.entry[index].rob_index << " instr_id: " << RQ.entry[index].instr_id << " lq_index: " << RQ.entry[index].lq_index << endl; });
                MSHR.entry[mshr_index].lq_index_depend_on_me.join(RQ.entry[index].lq_index_depend_on_me, LQ_SIZE);
                if (RQ.entry[index].store_merged)
                {
                  MSHR.entry[mshr_index].store_merged = 1;
                  MSHR.entry[mshr_index].sq_index_depend_on_me.join(RQ.entry[index].sq_index_depend_on_me, SQ_SIZE);
                }
              }
            }

            // update fill_level
            if (RQ.entry[index].fill_level < MSHR.entry[mshr_index].fill_level)
              MSHR.entry[mshr_index].fill_level = RQ.entry[index].fill_level;

            if ((RQ.entry[index].fill_l1i) && (MSHR.entry[mshr_index].fill_l1i != 1))
            {
              MSHR.entry[mshr_index].fill_l1i = 1;
            }
            if ((RQ.entry[index].fill_l1d) && (MSHR.entry[mshr_index].fill_l1d != 1))
            {
              MSHR.entry[mshr_index].fill_l1d = 1;
            }

            // update request
            if (MSHR.entry[mshr_index].type == PREFETCH)
            {
              uint8_t prior_returned = MSHR.entry[mshr_index].returned;
              uint64_t prior_event_cycle = MSHR.entry[mshr_index].event_cycle;
              MSHR.entry[mshr_index] = RQ.entry[index];

              // in case request is already returned, we should keep event_cycle and retunred variables
              MSHR.entry[mshr_index].returned = prior_returned;
              MSHR.entry[mshr_index].event_cycle = prior_event_cycle;
            }

            MSHR_MERGED[RQ.entry[index].type]++;

            DP(if (warmup_complete[read_cpu]) {
                        cout << "[" << NAME << "] " << __func__ << " mshr merged";
                        cout << " instr_id: " << RQ.entry[index].instr_id << " prior_id: " << MSHR.entry[mshr_index].instr_id; 
                        cout << " address: " << hex << RQ.entry[index].address;
                        cout << " full_addr: " << RQ.entry[index].full_addr << dec;
                        cout << " cycle: " << RQ.entry[index].event_cycle << endl; });
          }
          else
          { // WE SHOULD NOT REACH HERE
            cerr << "[" << NAME << "] MSHR errors" << endl;
            assert(0);
          }
        }

        if (miss_handled)
        {
          // update prefetcher on load instruction
          if (RQ.entry[index].type == LOAD)
          {
            if (cache_type == IS_L1I)
              l1i_prefetcher_cache_operate(read_cpu, RQ.entry[index].ip, 0, 0);
            if (cache_type == IS_L1D)
              l1d_prefetcher_operate(RQ.entry[index].full_addr, RQ.entry[index].ip, 0, RQ.entry[index].type);
            if (cache_type == IS_L2C)
              l2c_prefetcher_operate(RQ.entry[index].address << LOG2_BLOCK_SIZE, RQ.entry[index].ip, 0, RQ.entry[index].type, 0);
            if (cache_type == IS_LLC)
            {
              cpu = read_cpu;
              llc_prefetcher_operate(RQ.entry[index].address << LOG2_BLOCK_SIZE, RQ.entry[index].ip, 0, RQ.entry[index].type, 0);
              cpu = 0;
            }
          }

          MISS[RQ.entry[index].type]++;
          ACCESS[RQ.entry[index].type]++;

          // remove this entry from RQ
          RQ.remove_queue(&RQ.entry[index]);
          reads_available_this_cycle--;
        }
      }
    }
    else
    {
      return;
    }

    if (reads_available_this_cycle == 0)
    {
      return;
    }
  }
}

void CACHE::handle_prefetch()
{
  // handle prefetch

  for (uint32_t i = 0; i < MAX_READ; i++)
  {

    uint32_t prefetch_cpu = PQ.entry[PQ.head].cpu;
    if (prefetch_cpu == NUM_CPUS)
      return;

    // handle the oldest entry
    if ((PQ.entry[PQ.head].event_cycle <= current_core_cycle[prefetch_cpu]) && (PQ.occupancy > 0))
    {
      int index = PQ.head;

      // access cache
      uint32_t set = get_set(PQ.entry[index].address);
      int way = check_hit(&PQ.entry[index]);

      if (way >= 0)
      { // prefetch hit

        // update replacement policy
        if (cache_type == IS_LLC)
        {
          llc_update_replacement_state(prefetch_cpu, set, way, block[set][way].full_addr, PQ.entry[index].ip, 0, PQ.entry[index].type, 1);
        }
        else
          update_replacement_state(prefetch_cpu, set, way, block[set][way].full_addr, PQ.entry[index].ip, 0, PQ.entry[index].type, 1);

        // COLLECT STATS
        sim_hit[prefetch_cpu][PQ.entry[index].type]++;
        sim_access[prefetch_cpu][PQ.entry[index].type]++;

        // run prefetcher on prefetches from higher caches
        if (PQ.entry[index].pf_origin_level < fill_level)
        {
          if (cache_type == IS_L1D)
            l1d_prefetcher_operate(PQ.entry[index].full_addr, PQ.entry[index].ip, 1, PREFETCH);
          else if (cache_type == IS_L2C)
            PQ.entry[index].pf_metadata = l2c_prefetcher_operate(block[set][way].address << LOG2_BLOCK_SIZE, PQ.entry[index].ip, 1, PREFETCH, PQ.entry[index].pf_metadata);
          else if (cache_type == IS_LLC)
          {
            cpu = prefetch_cpu;
            PQ.entry[index].pf_metadata = llc_prefetcher_operate(block[set][way].address << LOG2_BLOCK_SIZE, PQ.entry[index].ip, 1, PREFETCH, PQ.entry[index].pf_metadata);
            cpu = 0;
          }
        }

        // check fill level
        if (PQ.entry[index].fill_level < fill_level)
        {

          if (fill_level == FILL_L2)
          {
            if (PQ.entry[index].fill_l1i)
            {
              upper_level_icache[prefetch_cpu]->return_data(&PQ.entry[index]);
            }
            if (PQ.entry[index].fill_l1d)
            {
              upper_level_dcache[prefetch_cpu]->return_data(&PQ.entry[index]);
            }
          }
          else
          {
            if (PQ.entry[index].instruction)
              upper_level_icache[prefetch_cpu]->return_data(&PQ.entry[index]);
            if (PQ.entry[index].is_data)
              upper_level_dcache[prefetch_cpu]->return_data(&PQ.entry[index]);
          }
        }

        HIT[PQ.entry[index].type]++;
        ACCESS[PQ.entry[index].type]++;

        // remove this entry from PQ
        PQ.remove_queue(&PQ.entry[index]);
        reads_available_this_cycle--;
      }
      else
      { // prefetch miss

        DP(if (warmup_complete[prefetch_cpu]) {
                cout << "[" << NAME << "] " << __func__ << " prefetch miss";
                cout << " instr_id: " << PQ.entry[index].instr_id << " address: " << hex << PQ.entry[index].address;
                cout << " full_addr: " << PQ.entry[index].full_addr << dec << " fill_level: " << PQ.entry[index].fill_level;
                cout << " cycle: " << PQ.entry[index].event_cycle << endl; });

        // check mshr
        uint8_t miss_handled = 1;
        int mshr_index = check_mshr(&PQ.entry[index]);

        if (mshr_index == -2)
        {
          // this is a data/instruction collision in the MSHR, so we have to wait before we can allocate this miss
          miss_handled = 0;
        }
        else if ((mshr_index == -1) && (MSHR.occupancy < MSHR_SIZE))
        { // this is a new miss

          DP(if (warmup_complete[PQ.entry[index].cpu]) {
                    cout << "[" << NAME << "_PQ] " <<  __func__ << " want to add instr_id: " << PQ.entry[index].instr_id << " address: " << hex << PQ.entry[index].address;
                    cout << " full_addr: " << PQ.entry[index].full_addr << dec;
                    cout << " occupancy: " << lower_level->get_occupancy(3, PQ.entry[index].address) << " SIZE: " << lower_level->get_size(3, PQ.entry[index].address) << endl; });

          // first check if the lower level PQ is full or not
          // this is possible since multiple prefetchers can exist at each level of caches
          if (lower_level)
          {
            if (cache_type == IS_LLC)
            {
              if (lower_level->get_occupancy(1, PQ.entry[index].address) == lower_level->get_size(1, PQ.entry[index].address))
                miss_handled = 0;
              else
              {

                // run prefetcher on prefetches from higher caches
                if (PQ.entry[index].pf_origin_level < fill_level)
                {
                  if (cache_type == IS_LLC)
                  {
                    cpu = prefetch_cpu;
                    PQ.entry[index].pf_metadata = llc_prefetcher_operate(PQ.entry[index].address << LOG2_BLOCK_SIZE, PQ.entry[index].ip, 0, PREFETCH, PQ.entry[index].pf_metadata);
                    cpu = 0;
                  }
                }

                // add it to MSHRs if this prefetch miss will be filled to this cache level
                if (PQ.entry[index].fill_level <= fill_level)
                  add_mshr(&PQ.entry[index]);

                lower_level->add_rq(&PQ.entry[index]); // add it to the DRAM RQ
              }
            }
            else
            {
              if (lower_level->get_occupancy(3, PQ.entry[index].address) == lower_level->get_size(3, PQ.entry[index].address))
                miss_handled = 0;
              else
              {

                // run prefetcher on prefetches from higher caches
                if (PQ.entry[index].pf_origin_level < fill_level)
                {
                  if (cache_type == IS_L1D)
                    l1d_prefetcher_operate(PQ.entry[index].full_addr, PQ.entry[index].ip, 0, PREFETCH);
                  if (cache_type == IS_L2C)
                    PQ.entry[index].pf_metadata = l2c_prefetcher_operate(PQ.entry[index].address << LOG2_BLOCK_SIZE, PQ.entry[index].ip, 0, PREFETCH, PQ.entry[index].pf_metadata);
                }

                // add it to MSHRs if this prefetch miss will be filled to this cache level
                if (PQ.entry[index].fill_level <= fill_level)
                  add_mshr(&PQ.entry[index]);

                lower_level->add_pq(&PQ.entry[index]); // add it to the DRAM RQ
              }
            }
          }
        }
        else
        {
          if ((mshr_index == -1) && (MSHR.occupancy == MSHR_SIZE))
          { // not enough MSHR resource

            // TODO: should we allow prefetching with lower fill level at this case?

            // cannot handle miss request until one of MSHRs is available
            miss_handled = 0;
            STALL[PQ.entry[index].type]++;
          }
          else if (mshr_index != -1)
          { // already in-flight miss

            // no need to update request except fill_level
            // update fill_level
            if (PQ.entry[index].fill_level < MSHR.entry[mshr_index].fill_level)
              MSHR.entry[mshr_index].fill_level = PQ.entry[index].fill_level;

            if ((PQ.entry[index].fill_l1i) && (MSHR.entry[mshr_index].fill_l1i != 1))
            {
              MSHR.entry[mshr_index].fill_l1i = 1;
            }
            if ((PQ.entry[index].fill_l1d) && (MSHR.entry[mshr_index].fill_l1d != 1))
            {
              MSHR.entry[mshr_index].fill_l1d = 1;
            }

            MSHR_MERGED[PQ.entry[index].type]++;

            DP(if (warmup_complete[prefetch_cpu]) {
                        cout << "[" << NAME << "] " << __func__ << " mshr merged";
                        cout << " instr_id: " << PQ.entry[index].instr_id << " prior_id: " << MSHR.entry[mshr_index].instr_id; 
                        cout << " address: " << hex << PQ.entry[index].address;
                        cout << " full_addr: " << PQ.entry[index].full_addr << dec << " fill_level: " << MSHR.entry[mshr_index].fill_level;
                        cout << " cycle: " << MSHR.entry[mshr_index].event_cycle << endl; });
          }
          else
          { // WE SHOULD NOT REACH HERE
            cerr << "[" << NAME << "] MSHR errors" << endl;
            assert(0);
          }
        }

        if (miss_handled)
        {

          DP(if (warmup_complete[prefetch_cpu]) {
                    cout << "[" << NAME << "] " << __func__ << " prefetch miss handled";
                    cout << " instr_id: " << PQ.entry[index].instr_id << " address: " << hex << PQ.entry[index].address;
                    cout << " full_addr: " << PQ.entry[index].full_addr << dec << " fill_level: " << PQ.entry[index].fill_level;
                    cout << " cycle: " << PQ.entry[index].event_cycle << endl; });

          MISS[PQ.entry[index].type]++;
          ACCESS[PQ.entry[index].type]++;

          // remove this entry from PQ
          PQ.remove_queue(&PQ.entry[index]);
          reads_available_this_cycle--;
        }
      }
    }
    else
    {
      return;
    }

    if (reads_available_this_cycle == 0)
    {
      return;
    }
  }
}

void CACHE::operate()
{
  if (NAME == "LLC")
  {
    if (current_core_cycle[0] / 5000000 != partition_count)
    {
      vector<uint32_t> new_allocations = partition_algorithm();
      if (partition_count == 0)
        cerr << "Partition Changes every 5000000 cycles:\n";
      cerr<<(partition_count+1)*5000000<<' ';
      for (auto i : new_allocations)
        cerr << i << ' ';
      cerr << endl;
      vector<uint32_t> extra;     // Contains apps with extra ways
      vector<uint32_t> deficient; // Contains apps with deficient ways
      for (uint32_t application = 0; application < NUM_CPUS; application++)
      {
        // Partition array for cpus assumed
        if (partitions[application] > new_allocations[application])
        {
          extra.push_back(application); // Counting the number of cores with extra ways
        }
        if (partitions[application] < new_allocations[application])
        {
          deficient.push_back(application); // Counting the number of cores which need more ways
        }
      }
      for (int set = 0; set < NUM_SET; set++)
      {
        vector<uint32_t> to_allocate;
        for (int way = 0; way < NUM_WAY; way++)
        {
          if (block[set][way].lru >= new_allocations[block[set][way].cpu])
          {
            to_allocate.push_back(way); // We count the ways that currently have higher LRU values than allowed
          }
        }
        for (uint32_t i = 0; i < extra.size(); i++)
        {
          // We reduce the number of ways to the number given by the partitioning algorithm
          // (Only for the cores which have more ways than required)
          partitions[extra[i]] = new_allocations[extra[i]];
        }
        vector<int> copy_partitions;
        for (auto i : partitions)
        {
          // Making a copy of the current partitions that we will modify for each set
          copy_partitions.push_back(i);
        }
        for (uint32_t i = 0; i < deficient.size(); i++)
        {
          while (copy_partitions[deficient[i]] < new_allocations[deficient[i]])
          {
            // We allocate the ways that we have collected from the other cores to the cores that need more ways
            uint32_t req_way = to_allocate[to_allocate.size() - 1];
            block[set][req_way].cpu = deficient[i];
            // block[set][req_way].valid = 0;
            block[set][req_way].lru = copy_partitions[deficient[i]];
            copy_partitions[deficient[i]]++;
            to_allocate.pop_back();
          }
        }
      }
      for (int i = 0; i < partitions.size(); i++)
      {
        // We set the partitions of each core to the value given by the partitioning algorithm
        partitions[i] = new_allocations[i];
      }
      partition_count++;
    }
  }
  handle_fill();
  handle_writeback();
  reads_available_this_cycle = MAX_READ;
  handle_read();

  if (PQ.occupancy && (reads_available_this_cycle > 0))
    handle_prefetch();
}

uint32_t CACHE::get_set(uint64_t address)
{
  return (uint32_t)(address & ((1 << lg2(NUM_SET)) - 1));
}

uint32_t CACHE::get_way(uint64_t address, uint32_t set)
{
  for (uint32_t way = 0; way < NUM_WAY; way++)
  {
    if (block[set][way].valid && (block[set][way].tag == address))
      return way;
  }

  return NUM_WAY;
}

void CACHE::fill_cache(uint32_t set, uint32_t way, PACKET *packet)
{
#ifdef SANITY_CHECK
  if (cache_type == IS_ITLB)
  {
    if (packet->data == 0)
      assert(0);
  }

  if (cache_type == IS_DTLB)
  {
    if (packet->data == 0)
      assert(0);
  }

  if (cache_type == IS_STLB)
  {
    if (packet->data == 0)
      assert(0);
  }
#endif
  if (block[set][way].prefetch && (block[set][way].used == 0))
    pf_useless++;

  if (block[set][way].valid == 0)
    block[set][way].valid = 1;
  block[set][way].dirty = 0;
  block[set][way].prefetch = (packet->type == PREFETCH) ? 1 : 0;
  block[set][way].used = 0;

  if (block[set][way].prefetch)
    pf_fill++;

  block[set][way].delta = packet->delta;
  block[set][way].depth = packet->depth;
  block[set][way].signature = packet->signature;
  block[set][way].confidence = packet->confidence;

  block[set][way].tag = packet->address;
  block[set][way].address = packet->address;
  block[set][way].full_addr = packet->full_addr;
  block[set][way].data = packet->data;
  block[set][way].ip = packet->ip;
  block[set][way].cpu = packet->cpu;
  block[set][way].instr_id = packet->instr_id;

  DP(if (warmup_complete[packet->cpu]) {
    cout << "[" << NAME << "] " << __func__ << " set: " << set << " way: " << way;
    cout << " lru: " << block[set][way].lru << " tag: " << hex << block[set][way].tag << " full_addr: " << block[set][way].full_addr;
    cout << " data: " << block[set][way].data << dec << endl; });
}

void CACHE::fill_atd(uint32_t set, uint32_t way, PACKET *packet)
{
  /*
    Similar to fill_cache function
    Filling the ATD of the CPU of the requested packet
  */
  uint64_t curr_cpu = packet->cpu;

  if (atd[curr_cpu][set][way].valid == 0)
  {
    atd[curr_cpu][set][way].valid = 1;
  }

  atd[curr_cpu][set][way].dirty = 0;
  atd[curr_cpu][set][way].used = 0;
  atd[curr_cpu][set][way].depth = packet->depth;
  atd[curr_cpu][set][way].signature = packet->signature;
  atd[curr_cpu][set][way].confidence = packet->confidence;
  atd[curr_cpu][set][way].delta = packet->delta;

  atd[curr_cpu][set][way].tag = packet->address;
  atd[curr_cpu][set][way].address = packet->address;
  atd[curr_cpu][set][way].full_addr = packet->full_addr;
  atd[curr_cpu][set][way].ip = packet->ip;
  atd[curr_cpu][set][way].cpu = packet->cpu;
  atd[curr_cpu][set][way].instr_id = packet->instr_id;

  DP(if (warmup_complete[packet->cpu]) {
    cout << "[" << NAME << "] " << __func__ << " set: " << set << " way: " << way;
    cout << " lru: " << atd[curr_cpu][set][way].lru << " tag: " << hex << atd[curr_cpu][set][way].tag << " full_addr: " << atd[curr_cpu][set][way].full_addr;
    cout << " data: " << atd[curr_cpu][set][way].data << dec << endl; });
}

int CACHE::check_hit(PACKET *packet)
{
  uint32_t set = get_set(packet->address);
  int match_way = -1;

  if (NUM_SET < set)
  {
    cerr << "[" << NAME << "_ERROR] " << __func__ << " invalid set index: " << set << " NUM_SET: " << NUM_SET;
    cerr << " address: " << hex << packet->address << " full_addr: " << packet->full_addr << dec;
    cerr << " event: " << packet->event_cycle << endl;
    assert(0);
  }
  // hit
  if (NAME != "LLC")
    for (uint32_t way = 0; way < NUM_WAY; way++)
    {
      if (block[set][way].valid && (block[set][way].tag == packet->address))
      {

        match_way = way;

        DP(if (warmup_complete[packet->cpu]) {
            cout << "[" << NAME << "] " << __func__ << " instr_id: " << packet->instr_id << " type: " << +packet->type << hex << " addr: " << packet->address;
            cout << " full_addr: " << packet->full_addr << " tag: " << block[set][way].tag << " data: " << block[set][way].data << dec;
            cout << " set: " << set << " way: " << way << " lru: " << block[set][way].lru;
            cout << " event: " << packet->event_cycle << " cycle: " << current_core_cycle[cpu] << endl; });

        break;
      }
    }
  else
  {
    for (uint32_t way = 0; way < NUM_WAY; way++)
    {
      if (block[set][way].valid && (block[set][way].tag == packet->address) && block[set][way].cpu == packet->cpu)
      {

        match_way = way;

        DP(if (warmup_complete[packet->cpu]) {
                  cout << "[" << NAME << "] " << __func__ << " instr_id: " << packet->instr_id << " type: " << +packet->type << hex << " addr: " << packet->address;
                  cout << " full_addr: " << packet->full_addr << " tag: " << block[set][way].tag << " data: " << block[set][way].data << dec;
                  cout << " set: " << set << " way: " << way << " lru: " << block[set][way].lru;
                  cout << " event: " << packet->event_cycle << " cycle: " << current_core_cycle[cpu] << endl; });

        break;
      }
    }
  }
  return match_way;
}

int CACHE::check_hit_atd(PACKET *packet)
{
  /*
    Checking for hit in the ATD
  */
  uint32_t set = get_set(packet->address) / (NUM_SET / 32); // Mapping requested in the ATD
  int match_way = -1;
  int curr_cpu = packet->cpu; // Finding the CPU of the requested packet

  if (NAME == "LLC")
    for (uint32_t way = 0; way < NUM_WAY; way++)
    {
      if (atd[curr_cpu][set][way].valid && (atd[curr_cpu][set][way].tag == packet->address)) // Checking for hit in each of the ways in ATD of requested CPU
      {
        match_way = way;
        hit_counts[curr_cpu][atd[curr_cpu][set][way].lru]++; // Incrementing the hit counts of the LRU position of the ATD
        DP(if (warmup_complete[packet->cpu]) {
              cout << "[" << NAME << "] " << __func__ << " instr_id: " << packet->instr_id << " type: " << +packet->type << hex << " addr: " << packet->address;
              cout << " full_addr: " << packet->full_addr << " tag: " << block[set][way].tag << " data: " << block[set][way].data << dec;
              cout << " set: " << set << " way: " << way << " lru: " << block[set][way].lru;
              cout << " event: " << packet->event_cycle << " cycle: " << current_core_cycle[cpu] << endl; });

        break;
      }
    }
  return match_way; // Returning the matched way (-1 in case of miss)
}

int CACHE::invalidate_entry(uint64_t inval_addr)
{
  uint32_t set = get_set(inval_addr);
  int match_way = -1;

  if (NUM_SET < set)
  {
    cerr << "[" << NAME << "_ERROR] " << __func__ << " invalid set index: " << set << " NUM_SET: " << NUM_SET;
    cerr << " inval_addr: " << hex << inval_addr << dec << endl;
    assert(0);
  }

  // invalidate
  for (uint32_t way = 0; way < NUM_WAY; way++)
  {
    if (block[set][way].valid && (block[set][way].tag == inval_addr))
    {

      block[set][way].valid = 0;

      match_way = way;

      DP(if (warmup_complete[cpu]) {
            cout << "[" << NAME << "] " << __func__ << " inval_addr: " << hex << inval_addr;  
            cout << " tag: " << block[set][way].tag << " data: " << block[set][way].data << dec;
            cout << " set: " << set << " way: " << way << " lru: " << block[set][way].lru << " cycle: " << current_core_cycle[cpu] << endl; });

      break;
    }
  }

  return match_way;
}

int CACHE::add_rq(PACKET *packet)
{
  // check for the latest wirtebacks in the write queue
  int wq_index = WQ.check_queue(packet);
  if (wq_index != -1)
  {

    // check fill level
    if (packet->fill_level < fill_level)
    {

      packet->data = WQ.entry[wq_index].data;

      if (fill_level == FILL_L2)
      {
        if (packet->fill_l1i)
        {
          upper_level_icache[packet->cpu]->return_data(packet);
        }
        if (packet->fill_l1d)
        {
          upper_level_dcache[packet->cpu]->return_data(packet);
        }
      }
      else
      {
        if (packet->instruction)
          upper_level_icache[packet->cpu]->return_data(packet);
        if (packet->is_data)
          upper_level_dcache[packet->cpu]->return_data(packet);
      }
    }

#ifdef SANITY_CHECK
    if (cache_type == IS_ITLB)
      assert(0);
    else if (cache_type == IS_DTLB)
      assert(0);
    else if (cache_type == IS_L1I)
      assert(0);
#endif
    // update processed packets
    if ((cache_type == IS_L1D) && (packet->type != PREFETCH))
    {
      if (PROCESSED.occupancy < PROCESSED.SIZE)
        PROCESSED.add_queue(packet);

      DP(if (warmup_complete[packet->cpu]) {
            cout << "[" << NAME << "_RQ] " << __func__ << " instr_id: " << packet->instr_id << " found recent writebacks";
            cout << hex << " read: " << packet->address << " writeback: " << WQ.entry[wq_index].address << dec;
            cout << " index: " << MAX_READ << " rob_signal: " << packet->rob_signal << endl; });
    }

    HIT[packet->type]++;
    ACCESS[packet->type]++;

    WQ.FORWARD++;
    RQ.ACCESS++;

    return -1;
  }

  // check for duplicates in the read queue
  int index = RQ.check_queue(packet);
  if (index != -1)
  {

    if (packet->instruction)
    {
      uint32_t rob_index = packet->rob_index;
      RQ.entry[index].rob_index_depend_on_me.insert(rob_index);
      RQ.entry[index].instruction = 1; // add as instruction type
      RQ.entry[index].instr_merged = 1;

      DP(if (warmup_complete[packet->cpu]) {
            cout << "[INSTR_MERGED] " << __func__ << " cpu: " << packet->cpu << " instr_id: " << RQ.entry[index].instr_id;
            cout << " merged rob_index: " << rob_index << " instr_id: " << packet->instr_id << endl; });
    }
    else
    {
      // mark merged consumer
      if (packet->type == RFO)
      {

        uint32_t sq_index = packet->sq_index;
        RQ.entry[index].sq_index_depend_on_me.insert(sq_index);
        RQ.entry[index].store_merged = 1;
      }
      else
      {
        uint32_t lq_index = packet->lq_index;
        RQ.entry[index].lq_index_depend_on_me.insert(lq_index);
        RQ.entry[index].load_merged = 1;

        DP(if (warmup_complete[packet->cpu]) {
                cout << "[DATA_MERGED] " << __func__ << " cpu: " << packet->cpu << " instr_id: " << RQ.entry[index].instr_id;
                cout << " merged rob_index: " << packet->rob_index << " instr_id: " << packet->instr_id << " lq_index: " << packet->lq_index << endl; });
      }
      RQ.entry[index].is_data = 1; // add as data type
    }

    if ((packet->fill_l1i) && (RQ.entry[index].fill_l1i != 1))
    {
      RQ.entry[index].fill_l1i = 1;
    }
    if ((packet->fill_l1d) && (RQ.entry[index].fill_l1d != 1))
    {
      RQ.entry[index].fill_l1d = 1;
    }

    RQ.MERGED++;
    RQ.ACCESS++;

    return index; // merged index
  }

  // check occupancy
  if (RQ.occupancy == RQ_SIZE)
  {
    RQ.FULL++;

    return -2; // cannot handle this request
  }

  // if there is no duplicate, add it to RQ
  index = RQ.tail;

#ifdef SANITY_CHECK
  if (RQ.entry[index].address != 0)
  {
    cerr << "[" << NAME << "_ERROR] " << __func__ << " is not empty index: " << index;
    cerr << " address: " << hex << RQ.entry[index].address;
    cerr << " full_addr: " << RQ.entry[index].full_addr << dec << endl;
    assert(0);
  }
#endif

  RQ.entry[index] = *packet;

  // ADD LATENCY
  if (RQ.entry[index].event_cycle < current_core_cycle[packet->cpu])
    RQ.entry[index].event_cycle = current_core_cycle[packet->cpu] + LATENCY;
  else
    RQ.entry[index].event_cycle += LATENCY;

  RQ.occupancy++;
  RQ.tail++;
  if (RQ.tail >= RQ.SIZE)
    RQ.tail = 0;

  DP(if (warmup_complete[RQ.entry[index].cpu]) {
    cout << "[" << NAME << "_RQ] " <<  __func__ << " instr_id: " << RQ.entry[index].instr_id << " address: " << hex << RQ.entry[index].address;
    cout << " full_addr: " << RQ.entry[index].full_addr << dec;
    cout << " type: " << +RQ.entry[index].type << " head: " << RQ.head << " tail: " << RQ.tail << " occupancy: " << RQ.occupancy;
    cout << " event: " << RQ.entry[index].event_cycle << " current: " << current_core_cycle[RQ.entry[index].cpu] << endl; });

  if (packet->address == 0)
    assert(0);

  RQ.TO_CACHE++;
  RQ.ACCESS++;

  return -1;
}

int CACHE::add_wq(PACKET *packet)
{
  // check for duplicates in the write queue
  int index = WQ.check_queue(packet);
  if (index != -1)
  {

    WQ.MERGED++;
    WQ.ACCESS++;

    return index; // merged index
  }

  // sanity check
  if (WQ.occupancy >= WQ.SIZE)
    assert(0);

  // if there is no duplicate, add it to the write queue
  index = WQ.tail;
  if (WQ.entry[index].address != 0)
  {
    cerr << "[" << NAME << "_ERROR] " << __func__ << " is not empty index: " << index;
    cerr << " address: " << hex << WQ.entry[index].address;
    cerr << " full_addr: " << WQ.entry[index].full_addr << dec << endl;
    assert(0);
  }

  WQ.entry[index] = *packet;

  // ADD LATENCY
  if (WQ.entry[index].event_cycle < current_core_cycle[packet->cpu])
    WQ.entry[index].event_cycle = current_core_cycle[packet->cpu] + LATENCY;
  else
    WQ.entry[index].event_cycle += LATENCY;

  WQ.occupancy++;
  WQ.tail++;
  if (WQ.tail >= WQ.SIZE)
    WQ.tail = 0;

  DP(if (warmup_complete[WQ.entry[index].cpu]) {
    cout << "[" << NAME << "_WQ] " <<  __func__ << " instr_id: " << WQ.entry[index].instr_id << " address: " << hex << WQ.entry[index].address;
    cout << " full_addr: " << WQ.entry[index].full_addr << dec;
    cout << " head: " << WQ.head << " tail: " << WQ.tail << " occupancy: " << WQ.occupancy;
    cout << " data: " << hex << WQ.entry[index].data << dec;
    cout << " event: " << WQ.entry[index].event_cycle << " current: " << current_core_cycle[WQ.entry[index].cpu] << endl; });

  WQ.TO_CACHE++;
  WQ.ACCESS++;

  return -1;
}

int CACHE::prefetch_line(uint64_t ip, uint64_t base_addr, uint64_t pf_addr, int pf_fill_level, uint32_t prefetch_metadata)
{
  pf_requested++;

  if (PQ.occupancy < PQ.SIZE)
  {
    if ((base_addr >> LOG2_PAGE_SIZE) == (pf_addr >> LOG2_PAGE_SIZE))
    {

      PACKET pf_packet;
      pf_packet.fill_level = pf_fill_level;
      pf_packet.pf_origin_level = fill_level;
      if (pf_fill_level == FILL_L1)
      {
        pf_packet.fill_l1d = 1;
      }
      pf_packet.pf_metadata = prefetch_metadata;
      pf_packet.cpu = cpu;
      // pf_packet.data_index = LQ.entry[lq_index].data_index;
      // pf_packet.lq_index = lq_index;
      pf_packet.address = pf_addr >> LOG2_BLOCK_SIZE;
      pf_packet.full_addr = pf_addr;
      // pf_packet.instr_id = LQ.entry[lq_index].instr_id;
      // pf_packet.rob_index = LQ.entry[lq_index].rob_index;
      pf_packet.ip = ip;
      pf_packet.type = PREFETCH;
      pf_packet.event_cycle = current_core_cycle[cpu];

      // give a dummy 0 as the IP of a prefetch
      add_pq(&pf_packet);

      pf_issued++;

      return 1;
    }
  }

  return 0;
}

int CACHE::kpc_prefetch_line(uint64_t base_addr, uint64_t pf_addr, int pf_fill_level, int delta, int depth, int signature, int confidence, uint32_t prefetch_metadata)
{
  if (PQ.occupancy < PQ.SIZE)
  {
    if ((base_addr >> LOG2_PAGE_SIZE) == (pf_addr >> LOG2_PAGE_SIZE))
    {

      PACKET pf_packet;
      pf_packet.fill_level = pf_fill_level;
      pf_packet.pf_origin_level = fill_level;
      if (pf_fill_level == FILL_L1)
      {
        pf_packet.fill_l1d = 1;
      }
      pf_packet.pf_metadata = prefetch_metadata;
      pf_packet.cpu = cpu;
      // pf_packet.data_index = LQ.entry[lq_index].data_index;
      // pf_packet.lq_index = lq_index;
      pf_packet.address = pf_addr >> LOG2_BLOCK_SIZE;
      pf_packet.full_addr = pf_addr;
      // pf_packet.instr_id = LQ.entry[lq_index].instr_id;
      // pf_packet.rob_index = LQ.entry[lq_index].rob_index;
      pf_packet.ip = 0;
      pf_packet.type = PREFETCH;
      pf_packet.delta = delta;
      pf_packet.depth = depth;
      pf_packet.signature = signature;
      pf_packet.confidence = confidence;
      pf_packet.event_cycle = current_core_cycle[cpu];

      // give a dummy 0 as the IP of a prefetch
      add_pq(&pf_packet);

      pf_issued++;

      return 1;
    }
  }

  return 0;
}

int CACHE::add_pq(PACKET *packet)
{
  // check for the latest wirtebacks in the write queue
  int wq_index = WQ.check_queue(packet);
  if (wq_index != -1)
  {

    // check fill level
    if (packet->fill_level < fill_level)
    {

      packet->data = WQ.entry[wq_index].data;

      if (fill_level == FILL_L2)
      {
        if (packet->fill_l1i)
        {
          upper_level_icache[packet->cpu]->return_data(packet);
        }
        if (packet->fill_l1d)
        {
          upper_level_dcache[packet->cpu]->return_data(packet);
        }
      }
      else
      {
        if (packet->instruction)
          upper_level_icache[packet->cpu]->return_data(packet);
        if (packet->is_data)
          upper_level_dcache[packet->cpu]->return_data(packet);
      }
    }

    HIT[packet->type]++;
    ACCESS[packet->type]++;

    WQ.FORWARD++;
    PQ.ACCESS++;

    return -1;
  }

  // check for duplicates in the PQ
  int index = PQ.check_queue(packet);
  if (index != -1)
  {
    if (packet->fill_level < PQ.entry[index].fill_level)
    {
      PQ.entry[index].fill_level = packet->fill_level;
    }
    if ((packet->instruction == 1) && (PQ.entry[index].instruction != 1))
    {
      PQ.entry[index].instruction = 1;
    }
    if ((packet->is_data == 1) && (PQ.entry[index].is_data != 1))
    {
      PQ.entry[index].is_data = 1;
    }
    if ((packet->fill_l1i) && (PQ.entry[index].fill_l1i != 1))
    {
      PQ.entry[index].fill_l1i = 1;
    }
    if ((packet->fill_l1d) && (PQ.entry[index].fill_l1d != 1))
    {
      PQ.entry[index].fill_l1d = 1;
    }

    PQ.MERGED++;
    PQ.ACCESS++;

    return index; // merged index
  }

  // check occupancy
  if (PQ.occupancy == PQ_SIZE)
  {
    PQ.FULL++;

    DP(if (warmup_complete[packet->cpu]) { cout << "[" << NAME << "] cannot process add_pq since it is full" << endl; });
    return -2; // cannot handle this request
  }

  // if there is no duplicate, add it to PQ
  index = PQ.tail;

#ifdef SANITY_CHECK
  if (PQ.entry[index].address != 0)
  {
    cerr << "[" << NAME << "_ERROR] " << __func__ << " is not empty index: " << index;
    cerr << " address: " << hex << PQ.entry[index].address;
    cerr << " full_addr: " << PQ.entry[index].full_addr << dec << endl;
    assert(0);
  }
#endif

  PQ.entry[index] = *packet;

  // ADD LATENCY
  if (PQ.entry[index].event_cycle < current_core_cycle[packet->cpu])
    PQ.entry[index].event_cycle = current_core_cycle[packet->cpu] + LATENCY;
  else
    PQ.entry[index].event_cycle += LATENCY;

  PQ.occupancy++;
  PQ.tail++;
  if (PQ.tail >= PQ.SIZE)
    PQ.tail = 0;

  DP(if (warmup_complete[PQ.entry[index].cpu]) {
    cout << "[" << NAME << "_PQ] " <<  __func__ << " instr_id: " << PQ.entry[index].instr_id << " address: " << hex << PQ.entry[index].address;
    cout << " full_addr: " << PQ.entry[index].full_addr << dec;
    cout << " type: " << +PQ.entry[index].type << " head: " << PQ.head << " tail: " << PQ.tail << " occupancy: " << PQ.occupancy;
    cout << " event: " << PQ.entry[index].event_cycle << " current: " << current_core_cycle[PQ.entry[index].cpu] << endl; });

  if (packet->address == 0)
    assert(0);

  PQ.TO_CACHE++;
  PQ.ACCESS++;

  return -1;
}

void CACHE::return_data(PACKET *packet)
{
  // check MSHR information
  int mshr_index = check_mshr(packet);

  // sanity check
  if (mshr_index == -1)
  {
    cerr << "[" << NAME << "_MSHR] " << __func__ << " instr_id: " << packet->instr_id << " cannot find a matching entry!";
    cerr << " full_addr: " << hex << packet->full_addr;
    cerr << " address: " << packet->address << dec;
    cerr << " event: " << packet->event_cycle << " current: " << current_core_cycle[packet->cpu] << endl;
    assert(0);
  }

  // MSHR holds the most updated information about this request
  // no need to do memcpy
  MSHR.num_returned++;
  MSHR.entry[mshr_index].returned = COMPLETED;
  MSHR.entry[mshr_index].data = packet->data;
  MSHR.entry[mshr_index].pf_metadata = packet->pf_metadata;

  // ADD LATENCY
  if (MSHR.entry[mshr_index].event_cycle < current_core_cycle[packet->cpu])
    MSHR.entry[mshr_index].event_cycle = current_core_cycle[packet->cpu] + LATENCY;
  else
    MSHR.entry[mshr_index].event_cycle += LATENCY;

  update_fill_cycle();

  DP(if (warmup_complete[packet->cpu]) {
    cout << "[" << NAME << "_MSHR] " <<  __func__ << " instr_id: " << MSHR.entry[mshr_index].instr_id;
    cout << " address: " << hex << MSHR.entry[mshr_index].address << " full_addr: " << MSHR.entry[mshr_index].full_addr;
    cout << " data: " << MSHR.entry[mshr_index].data << dec << " num_returned: " << MSHR.num_returned;
    cout << " index: " << mshr_index << " occupancy: " << MSHR.occupancy;
    cout << " event: " << MSHR.entry[mshr_index].event_cycle << " current: " << current_core_cycle[packet->cpu] << " next: " << MSHR.next_fill_cycle << endl; });
}

void CACHE::update_fill_cycle()
{
  // update next_fill_cycle
  uint64_t min_cycle = UINT64_MAX;
  uint32_t min_index = MSHR.SIZE;
  for (uint32_t i = 0; i < MSHR.SIZE; i++)
  {
    if ((MSHR.entry[i].returned == COMPLETED) && (MSHR.entry[i].event_cycle < min_cycle))
    {
      min_cycle = MSHR.entry[i].event_cycle;
      min_index = i;
    }

    DP(if (warmup_complete[MSHR.entry[i].cpu]) {
        cout << "[" << NAME << "_MSHR] " <<  __func__ << " checking instr_id: " << MSHR.entry[i].instr_id;
        cout << " address: " << hex << MSHR.entry[i].address << " full_addr: " << MSHR.entry[i].full_addr;
        cout << " data: " << MSHR.entry[i].data << dec << " returned: " << +MSHR.entry[i].returned << " fill_level: " << MSHR.entry[i].fill_level;
        cout << " index: " << i << " occupancy: " << MSHR.occupancy;
        cout << " event: " << MSHR.entry[i].event_cycle << " current: " << current_core_cycle[MSHR.entry[i].cpu] << " next: " << MSHR.next_fill_cycle << endl; });
  }

  MSHR.next_fill_cycle = min_cycle;
  MSHR.next_fill_index = min_index;
  if (min_index < MSHR.SIZE)
  {

    DP(if (warmup_complete[MSHR.entry[min_index].cpu]) {
        cout << "[" << NAME << "_MSHR] " <<  __func__ << " instr_id: " << MSHR.entry[min_index].instr_id;
        cout << " address: " << hex << MSHR.entry[min_index].address << " full_addr: " << MSHR.entry[min_index].full_addr;
        cout << " data: " << MSHR.entry[min_index].data << dec << " num_returned: " << MSHR.num_returned;
        cout << " event: " << MSHR.entry[min_index].event_cycle << " current: " << current_core_cycle[MSHR.entry[min_index].cpu] << " next: " << MSHR.next_fill_cycle << endl; });
  }
}

int CACHE::check_mshr(PACKET *packet)
{
  // search mshr
  // bool instruction_and_data_collision = false;

  for (uint32_t index = 0; index < MSHR_SIZE; index++)
  {
    if (MSHR.entry[index].address == packet->address)
    {
      // if(MSHR.entry[index].instruction != packet->instruction)
      //   {
      //     instruction_and_data_collision = true;
      //   }
      // else
      //   {
      DP(if (warmup_complete[packet->cpu]) {
		  cout << "[" << NAME << "_MSHR] " << __func__ << " same entry instr_id: " << packet->instr_id << " prior_id: " << MSHR.entry[index].instr_id;
		  cout << " address: " << hex << packet->address;
		  cout << " full_addr: " << packet->full_addr << dec << endl; });

      return index;
      //  }
    }
  }

  // if(instruction_and_data_collision) // remove instruction-and-data collision safeguard
  //   {
  // return -2;
  //   }

  DP(if (warmup_complete[packet->cpu]) {
    cout << "[" << NAME << "_MSHR] " << __func__ << " new address: " << hex << packet->address;
    cout << " full_addr: " << packet->full_addr << dec << endl; });

  DP(if (warmup_complete[packet->cpu] && (MSHR.occupancy == MSHR_SIZE)) { 
    cout << "[" << NAME << "_MSHR] " << __func__ << " mshr is full";
    cout << " instr_id: " << packet->instr_id << " mshr occupancy: " << MSHR.occupancy;
    cout << " address: " << hex << packet->address;
    cout << " full_addr: " << packet->full_addr << dec;
    cout << " cycle: " << current_core_cycle[packet->cpu] << endl; });

  return -1;
}

void CACHE::add_mshr(PACKET *packet)
{
  uint32_t index = 0;

  packet->cycle_enqueued = current_core_cycle[packet->cpu];

  // search mshr
  for (index = 0; index < MSHR_SIZE; index++)
  {
    if (MSHR.entry[index].address == 0)
    {

      MSHR.entry[index] = *packet;
      MSHR.entry[index].returned = INFLIGHT;
      MSHR.occupancy++;

      DP(if (warmup_complete[packet->cpu]) {
            cout << "[" << NAME << "_MSHR] " << __func__ << " instr_id: " << packet->instr_id;
            cout << " address: " << hex << packet->address << " full_addr: " << packet->full_addr << dec;
            cout << " index: " << index << " occupancy: " << MSHR.occupancy << endl; });

      break;
    }
  }
}

uint32_t CACHE::get_occupancy(uint8_t queue_type, uint64_t address)
{
  if (queue_type == 0)
    return MSHR.occupancy;
  else if (queue_type == 1)
    return RQ.occupancy;
  else if (queue_type == 2)
    return WQ.occupancy;
  else if (queue_type == 3)
    return PQ.occupancy;

  return 0;
}

uint32_t CACHE::get_size(uint8_t queue_type, uint64_t address)
{
  if (queue_type == 0)
    return MSHR.SIZE;
  else if (queue_type == 1)
    return RQ.SIZE;
  else if (queue_type == 2)
    return WQ.SIZE;
  else if (queue_type == 3)
    return PQ.SIZE;

  return 0;
}

void CACHE::increment_WQ_FULL(uint64_t address)
{
  WQ.FULL++;
}

float CACHE::get_mu_value(uint32_t core, uint32_t a, uint32_t b)
{
  // Function calculate the marginal utility value for the core given as arguement
  vector<vector<int>> arr(NUM_CPUS);
  for (uint32_t i = 0; i < NUM_CPUS; i++)
  {
    // We create a prefix array that will be used to calculate the difference in misses between different allocations
    arr[i].push_back(hit_counts[i][0]);
    for (uint32_t j = 1; j < NUM_WAY; j++)
    {
      arr[i].push_back(arr[i][j - 1] + hit_counts[i][j]);
    }
  }
  // U is the difference in misses when the core is allocated different number of ways
  int U = arr[core][b - 1] - arr[core][a - 1];
  // Marginal utility = (missa - missb) / (b-a)
  return (float)U / (float)(b - a);
}

pair<float, uint32_t> CACHE::get_max_mu(uint32_t core, uint32_t alloc, uint32_t balance)
{
  float max_mu = 0;
  uint32_t min_way = 0;
  // We find the maximum utility by giving it one additional way at a time
  // This also allows us to find the minimum number of ways it needs to achieve this utility value
  for (uint32_t way = 1; way <= balance; way++)
  {
    float mu = get_mu_value(core, alloc, alloc + way);
    if (mu > max_mu)
    {
      max_mu = mu;
      min_way = way;
    }
  }
  return {max_mu, min_way};
}

vector<uint32_t> CACHE::partition_algorithm()
{
  // We allocate atleast one way to each CPU
  int balance = NUM_WAY - NUM_CPUS;
  vector<uint32_t> allocations(NUM_CPUS, 1);
  vector<pair<float, uint32_t>> present_state(NUM_CPUS, {0, 0});
  while (balance != 0)
  {
    // We find the maximum marginal utility when we have 'balance' number of ways available for distribution
    for (uint32_t application = 0; application < NUM_CPUS; application++)
    {
      uint32_t alloc = allocations[application];
      present_state[application] = get_max_mu(application, alloc, balance);
    }
    uint32_t winner = 0;
    float max_mu = 0;
    // We check which core has the highest marginal utility value
    // We also find the number of ways it requires to achieve this marginal utility
    for (uint32_t application = 0; application < NUM_CPUS; application++)
    {
      if (present_state[application].first > max_mu)
      {
        winner = application;
        max_mu = present_state[application].first;
      }
    }
    allocations[winner] += present_state[winner].second;
    balance -= present_state[winner].second;
    // If no core requires additional ways to improve its performance, we break
    if (present_state[winner].second == 0)
      break;
  }
  uint32_t allocate = balance / NUM_CPUS;
  uint32_t left = balance % NUM_CPUS;
  // If we have ways left for distribution, we allocate them equally to all the cores
  for (uint32_t i = 0; i < NUM_CPUS; i++)
  {
    allocations[i] += allocate;
    if (i == 0)
      allocations[i] += left;
  }
  // If the partitioning algorithm is called, we divide the number of counters by 2
  for (uint32_t i = 0; i < NUM_CPUS; i++)
  {
    for (uint32_t j = 0; j < NUM_WAY; j++)
    {
      hit_counts[i][j] /= 2;
    }
  }
  return allocations;
}