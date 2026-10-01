#include "Processor.h"
#include "Config.h"
#include "Controller.h"
#include "SpeedyController.h"
#include "3DMemory_Controller.h"
#include "Memory.h"
#include "3DMemory.h"
#include "DRAM.h"
#include "Statistics.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdlib.h>
#include <functional>
#include <map>
#include <boost/program_options.hpp>

/* Standards */
#include "Gem5Wrapper.h"
#include "DDR3.h"
#include "DDR4.h"
#include "DSARP.h"
#include "GDDR5.h"
#include "LPDDR3.h"
#include "LPDDR4.h"
#include "WideIO.h"
#include "WideIO2.h"
#include "HBM.h"
#include "HBM2.h"
#include "HMC.h"
#include "SALP.h"
#include "ALDRAM.h"
#include "TLDRAM.h"

using namespace std;
using namespace ramulator;
namespace po = boost::program_options;

static int gcd(int u, int v) {
  if (v > u) {
    swap(u,v);
  }
  while (v != 0) {
    int r = u % v;
    u = v;
    v = r;
  }
  return u;
}

static int gcd_ThreeNumbers(int a, int b, int c) {
    return gcd(gcd(a, b), c);
}

class OutstandingReqWindow {
 private:
  int inflight_req = 0;
  int inflight_limit = -1; // if inflight_limit == -1, the window is unlimited

 public:
  OutstandingReqWindow(int inflight_limit):inflight_limit(inflight_limit) {}
  bool is_unlimited() const {return (inflight_limit == -1);}
  bool is_full() const { return !is_unlimited() && (inflight_limit == inflight_req); }
  bool is_empty() const { return (inflight_req == 0);}
  void insert() {
    if (!is_unlimited()) {
      assert(!is_full());
      inflight_req++;
    }
  }
  void retire() {
    if (!is_unlimited()) {
      assert(!is_empty());
      inflight_req--;
    }
  }
};

template <typename T>
void run_cputrace(const Config& configs, Memory<T, Controller>& memory, const std::vector<string>& files)
{
    // time unit is ps. setup the clock tick for entire simulation.
    int cpu_tick = configs.get_cpu_tick();
    int nmp_tick = configs.get_nmp_tick();
    int mem_tick = memory.clk_ns() * 1000;
    int tick_gcd = gcd_ThreeNumbers(cpu_tick, nmp_tick, mem_tick);
    // printf("tick_gcd: %d\n", tick_gcd);
    cpu_tick /= tick_gcd;
    // printf("cpu_tick: %d\n", cpu_tick);
    nmp_tick /= tick_gcd;
    // printf("mcp_tick: %d\n", nmp_tick);
    mem_tick /= tick_gcd;
    // printf("mem_tick: %d\n", mem_tick);
    long next_cpu_tick = cpu_tick - 1;
    long next_nmp_tick = nmp_tick - 1;
    long next_mem_tick = mem_tick - 1;

    auto send = [&memory](Request req) { return memory.send(std::move(req)); };        // send function into memory (a lambda avoids std::bind's heap allocation).
    Processor proc(configs, files, send, memory, false);                               // create CPU processor.
    Processor nmp_proc(configs, files, send, memory, true);                            // create memory side MCP PUs. (i.e., NMP)
    Processor nlp_proc(configs, files, send, memory, true, &proc.llc, proc.cachesys);  // create LLC side MCP PUs. (i.e., NLP)

    proc.nmp_proc = &nmp_proc;                  // initiate NMP & NLP side processors to CPU processor.
    proc.nlp_proc = &nlp_proc;
    proc.init_nmp_side();
    proc.init_nlp_side();

    nmp_proc.nlp_proc = &nlp_proc;              // initiate NMP & NLP processors to each others.
    nlp_proc.nmp_proc = &nmp_proc;
    nmp_proc.init_nlp_side();
    nlp_proc.init_nmp_side();

    cout << "System initialization done, simulation starting...\n";

    // Config values read once, instead of a string-map lookup on every loop iteration.
    const long warmup_insts = configs.get_warmup_insts();
    const bool host_only = (configs.get_simulation_mode() == "Host-Only");
    const bool nlp_on = configs.contains("nlp_facility") && configs["nlp_facility"] == "on";
    const bool weighted_speedup = configs.calc_weighted_speedup();
    const bool early_exit = configs.is_early_exit();

    // The loop time i is in units of tick_gcd. Instead of stepping i by 1 and doing nothing on
    // most steps, it jumps straight to the next clock edge. Every edge is still visited, in the
    // same order (CPU, then MCP, then memory), so the simulation is unchanged.
    bool is_warming_up = (warmup_insts != 0);
    for (long i = 0; is_warming_up; i = std::min(next_cpu_tick, next_mem_tick)) {
      if (i == next_cpu_tick) {            
        next_cpu_tick += cpu_tick;
        proc.tick();
        Stats::curTick++;
        is_warming_up = true;
        if(proc.get_executed_insts() >= warmup_insts) is_warming_up = false;
        
        if (proc.has_reached_limit()) {
            printf("WARNING: The end of the input trace file was reached during warmup. Consider changing warmup_insts in the config file.\n");
            break;
        }
      }

      if (i == next_mem_tick) {
          next_mem_tick += mem_tick;
          memory.tick();
      }
    }

    printf("Warmup complete! ");
    // Stats::statlist.reset_stats();
    // proc.reset_stats();
    // nmp_proc.reset_stats();
    proc.warmedup_activate();
    printf("Starting the simulation...\n");

    // i restarts at 0 here, as in the original loop. The MCP clock was not advanced during warmup,
    // so in MCP modes it first catches up on its earlier edges before the CPU edges come around.
    for (long i = 0; ; ) {
        if (i == next_cpu_tick) {            
            next_cpu_tick += cpu_tick;
            proc.tick();
            Stats::curTick++; // processor clock, global, for Statistics

            if (weighted_speedup) {
                if (proc.has_reached_limit() || proc.finished()) {
                   if ((proc.is_complete()) && (nmp_proc.is_complete()) && (nlp_proc.is_complete())) break;
                }
            }
            else{
                if (early_exit) {
                    if (proc.finished())
                        break;
                }
                else {
                    if (proc.finished() && nmp_proc.finished() && nlp_proc.finished() && (proc.is_complete()) && (nmp_proc.is_complete()) && (nlp_proc.is_complete())){
                         break;
                    }
                }
            }
        }

        if(i == next_nmp_tick && !host_only) {
          next_nmp_tick += nmp_tick;
          nmp_proc.tick();
          if (nlp_on) nlp_proc.tick();
          if (proc.finished() && nmp_proc.finished() && nlp_proc.finished() && (proc.is_complete()) && (nmp_proc.is_complete()) && (nlp_proc.is_complete())) break;
        }

        if (i == next_mem_tick) {
            next_mem_tick += mem_tick;
            memory.tick();
        }

        // jump to the next clock edge (the MCP clock only counts when it is actually ticked).
        long next_edge = std::min(next_cpu_tick, next_mem_tick);
        if (!host_only) next_edge = std::min(next_edge, next_nmp_tick);
        i = next_edge;
    }

    // Calculate stats.
    proc.calc_stats();
    if (configs.get_simulation_mode() != "Host-Only" && configs.get_nlp_facility() == "on") nlp_proc.calc_stats();
    if (configs.get_simulation_mode() != "Host-Only") nmp_proc.calc_stats();

    // This a workaround for statistics set only initially lost in the end
    memory.finish();
    Stats::statlist.printall();
}

