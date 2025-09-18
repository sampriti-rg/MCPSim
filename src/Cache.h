#ifndef __CACHE_H
#define __CACHE_H

#include "Config.h"
#include "Request.h"
#include "Statistics.h"
#include <algorithm>
#include <cstdio>
#include <cassert>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <queue>
#include <list>

namespace ramulator
{
  class CacheSystem;

  class Cache
  {
  public:
    enum class Level
    {
      L1,
      L2,
      L3,
      MAX
    } level;
    std::string level_string;

    // cache line structure.
    struct Line
    {
      long addr;
      long tag;
      bool lock; // when the lock is on, the value is not valid yet.
      bool dirty;
      long coreId;
      Line(long addr, long tag) : addr(addr), tag(tag), lock(true), dirty(false) {}
      Line(long addr, long tag, long coreId) : addr(addr), tag(tag), lock(true), dirty(false), coreId(coreId) {}
      Line(long addr, long tag, bool lock, bool dirty, long coreId) : addr(addr), tag(tag), lock(lock), dirty(dirty), coreId(coreId) {}
    };

    Cache(int size, int assoc, int block_size, int mshr_entry_num,
          Level level, std::shared_ptr<CacheSystem> cachesys, bool is_nmp);

    
    bool is_nmp;

    // L1, L2, L3 accumulated latencies
    int latency[int(Level::MAX)] = {4, 4 + 12, 4 + 12 + 31};
    int latency_each[int(Level::MAX)] = {4, 12, 31};

    // L1, L2, L3 accumulated energies.
    double energy_consuption[int(Level::MAX)] = {0.494, 3.307, 6.995};

    std::shared_ptr<CacheSystem> cachesys;  // pointer of overall cache system.
    std::vector<Cache *> higher_cache;      // LLC has multiple higher caches.
    Cache *lower_cache;

    ScalarStat cache_read_miss;             // stats containers.
    ScalarStat cache_write_miss;
    ScalarStat cache_total_miss;
    ScalarStat cache_eviction;
    ScalarStat cache_read_access;
    ScalarStat cache_write_access;
    ScalarStat cache_total_access;
    ScalarStat cache_mshr_hit;
    ScalarStat cache_mshr_unavailable;
    ScalarStat cache_set_unavailable;
    ScalarStat cache_hit;
    ScalarStat cache_load_blocks;
    ScalarStat cache_write_back_lower;
    ScalarStat cache_write_back_hmc;

    void tick();
    bool send(Request req);
    void concatlower(Cache *lower);
    void callback(Request &req);

  protected:
    bool is_first_level;
    bool is_last_level;
    size_t size;
    unsigned int assoc;
    unsigned int block_num;
    unsigned int block_size;
    unsigned int mshr_entry_num;

    int calc_log2(int val)
    {
      int n = 0;
      while ((val >>= 1))
        n++;
      return n;
    }

    // align the address to cache line size.
    long align(long addr)
    {
      return (addr & ~(block_size - 1l));
    }

    // evict the cache line from higher level to this level.
    // pass the dirty bit and update LRU queue.
    void evictline(long addr, bool dirty);

    // invalidate the line from this level to higher levels.
    // the return value is a pair. The first element is invalidation
    // latency, and the second is wether the value has new version
    // in higher level and this level.
    std::pair<long, bool> invalidate(long addr);

    // evict the victim from current set of lines.
    // first do invalidation, then call evictline(L1 or L2) or send
    // a write request to memory(L3) when dirty bit is on.
    void evict(std::list<Line> *lines, std::list<Line>::iterator victim);

    // first test whether need eviction, if so, do eviction by
    // calling evict function. Then allocate a new line and return
    // the iterator points to it.
    std::list<Line>::iterator allocate_line(std::list<Line> &lines, long addr, long coreId); //2406

    // check whether the set to hold addr has space or eviction is
    // needed.
    bool need_eviction(const std::list<Line> &lines, long addr);

