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
#pragma once

#include "ipc/shm/arena_lend/jemalloc/jemalloc_fwd.hpp"
#include "ipc/shm/arena_lend/arena_lend_stats.hpp"
#include "ipc/util/util.hpp"
#include <string>
#include <vector>

namespace ipc::shm::arena_lend::jemalloc::stat
{

// Types.

/**
 * Printable (with knobs in contained util::stat::Info_dump_format) bundling of stats/info relevant to (but not
 * all necessarily owned by) a particular jemalloc::Ipc_arena as stat-consumed at a particular point in time.
 *   - Stats/info stored by-value; can be queried in peace.
 *   - Can be printed to an `ostream` via `ostream<<`.  #m_fmt (`Info_dump_format`) has output-format knobs,
 *     adjusting which ahead of the `<<` will affect the output.
 *     - As usual: can `FLOW_LOG_...(x)` it, `boost::lexical_cast<string>(x)` it, `flow::util::ostream_op_string(x)`,
 *       where `x` is a `*this`.
 *
 * ### Subject matter ###
 * One fills-out the payload by calling `arena.info_dump(&x)`, where `arena` is an `Ipc_arena`; `x` is a `*this`.
 *
 * Some of the items saved in a `*this` were (pre-copying-them-into-`*this`) owned by/pertaining to the
 * specific target Ipc_arena `arena`.  (E.g.: #m_sharded_stats.)
 *
 * Others (e.g.: #m_owner_pool_lookup_global_stats) are global -- not pertaining to any particular arena -- but
 * pertain to SHM-arena(s) in general, at least tangentially.  Such entities are a/k/a *owner-side*.  So they
 * pertain to `Ipc_arena`s in at least some sense... just not necessarily to *the* arena `arena` that filled `*this`.
 *
 * The non-per-`Ipc_arena` data in `*this` may be redundant w/r/t the non-per-`Ipc_arena` data in `*that`.
 * It is still included simply out of the pragmatic belief that, if one is interested about "everything"
 * about arena X, they'll want to know about data/algorithms that make arena X (...but also other arenas)
 * work properly.  I.e.: the more the merrier.
 *
 * @see The other, disjoint such bundling in SHM-jemalloc is Shm_session_info_dump (<=> jemalloc::Shm_session);
 *      it is generally *borrower-side* info.  (Arena_info_dump (<=> jemalloc::Ipc_arena) is SHM-jemalloc's
 *      *owner-side* info.)
 */
struct Arena_info_dump
{
  // Data.

  /**
   * Print-format knobs in effect at `ostream << *this` time.
   *
   * @note Info_dump_format::m_verbose specifically controls whether -- if available in the first
   *       place -- #m_mem_mgr_stats_dump (as from jemalloc::Memory_manager::stats_dump_to_ostream())
   *       shall be included in the output (`true` <=> yes).  However:
   * @note Info_dump_format::m_multiline being `false` shall cause printing as-if `m_verbose == false`
   *       regardless of its actual value.  (It is not reasonable to print jemalloc's huge #m_mem_mgr_stats_dump,
   *       if newlines are not allowed.)
   */
  util::stat::Info_dump_format m_fmt;

  /// If unavailable then empty; otherwise the newline-terminated output of Memory_manager::stats_dump_to_ostream().
  std::string m_mem_mgr_stats_dump;

  /// If unavailable then empty; otherwise the target `Ipc_arena`'s stats from: Ipc_arena::memory_manager_stats().
  std::vector<arena_lend::stat::Memory_manager_stats> m_mem_mgr_stats;

  /// Target `Ipc_arena`'s stats from: Ipc_arena::sharded_stats().
  arena_lend::stat::Sharded_stats m_sharded_stats;

  /// Target `Ipc_arena`'s stats from: Ipc_arena::pool_stats().
  arena_lend::stat::Pool_stats m_pool_stats;

  /// Global stats from: `static` Ipc_arena::obj_db_aux_pool_global_stats().
  arena_lend::stat::Obj_db_aux_pool_global_stats m_obj_db_aux_pool_global_stats;

  /// Global stats from: `static` Ipc_arena::owner_pool_lookup_global_stats().
  arena_lend::stat::Owner_pool_lookup_global_stats m_owner_pool_lookup_global_stats;

  /// Target `Ipc_arena`'s info from: Ipc_arena::shm_pool_live_info().
  std::vector<arena_lend::stat::Shm_pool_info> m_shm_pool_live_info;
}; // struct Arena_info_dump

/**
 * Printable (with knobs in contained util::stat::Info_dump_format) bundling of stats/info relevant to (but not
 * all necessarily owned by) a particular session::shm::arena_lend::jemalloc::Shm_session as stat-consumed at
 * particular point in time.
 *
 * @see brief notes in a similar place in Arena_info_dump doc header.
 *
 * ### Subject matter ###
 * One fills-out the payload by calling `sess.info_dump(&x)`, where `sess` is a `Shm_session`; `x` is a `*this`.
 *
 * Some of the items saved in a `*this` were (pre-copying-them-into-`*this`) owned by/pertaining to the
 * specific target Shm_session `sess`.  (E.g.: #m_borrower_pool_stats.)
 *
 * Others (e.g.: #m_borrower_pool_lookup_global_stats) are global -- not pertaining to any particular session -- but
 * pertain to SHM-session(s) in general, at least tangentially.  Such entities are a/k/a *borrower-side*.  So they
 * pertain to `Shm_session`s in at least some sense... just not necessarily to *the* session `sess` that filled `*this`.
 *
 * As with Arena_info_dump + Ipc_arena: the preceding paragraph means 2 `Shm_session_info_dump`s will, in the
 * global-style members, have potentially mutually-redundant -- but still of potential interest -- information.
 *
 * @see The other, disjoint such bundling in SHM-jemalloc is Arena_info_dump (<=> jemalloc::Ipc_arena);
 *      it is generally *owner-side* info.  (Shm_session_info_dump (<=> jemalloc::Shm_session) is SHM-jemalloc's
 *      *borrower-side* info.)
 */
struct Shm_session_info_dump
{
  // Data.

  /**
   * Print-format knobs in effect at `ostream << *this` time.
   *
   * @note Info_dump_format::m_verbose has no effect.
   */
  util::stat::Info_dump_format m_fmt;

  /// Target `Shm_session`'s stats from: Shm_session::borrower_pool_stats().
  arena_lend::stat::Borrower_pool_stats m_borrower_pool_stats;

  /// Global stats from: `static` Shm_session::borrower_pool_stats_process_wide(): return value.
  arena_lend::stat::Borrower_pool_stats m_borrower_pool_stats_process_wide_total;

  /// Global stats from: `static` Shm_session::borrower_pool_stats_process_wide(): the per-arena out-arg.
  std::vector<arena_lend::stat::Borrower_pool_stats> m_borrower_pool_stats_process_wide_per_arena;

  /// Global stats from: `static` Shm_session::borrower_pool_lookup_global_stats().
  arena_lend::stat::Borrower_pool_lookup_global_stats m_borrower_pool_lookup_global_stats;

  /// Global info from: `static` Shm_session::shm_pool_live_info().
  std::vector<arena_lend::stat::Shm_pool_info> m_borrowed_shm_pool_live_info;
}; // struct Shm_session_info_dump

} // namespace ipc::shm::arena_lend::jemalloc::stat
