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

#include "ipc/shm/arena_lend/detail/arena_lend_fwd.hpp"
#include "ipc/shm/arena_lend/shm_pool.hpp"
#include "ipc/shm/arena_lend/arena_lend_stats.hpp"
#include <flow/util/thread_lcl.hpp>
#include <flow/util/util.hpp>
#include <flow/util/stat/stat_set_list.hpp>
#include <flow/util/stat/stat_set.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <type_traits>

namespace ipc::shm::arena_lend::detail
{

// Types.

/**
 * Value type in the reverse-lookup map: identifies the pool and, when `LOOKUP_CAN_FAIL` is `true` for the
 * containing lookup structure, also carries the pool size so that a range-check can determine whether a
 * given vaddr actually falls inside the pool (as opposed to merely being above its base).
 */
struct Pool_by_base
{
  // Data.

  /// Pool identifier.
  pool_id_t m_id;
  /**
   * Pool size in bytes, or 0 when the lookup context guarantees that every address is in some pool
   * (`LOOKUP_CAN_FAIL == false`), rendering the size check unnecessary.
   */
  Shm_pool::size_t m_sz_or_0 = 0;
};

/**
 * Global stats singleton for either Owner_pool_lookup_global_stats or Borrower_pool_lookup_global_stats, per
 * SHM-provider (as identified by `Shm_arena` type).
 *
 * Stat-consumers: call stats() to read immutable Owner_pool_lookup_global_stats or Borrower_pool_lookup_global_stats;
 * stats_reset() to reset same.
 *
 * Stat-updaters: call stats_mutable() to update same.
 *
 * @tparam Shm_arena_t
 *         A thing that discriminates between SHM providers; by convention the arena type (such as jemalloc::Ipc_arena).
 *         Its API is not accessed.
 * @tparam OWNER_ELSE_BORROWER
 *         `true` if tracking owner-side activity; #Stats is stat::Owner_pool_lookup_global_stats.
 *         `false` if borrower; stat::Borrower_pool_lookup_global_stats.
 */
template<typename Shm_arena_t, bool OWNER_ELSE_BORROWER>
class Pool_lookup_global_stats
{
public:
  // Constants.

  /// Constant for template parameter.
  static constexpr bool S_OWNER_ELSE_BORROWER = OWNER_ELSE_BORROWER;

  // Types.

  /// Short-hand for the template parameter.
  using Shm_arena = Shm_arena_t;

  /// The actual stat-set type.
  using Stats = std::conditional_t<S_OWNER_ELSE_BORROWER,
                                   arena_lend::stat::Owner_pool_lookup_global_stats,
                                   arena_lend::stat::Borrower_pool_lookup_global_stats>;
  // Methods.

  /**
   * Returns reference-to-`const` stat-set, carrying information that is global but per-template-instance-type.
   * @return See above.  See also doc header of the #Stats alias's target type.
   */
  static const Stats& stats();

  /**
   * Mutable version of stats(), for stat-updating.
   * @return See above.
   */
  static Stats* stats_mutable();

