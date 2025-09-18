#include "Processor.h"
#include <stdexcept>

#include <cassert>

using namespace std;
using namespace ramulator;

/* defination of processor interface (CPU and MCP) */
Processor::Processor(const Config &configs,
                     vector<string> trace_list,
                     function<bool(Request)> send_memory,
                     MemoryBase &memory,
                     bool is_nmp)
    : configs(configs),
      is_nmp(is_nmp),    /* is_nmp = [true : MCP side], [false : CPU side] */
      early_exit(configs.is_early_exit()),
      no_core_caches(!configs.has_core_caches()),
      no_shared_cache(!(configs.has_l3_cache() && !is_nmp)),
      cachesys(new CacheSystem(configs, send_memory, is_nmp)),    /* creating overall cache system */
      llc(l3_size, l3_assoc, l3_blocksz, mshr_per_bank * trace_list.size(),   /* creating LLC */
          Cache::Level::L3, cachesys, is_nmp),
      memory(memory)
{
    assert(cachesys != nullptr);
    if (!is_nmp)  /* CPU core id: [0 to number_cores], MCP PU id [number_cores+1 to number_nmp_cores] */
    {
        initial_core_id = 0;
        number_cores = configs.get_core_num();
        cycle_time = configs.get_cpu_tick() / 1000.0;
    }
    else
    {
        initial_core_id = configs.get_core_num();
        number_cores = configs.get_nmp_core_num();
        cycle_time = configs.get_nmp_tick() / 1000.0;
    }

    /* creating cores with or without LLC */
    if (no_shared_cache)
    {
        for (int i = 0; i < number_cores; ++i)
        {
            cores.emplace_back(new Core(configs, i + initial_core_id, send_memory, nullptr, cachesys, memory, is_nmp, false));
        }
    }
    else
    {
        for (int i = 0; i < number_cores; i++)
        {
            cores.emplace_back(new Core(configs, i + initial_core_id, std::bind(&Cache::send, &llc, std::placeholders::_1),
                                        &llc, cachesys, memory, is_nmp, false));
        }
    }

    /* if CPU side the load the trace, otherwise only assign the perent processor interface */
    if (!is_nmp)
    {
        for (int i = 0; i < number_cores; ++i)
        {
            cores[i]->load_trace(trace_list[0], cores[i]->configs);
            cores[i]->get_first_instruction();
            cores[i]->own_proc = this;
            cores[i]->expected_limit_insts = configs.get_expected_limit_insts();
        }
    }
    else
    {
        for (int i = 0; i < number_cores; ++i)
            cores[i]->own_proc = this;
    }

    /* bind cores to memory by making receive function as callback */
    ipcs.resize(number_cores);
    for (int i = 0; i < number_cores; ++i)
    {
        cores[i]->callback = std::bind(&Processor::receive, this, placeholders::_1);
        ipcs[i] = -1;
    }

    /* setup stats metrices for processing side */
    if (!is_nmp)
    {
        cpu_cycles.name("cpu_cycles")
            .desc("cpu cycle number")
            .precision(0);
        general_ipc.name("ipc")
            .desc("final ipc number")
            .precision(6);
        total_time.name("total_time")
            .desc("Total Time (ns)")
            .precision(6);
        total_cpu_instructions.name("cpu_instructions")
            .desc("total cpu instructions number")
            .precision(0);
        total_idle_cycles.name("total_idle_cycles")
            .desc("Total idle cycles due to full window.")
            .precision(0);
        average_idle_cycles.name("average_idle_cycles")
            .desc("Average idle cycles due to full window")
            .precision(0);
        total_overhead_cycles.name("total_overhead_cycle")
            .desc("total overhead cycle")
            .precision(0);
        total_energy_consumption.name("total_energy_consumption")
            .desc("Total energy consumption")
            .precision(0);
    }
    else
    {
        cpu_cycles.name("nmp_cpu_cycles")
            .desc("NMP side cpu cycle number")
            .precision(0);
        general_ipc.name("nmp_ipc")
            .desc("NMP side final ipc number")
            .precision(6);
        total_time.name("nmp_total_time")
            .desc("NMP side total Time (ns)")
            .precision(6);
        total_cpu_instructions.name("nmp_cpu_instructions")
            .desc("NMP side total cpu instructions number")
            .precision(0);
        total_idle_cycles.name("nmp_total_idle_cycles")
            .desc("NMP side total idle cycles due to full window.")
            .precision(0);
        average_idle_cycles.name("nmp_average_idle_cycles")
            .desc("NMP side average idle cycles due to full window")
            .precision(0);
        total_energy_consumption.name("nmp_side_energy_consumption")
            .desc("NMP side total energy consumption")
            .precision(0);
    }

    /* set all metrics to zero initially */
    general_ipc = 0.0;
    total_cpu_instructions = 0;
    cpu_cycles = 0;
    average_idle_cycles = 0;
    total_idle_cycles = 0;
    total_overhead_cycles = 0;
    total_energy_consumption = 0;
}

/* for NLP side processor interface only */
Processor::Processor(const Config &configs,
                     vector<string> trace_list,
                     function<bool(Request)> send_memory,
                     MemoryBase &memory,
                     bool is_nmp,
                     Cache* llc_p,   /* pointer of LLC (CPU side) used here, thats why different interface */
                     std::shared_ptr<CacheSystem> cachesys_p)
    : configs(configs),
      is_nmp(is_nmp),
      early_exit(configs.is_early_exit()),
      no_core_caches(!configs.has_core_caches()),
      no_shared_cache(!configs.has_l3_cache()),
      memory(memory),
      cachesys(cachesys_p),
      llc(l3_size, l3_assoc, l3_blocksz, mshr_per_bank * trace_list.size(),   /* created LLC but not used further */
          Cache::Level::L3, cachesys, is_nmp),
      llc_pointer(llc_p)       /* assigned LLC (CPU side) to llc_pointer */
{
    assert(cachesys != nullptr);
    initial_core_id = configs.get_core_num() + configs.get_nmp_core_num();   /* NLP core id start after number_cores+number_nmp_cores to number of banks */
    number_cores = configs.get_nlp_core_num();
    cycle_time = configs.get_nmp_tick() / 1000.0;
    nlp_side = true;   /* used to specify that is nlp processor side */
    
    /* creating NLP cores */
    for (int i = 0; i < number_cores; i++)
    {
        cores.emplace_back(new Core(configs, i + initial_core_id, std::bind(&Cache::send, llc_pointer, std::placeholders::_1),
                                    llc_pointer, cachesys, memory, is_nmp, nlp_side));
    }

    /* assign the perent processor interface */
    for (int i = 0; i < number_cores; ++i)
        cores[i]->own_proc = this;

    /* bind cores to memory by making receive function as callback */
    ipcs.resize(number_cores);
    for (int i = 0; i < number_cores; ++i)
    {
        cores[i]->callback = std::bind(&Processor::receive, this, placeholders::_1);
        ipcs[i] = -1;
    }

    /* setup stats metrices for processing side */
    cpu_cycles.name("nlp_cpu_cycles")
        .desc("NLP side cpu cycle number")
        .precision(0);
    general_ipc.name("nlp_ipc")
        .desc("NLP side final ipc number")
        .precision(6);
    total_time.name("nlp_total_time")
        .desc("NLP side total Time (ns)")
        .precision(6);
    total_cpu_instructions.name("nlp_cpu_instructions")
        .desc("NLP side total cpu instructions number")
        .precision(0);
    total_idle_cycles.name("nlp_total_idle_cycles")
        .desc("NLP side total idle cycles due to full window.")
        .precision(0);
    average_idle_cycles.name("nlp_average_idle_cycles")
        .desc("NLP side average idle cycles due to full window")
        .precision(0);
    total_energy_consumption.name("nlp_side_energy_consumption")
        .desc("NLP side total energy consumption")
        .precision(0);

    /* set all metrics to zero initially */
    general_ipc = 0.0;
    total_cpu_instructions = 0;
    cpu_cycles = 0;
    average_idle_cycles = 0;
    total_idle_cycles = 0;
    total_overhead_cycles = 0;
    total_energy_consumption = 0;
}

