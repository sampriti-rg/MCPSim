#ifndef __PROCESSOR_H
#define __PROCESSOR_H

#include "Cache.h"
#include "Config.h"
#include "Memory.h"
#include "Request.h"
#include "HMC.h"
#include "Controller.h"
#include "3DMemory.h"
#include "Statistics.h"
#include <iostream>
#include <vector>
#include <fstream>
#include <string>
#include <ctype.h>
#include <functional>
#include <queue>
#include <deque>
#include <stack>
#include <unordered_map>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <map>
#include <cmath>

#include "../Trace_Extractor/trace_format.h"

#include <nlohmann/json.hpp>
using json = nlohmann::json;

namespace ramulator 
{
struct BasicBlockInfo {
    int BasicBlockID;
    string BasicBlockName;
    int ArithmeticInstructions;
    int MemoryInstructions;
    int NonMemoryInstructions;
    int TotalInstructions;
    int TotalMemoryConsumption;
};

struct SqueduleQueue{
    deque<trace_format> trace_queue;
    int numberInstructionsInQueue = 0;

    bool is_empty(){
        if (numberInstructionsInQueue == 0 )
            return true;
        return false;
    }

    void pop_front(){
        if(!is_empty()){
            trace_queue.pop_front();
            numberInstructionsInQueue--;
        }
    }
};

class Trace {
public:
    Trace() {}
    Trace(const string& trace_fname);
    bool init_trace(const string& trace_fname, const Config &configs);
    bool get_trace_line(trace_format& trace_line);
    long expected_limit_insts = 0;
    Config configs;
    
private:
    FILE* file;
    std::string trace_name;
    std::vector<int> instructions;
};

class Window {
public:
    int ipc = 4;
    int depth = 128;
    int load = 0;
    int head = 0;
    int tail = 0;
    std::vector<bool> ready_list;
    std::vector<long> addr_list;

    Window() : ready_list(depth), addr_list(depth, -1) {}
    bool is_full();
    bool is_empty();
    void insert(bool ready, long addr);
    long retire();
    void set_ready(long addr, int mask);
    void reset_window();
};

class Processor;

/* Config values that the tick loop needs. They are parsed once at construction,
   because Config getters do a std::map<string,string> lookup and return a string copy. */
enum class CoreModel { InOrder, OutOrder, Unknown };
enum class SimMode { HostOnly, AllOffload, CoSimulation, MCPOnly, Unknown };
enum class MemStandard { HMC, HBM, HBM2, Unknown };

SimMode parse_sim_mode(const std::string& mode);

class Core {
public:
    Core(const Config& configs, int coreid,
        function<bool(Request)> send_next, Cache* llc,
        std::shared_ptr<CacheSystem> cachesys, MemoryBase& memory, bool is_nmp, bool nlp_side);     // constructor of cores.

    bool is_nmp;                            // specify the core assosiate to NMP side or not.
    bool pending_trace = false;             // if the offloaded side core's queue get full, this flag will set.
    long clk = 0;                           // record the cycle count.
    long retired = 0;                       // record the how many instruction get retired.
    int id = 0;                             // contain the unique ID for identify the core.
    unsigned inFlightMemoryAccess = 0;      // if core issued a instruction fatching request it will increase the counter.
    string cpu_type;                        // specify the cpy type in-order or OoO.
    bool lock_core = true;                  // if it set the core can not process instruction but did some other operation (e.g. inst bypass).
    bool no_core_caches = true;             // specify the core has private cache or not (L1/L2).
    bool no_shared_cache = true;            // specify that LLC exist or not.

    int l1_size = 1 << 15;                  // specification of private caches.
    int l1_assoc = 1 << 3;
    int l1_blocksz = 1 << 6;
    int l1_mshr_num = 16;

    int l2_size = 1 << 18;
    int l2_assoc = 1 << 3;
    int l2_blocksz = 1 << 6;
    int l2_mshr_num = 16;

    long bubble_cnt;                        // from trace line this information extracted for execution phase.
    long req_addr = -1;
    Request::Type req_type;
    long region_id = 0;