    // Check whether this addr is hit and fill in the pos_ptr with
    // the iterator to the hit line or lines.end()
    bool is_hit(std::list<Line> &lines, long addr, std::list<Line>::iterator *pos_ptr);

    bool all_sets_locked(const std::list<Line> &lines)
    {
      if (lines.size() < assoc)
      {
        return false;
      }
      for (const auto &line : lines)
      {
        if (!line.lock)
        {
          return false;
        }
      }
      return true;
    }

    bool check_unlock(long addr)
    {
      auto it = cache_lines.find(get_index(addr));
      if (it == cache_lines.end())
      {
        return true;
      }
      else
      {
        auto &lines = it->second;
        auto line = find_if(lines.begin(), lines.end(),
                            [addr, this](Line l)
                            { return (l.tag == get_tag(addr)); });
        if (line == lines.end())
        {
          return true;
        }
        else
        {
          bool check = !line->lock;
          if (!is_first_level)
          {
            for (auto hc : higher_cache)
            {
              if (!check)
              {
                return check;
              }
              check = check && hc->check_unlock(line->addr);
            }
          }
          return check;
        }
      }
    }

    std::vector<std::pair<long, std::list<Line>::iterator>>::iterator hit_mshr(long addr)
    {
      auto mshr_it =
          find_if(mshr_entries.begin(), mshr_entries.end(),
                  [addr, this](std::pair<long, std::list<Line>::iterator>
                                   mshr_entry)
                  {
                    return (align(mshr_entry.first) == align(addr));
                  });
      return mshr_it;
    }

    std::list<Line> &get_lines(long addr)
    {
      if (cache_lines.find(get_index(addr)) == cache_lines.end())
      {
        cache_lines.insert(make_pair(get_index(addr),
                                     std::list<Line>()));
      }
      return cache_lines[get_index(addr)];
    }
  
  public:
    unsigned int index_mask;
    unsigned int index_offset;
    unsigned int tag_offset;
    std::map<int, std::list<Line>> cache_lines;
    std::vector<std::pair<long, std::list<Line>::iterator>> mshr_entries;
    std::list<Request> retry_list;
    int get_index(long addr)
    {
      return (addr >> index_offset) & index_mask;
    } // 

    long get_tag(long addr)
    {
      return (addr >> tag_offset);
    }

    // bool flush_dirty_lines(long coreId);  // Not working ...
    void flush_line(long addr);
    void flush_all_dirty_lines();
  };

  class CacheSystem
  {
  public:
    CacheSystem(const Config &configs, std::function<bool(Request)> send_memory, bool is_nmp) : send_memory(send_memory), is_nmp(is_nmp)
    {
      if (configs.has_core_caches())
      {
        first_level = Cache::Level::L1;
      }
      else if (configs.has_l3_cache())
      {
        first_level = Cache::Level::L3;
      }
      else
      {
        last_level = Cache::Level::MAX; // no cache
      }

      if (configs.has_l3_cache())
      {
        last_level = Cache::Level::L3;
      }
      else if (configs.has_core_caches())
      {
        last_level = Cache::Level::L2;
      }
      else
      {
        last_level = Cache::Level::MAX; // no cache
      }

      // fixed only one level of cache (private cache)
      if (is_nmp)
      {
        first_level = Cache::Level::L1;
        last_level = Cache::Level::L1;
      }
    }

    // wait_list contains miss requests with their latencies in
    // cache. When this latency is met, the send_memory function
    // will be called to send the request to the memory system.
    std::list<std::pair<long, Request>> wait_list;
    bool is_wait_list_empty(long coreId);

    // hit_list contains hit requests with their latencies in cache.
    // callback function will be called when this latency is met and
    // set the instruction status to ready in processor's window.
    std::list<std::pair<long, Request>> hit_list;

    std::function<bool(Request)> send_memory;

    long clk = 0;
    void tick();
    bool is_nmp = false;

    Cache::Level first_level;
    Cache::Level last_level;
  };

} // namespace ramulator

#endif /* __CACHE_H */