  /// Resets stats().  The formal meaning of a reset is discussed in `flow::util::stat` doc header.
  static void stats_reset();
}; // struct Pool_lookup_global_stats

/**
 * (Internal-use) `Thread_local_pool_lookup_rev<R>.s_registry`, where `R` is a distinct pool-repository type,
 * enables high-perf reverse-lookup from a vaddr to the containing SHM-pool's ID and
 * offset in bytes from the pool-base vaddr.
 *
 * As far as we know, if one needs multithreaded such lookup with high performance, then whether owner-side or
 * borrower-side one should use Thread_local_pool_lookup_rev.  Note that `Thread_local_pool_lookup_rev<R1>`
 * and `Thread_local_pool_lookup_rev<R2>` are completely independent, if `R1` and `R2` are different types.
 *
 * @see This is a key building block for the rev-lookup operations of Owner_shm_pool_repository (and
 *      Shm_pool_offset_ptr operating against it, owner-side) and of
 *      session::shm::arena_lend::detail::Borrower_shm_pool_collection_repository (ditto but borrower-side).
 *      In both cases this is done indirectly via Shm_pool_repo_lookup_core_rev which uses us directly.
 *      The rationale of separating us (Thread_local_pool_lookup_rev) from Shm_pool_repo_lookup_core_rev is the
 *      analogous nature of us and our forward-lookup cousin Thread_local_pool_lookup_fwd.
 *
 * ### How to use ###
 * To actually perform the reverse-lookup operation, whose performance we are optimizing, use:
 *   - `Thread_local_pool_lookup_rev<Repository>::s_registry.this_thread_state()->lookup(vaddr, ...)`.
 *
 * The user has a rigid responsibility for loading the canonical, and complete, set of SHM-pool base vaddrs and
 * their respective #pool_id_t values.  This consists of two operations, and you must help us perform each:
 *   - *Pull*: A `*this` is auto-created in new thread Ti the first time it is needed.
 *     Therefore the new `*this` *pulls* the current canonical state from somewhere that you have it.
 *   - *Push*: You current canonical state is updated (e.g., memory-manager created new pool).
 *     Therefore you *push* that state, wherever you've got it, into *every* extant `*this`.
 *
 * ### How to use: pull ###
 * During the per-thread creation of a `*this`, the following expression shall be invoked on-demand:
 *   - `Repository::get_instance().tl_copy_rev_pull_pools(&M)`, where
 *   - `M` is #Map (vaddr -> Pool_by_base), and
 *   - that expression shall load `M` with the complete current information about base-vaddrs and pool info.
 *   - A central (1-1 with the type #Repository) mutex `Mtx` shall be locked;
 *     and it is the same mutex that shall be locked during a push (see below).
 *
 * ### How to use: push ###
 * When the canonical such structure is modified in repo #Repository (addition,
 * removal, init), that thread must do the following (certainly before any relevant subsequent lookup() calls):
 *   - `Thread_local_pool_lookup_rev<Repository>::s_registry.while_locked() {...}`.
 *   - Inside the `{...}`, obtain and loop-through each thread's extant thread-local
 *     Thread_local_pool_lookup_rev cache: `state_per_thread` from `.while_locked()` is a map whose
 *     keys are each a `Thread_local_pool_lookup_rev<Repository>`.
 *   - For each of those: `cache.push_pools(M)`, where `M` is the complete new mapping.
 *   - The mutex that shall be locked inside the `{...}` shall be the same mutex `Mtx` used during any pull
 *     (see above).
 *
 * ### Design / Impl ###
 * The basic lookup algorithm, even in one thread only, is that one needs a sorted-by-base-vaddr map from
 * base-vaddr to pool ID, and with logarithmic time complexity lookup() shall perform a binary-search
 * (as in `std::map::lower_bound()`) lookup of a nearby (versus the input `void*`) base-vaddr plus a possibly
 * adjustment of the resulting iterator.  (That gets the pool ID; the offset is just the address minus the
 * located base-vaddr.)
 *
 * For multithreaded access, the naive approach (full synchronization via one central mutex) exhibits terrible
 * lock contention under high load.  Therefore we looked for something involving a thread-local lookup().
 * Flow's `Thread_local_state_registry` provides TL semantics; we expose a global #s_registry of that type;
 * so `s_registry::this_thread_state()->lookup()` is how one would perform the lookup.
 *
 * In short: a given `*this` shall contain an up-to-date copy of the canonical set of bases and corresponding pool info.
 * Therefore one must copy-over the canonical such map into the thread-local #m_pools_sorted_by_base
 * *for each thread*, whenever said canonical map is updated (in whatever *pusher thread* happens to be
 * performing that update).
 *
 * Naturally this creates the need for locking -- whose perf characteristics require scrutiny.  The main critical
 * mutex is a *per-thread* mutex (hence a non-`static` member, #m_pools_sorted_by_base_mutex).  It is to be locked:
 *   - (Frequent) In end-user thread: lookup() -- around the lookup of #m_pools_sorted_by_base.
 *   - (Rare) In pusher thread: push_pools() -- around the replacement of #m_pools_sorted_by_base.
 *   - Since, if one assumes for simplicity of exposition that there's only one update happening at a time,
 *     this implies *at most* 2 threads contending for the lock, and usually only 1, this is perf-acceptable.
 *
 * There is another important lock involved.  Per `flow::util::Thread_local_state_registry`, when looping
 * over extant thread-local objects (`*this`es in our case) -- a key service of that utility module -- one
 * shall lock a built-in central mutex via `Thread_local_state_registry.while_locked(F)`, where `F()` contains
 * the code to copy-over the canonical map into individual `*this`es.  Hence the directive to use
 * `state_per_thread` in `F(state_per_thread)` to obtain the `*this`es.
 *
 * @note Tip: Since it is likely that the user code wants to protect against concurrent updates to the canonical map,
 *       it can take advantage of the mutex mentioned in the previous paragraph.  That is `F()` given to
 *       `while_locked()` can perform the update of the canonical map first as well.
 *
 * Thus, during an update push_pools() code-flow, for a given `*this`, the central lock is locked, then the per-thread
 * lock, then #m_pools_sorted_by_base is updated, then the locks are unlocked (in opposite order).
 *
 * During a pull code-flow, only the central lock is locked, then #m_pools_sorted_by_base is initialized (before
 * being available to the user).
 *
 * During the lookup() code-flow, only the per-thread lock is locked.
 *
 * There is no deadlock.
 *
 * ### Stats -- If #S_LOOKUP_CAN_FAIL is `true` only ###
 * The object at `static` Pool_lookup_global_stats<Shm_arena, true>::stats() -- a global, per-`decltype(*this)`
 * arena_lend::Owner_pool_lookup_global_stats -- contains some interesting stats as updated by every `*this`.
 *
 * To reset this stat-set, use Pool_lookup_global_stats<Shm_arena, true>::reset_stats().
 *
 * ### Stats -- If #S_LOOKUP_CAN_FAIL is `false` only ###
 * The object at `static` Pool_lookup_global_stats<Shm_arena, false>::stats() -- a global, per-`decltype(*this)`
 * arena_lend::Borrower_pool_lookup_global_stats -- contains some interesting stats as updated by every `*this`.
 *
 * To reset this stat-set, use Pool_lookup_global_stats<Shm_arena, false>::reset_stats().
 *
 * @tparam Repository_t
 *         See above.  Formally(ish) there are two requirements/properties.  1, if and only if
 *         `is_same_v<R1, R2> == true` then `Thread_local_pool_lookup_rev<R1, ...>` and
 *         `Thread_local_pool_lookup_rev<R2, ...>` are independent registries; otherwise they are the same registry.
 *         2, `Repository_t::get_instance().tl_copy_rev_pull_pools()` must exist and do what is
 *         explained above.
 * @tparam LOOKUP_CAN_FAIL
 *         If `true`, lookup() may fail to find an address in any pool (returns `pool_id = 0` in that case).
 *         This is needed on the owner side, where an `Shm_pool_offset_ptr` with `CAN_STORE_RAW_PTR == true`
 *         may call `from_address()` on a non-SHM (stack/heap) address.
 *         If `false`, every address is assumed to reside in some known pool (borrower side), and lookup()
 *         has undefined behavior on a miss (as before); pool sizes are not stored.
 * @tparam Shm_arena_t
 *         See Pool_lookup_global_stats.
 */
template<typename Repository_t, bool LOOKUP_CAN_FAIL, typename Shm_arena_t>
class Thread_local_pool_lookup_rev
{
public:
  // Types.