/* processor tick as clock pulse */
void Processor::tick()
{
    cpu_cycles++;

    if (!is_nmp)
    {
        if ((int(cpu_cycles.value()) % 1000000) == 0) {
            printf("CPU heartbeat, cycles: %d \n", (int(cpu_cycles.value())));
            printf("CPU executed instructions: %d\n", int(calculate_total_instruction()));
            printf("NMP executed instructions: %d\n", int(nmp_proc->calculate_total_instruction()));
            printf("NLP executed instructions: %d\n", int(nlp_proc->calculate_total_instruction()));
        }
    }

    if (!(no_core_caches))
    {
        cachesys->tick();
    }

    for (unsigned int i = 0; i < cores.size(); i++)
    {
        Core *core = cores[i].get();
        core->tick();
    }
}

/* when the processor recv a call back from memory */
void Processor::receive(Request &req)
{
    if (!no_shared_cache)  /* first llc will take the recv*/
    {
        llc.callback(req);
    }
    else if (!cores[0]->no_core_caches)
    {
        for (unsigned int i = 0; i < cores.size(); ++i)  /* then private caches will recv*/
        {
            Core *core = cores[i].get();
            core->caches[0]->callback(req);
        }
    }

    /* for the nlp side has not l2 therefore llc does not have valid higer cache 
       (during recv in cache interface) thus we need recv for l1 seperatly */
    if (nlp_side)
    {
        for (unsigned int i = 0; i < cores.size(); ++i)  
        {
            Core *core = cores[i].get();
            core->caches[0]->callback(req);
        }
    }

    Core *core = cores[req.coreid - initial_core_id].get();   /* finally core will recv */
    core->receive(req);
}

/* calculate basic stats to show in terminal */
void Processor::calc_stats()
{
    long long num_region_cnt = 0, offload_region_cnt = 0;

    for (unsigned int i = 0; i < cores.size(); ++i)
    {
        if(is_nmp)
        {
            cores[i]->record_cycs = cores[i]->clk;
            cores[i]->record_insts = long(cores[i]->cpu_inst.value());
            memory.record_core(cores[i]->id);
        }

        if (ipcs[i] < 0)
        {
            ipcs[i] = cores[i]->calc_ipc();
            ipc += ipcs[i];
            total_retired += cores[i]->retired;
            total_instructions += cores[i]->cpu_inst.value();
            total_idle_cycles += cores[i]->idle_cycles.value();
            num_region_cnt += cores[i]->record_region_count.value();
            offload_region_cnt += cores[i]->record_offload_region_count.value();
            total_overhead_cycles += cores[i]->overhead_cycles.value();
        }
        else
        {
            return;
        }
    }

    ipc = total_instructions / cpu_cycles.value();
    average_idle_cycles = total_idle_cycles.value() / cores.size();
    total_energy_consumption = calculate_Energy();

    cout << endl;
    cout << "-> total region: " << num_region_cnt << endl;
    cout << "-> offloaded region: " << offload_region_cnt << endl;
    cout << "-> retired: " << total_retired << endl;
    cout << "-> cycles: " << cpu_cycles.value() << endl;
    cout << "-> overhead cycles: " << total_overhead_cycles.value() << endl;
    cout << "-> ipc: " << ipc << endl;
    cout << "-> total instructions: " << total_instructions << endl;

    general_ipc = ipc;
    total_cpu_instructions = total_instructions;
    total_time = total_instructions * (1 / ipc) * cycle_time;
    cout << "-> total time: " << total_time.value() << "ns" << endl;
    cout << endl;
}

/* used to check the processor side finish the trace or not */
bool Processor::finished()
{
    if (early_exit)
    {
        for (unsigned int i = 0; i < cores.size(); ++i)
        {
            if (cores[i]->finished())
            {
                for (unsigned int j = 0; j < cores.size(); ++j)
                {
                    ipc += cores[j]->calc_ipc();
                }
                return true;
            }
        }
        return false;
    }
    else
    {
        for (unsigned int i = 0; i < cores.size(); ++i)
        {
            if (!cores[i]->finished())
            {
                return false;
            }
        }
        return true;
    }
}

/* used to lock/unlock (true/false in argument) the core for not processing further trace line */
void Processor::lock_all_cores(bool flag)
{
    for (unsigned int i = 0; i < cores.size(); ++i)
        cores[i]->lock_core = flag;
}

/* check each core whether reaches limit or not */
bool Processor::has_reached_limit()
{
    for (unsigned int i = 0; i < cores.size(); ++i)
    {
        if (!cores[i]->has_reached_limit())
        {
            return false;
        }
    }
    return true;
}

/* initialize nmp processor object using pointer */
void Processor::init_nmp_side()
{
    for (unsigned int i = 0; i < cores.size(); i++)
    {
        Core *core = cores[i].get();
        core->nmp_proc = nmp_proc;
    }
}

/* initialize nlp processor object using pointer */
void Processor::init_nlp_side()
{
    /* set is_nmp flag false for sending req from outside of memory,
       however set nlp_side to used for designate the nmp */
    for (unsigned int i = 0; i < nlp_proc->cores.size(); i++)
    {
        nlp_proc->cores[i].get()->is_nmp = false;
        nlp_proc->cores[i].get()->caches[0].get()->is_nmp = false;
    }

    for (unsigned int i = 0; i < cores.size(); i++)
    {
        Core *core = cores[i].get();
        core->nlp_proc = nlp_proc;
    }
}

/* used to reset the basic stats (value of metrices, clk, ipc, etc.) */
void Processor::reset_stats()
{
    for (unsigned int i = 0; i < cores.size(); i++)
        cores[i]->reset_stats();

    ipc = 0;

    for (unsigned int i = 0; i < ipcs.size(); i++)
        ipcs[i] = -1;
}

/* retrive how many total instructions executed on the corrent processing side */
long Processor::get_executed_insts()
{
    long insts_total = 0;
    for (unsigned int i = 0; i < cores.size(); i++)
    {
        insts_total += cores[i]->get_executed_insts();
    }
    return insts_total;
}

/* set the warmup flag to understand system warmedup */
void Processor::warmedup_activate()
{
    for (unsigned int i = 0; i < cores.size(); i++)
    {
        cores[i]->is_warmup_done = true;
    }
}

