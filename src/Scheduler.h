#ifndef __SCHEDULER_H
#define __SCHEDULER_H

#include "DRAM.h"
#include "Request.h"
#include "Controller.h"
#include <vector>
#include <map>
#include <list>
#include <functional>
#include <cassert>

#include <algorithm>
#include <cstring>
using namespace std;

namespace ramulator
{

template <typename T>
class Controller;

template <typename T>
class Scheduler
{
public:
    Controller<T>* ctrl;

    enum class Type {
        FCFS, FRFCFS, FRFCFS_Cap, FRFCFS_PriorHit, MAX
    } type = Type::FRFCFS_PriorHit;

    long cap = 16;

    Scheduler(Controller<T>* ctrl) : ctrl(ctrl) {}

    list<Request>::iterator get_head(list<Request>& q)
    {
      // FR-FCFS variants: the controller state (clock, bank states, timing tables) does not change
      // while the queue is scanned, so is_ready / is_row_hit / is_row_open are fixed for each request
      // during one call. They are computed at most once per request and cached, which picks the same
      // request as the old code but avoids asking the same question up to five times per request.
      if (type == Type::FRFCFS) {
        if (!load_queue(q)) return q.end();
        int head = 0;
        for (int k = 1; k < n_req; ++k)
          head = pick_frfcfs(head, k);
        return req_it[head];
      }

      // TODO make the decision at compile time
      if (type != Type::FRFCFS_PriorHit) {
        if (!q.size())
            return q.end();

        auto head = q.begin();
        for (auto itr = next(q.begin(), 1); itr != q.end(); itr++)
            head = compare[int(type)](head, itr);

        return head;
      } else {
        if (!load_queue(q)) return q.end();

        // oldest request among those that are ready and row hits (or oldest overall).
        int head = 0;
        for (int k = 1; k < n_req; ++k) {
          bool ready1 = ready_hit(head);
          bool ready2 = ready_hit(k);
          if (ready1 ^ ready2) {
            if (!ready1) head = k;
          } else if (!(req_it[head]->arrive <= req_it[k]->arrive)) {
            head = k;
          }
        }

        if (ready(head) && row_hit(head)) {
          return req_it[head];
        }

        // requests that are row hits; their bank/subarray prefix must not be closed by a PRE.
        hit_list.clear();
        for (int k = 0; k < n_req; ++k)
          if (row_hit(k)) hit_list.push_back(k);

        // TODO Here it assumes all DRAM standards use PRE to close a row
        const int group_len = int(ctrl->channel->spec->scope[int(T::Command::PRE)]) + 1;

        // if we can't find proper request, we need to return q.end(),
        // so that no command will be scheduled
        int best = -1;
        for (int k = 0; k < n_req; ++k) {
          bool violate_hit = false;
          if ((!row_hit(k)) && this->ctrl->is_row_open(req_it[k])) {
            // so the next instruction to be scheduled is PRE, might violate hit
            const int* mine = req_it[k]->addr_vec.data();
            for (int h : hit_list) {
              if (std::equal(mine, mine + group_len, req_it[h]->addr_vec.data())) {
                violate_hit = true;
                break;
              }
            }
          }
          if (violate_hit) {
            continue;
          }
          // If it comes here, that means it won't violate any hit request
          best = (best < 0) ? k : pick_frfcfs(best, k);
        }

        return best < 0 ? q.end() : req_it[best];
      }
    }

private:
    typedef list<Request>::iterator ReqIter;

    // per-call scratch, kept as members so no allocation happens on each scheduling decision.
    vector<ReqIter> req_it;
    vector<signed char> ready_cache, hit_cache;   // -1 = not computed yet
    vector<int> hit_list;
    int n_req = 0;

    bool load_queue(list<Request>& q)
    {
      n_req = 0;
      if (q.empty()) return false;
      req_it.clear();
      for (auto itr = q.begin(); itr != q.end(); ++itr) req_it.push_back(itr);
      n_req = int(req_it.size());
      if (int(ready_cache.size()) < n_req) { ready_cache.resize(n_req); hit_cache.resize(n_req); }
      std::memset(ready_cache.data(), -1, n_req);
      std::memset(hit_cache.data(), -1, n_req);
      return true;
    }

    bool ready(int k)
    {
      if (ready_cache[k] < 0) ready_cache[k] = this->ctrl->is_ready(req_it[k]);
      return ready_cache[k];
    }

    bool row_hit(int k)
    {
      if (hit_cache[k] < 0) hit_cache[k] = this->ctrl->is_row_hit(req_it[k]);
      return hit_cache[k];
    }

    bool ready_hit(int k) { return ready(k) && row_hit(k); }

