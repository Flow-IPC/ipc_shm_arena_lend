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

#include "ipc/shm/arena_lend/detail/lend_tracker_pool.hpp"
#include "ipc/shm/shm_stats.hpp"
#include <flow/util/stat/histo.hpp>
#include <flow/util/stat/stat_set.hpp>
#include <cstdint>
#include <string>
#include <type_traits>

namespace ipc::shm::arena_lend::stat
{

// Types.

/**
 * Similar mission to shm::stat::Owner_obj_stats (which is relevant regardless of SHM-provider type);
 * adds items relevant to arena-lending SHM-providers only.
 */
struct Owner_obj_stats
{
  // Data.

  /**
   * Of the first-class object deletions counted in shm::stat::Owner_obj_stats::m_destroy_count, count
   * of the ones that occurred synchronously/immediately upon the `shared_ptr` disposer executing.
   *
   * To increment this, the required sequence of events is as follows: `arena.construct<T>()`-returned
   * `shared_ptr` group's ref-count reached zero; disposer executed; this happened to be the last
   * process holding a reference to object (no borrowers also holding at that time); and this happened
   * to occur in the same thread as the original `arena.construct()`ing thread.
   *
   * @note Let S = shm::stat::Owner_obj_stats::m_destroy_count, A = #m_sync_destroy_count, and
   *       B = #m_drain_destroy_count.  Then (modulo concurrency artifacts):
   *       C = (S - A - B) = the following (derived) stat:
   *       Of the first-class object deletions counted as S, count of the ones such that (1) the
   *       first-class object `arena.construct()`ed in user thread U1 had its last cross-process reference
   *       dropped in thread U2; and then (2) the object was destroyed asynchronously during
   *       a subsequent opportunistic (a/k/a piggy-backed) *zombie-reaper scan* in U1.
   */
  std::atomic<uint64_t> m_sync_destroy_count = 0;

  /**
   * Of the first-class object deletions counted in shm::stat::Owner_obj_stats::m_destroy_count, count
   * of the ones that occurred in a specialized drain-thread that sprang up via SHM-jemalloc when
   * an `arena.construct()`ing user thread exited with live objects extant.  (Note that such a thread-exit
   * is not an error: it is perfectly legal for a thread to `arena.construct()` and then later join its parent,
   * a/k/a exit, while the earlier-`construct()`ed object is still live, presumably in-use by other
   * threads/processes.)
   *
   * To increment this, the required sequence of events is as follows: (see previous paragraph); then:
   * a user thread, in any process, dropped the last `shared_ptr` (whether `.construct()`ed or
   * `.borrow_object()`ed) group referencing the first-class object; the detached drain-thread (controlled by
   * SHM-jemalloc) woke up, noticed the now-zombie object; and therefore destroyed it.
   */
  std::atomic<uint64_t> m_drain_destroy_count = 0;
}; // struct Owner_obj_stats

/**
 * Cousin of arena_lend::Owner_obj_stats, this tracks the *zombie-reaper scan* algorithm whose task
 * is to find all first-class objects `arena.construct()`ed by this thread (or by the now-joined thread whose
 * exit caused this specialized drain-thread to be spawned) that are *zombie objects*; and to destroy them.
 * A zombie object is one such that no user thread, cross-process, holds a reference to it any longer.
 *
 * ### Context: How it works ###
 * Despite the scary name, a zombie object is a completely normal phenomenon -- it just means
 * the last guy to let go of its ref to the object happened to not be the original `.construct()`ing
 * thread-and-process.  Only that original thread may destroy such an object, and since we do not
 * control user threads, we can only do so opportunistically (a/k/a *piggy-backed*).  If that (user) thread happens to
 * exit (join its parent) while an object is still live, then a *specialized drain-thread* is launched at that point
 * by SHM-jemalloc and will periodically execute the zombie-reaping algorithm in the same way as
 * the piggy-backed scan.
 *
 * There are therefore two of Zombie_obj_reaper_stats per arena: one counting the piggy-backed scans' stats;
 * the other counting the dragin-threads' scans' stats.
 *
 * @internal
 * The piggy-backed code path is detail::Thread_lcl_obj_db_admin::this_thread_piggy_scan().
 * (Additionally, up to once per `.construct()`ing thread, detail::Thread_lcl_obj_db_admin::~Thread_lcl_obj_db_admin()
 * dtor launches a scan.  It is counted together with the piggy-backed scans.)
 *
 * The drain-thread code path is detail::Thread_lcl_obj_db_admin::degraded_admin_thread_body().
 */
struct Zombie_obj_reaper_stats
{
  // Types.

  /**
   * Groups stats that count events any of which can occur 0+ -- not a maximum of one time -- per scan.
   *
   * @internal
   * The `ATOMIC_OR_NOT` parameterization helps in a certain impl detail; we use some local batching to
   * speed-up stats-updates, at which point the temporary local Events instance is faster as just storing
   * regular values, as no other thread will ever see them.
   * @endinternal
   *
   * @tparam ATOMIC_OR_NOT
   *         If `true`, each counter is `atomic<T>`; else just `T`, where `T` is whatever appropriate integer type.
   */
  template<bool ATOMIC_OR_NOT = true>
  struct Events
  {
    // Constants

    /// The template parameter's value.
    static constexpr bool S_ATOMIC_OR_NOT = ATOMIC_OR_NOT;

    // Types.

    /// Used for all ACCUMULATORs in a `*this`.
    using Counter = std::conditional_t<S_ATOMIC_OR_NOT, std::atomic<uint64_t>, uint64_t>;

    // Data.

    /**
     * The number of single-object hints scanned (each of which may or may not have resulted in 1 zombie-object
     * being found and therefore reaped).
     *
     * @see Zombie_obj_reaper_stats::m_hints_sufficient_scans doc header for related background.
     *
     * Modulo concurrency artifacts, one can derive the quantity
     * `m_hints_checked - m_hints_stale - m_hints_found_resurrected_zombie`.
     * This quantity is the following: Of the single-object hints counted in #m_hints_checked, count of how many
     * were indeed accurate hints (object is zombie and therefore reaped).
     */
    Counter m_hints_checked = 0;

    /**
     * Of the single-object hints counted in #m_hints_checked, count of how many resulted in the finding that
     * the indicated object was already destroyed (not a zombie; reaped earlier by this algorithm).  This phenomenon
     * is not an error but merely a consequence of concurrent events.
     */
    Counter m_hints_stale = 0;

    /**
     * Of the single-object hints counted in #m_hints_checked, count of how many resulted in the finding that
     * the indicated object was in fact not a zombie but live (1+ references are held cross-process).
     *
     * @warning (Canary stat) Based on our own understanding of the system, this count should remain zero.
     *          However, the code is written defensively, so that instead of panicking/crashing/etc.,
     *          it is tracked here.  If a user sees this stat showing non-zero, we should investigate
     *          the sequence of events that led to it.  (As of this writing we have not seen this.)
     */
    Counter m_hints_found_resurrected_zombie = 0;

    /**
     * In the (typically-unnecessary, we hope) FIFO-zombie-search phases of the zombie-reaping scans,
     * the number of objects in the obj-DB that were checked for zombie status (regardless of the answer).
     *
     * Modulo concurrency artifacts, one can derive the quantity
     * `m_fifo_objs_checked - m_fifo_objs_live`.  This quantity is the following: Of the checked-objects counted in
     * #m_fifo_objs_checked, count of those that were indeed zombies and therefore were destroyed.
     */
    Counter m_fifo_objs_checked = 0;

    /**
     * Of the checked-objects counted in #m_fifo_objs_checked, count of those that were live -- not zombies -- and
     * were thus left alone.  To be clear: it is normal for this to happen for a given object.
     */
    Counter m_fifo_objs_live = 0;
  }; // struct Events

  // Data.

  /**
   * Number of times the zombie-reaper scan algorithm executed.
   *
   * @note Does not include dead-thread "scans": This is where the scan immediately no-ops due to the following:
   *       The user thread has exited earlier; and any live objects originally constructed in that thread in the
   *       `*this`-tracked arena, that may have been around at thread exit time, have by now all been freed;
   *       therefore the thread's aux pool no longer exists: nothing to scan.
   *       (For aux-pool background: see Obj_db_aux_pool_stats doc header.)
   *
   * Modulo concurrency artifacts, one can derive the quantity
   * `m_scans - m_hints_sufficient_scans - m_fifo_scans - m_fifo_exhausted_scans`.
   * This quantity is the following: Of the scans counted in #m_scans, count of how many immediately
   * exited due to the zombie-object-count estimate N equalling zero.  This is the fast-path;
   * we hope that many -- most -- scans result in this eventuality over time.
   *
   * @note The previous paragraph does not mention `m_exhaustive_scans`.  There is however a relationship
   *       between #m_fifo_exhausted_scans and #m_exhaustive_scans.  See other doc headers nearby.
   */
  std::atomic<uint64_t> m_scans = 0;

  /**
   * Of the scans counted in #m_scans, count of how many were *exhaustive* scans; that is performed up to once
   * per constructing-thread, when that (user) thread exits.  #m_fifo_exhausted_scans shall equal this
   * value; if it exceeds it... see #m_fifo_exhausted_scans doc header for more.
   */
  std::atomic<uint64_t> m_exhaustive_scans = 0;

  /**
   * Of the scans counted in #m_scans, count of how many did *not* immediately exit due to the
   * zombie-object-count estimate N equalling zero; but that successfully reaped at least N zombies
   * based only on the initial *hint phase* of the scan.
   *
   * The *hint phase* checks a special non-user-data SHM pool (*aux pool*) for a limited number of
   * hints, each of which points at a known zombie.  This is fast, as it requires no searching
   * through the obj-DB.  However, there is a limited number of possible hints (as of this writing 16; but
   * this could change and is cited here only for ballpark-exposition purposes), so depending on
   * timing of piggy-backing and other factors this phase may not be sufficient to "get" all N zombies.
   */
  std::atomic<uint64_t> m_hints_sufficient_scans = 0;

  /**
   * Of the scans counted in #m_scans, count of how many did *not* immediately exit due to the
   * zombie-object-count estimate N equalling zero; and for which the initial hint-phase of the algorithm
   * did not successfully reap at least N zombies.
   *
   * @see #m_hints_sufficient_scans doc header for related background.
   *
   * @note For a given scan, it is not possible that #m_fifo_scans *and* #m_fifo_exhausted_scans were
   *       both incremented.  #m_fifo_scans counts only the scans where (1) the scan entered this phase
   *       and (2) during this phase reaped enough zombies as to reach N together with the hint-phase.
   *       #m_fifo_exhausted_scans counts only those where even that somehow was not enough.
   *
   * The phase after the hint-phase is called here the FIFO-scan phase; so-named because while
   * it is in a sense a brute-force search through the obj-DB -- importantly, one that stops once N
   * zombies have been reapted -- it is performed in FIFO order; that is based on chronological order
   * of `.construct()`ion.  It is arguably likely (but certainly not guaranteed) that objects become
   * zombies roughly in the same order that they came into existence as fresh live objects.
   */
  std::atomic<uint64_t> m_fifo_scans = 0;

  /**
   * Of the scans counted in #m_scans, count of how many did *not* immediately exit due to the
   * zombie-object-count estimate N equalling zero; and then ultimately failed to find at least N
   * zombies to reap.
   *
   * @warning (Canary stat) Based on our own understanding of the system, this count should remain
   *          equal to #m_exhaustive_scans but never exceed it (modulo concurrency artifacts).
   *          If a user sees this stat exceeding `m_exhaustive_scans`, we should investigate
   *          the sequence of events that led to it.  (As of this writing we have not seen this.)
   *
   * @see #m_fifo_scans doc header for related background.
   */
  std::atomic<uint64_t> m_fifo_exhausted_scans = 0;