  /// Short-hand for the template parameter.
  using Repository = Repository_t;
  /// Short-hand for the template parameter.
  using Shm_arena = Shm_arena_t;
  /// Mirrors the template parameter; `true` if lookup() can legitimately fail to find the containing pool.
  static constexpr bool S_LOOKUP_CAN_FAIL = LOOKUP_CAN_FAIL;
  /// Short-hand for pool-offset type (byte offset from pool base).
  using pool_offset_t = Shm_pool::size_t;

  /**
   * Reverse-lookup map type (base vaddr -> Pool_by_base).  Used internally but exposed to our direct user via
   * the push and pull APIs.
   *
   * @internal
   * ### Rationale for chosen type ###
   * Used for #m_pools_sorted_by_base; is a `flat_map` (sorted contiguous vector) rather than `std::map`
   * (red-black tree).  This is ideal here: lookup() -- the perf-critical hot path -- performs `upper_bound()` on
   * contiguous memory (cache-friendly binary search) instead of pointer-chasing through scattered tree nodes.
   *
   * The downside of `flat_map` -- O(n) insertion -- is low-impact: the thread-local copy is never incrementally
   * modified; it is only replaced wholesale in push_pools() (centrally when pool added/removed) and initialized
   * wholesale in the pull path (once per thread).  Both are off the fast-path.
   */
  using Map = boost::container::flat_map<void*, Pool_by_base>;

  // Constructors/destructor.