/* return true if the processor side can switch to other side otherwise false (only between CPU and MCP) */
bool Processor::can_context_switch(long processID)
{   
    //check all the empty or not, such as, cores window, cache req lists, memory req lists.
    if (!is_nmp && !nlp_side)   // used for CPU side only.
    {
        for (unsigned int i = 0; i < cores.size(); ++i)
        {
            // only check the participating cores can switch or not.
            if(cores[i]->deployed_app_id == processID) {     
                if (!cores[i]->window.is_empty()) return false;
                if (configs.debug_context_swithing() == "on")    // if simulation encounter problem then retry, mshr list checking required.
                {
                    if (!cores[i]->caches[1]->retry_list.empty()) return false;
                    if (!cores[i]->caches[0]->retry_list.empty()) return false;
                    if (!cores[i]->llc->retry_list.empty()) return false;
                    if (!cores[i]->caches[1]->mshr_entries.empty()) return false;   //
                    if (!cores[i]->caches[0]->mshr_entries.empty()) return false;
                    if (!cores[i]->llc->mshr_entries.empty()) return false;
                }

                /* if NLP side not present then flush all the dirty data to memory before NMP execution */
                if (configs.get_nlp_facility() == "off") 
                {
                    // if (cores[i]->caches[1]->flush_dirty_lines(cores[i]->id)) return false;  // These are not correctly working.
                    // if (cores[i]->caches[0]->flush_dirty_lines(cores[i]->id)) return false;
                    // if (cores[i]->llc->flush_dirty_lines(cores[i]->id)) return false;
                    flush_all_caches();
                }

                if (configs.debug_context_swithing() == "on") if (!cachesys->is_wait_list_empty(cores[i]->id)) return false;
            }
        }
        if (configs.debug_context_swithing() == "on") 
        {
            if (memory.pending_link_packets() > 0) return false;
            if (memory.pending_requests() > 0) return false;
        }
    }
    else  // used for NMP side only.
    {
        for (unsigned int i = 0; i < cores.size(); ++i)
        {
            if (!cores[i]->finished()) return false; 
            if (configs.debug_context_swithing() == "on")
            {
                if (cores[i]->more_reqs) return false;
                if (!cores[i]->caches[0]->retry_list.empty()) return false;
                if (cores[i]->llc != nullptr ) if (!cores[i]->llc->retry_list.empty()) return false;
                if (!cores[i]->caches[0]->mshr_entries.empty()) return false;
                if (cores[i]->llc != nullptr )  if (!cores[i]->llc->mshr_entries.empty()) return false;
                if (!cachesys->is_wait_list_empty(cores[i]->id)) return false;
            }
        }
        if (configs.debug_context_swithing() == "on") 
        {
            if (memory.pending_link_packets() > 0) return false;
            if (memory.pending_requests() > 0) return false;
        }

        memory.restore_hmc_tags();  // to resolve the co-simulation problem.
    }   
    return true;
}

/* flush the dirty chahe lines */
void Processor::flush_all_caches() 
{
    for (auto &core : cores) {
        for (auto &cache : core->caches) {
            if (cache) {
                cache->flush_all_dirty_lines();
            }
        }
    }
    llc.flush_all_dirty_lines();
}


/* return true if the one MCP side (NMP/NLP) can switch to other NMP side otherwise false (only between MCPs) */
bool Processor::can_nmp_switch()
{
    if (nlp_side)  // NLP side.
    {
        for (unsigned int i = 0; i < cores.size(); ++i)
        {
            if (!cores[i]->finished()) return false; 
            if (configs.debug_context_swithing() == "on")
            {
                if (cores[i]->more_reqs) return false;
                if (!cores[i]->caches[0]->retry_list.empty()) return false;
                if (cores[i]->llc != nullptr ) if (!cores[i]->llc->retry_list.empty()) return false;
                if (!cores[i]->caches[0]->mshr_entries.empty()) return false;
                if (cores[i]->llc != nullptr ) if (!cores[i]->llc->mshr_entries.empty()) return false;
                if (!cachesys->is_wait_list_empty(cores[i]->id)) return false;
            }
        }
        if (configs.debug_context_swithing() == "on") 
        {
            if (memory.pending_link_packets() > 0) return false;
            if (memory.pending_requests() > 0) return false;
        }
    } 
    else   // NMP side.
    {
        for (unsigned int i = 0; i < cores.size(); ++i)
        {
            if (configs.debug_context_swithing() == "on")
            {
                if (!cores[i]->caches[0]->retry_list.empty()) return false;
                if (cores[i]->llc != nullptr ) if (!cores[i]->llc->retry_list.empty()) return false;
                if (!cachesys->is_wait_list_empty(cores[i]->id)) return false;
            }
        }
        if (configs.debug_context_swithing() == "on") 
        {
            if (memory.pending_link_packets() > 0) return false;
            if (memory.pending_requests() > 0) return false;
        }
        // memory.restore_hmc_tags();    // enable if encounter problem.
    }
    return true;
}

/* check the processing side complete the execution in overall (from core to memory), it almost same as context switching function */
bool Processor::is_complete()
{
    if (!is_nmp && !nlp_side)
    {
        for (unsigned int i = 0; i < cores.size(); ++i)
        {
            if (!cores[i]->window.is_empty()) return false;
            for (unsigned int j = 0; j < cores[i]->caches.size(); ++j)
                if (!cores[i]->caches[j]->retry_list.empty()) return false;
        }
    }
    else
        for (unsigned int i = 0; i < cores.size(); ++i)
            if (cores[i]->more_reqs) return false;

    if (!cachesys->wait_list.empty()) return false;
    if (memory.pending_link_packets() > 0) return false;
    if (memory.pending_requests() > 0) return false;    

    return true;
}

/* calculate total executed instruction on the processing side (for all cores) */
float Processor::calculate_total_instruction()
{
    long totalInst = 0;
    for (unsigned int i = 0; i < cores.size(); ++i)
    {
        totalInst += cores[i]->cpu_inst.value();
    }
    return totalInst;
}

/* calculate total energy on the processing side (from core to memory) */
float Processor::calculate_Energy()
{
    // for core active and idle cycles are considered.
    // for cache uses number of access.
    // for mmeory uses no of offchip data trasfer x block size
    long active_cycle = 0;
    long idle_cycle = 0;
    long l1_cache_access = 0, l2_cache_access = 0, llc_cache_access = 0, memory_access = 0;
    float energy;
    for (unsigned int i = 0; i < cores.size(); ++i)
    {
        active_cycle += (cores[i]->clk - cores[i]->idle_cycles.value());
        idle_cycle += cores[i]->idle_cycles.value();
        if (cores[i]->is_nmp || nlp_side)
        {
            l1_cache_access += cores[i]->caches[0]->cache_total_access.value();
            if (!nlp_side) memory_access += cores[i]->caches[0]->cache_load_blocks.value() + cores[i]->caches[0]->cache_write_back_hmc.value();
        }
        else
        {
            l1_cache_access += cores[i]->caches[1]->cache_total_access.value();
            l2_cache_access += cores[i]->caches[0]->cache_total_access.value();
        }
    }

    if (!no_shared_cache) 
    {
        llc_cache_access += llc.cache_total_access.value();
        memory_access =  llc.cache_load_blocks.value() + llc.cache_write_back_hmc.value() + llc.cache_write_back_lower.value();
    }

    if (!is_nmp) 
        energy = (active_cycle * cores[0]->active_core_energy) + 
                 (idle_cycle * cores[0]->idle_core_energy) +
                 (l1_cache_access * llc.energy_consuption[0]) + 
                 (l2_cache_access * llc.energy_consuption[1]) + 
                 (llc_cache_access * llc.energy_consuption[2]) +
                 (memory_access * 512 * cores[0]->memory_energy);
    else 
        energy = (active_cycle * cores[0]->active_core_energy) + 
                 (idle_cycle * cores[0]->idle_core_energy) +
                 (l1_cache_access * llc.energy_consuption[0]) +
                 (memory_access * 512 * cores[0]->memory_energy);

    return energy;
}