  static_assert((detail::Lend_tracker_pool::S_N_UNUSED_IDX_HINTS % 4) == 0,
                "N_UNUSED_IDX_HINTS should be divisible by 4, if only for this stat histogram's buckets "
                  "to be nice and even.");
  /**
   * Histogram that records, at the start of each run through the zombie-reaping scan algorithm (see #m_scans),
   * the zombie-object-count estimate N -- excluding N=0 (no work to do; immediate exit) and excluding
   * *exhaustive* scans (see #m_exhaustive_scans): those do not read the estimate at all.
   *
   * As noted in various nearby stat-member doc headers, each run through the zombie-reaper scan shall
   * strive to find (and therefore destroy) at least N zombie-objects in the obj-DB.
   *
   * If the histogram shows that the most frequent eventualities are those where N is under the
   * value of the internal constant `"detail::Lend_tracker_pool::S_N_UNUSED_IDX_HINTS"` -- the max # of
   * zombie-object hints that can be communicated via SHM-aux-pool at a time -- then the algorithm
   * is performant.  Otherwise it might be best to tune that constant in the *up* direction.
   * (Conversely, if the histogram clusters around the left side, leaving lots of room before
   * the aforementioned constant, then even more perf may be squeezed out by *lowering* it.)
   *
   * @note We choose the ~middle of the bucket range to match the aforementioned constant
   *       `"detail::Lend_tracker_pool::S_N_UNUSED_IDX_HINTS"`.
   *
   *
   * Suppose `"Lend_tracker_pool::S_N_UNUSED_IDX_HINTS"` is 16; then we get this bucket structure:
   * [1..3 | 4..7 | 8..11 | 12..16 | 16..19 | 20..23 | 24..27 | 28..31 | 32..35.. ].
   * Recall that, as usual, the final bucket gets all the overflow events; meaning cases where the
   * estimate N showed anything over 32 -- not necessarily only up to 35.
   */
  flow::util::stat::Histogram_counter
    m_histo_zombie_ct_estimate{9,
                               (detail::Lend_tracker_pool::S_N_UNUSED_IDX_HINTS / 4) - 1,
                               detail::Lend_tracker_pool::S_N_UNUSED_IDX_HINTS / 4,
                               1};

  /// See Events doc header.
  Events<> m_events;
}; // struct Zombie_obj_reaper_stats

/**
 * A bundle of several individually-documented stat-sets -- object construct/dispose/live and
 * zombie-reaper stats -- grouped into this one `Stat_set` because they are all collected together (for
 * performance, via thread-local sharding) and therefore consumed together but separately from non-sharded
 * items.  The grouping itself is based on these impl constraints; to the stat-consuming user that is not
 * too interesting.  What is interesting is what each member tracks; see that member's own type's doc header.
 *
 * @see Pool_stats: A peer grouping of per-arena stats; it must be separate due to not being TL-sharded when
 *      internally collected; therefore the method of return is different from a `*this`.
 * @see Memory_manager_stats: A peer grouping of per-arena stats; it is separate due to coming from an
 *      external data source (memory manager: e.g., jemalloc) as opposed to from internal algorithms/data.
 *      (They could still be bundled together.  Nevertheless the subject matter/data source are, we feel,
 *      so sharply different that Memory_manager_stats simply should remain separate from all others.)
 *
 * ### Semantics ###
 * Generally, they're just `struct`s as usual.  In a publicly returned `*this` (returned by, e.g.,
 * jemalloc::Ipc_arena::sharded_stats()), the value in a given leaf stat-member is what it is: the measurement
 * of that thing, since the last reset (or init).  However:
 *
 * As a TL-sharded `Stat_set` (see `flow::util::stat` doc header for full discussion), there are some caveats
 * or interesting notes to keep in mind about it.  To recap these:
 *   - Stat-members named `..._hi_wmark` (by convention having `Stat_type::S_HI_WMARK`: high-water-mark) do not
 *     (cannot, due to limitations of sharding) represent, simply, the highest value (since reset if any) that
 *     the gauged-stat has ever achieved.  Instead each `_hi_wmark` represents the highest value (since reset if
 *     any) that the gauged-stat has achieved *sampled at stat-consumption (or last reset, or init) times*.
 *     Hence the more regular stat-consumption is, the more accurate/interesting `_hi_wmark`s are.
 *   - A diagnostic member named `m_n_shards` -- in this case shm::stat::Owner_obj_stats::m_n_shards -- is
 *     interesting in that it shows how many shard-`*this`es were used to generate (generally via summing, at
 *     last stat-consume time) the resulting `*this` being inspected.  That's how many threads overall participated
 *     in reporting data, each reporting at least one stat-member's worth.
 *
 * @internal
 * Stat sub-`struct`s grouped in this one `struct`, so that Thread_lcl_obj_db_admin and
 * Thread_lcl_obj_db_client TL-registries can, at stat consumption time, aggregate per-thread shards corresponding to
 * each of those sub-`struct`s.  Since a `*this` is a single (composed) `Stat_set` (in the `flow::util::stat`
 * sense), it is possible to aggregate N threads' shards into an overall result `Stat_set` with a single
 * `flow::util::stat::stats_aggregate_shards()` call.  After that, the stat-consumption API can copy-out or
 * otherwise rearrange/present individual sub-`struct`s to the user, re-organizing them as it sees fit.  Or
 * it can just present them all in this same grouping, which is what we chose to do ultimately; hence why this
 * grouping `struct` is public.
 *
 * Impl nitty-gritty notes
 * -----------------------
 * If none of the below makes any sense, have a look at the stat-consumption API's impl as well as a few places
 * where these stat-members are updated.  In any case, also, a familiarity with `flow::util::stat` namespace doc
 * header is assumed.
 *
 * @see detail::stat::sharded_stats() (stat-consume), detail::stat::sharded_stats_reset() (stat-reset).
 * @see detail::Thread_lcl_obj_db_admin, detail::Thread_lcl_obj_db_client, and
 *      detail::stat::Finalized_shards: These three hold shard `*this`es; the former two regularly update
 *      theirs.  Then the free function `sharded_stats()` aggregates them into a target `*this`;
 *      `sharded_stats_reset()` resets that target `*this`.
 *
 * The following is arguably really about Thread_lcl_obj_db_admin + Thread_lcl_obj_db_admin impl details,
 * and could be best discussed in one of those doc headers, but we're all friends (but not `friend`s) here,
 * and if you're interested in stats-related impl design, then this is a pretty good place to talk about it.
 * In any case Sharded_stats impl (the way it chooses to group sub-`struct`s basically) and
 * Thread_lcl_obj_db_admin + Thread_lcl_obj_db_client impls are all designed to work together.
 *
 * ### Who updates the stat-member values between stat-consumptions? ###
 * For each given stat-member, it may be updated only by Thread_lcl_obj_db_admin, or only by
 * Thread_lcl_obj_db_client, or by both (across potentially many threads).  If it's not *both* but only one --
 * let's say `_admin` for exposition purposes -- then no per-thread `_client`
 * will ever touch that stat-member, so its value shall remain zero in all per-`_client` shard `*this`es.
 * During aggregation any given `_client` simply won't contribute (to the aggregated total for this stat-member)
 * and is harmless.
 *
 * There is an alternative approach: Have 3 `struct`s, conceptually the non-overlapping union of which
 * equals the 1 `struct` you're reading now; one with the `_admin`-only stats, one with the `_client`-only stats,
 * and one with those that both touch.  One could claim this is cleaner; it does save some processor cycles
 * and RAM -- but the saved cycles are at stat consumption time (rare), and the RAM difference is small.
 * The increase in boiler-plate is considerable however; there are 3 types instead of 1; each has to be
 * `stats_aggregate_shards()`ed separately.  So, we're doing it the other way (previous paragraph).
 *
 * There is another slightly different category of stat-member in here too; as of this writing comprising
 * only `m_live_obj.m_live_object_zombies`.  (So the preceding 2 paragraphs describe all but that stat-member.)
 * For this category:
 *   - The stat-members are still sharded/aggregated as described.
 *   - They are, however, not updated during the regular operation of `Thread_lcl_obj_db_*` -- that is, between
 *     stat-consumption points.  Instead, at stat-consumption -- before the actual `stats_aggregate_shards()`
 *     call but under central lock that allows an undisturbed aggregation-walk over the extant per-thread
 *     `Thread_lcl_obj_db_*` objects -- we grab the required values (as of this writing, all are fittingly
 *     GAUGEs) and stuff them into the per-thread `*this`.  Then they get aggregated with everything else.
 *
 * @see stats_update_pre_consumption() impl which explains why we do this.
 *
 * ### Stat consumption / Stat resetting ###
 * We've already discussed stat-consumption above.  Just to be explicit about it though: User wants stats so:
 * We follow the suggested pattern for TL-sharded stat-consumption from `flow::util::stat` doc header.
 * (For simplicity of exposition let's ignore Thread_lcl_obj_db_client and pretend only Thread_lcl_obj_db_admin
 * is involved.  Call it `Admin` + some other made-up short identifiers for brevity.)  So there is a
 * `static Thread_local_state_registry<Admin> s_reg`.  At stat-consumption we do `s_reg.while_locked(F)`,
 * where `F()` is where we do the aggregation.  To wit in `F()` we use `state_per_thread` which
 * is essentially a set-container of per-thread `Admin`s.  While the state *within* each `Admin` can
 * absolutely concurrently change (in our case the state of interest is specifically the `*this` stat-set
 * inside `Admin`), no new `Admin`s can be created (new thread joins the fray) or removed (thread exits) --
 * within `F(){}` body.  Now we can use some iterator magic to feed an iterator-range of `*this`es to
 * `flow::util::stat::stats_aggregate_shards()`, corresponding to the set-container of `Admin`s.
 *
 * Resetting is similar (and again it follows the `flow::util::stat` doc header's suggestions).
 * User wants the ongoing stat-collection to be reset (in short: leave GAUGEs alone, reset ACCUMULATORs to zero,
 * and reset HI_WMARKs to match the GAUGEs' current values).  So we do the above, except that in `F()` upon
 * acquiring the set-container of extant `Admin`s, we don't feed their respective `*this`es into a single
 * `stats_aggregate_shards()` but rather `stats_reset()` each one.
 *
 * This also has the nice property of synchronizing (a/k/a making serial) stat-consumptions and stat-resets.
 * The `X.while_locked()` is both cases is on the same `X`, thus internally the same central mutex is being
 * locked.
 *
 * Adding Thread_lcl_obj_db_client into this does not essentially change the picture.  The code itself will
 * clarify those details.
 */
struct Sharded_stats
{
  // Data.

  /**
   * Sub-`struct` with individual stats; see type's doc header.
   *
   * Per suggested convention in `Owner_obj_stats` doc header, we place this one first.
   */
  shm::stat::Owner_obj_stats m_owner_obj;
  /// Sub-`struct` with individual stats; see type's doc header.
  arena_lend::stat::Owner_obj_stats m_owner_obj_arena_lend;
  /// Sub-`struct` with individual stats; see type's doc header.
  shm::stat::Lender_obj_stats m_lend_obj;
  /// Sub-`struct` with individual stats; see type's doc header.
  shm::stat::Live_obj_stats m_live_obj;
  /// (During mainstream piggy-backed zombie reaping) Sub-`struct` with individual stats; see type's doc header.
  arena_lend::stat::Zombie_obj_reaper_stats m_zombie_obj_reaper_main;
  /// (During drain-phase thread, after user thread exited) Sub-`struct` with individual stats; see type's doc header.
  arena_lend::stat::Zombie_obj_reaper_stats m_zombie_obj_reaper_drain;
}; // struct Sharded_stats

/**
 * Stats about the RAM use of the arena-lending SHM-provider's auxilliary (non-user-data) SHM-pools which are
 * used internally to store and performantly communicate (cross-process) the current use-counts per
 * first-class (`arena.construct<T>()`ed) object.
 *
 * ### Background et al ###
 * Internally, an arena-lending SHM-provider (including SHM-jemalloc) maintains a *first-class object*
 * (a/k/a `arena.construct()`ed object) database in memory.  A given such object's *use-count* is defined as 1
 * upon `.construct()` (and thus insertion into this obj-DB).  Each `shm_session.lend_object()` -- typically
 * so as to share it to another process which obtains its first-class object handle via
 * `shm_session.borrow_object()` -- increments use-count by 1.  Conversely, when the `.construct()`ed `shared_ptr`
 * group reaches ref-count 0, use-count decrements by 1; and similarly for the `.borrow_object()`ed `shared_ptr`
 * group.  Once the use-count reaches zero, it is garbage-collected (GCed).
 *
 * The use-count is a *cross-process* quantity: process A constructs/lends/drops
 * handle (`= 1`, `++`, `--` respectively), but process B borrows/drops (`--` for the latter), usually.
 * Process A is the one with the original *arena*, so it must be aware of then the use-count drops to zero,
 * including if process B was the one to have executed the last `--`.  To avoid sending actual IPC messages for
 * this situation, we maintain an internal-use only SHM-pool (per arena *per `arena.construct()`ing thread*),
 * called *aux pool*; it is created the first time per `arena` that a given *thread* does `arena.construct<T>()`.
 * Other thread(s), in this (owner) and borrowing process(es), each opens a SHM-handle to this same pool as
 * needed, when a use-count must be affected (namely on `shm_session.lend_object()`, dispose in owner process;
 * dispose in borrower process).  When the aux-pool is no longer necessary (arena shutdown; or user-thread exit +
 * no live objects from it left), it is destroyed.
 *
 * The point for stats is this: These aux pools take non-trivial RAM.  This `Stat_set` tracks various facts
 * about that topic and similar (e.g., not just about RAM per se but also counts of things that are causing the
 * RAM use, or vaddr space use for that matter).
 */
struct Obj_db_aux_pool_stats
{
  // Data.