  /**
   * Constructs this thread-local cache by pulling the current canonical pool state from #Repository.
   * Called automatically by #s_registry when a new thread first accesses `this_thread_state()`.
   *
   * `s_registry.while_locked()` is in effect at the time of this call (per
   * `Thread_local_state_registry::this_thread_state()` contract).
   *
   * @warning It is public due to `Thread_local_state_registry` requirements only.  Do not call directly.
   *          See class doc header "How to use: push" section however.
   */
  Thread_local_pool_lookup_rev();

  /// Destructor: runs at thread exit.
  ~Thread_local_pool_lookup_rev();

  // Methods.

  /**
   * Performs the perf-critical reverse lookup: given a vaddr, determines the containing pool's ID and the
   * byte offset of the address from that pool's base.
   *
   * If `S_LOOKUP_CAN_FAIL == false`, `address` must reside within a known pool (else undefined behavior).
   * If `S_LOOKUP_CAN_FAIL == true`, `address` may be outside any pool; in that case `pool_id` is set to 0
   * and `pool_offset` is left untouched.
   *
   * @param address
   *        The vaddr to look up.
   * @param pool_id
   *        On return, set to the ID of the pool containing `address`; or 0 if not found
   *        (only when `S_LOOKUP_CAN_FAIL`).
   * @param pool_offset
   *        On return, set to `address` minus the base vaddr of the containing pool; untouched if not found.
   */
  void lookup(const void* address, pool_id_t& pool_id, pool_offset_t& pool_offset) const;

  /**
   * Replaces this thread-local sorted map with a new canonical snapshot.
   *
   * Called from the pusher thread (not necessarily the thread owning this `*this`).
   * `s_registry.while_locked()` shall be in effect at the time of this call.
   *
   * See class doc header "How to use: pull" section.
   *
   * @param pools_sorted_by_base
   *        The complete new mapping (pool-base vaddr -> Pool_by_base); moved-from on return.
   */
  void push_pools(Map&& pools_sorted_by_base);

  // Data.

  /**
   * Global registry of per-thread `*this` instances.  See class doc header for the pull/push protocol
   * using `this_thread_state()`, `while_locked()`.
   */
  static flow::util::Thread_local_state_registry<Thread_local_pool_lookup_rev> s_registry;

private:
  // Data.

  /// Per-thread mutex protecting #m_pools_sorted_by_base.  Contended by at most 2 threads (local lookup vs. pusher).
  mutable flow::util::Mutex_non_recursive m_pools_sorted_by_base_mutex;
  /**
   * Thread-local copy of the canonical sorted map: pool-base vaddr -> Pool_by_base.  See #Map doc and class
   * doc header.
   */
  Map m_pools_sorted_by_base;
}; // class Thread_local_pool_lookup_rev

/**
 * (Internal-use) Analogous to Thread_local_pool_lookup_rev but for forward lookup: pool ID -> base vaddr.
 *
 * As far as we know, if one needs multithreaded such lookup with high performance, then
 * borrower-side one should use Thread_local_pool_lookup_fwd.  However owner-side one can use a
 * straight `static thread_local` map from pool ID to `void*`, updating it lazily on-demand on cache miss (letting
 * outdated pool entries negligibly leak).  Such a solution is insufficient for borrower-side lookup, as
 * it is possible a pool is mapped, unmapped, then mapped to a different base vaddr -- but a thread's cache
 * might (forever) store the now-incorrect base-vaddr.  Hence use Thread_local_pool_lookup_fwd.
 *
 * The API and impl are the same: thread-local caches in a global #s_registry, with the same pull/push
 * protocol and the same per-thread locking strategy.  See Thread_local_pool_lookup_rev class doc header for
 * the full design discussion; only the differences are noted here:
 *   - The cached data structure, while still a map, is simply looked-up directly: pool ID in, base-vaddr out.
 *     The chosen type for #Map may be of interest (see `Map` doc header).
 *   - The pull callback is `Repository::get_instance().tl_copy_fwd_pull_pools()` (not `..._rev_...`).
 *
 * ### Stats ###
 * The object at `static` Pool_lookup_global_stats<Shm_arena, false>::stats() -- a global, per-`decltype(*this)`
 * arena_lend::Borrower_pool_lookup_global_stats -- contains some interesting stats as updated by every `*this`.
 *
 * To reset this stat-set, use Pool_lookup_global_stats<Shm_arena, false>::reset_stats().
 *
 * @tparam Repository_t
 *         Same concept as for Thread_local_pool_lookup_rev.
 *         `Repository_t::get_instance().tl_copy_fwd_pull_pools()` must exist and behave analogously
 *         to the reverse counterpart.
 * @tparam Shm_arena_t
 *         See Pool_lookup_global_stats.
 */
template<typename Repository_t, typename Shm_arena_t>
class Thread_local_pool_lookup_fwd
{
public:
  // Types.

