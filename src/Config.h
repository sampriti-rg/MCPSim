#ifndef __CONFIG_H
#define __CONFIG_H

#include <string>
#include <fstream>
#include <vector>
#include <map>
#include <iostream>
#include <cassert>

namespace ramulator
{

class Config {
public:
    enum class Format
    {
        PISA,
        ZSIM,
        PIN
    } format;
private:
    std::map<std::string, std::string> options;
    int stacks;
    int channels;
    int ranks;
    int subarrays;
    int cpu_frequency;
    int core_num = 0;
    int cacheline_size = 64;
    long expected_limit_insts = 0;
    bool disable_per_scheduling = true;
    std::string org = "in_order";

    bool pim_mode_enable = false;
public:
    Config() {}
    Config(const std::string& fname);
    void parse(const std::string& fname);
    std::string operator [] (const std::string& name) const {
      if (options.find(name) != options.end()) {
        return (options.find(name))->second;
      } else {
        return "";
      }
    }

    std::string get_config_path() const {return options.find("config_path")->second;}
    std::string get_core_org() const {return options.find("core_org")->second;}
    int get_core_num() const {return get_int_value("number_cores");}
    int get_host_llc_size() const {return get_int_value("llc_size");}
    int get_host_llc_assoc() const {return get_int_value("llc_assoc");}
    int get_host_active_energy() const {return get_int_value("host_active_energy");}
    int get_host_idle_energy() const {return get_int_value("host_idle_energy");}

    std::string get_nmp_core_org() const {return options.find("mcp_core_org")->second;}
    std::string get_nmp_core_inst_issue_type() const {return options.find("mcp_core_inst_issue")->second;}
    int get_nmp_core_num() const {return get_int_value("number_mcp_cores");}
    int get_nmp_tick() const {return int(1000000.0 / get_int_value("mcp_frequency"));}

    bool has_nmp_core_caches() const {
      if (options.find("mcp_cache") != options.end()) {
        const std::string& cache_option = (options.find("mcp_cache"))->second;
        return (cache_option == "L1");
      } else {
        return false;
      }
    }

    int get_nmp_core_queue_max_size() const {return get_int_value("mcp_core_queue_max_size");}
    int get_nmp_active_energy() const {return get_int_value("mcp_active_energy");}
    int get_nmp_idle_energy() const {return get_int_value("mcp_idle_energy");}

    std::string get_nlp_facility() const {return options.find("nlp_facility")->second;}
    int get_nlp_core_num() const {return get_int_value("llc_slice");}

    std::string get_host_thread_spawning() const {return options.find("host_thread_spawning")->second;}
    std::string debug_context_swithing() const {return options.find("debug_context_swithing")->second;}
    std::string inst_fetching() const {return options.find("consider_inst_fetching")->second;}
    long get_warmup_insts() const {return get_int_value("simulated_warmup_insts");}
    std::string get_json_path() const {return options.find("json_path")->second;}
    int get_overhead_cycle() const {return get_int_value("overhead_cycle");}
    
    std::string get_dram_power_config() const {return options.find("drampower_memspecs")->second;}
    std::string get_simulation_mode() const {return options.find("sim_mode")->second;}

    void set_org(std::string _org){org = _org;}
    void parse_to_const(const std::string& name, const std::string& value);
    void set_disable_per_scheduling(bool status){disable_per_scheduling = status;}
    bool get_disable_per_scheduling() const {return disable_per_scheduling;}

    bool contains(const std::string& name) const {
      if (options.find(name) != options.end()) {
        return true;
      } else {
        return false;
      }
    }

    void add (const std::string& name, const std::string& value) {
      if (!contains(name)) {
        options.insert(make_pair(name, value));
      } else {
        printf("ramulator::Config::add options[%s] already set.\n", name.c_str());
      }
    }

    void set(const std::string& name, const std::string& value) {
      options[name] = value;
      // TODO call this function only name maps to a constant
      parse_to_const(name, value);
    }

    void set_core_num(int _core_num) {core_num = _core_num;}
    void set_cacheline_size(int _cacheline_size) {cacheline_size = _cacheline_size;}

    int get_int_value(const std::string& name) const {
      assert(options.find(name) != options.end() && "can't find this argument");
      return atoi(options.find(name)->second.c_str());
    }
    
    int get_stacks() const {return get_int_value("stacks");}
    int get_channels() const {return get_int_value("channels");}
    int get_subarrays() const {return get_int_value("subarrays");}
    int get_ranks() const {return get_int_value("ranks");}
    Format get_trace_format() const{return format;}
    std::string get_cpu_type() const{
       /*if (contains("core_type")) {
           return options.find("core_type")->second;
       }*/
       return org;
    }
    int get_cpu_tick() const {return int(1000000.0 / get_int_value("cpu_frequency"));}
    int get_cacheline_size() const {return cacheline_size;}

    long get_expected_limit_insts() const {
      if (contains("expected_limit_insts")) {
        return get_int_value("expected_limit_insts");
      } else {
        return 0;
      }
    }

    bool has_l3_cache() const {
      if (options.find("cache") != options.end()) {
        const std::string& cache_option = (options.find("cache"))->second;
        return (cache_option == "all") || (cache_option == "L3");
      } else {
        return false;
      }
    }

    bool has_core_caches() const {
      if (options.find("cache") != options.end() && options.find("mcp_cache") != options.end()) {
        const std::string& cache_option = (options.find("cache"))->second;
        const std::string& nmp_cache_option = (options.find("mcp_cache"))->second;
        return (cache_option == "all" || nmp_cache_option == "L1");
      } else {
        return false;
      }
    }

    bool is_early_exit() const {
      // the default value is true
      if (options.find("early_exit") != options.end()) {
        if ((options.find("early_exit"))->second == "off") {
          return false;
        }
        return true;
      }
      return true;
    }

    bool calc_weighted_speedup() const {
      return (expected_limit_insts != 0);
    }

    bool record_cmd_trace() const {
      // the default value is false
      if (options.find("record_cmd_trace") != options.end()) {
        if ((options.find("record_cmd_trace"))->second == "on") {
          return true;
        }
        return false;
      }
      return false;
    }

    bool print_cmd_trace() const {
      // the default value is false
      if (options.find("print_cmd_trace") != options.end()) {
        if ((options.find("print_cmd_trace"))->second == "on") {
          return true;
        }
        return false;
      }
      return false;
    }
};
} /* namespace ramulator */

#endif /* _CONFIG_H */