  /**
   * Number of aux SHM-pools currently in existence; incremented when a given owner-process thread first
   * executes `arena.construct()`; decremented when the pool's name is removed from the file-system.
   *
   * Caveat(s) and fact(s):
   *   - The actual RAM is returned for general OS use no sooner than (1) this counter decrements *and* (2) all
   *     cross-process handles to the pool are closed.  When properly functioning and without pathological
   *     user app behavior, (2) should occur soon after (1).
   */
  std::atomic<uint64_t> m_aux_pool_count = 0;

  /// Max of #m_aux_pool_count observed so far.
  std::atomic<uint64_t> m_aux_pool_count_hi_wmark = 0;

  /**
   * Of the quanta counted in #m_use_ct_quanta, count of the ones in active use.
   *
   * Caveat(s) and fact(s):
   *   - Each pool currently contributing to #m_aux_pool_count contributes at least 1 to #m_use_ct_quanta.
   *   - Its contribution may go up over time but never down...
   *   - ...except when #m_aux_pool_count is decremented by 1: then the contribution on that 1 pool's
   *     behalf so far is decremented from #m_use_ct_active_quanta.
   *   - Even just 1 live-object *ever* (up to pool deletion per above) being tracked in a given quantum means
   *     that quantum counts as 1 full unit in #m_use_ct_active_quanta.  This is the case even if no active
   *     live-object (use-count) in that quantum still exists at this time (meaning all were freed).  (The
   *     quantum still is available for use; the RAM is resident through deletion; so it all counts.)
   */
  std::atomic<uint64_t> m_use_ct_active_quanta = 0;

  /// Max of #m_use_ct_active_quanta observed so far.
  std::atomic<uint64_t> m_use_ct_active_quanta_hi_wmark = 0;

  /**
   * Count of quanta available -- not necessarily (or typically) all active -- across the currently live
   * aux SHM-pools as counted in #m_aux_pool_count.
   *
   * ### Background ###
   * The max available space in an aux-pool is divided into N *quanta*.  (As of this writing N=32, but this could
   * change; citing the number here for exposition purposes.)  To limit unnecessary RAM use, the algorithm
   * will issue use-count slots as needed from only 1 quantum.  Once it is completely full (there are enough
   * live first-class objects from the tracked arena constructed in the tracked thread), quantum 2 is added
   * to the set of active quanta; use-count slots shall be issued from both active quanta.  Once both of those
   * run out at the same time, quantum 3 is added; etc.  If all N quanta are exhausted at the same time, it
   * is a fatal error -- but the count and capacity of each quantum are such that this is quite unlikely.
   *
   * This stat-member is the sum of these Ns across all #m_aux_pool_count extant aux SHM-pools.  Hence it goes
   * up by N when `m_aux_pool_count` goes up by 1 and both values are subtracted together similarly.
   *
   * @note As of this writing N is a compile-time constant; hence one could derive this value by grabbing N
   *       somewhere from the Flow-IPC code and multiplying by #m_aux_pool_count.  This could change, however,
   *       so we found it prudent to make this a dynamic stat, for completeness and forward-compatibility of sorts.
   *       Due to internal impl details, the perf impact of this is negligible.
   *
   * ### Caveat(s) and fact(s) ###
   *   - This stat-member does *not* correspond to actual RAM taken away from general OS use (#m_resident_sz).
   *     It does correspond to the vaddr space *mapped* (#m_mapped_sz).  One can think of it as the worst case
   *     scenario, if all owner-process threads to have ever constructed objects in this arena will construct as
   *     many objects as humanly possible.
   *   - Even just 1 live-object *ever* (up to pool deletion per above) being tracked in a given quantum means
   *     that quantum counts as N full units in #m_use_ct_quanta.
   */
  std::atomic<uint64_t> m_use_ct_quanta = 0;

  /// Max of #m_use_ct_quanta observed so far.
  std::atomic<uint64_t> m_use_ct_quanta_hi_wmark = 0;

  /**
   * Of the mapped space tracked (in bytes) in #m_mapped_sz, the approximate/ceiling size of *resident* RAM
   * (RAM taken from general OS use): small header plus space used by #m_use_ct_active_quanta quanta.  In short
   * it's a (conservative) estimate of RAM used by the SHM aux-pools.
   *
   * @note The word resident is sometimes termed as *dirty*, particularly w/r/t kernel *pages*.
   *
   * Caveat(s) and fact(s):
   *   - Each active quantum added to #m_use_ct_quanta adds a constant (per-pool at least) amount of RAM
   *     to #m_resident_sz; except that the very 1st quantum (per-pool) adds a bit more due to the required
   *     header/metadata.
   *   - This value is conservative, in that it acts as-if each active quantum is immediately full of used
   *     use-count slots (even after just 1 actual use-count was allocated therein).  The accounting required
   *     to obtain a tighter ceiling would be too onerous in terms of complexity and performance.
   *     - In particular, even if several pages-full of use-count slots were all freed, we do not return that
   *       vaddr space into general OS use -- it remains resident.
   *   - This value is approximate, +/- the OS page size (typically 4Ki bytes).  We make a reasonable attempt
   *     to keep certain bounds and offsets consistent with page boundaries, but it is not fully rigorous.
   */
  std::atomic<size_t> m_resident_sz = 0;

  /// Max of #m_resident_sz observed so far.
  std::atomic<size_t> m_resident_sz_hi_wmark = 0;

  /**
   * Total vaddr space, in bytes, reserved for SHM aux-pools.  This is to #m_resident_sz what #m_use_ct_quanta is
   * to #m_use_ct_active_quanta.  Therefore it is important to remember this is not RAM taken from general OS
   * use but merely mapped.  #m_resident_sz is the actual RAM taken.  One can think of it as the worst case
   * scenario, if all owner-process threads to have ever constructed objects in this arena will construct as
   * many objects as humanly possible.
   *
   * Thus, when #m_aux_pool_count goes up by 1, #m_mapped_sz is increased by K; and both values are subtracted
   * at the same time also, at pool deletion.
   *
   * @note As of this writing K is a compile-time constant in practice, though certain internal subtleties would
   *       make it challenging to find the exact K without some run-time voodoo.  Even ignoring that, however,
   *       the constance of K could change.  So we found it prudent to make this a dynamic stat.
   *       (Due to internal impl details, the perf impact of this is negligible.)  For consistency, at least,
   *       we also implemented #m_use_ct_quanta as a dynamic stat rather than relying on knowing K and multiplying
   *       by `m_aux_pool_count`.
   *
   * @internal
   * The "internal subtleties" come from, at least, the fact that detail::Use_count_registry (our custom
   * boost.ipc memory-algorithm impl) -- by bipc's design -- receives `extra_hdr_sz`, which contributes to
   * K, as a run-time argument from `bipc::basic_managed_shared_memory` (as used in detail::Lend_tracker_pool).
   * It's some bipc internal header-stuff, that might even actually be zero-sized in our case; we can deduce it
   * empirically and hard-code it... but it'd be an unnecessary kludge.
   */
  std::atomic<size_t> m_mapped_sz = 0;

  /// Max of #m_mapped_sz observed so far.
  std::atomic<size_t> m_mapped_sz_hi_wmark = 0;
}; // struct Obj_db_aux_pool_stats

/**
 * Similar mission to Obj_db_aux_pool_stats but covers information that, by nature, is not per-arena but rather
 * global (but still segregated by SHM-provider, such as SHM-jemalloc versus other-arena-lending-SHM-provider X).
 *
 * @see Obj_db_aux_pool_stats doc header for background on aux-pool mechanism.
 *
 * ### (Further) background et al ###
 * Generally they can also be contrasted like this:
 *   - Obj_db_aux_pool_stats: Admin/owner perspective; counts aux-pools themselves as well as the RAM use thereof.
 *     Each aux-pool whose name is in file-system <=> one *admin* SHM-pool handle.
 *   - Obj_db_aux_pool_global_stats: Client (in both owner process and borrower process(es)) perspective; counts
 *     *client* SHM-pool handles, their openings/closings (the former triggered lazily by each user thread
 *     needing to access an aux-pool, excluding the admin/constructing thread).
 *     - The latter is called a *client* thread.  Cf. admin thread (<=> actual named in-SHM aux-pool).
 *       The first of the following user-invoked events triggers opening a per-arena, per-client-thread
 *       aux-SHM-pool handle:
 *       - *owner-side* `shared_ptr<T>` group (from `arena.construct<T>()`) reaches ref-count=0 in non-admin thread
 *         => disposer runs in that thread => cross-process use-count happens to reach zero.  (Can instead happen
 *         in admin-thread disposer; or in *borrower-side* disposer -- next bullet.)
 *       - *borrower-side* `shared_ptr<T>` group (from `Shm_session::borrow_object()`) reaches ref-count=0.
 *         (Outside of test scenarios, this usually occurs in different process from arena-owner's.)
 *     - The following triggers the closing of such an aux-SHM-pool handle:
 *       -# In admin/owner-land (tracked by Obj_db_aux_pool_stats) the SHM-pool name is deleted from file-system
 *          (user constructing thread exited; all live objects immediately or subsequently dropped by user app).
 *       -# In-SHM this is marked by a *dead* flag become `true` permanently.
 *       -# In each given client thread, at certain opportunistic/piggy-backed points, the flag is checked
 *          and observed to be `true`.  Therefore, as all live objects are gone and none may appear further
 *          (admin thread has exited): close the client aux-SHM-pool handle.
 *       -# Result: aux SHM-pool RAM returned to OS for general use (once all such cross-process pool-handles
 *          have closed; that is the above steps have executed in every relevant thread cross-process).
 */
struct Obj_db_aux_pool_global_stats
{
  // Data.

  /**
   * (Gauge) In this process, the number of all per-owner-thread, per-arena thread-local aux SHM-pool handles currently
   * open.  This total, across all processes (1 owner process + 1 per each borrower process), plus the original
   * admin handles (Obj_db_aux_pool_stats::m_aux_pool_count across all arenas) = total descriptor use by the
   * aux pool mechanism.
   *
   * Modulo concurrency artifacts this shall equal #m_client_tl_aux_pool_hndl_open_count minus
   * #m_client_tl_aux_pool_hndl_reap_count (can use as sanity-check).
   */
  std::atomic<uint64_t> m_client_tl_aux_pool_live_hndls = 0;

  /// Max of #m_client_tl_aux_pool_live_hndls observed so far.
  std::atomic<uint64_t> m_client_tl_aux_pool_live_hndls_hi_wmark = 0;

  /**
   * In this process: number of times a client aux-SHM-pool handle (as described for
   * #m_client_tl_aux_pool_live_hndls) has been opened.
   */
  std::atomic<uint64_t> m_client_tl_aux_pool_hndl_open_count = 0;

  /**
   * In this process: number of times an attempt to open a client aux-SHM-pool handle (as described for
   * #m_client_tl_aux_pool_live_hndls) failed -- most likely because the aux-pool no longer exists in the
   * file-system.  Depending on the operation attempting it, that condition is either benign or grave.
   * Benign: one is releasing (dropping one's last handle to) a *borrowed* object, but the lending arena --
   * possibly its whole process -- is already gone; then the object is moot by contract, and the release
   * quietly succeeds.  Grave, in any other situation (e.g., resource exhaustion; or an external actor
   * deleting shared-memory objects): the operation in question shall throw.  Hence: in an application that
   * is otherwise humming along, this stat counts benign-release events; while a non-zero value accompanied
   * by throwing/aborting operations indicates the grave sort.
   *
   * The grave type *will* mean a thrown exception, and in some cases a
   * guaranteed abort; it would be surprising if your application continued
   * operating just fine, even if you managed to catch this truly-exceptional exception.  So, in and of itself, seeing
   * a non-zero value here should not be a source of worry.  If everything else is fine, then it's the
   * benign thing.  As for the benign thing:
   *
   * This is an interesting value, if non-zero, indicating a quite-tight (chronologically) shutdown sequence:
   * a process decided to end a session; immediately ended a lent (through that session) SHM-arena; and the
   * opposing side detected the session-end and immediately did the compliant thing: nullify any outstanding
   * `borrowed_object()`ed SHM-handles (and probably then end the session object).  The nullify step there is
   * what would trigger a `++` here.  To be clear we reiterate: that does not mean you are doing anything wrong.
   *
   * @internal
   *
   * Incremented in Thread_lcl_obj_db_client::new_pool_data(), in the stats-keeping surrounding the
   * client-mode Lend_tracker_pool ctor throwing.  The benign case is the not-found-tolerant catch clause in
   * the borrower-side Thread_lcl_obj_db_client::disposing_obj() overload; every other path lets the
   * exception fly ("grave" situation mentioned above).
   */
  std::atomic<uint64_t> m_client_tl_aux_pool_hndl_open_fail_count = 0;