/* in compiler-assisted mode, when reuired to observe the system, this fuction will use, it return a list of system performaces metrices */
std::vector<float> Processor::collect_system_info()
{
    std::vector<float> state;
    float IPS, miss_rate, off_chip_trans, exe_time, energy, bandwidth;
    exe_time = cpu_cycles.value() * cycle_time;
    energy = calculate_Energy() + nmp_proc->calculate_Energy();
    miss_rate = llc.cache_total_miss.value() / llc.cache_total_access.value();
    off_chip_trans = llc.cache_load_blocks.value() + llc.cache_write_back_hmc.value() + llc.cache_write_back_lower.value();
    state.push_back(IPS);
    state.push_back(energy/exe_time);
    state.push_back(miss_rate);
    state.push_back(off_chip_trans/calculate_total_instruction());
    return state;
}


/* defination of the core interface */
Core::Core(const Config &configs, int coreid, function<bool(Request)> send_next,
           Cache *llc, std::shared_ptr<CacheSystem> cachesys, MemoryBase &memory, bool is_nmp, bool nlp_side)
    : configs(configs), is_nmp(is_nmp), nlp_side(nlp_side), id(coreid), no_core_caches(!configs.has_core_caches()),
      no_shared_cache(!(configs.has_l3_cache() && !is_nmp)), cachesys(cachesys),
      llc(llc), memory(memory)
{
    inFlightMemoryAccess = 0;
    more_reqs = false;
    deployed_app_id = 0;

    // initially reset warmup.
    if (configs.get_warmup_insts() != 0)
        is_warmup_done = false;
    
    if (!is_nmp)
    {
        lock_core = false;
        cpu_type = configs.get_cpu_type();
        active_core_energy = configs.get_host_active_energy()/(configs.get_int_value("cpu_frequency")/1000.0);
        idle_core_energy = configs.get_host_idle_energy()/(configs.get_int_value("cpu_frequency")/1000.0);

        if (no_core_caches)
        {
            send = send_next;
        }
        else
        {
            // L2 caches[0]
            caches.emplace_back(new Cache(
                l2_size, l2_assoc, l2_blocksz, l2_mshr_num,
                Cache::Level::L2, cachesys, is_nmp));
            // L1 caches[1]
            caches.emplace_back(new Cache(
                l1_size, l1_assoc, l1_blocksz, l1_mshr_num,
                Cache::Level::L1, cachesys, is_nmp));
            send = bind(&Cache::send, caches[1].get(), placeholders::_1);

            if (llc != nullptr)
            {
                caches[0]->concatlower(llc);
            }
            caches[1]->concatlower(caches[0].get());

            first_level_cache = caches[1].get();
        }
    }
    else
    {
        lock_core = true;
        cpu_type = configs.get_nmp_core_org();
        if (configs.get_nmp_core_inst_issue_type() == "single") window.ipc = 1;
        active_core_energy = (configs.get_nmp_active_energy()*1.0)/configs.get_int_value("mcp_frequency");
        idle_core_energy = (configs.get_nmp_idle_energy()*1.0)/configs.get_int_value("mcp_frequency");
        if (!nlp_side)
        { 
            memory_energy = 0.010558;  //nj/bit [DRAM+TSV+Logic]
            own_vault_target_addr = (id - configs.get_core_num());  // assuming [vault id == core id]
        }

        if (no_core_caches)
        {
            send = send_next;
        }
        else
        {
            // L1 caches[0]
            caches.emplace_back(new Cache(l1_size, l1_assoc, l1_blocksz, l1_mshr_num,
                                          Cache::Level::L1, cachesys, is_nmp));
            send = bind(&Cache::send, caches[0].get(), placeholders::_1);

            if (llc != nullptr)
            {
                caches[0]->concatlower(llc);
            }
            first_level_cache = caches[0].get();
        }
    }

    initialize_arch_cycle_db();  // initialize the cycle consumption values for x86 opcode.

    /* setup stats metrices for individual core */
    record_region_count.name("record_region_count_" + to_string(id))
        .desc("Record number of basic block encounter during simualation")
        .precision(0);
    record_offload_region_count.name("record_offload_region_count_" + to_string(id))
        .desc("Record number of basic block offloaded during simualation")
        .precision(0);
    record_cycs.name("record_cycs_core_" + to_string(id))
        .desc("Record cycle number for calculating weighted speedup. (Only valid when expected limit instruction number is non zero in config file.)")
        .precision(0);
    record_insts.name("record_insts_core_" + to_string(id))
        .desc("Retired instruction number when record cycle number. (Only valid when expected limit instruction number is non zero in config file.)")
        .precision(0);
    memory_access_cycles.name("memory_access_cycles_core_" + to_string(id))
        .desc("memory access cycles in memory time domain")
        .precision(0);
    cpu_inst.name("cpu_instructions_core_" + to_string(id))
        .desc("cpu instruction number")
        .precision(0);
    idle_cycles.name("idle_cycles_core_" + to_string(id))
        .desc("idle cycles due to windows full")
        .precision(0);
    memory_inst.name("memory_instructions_core_" + to_string(id))
        .desc("memory instruction number")
        .precision(0);
    overhead_cycles.name("overhead_cycle_core_" + to_string(id))
        .desc("amonunt of overhead cycles")
        .precision(0);

    /* set all metrics to zero initially */
    cpu_inst = 0;
    memory_inst = 0;
    idle_cycles = 0;
    memory_access_cycles = 0;
    overhead_cycles = 0;
}

/* initialize cycle consumption values w.r.t. opcode (for x86), it can be extend for diverse architecture */
void Core::initialize_arch_cycle_db()
{
    std::ifstream opCyfile("common/x86_opcode_cycles.csv");
    if (!opCyfile) {
        std::cerr << "Error opening file!" << std::endl;
        return;
    }
    std::string opcy_line;
    while (std::getline(opCyfile, opcy_line)) {
        std::stringstream ss(opcy_line);
        std::string opcode;
        int cycleCount;
        if (std::getline(ss, opcode, ',') && ss >> cycleCount) {
            opcodeCycleCount[opcode] = cycleCount;
        }
    }
}

/* it load the trace files for the core */
void Core::load_trace(string trace_base_name, const Config &configs)
{
    // cout << "Core " << id << " trying to load trace " << trace_base_name + "." + std::to_string(id) << endl;  // for debug purpose.
    if (trace_per_core.init_trace(trace_base_name + "." + std::to_string(id), configs)) { trace_assigned = true; }
    // appName = trace_base_name;    // no need.
}

/* after trace read, requesting addresses will allocte in memory */
void Core::memory_allocates()
{
    trace_line.instPointer = memory.page_allocator(trace_line.instPointer, id);
    for (int i = 0; i < NUM_INSTR_SOURCES; i++)  //considering eual number of source and destination addresses.
    {
        if (trace_line.sourceAddr[i] != 0)
            trace_line.sourceAddr[i] = memory.page_allocator(trace_line.sourceAddr[i], id);
        if (trace_line.destAddr[i] != 0)
            trace_line.destAddr[i] = memory.page_allocator(trace_line.destAddr[i], id);
    } 
}