// template of start_run, it can be extended to other memory type.
template<typename T>
void start_run(const Config& configs, T* spec, const vector<string>& files) {
  // initiate controller and memory
  int C = configs.get_channels(), R = configs.get_ranks();
  // Check and Set channel, rank number
  spec->set_channel_number(C);
  spec->set_rank_number(R);
  std::vector<Controller<T>*> ctrls;
  for (int c = 0 ; c < C ; c++) {
    DRAM<T>* channel = new DRAM<T>(spec, T::Level::Channel);
    channel->id = c;
    channel->regStats("");
    Controller<T>* ctrl = new Controller<T>(configs, channel);
    ctrls.push_back(ctrl);
  }
  Memory<T, Controller> memory(configs, ctrls);

  assert(files.size() != 0);
  if (configs["trace_type"] == "CPU") {
    run_cputrace(configs, memory, files);
  } 
  else {
    cout << "Trace type should be CPU type." << endl ;
  }
}

template<>
void start_run<HMC>(const Config& configs, HMC* spec, const vector<string>& files) {
  int V = spec->org_entry.count[int(HMC::Level::Vault)];    // get the vault count.
  int S = configs.get_stacks();                             // get the stack count.
  int total_vault_number = V * S;                           // get the total no of vault in memory (currently one stack support).
  debug_hmc("total_vault_number: %d\n", total_vault_number);
  std::vector<Controller<HMC>*> vault_ctrls;                // vault controller pointer container.
  for (int c = 0 ; c < total_vault_number ; ++c) {          // creating vault controller.
    DRAM<HMC>* vault = new DRAM<HMC>(spec, HMC::Level::Vault);
    vault->id = c;
    vault->regStats("");
    Controller<HMC>* ctrl = new Controller<HMC>(configs, vault);
    vault_ctrls.push_back(ctrl);
  }
  Memory<HMC, Controller> memory(configs, vault_ctrls);

  assert(files.size() != 0);
  if (configs["trace_type"] == "CPU") {
    run_cputrace(configs, memory, files);
  } 
  else {
    cout << "Trace type should be CPU type." << endl ;
  }
}

