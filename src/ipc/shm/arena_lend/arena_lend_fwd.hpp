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

#include "ipc/util/shared_name_fwd.hpp"
#include <flow/log/log_fwd.hpp>
#include <flow/util/action_registry.hpp>
#include <sys/types.h>
#include <cstddef>
#include <string>
#include <memory>

/**
 * ipc::shm sub-module providing the SHM-provider family based on the *arena-lending* paradigm: each process
 * allocates in its own arena(s), lending the resulting objects to other processes, which can read them but not
 * allocate (nor deallocate) in that arena.  As of this writing there is one arena-lending SHM-provider available
 * out-of-the-box, SHM-jemalloc (see sub-namespace arena_lend::jemalloc).  The present namespace contains
 * items applicable to *any* arena-lending provider, with provider-specific items in such sub-namespaces.
 *
 * @see ipc::shm doc header for an overview of the available SHM-provider families and how they compare.
 */
namespace ipc::shm::arena_lend
{

// Types.

// Find doc headers near the bodies of these compound types.

template<typename Repository_type>
class Borrower_allocator_arena;

template<typename Pointed_type, typename Repository_type,
         typename Difference_type = int, bool CAN_STORE_RAW_PTR = true>
class Shm_pool_offset_ptr;

class Borrower_shm_pool_collection;
class Divisible_shm_pool;
class Memory_manager;
class Owner_shm_pool_collection;
class Owner_shm_pool_listener;
class Shm_pool;
class Shm_pool_collection;
struct Set_logger_tag;

/**
 * Determines the `shared_ptr` variety returned by `A::construct()` and `S::borrow_object()` for any
 * arena-lending SHM-provider, where for a given SHM-provider `A` is the arena type
 * (SHM-jemalloc: jemalloc::Ipc_arena), `S` is the SHM-session type (SHM-jemalloc: jemalloc::Shm_session).
 *
 * ### Opportunistic background ###
 * For a given SHM-provider, those 2 APIs are the only sources of *first-class* a/k/a *outer* object handles.
 * An outer object is by definition one constructed by an arena `A` or IPC-transmitted and re-obtained borrower-side
 * via SHM-session `S`.  The `a.construct()` call involves at least allocating, in SHM, `sizeof(T)` bytes;
 * further *inner* allocations are possible if `T` uses the proper SHM-capable allocator
 * (we recommend stl::Stateless_allocator); for example if `T` is `vector` then the `vector` ctor invocation
 * during `a.construct()` might perform an inner allocation, as might a subsequent `.resize()`.
 *
 * @tparam T
 *         Outer object type.
 */
template<typename T>
using Obj_handle = std::shared_ptr<T>;

/// Short-hand for util::Shared_name; used in particular for SHM pool names at least.
using Shared_name = util::Shared_name;

/**
 * Alias for an identifier of the owner (essentially namespace of `collection_id_t`s) of shared information.
 *
 * @internal
 *
 * @todo Consider moving `owner_id_t` into detail/.
 */
using owner_id_t = util::process_id_t;

/**
 * Identifier type for a shared memory pool collection.
 * @todo Determine proper size of identifier.
 *
 * @internal
 *
 * @todo Consider moving `collection_id_t` into detail/.
 */
using collection_id_t = uint32_t;

/**
 * The Action_registry that fans out `set_logger()` calls to all SHM-arena-lend singletons.
 *
 * The end user is likely to not need to directly reference this: use arena_lend::set_logger() free function.
 */
using Set_logger_registry = flow::util::Action_registry<Set_logger_tag, flow::log::Logger*>;

// Free functions.

/**
 * Convenience function that sets (or clears) the `Logger` used by all SHM-arena-lend
 * singleton subsystems.
 *
 * Typical usage brackets `main()`:
 *
 *   ~~~
 *   ipc::shm::arena_lend::set_logger(&my_logger); // startup
 *   // ... application runs ...
 *   ipc::shm::arena_lend::set_logger(nullptr);    // teardown (before Logger is destroyed)
 *   ~~~
 *
 * @param logger_ptr
 *        Pointer to the Logger to use, or null to disable logging.
 */
void set_logger(flow::log::Logger* logger_ptr);

} // namespace ipc::shm::arena_lend