/* set some flag to perform OoO execution properly */
void Core::execution_flag_set()
{
    loads_exe_flag = true;
    stores_exe_flag = true;
    l_index = 0;
    s_index = 0;

    for (size_t i = 0; i < NUM_INSTR_SOURCES; ++i)    // TODO:  can be optimize just checking index location
    {
        if (trace_line.sourceAddr[i] != 0) loads_exe_flag = false;
        if (trace_line.destAddr[i] != 0) stores_exe_flag = false;
    }

    // get the no of cycle required to perform the operation by the core without memory transfer (getting from x86_opcode_cycles.csv).
    if (opcodeCycleCount.find(trace_line.opcode) != opcodeCycleCount.end())
        bubble_cnt = opcodeCycleCount[trace_line.opcode];
    else
        bubble_cnt = 0;
}

/* load first trace line when core to be execute */
void Core::get_first_instruction()
{
    if (!more_reqs)
    {
        if (trace_assigned)  // load from trace file.
        {
            more_reqs = trace_per_core.get_trace_line(trace_line);
            if (more_reqs) 
            {
                deployed_app_id = trace_line.processID;
                current_thread_id = 0;
                memory_allocates();
                execution_flag_set();
                lock_core = (!more_reqs);
                if (configs.get_simulation_mode() == "MCP-Only") lock_core = true;
            }
        }
        else // load from inst_queue.trace_queue (MCP PUs always load from queue).
        {
            if (!inst_queue.is_empty())
            {
                more_reqs = true;
                trace_line = inst_queue.trace_queue.front();
                inst_queue.pop_front();
                deployed_app_id = trace_line.processID;
                execution_flag_set();
            }
            else 
            { 
                more_reqs = false;
                deployed_app_id = 0;
            }
            
            lock_core = (!more_reqs);
        }

        reached_limit = (!more_reqs);

        // call for initilize the JSON file which contains offlaodable region's information.
        if (configs.get_simulation_mode() == "Co-Simulation" && cpu_type == "outOrder" && more_reqs) compiler_assist_setup(deployed_app_id);
    }
}

/* load next trace line if avaliable otherwise more_req will be false */
bool Core::get_next_instruction()
{
    if (trace_assigned)
    {
        more_reqs = trace_per_core.get_trace_line(trace_line);
        memory_allocates();
        while (trace_line.threadID != current_thread_id)
        {
            own_proc->cores[trace_line.threadID]->inst_queue.trace_queue.push_back(trace_line);
            own_proc->cores[trace_line.threadID]->inst_queue.numberInstructionsInQueue++;
            if (!own_proc->cores[trace_line.threadID]->more_reqs) own_proc->cores[trace_line.threadID]->get_first_instruction();
            more_reqs = trace_per_core.get_trace_line(trace_line);
            memory_allocates();
        }
    }
    else
    {
        if (!inst_queue.is_empty())
        {
            more_reqs = true;
            trace_line = inst_queue.trace_queue.front();
            inst_queue.pop_front();
        }
        else { more_reqs = false; }
    }

    // process id assigned from the trace, core get lock if there no request (it helps to not process further).
    if (deployed_app_id != trace_line.processID) deployed_app_id = trace_line.processID;
    lock_core = (!more_reqs);
    return more_reqs;
}

/* Out of order core working (instruction execution simulation) */
void Core::tick_outOrder()
{ 
    // if retry list of cache contain req then resend them.
    if (first_level_cache != nullptr)
        first_level_cache->tick();

    // increament the no of retiered instruction.
    retired += window.retire();

    // if there no trace line/req then consume idle cycle.
    if (!more_reqs) { idle_cycles++; return; }   

    // if instruction fatching not recv at core then simply return.
    if (inFlightMemoryAccess >= 1) return;    
    
    // consume cycle to simulate overhead time consumption and context switching waiting.
    if (decision_overhead_cycles >= 0 && lock_core)
    {
        if (decision_overhead_cycles > 0)    // consumeing decision-making overhead.
        {
            --decision_overhead_cycles;
            overhead_cycles++;
            return;
        }

        if (!proc_switching_flag)    // wait for the CPU side to complete its current (upto offloadable region) operation. 
        {
            proc_switching_flag = own_proc->can_context_switch(trace_line.processID);
            if (!proc_switching_flag) return;
        }

        if (decision_overhead_cycles == 0)    // when CPU ready to switch then load the trace line (instruction) in MCP PUs.
        {   
            for (unsigned int k = 0; k < nmp_proc->cores.size(); ++k) {
                nmp_proc->cores[k]->get_first_instruction();
            }
            
            if (configs.get_nlp_facility() == "on")
            {
                for (unsigned int l = 0; l < nlp_proc->cores.size(); ++l) {
                    nlp_proc->cores[l]->get_first_instruction();
                }
            }
        }
    }

    // if the MCP PU's queue has limit then bypass function will call again. 
    if (pending_inst_bypass)
    {
        instruction_bypass();
        if (pending_inst_bypass) { idle_cycles++; return; }
    }

    // when control exit from offloadble region then wait for MCP to complete their execution then CPU start to perform.
    if (wait_for_nmp_finish)
    {
        if (!nmp_proc->can_context_switch(trace_line.processID)) { idle_cycles++; return; }
        if (configs.get_nlp_facility() == "on") if (!nlp_proc->can_context_switch(trace_line.processID)) { idle_cycles++; return; }
        wait_for_nmp_finish = false;
        proc_switching_flag = false;
    }

    // execute the instruction.
    if (!lock_core)
    {
        int inserted = 0;
        if (trace_line.instPointer != 0)    // get the instruction from memory.
        {
            Request req(trace_line.instPointer, Request::Type::READ, callback, id, is_nmp);
            req.instruction_request = true;
            if (!send(req)) { idle_cycles++; return; }
            inFlightMemoryAccess++;     // inst is fetching therefor no further execution until it recv.
            trace_line.instPointer = 0;
            cpu_inst++;
            if (!loads_exe_flag || !stores_exe_flag) memory_inst++;
            inserted++;
        }

        // int inserted = 0;
        while (bubble_cnt > 0)    // consume cycles to simulate performing the operation.
        {
            if (inserted == window.ipc) { idle_cycles++; return; }
            if (window.is_full()){ idle_cycles++; return; }
            window.insert(true, -1);
            inserted++;
            bubble_cnt--;
        }

        if (!loads_exe_flag)    // memory read requset send to memory. 
        {
            while (trace_line.sourceAddr[l_index] != 0)
            {
                if (inserted == window.ipc) { idle_cycles++; return; }
                if (window.is_full()) { idle_cycles++; return; }
                Request req(trace_line.sourceAddr[l_index], Request::Type::READ, callback, id, is_nmp);
                if (!send(req)) { idle_cycles++; return; }
                window.insert(false, trace_line.sourceAddr[l_index]);
                inserted++;
                l_index++;
            }
            loads_exe_flag = true;
        }
        
        if (!stores_exe_flag)    // memory write requset send to memory. 
        {
            while (trace_line.destAddr[s_index] != 0)
            {
                Request req(trace_line.destAddr[s_index], Request::Type::WRITE, callback, id, is_nmp);
                if (!send(req)) { idle_cycles++; return; }
                s_index++;
            }
            stores_exe_flag = true;
        }
    }

    get_next_instruction();    // get the next trace line (or say instruction).

    // if trace line contain offloadble tag (ROI_BEGIN/ROI_END) or line reside in an offloadable region (inside_region flag), offloading operation perform.
    if (strcmp(trace_line.opcode, "ROI_BEGIN") == 0 || strcmp(trace_line.opcode, "ROI_END") == 0 || inside_region || configs.get_simulation_mode() == "MCP-Only")
        offload_stratigy();

    // if limit of executed instruction reaches limit then finish and set reached_limit flag to true, also more_req set to false (to specify forefully that there no line exist).
    if (long(own_proc->calculate_total_instruction() + 
            nmp_proc->calculate_total_instruction() + 
            nlp_proc->calculate_total_instruction()) >= expected_limit_insts 
            && !reached_limit)
    {
        record_cycs = clk;
        record_insts = long(cpu_inst.value());
        memory.record_core(id);
        for (unsigned int index = 0; index < nlp_proc->cores.size(); index++)
            nlp_proc->cores[index]->more_reqs = false;
        for (unsigned int index = 0; index < nmp_proc->cores.size(); index++)
            nmp_proc->cores[index]->more_reqs = false;
        for (unsigned int index = 0; index < own_proc->cores.size(); index++) {
            own_proc->cores[index]->reached_limit = true;
            own_proc->cores[index]->more_reqs = false;
        }
    }

    // if the length of the trace is shorter than expected length, then record it when the whole trace finishes, and set reached_limit to true.
    if (!more_reqs)
    {
        if (!reached_limit)
        { 
            record_cycs = clk;
            record_insts = long(cpu_inst.value());
            memory.record_core(id);
            reached_limit = true;
        }
    }
    else    // otherwise continue the execution.
        execution_flag_set();
}