  /**
   * In this process: number of times a client aux-SHM-pool handle (as described for
   * #m_client_tl_aux_pool_live_hndls) has been closed.
   */
  std::atomic<uint64_t> m_client_tl_aux_pool_hndl_reap_count = 0;

  /**
   * In this process: number of times a client aux-SHM-pool handle (as described for
   * #m_client_tl_aux_pool_live_hndls) was used to check whether the aux-pool is in *dead* flag state.
   * (If the check showed an indeed-dead pool, #m_client_tl_aux_pool_hndl_reap_count goes up by 1 also.
   * Otherwise it does not.)  Can use this to get a sense of absolute frequency of the reap-checks
   * as well as to compute the rate at which it results in a reaping (closing of client pool-handle).
   */
  std::atomic<uint64_t> m_client_tl_aux_pool_hndl_reap_check_count = 0;
}; // struct Obj_db_aux_pool_global_stats

/**
 * Similar mission to shm::stat::Owner_obj_stats (which is relevant regardless of SHM-provider type) and
 * Owner_obj_stats but applied specifically to the mechanism of arena-lending SHM-providers wherein each SHM-arena
 * is divided into multiple, potentially many, SHM-pools.
 *
 * ### Background ###
 * From the perspective of the arena (hence `Owner_` prefix) in which first-class objects are constructed
 * (and GCed) and/therefore raw buffers are [de]allocated, consider the latter, most basic operation:
 * "allocate N bytes and return pointer to this area."  Ignore, temporarily, the relevance of SHared-Memory (SHM);
 * consider a regular heap `alloc(N)` albeit in a segregated arena, as in the jemalloc memory manager.
 * Before employing memory-manager internal algorithms -- free lists, size classes, who knows -- the manager
 * needs a contiguous *vaddr area* (a/k/a *extent*) in which to operate.  Hence it musk ask the OS
 * to *memory-map* (in Linux et al: `mmap()`) a chunk of some large-enough size (or use one that was mapped
 * earlier on-demand or otherwise).  Not all parts of the extent need to be actually written/read at first
 * (or possibly even ever, depending on what happens on the user's part); the extent is divided into *pages*,
 * and a page only becomes *dirty* (or *resident*) once it is written/read; that means it is assigned to actual
 * physical RAM which now cannot be used by something else.  At any rate, the `alloc(N)` in
 * particular will need some resident page(s) inside which to reserve the N bytes.
 *
 * All that is to say: inside the arena unit, the extent is the next smallest unit.  Now, consider SHM, as in
 * SHM-jemalloc.  The above still applies, but the `mmap()` (or equivalent) is no longer against the general
 * heap RAM but rather against some specific, named (at least initially at creation/opening time) *SHM-pool*
 * (a/k/a segment, SHM-object; in Flow-IPC we use "[SHM-]pool").  (A SHM-pool can be accessed by other process(es);
 * however that is not of direct interest from the owner's PoV, and therefore in the context of this discussion.)
 *
 * So, here we concern ourselves with SHM-pools, which compose the arena.  Events of interest are, e.g.,
 * memory-manger requested an extent (for us, pool), so we opened a SHM-pool and mapped M total bytes in that pool;
 * perhaps the reverse (pool no longer needed, close it).  These are straightforward, really, but a few
 * things to keep in mind:
 *   - Pools are generally created when a user, directly or indirectly (usually indirectly in Flow-IPC),
 *     executes something like `alloc(N)`, and then *the memory manager, lazily requests pool creation* upon
 *     having run out of already-mapped pool space.
 *   - A pool can never change size, and it either exists (is mapped) or does not (is not mapped, name removed
 *     from file-system), in the arena-lending SHM-provider's owner context (i.e., arena context).
 *   - A given memory manager may want to work on finer units than entire pools, or possibly the reverse:
 *     it could split a pool into 2+ extents; or possibly merge 2+ pools into an extent -- if they are
 *     adjacent in vaddr-space.  In short, we do not concern ourselves with that here.  It is not super interesting
 *     to us from the SHM-provider perspective; and in any case see preceding bullet.
 *
 * @internal
 * There are some counts that would be available to count, in SHM-jemalloc extent-hooks in particular at least --
 * unclear if they would have equivalents for other/all arena-lend SHM-providers -- which we did not include.
 * They are at least (again, for SHM-jemalloc) as follows.
 *   - Split-extent and merge-extents requests:  Potentially interesting albeit inside-baseball.  Not included
 *     chiefly because for all we currently know, they could be frequent enough to where the non-sharded
 *     counting otherwise used for Owner_pool_stats become sufficiently costly to have to introduce sharding or
 *     at least have a compile-time gate for whether to track these.  Information is likely also available in
 *     native-jemalloc stats, which are available without us, *and* we facilitate grabbing them more conveniently
 *     elsewhere as well.
 *   - Commits/decommits:  Too hairy/inside-baseball; not really an area of SHM-provider interest; not sure if perf
 *     risk.  Probably available in native-jemalloc stats (ditto prev bullet).  Might also be more of a thing or
 *     less of a thing depending on OS which speaks to its inside-baseballhood.
 *   - Lazy-purge, forced-purge: Probably not a perf risk. (Purging is time-based -- the `dirty/muzzy_decay_ms`
 *     thresholds, ~10s by default, set how long unused space must age first.  Use of jemalloc, alloc/free specifically,
 *     provides the piggy-backed opportunities to check against these default-generous time thresholds and sometimes
 *     proceed.)  Again it seems too hairy/inside-baseball; and again probably available in native-jemalloc stats.
 *     As in prev bullet might be OS-dependent.
 */
struct Owner_pool_stats
{
  // Data.

  /**
   * Number of times the memory manager (e.g., jemalloc for SHM-jemalloc) requested the creation of a new
   * extent of a certain size, and we therefore obliged it by creating a named SHM-pool of that actual size.
   *
   * ### SHM-jemalloc-specific ###
   * *As of this writing*, due to the policy described in #m_pool_optional_destroy_request_count
   * doc header, #m_pool_destroy_count and #m_pool_destroy_sz are in practice likely the be zero, until arena
   * shuts down... at which point there is no jemalloc::Ipc_arena object wherein to query current stats.
   *
   * Therefore:
   *   - #m_pool_create_count is effectively the **current pool count** for the arena;
   *   - #m_pool_create_sz is effectively the **current mapped (not necessarily resident!) total size** for
   *     the arena;
   *   - but do glance at `m_pool_destroy_*` to sanity-check they are zero.  If not then (1) look into why and (2)
   *     subtract those to get the current-pool-count and current-mapped-size values.
   *
   * @internal
   * @todo Once the aforementioned policy changes, we should do a few things in Owner_pool_stats:
   * (1) add GAUGEs `atomic<size_t> m_n_pools` and `m_mapped_sz`; (2) implement their tracking by live-updating them via
   * `fetch_add()` and `fetch_sub()`, whenever the existing `m_pool_create/destroy_*` values are updated;
   * (3) add `_hi_wmark` counterparts to both; and (4) update them in the usual way; these will (as is typical
   * elsewhere in Flow-IPC) become roughly the reason to even keep separate GAUGEs which are otherwise derivable
   * from existing create/destroy ACCUMULATORs.  Until all that, the existing ACCUMULATORs are sufficient;
   * and the `_destroy_` stat-members are nice canaries to detect unexpected (non-shutdown) pool-removals.
   */
  std::atomic<uint64_t> m_pool_create_count = 0;

  /// Across the pool-creations counted in #m_pool_create_count, the sum of the requested/actual sizes of those pools.
  std::atomic<uint64_t> m_pool_create_sz = 0;

  /**
   * Number of times the memory manager (e.g., jemalloc for SHM-jemalloc) requested *something*, and as a result
   * we removed 1 SHM-pool, undoing a pool-creation op counted in #m_pool_create_count.
   *
   * ### SHM-jemalloc-specific ###
   * *As of this writing*, due to the policy described in #m_pool_optional_destroy_request_count
   * doc header, #m_pool_destroy_count and #m_pool_destroy_sz are in practice likely the be zero, until arena
   * shuts down... at which point there is no jemalloc::Ipc_arena object wherein to query current stats.
   * Hence this is likely to always be zero.
   *
   * ### Context ###
   * From the PoV of what the SHM-provider has *done*, this is symmetrical to #m_pool_create_count.  That one
   * counts creations; this one counts destructions thereof.  Simple.
   *
   * From the PoV of the memory manager, in terms of what it *requested* the SHM-provider do, it is more complex.
   * The following information is based on SHM-jemalloc (as of this writing the only arena-lending SHM-provider
   * in Flow-IPC); it is conceivable other arena-lending SHM-providers might change the picture somewhat, leading
   * to changes to this/related members and/or creation of provider-specific `struct`(s).
   *   - Creation: As noted: mem-mgr requests extent; we create SHM-pool.  Request is always honored; that is
   *     counted in #m_pool_create_count.
   *   - Destruction: A couple of possible triggers:
   *     - Arena shutdown (user-triggered; e.g., application shutting down) in progress; mem-mgr requests the
   *       destruction of each pool one-by-one.  This is always honored and is (at least part of) #m_pool_destroy_count.
   *       - Fine... conceivably a pool might be removed not as one pool-sized chunk but rather in pieces, until
   *         the final removed-piece gets the un-removed byte count for the pool down to zero.  We state this
   *         here only for the sake of the pedants; the point is -- in however many steps -- arena shutdown =>
   *         pool removed, for each live pool.
   *     - Mem-mgr is observing a large number of allocs have been dealloc-ed, so some pools are no longer
   *       needed -- perhaps they can be returned as free RAM to OS.  This may or may not be honored; it is counted
   *       in #m_pool_destroy_count only if, yes, was honored.
   *
   * This stat-member tracks only the result of such requests, not the requests.  However see doc header
   * for #m_pool_optional_destroy_request_count for more on that topic.
   */
  std::atomic<uint64_t> m_pool_destroy_count = 0;

  /// Across the pool-destructions counted in #m_pool_destroy_count, the sum of the actual sizes of those pools.
  std::atomic<uint64_t> m_pool_destroy_sz = 0;

  /**
   * Number of times the memory manager (e.g., jemalloc for SHM-jemalloc) requested the *optional* removal
   * (unmap + delete name from file-system) of 1 SHM-pool; "optional" meaning the SHM-provider could accept
   * (thus counting in #m_pool_destroy_count) or reject the request.
   *
   * @see #m_pool_destroy_count doc header for background.
   * @note This does *not* count removal-requests during arena shutdown; these removal-requests are not mandatory;
   *       they are always honored.
   *
   * SHM-jemalloc: As of this writing we *always* deny these requests; and SHM-jemalloc is the only arena-lend-type
   * SHM-provider.  Therefore we do not yet have a `m_pool_optional_destroy_accept_count` counterpart; it would
   * always be zero.
   *
   * @note The reason for this policy is out of our scope here; but it may well not be indefinitely the case that
   *       we follow it.  We might be more intelligent about it in a future SHM-jemalloc version.
   * @note Incidentally: If an optional pool-destroy request from mem-mgr is rejected by SHM-provider, mem-mgr
   *       shall likely reuse the SHM-pool for future allocations if suitable.  The fact it remains mapped indefinitly
   *       does not even mean it will *necessarily* stay resident indefinitely; mem-mgrs can and do use special
   *       OS calls to "un-dirty" unused pages (return physical RAM back to the OS for general availability).
   *
   * @internal
   * We considered adding an `_sz` counterpart to #m_pool_optional_destroy_request_count; but to me (ygoldfel) is
   * is not immediately clear what that should be.  As of this writing the extent-hook handler
   * Ipc_arena::optional_remove_shm_pool() simply returns `false`; and I am not immediately sure if in practice
   * the `size` arg would end up equalling the removed pool's size had it returned `true` and actually removed
   * some pool.  It is probably an answerable question, and this might even be an official to-do, but I figured
   * let's have it return `true` sometimes first before worrying about such a thing (which is currently a
   * hypothetical in terms of actually acting on it).
   */
  std::atomic<uint64_t> m_pool_optional_destroy_request_count = 0;
}; // struct Owner_pool_stats

/**
 * Bundles together per-arena Owner_pool_stats and Obj_db_aux_pool_stats, so they can be accessed via one accessor
 * (e.g., jemalloc::Ipc_arena::pool_stats()) and handled (e.g., `ostream << print()`, `stat_set_aggregate()`)
 * as one unit.  The subject matter loosely connecting these sub-`struct`s is that they deal with SHM-pools
 * (though of quite different kinds).
 *
 * @see Sharded_stats: A peer grouping of per-arena stats; it must be separate due to being TL-sharded when
 *      internally collected; therefore the method of return is different.
 * @see Memory_manager_stats: A peer grouping of per-arena stats; it must separate due to coming from an
 *      external data source (memory manager: e.g., jemalloc) and therefore also featuring an incompatible method
 *      of return versus a `*this`.
 *
 * ### Nitpick ###
 * Why not follow various precedent, given that Owner_pool_stats and Obj_db_aux_pool_stats are only stored
 * in this one `struct`, and do the following?
 *   - Remove `_stats` from both type names and make them nested in Pool_stats.
 *   - Have one declare_stats() covering all the stat-members directly instead of 3: 2 per sub-`struct`, 1
 *     for Pool_stats which forwards to the other 2.
 *
 * There's no massive reason.  It was just slightly more convenient; a certain `#include` cycle goes away,
 * as certain headers don't need to include more than a `_fwd.hpp`.  The user experience is ~identical; in fact
 * arguably a little more flexible this way, as with it the individual fields (and their types) #m_owner_pool and
 * #m_obj_db_aux_pool can be handled by themselves if desired: `ostream << print()`, `stat_set_aggregate()`, etc.
 *
 * Doing it the other way would have been more compact in this file basically.  It would also be defensible.
 */
struct Pool_stats
{
  // Data.