    // same rule as the FRFCFS comparator: a ready request wins, otherwise the older one (ties keep a).
    int pick_frfcfs(int a, int b)
    {
      bool ready1 = ready(a);
      bool ready2 = ready(b);
      if (ready1 ^ ready2) return ready1 ? a : b;
      return (req_it[a]->arrive <= req_it[b]->arrive) ? a : b;
    }
    function<ReqIter(ReqIter, ReqIter)> compare[int(Type::MAX)] = {
        // FCFS
        [this] (ReqIter req1, ReqIter req2) {
            if (req1->arrive <= req2->arrive) return req1;
            return req2;},

        // FRFCFS
        [this] (ReqIter req1, ReqIter req2) {
            bool ready1 = this->ctrl->is_ready(req1);
            bool ready2 = this->ctrl->is_ready(req2);

            if (ready1 ^ ready2) {
                if (ready1) return req1;
                return req2;
            }

            if (req1->arrive <= req2->arrive) return req1;
            return req2;},

        // FRFCFS_CAP
        [this] (ReqIter req1, ReqIter req2) {
            bool ready1 = this->ctrl->is_ready(req1);
            bool ready2 = this->ctrl->is_ready(req2);

            ready1 = ready1 && (this->ctrl->rowtable->get_hits(req1->addr_vec) <= this->cap);
            ready2 = ready2 && (this->ctrl->rowtable->get_hits(req2->addr_vec) <= this->cap);

            if (ready1 ^ ready2) {
                if (ready1) return req1;
                return req2;
            }

            if (req1->arrive <= req2->arrive) return req1;
            return req2;},
        // FRFCFS_PriorHit
        [this] (ReqIter req1, ReqIter req2) {
            bool ready1 = this->ctrl->is_ready(req1) && this->ctrl->is_row_hit(req1);
            bool ready2 = this->ctrl->is_ready(req2) && this->ctrl->is_row_hit(req2);

            if (ready1 ^ ready2) {
                if (ready1) return req1;
                return req2;
            }

            if (req1->arrive <= req2->arrive) return req1;
            return req2;}
    };
};


template <typename T>
class RowPolicy
{
public:
    Controller<T>* ctrl;

    enum class Type {
        Closed, Opened, Timeout, MAX
    } type = Type::Opened;

    int timeout = 50;

    RowPolicy(Controller<T>* ctrl) : ctrl(ctrl) {}

    vector<int> get_victim(typename T::Command cmd)
    {
        return policy[int(type)](cmd);
    }

private:
    function<vector<int>(typename T::Command)> policy[int(Type::MAX)] = {
        // Closed
        [this] (typename T::Command cmd) -> vector<int> {
            for (auto& kv : this->ctrl->rowtable->table) {
                if (!this->ctrl->is_ready(cmd, kv.first))
                    continue;
                return kv.first;
            }
            return vector<int>();},

        // Opened
        [this] (typename T::Command cmd) {
            return vector<int>();},

        // Timeout
        [this] (typename T::Command cmd) -> vector<int> {
            for (auto& kv : this->ctrl->rowtable->table) {
                auto& entry = kv.second;
                if (this->ctrl->clk - entry.timestamp < timeout)
                    continue;
                if (!this->ctrl->is_ready(cmd, kv.first))
                    continue;
                return kv.first;
            }
            return vector<int>();}
    };

};


template <typename T>
class RowTable
{
public:
    Controller<T>* ctrl;

    struct Entry {
        int row;
        int hits;
        long timestamp;
    };

    map<vector<int>, Entry> table;

    RowTable(Controller<T>* ctrl) : ctrl(ctrl) {}

    void update(typename T::Command cmd, const vector<int>& addr_vec, long clk)
    {
        auto begin = addr_vec.begin();
        auto end = begin + int(T::Level::Row);
        vector<int> rowgroup(begin, end); // bank or subarray
        int row = *end;

        T* spec = ctrl->channel->spec;

        if (spec->is_opening(cmd))
            table.insert({rowgroup, {row, 0, clk}});

        if (spec->is_accessing(cmd)) {
            // we are accessing a row -- update its entry
            auto match = table.find(rowgroup);
            assert(match != table.end());
            assert(match->second.row == row);
            match->second.hits++;
            match->second.timestamp = clk;
        } /* accessing */

        if (spec->is_closing(cmd)) {
          // we are closing one or more rows -- remove their entries
          int n_rm = 0;
          int scope = int(spec->scope[int(cmd)]);
          for (auto it = table.begin(); it != table.end();) {
            if (equal(begin, begin + scope + 1, it->first.begin())) {
              n_rm++;
              it = table.erase(it);
            }
            else
              it++;
          }
          assert(n_rm > 0);
        } /* closing */
    }

    int get_hits(vector<int>& addr_vec)
    {
        auto begin = addr_vec.begin();
        auto end = begin + int(T::Level::Row);

        vector<int> rowgroup(begin, end);
        int row = *end;

        auto itr = table.find(rowgroup);
        auto entry = table[rowgroup];
        if (itr == table.end() || entry.row != row)
            return 0;

        return entry.hits;
    }
};

} /*namespace ramulator*/

#endif /*__SCHEDULER_H*/