/* decisde the offloading approach */
void Core::offload_stratigy()
{
    if (!is_warmup_done)    // till warmup host CPU will execute solely.
    {
        host_only();
        return;
    }

    if (configs.get_simulation_mode() == "Host-Only")
        host_only();
    else if (configs.get_simulation_mode() == "All-Offload")
        all_offload();
    else if (configs.get_simulation_mode() == "Co-Simulation")
        compiler_assist_offload();
    else if (configs.get_simulation_mode() == "MCP-Only")
        nmp_only();
}

/* host CPU will perform each instruaction (trace line) except offloadable tags */
void Core::host_only()
{
    bool valid;
    do
    {
        valid = true;
        if (!get_next_instruction()) return;
        if (strcmp(trace_line.opcode, "ROI_BEGIN") == 0 || strcmp(trace_line.opcode, "ROI_END") == 0)    // offloadable tag will skip.
            valid = false;
    } while (!valid);   
}

/* host core bypass all the instruction to the MCP PUs blindly (act like entire application execute by MCP side) */
void Core::nmp_only()
{
    if (strcmp(trace_line.opcode, "ROI_BEGIN") == 0 || strcmp(trace_line.opcode, "ROI_END") == 0)    // if trace line contain tag then read next non-tag trace line.
    {
        bool valid;
        do
        {
            valid = true;
            if (!get_next_instruction()) return;
            if (strcmp(trace_line.opcode, "ROI_BEGIN") == 0 || strcmp(trace_line.opcode, "ROI_END") == 0)    // offloadable tag will skip.
                valid = false;   
        } while (!valid); 
    }
    
    decision_overhead_cycles += configs.get_overhead_cycle();
    lock_core = true;
    instruction_bypass();
}

/* all offloadble region will be executed on the MCP side */
void Core::all_offload()
{
    // when the instruction belong from an offloadable region, simply bypass and return.
    if (strcmp(trace_line.opcode, "ROI_BEGIN") != 0 && strcmp(trace_line.opcode, "ROI_END") != 0 && !offload_region_ids.empty() && offload_region_ids.count(trace_line.regionID) > 0)
    {
        instruction_bypass();
        return;
    }

    // when the instruction not belong to an offloadble region, will perform by host CPU but after completing the MCP side execution and return.
    if (strcmp(trace_line.opcode, "ROI_BEGIN") != 0 && strcmp(trace_line.opcode, "ROI_END") != 0 && !offload_region_ids.empty() && offload_region_ids.count(trace_line.regionID) == 0)
    {
        wait_for_nmp_finish = true;
        lock_own_cores(trace_line.processID, false);
        return;
    }

    // if the trace line contain ending offloadble tag then remove from the offading region set and reset the inside_region flag if set is empty.
    if (strcmp(trace_line.opcode, "ROI_END") == 0 && !offload_region_ids.empty() && offload_region_ids.count(trace_line.regionID) > 0)
    {
        offload_region_ids.erase(trace_line.regionID);
        if (offload_region_ids.empty())
            inside_region = false;
    }

    // if the trace line contain starting offloadable tag then push the region id in offloading set, and lock the participating CPU core not perform any instruction further.
    if (strcmp(trace_line.opcode, "ROI_BEGIN") == 0)
    {
        decision_overhead_cycles += configs.get_overhead_cycle();    // decision-making overhead cycle added.
        record_region_count++;
        record_offload_region_count++;
        offload_region_ids.insert(trace_line.regionID);     // offloaded region ID inserted added in offloading list.
        inside_region = true;
        nlp_core_id_gen = 0;    // re-inititiate NLP core ID to 0 (act like round-robin), futher well load balancing mechanism can be implemented.
        lock_own_cores(trace_line.processID, true);
    }

    // if prev line contain offloadable tag then, read the next trace line until the line contain an instruction.
    while (get_next_instruction())
    {
        if (strcmp(trace_line.opcode, "ROI_BEGIN") == 0)    // if the next line contain starting offloadbale tag then perform as previous.
        {
            record_region_count++;
            record_offload_region_count++;
            decision_overhead_cycles += configs.get_overhead_cycle();
            offload_region_ids.insert(trace_line.regionID);
            if (offload_region_ids.size() >= 1)
            {
                inside_region = true;
                nlp_core_id_gen= 0;
                lock_own_cores(trace_line.processID, true);
                continue;
            }
        }
        else if (strcmp(trace_line.opcode, "ROI_END") == 0 &&  offload_region_ids.count(trace_line.regionID) > 0)    // if the next line contain ending offloadbale tag then perform as previous.
        {
            offload_region_ids.erase(trace_line.regionID);
            if (offload_region_ids.empty())
                inside_region = false;
            continue;
        }
        else if (!offload_region_ids.empty() && offload_region_ids.count(trace_line.regionID) > 0)    // if the next line contain instruction who belong to an offloading region then bypass.
        {
            instruction_bypass();
            break;
        }
        else    // otherwise perfom by host CPU, therefore unlocking the CPU cores.
        {
            if (inside_region) wait_for_nmp_finish = true;
            lock_own_cores(trace_line.processID, false);
            break;
        }
    }
}