template<>
void start_run<HBM>(const Config& configs, HBM* spec, const vector<string>& files) {
  int C = configs.get_channels();                         // get the channel count.
  int R = configs.get_ranks();                            // get the rank count.
  int S = configs.get_stacks();                           // get the memory stack count.
  spec->set_channel_number(C);
  spec->set_rank_number(R);
  int total_chennel_number = C * S;
  debug_hmc("total_chennel_number: %d\n", total_chennel_number);
  std::vector<Controller<HBM>*> ctrls;                        // channel controller pointer container.
  for (int c = 0 ; c < total_chennel_number ; ++c) {          // creating vault controller.
    DRAM<HBM>* channel = new DRAM<HBM>(spec, HBM::Level::Channel);
    channel->id = c;
    channel->regStats("");
    Controller<HBM>* ctrl = new Controller<HBM>(configs, channel);
    ctrls.push_back(ctrl);
  }
  Memory<HBM, Controller> memory(configs, ctrls);

  assert(files.size() != 0);
  if (configs["trace_type"] == "CPU") {
    run_cputrace(configs, memory, files);
  } 
  else {
    cout << "Trace type should be CPU type." << endl ;
  }
}

template<>
void start_run<HBM2>(const Config& configs, HBM2* spec, const vector<string>& files) {
  int C = configs.get_channels();                         // get the channel count.
  int R = configs.get_ranks();                            // get the rank count.
  int S = configs.get_stacks();                           // get the memory stack count.
  spec->set_channel_number(C);
  spec->set_rank_number(R);
  int total_chennel_number = C * S;
  debug_hmc("total_chennel_number: %d\n", total_chennel_number);
  std::vector<Controller<HBM2>*> ctrls;                        // channel controller pointer container.
  for (int c = 0 ; c < total_chennel_number ; ++c) {          // creating vault controller.
    DRAM<HBM2>* channel = new DRAM<HBM2>(spec, HBM2::Level::Channel);
    channel->id = c;
    channel->regStats("");
    Controller<HBM2>* ctrl = new Controller<HBM2>(configs, channel);
    ctrls.push_back(ctrl);
  }
  Memory<HBM2, Controller> memory(configs, ctrls);

  assert(files.size() != 0);
  if (configs["trace_type"] == "CPU") {
    run_cputrace(configs, memory, files);
  } 
  else {
    cout << "Trace type should be CPU type." << endl ;
  }
}

int main(int argc, const char *argv[])
{
    cout << "___  ________ ______  _____ _           \n";
    cout << "|  \\/  /  __ \\| ___ \\/  ___(_)          \n";
    cout << "| .  . | /  \\/| |_/ /\\ `--. _ _ __ ___  \n";
    cout << "| |\\/| | |    |  __/  `--. \\ | '_ ` _ \\ \n";
    cout << "| |  | | \\__/\\| |    /\\__/ / | | | | | |\n";
    cout << "\\_|  |_/\\____/\\_|    \\____/|_|_| |_| |_|\n";
    cout << "______________________________________________________________\n";

    // to show the help prompt.
    po::options_description desc;
    desc.add_options()
      ("help", "print simple manual")
      ("config", po::value<string>(), "path to config file.")
      ("stats", po::value<string>(), "path to output file.")
      ("trace", po::value<std::vector<string>>()->multitoken(), "a single or a list of file name(s) that are the traces to run.")
       ;
    
    // read the configuration from config and setting up.
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (argc < 2 || vm.count("help")) {
      cout << desc << "\n";
      return 1;
    }

    if (!vm.count("config")) {
      cout << "config file is required. (missing --config [configfile])" << endl;
      return 1;
    }

    Config configs(vm["config"].as<string>());

    const std::string& standard = configs["standard"];
    assert(standard != "" || "DRAM standard should be specified.");

    configs.add("trace_type", "CPU");
    
    string stats_out;
    if (vm.count("stats")) {
      stats_out = vm["stats"].as<string>();
    } else {
      stats_out = standard + string(".stats");
    }
    Stats::statlist.output(stats_out);
    
    std::vector<string> files;
    if (vm.count("trace")) {
      files = vm["trace"].as<vector<string>>();
    } else {
      cout << "trace file name(s) is(are) required. (missing --trace [core1-trace core2-trace...])";
    }

    configs.set_core_num(configs.get_core_num());
    configs.set_org(configs.get_core_org());

    if (configs["unlimit_bandwidth"] == "true") {
      configs.set("speed", configs["speed"] + "_unlimit_bandwidth");
    }
    
    if (standard == "HMC") {
      HMC* hmc = new HMC(configs["org"], configs["speed"], configs["maxblock"],
          configs["link_width"], configs["lane_speed"],
          configs.get_int_value("source_mode_host_links"),
          configs.get_int_value("payload_flits"));
      start_run(configs, hmc, files);
    }
    else if (standard == "HBM") {
      HBM* hbm = new HBM(configs["org"], configs["speed"]);
      start_run(configs, hbm, files);
    }
    else if (standard == "HBM2") {
      HBM2* hbm = new HBM2(configs["org"], configs["speed"]);
      start_run(configs, hbm, files);
    }
    else {
      cout << "Currently it supporting 3D-stacked memory (i.e., HBM/HMC/HBM2), later it can be extended." << endl ;
    }

    cout << "Simulation done. Statistics written to " << stats_out << endl ;

    return 0;
}