  /// Short-hand for the template parameter.
  using Repository = Repository_t;
  /// Short-hand for the template parameter.
  using Shm_arena = Shm_arena_t;
  /// Short-hand for pool-offset type (byte offset from pool base).
  using pool_offset_t = Shm_pool::size_t;

  /**
   * Forward-lookup map type (pool ID -> base vaddr).  Used internally but exposed to our direct user via
   * the push and pull APIs.
   *
   * @internal
   * ### Rationale for chosen type ###
   * As the Thread_local_pool_lookup_rev::Map cousin uses `flat_map` (sorted contiguous vector) for
   * cache-friendly `lower_bound()`, we analogously use `unordered_flat_map` (open-addressing hash table) for
   * cache-friendly `find()`.  Same rationale.
   */
  using Map = boost::unordered_flat_map<pool_id_t, void*>;

  // Constructors/destructor.

  /// Analogous to Thread_local_pool_lookup_rev::Thread_local_pool_lookup_rev().  Reminder: Do not call directly.
  Thread_local_pool_lookup_fwd();

  /// Destructor: runs at thread exit.
  ~Thread_local_pool_lookup_fwd();

  // Methods.

  /**
   * Performs the forward lookup: given a pool ID, returns the pool's base vaddr; if pool ID is invalid undefined
   * behavior ensues.
   *
   * @see lookup_safe() for an alternative without the aforementioned undefined-behavior.
   *
   * @param pool_id
   *        The pool ID to look up.
   * @return The base vaddr of the pool identified by `pool_id`.
   */
  void* lookup(pool_id_t pool_id) const;

  /**
   * Identical to lookup() but returns null if `pool_id` is not a registered pool (never registered or registered
   * but removed).
   *
   * @param pool_id
   *        The pool ID to look up.
   * @return The base vaddr of the pool identified by `pool_id`; or null.
   */
  void* lookup_safe(pool_id_t pool_id) const;

  /**
   * Analogous to Thread_local_pool_lookup_rev::push_pools() but replaces the forward-lookup hash map.
   *
   * @param pool_bases_by_id
   *        The complete new mapping (pool ID -> base vaddr); moved-from on return.
   */
  void push_pools(Map&& pool_bases_by_id);

  // Data.

  /**
   * Global registry of per-thread `*this` instances.
   * @see Thread_local_pool_lookup_rev::s_registry -- same protocol applies.
   */
  static flow::util::Thread_local_state_registry<Thread_local_pool_lookup_fwd> s_registry;

private:
  // Data.