/* offloading decision make depend on compiler extracted information (JSON) and currect system stats (structure similar to all_offload()) */
void Core::compiler_assist_offload()
{
    // when the instruction belong from an offloadable region, simply bypass and return.
    if (strcmp(trace_line.opcode, "ROI_BEGIN") != 0 && strcmp(trace_line.opcode, "ROI_END") != 0 && !offload_region_ids.empty() && offload_region_ids.count(trace_line.regionID) > 0)
    {
        instruction_bypass();
        return;
    }

    // when the instruction not belong to an offloadble region, will perform by host CPU but after completing the MCP side execution and return.
    if (strcmp(trace_line.opcode, "ROI_BEGIN") != 0 && strcmp(trace_line.opcode, "ROI_END") != 0 && !offload_region_ids.empty() && offload_region_ids.count(trace_line.regionID) == 0)
    {
        wait_for_nmp_finish = true;
        lock_own_cores(trace_line.processID, false);
        return;
    }

    // if the trace line contain ending offloadble tag then remove from the offading region set and reset the inside_region flag if set is empty.
    if (strcmp(trace_line.opcode, "ROI_END") == 0 && !offload_region_ids.empty() && offload_region_ids.count(trace_line.regionID) > 0)
    {
        offload_region_ids.erase(trace_line.regionID);
        if (offload_region_ids.empty())
            inside_region = false;
    }

    // if the trace line contain starting offloadable tag then push the region id in offloading set, and lock the participating CPU core not perform any instruction further.
    if (strcmp(trace_line.opcode, "ROI_BEGIN") == 0)
    {
        record_region_count++;
        std::vector<float> system_state = own_proc->collect_system_info();    // currently system stats are not using during decision-making process but can be used.
        std::vector<float> bb_info_state = collect_basicblock_info(trace_line.regionID);    // collect basic block info using its region ID.

        /* currently its only check the no of memory and non-memory inst. if more memory inst then it offload to MCP side */
        if (bb_info_state[0] > bb_info_state[1]) {
            decision_overhead_cycles += configs.get_overhead_cycle();
            record_offload_region_count++;
            offload_region_ids.insert(trace_line.regionID);
            inside_region = true;
            nlp_core_id_gen = 0;
            lock_own_cores(trace_line.processID, true);
        }
    }

    // if prev line contain offloadable tag then, read the next trace line until the line contain an instruction.
    while (get_next_instruction())
    {
        if (strcmp(trace_line.opcode, "ROI_BEGIN") == 0)    // if the next line contain starting offloadbale tag then perform as previous.
        {
            record_region_count++;
            std::vector<float> system_state = own_proc->collect_system_info();
            std::vector<float> bb_info_state = collect_basicblock_info(trace_line.regionID);
            if (bb_info_state[0] > bb_info_state[1]) {
                decision_overhead_cycles += configs.get_overhead_cycle();
                record_offload_region_count++;
                offload_region_ids.insert(trace_line.regionID);
                inside_region = true;
                nlp_core_id_gen= 0;
                lock_own_cores(trace_line.processID, true);
            }
            continue;
        }
        else if (strcmp(trace_line.opcode, "ROI_END") == 0 &&  offload_region_ids.count(trace_line.regionID) > 0)    // if the next line contain ending offloadbale tag then perform as previous.
        {
            offload_region_ids.erase(trace_line.regionID);
            if (offload_region_ids.empty())
                inside_region = false;
            continue;
        }
        else if (!offload_region_ids.empty() && offload_region_ids.count(trace_line.regionID) > 0)    // if the next line contain ending offloadbale tag then perform as previous.
        {
            instruction_bypass();
            break;
        }
        else    // otherwise perfom by host CPU, therefore unlocking the CPU cores.
        {
            wait_for_nmp_finish = true;
            lock_own_cores(trace_line.processID, false);
            break;
        }
    }
}

/* it will load and initilize compiler extracted info */
void Core::compiler_assist_setup(int procId)
{
    // Load the JSON info files.
    std::string bb_path = configs.get_json_path() + "proc_"+ std::to_string(procId) + "_bb_info.json";
    ifstream file1(bb_path);
    if (!file1.is_open())
        std::cerr << "Failed to open the info files!" << endl;
    else
    {
        file1 >> bb_info_data;
        file1.close();
    }
}

/* collect the compiler extreacted info using unique blockID */
std::vector<float> Core::collect_basicblock_info(int blockID)
{
    std::vector<float> result;
    for (auto &function : bb_info_data)
    {
        auto &basicBlocks = function["BasicBlocks"];
        for (auto &block : basicBlocks)
        {
            if (block["BasicBlockID"] == blockID)
            {
                result.push_back(block["MemoryInstructions"]);
                result.push_back(block["NonMemoryInstructions"]);
                result.push_back(block["TotalInstructions"]);
                result.push_back(block["TotalMemoryConsumption"]);

                return result;
            }
        }
    }
    return result;
}

/* this will bypass the instruction to MCP side queue */
void Core::instruction_bypass()
{
    lock_own_cores(trace_line.processID, true);    // initially lock the all participating cores of CPU side.

    bool read_dirty = false, write_dirty = false;     // reset the flags.

    // if NLP mode active then check the dirty data at LLC level.
    if (configs.get_nlp_facility() == "on")     
    { 
        int counter = 0;
        while (trace_line.sourceAddr[counter] != 0)
        {
            if (check_for_dirty(trace_line.sourceAddr[counter])) read_dirty = true;
            ++counter;
        }
        counter = 0;
        while (trace_line.destAddr[counter] != 0)
        {
            if (check_for_dirty(trace_line.destAddr[counter])) write_dirty = true;
            ++counter;
        }
    }

    // if dirty data found then instruction will added to NLP cores otherwise NMP cores. (this can be extended using &&).
    if (read_dirty || write_dirty)
    {
        int dist_nlp_core_id = nlp_core_id_gen % configs.get_nlp_core_num();    // get the ID of NLP core.

        // if the queue is full then return and wait for next tick utill it find vacant.
        if (configs.get_nmp_core_queue_max_size() != 0 && nlp_proc->cores[dist_nlp_core_id]->inst_queue.numberInstructionsInQueue >= configs.get_nmp_core_queue_max_size())
        {
            pending_inst_bypass = true;
            return;
        }
        nlp_proc->cores[dist_nlp_core_id]->inst_queue.trace_queue.push_back(trace_line);    // inserting instruction in queue.
        nlp_proc->cores[dist_nlp_core_id]->inst_queue.numberInstructionsInQueue++;    // count the queued instruction.
        nlp_core_id_gen++;    // increment for getting next NLP core (as round-robin).
        pending_inst_bypass = false;
        nmp_proc->lock_all_cores(true);    // lock all the NMP cores until NLP finish its task to simulate consistency.
    }
    else    // otherwise insert the instruction to the NMP core whos corrosponding vault has the instruction.
    {
        Core *nmp_core = nmp_proc->cores[get_vault_target(trace_line.instPointer)].get();
        if (configs.get_nmp_core_queue_max_size() != 0 && nmp_core->inst_queue.numberInstructionsInQueue >= configs.get_nmp_core_queue_max_size())
        {
            pending_inst_bypass = true;
            return;
        }
        nmp_core->inst_queue.trace_queue.push_back(trace_line);
        nmp_core->inst_queue.numberInstructionsInQueue++;
        pending_inst_bypass = false;
    }
}

/* check the current address (addr) is dirty at level LLC or not (return true/false) */
bool Core::check_for_dirty(long addr)
{
    auto it =  llc->cache_lines.find(llc->get_index(addr));
    if(it != llc->cache_lines.end()){
        auto& lines = it->second;
        for(auto it = lines.begin(); it != lines.end(); ++it){
            if(it->tag == llc->get_tag(addr)){
                if(it->dirty == true) {
                    return true;
                }
            }
        }
    }
    return false;
}

/* get the mmeory vault address where the requesting address (mem_addr) is reside */
int Core::get_vault_target(long mem_addr)
{
    MemoryBase *ptr_mem = &memory;
    Memory<HMC, Controller> *ptr = dynamic_cast<Memory<HMC, Controller> *>(ptr_mem);
    ptr->clear_higher_bits(mem_addr, ptr->max_address - 1ll);
    ptr->clear_lower_bits(mem_addr, ptr->tx_bits);
    int max_block_col_bits = ptr->spec->maxblock_entry.flit_num_bits - ptr->tx_bits;
    ptr->slice_lower_bits(mem_addr, max_block_col_bits);
    int vault_target = ptr->slice_lower_bits(mem_addr, ptr->addr_bits[int(HMC::Level::Vault)]);
    return vault_target;
}