  /// Sub-`struct` with individual stats; see type's doc header.
  Owner_pool_stats m_owner_pool;
  /// Sub-`struct` with individual stats; see type's doc header.
  Obj_db_aux_pool_stats m_obj_db_aux_pool;
};

/**
 * Similar mission to Owner_pool_stats but covers information that, by nature, is not per-arena but rather
 * global (but still segregated by SHM-provider, such as SHM-jemalloc versus other-arena-lending-SHM-provider X);
 * concerned with raw-offset-pointer lookups.
 */
struct Owner_pool_lookup_global_stats
{
  // Data.

  /**
   * (Gauge) Number of thread-local vaddr-to-SHM-pool caches currently in existence; equals the number of threads
   * that have ever needed to perform at least 1 such lookup, minus the number of those threads that have
   * exited.
   *
   * ### Context/background ###
   * As such this stat-member illuminates the application's cross-thread pattern of use of these lookups.
   * In particular, many STL-compliant containers (in SHM) would perform these lookups in various situations; but
   * it really depends on the purpose and implementation of a given container.  Rewinding a bit though:
   *
   * The lookup in question is as follows: You've got some data structure or datum in-SHM, created in a SHM-arena
   * (possibly but not necessarily via `arena.construct<T>(...)`, where `T` contains or is something relevant), and
   * in working with it you've got an owner-process-local vaddr -- which may or may not in fact be in the SHM-arena (if
   * not then in the stack/heap... but don't worry about that now, off-topic) -- and you need to find out (1) whether
   * it is in fact in the SHM-arena (<=> in exactly 1 of the pools composing said arena) and (2) if so, which
   * one.  (The latter "you" is usually not user code per se but something inside an STL-compliant container's code,
   * with its allocator tparam set to our shm::stl::Stateless_allocator, trying to do something like
   * `pointer_traits::pointer_to()`.  That is it is converting a raw pointer `T*` to a fancy-pointer; in Flow-IPC's
   * arena-lending-SHM-provider case `Shm_pool_offset_ptr<T>`.)
   *
   * This is a non-trivial computation, and it requires lookups (possibly multiple lookups in a binary-search
   * algorithm) in a *global map from vaddr to SHM-pool base-vaddr*.  Moreover, this map/table might change at
   * any time, as concurrent user SHM-allocations sometimes trigger the memory manager (e.g., jemalloc in
   * SHM-jemalloc) to ask for a new mem-mapped vaddr-area -- i.e., in our case a new SHM pool.  To massively
   * speed-up these lookups, we store thread-local copies of the table: *pulled* from the canonical central
   * copy at first per-thread such lookup (a/k/a lazy init) and then zealously *pushed* whenever that canonical
   * copy is modified (pool inserted/removed, in *any* arena).
   *
   * Thus:
   *   - This stat-member goes up by 1 at *first such reverse-lookup* in a given user thread.
   *   - This stat-member goes down by 1 when 1 such thread exits.
   */
  std::atomic<unsigned int> m_pool_rev_lookup_tl_cache_count = 0;

  /// Max of #m_pool_rev_lookup_tl_cache_count observed so far.
  std::atomic<unsigned int> m_pool_rev_lookup_tl_cache_count_hi_wmark = 0;
}; // struct Owner_pool_lookup_global_stats

/**
 * IDs an arena, as a GAUGE in contexts like Shm_pool_info and per-arena Borrower_pool_stats, uniquely
 * machine-wide up to boot.  If N/A: special value zero is used for all fields.
 */
struct Uniq_arena_id
{
  // Data.

  /**
   * Iff per-arena context (else zero): Unique (machine-wide, between boots) ID of containing SHM-arena: first half,
   * currently the owner PID.
   */
  uint64_t m_id1 = 0;

  /**
   * Iff per-arena context (else zero): Unique (machine-wide, between boots) ID of containing SHM-arena: second half,
   * currently: ordinal within process.
   */
  uint64_t m_id2 = 0;
}; // struct Uniq_arena_id

/**
 * Loosely similar subject matter to Owner_pool_stats but regarding the *borrower-side* view of SHM-pools,
 * as opposed to an owner/arena-side view thereof.
 *
 * ### Background et al ###
 * The most basic fact here is that, essentially, an owner-process has an arena which in a sense *produces*
 * SHM-pools (as required by the memory manager, such as jemalloc, during allocations), and it also *deletes*
 * them.  Cf. the borrower side which *consumes* them via a procedure called pool-lending/borrowing.
 * (This occurs in the background without user participation.)  The setup is:
 *   - owner process A with arena X;
 *   - borrower process B;
 *   - a session SAB (`Shm_session` object SA in A, peer object SB in B) = communication context between them;
 *   - user explicitly *lends* arena: `SA.lend_arena(X)`.  Now:
 *   - whenever arena X creates pool P1, it auto-lends P1 to B via SAB by-virtue of the fact that X has been lent
 *     through SAB.  (There can of course be more pools over time.)
 *
 * None of this is limited to, like, one thing. A can have session SAC to process C; it can even have a
 * separate session SAB2 -- but only via a separate `Shm_session` SA2 (and SB2 in B) -- again to B.  (If X
 * is lent through all of these sessions -- SAB, SAC, SAB2 in this example -- then a pool like P1 is auto-lent
 * through all of them: twice to B, once to C.)
 *
 * @note What is "borrowing a pool"?  It is: open a SHM-pool handle according to the pool's name (same one with
 *       which owner-process-arena create-opened it) -- as of this writing for read-only access, enforced by
 *       the kernel -- and memory-map it to a process-local vaddr.
 *
 * Due to this setup, the accounting is a bit tricky.  Consider, now, a given process; B for example.  It maintains,
 * internally, a **global** *borrower repository* (*BR* for short), whose basic purpose is to maintain
 * *every* pool currently being borrowed.  We've shown above, though, that a given arena X can be (from a BR's
 * PoV) be borrowed 2x+; and therefore a given pool P1 in X can be borrowed 2x+ also.  BR shall nevertheless
 * only maintain *one* SHM-pool handle (and *one* record of containing arena), while maintaining a *use-count*
 * for each.
 *   - When an arena, and in-arena pool's, use-count goes from 0 to 1: record arena; *open pool*.
 *   - When an arena, and in-arena pool's, use-count goes from not-0 to not-0 (borrowed again/unborrowed, but use-count
 *     remains 1+): just inc or dec the use-counts; neither open nor close pool; just let it be.
 *   - When an arena, and in-arena pool's, use-count goes from 1 to 0 (unborrowed, as from session closing):
 *     *close pool* (un-memory-map; close pool-handle; delete record of arena).
 *
 * Therefore, from BR's (global) PoV, you've got: arena/pool registering (a/k/a borrowing), where use-count goes up;
 * the opposite (goes down); opening (use-count reaches 1); and closing (use-count reaches 0).  Plus, since each
 * pool has a permanent (can never change) size, each actual-pool-handle + sums thereof <=> mapped-size + sums
 * thereof.  All these things, including use-counts, are counted in Borrower_pool_stats; and the global BR thus owns
 * a global Borrower_pool_stats.
 *
 * The complexity continues: the above talks of the BR's PoV; but there is also:
 *   - BR's PoV, but broken down by *ever-borrowed arena*.  (An arena, in this context, is identified by a
 *     machine-wide-unique, between boots, ID: owner/PID + in-process-ordinal-ID.)
 *     - A given arena X can be borrowed via session SAB, then unborrowed when SAB closes; then borrowed again
 *       via fully-subsequent session SAB2.  Accumulators like pool-open-count for pool P1 will carry-over
 *       throughout and shall persist during any gap when X isn't borrowed by anything.  Nothing is degenerate;
 *       though #m_n_borrowed_arenas cannot exceed 1 (an arena is either currently being borrowed, or it isn't).
 *   - A given `Shm_session`'s point-of-view.  To wit: a given arena can only be borrowed via SB once
 *     (borrowing same via SB2 is not the same thing; the processes are the same in our example, but the
 *     session -- while similar in configuration -- is a different session).  So all of the same things apply;
 *     but the use-count of a given arena, and pool therein, can only ever be 1.  The values of some stat-members,
 *     when applied to a `Shm_session`, will thus carry certain degenerate values which are correct but of low
 *     interest.  They are still recorded, however, so we can reuse the same `struct` -- the amount of boiler-plate
 *     otherwise would be obnoxious.
 *     - Remember though: from this PoV, last-deregistering a pool does *not* necessarily mean closing its handle
 *       and unmapping it.  Same with first-registering versus opening+mapping.  (It only does mean that from the
 *       global BR's overall PoV, or as broken-down by arena.)
 *
 * Well... don't worry about it too much right now.  Things will make more sense once you look at the actual
 * stat-consumption API... or, indeed, each of the 3:
 *   - BR's global `Borrower_pool_stats` object.  (No degenerate-valued members.)
 *   - BR's global by-arena map: arena-ID => `Borrower_pool_stats`.  (No degenerate-valued members.)
 *   - A `Shm_session`'s non-global `Borrower_pool_stats` object.  (Some degenerate-valued arena/pool-oriented members.)
 */
struct Borrower_pool_stats
{
  // Data.

  /// Iff per-arena context (else zeroes): Unique (machine-wide, between boots) ID of containing SHM-arena.
  Uniq_arena_id m_uniq_arena_id;

  /// How many times an arena was borrowed (opposing user did `S.lend_arena(X)`), including when already-being-borrowed.
  std::atomic<uint64_t> m_arena_register_count = 0;

  /**
   * Of the borrowings counted by #m_arena_register_count, how many have been undone (as of this writing: via
   * closing of the pertinent `Shm_session S`; there is no support for explicit un-lending while retaining
   * session, but in theory this could be added).
   */
  std::atomic<uint64_t> m_arena_deregister_count = 0;

  /**
   * Of the borrowings counted by #m_arena_register_count, exclude those wherein arena was borrowed while already
   * borrowed.
   *
   * @note Non-degenerate from PoV of the global `*this` and the per-arena breakdown: 2+ concurrent
   *       `Shm_session`s can borrow the same arena (e.g., session rotation with overlap), and only the
   *       first borrowing counts here.  Per-session, however, this (modulo concurrency artifacts) shall
   *       equal #m_arena_register_count (a given arena is borrowed at most once via a given session).
   */
  std::atomic<uint64_t> m_arena_first_register_count = 0;

  /**
   * Of the unborrowings counted by #m_arena_deregister_count, exclude those wherein arena was unborrowed while
   * it still remains borrowed on behalf of another session.
   *
   * @note Non-degenerate from PoV of the global `*this` and the per-arena breakdown (see
   *       #m_arena_first_register_count).  Per-session, however, this (modulo concurrency artifacts) shall
   *       equal #m_arena_deregister_count.
   */
  std::atomic<uint64_t> m_arena_last_deregister_count = 0;

