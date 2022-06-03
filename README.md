<p align="center">
  <h1 align="center">Utility Based Cache Partitioning in ChampSim </h1>
  <p> ChampSim is a trace-based simulator for a microarchitecture study. You can sign up to the public mailing list by sending an empty mail to champsim+subscribe@googlegroups.com. Traces for the 3rd Data Prefetching Championship (DPC-3) can be found from here (https://dpc3.compas.cs.stonybrook.edu/?SW_IS). A set of traces used for the 2nd Cache Replacement Championship (CRC-2) can be found from this link. (http://bit.ly/2t2nkUj) <p>
</p>

Changes have been made in the following files:



1. base_replacement.cc
2. llc_replacement.cc
3. cache.cc
4. cache.h

# ```base_replacement.cc```

**Functions added:**



1. **atd_lru_update()**

    Arguments passed: uint32_t set, uint32_t way, uint32_t cpu


    This function updates the LRU values of the blocks in the ATD.

2. **atd_lru_victim()**

    Arguments passed: uint32_t cpu, uint32_t set


    Returns: uint32_t way


    This function finds the victim to be evicted from the ATD for a certain core and returns the way which contains the block to be evicted.

3. **lru_cpu_victim()**

    Arguments passed: uint32_t cpu, uint64_t instr_id, uint32_t set, const BLOCK *current_set, uint64_t ip, uint64_t full_addr, uint32_t type


    Returns: uint32_t way


    This function finds the victim in the partitioned LLC and returns the way which contains the block to be evicted.

4. **lru_cpu_update()**

    Arguments passed: uint32_t set, uint32_t way, uint32_t cpu


    This function updates the LRU values of the blocks in the partition of a particular core in the LLC.


# ```llc_replacement.cc```

**Functions changed:**



1. **llc_find_victim()**

    lru_cpu_victim()  is called here instead of lru_victim() to find the victim block. The argument ‘cpu’ is passed to find the victim from that particular core’s partition.

2. **llc_update_replacement_state()**

    lru_cpu_update() is called here instead of lru_update() to update the LRU values of the blocks in a particular core’s partition.


# ```cache.cc```

**Functions changed:**



1. **operate()**

    This function was changed to call the partitioning algorithm every 5 million clock cycles. In this, we are receiving the new allocations from the partition algorithm. Based on the difference between new allocations and present allocations, ways are reallocated.

2. **handle_writeback()**

    We added functionality to check for hits in the ATD whenever hits are checked for in the LLC and update the LRU values and fill the ATD in case of misses.

3. **handle_read()**

    We added functionality to check for hits in the ATD whenever hits are checked for in the LLC and update the LRU values and fill the ATD in case of misses.


**Functions added:**



1. **fill_atd()**
2. **check_hit_atd()**
3. **get_mu_value()**

    Arguments passed: uint32_t core, uint32_t a, uint32_t b


    Returns:  mu_value


    This function calculates and returns the marginal utility of a particular core if the partition increases from a to b ways.

4. **get_max_mu()**

    Arguments passed: uint32_t core, uint32_t alloc, uint32_t balance


    Returns: pair&lt;float, uint32_t> {max_mu, min_way}


    This function calculates and returns the maximum marginal utility along with the minimum number of ways to be added to achieve the same.

5. **partition_algorithm()**

    Arguments passed: None


    Returns: vector&lt;uint32_t> allocations


    This function uses the look-ahead algorithm mentioned in the UCP paper. It calls functions (3) and (4) to calculate the optimal number of ways to be allocated to each application (core).


# ```cache.h```

  The Constructor of the Cache is changed to accommodate the changes of UCP. 
  ATD is initialized for each core. Initially partition gives equal number of ways to each 
  applications. Only 32 sets are sampled under Dynamic set sampling. Hit counters and LRU values are also initialized accordingly.

# ```Results```
  
   **Core-1 :** bzip2_281B.trace.xz  
   **Core-2 :** lbm_1004B.trace.xz  
   
   ![Figure](https://user-images.githubusercontent.com/75127394/168048759-27ebf95a-ed31-4c08-8568-57408048e6ce.png)  
   **Core 1:**
   * **Cumulative IPC:** 1.00332 instructions: 192667751 cycles: 192029585
   * LLC TOTAL     ACCESS:    5866175  HIT:    2494774  MISS:    3371401
 
   **Core 2:**
   * **Cumulative IPC:** 0.520753 instructions: 100000002 cycles: 192029586  
   * LLC TOTAL     ACCESS:    1085561  HIT:     837840  MISS:     247721

More results (for 2 core, 4 core and 8 core) can be seen at: https://docs.google.com/spreadsheets/d/1pXiMeEUcKjsCnb-nVGWeX-XJbTiYhffnHd_GaR1g5j4/edit#gid=2084777539