    bool more_reqs =false;                  // specify that the core has pending instruction to process.
    long last = 0;                          // used in callback cycle calculation.
    long expected_limit_insts;              // contain the limit of simulation.
    bool reached_limit = false;             // this set true iff expected number of instructions has been executed or all instructions are executed.
    bool inside_region = false;             // indicate that control inside the offloadbale region.
    bool is_warmup_done = true;             // indicate that the warmup stage.
    bool pending_inst_bypass = false;       // used to specify that still there instruction pending to bypass on NMP side.
    bool wait_for_nmp_finish = false;       // used to notify the CPU cores to wait from NMP side execution finish.
    int decision_overhead_cycles = 0;       // carry the decisio-making overhead cycle count.
    float active_core_energy;               // contain core's active cycle energy consumption.
    float idle_core_energy;                 // contain core's idle cycle energy consumption.
    float memory_energy = 0.024258;         // in nj/bit [DRAM+TSV+Logic+Link]
    int l_index, s_index;                   // used in loop.
    int deployed_app_id, current_thread_id; // contain the process and thread ID w.r.t. core.
    bool trace_assigned = false;            // specify that the core contain the master thread.
    int own_vault_target_addr = -1;         // initially core are not assign to NMP side.
    bool proc_switching_flag = false;       // used in context switching to specify the CPU side can switch to NMP side.
    long nlp_core_id_gen = 0;               // used to generate core ID for NLP side.
    bool nlp_side = false;                  // used to specify the core belong to NLP side.
    bool loads_exe_flag, stores_exe_flag;   // these are simple excution tracking flags.
    double prev_ips;

    const std::unordered_map<std::string, int>& opcodeCycleCount;  // shared, read-only opcode -> cycle table (loaded once).
    set<long> offload_region_ids;                           // track the offloading region IDs.
    std::shared_ptr<CacheSystem> cachesys;                  // cache system pointer.
    trace_format trace_line;                                // storing one instruction info which fetched from trace file.
    SqueduleQueue inst_queue;                               // store the offloaded instruction.
    json bb_info_data;                                      // contain compiler-extracted info.
    function<bool(Request)> send;                           // by this function memory request will traverse from core to memory.
    function<void(Request&)> callback;                      // by this function each module (from memory to core) get the response of request.
    std::vector<std::shared_ptr<Cache>> caches;             // pointer of private caches.
    Cache* llc;                                             // pointer of LLC.
    
    Window window;                              // window of OoO
    Cache* first_level_cache = nullptr;
    MemoryBase& memory;                         // pointer of memory module.
    Trace trace_per_core;
    Config configs;                             // contain all the configuration details
    Processor* nmp_proc;                        // pointer of NMP side.
    Processor* nlp_proc;                        // pointer of NLP side.
    Processor* own_proc;                        // pointer of own processing side.

    CoreModel core_model = CoreModel::Unknown;  // cached from cpu_type.
    SimMode sim_mode = SimMode::Unknown;        // cached from configs.get_simulation_mode().
    bool nlp_on = false;                        // cached from configs.get_nlp_facility() == "on".
    bool inst_fetch_on = false;                 // cached from configs.inst_fetching() == "on".
    int overhead_cycle_cfg = 0;                 // cached from configs.get_overhead_cycle().
    int nmp_queue_max = 0;                      // cached from configs.get_nmp_core_queue_max_size().
    int nlp_core_count = 1;                     // cached from configs.get_nlp_core_num().
    MemStandard mem_standard = MemStandard::Unknown;
    Memory<HMC, Controller>* mem_hmc = nullptr; // typed memory pointers, resolved once instead of a
    Memory<HBM, Controller>* mem_hbm = nullptr; // dynamic_cast on every address lookup.
    Memory<HBM2, Controller>* mem_hbm2 = nullptr;
    std::string loaded_bb_path;                 // JSON already parsed into bb_info_data.
    std::unordered_map<long, json*> bb_index;   // BasicBlockID -> block, built once per JSON load.
    bool bb_index_complete = false;             // false if some IDs could not be indexed (fallback to scan).
    void build_bb_index();
    bool core_idle_fast();                      // true when an out-of-order tick would do nothing but count idle.

    // An idle out-of-order core is put to sleep instead of being ticked every cycle. While asleep,
    // each processor tick it misses would only have done clk++ and idle_cycles++, so those are
    // added in one step (sync_idle) before anything reads them or the core gets work again (wake).
    bool asleep = false;
    long sleep_from = 0;                        // last processor tick already counted in clk / idle_cycles.
    void sync_idle();
    void wake();

    ScalarStat record_cycs;                     // stats containers.
    ScalarStat record_insts;
    ScalarStat memory_access_cycles;
    ScalarStat cpu_inst;
    ScalarStat idle_cycles;
    ScalarStat memory_inst;
    ScalarStat record_region_count;
    ScalarStat record_offload_region_count;
    ScalarStat overhead_cycles;

