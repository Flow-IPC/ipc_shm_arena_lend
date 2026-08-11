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

#include "ipc/shm/arena_lend/arena_lend_fwd.hpp"
#include <flow/util/util_fwd.hpp>
#include <cstdint>
#include <atomic>
#include <ostream>
#include <memory>
#include <utility>
#include <string>

namespace ipc::shm::arena_lend::detail
{

// Types.

// Find doc headers near the bodies of these compound types.

class Use_count_registry;
class Lend_tracker_pool;
class Shm_pool_offset_ptr_data_base;

template<typename Repository_type, bool CAN_STORE_RAW_PTR>
class Shm_pool_offset_ptr_data;

template<typename Shm_arena_t>
class Thread_lcl_obj_db_admin;
template<typename Shm_arena_t>
class Thread_lcl_obj_db_client;

struct Pool_by_base;
template<typename Shm_arena_t, bool OWNER_ELSE_BORROWER>
class Pool_lookup_global_stats;
template<typename Repository_t, typename Shm_arena_t>
class Thread_local_pool_lookup_fwd;
template<typename Repository_t, bool LOOKUP_CAN_FAIL, typename Shm_arena_t>
class Thread_local_pool_lookup_rev;

template<typename Repository_t, bool LOOKUP_CAN_FAIL, typename Shm_arena_t>
class Shm_pool_repo_lookup_core_rev;

template<typename Shm_arena_t>
class Owner_shm_pool_repository;

template<typename Base_t>
struct Owner_spc_impl;

/**
 * Globally unique (up to reboot) arena (collection) ID.  `owner_id_t` (PID) is globally unique w/r/t owner
 * processes, while `collection_id_t` ordinally identifies an arena for a given owner process (1, 2, ...).
 */
using Uniq_collection_id = std::pair<owner_id_t, collection_id_t>;

/**
 * Pool ID type.  Outside of Shm_pool_offset_ptr_data you must use Shm_pool_offset_ptr_data_base::generate_pool_id()
 * to generate new ones.
 *
 * ### Impl: Why `uint32_t` was chosen ###
 * Per Shm_pool_offset_ptr_data doc header impl discussion, it must fit into Shm_pool_offset_ptr_data_base::m_rep_t in
 * addition to 1 selector bit and Shm_pool_offset_ptr_data_base::pool_offset_t.  We've chosen 32 LSB of 64-bit
 * `rep_t` to be `pool_offset_t`; so that leaves 31 bits; 32 bits is the smallest type available to hold a 31-bit
 * unsigned number.
 */
using pool_id_t = uint32_t;

/**
 * Index into a given Lend_tracker_pool's bulk storage of arena-`construct()`ed (outer a/k/a first-class)
 * in-SHM objects.
 *
 * ### Context info ###
 * If one has an aux Lend_tracker_pool handle opened, one only needs its #pool_id_t (pool ID)
 * and #use_ct_idx_t to know which atomic integer in memory to increment (session `lend_object()`) or decrement
 * (`shared_ptr` disposers) or initialize to 1 (arena `construct()`).
 *
 * If the given aux pool handle has not yet been opened on-demand, then a #collection_id_t (arena/collection ID) is
 * also required, so as to look up that arena's *pool name base* to which to append pool ID to get the pool name.
 * As for a given arena's pool name base, from SHM-arena-lending SHM-provider module's PoV it comes form the user
 * when issuing `Arena::create()` (e.g.: jemalloc::Ipc_arena::create()).  ipc::session::shm::arena_lend
 * general `ipc::session`-compliant module forms this on the user's behalf internally, if they (as encouraged)
 * choose to use that module instead of using SHM-jemalloc/etc. in standalone fashion.  `ipc::session` forms
 * those names with hierarchy and easy on-crash cleanup in mind.
 */
using use_ct_idx_t = uint32_t;

// Free functions.

/**
 * (Roughly) the borrower-side counterpart of Owner_obj_disposer_and_mdt, this returns a borrower-side
 * handle `shared_ptr<T>` whose disposer is the borrower-side counterpart to Owner_obj_disposer_and_mdt.
 *
 * Also: keeps `shm_session_to_keep_alive` alive until disposer executes (~analogously to Owner_obj_disposer_and_mdt
 * doing so w/r/t the constructing `Shm_arena`).  (Rationale is similar; the machinery tracking borrowed objects
 * via session so-and-so is maintained, at least partially, in so-and-so borrower-side `Shm_session`).
 *
 * This is a key helper for `Shm_session::borrow_object()`, returning the very disposer-furnished handle
 * that method must return.  The caller must first obtain the borrower-side vaddr of the in-SHM object (`addr`)
 * as well as certain values (all originating in the opposing process; see Owner_obj_disposer_and_mdt)
 * required for the disposer we furnish to act properly.  Namely that disposer will simply decrement a single
 * in-SHM atomic ref-count via Lend_tracker_pool; the saved args like `use_ct_idx` are needed to identify the
 * location of this atomic.
 *
 * @note One might assume that the `--` is in the disposer, so the `++` is inside this free function.  Not so:
 *       The `++` occurred in fact in the *opposing* process's earlier `Shm_session::lend_object()` call which,
 *       incidentally, returned the blob that encoded the values that *our* side just decoded again into
 *       the args to our function here.  Otherwise the object might be destroyed before lending process was
 *       able to transmit said blob to us and/or before this function call.  The key point:
 *       Calling `blob_here = sess_here->lend_object(p_here)` in P1 is part of 3-part procedure; part 2 is
 *       IPC-transmitting `blob_here` to P2 a `blob_there`; part 3 is
 *       `auto p_there = sess_there->borrow_object(blob_there)`, which calls us upon decoding `blob_there`.
 *
 * @tparam T
 *         In-SHM object type.
 * @tparam Shm_session
 *         See above.
 * @param addr
 *        Borrower-side vaddr of the in-SHM object.
 * @param lend_tracker_pool_id
 *        See Owner_obj_disposer_and_mdt data member.
 * @param use_ct_idx
 *        See Owner_obj_disposer_and_mdt data member.
 * @param owner_id
 *        Globally unique ID (as of this writing simply PID) of *owner* process of `addr`-pointed in-SHM object.
 *        That process is the one that did `arena->construct<T>(...)` originally.
 *        As of this writing that isn't transmitted on a per-borrowed-handle basis but rather available
 *        in the `Shm_session` at large (we know the PID of the guy with whom we converse).
 * @param collection_id
 *        Given `owner_id`, unique arena-ID of the *owner* `Shm_arena` (e.g. jemalloc::Ipc_arena).
 *        (Together `owner_id` + `collection_id`, sometimes known as #Uniq_collection_id, is a globally unique
 *        identifier of that `Shm_arena`.)
 * @param shm_session_to_keep_alive
 *        As it says!  The arg itself is nullified.
 *        As of this writing we merely keep it alive; we don't modify it (nor even need to query it).
 * @return Borrower-side ref-counted handle to `addr`-pointed in-SHM object whose disposer shall participate in
 *         the *cross-process garbage collection* of that object.
 */
template<typename T, typename Shm_session>
Obj_handle<T> construct_with_borrower_obj_disposer(T* addr,
                                                   pool_id_t lend_tracker_pool_id,
                                                   use_ct_idx_t use_ct_idx,
                                                   owner_id_t owner_id,
                                                   collection_id_t collection_id,
                                                   std::shared_ptr<const Shm_session>&& shm_session_to_keep_alive);

/**
 * Prints string representation of the given `Shm_pool_offset_ptr_data` to the given `ostream`.
 *
 * @relatesalso Shm_pool_offset_ptr_data
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Repository_type, bool CAN_STORE_RAW_PTR>
std::ostream& operator<<(std::ostream& os,
                         Shm_pool_offset_ptr_data<Repository_type, CAN_STORE_RAW_PTR> val);

/**
 * Prints string representation of the given `Lend_tracker_pool` to the given `ostream`.
 *
 * @relatesalso Lend_tracker_pool
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Lend_tracker_pool& val);

} // namespace ipc::shm::arena_lend::detail

/// Stats-related sub-namespace, for ADL segregation and general organization.
namespace ipc::shm::arena_lend::detail::stat
{

// Types.

// Find doc headers near the bodies of these compound functions.

/// Short-hand for brevity: `Thread_lcl_obj_db_*` and buddies make frequent mention of this.
using Sharded_stats = arena_lend::stat::Sharded_stats;

template<typename Shm_arena>
class Finalized_shards;

// Free functions.

/**
 * Assembles and outputs the current stats tracked by the obj-DB module (as anchored by Thread_lcl_obj_db_admin).
 * This is the main stat-consumption API for the obj-DB module, as required by the `Shm_arena`
 * (e.g., jemalloc::Ipc_arena) stat-consumption API's impl.  Since the obj-DB module (again, as anchored
 * by Thread_lcl_obj_db_admin and further aided by Thread_lcl_obj_db_client) is a highly central engine
 * to the functioning of an arena-lend-type SHM-provider (e.g., SHM-jemalloc), many important stats are
 * tracked within the compiled-here `Stat_set`, namely Sharded_stats.
 *
 * @see Sharded_stats doc header (and relevant stat-member doc headers) to get a bird's-eye-view of
 *      the kinds of stats included here (and many details if desired).
 *
 * ### How to use this ###
 * `Shm_arena` should store a Sharded_stats, initially default-cted.
 * Suppose the `Shm_arena` user requests Sharded_stats (or some other grouping of stat-sets which
 * includes the items in Sharded_stats; but let's assume not for exposition simplicity; it does not
 * change the point).  Call this free function, targeting the stored stat-set; then return it, probably, by value
 * (`stats_assign()`).
 *
 * ### Impl notes for context ###
 * Briefly: the obj-DB module is essentially thread-locally implemented (hence the names like
 * Sharded_stats).  As such, for stat-keeping performance, stats are TL-sharded (per
 * design explained in `flow::util::stat` doc header).  This free function:
 *   - locks the various sources of Sharded_stats shards, comprising as of this writing
 *     Thread_lcl_obj_db_admin, Thread_lcl_obj_db_client, and Finalized_shards;
 *   - aggregates all those shards via `flow::util::stat::stats_aggregate_shards()` onto
 *     `*target_stats`.
 *     - Reminder: The current values in `*target_stats` do -- in some specific cases -- matter; it is an out-arg, yes,
 *       but also partially an in-arg.  The "some specific cases" are: any `HI_WMARK` stat-members.
 *       See `flow::util::stat::stats_aggregate_shards()` doc header for all the deets on that.
 *       Bottom line: caller shall remember to keep `*target_stats` for next time this free function
 *       is called for the same arena.
 *
 * @param shm_arena
 *        See, e.g., Thread_lcl_obj_db_admin::constructing_obj().
 * @param target_stats
 *        See above.
 */
template<typename Shm_arena>
void sharded_stats(const Shm_arena& shm_arena, Sharded_stats* target_stats);

/**
 * Resets Sharded_stats; companion to sharded_stats().
 *
 * ### How to use this ###
 * See sharded_stats() doc header eponymous section.  If the `Shm_arena` user requests a reset,
 * call this free function on the stored stat-set.
 *
 * ### Impl notes for context ###
 * It is similar to sharded_stats(); just basically instead of `flow::util::stat::stats_aggregate_shards()`
 * it uses `stats_reset_shard_aggregate()`.
 *
 * @param shm_arena
 *        See above.
 * @param target_stats
 *        See above.
 */
template<typename Shm_arena>
void sharded_stats_reset(const Shm_arena& shm_arena, Sharded_stats* target_stats);

/**
 * Helper: implements sharded_stats() or sharded_stats_reset().
 *
 * @tparam CONSUME_ELSE_RESET
 *         Whether implementing sharded_stats() (`true`) or sharded_stats_reset().
 * @param shm_arena
 *        See above.
 * @param target_stats
 *        See above.
 */
template<bool CONSUME_ELSE_RESET, typename Shm_arena>
void sharded_stats_impl(const Shm_arena& shm_arena, Sharded_stats* target_stats);

/**
 * (Helper for sharded_stats_impl() and `Thread_lcl_obj_db_admin/client`) Process-global, immortal recursive mutex
 * serializing obj-DB stat-consume/reset against `_admin` + `_client` thread exit.  It eliminates The Gap during
 * thread exit: the brief window in which a going-away `_admin` or `_client` has been removed
 * from its `Thread_local_state_registry` but has not yet deposited its stat-shard into either Finalized_shards
 * or a new drain-mode `_admin` in a replacement thread.
 *
 * ### Premise: The Gap ###
 * This is best observed in ~Thread_lcl_obj_db_client() and ~Thread_lcl_obj_db_admin() dtors; the former is
 * simpler as an intro to the topic.  In either place, you'll see that each (per-arena) Sharded_stats still
 * in the `TLODB...` does not simply disappear but rather needs to be saved elsewhere: either into Finalized_shards
 * (`_client` case; `_admin` case where no degraded-mode `_admin` thread/object is required) or into
 * a new `_admin` (`_admin` degraded-mode case).  Before The Gap such a shard would be -- upon locking certain
 * other central mutexes -- included in the sharded_stats_impl() range-walk through all extant TL-shards:
 * After The Gap: same (either in an `_admin` or in Finalized_shards).  *During* The Gap, however, the
 * extant-TL-state registry (`Thread_local_state_registry<_admin>` or `Thread_local_state_registry<_client>`) has
 * ejected the `_admin` or `_client` but either has not yet reinserted the replacement `_admin` therein, or has done
 * so without the shard being transferred-over into that `_admin` yet; nor have we yet inserted it into
 * Finalized_shards.
 *
 * Bad things about these stat-ops happening during The Gap:
 *   - sharded_stats() will yield incomplete `*target_stats` (each ACC and GAUGE may be missing 1+ addends),
 *     and HI_WMARKs might miss peaks.
 *     - If sharded_stats() was called merely to update HI_WMARKs (e.g.: Ipc_arena::sample_hi_wmarks()), then
 *       it may update them incorrectly.
 *   - sharded_stats_reset() may fail to reset ACCs, and HI_WMARKs might be wrong subsequently.
 *   - Explaining to the user what they must avoid in order to avoid such things is not that easy.
 *     User thread exit may be not too bad for them to control; but the degraded-mode (drain) `_admin` threads
 *     are an internal thing, so even having to mention it is a communication ordeal.
 *   - As of this writing SHM-jemalloc ipc::session machinery auto-tickles Ipc_arena::sample_hi_wmarks() regularly;
 *     if that were to happen during a relevant thread-exit (thus a Gap), then there can be HWM-correctness trouble.
 *
 * All in all, it is not the crime of the century, but it is not good.  Better is to lack any such caveats.
 *
 * ### Basic solution ###
 * Setting aside the mechanism we're about to describe, consider what ops are involved and when.
 *   - sharded_stats_impl() (stat-consume/reset/HWM-tickle).  Bracketed by 3 mutex-locks: `_admin`-registry's,
 *     `_client`-registry's, Finalized_shards singleton's.
 *   - `_admin` thread-exit: `_admin`-registry lock (by `Thread_local_state_registry` code), removal from registry,
 *     unlock, `~_admin()` dtor.  Inside there: either:
 *     - Finalized_shards lock, insert of Sharded_stats into it, unlock; exit dtor; or:
 *     - spawn drain-thread, `_admin`-registry lock, construct `_admin` and add to registry, unlock;
 *       shove the Sharded_stats into the new `_admin` in drain-thread; once signalled that is complete: exit dtor.
 *   - `_client` thread-exit: `_client`-registry lock (by `Thread_local_state_registry` code), removal from registry,
 *     unlock, `~_client()` dtor.  Inside there:
 *     - Finalized_shards lock, insert of Sharded_stats into it, unlock; exit dtor.
 *
 * The "whens": sharded_stats_impl() -- anytime; the `_admin` teardown -- 1x per thread at most; `_client` ditto.
 * (The latter two can *both* happen in a given thread; but actually fully in-series within that thread; there is
 * no interleaving.)
 *
 * @note The "1x per thread" is per `Shm_arena` type.  So generally speaking there can be more, if SHM-jemalloc were
 *       to be used simultaneously with another SHM-arena-lend provider.
 *
 * The trouble occurs with bullet 2 and bullet 3: after registry-unlock, before "exit dtor."  (`~admin()` in the
 * spawns-new-thread case happens to exit dtor specifically just after new-thread-`_admin` code has signalled
 * it has moved all its state from dying-`_admin`.  The other 2 "exit dtor" eventualities are simpler.  In all cases,
 * though, "exit dtor" is when The Gap has definitely passed.)  So the challenge is to avoid bullet 1
 * between the 1 `_admin`'s (A) registry-lock-at-thread-teardown and (B) its "exit dtor" point; and ditto
 * for the 1 `_client`.
 *
 * So: Use a global mutex (that is us) around each bullet above.  The existing locks "almost" do it but not actually:
 * in bullets 2 and 3 they are active only at the start, but then by the time each dtor starts -- no longer so.
 * As long as this mutex is locked before existing first lock and unlocked after existing last unlock: solved.
 *
 * ### Refinement: `at_thread_exit()` ###
 * It would be easy-peasy, if each bullet above were a linear sequence of statements.  That is not the case
 * however: `Thread_local_state_registry` controls bullet 2 and bullet 3 and launches each dtor after
 * lock/remove/unlock.  (`sharded_stats_impl()` is all us though, so no problem there.)  Locking this mutex
 * in `Thread_local_state_registry<T>` cleanup would require some kind of hook API in addition to the existing
 * straightforward execution of `~T()`.  A possibility -- but we went another way.  It is haxory, but we have
 * a bespoke challenge which we resolve with a bespoke technique instead of modifying the TL-utility with an
 * awkward hook.
 *
 * `Thread_local_state_registry` ultimately is explicitly executing all cleanup (our dtors in this case) off
 * `boost::thread_specific_ptr` per-thread cleanup phase.  `boost::this_thread::at_thread_exit(F)` specifically
 * allows one to execute `F()` (and *all* such registered functions) during thread shutdown (yes, with or without
 * a backing `boost::thread`; `std::thread` => still yes) and specifically before the TL-ptr cleanups.
 *
 * Call our mutex here `M`.  Stipulate that when an `_admin` or `_client` is (per-thread) created, it registers
 * the following to execute `at_thread_exit()`: `M.lock()`.  Stipulate further that:
 *   - sharded_stats_impl() is bracketed with lock/unlock of `M`.
 *   - `~admin()` and `~client()` each does `M.unlock()` at "exit dtor" point.
 *
 * So the order is:
 *   - Sometime during thread-proper: Register `at_thread_exit(F)` (see above).
 *   - Thread exit begins.
 *   - `boost::thread` machinery executes the at-thread-exit `F()`s.  So, in some arbitrary order:
 *     - 1 `M.lock()` for `_admin<Arena_type1>`.
 *     - 1 `M.lock()` for `_client<Arena_type1>`.
 *     - 1 `M.lock()` for `_admin<Arena_type2>`.
 *     - 1 `M.lock()` for `_client<Arena_type2>`.
 *     - ...
 *  - Boost machinery executes the `tsp` cleanups.  For us: in some arbitrary order:
 *     - For `_admin<Arena_type1>`:
 *       - `Thread_local_state_registry<Arena_type1>` locks registry, removes, unlocks.  Gap begins here.
 *       - It calls `~_admin()`.   Gap continues, until the shards are parked where they must during this dtor.
 *       - Last thing `~_admin()` does: `M.unlock()`.  Gap ends here.
 *     - For `_client<Arena_type1>`: Ditto.
 *     - For `_admin<Arena_type2>`: Ditto.
 *     - For `_client<Arena_type2>`: Ditto.
 *     - ...
 *
 * The refinement we were building up to is this: By using `at_thread_exit()` we caused all the `M.lock()`s
 * to happen first and the `M.unlock()`s second.  So we aren't cleanly bracketing each Gap.  That is OK:
 *   - Ensure `M` is recursive.  Now `M` will be held from the first `M.lock()` to the last `M.unlock()`.
 *     - The Gaps, in a sense, merge into one MegaGap; and there's no deadlock at the 2nd `M.lock()`.
 *   - Ensure `M` spans *all* `Shm_arena` types, as opposed to maintaining an `M<Shm_arena>` per `Shm_arena`.
 *     Otherwise there can be trouble given the arbitrary order as noted above; 2 threads exiting concurrently may
 *     do M1-lock/M2-lock and M2-lock/M1-lock (standard deadlock).
 *
 * @warning Both of these, normally, would be red flags in my (ygoldfel) opinion/experience.  First, recursive mutexes
 *          tend to paper-over some design corner-cutting.  Second, every single other mechanism as of this writing
 *          segregates one SHM-provider from another -- but not here.  Yet we do both things here, and both things
 *          have one original sin causing them: The `.lock()` is decoupled from `.unlock()`.  Given that we've
 *          decided not to invade `Thread_local_state_registry` cleanup code/add hooks to it, instead using
 *          a more-global Boost hook, we have no choice but to deal with it accordingly.
 *
 * Okay: so given the above, if sharded_stats_impl() happens to execute during all this, it will either complete
 * before the first `M.lock()` above or pause and actually-execute after the last `M.unlock()` above.
 *
 * @return Reference to the singleton mutex.  The pointee will outlive all C++ code.
 */
flow::util::Mutex_recursive& thread_end_gap_mutex();

} // namespace ipc::shm::arena_lend::detail::stat
