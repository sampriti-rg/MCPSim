#include "Processor.h"
#include "Config.h"
#include "Controller.h"
#include "SpeedyController.h"
#include "HMC_Controller.h"
#include "Memory.h"
#include "HMC_Memory.h"
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

    auto send = bind(&Memory<T, Controller>::send, &memory, placeholders::_1);         // bind the send function with mmeory.
    Processor proc(configs, files, send, memory, false);                               // create CPU processor.
    Processor nmp_proc(configs, files, send, memory, true);                            // create memory side MCP PUs. (i.e., NMP)
    Processor nlp_proc(configs, files, send, memory, true, &proc.llc, proc.cachesys);  // create LLC side MCP PUs. (i.e., NLP)

    proc.nmp_proc = &nmp_proc;                  // initiate NMP side processors to CPU processor.
    proc.nlp_proc = &nlp_proc;
    proc.init_nmp_side();
    proc.init_nlp_side();

    nmp_proc.nlp_proc = &nlp_proc;              // initiate NMP processors to each others.
    nlp_proc.nmp_proc = &nmp_proc;
    nmp_proc.init_nlp_side();
    nlp_proc.init_nmp_side();

    bool is_warming_up = (configs.get_warmup_insts() != 0);
    for(long i = 0; is_warming_up; i++) {
      if (i == next_cpu_tick) {            
        next_cpu_tick += cpu_tick;
        proc.tick();
        Stats::curTick++;
        is_warming_up = true;
        if(proc.get_executed_insts() >= configs.get_warmup_insts()) is_warming_up = false;
        
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

    for (long i = 0; ; i++) {
        if (i == next_cpu_tick) {            
            next_cpu_tick += cpu_tick;
            proc.tick();
            Stats::curTick++; // processor clock, global, for Statistics

            if (configs.calc_weighted_speedup()) {
                if (proc.has_reached_limit() || proc.finished()) {
                   if ((proc.is_complete()) && (nmp_proc.is_complete()) && (nlp_proc.is_complete())) break;
                }
            }
            else{
                if (configs.is_early_exit()) {
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

        if(i == next_nmp_tick && configs.get_simulation_mode() != "Host-Only") {
          next_nmp_tick += nmp_tick;
          nmp_proc.tick();
          if (configs.get_nlp_facility() == "on") nlp_proc.tick();
          if (proc.finished() && nmp_proc.finished() && nlp_proc.finished() && (proc.is_complete()) && (nmp_proc.is_complete()) && (nlp_proc.is_complete())) break;
        }

        if (i == next_mem_tick) {
            next_mem_tick += mem_tick;
            memory.tick();
        }
    }

    // Calculate stats.
    proc.calc_stats();
    if (configs.get_simulation_mode() != "Host-Only") nmp_proc.calc_stats();
    if (configs.get_simulation_mode() != "Host-Only" && configs.get_nlp_facility() == "on") nlp_proc.calc_stats();

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

int main(int argc, const char *argv[])
{
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
    else {
      cout << "Currently it supporting HMC, later it can be extended." << endl ;
    }

    cout << "Simulation done. Statistics written to " << stats_out << endl ;

    return 0;
}