  /// (Gauge) Effectively #m_arena_first_register_count minus #m_arena_last_deregister_count.
  std::atomic<uint64_t> m_n_borrowed_arenas = 0;

  /// Max of #m_n_borrowed_arenas observed so far.
  std::atomic<uint64_t> m_n_borrowed_arenas_hi_wmark = 0;

  /// Analogous to #m_arena_register_count but counts individual in-arena SHM-pools.
  std::atomic<uint64_t> m_pool_register_count = 0;

  /// Analogous to #m_arena_deregister_count but counts individual in-arena SHM-pools.
  std::atomic<uint64_t> m_pool_deregister_count = 0;

  /**
   * Analogous to #m_arena_first_register_count but counts individual in-arena SHM-pools.
   *
   * From the global or per-arena point-of-view (not per-session): this counts actual open+map ops.
   */
  std::atomic<uint64_t> m_pool_open_count = 0;

  /**
   * Analogous to #m_arena_last_deregister_count but counts individual in-arena SHM-pools.
   *
   * From the global or per-arena point-of-view (not per-session): this counts actual unmap+close ops.
   */
  std::atomic<uint64_t> m_pool_close_count = 0;

  /// (Gauge) Analogous to #m_n_borrowed_arenas but counts individual in-arena SHM-pools.
  std::atomic<uint64_t> m_n_open_pools = 0;

  /// Max of #m_n_open_pools observed so far.
  std::atomic<uint64_t> m_n_open_pools_hi_wmark = 0;

  /**
   * (Gauge) The combined vaddr-space, in bytes, across the SHM-pools counted in #m_n_open_pools (modulo
   * concurrency artifacts).  Reminders:
   *   - These are in SHM; mapping this in borrower-process does not use any more RAM than is actually used by
   *     the pool owner-side.
   *   - A pool's (mapped) size does not necessarily (or even typically) equal how much RAM is taken away
   *     from general OS use; mapped space is only so-taken (a/k/a resident, dirty) if pages have been
   *     read/written.  (Furthermore, by the memory manager's prerogative, previously resident pages can be
   *     made un-resident when not in use.  How aggressively this is done is up to the memory manager; e.g.
   *     jemalloc.  All of this occurs within the owner/arena process, not borrower-side.)
   */
  std::atomic<uint64_t> m_mapped_sz = 0;

  /// Max of #m_mapped_sz observed so far.
  std::atomic<uint64_t> m_mapped_sz_hi_wmark = 0;
}; // struct Borrower_pool_stats

/// Analogous subject matter to Owner_pool_lookup_global_stats but borrower-side.
struct Borrower_pool_lookup_global_stats
{
  // Data.

  /**
   * (Gauge) Number of thread-local vaddr-to-SHM-pool caches currently in existence; equals the number of threads
   * that have ever needed to perform at least 1 such lookup, minus the number of those threads that have
   * exited.
   *
   * ### Context/background ###
   * Identical to Owner_pool_lookup_global_stats, conceptually, but applied to interpreting vaddrs belonging to borrowed
   * data structures.
   */
  std::atomic<unsigned int> m_pool_rev_lookup_tl_cache_count = 0;

  /// Max of #m_pool_rev_lookup_tl_cache_count observed so far.
  std::atomic<unsigned int> m_pool_rev_lookup_tl_cache_count_hi_wmark = 0;

  /**
   * (Gauge) Number of thread-local SHM-pool-to-vaddr caches currently in existence; equals the number of threads
   * that have ever needed to perform at least 1 such lookup, minus the number of those threads that have
   * exited.
   *
   * ### Context/background ###
   * While conceptually not-dissimilar to the reverse-lookup (#m_pool_rev_lookup_tl_cache_count), the
   * forward lookup is, in fact, significantly more frequent in practice.  Consider a given in-SHM STL-compliant
   * container -- as usual, using our shm::stl::Stateless_allocator as the allocator tparam -- being borrowed
   * from a (typically) cross-process arena.  Any dynamically-sized container (among the simplest being
   * `vector`) will store at least one pointer to a dynamically-sized buffer; but since said buffer is
   * in SHM, and so is the pointer, the pointer cannot be stored in raw vaddr from -- as the vaddr in owner
   * process A =/= vaddr in borrower process B =/= (ditto in other potential borrower process(es)).  Therefore
   * a `Shm_pool_offset_ptr` is stored.  In SHM-jemalloc et al this basically encodes a SHM-pool ID and offset,
   * within the same `sizeof(void*)` bytes required for a raw ptr.  The fwd lookup tracked here comes down to,
   * most saliently, this:
   *   - look in the global borrowed-pool repository (called *BR* in Borrwer_pool_stats doc header); it
   *     stores a map from pool ID to the pool's base-vaddr (in this borrower process).
   *
   * Any dereferencing so as to read something in the STL-container will mean performing this fwd-lookup.
   *
   * For performance, as in the reverse-direction, we internally distribute the canonical map onto each
   * thread-local (TL) copy, as needed per-thread.  When the canonical map changes (pool first-borrowed or
   * last-unborrowed), this is proactively distributed per-TL-copy.  In this stat-member, similarly
   * to #m_pool_rev_lookup_tl_cache_count, we count how many of these copies (extant threads) there currently are.
   *
   * @note Why is there no equivalent stat-member in Owner_pool_lookup_global_stats -- why does that one feature only
   *       the reverse-lookup-TL-copy tracking?  Answer: never mind; it is too inside-baseball to explain here.
   *       Bottom line is, subtle algorithmic differences between owner-side and borrower-side are such that
   *       owner-side can use a pull-only TL-distribution scheme -- where no tracking of extant threads is
   *       required -- and therefore, unless we add such tracking just for stats, one cannot count them.
   *       We don't want to waste the cycles (just for stats): so we don't.  Hence that info is not available.
   */
  std::atomic<unsigned int> m_pool_fwd_lookup_tl_cache_count = 0;

  /// Max of #m_pool_fwd_lookup_tl_cache_count observed so far.
  std::atomic<unsigned int> m_pool_fwd_lookup_tl_cache_count_hi_wmark = 0;
}; // struct Borrower_pool_lookup_global_stats

/**
 * This info-`struct` contains basic properties of a SHM-pool (in a SHM-arena).
 *
 * The info included here is not *really* stats, per se, informally speaking.  Nevertheless we declare it
 * as a `Stat_set` if only to get `ostream << print(...)` "for free."  It is not really meant for use with other
 * `flow::util::stat::stats_*()` utilities, so you are on your own, if you try that.
 *
 * Each property is therefore declared as a GAUGE; as GAUGEs are, all else being equal, formally capable
 * of representing arbitrary values (until, that is, one tries to `stat_aggregate()` them or something).
 */
struct Shm_pool_info
{
  // Data.

  /**
   * Pool ID, trivially cast to a large unsigned integer type; identifies the SHM-pool with *machine-wide uniqueness*
   * (between successive boots); lower ID means earlier pool creation.
   *
   * @internal
   * Not using `pool_id_t`; that type does not as of this writing strictly need to be exposed
   * publicly (non-`detail/`).
   */
  uint64_t m_id = 0;

  /// Pool size in bytes.
  size_t m_sz = 0;

  /// Unique (machine-wide, between boots) ID of containing SHM-arena.
  Uniq_arena_id m_uniq_arena_id;

  /**
   * If applicable (otherwise left at default value: 1), the use-count or ref-count of the particular
   * underlying SHM pool (<=> name in file-system) in the containing context.  That's vague, but it just
   * depends on the context.  As of this writing there is one known place where this has meaning:
   *   - When reporting the global (per SHM-provider; e.g. for SHM-jemalloc) set of known borrowed SHM-pools,
   *     `m_use_count` indicates the number of `Shm_session`s that are currently each borrowing that particular
   *     pool; there is only one pool-handle + memory-mapped area, but `m_use_count` sessions are able to
   *     access data therein.
   */
  size_t m_use_count = 1;
}; // struct Shm_pool_info

/**
 * The memory-manager-level statistics for a single native allocator-arena (for SHM-jemalloc: one jemalloc
 * arena) backing an arena-lend `Shm_arena` (e.g. jemalloc::Ipc_arena).
 *
 * @see Pool_stats: A peer grouping of per-arena stats; it must be separate due to not being TL-sharded or
 *      coming from an external data source when internally collected; therefore the method of return is different
 *      from a `*this`.
 * @see Sharded_stats: A peer grouping of per-arena stats; we are separate from it due to coming from an
 *      external data source (memory manager: e.g., jemalloc) as opposed to from internal algorithms/data.
 *      (They could still be bundled together.  Nevertheless the subject matter/data source are, we feel,
 *      so sharply different that Memory_manager_stats simply should remain separate from all others.)
 *
 * To summarize: An arena-lending SHM-provider (at least: SHM-jemalloc) manages SHM pools and sessions, as well as
 * cross-process garbage-collectable *first-class objects* (as `.construct<T>()`ed and `.borrow_object<T>()`ed);
 * but when it is required to actually allocate (or later deallocate) a buffer in SHM, the SHM-provider uses
 * a *memory manager* (for SHM-jemalloc, that's jemalloc).  (In the middle of an allocation, sometimes
 * the memory mnanager needs a new vaddr area, which it requests of SHM-jemalloc via customization hook, so we
 * create a SHM-pool and... so it goes; the memory manager and the leveraging SHM-provider thus cooperate.)
 *
 * So, Memory_manager_stats tracks stats that happen to have these general properties:
 *   - They are about the memory manager.
 *   - They are usually collected by the memory manager itself; so Flow-IPC queries its stats subsystem
 *     (e.g., jemalloc's robust stats/profiling module) and organizes it in useful shapes here.  (All of the
 *     information -- and surely more -- is available directly if desired.  Use, e.g.,
 *     jemalloc::Ipc_arena::get_jemalloc_arena_id() to grab the equivalent of #m_native_arena_id; then
 *     query the memory manager itself.  We aim to give the most relevant stuff here, though, for convenience.)
 *   - They include (but aren't limited to) stats such that accurately accounting them means some kind of
 *     tracking inside every `allocate()` and `deallocate()` ([de]allocate buffer) call.  We stay away from
 *     doing this in Flow-IPC proper, due to the performance risk at least, but memory managers know how to do
 *     it well.
 *   - `_hi_wmark` (e.g.: Vaddr::m_mapped_sz_hi_wmark) fields are not reported by memory manager directly (if they
 *     were, we'd take them!), so their semantics have the same limitation as within TL-sharded `struct`s (e.g.: see
 *     Shared_stats doc header).
 *
 * This `struct` is provider-agnostic by design: it is the stat-set of arena_lend::Memory_manager (the base
 * abstraction); a given SHM-provider impl fills what it can and leaves the rest at default (usually zero).
 *
 * The first (as of this writing only) arena-lend provider is SHM-jemalloc, which fills these from jemalloc's
 * `mallctl()` statistics surface.
 *
 * ### How it is consumed ###
 * Unlike the continuously-sampled (and often TL-sharded) stat-sets, these are *raw readings* obtained from the
 * provider at stat-consumption time -- a single consistent snapshot per consume (for SHM-jemalloc: one
 * `mallctl("epoch")` refresh, then a batch of reads).  (Internally this uses
 * `flow::util::stat::stats_since_reset_state()`/`stats_mark_reset_state()`.)
 *
 * Per-arena context: a `Shm_arena` exposes one Memory_manager_stats per native arena it maintains, each
 * self-identified by #m_uniq_arena_id (<=> `Shm_arena`) and #m_native_arena_id (<=> native arena), both emitted
 * as GAUGEs.
 *
 * For SHM-jemalloc, as of this writing:
 *   - The `Shm_arena` is in fact jemalloc::Ipc_arena.
 *   - There is exactly one jemalloc (native) arena per `Ipc_arena`.  (There are as of this writing plans to
 *     potentially add more, perhaps for better multi-core behavior.)
 *     - It is available by itself via jemalloc::Ipc_arena::get_jemalloc_arena_id().
 */
struct Memory_manager_stats
{
  // Types.

  /**
   * Sub-group: the arena's virtual-address-space / page-state footprint -- how much memory it currently holds
   * from the OS and in what state.  All members are GAUGEs in bytes.
   *
   * Approximate relationships (sizes are post size-class quantization; see jemalloc docs for precise meanings):
   *   - #m_mapped_sz ~= #m_active_sz + #m_inactive_resident_sz + #m_inactive_muzzy_sz + allocator-metadata.
   *   - #m_resident_sz ~= #m_active_sz + #m_inactive_resident_sz + allocator-metadata (excludes muzzy + retained).
   *   - #m_retained_sz is separate: space excluded from #m_mapped_sz book-keeping yet deliberately kept
   *     mapped, saved for cheap reuse (see its doc header).
   */
  struct Vaddr
  {
    // Data.

