/* Flow-IPC: SHM-jemalloc
 * Copyright (c) 2023 Akamai Technologies, Inc.; and other contributors.
 * Each commit is copyright by its respective author or author's employer.
 *
 * Licensed under the MIT License:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE. */

/// @file

#include "ipc/shm/arena_lend/jemalloc/stat_info_dump.hpp"
#include "ipc/util/util_fwd.hpp"
#include <flow/util/string_view.hpp>
#include <flow/util/stat/stat_set.hpp>

namespace ipc::shm::arena_lend::jemalloc::stat
{

// Implementations.

std::ostream& operator<<(std::ostream& os, const Arena_info_dump& val)
{
  using util::String_view;
  using flow::util::stat::print;

  String_view ln; // Separator when next thing has no conceptual indent-level.
  String_view ln_ln; // Separator when next thing has 1 conceptual indent-level.
  bool verbose;
  val.m_fmt.m_multiline ? (os << "- ", ln = "\n- ", ln_ln = "\n  - ", verbose = val.m_fmt.m_verbose)
                        : (ln = (ln_ln = " | "), verbose = false);
                        // As advertised (can't print jemalloc-stats-dump without lots of newlines).

  if (verbose) // (Implies multiline.)
  {
    os << "jemalloc_dump/GLOBAL: DUMP_START[[[\n" << val.m_mem_mgr_stats_dump // By contract: newline-terminated.
       << "]]]END_DUMP" << ln;
  }
  else
  {
    os << "jemalloc_dump/GLOBAL: [non-verbose/skipped]" << ln;
  }
  // else { Just skip it.  `verbose` affect nothing else. }

  /* (Subtlety: the "/arena" snippet is a nod to the fact that there are potentially multiple jemalloc-arenas
   * in an Ipc_arena.  (As of this writing there is exactly 1, but this could -- probably will -- sometime change.)
   * Nevertheless we don't use "conceptual indentation" (ln_ln) for it.  It's a subjective feel thing; these
   * stats are pretty central (and indenting always-1 thing looks a bit meh).
   * @todo Once there are indeed potentially >1 jemalloc-arenas in an Ipc_arena, revisit this; see how it looks/feels.
   * Reconsider the "/arena" snippet too; possibly handle the whole thing, cosmetics-wise, as we handle
   * live_pools below.) */
  const auto& mem_mgr_stats_vec = val.m_mem_mgr_stats;
  if (mem_mgr_stats_vec.empty())
  {
    os << "mem_mgr/arena: [Skipping due to jemalloc arena-concurrency bug; upgrade linked jemalloc.]" << ln;
  }
  else
  {
    for (const auto& mem_mgr_stats : mem_mgr_stats_vec)
    {
      // @todo Add idx printout like below, when in reality `mem_mgr_stats_vec.size() > 1` becomes a thing.
      os << "mem_mgr/arena: [" << print(mem_mgr_stats) << ']' << ln; // mem_mgr_stats includes jemalloc-arena-ID.
    }
  }

  os << "sharded: [" << print(val.m_sharded_stats) << ']' << ln
     << "pool: [" << print(val.m_pool_stats) << ']' << ln
     << "obj_db_aux_pool/GLOBAL: [" << print(val.m_obj_db_aux_pool_global_stats) << ']' << ln
     << "owner_pool_lookup/GLOBAL: [" << print(val.m_owner_pool_lookup_global_stats) << ']' << ln;

  const auto& shm_pool_info_vec = val.m_shm_pool_live_info;
  if (shm_pool_info_vec.empty())
  {
    os << "live_pools: [none]"; // No trailing newlines (as advertised).
  }
  else
  {
    os << "live_pools->" << ln_ln;

    size_t idx = 0;
    const auto n_pools = shm_pool_info_vec.size();
    for (const auto& info : shm_pool_info_vec)
    {
      os << "pool[" << (++idx) << '/' << n_pools << "]: [" << print(info) << ']';
      if (idx != n_pools)
      {
        os << ln_ln;
      }
    }
  }

  return os;
} // operator<<(ostream&, Arena_info_dump)

std::ostream& operator<<(std::ostream& os, const Shm_session_info_dump& val)
{
  using util::String_view;
  using flow::util::stat::print;

  String_view ln, ln_ln;
  val.m_fmt.m_multiline ? (os << "- ", ln = "\n- ", ln_ln = "\n  - ")
                        : (ln = (ln_ln = " | "));

  os << "borrower_pool: [" << print(val.m_borrower_pool_stats) << ']' << ln
     << "borrower_pool/total/GLOBAL: [" << print(val.m_borrower_pool_stats_process_wide_total) << ']' << ln;

  const auto& brw_pool_stats_vec = val.m_borrower_pool_stats_process_wide_per_arena;
  if (brw_pool_stats_vec.empty())
  {
    os << "borrower_pool/by_arena/GLOBAL: [none]" << ln;
  }
  else
  {
    os << "borrower_pool/by_arena/GLOBAL->" << ln_ln;

    size_t idx = 0;
    const auto n_arenas = brw_pool_stats_vec.size();
    for (const auto& stats : brw_pool_stats_vec)
    {
      ++idx;
      os << "arena[" << idx << '/' << n_arenas << "]: [" << print(stats) << ']'
         << ((idx == n_arenas) ? ln : ln_ln);
    }
  }

  os << "borrower_pool_lookup/GLOBAL: [" << print(val.m_borrower_pool_lookup_global_stats) << ']' << ln;

  const auto& shm_pool_info_vec = val.m_borrowed_shm_pool_live_info;
  if (shm_pool_info_vec.empty())
  {
    os << "borrowed_pools: [none]"; // No trailing newlines (as advertised).
  }
  else
  {
    os << "borrowed_pools->" << ln_ln;

    size_t idx = 0;
    const auto n_pools = shm_pool_info_vec.size();
    for (const auto& info : shm_pool_info_vec)
    {
      os << "pool[" << (++idx) << '/' << n_pools << "]: [" << print(info) << ']';
      if (idx != n_pools)
      {
        os << ln_ln;
      }
    }
  }

  return os;
} // operator<<(ostream&, Shm_session_info_dump)

} // namespace ipc::shm::arena_lend::jemalloc::stat