    void tick();                                // function defination will be specified in cpp file.
    void receive(Request& req);
    double calc_ipc();
    bool finished();
    bool has_reached_limit();
    void tick_inOrder();
    void tick_outOrder();
    void get_first_instruction();
    void load_trace(string trace_base_name, const Config& configs);
    std::vector<float> collect_basicblock_info(int blockID);
    static const std::unordered_map<std::string, int>& initialize_arch_cycle_db();
    void compiler_assist_offload();
    void compiler_assist_setup(int procId);
    bool check_for_dirty(long addr);
    int get_channel_or_vault_target(long mem_addr);
    void lock_own_cores(long app_id, bool flag);
    bool get_next_instruction();
    void execution_flag_set();
    void memory_allocates();
    void offload_stratigy();
    void host_only();
    void nmp_only();
    void all_offload();
    void reset_stats();
    long get_executed_insts();
    void instruction_bypass();
};

class Processor {
public:
    Processor(const Config& configs, vector<string> trace_list,         // constructor of processor.
        function<bool(Request)> send, MemoryBase& memory, bool is_nmp);
    Processor(const Config& configs, vector<string> trace_list,
        function<bool(Request)> send, MemoryBase& memory, bool is_nmp,
        Cache* llc, std::shared_ptr<CacheSystem> cachesys);
    
    int initial_core_id;            // used to specify the starting ID of containing cores.
    int number_cores;               // specify how many cores the processor has.
    double ipc = 0;                 // instruction-per-sec.
    long total_retired = 0;         // store the total no of retired instructions.
    long total_instructions = 0;    // store total no of executed instruction.
    double cycle_time;              // processor cycle time.
    bool early_exit;                // when early_exit is true, the simulation exits when the earliest trace finishes.
    bool is_nmp;                    // specify is this NMP side or processor side computing unit.
    bool nlp_side = false;          // specify is this NLP side computing units.
    bool no_core_caches = true;     // specify the processor has private cache facility.
    bool no_shared_cache = true;    // specify the processor has LLC facility.

    int l3_size = 1 << 25;          // specification of LLC.
    int l3_assoc = 1 << 5;
    int l3_blocksz = 1 << 6;
    int mshr_per_bank = 16;

    std::shared_ptr<CacheSystem> cachesys;
    std::vector<std::unique_ptr<Core>> cores;       // contain all the pointer of corrosponding cores.
    std::vector<double> ipcs;                       // carry individual IPC of corrospondingg cores.

    Cache llc;              // LLC.
    Cache* llc_pointer;     // pointer of LLC (used in NLP side).
    Config configs;         // all the configuration.
    Trace trace;            // contain the trace file.
    MemoryBase& memory;     // pointer of memory.
    Processor* nmp_proc;    // pointer of memory side NMP.
    Processor* nlp_proc;    // pointer of LLC side NLP.

    bool debug_ctx_switch = false;      // cached from configs.debug_context_swithing() == "on".
    bool nlp_off = false;               // cached from configs.get_nlp_facility() == "off".
    long executed_insts = 0;            // running sum of cores[i]->cpu_inst, kept in step with it.
    long proc_ticks = 0;                // number of tick() calls so far.
    int ticking_index = -1;             // index of the core being ticked right now, -1 outside tick().
    void sync_all_idle();               // bring every sleeping core's clk / idle_cycles up to date.

    ScalarStat cpu_cycles;              // all the stats.
    ScalarStat total_idle_cycles;
    ScalarStat average_idle_cycles;
    ScalarStat total_cache_misses;
    ScalarStat total_cache_hits;
    ScalarStat general_ipc;
    ScalarStat total_cpu_instructions;
    ScalarStat total_time;
    ScalarStat total_overhead_cycles;
    ScalarStat total_energy_consumption;
    
    void tick();                    // function defination will be specified in cpp file.
    void receive(Request& req);
    bool finished();
    void calc_stats();
    bool has_reached_limit();
    void init_nmp_side();
    void init_nlp_side();
    void lock_all_cores(bool flag);
    void reset_stats();
    long get_executed_insts();
    void warmedup_activate();
    float calculate_total_instruction();
    float calculate_Energy();
    bool is_complete();
    bool can_context_switch(long processID);
    bool can_nmp_switch();
    std::vector<float> collect_system_info();  
    void flush_all_caches();
};

}
#endif /* __PROCESSOR_H */