/* this is used to lock/unlock the cores (using flag) which executing the current process (using processID) */
void Core::lock_own_cores(long processID, bool flag)
{
    for (unsigned int index = 0; index < own_proc->cores.size(); index++) 
    {
        if (own_proc->cores[index]->deployed_app_id == processID) 
            own_proc->cores[index]->lock_core = flag;
    }
}

/* In order core working (instruction execution simulation), similar to OoO interface but due to in-order there is no window used */
void Core::tick_inOrder()
{        
    if (!more_reqs) { idle_cycles++; return; }

    if (inFlightMemoryAccess >= 1) return;

    // if NMP side, then after NLP finish NMP cores will be unloacked to execute further.
    if (configs.get_nlp_facility() == "on" && !nlp_side) {
        if (!nlp_proc->can_nmp_switch()) { idle_cycles++; return; }
        else lock_core = false;
    }

    if (lock_core) { idle_cycles++; return; }    // if locked then consume idle cycle.

    // if NLP side, then after NMP's request completion NLP cores will be execute further.
    if (nlp_side) if (!nmp_proc->can_nmp_switch()) { idle_cycles++; return; }

    // begin to execute the instruction (remining workflow are same as OoO).
    if (trace_line.instPointer != 0)
    {
        if (configs.inst_fetching() == "on")    // if instruction fetching as an read req is enable then it send to memory.
        {
            Request req(trace_line.instPointer, Request::Type::READ, callback, id, is_nmp);
            req.instruction_request = true;
            if (!send(req)) { idle_cycles++; return; }
            inFlightMemoryAccess++;
        } else ++bubble_cnt;    // otherwise just consume one cycle (bubble_cnt) becz this simulator does not have icache concept properly.
        trace_line.instPointer = 0;
        cpu_inst++;
        if (!loads_exe_flag || !stores_exe_flag) memory_inst++;
        if (configs.inst_fetching() == "on") return;
    }

    int inserted = 0;
    while (bubble_cnt > 0)
    {
        if (inserted == window.ipc) { idle_cycles++; return; }
        inserted++;
        bubble_cnt--;
        retired++;
    }

    if (!loads_exe_flag)
    {
        while (trace_line.sourceAddr[l_index] != 0)
        {
            if (inserted == window.ipc) { idle_cycles++; return; }
            if (get_vault_target(trace_line.sourceAddr[l_index]) == own_vault_target_addr)
            {
                Request req(trace_line.sourceAddr[l_index], Request::Type::READ, callback, id, is_nmp);
                if (!send(req)) { idle_cycles++; return; }
            }
            else
            {
                Request req(trace_line.sourceAddr[l_index], Request::Type::READ, callback, id, false);
                if (!send(req)) { idle_cycles++; return; }
            }
            inserted++;
            l_index++;
        }
        loads_exe_flag = true;
    }
    
    if (!stores_exe_flag)
    {
        while (trace_line.destAddr[s_index] != 0)
        {
            if (get_vault_target(trace_line.destAddr[s_index]) == own_vault_target_addr)
            {
                Request req(trace_line.destAddr[s_index], Request::Type::WRITE, callback, id, is_nmp);
                if (!send(req)) { idle_cycles++; return; }
            }
            else
            {
                Request req(trace_line.destAddr[s_index], Request::Type::WRITE, callback, id, false);
                if (!send(req)) { idle_cycles++; return; }
            }
            s_index++;
        }
        stores_exe_flag = true;
    }

    if (get_next_instruction()) { execution_flag_set(); }
}

/* clock as tick call core to perform */
void Core::tick()
{
    clk++;
    if (cpu_type == "inOrder")
        tick_inOrder();
    else if (cpu_type == "outOrder")
        tick_outOrder();
    else
        cout << "Something is wrong \n";
}

/* calculate and show the weightage IPC for the core */
double Core::calc_ipc()
{
    printf("[%d]retired: %ld, clk, %ld\n", id, retired, clk);
    return (double)retired / clk;
}

/* return the core has more instruction to execute or not */
bool Core::finished()
{
    return !more_reqs;
}

/* check the core reached to its limit or not using the simple flag */
bool Core::has_reached_limit()
{
    return reached_limit;
}

/* when core recv a response of a req as callback */
void Core::receive(Request &req)
{
    window.set_ready(req.addr, ~(l1_blocksz - 1l));    // reset the ready flag in window.

    if (req.arrive != -1 && req.depart > last)     // compute the req memory walking time.
    {
        memory_access_cycles += (req.depart - max(last, req.arrive));
        last = req.depart;
    }

    if (req.instruction_request == true)    // if inst fetch req-response then reset inFlightMemoryAccess.
    {
        if (inFlightMemoryAccess != 0)
        {
            inFlightMemoryAccess--;
        }
    }
}

/* rest performance stats */
void Core::reset_stats()
{
    clk = 0;
    retired = 0;
    cpu_inst = 0;
}

/* retrive how many inst executed by the current core */
long Core::get_executed_insts()
{
    return long(cpu_inst.value());
}


/* check OoO window is full or not */
bool Window::is_full()
{
    return load == depth;
}

/* check OoO window is empty or not */
bool Window::is_empty()
{
    return load == 0;
}

/* inserting requesting mem address in window and set the flag, when the req-response recv the fag will be reset */
void Window::insert(bool ready, long addr)
{
    assert(load <= depth);

    ready_list.at(head) = ready;
    addr_list.at(head) = addr;

    head = (head + 1) % depth;
    load++;
}

/* when a req-response recv, window's flag get reset depend on the ready list status */
long Window::retire()
{
    assert(load <= depth);

    if (load == 0)
        return 0;

    int retired = 0;
    while (load > 0 && retired < ipc)
    {
        if (!ready_list.at(tail))
            break;

        tail = (tail + 1) % depth;
        load--;
        retired++;
    }

    return retired;
}

/* when recv a req-response, rest the flag in ready list */
void Window::set_ready(long addr, int mask)
{
    if (load == 0)
        return;

    for (int i = 0; i < load; i++)
    {
        int index = (tail + i) % depth;
        if ((addr_list.at(index) & mask) != (addr & mask))
            continue;
        ready_list.at(index) = true;
    }
}

/* reset the window */
void Window::reset_window()
{
    load = 0;
    head = 0;
    tail = 0;
    ready_list.assign(depth, false);
    addr_list.assign(depth, -1);
}


/* trace interface */
Trace::Trace(const string &trace_fname) : trace_name(trace_fname)
{ }

/* initilaize the trace file for corrosponding core */
bool Trace::init_trace(const string &trace_fname, const Config &configs)
{
    this->configs = configs;
    trace_name = trace_fname;
    file = fopen(trace_name.c_str(), "rb");
    if (file == NULL)
    {
        // std::cerr << "Bad trace file: " << trace_fname << std::endl;
        return false;
    }
    cout << "Trace opended: " << trace_fname << endl;
    return true;
}

/* read the trace line from tarce file */
bool Trace::get_trace_line(trace_format &trace_line)
{
    if (file != NULL)
        if (fread(&trace_line, sizeof(trace_format), 1, file))
            return true;

    return false;
}


  