/// Stats-related sub-namespace, for ADL segregation and general organization.
namespace ipc::shm::arena_lend::stat
{

// Types.

// Find doc headers near the bodies of these compound functions.

struct Uniq_arena_id;
struct Owner_obj_stats;
struct Zombie_obj_reaper_stats;
struct Sharded_stats;
struct Obj_db_aux_pool_stats;
struct Obj_db_aux_pool_global_stats;
struct Owner_pool_stats;
struct Pool_stats;
struct Owner_pool_lookup_global_stats;
struct Borrower_pool_stats;
struct Borrower_pool_lookup_global_stats;
struct Shm_pool_info;
struct Memory_manager_stats;

// Free functions.

/**
 * Declares the stats for Owner_obj_stats.  Not invoked directly except by `flow::util::stat` internals,
 * or when composing this stat-set into another.
 * @see `flow::util::stat` namespace doc header for background on the declare/visit mechanism.
 *
 * @tparam Visitor
 *         See above.
 * @param name_prefix
 *        See above.
 * @param src_stats
 *        See above.
 * @param target_stats
 *        See above.
 * @param visitor
 *        See above.
 */
template<typename Visitor>
void declare_stats(std::string name_prefix, const Owner_obj_stats* src_stats, Owner_obj_stats* target_stats,
                   Visitor&& visitor);

/**
 * Declares the stats for Zombie_obj_reaper_stats.  Not invoked directly except by `flow::util::stat` internals,
 * or when composing this stat-set into another.
 * @see `flow::util::stat` namespace doc header for background on the declare/visit mechanism.
 *
 * @tparam Visitor
 *         See above.
 * @param name_prefix
 *        See above.
 * @param src_stats
 *        See above.
 * @param target_stats
 *        See above.
 * @param visitor
 *        See above.
 */
template<typename Visitor>
void declare_stats(std::string name_prefix,
                   const Zombie_obj_reaper_stats* src_stats, Zombie_obj_reaper_stats* target_stats, Visitor&& visitor);

/**
 * Declares the stats for Sharded_stats.  Not invoked directly except by `flow::util::stat` internals,
 * or when composing this stat-set into another.
 * @see `flow::util::stat` namespace doc header for background on the declare/visit mechanism.
 *
 * @tparam Visitor
 *         See above.
 * @param name_prefix
 *        See above.
 * @param src_stats
 *        See above.
 * @param target_stats
 *        See above.
 * @param visitor
 *        See above.
 */
template<typename Visitor>
void declare_stats(std::string name_prefix,
                   const Sharded_stats* src_stats, Sharded_stats* target_stats, Visitor&& visitor);

/**
 * Declares the stats for Obj_db_aux_pool_stats.  Not invoked directly except by `flow::util::stat` internals,
 * or when composing this stat-set into another.
 * @see `flow::util::stat` namespace doc header for background on the declare/visit mechanism.
 *
 * @tparam Visitor
 *         See above.
 * @param name_prefix
 *        See above.
 * @param src_stats
 *        See above.
 * @param target_stats
 *        See above.
 * @param visitor
 *        See above.
 */
template<typename Visitor>
void declare_stats(std::string name_prefix,
                   const Obj_db_aux_pool_stats* src_stats, Obj_db_aux_pool_stats* target_stats, Visitor&& visitor);

/**
 * Declares the stats for Obj_db_aux_pool_global_stats.  Not invoked directly except by `flow::util::stat` internals,
 * or when composing this stat-set into another.
 * @see `flow::util::stat` namespace doc header for background on the declare/visit mechanism.
 *
 * @tparam Visitor
 *         See above.
 * @param name_prefix
 *        See above.
 * @param src_stats
 *        See above.
 * @param target_stats
 *        See above.
 * @param visitor
 *        See above.
 */
template<typename Visitor>
void declare_stats(std::string name_prefix,
                   const Obj_db_aux_pool_global_stats* src_stats, Obj_db_aux_pool_global_stats* target_stats,
                   Visitor&& visitor);

/**
 * Declares the stats for Owner_pool_stats.  Not invoked directly except by `flow::util::stat` internals,
 * or when composing this stat-set into another.
 * @see `flow::util::stat` namespace doc header for background on the declare/visit mechanism.
 *
 * @tparam Visitor
 *         See above.
 * @param name_prefix
 *        See above.
 * @param src_stats
 *        See above.
 * @param target_stats
 *        See above.
 * @param visitor
 *        See above.
 */
template<typename Visitor>
void declare_stats(std::string name_prefix,
                   const Owner_pool_stats* src_stats, Owner_pool_stats* target_stats, Visitor&& visitor);

/**
 * Declares the stats for Pool_stats.  Not invoked directly except by `flow::util::stat` internals,
 * or when composing this stat-set into another.
 * @see `flow::util::stat` namespace doc header for background on the declare/visit mechanism.
 *
 * @tparam Visitor
 *         See above.
 * @param name_prefix
 *        See above.
 * @param src_stats
 *        See above.
 * @param target_stats
 *        See above.
 * @param visitor
 *        See above.
 */
template<typename Visitor>
void declare_stats(std::string name_prefix,
                   const Pool_stats* src_stats, Pool_stats* target_stats, Visitor&& visitor);

/**
 * Declares the stats for Borrower_pool_lookup_global_stats.  Not invoked directly except by `flow::util::stat`
 * internals, or when composing this stat-set into another.
 * @see `flow::util::stat` namespace doc header for background on the declare/visit mechanism.
 *
 * @tparam Visitor
 *         See above.
 * @param name_prefix
 *        See above.
 * @param src_stats
 *        See above.
 * @param target_stats
 *        See above.
 * @param visitor
 *        See above.
 */
template<typename Visitor>
void declare_stats(std::string name_prefix,
                   const Owner_pool_lookup_global_stats* src_stats, Owner_pool_lookup_global_stats* target_stats,
                   Visitor&& visitor);

/**
 * Declares the stats for Borrower_pool_stats.  Not invoked directly except by `flow::util::stat` internals,
 * or when composing this stat-set into another.
 * @see `flow::util::stat` namespace doc header for background on the declare/visit mechanism.
 *
 * @tparam Visitor
 *         See above.
 * @param name_prefix
 *        See above.
 * @param src_stats
 *        See above.
 * @param target_stats
 *        See above.
 * @param visitor
 *        See above.
 */
template<typename Visitor>
void declare_stats(std::string name_prefix,
                   const Borrower_pool_stats* src_stats, Borrower_pool_stats* target_stats, Visitor&& visitor);

/**
 * Declares the stats for Borrower_pool_lookup_global_stats.  Not invoked directly except by `flow::util::stat`
 * internals, or when composing this stat-set into another.
 * @see `flow::util::stat` namespace doc header for background on the declare/visit mechanism.
 *
 * @tparam Visitor
 *         See above.
 * @param name_prefix
 *        See above.
 * @param src_stats
 *        See above.
 * @param target_stats
 *        See above.
 * @param visitor
 *        See above.
 */
template<typename Visitor>
void declare_stats(std::string name_prefix,
                   const Borrower_pool_lookup_global_stats* src_stats, Borrower_pool_lookup_global_stats* target_stats,
                   Visitor&& visitor);

/**
 * Declares the fields for Shm_pool_info, intended for use with `stat::print()`.
 * @see Shm_pool_info doc header which explains how it is a `Stat_set` but not in the classic sense.
 * @see `flow::util::stat` namespace doc header for background on the declare/visit mechanism.
 *
 * @tparam Visitor
 *         See above.
 * @param name_prefix
 *        See above.
 * @param src_stats
 *        See above.
 * @param target_stats
 *        See above.
 * @param visitor
 *        See above.
 */
template<typename Visitor>
void declare_stats(std::string name_prefix,
                   const Shm_pool_info* src_stats, Shm_pool_info* target_stats, Visitor&& visitor);

/**
 * Declares the stats for Memory_manager_stats.  Not invoked directly except by `flow::util::stat` internals,
 * or when composing this stat-set into another.
 * @see `flow::util::stat` namespace doc header for background on the declare/visit mechanism.
 *
 * @tparam Visitor
 *         See above.
 * @param name_prefix
 *        See above.
 * @param src_stats
 *        See above.
 * @param target_stats
 *        See above.
 * @param visitor
 *        See above.
 */
template<typename Visitor>
void declare_stats(std::string name_prefix,
                   const Memory_manager_stats* src_stats, Memory_manager_stats* target_stats, Visitor&& visitor);

} // namespace ipc::shm::arena_lend::stat