  /// Per-thread mutex protecting #m_pool_bases_by_id.  Contended by at most 2 threads (local lookup vs. pusher).
  mutable flow::util::Mutex_non_recursive m_pool_bases_by_id_mutex;
  /// Thread-local copy of the canonical hash map: pool ID -> base vaddr.  See #Map doc and class doc header.
  Map m_pool_bases_by_id;
}; // class Thread_local_pool_lookup_fwd

// Template static initializers.

template<typename Repository_t, bool LOOKUP_CAN_FAIL, typename Shm_arena_t>
flow::util::Thread_local_state_registry<Thread_local_pool_lookup_rev<Repository_t, LOOKUP_CAN_FAIL, Shm_arena_t>>
  Thread_local_pool_lookup_rev<Repository_t, LOOKUP_CAN_FAIL, Shm_arena_t>::s_registry
    {nullptr, "Thread_local_pool_lookup_rev"};

template<typename Repository_t, typename Shm_arena_t>
flow::util::Thread_local_state_registry<Thread_local_pool_lookup_fwd<Repository_t, Shm_arena_t>>
  Thread_local_pool_lookup_fwd<Repository_t, Shm_arena_t>::s_registry{nullptr, "Thread_local_pool_lookup_fwd"};

// Template implementations: Thread_local_pool_lookup_rev.

template<typename Repository_t, bool LOOKUP_CAN_FAIL, typename Shm_arena_t>
Thread_local_pool_lookup_rev<Repository_t, LOOKUP_CAN_FAIL, Shm_arena_t>::Thread_local_pool_lookup_rev()
{
  using flow::util::stat::fetch_add;
  using flow::util::stat::update_hi_wmark;

  // s_registry.while_locked() is in effect (by Thread_local_state_registry::this_thread_state() contract).

  // No need to lock m_pools_sorted_by_base_mutex yet: we have not been returned to user.

  Repository::get_instance().tl_copy_rev_pull_pools(&m_pools_sorted_by_base);

  { // Stats.
    auto& stats = *(Pool_lookup_global_stats<Shm_arena, S_LOOKUP_CAN_FAIL>::stats_mutable());
    update_hi_wmark(&stats.m_pool_rev_lookup_tl_cache_count_hi_wmark,
                    fetch_add(&stats.m_pool_rev_lookup_tl_cache_count, 1) + 1);
  }
}

template<typename Repository_t, bool LOOKUP_CAN_FAIL, typename Shm_arena_t>
Thread_local_pool_lookup_rev<Repository_t, LOOKUP_CAN_FAIL, Shm_arena_t>::~Thread_local_pool_lookup_rev()
{
  using flow::util::stat::fetch_sub;

  { // Stats.
    fetch_sub(&(Pool_lookup_global_stats<Shm_arena, S_LOOKUP_CAN_FAIL>::stats_mutable()
                  ->m_pool_rev_lookup_tl_cache_count), 1);
  }
}

template<typename Repository_t, bool LOOKUP_CAN_FAIL, typename Shm_arena_t>
void Thread_local_pool_lookup_rev<Repository_t, LOOKUP_CAN_FAIL, Shm_arena_t>::lookup(const void* address,
                                                                                      pool_id_t& pool_id,
                                                                                      pool_offset_t& pool_offset) const
{
  // Lock with minimal contention (see class doc header Impl section).
  flow::util::Lock_guard<decltype(m_pools_sorted_by_base_mutex)> lock{m_pools_sorted_by_base_mutex};

  /* It would be typical in the rest of the code base to add assert(s)/guards/things like that.
   * However we choose to skip past anything that might take cycles in production -- where it is not uncommon
   * to keep assertions enabled.  We're playing a little chicken here on purpose; the perf here has been
   * shown under load to be of high impact. */

  /* upper_bound() returns the first element with base > address.  The candidate pool (if any) is the one
   * just before that.  Unlike lower_bound(), there is no exact-match special case: always --it. */
  auto it = m_pools_sorted_by_base.upper_bound(const_cast<void*>(address));

  if constexpr(S_LOOKUP_CAN_FAIL)
  {
    if (it == m_pools_sorted_by_base.begin())
    {
      pool_id = 0;
      return;
    }
  }
  // else: Cannot be begin() by contract (address is in some pool).

  --it;

  const auto& pool_data = it->second;
  const auto addr_ptr = static_cast<const uint8_t*>(address);
  const auto pool_base = static_cast<const uint8_t*>(it->first);

  if constexpr(S_LOOKUP_CAN_FAIL)
  {
    /* Check that address actually falls within this pool's [base, base + size) range.
     * If not, it's in the gap between pools (or past the last pool). */
    if (addr_ptr >= (pool_base + pool_data.m_sz_or_0))
    {
      pool_id = 0;
      return;
    }
  }

  pool_id = pool_data.m_id;
  pool_offset = addr_ptr - pool_base;
} // Thread_local_pool_lookup_rev::lookup()

template<typename Repository_t, bool LOOKUP_CAN_FAIL, typename Shm_arena_t>
void Thread_local_pool_lookup_rev<Repository_t, LOOKUP_CAN_FAIL, Shm_arena_t>::push_pools(Map&& pools_sorted_by_base)
{
  // We are potentially *not* in our thread-local thread.

  // s_registry.while_locked() is in effect.

  // Must also lock this against at least lookup().
  flow::util::Lock_guard<decltype(m_pools_sorted_by_base_mutex)> lock{m_pools_sorted_by_base_mutex};

  m_pools_sorted_by_base = std::move(pools_sorted_by_base);
}

// Template implementations: Thread_local_pool_lookup_fwd.

template<typename Repository_t, typename Shm_arena_t>
Thread_local_pool_lookup_fwd<Repository_t, Shm_arena_t>::Thread_local_pool_lookup_fwd()
{
  using flow::util::stat::fetch_add;
  using flow::util::stat::update_hi_wmark;

  // s_registry.while_locked() is in effect.

  // No need to lock m_pool_bases_by_id_mutex yet: we have not been returned to user.

  Repository::get_instance().tl_copy_fwd_pull_pools(&m_pool_bases_by_id);

  { // Stats.
    auto& stats = *(Pool_lookup_global_stats<Shm_arena, false>::stats_mutable());
    update_hi_wmark(&stats.m_pool_fwd_lookup_tl_cache_count_hi_wmark,
                    fetch_add(&stats.m_pool_fwd_lookup_tl_cache_count, 1) + 1);
  }
}

template<typename Repository_t, typename Shm_arena_t>
Thread_local_pool_lookup_fwd<Repository_t, Shm_arena_t>::~Thread_local_pool_lookup_fwd()
{
  using flow::util::stat::fetch_sub;

  { // Stats.
    fetch_sub(&(Pool_lookup_global_stats<Shm_arena, false>::stats_mutable()->m_pool_fwd_lookup_tl_cache_count), 1);
  }
}

template<typename Repository_t, typename Shm_arena_t>
void* Thread_local_pool_lookup_fwd<Repository_t, Shm_arena_t>::lookup(pool_id_t pool_id) const
{
  // Lock with minimal contention.
  flow::util::Lock_guard<decltype(m_pool_bases_by_id_mutex)> lock{m_pool_bases_by_id_mutex};

  // As in _rev::lookup() for same reason... intentionally not checking against end()/assert()ing/etc.

  return m_pool_bases_by_id.find(pool_id)->second;
}

template<typename Repository_t, typename Shm_arena_t>
void* Thread_local_pool_lookup_fwd<Repository_t, Shm_arena_t>::lookup_safe(pool_id_t pool_id) const
{
  // Lock with minimal contention.
  flow::util::Lock_guard<decltype(m_pool_bases_by_id_mutex)> lock{m_pool_bases_by_id_mutex};

  const auto it = m_pool_bases_by_id.find(pool_id);
  return (it == m_pool_bases_by_id.end()) ? static_cast<void*>(nullptr)
                                          : it->second;
}

template<typename Repository_t, typename Shm_arena_t>
void Thread_local_pool_lookup_fwd<Repository_t, Shm_arena_t>::push_pools(Map&& pool_bases_by_id)
{
  // We are potentially *not* in our thread-local thread.

  // s_registry.while_locked() is in effect.

  // Must also lock this against at least lookup().
  flow::util::Lock_guard<decltype(m_pool_bases_by_id_mutex)> lock{m_pool_bases_by_id_mutex};

  m_pool_bases_by_id = std::move(pool_bases_by_id);
}

// Template implementations: Pool_lookup_global_stats.

template<typename Shm_arena_t, bool OWNER_ELSE_BORROWER>
const typename Pool_lookup_global_stats<Shm_arena_t, OWNER_ELSE_BORROWER>::Stats&
  Pool_lookup_global_stats<Shm_arena_t, OWNER_ELSE_BORROWER>::stats() // Static.
{
  /* Rationale for using the Flow Global_stats facility (as opposed to just shoving a `public: static`
   * Stats member in the class): That would have been fine too, but then one would have to worry about
   * static-init order.  Could do an on-demand singleton ourselves, but Global_stats provides
   * on-demand static-init (and properly ordered deinit courtesy of C++ rules), so we use it to reduce
   * our impl burden here. */
  return flow::util::stat::Global_stats<Pool_lookup_global_stats, Stats>::get().stats_default();
}

template<typename Shm_arena_t, bool OWNER_ELSE_BORROWER>
typename Pool_lookup_global_stats<Shm_arena_t, OWNER_ELSE_BORROWER>::Stats*
  Pool_lookup_global_stats<Shm_arena_t, OWNER_ELSE_BORROWER>::stats_mutable() // Static.
{
  return &(flow::util::stat::Global_stats<Pool_lookup_global_stats, Stats>::get().stats_mutable_default());
}

template<typename Shm_arena_t, bool OWNER_ELSE_BORROWER>
void Pool_lookup_global_stats<Shm_arena_t, OWNER_ELSE_BORROWER>::stats_reset() // Static.
{
  flow::util::stat::Global_stats<Pool_lookup_global_stats, Stats>::get().reset();
}

} // namespace ipc::shm::arena_lend::detail