    /**
     * (Gauge) Total bytes currently mapped from the OS into the arena's address space.  See sub-group doc header.
     *
     * Reminder: While important, this does not represent RAM taken from OS/unavailable for general use by OS
     * (for other applications): a given page generally start off as *not* resident (a/k/a dirty); once
     * read/written (even 1 byte) it becomes resident/dirty (not available for general use by OS).  (A page by
     * default is along the lines of 4Ki bytes.)  E.g.: it is entirely possible and not even unusual
     * for a particular vaddr area (in our case backed by a SHM pool) to be, say, mapped for 100 megabytes --
     * but only 500 kilobytes of it correspond to actual RAM use (#m_resident_sz + possibly a bit more).
     */
    size_t m_mapped_sz = 0;

    /**
     * Max of #m_mapped_sz observed so far.
     *
     * @note For this and nearby `_hi_wmark`s: Due to the way these data are acquired from jemalloc-stats (or
     *       equivalent), the high-water-mark values here cannot be true max-values since the last reset but merely
     *       from among the times `*this` is sampled via stat-consumption, plus the last reset.
     */
    size_t m_mapped_sz_hi_wmark = 0;

    /// (Gauge) Total physically-resident bytes (RSS-like).  See sub-group doc header.
    size_t m_resident_sz = 0;

    /// Max of #m_resident_sz observed so far.
    size_t m_resident_sz_hi_wmark = 0;

    /**
     * (Gauge) Bytes in pages backing live (active) allocations (~= live allocated bytes + small allocator metadata).
     *
     * @see Memory_manager_stats::Alloc subgroup doc header discussion may be relevant.
     */
    size_t m_active_sz = 0;

    /// Max of #m_active_sz observed so far.
    size_t m_active_sz_hi_wmark = 0;

    /**
     * (Gauge) Not-active (so freed from/not yet used for allocated user-buffers; contrast with #m_active_sz) bytes
     * that are physically resident (not available for general use by OS).  This is  cheaply reusable in-place (no
     * syscall required; just read/write at vaddr).
     *
     * For SHM-jemalloc: *dirty* pages (`pdirty` x page-size).
     */
    size_t m_inactive_resident_sz = 0;

    /// Max of #m_inactive_resident_sz observed so far.
    size_t m_inactive_resident_sz_hi_wmark = 0;

    /**
     * (Gauge) Not-active (see also #m_inactive_resident_sz) bytes that the memory-manager formerly held as resident
     * but has since offered to the OS for reclamation into general availability via *lazy* purge.  (In Linux: this
     * is `madvise(MADV_FREE)` -- OS may take the physical pages at its leisure; contents stay valid unless/until
     * it does.)  The OS having possibly not (yet) taken up the offer is why this is "inactive-but-possibly-resident"
     * rather than simply freed; either way it is counted in this stat-member.
     *
     * For SHM-jemalloc: *muzzy* pages (`pmuzzy` x page-size).
     *
     * @note The harsher ways of returning RAM are *not* counted here, as pages so released are gone from
     *       residence immediately: *forced* purge (generically in Linux `madvise(MADV_DONTNEED)`; for SHM-jemalloc:
     *       SHM-object hole-punching) and un-memory-mapping (`munmap()`) the whole containing vaddr area
     *       (a/k/a extent, pool).  Note also that SHM-jemalloc configures its memory manager *without* a
     *       lazy-purge ability (there is no SHM analog of `MADV_FREE`'s recoverability): its physical give-back
     *       is via the hole-punch (#m_retained_sz).  That is to say: as of this writing this stat-member should
     *       show zero.
     */
    size_t m_inactive_muzzy_sz = 0;

    /// Max of #m_inactive_muzzy_sz observed so far.
    size_t m_inactive_muzzy_sz_hi_wmark = 0;

    /**
     * (Gauge) Bytes of once-used vaddr space the memory-manager has excluded from #m_mapped_sz book-keeping
     * yet deliberately keeps mapped -- *not* returned to the OS -- saved for cheap reuse (no memory-map op
     * needed; the range is simply handed back out on a suitable future allocation).  For SHM-jemalloc this
     * state is entered when jemalloc asks to remove unused extent (SHM-pool) space, and policy declines
     * (as of this writing in our impl we indeed always decline): the backing physical pages are then released
     * via SHM-object hole-punching -- `fallocate(FALLOC_FL_PUNCH_HOLE)` et al in Linux -- while the vaddr range
     * and its SHM-pool remain intact.
     *
     * @internal
     * SHM-jemalloc: See jemalloc::Ipc_arena::optional_remove_shm_pool().
     */
    size_t m_retained_sz = 0;

    /// Max of #m_retained_sz observed so far.
    size_t m_retained_sz_hi_wmark = 0;
  }; // struct Vaddr

  /**
   * Sub-group: raw allocation activity -- cumulative counts of memory-manager allocate/deallocate operations
   * (e.g. each `Arena::construct<T>()` plus each nested container allocation), and the currently-live count/bytes
   * derived from them.
   *
   * These count *raw* (low-level) allocations, NOT first-class `construct()`ed objects (those are tracked
   * elsewhere, e.g. shm::stat::Owner_obj_stats): one `construct<T>()` of a container-bearing `T` may trigger many raw
   * allocations.
   *
   * The cumulative counts are ACCUMULATORs (reported relative to the last reset; see Memory_manager_stats "How it
   * is consumed"); the live count/bytes are GAUGEs (absolute), each with a high-water mark.
   *
   * Regarding HI_WMARKs: See note for Vaddr::m_mapped_sz_hi_wmark.
   *
   * SHM-jemalloc: How these stats are accounted
   * -------------------------------------------
   * ### Why discuss SHM-jemalloc specifically, here? ###
   * Formally speaking SHM-jemalloc is "merely" one SHM-provider of the arena-lend type.  It is however the first
   * such SHM-provider; and as of this writing it is the only one.  Therefore, out of pragmatism, we include
   * these notes here, even though this is the shm::arena_lend namespace as opposed to shm::arena_lend::jemalloc.
   * If or when more arena-lend SHM-providers are added, we may choose to reorganize this documentation; but
   * until then this discussion should be here for posterity.
   *
   * Now to the actual topic at hand: *How does (SHM-)jemalloc (de)allocation work + how is it accounted in `*this`*?
   *
   * ### Significant mechanism: jemalloc tcache ###
   * jemalloc -- and plausibly future arena-lend providers -- adds a *thread cache* (*tcache*) between the
   * "user" and the arena proper: a small, per-thread, lock-free free-list (one per size-class) of regions kept
   * for immediate reuse.  Its purpose is purely performance: an allocate/deallocate the tcache can service needs
   * no arena-level locking.  SHM-jemalloc uses a tcache on *every* allocation (one tcache per thread,
   * per-jemalloc-arena #m_native_arena_id).  (This is fully skipped, however, by enabling build flag
   * `IPC_SHM_ARENA_LEND_JEMALLOC_NO_TCACHE`.  Let us assume it is not skipped, as tcache is a valuable perf
   * mechanism.)
   *
   * Crucially the tcache holds no memory of its own: its entries are *pointers* to regions that live
   * in the arena's normal *slabs* (blocks of space initially available for future allocated-buffers).  A region
   * "in the tcache" has merely been taken off its slab's free-list -- so from the arena's accounting standpoint
   * it is *allocated* (or *active*) -- but it is held by the cache for (mega-quick) reuse, not by the user's
   * allocated-buffer(s).
   *
   * @note The stuff about slabs and free-lists, while important, might be at a higher level of lower interest.
   *       At its core, a tcache is a (thread-local!) store of would-be allocate-op-returned pointers that
   *       sits as an intermediary between the actual arena that can dole such pointers out originally; and the
   *       user who ultimately wants to use these pointers.  Grabbing it from that intermediary, which has
   *       pre-grabbed it already from source, is quicker than really-grabbing it from that source.  Returning it
   *       to that intermediary is itself quicker than really-returning it to the original source, *and*
   *       it makes it available as-if-pre-grabbed for the next grab by whichever allocate-op of a compatible
   *       size.  (It is even possible to slow-grab directly from source but then return to tcache intermediary;
   *       this is not -- from jemalloc's PoV -- special.  It's just a pointer store ultimately, is the point.)
   *
   * @see Thread_cache doc header for a deeper look at how jemalloc tcache works.
   *
   * The upshot for the stats here is that jemalloc's counters (whether the `_sz` byte-counts or the `_count`s)
   * count regions becoming used/allocated or unused/free from the "point of view of the slabs,"
   * updating which information requires a *costly mutex-lock*.  A given `allocate()` or `deallocate()` call, that
   * goes potentially through tcache, might well not update those counters, because only updating information
   * in the (thread-local) tcache is required.  Over time, though, the counters will be updated -- but in a
   * lumpier/batchier way.
   *
   * Thus with the tcache in play:
   *   - Cumulative #m_alloc_count / #m_alloc_sz (and the dealloc counterparts) measure region churn between
   *     slabs and caches, not the count/bytes of user calls.  They differ from the user-call counts in *either*
   *     direction: a low-traffic class shows more region-churn than calls (a batch-fill overshoots demand,
   *     caching the surplus), while a hot, recycled class can show fewer (regions ping-pong between user and
   *     cache without touching a slab).  The size of the gap depends on the tcache churn pattern.
   *   - The live #m_live_count / #m_live_sz similarly reflect what is currently taken from slabs, which includes
   *     regions in tcaches (therefore *not* used by actual user allocated-buffers).  So `m_live_*` members
   *     overstate what the user actually holds, by the current cache contents.
   *     - Cf. Vaddr::m_active_sz: its pages stay "active" while any of a slab's regions sit in a cache -- same effect,
   *       same cause.
   *
   * If the tcache use (by SHM-jemalloc) is compiled-out (see above), then all of this collapses: each counter
   * tracks user `allocate()`/`deallocate()` directly, and the live values are what the user holds.  That's nice
   * for understanding the stats in real-time fashion, but in terms of perf it means more mutex-locking inside
   * jemalloc.
   *
   * @note A more truthful (assuming truth = areas allocated/deallocated/used/free from the user's PoV) per-call
   *       counter exists in jemalloc (`nrequests`) but has no symmetric dealloc counterpart, so it
   *       cannot feed the gauge derivations `m_live_*`; hence we account via `nmalloc`/`ndalloc` for all non-histogram
   *       members, and histogram #m_histo_alloc_count_by_sz inherits the same view and the same caveat.
   *       Nor do we provide any `nrequests`-based stat, though a by-size *request-count* histogram would be a
   *       nice complement to #m_histo_alloc_count_by_sz: with explicit tcaches, jemalloc files each tcache's
   *       locally-tallied `nrequests` into the books of the tcache's *associated* arena -- an association
   *       chosen at tcache-creation via a thread-internal binding that public jemalloc API cannot influence
   *       (`thread.arena` affects only the application-arena binding; no internal-arena counterpart exists) --
   *       so those tallies land in an unrelated automatic arena, and per-*our*-arena request-counts are
   *       unobtainable.  (Verified against jemalloc 5.3.0 source.  Only tcache-ineligible -- huge -- size
   *       classes would report; not worth a misleading partial stat.)
   */
  struct Alloc
  {
    // Data.

    /// Cumulative count of allocate ops (for SHM-jemalloc: jemalloc small + large `nmalloc`).
    uint64_t m_alloc_count = 0;

    /// Cumulative count of deallocate ops (for SHM-jemalloc: jemalloc small + large `ndalloc`).
    uint64_t m_dealloc_count = 0;

    /**
     * Cumulative bytes allocated (for SHM-jemalloc: sum over size-classes of `nmalloc` x size-class-byte-size).
     *
     * @note Post size-class quantization (see #m_live_sz): bytes the heap handed out, not user-requested sizes.
     */
    uint64_t m_alloc_sz = 0;

    /// Cumulative bytes deallocated (for SHM-jemalloc: sum over size-classes of `ndalloc` x size-class-byte-size).
    uint64_t m_dealloc_sz = 0;

    /// (Gauge) Currently-live allocation count (= #m_alloc_count minus #m_dealloc_count).
    uint64_t m_live_count = 0;

    /// Max of #m_live_count observed so far.
    uint64_t m_live_count_hi_wmark = 0;

    /**
     * (Gauge) Currently-live allocated bytes (for SHM-jemalloc: jemalloc small + large `allocated`).
     *
     * @note This is post size-class quantization -- i.e. "what the heap holds," not the sum of user-requested sizes.
     */
    size_t m_live_sz = 0;

    /// Max of #m_live_sz observed so far.
    size_t m_live_sz_hi_wmark = 0;

    /**
     * Cumulative allocation count broken down by allocation size-class, as a histogram: each bucket counts the
     * cumulative allocations of a certain size-class; so a user-request for allocation of N bytes results in
     * a `++` for the bucket whose `Histogram_counter` bounds are [N or lower, N or higher].
     *
     * A caveat here:
     *
     * @note The previous description ignores the tcache mechanism.  See Alloc doc header which explains
     *       how tcache can affect these semantics.
     *
     * For SHM-jemalloc this is jemalloc's small `bins` followed by its large `lextents`, concatenated into one
     * ascending sequence: the small/large split is a jemalloc implementation detail.  We combine them into
     * this one histogram.  (The small-vs-large boundary remains visible in the bucket sizes, for SHM-jemalloc
     * around 14-16Ki bytes.)
     *
     * @note The bucket structure of the histogram is determined by the memory manager's size classes which (1) are
     *       process-constant but (2) reported at run-time by the memory manager (e.g.: jemalloc).  This is
     *       unknown at compile-time, so we use some placeholder values (`{2, 1}`) in the default-init near this
     *       doc-comment; but this structure is never used.  The placeholder bounds in the default-initializer
     *       are overwritten when actually emitting stats into a `*this`, most notably at stat-consume (via public
     *       `Shm_arena` API invoked by the user) time.
     */
    flow::util::stat::Histogram_counter m_histo_alloc_count_by_sz{2, 1};
  }; // struct Alloc

  // Data.

  /// Unique (machine-wide, between boots) ID of managing Flow-IPC SHM-arena.  See also Shm_pool_info::m_uniq_arena_id.
  Uniq_arena_id m_uniq_arena_id;

  /**
   * The memory-manager's native arena ID of arena described in `*this`.  For SHM-jemalloc this is the
   * jemalloc arena ID.
   *
   * Stored as `uint64_t` rather than a provider-specific ID type which need not be exposed publicly here.
   */
  uint64_t m_native_arena_id = 0;

  /// See Vaddr doc header.
  Vaddr m_vaddr;

  /// See Alloc doc header.
  Alloc m_alloc;
}; // struct Memory_manager_stats

// Template implementations.

template<typename Visitor>
void declare_stats(std::string name_prefix, const Owner_obj_stats* src_stats, Owner_obj_stats* target_stats,
                   Visitor&& visitor)
{
  FLOW_UTIL_STAT_DECLARE(m_sync_destroy_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_drain_destroy_count, ACCUMULATOR);
}

template<typename Visitor>
void declare_stats(std::string name_prefix,
                   const Zombie_obj_reaper_stats* src_stats, Zombie_obj_reaper_stats* target_stats, Visitor&& visitor)
{
  FLOW_UTIL_STAT_DECLARE(m_scans, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_exhaustive_scans, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_hints_sufficient_scans, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_fifo_scans, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_fifo_exhausted_scans, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_histo_zombie_ct_estimate, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_events.m_hints_checked, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_events.m_hints_stale, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_events.m_hints_found_resurrected_zombie, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_events.m_fifo_objs_checked, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_events.m_fifo_objs_live, ACCUMULATOR);
}

template<typename Visitor>
void declare_stats(std::string name_prefix,
                   const Obj_db_aux_pool_stats* src_stats, Obj_db_aux_pool_stats* target_stats, Visitor&& visitor)
{
  FLOW_UTIL_STAT_DECLARE(m_aux_pool_count, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_aux_pool_count_hi_wmark, m_aux_pool_count);
  FLOW_UTIL_STAT_DECLARE(m_use_ct_active_quanta, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_use_ct_active_quanta_hi_wmark, m_use_ct_active_quanta);
  FLOW_UTIL_STAT_DECLARE(m_use_ct_quanta, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_use_ct_quanta_hi_wmark, m_use_ct_quanta);
  FLOW_UTIL_STAT_DECLARE(m_resident_sz, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_resident_sz_hi_wmark, m_resident_sz);
  FLOW_UTIL_STAT_DECLARE(m_mapped_sz, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_mapped_sz_hi_wmark, m_mapped_sz);
}

template<typename Visitor>
void declare_stats(std::string name_prefix,
                   const Obj_db_aux_pool_global_stats* src_stats, Obj_db_aux_pool_global_stats* target_stats,
                   Visitor&& visitor)
{
  FLOW_UTIL_STAT_DECLARE(m_client_tl_aux_pool_live_hndls, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_client_tl_aux_pool_live_hndls_hi_wmark, m_client_tl_aux_pool_live_hndls);
  FLOW_UTIL_STAT_DECLARE(m_client_tl_aux_pool_hndl_open_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_client_tl_aux_pool_hndl_open_fail_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_client_tl_aux_pool_hndl_reap_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_client_tl_aux_pool_hndl_reap_check_count, ACCUMULATOR);
}

template<typename Visitor>
void declare_stats(std::string name_prefix,
                   const Owner_pool_stats* src_stats, Owner_pool_stats* target_stats, Visitor&& visitor)
{
  FLOW_UTIL_STAT_DECLARE(m_pool_create_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_pool_create_sz, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_pool_destroy_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_pool_destroy_sz, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_pool_optional_destroy_request_count, ACCUMULATOR);
}

template<typename Visitor>
void declare_stats(std::string name_prefix,
                   const Pool_stats* src_stats, Pool_stats* target_stats, Visitor&& visitor)
{
  declare_stats(name_prefix + "owner.",
                src_stats ? &src_stats->m_owner_pool : nullptr,
                target_stats ? &target_stats->m_owner_pool : nullptr,
                visitor);
  declare_stats(name_prefix + "dbaux.",
                src_stats ? &src_stats->m_obj_db_aux_pool : nullptr,
                target_stats ? &target_stats->m_obj_db_aux_pool : nullptr,
                visitor);
}

template<typename Visitor>
void declare_stats(std::string name_prefix,
                   const Borrower_pool_stats* src_stats, Borrower_pool_stats* target_stats, Visitor&& visitor)
{
  FLOW_UTIL_STAT_DECLARE(m_uniq_arena_id.m_id1, GAUGE);
  FLOW_UTIL_STAT_DECLARE(m_uniq_arena_id.m_id2, GAUGE);
  FLOW_UTIL_STAT_DECLARE(m_arena_register_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_arena_deregister_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_arena_first_register_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_arena_last_deregister_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_n_borrowed_arenas, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_n_borrowed_arenas_hi_wmark, m_n_borrowed_arenas);
  FLOW_UTIL_STAT_DECLARE(m_pool_register_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_pool_deregister_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_pool_open_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_pool_close_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_n_open_pools, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_n_open_pools_hi_wmark, m_n_open_pools);
  FLOW_UTIL_STAT_DECLARE(m_mapped_sz, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_mapped_sz_hi_wmark, m_mapped_sz);
}

template<typename Visitor>
void declare_stats(std::string name_prefix,
                   const Owner_pool_lookup_global_stats* src_stats, Owner_pool_lookup_global_stats* target_stats,
                   Visitor&& visitor)
{
  FLOW_UTIL_STAT_DECLARE(m_pool_rev_lookup_tl_cache_count, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_pool_rev_lookup_tl_cache_count_hi_wmark, m_pool_rev_lookup_tl_cache_count);
}

template<typename Visitor>
void declare_stats(std::string name_prefix,
                   const Borrower_pool_lookup_global_stats* src_stats, Borrower_pool_lookup_global_stats* target_stats,
                   Visitor&& visitor)
{
  FLOW_UTIL_STAT_DECLARE(m_pool_rev_lookup_tl_cache_count, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_pool_rev_lookup_tl_cache_count_hi_wmark, m_pool_rev_lookup_tl_cache_count);
  FLOW_UTIL_STAT_DECLARE(m_pool_fwd_lookup_tl_cache_count, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_pool_fwd_lookup_tl_cache_count_hi_wmark, m_pool_fwd_lookup_tl_cache_count);
}

template<typename Visitor>
void declare_stats(std::string name_prefix, const Shm_pool_info* src_stats, Shm_pool_info* target_stats,
                   Visitor&& visitor)
{
  FLOW_UTIL_STAT_DECLARE(m_id, GAUGE);
  FLOW_UTIL_STAT_DECLARE(m_sz, GAUGE);
  FLOW_UTIL_STAT_DECLARE(m_uniq_arena_id.m_id1, GAUGE);
  FLOW_UTIL_STAT_DECLARE(m_uniq_arena_id.m_id2, GAUGE);
  FLOW_UTIL_STAT_DECLARE(m_use_count, GAUGE);
}

template<typename Visitor>
void declare_stats(std::string name_prefix,
                   const Memory_manager_stats* src_stats, Memory_manager_stats* target_stats, Visitor&& visitor)
{
  FLOW_UTIL_STAT_DECLARE(m_uniq_arena_id.m_id1, GAUGE);
  FLOW_UTIL_STAT_DECLARE(m_uniq_arena_id.m_id2, GAUGE);
  FLOW_UTIL_STAT_DECLARE(m_native_arena_id, GAUGE);
  FLOW_UTIL_STAT_DECLARE(m_vaddr.m_mapped_sz, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_vaddr.m_mapped_sz_hi_wmark, m_vaddr.m_mapped_sz);
  FLOW_UTIL_STAT_DECLARE(m_vaddr.m_resident_sz, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_vaddr.m_resident_sz_hi_wmark, m_vaddr.m_resident_sz);
  FLOW_UTIL_STAT_DECLARE(m_vaddr.m_active_sz, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_vaddr.m_active_sz_hi_wmark, m_vaddr.m_active_sz);
  FLOW_UTIL_STAT_DECLARE(m_vaddr.m_inactive_resident_sz, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_vaddr.m_inactive_resident_sz_hi_wmark, m_vaddr.m_inactive_resident_sz);
  FLOW_UTIL_STAT_DECLARE(m_vaddr.m_inactive_muzzy_sz, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_vaddr.m_inactive_muzzy_sz_hi_wmark, m_vaddr.m_inactive_muzzy_sz);
  FLOW_UTIL_STAT_DECLARE(m_vaddr.m_retained_sz, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_vaddr.m_retained_sz_hi_wmark, m_vaddr.m_retained_sz);
  FLOW_UTIL_STAT_DECLARE(m_alloc.m_alloc_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_alloc.m_dealloc_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_alloc.m_alloc_sz, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_alloc.m_dealloc_sz, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_alloc.m_live_count, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_alloc.m_live_count_hi_wmark, m_alloc.m_live_count);
  FLOW_UTIL_STAT_DECLARE(m_alloc.m_live_sz, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_alloc.m_live_sz_hi_wmark, m_alloc.m_live_sz);
  FLOW_UTIL_STAT_DECLARE(m_alloc.m_histo_alloc_count_by_sz, ACCUMULATOR);
}

template<typename Visitor>
void declare_stats(std::string name_prefix,
                   const Sharded_stats* src_stats, Sharded_stats* target_stats,
                   Visitor&& visitor)
{
  name_prefix += "obj."; // Similar rationale as in `declare_stats(shm::classic::stat::Arena_stats)`.
  declare_stats(name_prefix + "own.",
                src_stats ? &src_stats->m_owner_obj : nullptr,
                target_stats ? &target_stats->m_owner_obj : nullptr,
                visitor);
  declare_stats(name_prefix + "own.", // Same prefix; the names in m_owner[_arena_lend] are chosen synergistically.
                src_stats ? &src_stats->m_owner_obj_arena_lend : nullptr,
                target_stats ? &target_stats->m_owner_obj_arena_lend : nullptr,
                visitor);
  declare_stats(name_prefix + "lnd.",
                src_stats ? &src_stats->m_lend_obj : nullptr,
                target_stats ? &target_stats->m_lend_obj : nullptr,
                visitor);
  declare_stats(name_prefix, // Individual names are chosen so that "live." (or whatever) would be unnecessary.
                src_stats ? &src_stats->m_live_obj : nullptr,
                target_stats ? &target_stats->m_live_obj : nullptr,
                visitor);
  declare_stats(name_prefix + "zombie.",
                src_stats ? &src_stats->m_zombie_obj_reaper_main : nullptr,
                target_stats ? &target_stats->m_zombie_obj_reaper_main : nullptr,
                visitor);
  declare_stats(name_prefix + "zb.drn.",
                src_stats ? &src_stats->m_zombie_obj_reaper_drain : nullptr,
                target_stats ? &target_stats->m_zombie_obj_reaper_drain : nullptr,
                visitor);
}

} // namespace ipc::shm::arena_lend::stat
