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

#include "ipc/shm/arena_lend/borrower_shm_pool_collection.hpp"
#include "ipc/shm/arena_lend/detail/owner_shm_pool_repository.hpp"
#include "ipc/shm/arena_lend/detail/thread_lcl_pool_lookup.hpp"
#include "ipc/shm/arena_lend/arena_lend_stats.hpp"
#include "ipc/shm/arena_lend/arena_lend_fwd.hpp"
#include "ipc/shm/arena_lend/detail/arena_lend_fwd.hpp"
#include "ipc/session/standalone/shm/arena_lend/arena_lend_fwd.hpp"
#include "ipc/session/standalone/shm/arena_lend/detail/arena_lend_fwd.hpp"
#include <flow/util/action_registry.hpp>
#include <flow/util/linked_hash_map.hpp>
#include <flow/log/log.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/algorithm/sort.hpp>
#include <memory>
#include <tuple>

namespace ipc::session::shm::arena_lend::detail
{

// Types.

/**
 * (Internal-use) Singleton repository for borrowed SHM-pool data.  All sessions (jemalloc::Shm_session) in this
 * process funnel their borrowed pool info here, enabling offset-pointer resolution (to_address(), from_address())
 * and pool-handle lifecycle management (ref-counted open/close).
 *
 * @note We mention #Uniq_collection_id below... though as of this writing it is not used in the actual public
 *       API.  When we do this, we just mean an (`owner_id_t`, `collection_id_t`) pair.  Such a pair ultra-uniquely
 *       identifies an arena a/k/a owner-pool-collection.
 *
 * ### Responsibility: Pool (+arena) lifecycle ###
 * To summarize: First registration of a globally-unique `pool_id` opens the pool-handle/maps the vaddr area
 * (stores a `Shm_pool`); last deregistration undoes both.  That is the bottom line.  To make that work you
 * must register and deregister SHM-pools (and arenas a/k/a collections containing them).
 *
 *   - register_collection() upon an opposing process lending its arena, via a `Shm_session`, to us.
 *     - deregister_collection() for each arena that had been lent, when that arena is un-lent.
 *       (As of this writing one does not un-lend an arena without closing session, which unlends *all*
 *       lent-through-it arenas; but we support the former here no problem.)
 *     - The same (meaning, same #Uniq_collection_id) arena being lent to us, via a different `Shm_session`, at
 *       the same time is possible; call it again then.  Thus a given arena may be registered 1+ times
 *       concurrently.
 *     - Nothing spectacular happens once a particular arena goes from not-registered to 1x-registered, or vice versa.
 *       We track them at all, really, only because we need certain info on a per-arena (collection) basis
 *       (the `pool_name_base` at least).
 *   - register_shm_pool() and deregister_shm_pool() w/r/t SHM-pools being lent via `Shm_session`s works just the
 *     same... but know the following:
 *     - If pool P in arena C was lent through session S, then by definition first S notes the lending of C, then
 *       the lending of P.  Therefore you shall call, for session S, `register_collection(C)` and
 *       `register_shm_pool(P)` in that order.  (The same order of things, for the same P and therefore C may
 *       happen at any time including overlapping chronologically... but only for a different session than S.)
 *       This is all vanilla `Shm_session` stuff; we are restating it for context/exposition purposes.
 *     - When a given pool P goes from not-registered to 1x-registered, a `Shm_pool` is created and saved which means
 *       (as noted already) open SHM-pool-handle, map it into SHM....  Fairly spectacular!
 *     - When the reverse occurs, it is released: unmap from SHM, close SHM-pool-handle.  Also spectacular!
 *       In fact this is required for the underlying RAM to be returned to the system for general use.  (It is
 *       required but not sufficient: There may be SHM-pool-handles open in owner, and the pool-name
 *       may be registered in the file-system; owner side handles all that including undoing it, even after crash.)
 *
 * ### Responsibility: Offset-pointer resolution for Shm_pool_offset_ptr ###
 * One uses to_address() for forward lookup (`pool_id` + offset -> vaddr).
 * One uses from_address() for the reverse lookup (vaddr -> `pool_id` + offset).
 *
 * @see ipc::shm::arena_lend::Shm_pool_offset_ptr
 *
 * The `register_*()` stuff above provides the necessary inputs to make these work.
 *
 * ### Impl ###
 * We could pontificate here about this and that impl detail, but it would probably not be helpful didactically,
 * as it's all about various details, and those are documented closer to the fact.  So we'd just be redundant.
 * In this case it's best to just start with the data members and see how things interact.
 *
 * That said, we have found that the biggest hurdle is just grasping the 2 responsibilities in some depth.  So
 * to (re-)emphasize:
 *   - We need to keep *all* SHM-pools (`shared_ptr<Shm_pool>`) open, each one if and only if at least 1 -- possibly
 *     more -- session is borrowing it.  Grasp how a pool ID is globally unique forever; but it does pertain to
 *     a particular arena (ID = #Uniq_collection_id = owner-PID + ordinal collection-ID for that PID), and (from
 *     both a `Shm_session`'s PoV *and* (to some extent) our PoV) this is significant.  In our case that's so, b/c
 *     a `pool_name_base` `Shared_name`-fragment (prefix) is per-arena and determines the pool name, which is how
 *     we can even open the `Shm_pool`.
 *   - We need to *also* funnel that info into a global repository of *borrower* pool base-vaddrs and IDs.
 *     This enables high-performance computations for `Shm_pool_offset_ptr`s... which will absolutely litter
 *     any STL-compliant-container-stored-in-SHM.  We provide the relevant output of this info: to_address() and
 *     from_address().
 *
 * Having grasped all that, go forth to the data members and the methods' insides.  OK, no, one more thing....
 *
 * ### Impl: forward/reverse lookup and the 2 mutexes ###
 * While the register/deregister calls feed the info needed for the following topic, that topic is otherwise
 * essentially its own thing.  That thing is: We are the top-level of the impl for:
 *   - Forward-lookup (to_address()) whose complexity reduces to: lookup from #pool_id_t to `void*` base-vaddr
 *     of that SHM-pool.
 *   - Reverse-lookup (from_address()) which is: lookup from vaddr `void*` within a SHM-pool to: #pool_id_t
 *     (of containing pool) and #pool_offset_t (integer byte offset from that pool's base-vaddr).
 *
 * Updates to, and lookup from, the relevant data structures we treat as essentially mutually independent
 * in the sense of Fwd lookup versus Rev lookup.  That is: We allow, say, 2
 * `[de]register_shm_pool()` ops to be somewhat interleaved.  More specifically: there are 2 mutexes, and
 * in `[de]register_shm_pool()` we always lock/unlock one, then the other.  E.g., there could be 1 `Shm_session`
 * registering a pool while another is shutting down and deregistering a pool, so the update order of the
 * Fwd and Rev structures might look chronologically like F1R1F2R2 or F1F2R1R2 or F2F1R1R2 or....
 * If there is some danger to this, we don't see it.  The benefit is that we can place the relevant blocks of
 * code to be next to each other but independent -- no shared mutex, basically.  This helps impl simplicity, as the
 * primitives we use for each task (Fwd data update, Rev data update) are separate modules, and at least in one
 * case a primitive (detail::Shm_pool_repo_lookup_core_rev) is reused in owner-land (detail::Owner_shm_pool_repository).
 * Having to link the 2 things together code-wise by sharing a mutex would have been very annoying.
 *
 * So now we can talk about how each of those is implemented, separately.
 *
 * ### Impl: Forward-lookup ###
 * We use detail::Thread_local_pool_lookup_fwd (alias #Tl_copy_fwd).  See its doc header first; then come back here.
 * Welcome back:
 *   - #m_rc_pools_by_id is the canonical map from which we push/it pulls into the thread-local copies.
 *   - The "pull" impl is, as required by #Tl_copy_fwd, named tl_copy_fwd_pull_pools().
 *   - The "push" behavior is performed, in the manner required by #Tl_copy_fwd, within register_shm_pool() and
 *     deregister_shm_pool().
 *   - As recommended by #Tl_copy_fwd docs, we use its built-in central mutex as our overall Fwd-update mutex
 *     (1 of the 2 mutexes introduced above, thus synchronizing simultaneous SHM-pool-set updates) as well as
 *     to guard the push-op (as required by #Tl_copy_fwd formally).  Nice to use just one mutex for Fwd.
 *
 * ### Impl: Reverse-lookup ###
 * We simply use detail::Shm_pool_repo_lookup_core_rev, notably with
 * Shm_pool_repo_lookup_core_rev::S_LOOKUP_CAN_FAIL at `false` (only in-SHM addresses are supported on borrower side).
 * See its doc header.  It has its own mutex inside the simple-looking ops we invoke (`.insert()`, `.erase()`).
 * That's mutex 2 of 2.
 *
 * If you're wondering why Rev is so simple and encapsulated, while Fwd required a bunch of bullet points and
 * code: It's just because we are the only place in Flow-IPC that needs to do what we do for Fwd lookup.
 * (Owner-side Fwd lookup is algorithmically far simpler.  The reason for that is, as of this writing, briefly explained
 * in doc header of detail::Thread_local_pool_lookup_fwd doc header.)  However Rev lookup is ~identical
 * for us and in owner-land; hence it is encapsulated in `Shm_pool_repo_lookup_core_rev` and used here and ~identically
 * in detail::Owner_shm_pool_repository.
 *
 * @tparam Shm_arena_t
 *         SHM-arena type (e.g., `jemalloc::Ipc_arena`).  Not accessed API-wise; serves as a compile-time
 *         discriminator so that `Borrower_shm_pool_collection_repository<A1>` and
 *         `Borrower_shm_pool_collection_repository<A2>` are fully independent singleton registries
 *         if `A1 != A2`.
 */
template<typename Shm_arena_t>
class Borrower_shm_pool_collection_repository :
  /* `Log_context_mt`, not `Log_context`: our set_logger() is driven at arbitrary times -- via Set_logger_registry,
   * i.e., the public arena_lend::set_logger() -- concurrently with the borrower-side pool open/close work (in
   * arbitrary threads) that reads the logger.  So the thread-safe variant is required (unlike, e.g., Shm_session,
   * whose logger is fixed at construction). */
  public flow::log::Log_context_mt,
  private boost::noncopyable
{
private:
  // Types.

  /// Short-hand.
  using Borrower_shm_pool_collection = ipc::shm::arena_lend::Borrower_shm_pool_collection;

public:
  // Types.

  /// Compile-time discriminator alias.
  using Shm_arena = Shm_arena_t;

  /// Short-hand for pool ID type.
  using pool_id_t = Borrower_shm_pool_collection::pool_id_t;
  /// Short-hand for pool offset type.
  using pool_offset_t = Borrower_shm_pool_collection::pool_offset_t;

  /**
   * Short-hand for the thread-local forward-lookup (pool-ID -> base-vaddr) cache type.
   * Internal use; part of the pull/push protocol (see class doc header "Impl: Forward-lookup").
   */
  using Tl_copy_fwd
    = ipc::shm::arena_lend::detail::Thread_local_pool_lookup_fwd<Borrower_shm_pool_collection_repository, Shm_arena>;

  /// Alias for a stats type.
  using Borrower_pool_stats = ipc::shm::arena_lend::stat::Borrower_pool_stats;

  /// Short-hand for single-ownership pointer (`unique_ptr` of some sort).
  template<typename T>
  using Own = std::unique_ptr<T>;

  /// Short-hand for list of #Borrower_pool_stats (canonical definition is in arena_lend_fwd.hpp).
  using Borrower_pool_stats_list = ipc::session::shm::arena_lend::Borrower_pool_stats_list;

  /// Alias for a stats-like info type.
  using Shm_pool_info = ipc::shm::arena_lend::stat::Shm_pool_info;

  // Methods.

  /**
   * Returns the process-wide singleton instance of this class.
   * @return Reference to the singleton.
   */
  static Borrower_shm_pool_collection_repository& get_instance();

  /**
   * Key high-performance API: Given an in-SHM location as the pool and in-pool offset (values that remain constant
   * between the lending process and us, the borrowing process) returns the process-local (local process being us,
   * the borrower) vaddr pointing to that in-SHM location.
   *
   * Context: In particular, when processing fancy-pointers `Shm_pool_offset_ptr` (in STL-compliant structures
   * stored in SHM and borrowed by this process) and top-level borrowed (by us) objects, this shall be called.
   * E.g., when traversing some in-SHM borrowed STL-container, such lookups may be quite frequent.
   *
   * Perf: as of this writing this is a ~no-contention mutex-lock and a hash-map lookup of vaddr by integer;
   * plus an offset addition.
   *
   * ### Corner cases ###
   * If `pool_id` is that of a no-longer-borrowed, or never-borrowed, SHM-pool: undefined behavior.
   *
   * Otherwise, the returned address is non-null; however it shall simply equal the pool's base-vaddr plus
   * `pool_offset`.  If it is negative or beyond the pool's size, then the returned address shall be outside
   * the specified pool; we shall not bounds-check it.
   *
   * @see to_address_safe() for a variation that avoids the aforementioned undefined behavior.
   *
   * @param pool_id
   *        The identifier of the SHM-pool.
   * @param pool_offset
   *        The offset in bytes off the pool `pool_id` base vaddr.
   * @return Non-null vaddr.
   */
  static void* to_address(pool_id_t pool_id, pool_offset_t pool_offset);

  /**
   * Identical to to_address() except returns null on invalid (no-longer borrowed or never-borrowed) pool ID.
   *
   * ### Rationale ###
   * The precipitating use-case is `Shm_session::borrow_object()` that makes a best-effort safety check on
   * the values ostensibly IPCed-over from the lending process.  Even so, we can only do so much due to certain
   * perf-based internal decisions made as of this writing: In particular `pool_offset` can still point outside
   * the (valid) pool; this will not cause us to return null, even though for that particular use-case at least
   * that would be preferable.
   *
   * @param pool_id
   *        See above.
   * @param pool_offset
   *        See above.
   * @return Non-null vaddr; or null if `pool_id` not found.
   */
  static void* to_address_safe(pool_id_t pool_id, pool_offset_t pool_offset);

  /**
   * Key high-performance API: Essentially the reverse of to_address().
   *
   * Context: A reverse-lookup should be much less frequent than forward-lookup to_address(); but typical STL-container
   * code at times must figure out a `pointer` (in our case `Shm_pool_offset_ptr`) from a vaddr.  (In heap-land
   * the two are the same, but in SHM-land they are not.)
   *
   * Perf: similar to to_address() in terms of mutexes and maps involved; however the actual map lookup is essentially
   * a binary search through a sorted array.
   *
   * If `address` param is invalid (not in any borrowed pool), behavior is undefined.
   *
   * @param address
   *        Vaddr to within a SHM-pool.
   * @param pool_id
   *        Out-arg where the identifier of the pool containing `address` shall be placed.
   * @param pool_offset
   *        Out-arg where the offset of a byte within pool `pool_id` at which `address` points shall
   *        be placed.
   */
  static void from_address(const void* address, pool_id_t& pool_id, pool_offset_t& pool_offset);

  /**
   * Registers (or increments use of) a borrowed arena (collection) identified by arena ID
   * (`owner_id`, `collection_id`).  Multiple `Shm_session`s borrowing the same arena (by arena ID) would each
   * call this; use-count internally tracks how many.
   *
   * Nothing special happens when a new arena is added; but we at least need to save its `pool_name_base`, so
   * we can open SHM-pools from that arena (in register_shm_pool()).
   *
   * @param owner_id
   *        Owner (remote PID) of the collection.
   * @param collection_id
   *        Collection identifier (unique per `owner_id`).
   * @param pool_name_base
   *        Base name for constructing SHM object names for pools in this collection.
   */
  void register_collection(owner_id_t owner_id, collection_id_t collection_id, Shared_name&& pool_name_base);
  /**
   * Undoes register_collection() for the given arena ID.
   *
   * Nothing special happens when an arena's use-count drops to zero due to this call; but internally
   * we would at least forget its `pool_name_base`.
   *
   * @param owner_id
   *        Owner (remote PID) of the collection.
   * @param collection_id
   *        Collection identifier.
   */
  void deregister_collection(owner_id_t owner_id, collection_id_t collection_id);

  /**
   * Registers a borrowed SHM-pool.  On first registration the pool is opened (SHM-pool-handle opened, memory
   * mapped); subsequent registrations (from other `Shm_session`s to the same containing arena as identified
   * by `owner_id` + `collection_id`) increment the use-count.
   *
   * On failure to open/memory-map the SHM-pool (which implies a massive breakdown, as the owner side just
   * created it, and memory-mapping failure implies a horrible state of the machine), this will `FATAL`-log
   * and `abort()`.
   *
   * @param owner_id
   *        Owner (remote PID).
   * @param collection_id
   *        Collection containing the pool.
   * @param shm_pool_id
   *        Globally unique pool identifier.
   * @param shm_pool_size
   *        Size of the SHM-pool.
   */
  void register_shm_pool(owner_id_t owner_id, collection_id_t collection_id, pool_id_t shm_pool_id,
                         std::size_t shm_pool_size);

  /**
   * Decrements use of a borrowed SHM-pool; closes it (unmap memory + close SHM-pool-handle) when use-count reaches
   * zero.
   *
   * @note In order for a given SHM-pool's RAM to be returned to the system for general use, the following
   *       must occur: it must be deregistered here down to use-count zero; the same must occur in the owner process;
   *       and the pool's name must be removed from the file-system (by owner, in our design).
   *
   * @param owner_id
   *        Owner (remote PID).
   * @param collection_id
   *        Collection containing the pool.
   * @param shm_pool_id
   *        Globally unique pool identifier.
   */
  void deregister_shm_pool(owner_id_t owner_id, collection_id_t collection_id, pool_id_t shm_pool_id);

  /**
   * Given some attributes of a SHM-pool, returns the SHM object name for that pool.  Briefly locks
   * to look up the collection's `pool_name_base`; so avoid calling this frequently.
   *
   * @note You can see that 3 IDs are encoded in the pool name in some fashion.  However `pool_id` is globally unique.
   *       Hence it would have been enough, if the only objective is to come up with a unique name.
   *       The reason the other 2 things are required is that, specifically, we need the `pool_name_base` from
   *       earlier register_collection().  We need `pool_name_base`, which includes such things as owner application
   *       name and PID and more, for two broad reasons.  1 is it's nice for debugging/etc. (one can see the nature
   *       of a SHM-pool in Linux /dev/shm or logs at a glance).  2 (more critically) is it allows for (owner-side)
   *       cleanup-on-crash algorithm to remove-from-file-system (a/k/a unlink) whole swathes of names just by
   *       checking against a particular prefix.  (E.g., "delete all pools created by owner app A except by my own
   *       PID P.")  This note is strictly educational in nature.
   *
   * @param owner_id
   *        Owner (remote PID).
   * @param collection_id
   *        Collection containing the pool.
   * @param pool_id
   *        Pool identifier.
   * @return The computed SHM pool name.
   */
  Shared_name recompute_pool_name(owner_id_t owner_id, collection_id_t collection_id, pool_id_t pool_id) const;

  /**
   * Internal callback invoked by #Tl_copy_fwd ctor (via `Thread_local_state_registry`) during per-thread
   * cache creation.
   *
   * @note Not for direct use by callers of this class.  Public only because #Tl_copy_fwd
   *       (via `Thread_local_state_registry`) requires it.
   *
   * @param pool_bases_by_id
   *        Out-arg: map to populate with pool-ID -> base-vaddr entries.
   */
  void tl_copy_fwd_pull_pools(typename Tl_copy_fwd::Map* pool_bases_by_id) const;

  /**
   * Implements the borrower-side process-wide live-pool-info accessor; see
   * jemalloc::Shm_session::borrowed_shm_pool_live_info() for the full public description.
   *
   * @return See jemalloc::Shm_session::borrowed_shm_pool_live_info().
   */
  std::vector<Shm_pool_info> shm_pool_live_info() const;

  /**
   * Implements the borrower-side process-wide stats accessor; see
   * jemalloc::Shm_session::borrower_pool_stats_process_wide() for the full public description, including the
   * meaning of `per_arena_stats` and the returned totals.
   *
   * @param per_arena_stats
   *        See above.
   * @param per_arena_stats_sz_limit_or_0
   *        See above.
   * @param n_arenas_omitted
   *        See above.
   * @return See above.
   */
  const Borrower_pool_stats& stats(Borrower_pool_stats_list* per_arena_stats,
                                   size_t per_arena_stats_sz_limit_or_0, size_t* n_arenas_omitted) const;

  /// Resets stats().  The formal meaning of a reset is discussed in `flow::util::stat` doc header.
  void stats_reset();

private:
  // Types.

  /// Short-hand for (`owner_id`, `collection_id`) pair uniquely identifying a collection.
  using Uniq_collection_id = ipc::shm::arena_lend::detail::Uniq_collection_id;

  /**
   * Ref-counted collection data.  Multiple `Shm_session`s to the same owner-ID + collection-ID each register the same
   * collection; #m_use_count tracks how many; the `Borrower_shm_pool_collection` is shared among them.
   */
  struct Collection_data
  {
    // Data.

    /// The collection handle; shared among all `Shm_session`s that have registered this (owner, collection) pair.
    std::shared_ptr<Borrower_shm_pool_collection> m_collection;
    /// Number of `Shm_session`s currently borrowing this collection.
    unsigned int m_use_count;
  }; // struct Collection_data

  /**
   * Ref-counted pool data.  Pool IDs are globally unique, so the flat map keyed by pool-ID is sufficient.
   * #m_use_count tracks how many `Shm_session`s have registered this pool (multiple sessions can share
   * the same pool if they lend the same arena).  The pool is opened on first registration and closed on last
   * deregistration.
   */
  struct Pool_rc_data
  {
    // Data.

    /// The SHM-pool handle (open pool-handle + memory-mapped vaddr range).
    std::shared_ptr<ipc::shm::arena_lend::Shm_pool> m_shm_pool;
    /// Number of `Shm_session`s currently borrowing this pool.
    unsigned int m_use_count;
    /// Owning (owner, collection) pair.  For shm_pool_live_info() and debug logging.
    Uniq_collection_id m_uniq_collection_id;
  }; // struct Pool_rc_data

  /**
   * Short-hand for the reverse-lookup (vaddr -> pool-ID + offset) module.  It's a singleton like us and
   * has its own internal mutex (mentioned for perf context).
   *
   * @see Class doc header section "Impl: forward/reverse lookup and the 2 mutexes" and on.
   */
  using Core_rev = ipc::shm::arena_lend::detail::Shm_pool_repo_lookup_core_rev
                     <Borrower_shm_pool_collection_repository, false, Shm_arena>;

  // Constants.

  /**
   * The maximum `.size()` of #m_per_arena_borrower_pool_stats; once reached, and a `Stat_set` is to be
   * inserted, the LRU `Stat_set` is erased from that map.
   *
   * @see #m_per_arena_borrower_pool_stats doc header for discussion of the LRU-eviction algorithm.
   *
   * ### Value ###
   * We ran some experiments indicating how long stats()/stats_reset()/pretty-print of stats() takes and chose
   * a value well below the point where those times stopped being reasonable.  Since the value is still pretty high
   * (e.g., a user is likely to pass a much lower, nonzero `per_arena_stats_sz_limit_or_0` to stats()),
   * subjectively speaking, we feel this worked out OK.
   */
  static constexpr size_t S_N_PER_ARENA_BORROWER_POOL_STATS_SZ_LIMIT = 1000;

  // Constructors.

  /// Constructor.  Zero-arg due to singleton pattern.
  Borrower_shm_pool_collection_repository();

  // Methods.

  /**
   * Pushes updated forward-lookup data into all thread-local caches.  Called from within
   * register_shm_pool() / deregister_shm_pool() while `Tl_copy_fwd::s_registry.while_locked()` is in effect.
   *
   * @param state_per_thread
   *        Arg passed to `Tl_copy_fwd::s_registry.while_locked()` in the caller.
   */
  void tl_copy_fwd_push_pools
         (const typename decltype(Tl_copy_fwd::s_registry)::State_per_thread_map& state_per_thread) const;

  // Data.

  /**
   * Flat pools map: pool-ID -> ref-counted pool data.  Pool IDs are globally unique across all owners/collections.
   * Mutex: provided by `Tl_copy_fwd::s_registry.while_locked()`; see register_shm_pool(),
   * deregister_shm_pool().
   *
   * @see Class doc header section "Impl: forward/reverse lookup and the 2 mutexes" and on.
   */
  boost::unordered_flat_map<pool_id_t, Pool_rc_data> m_rc_pools_by_id;

  /// Map: (owner-ID, collection-ID) -> ref-counted collection data.  Same mutex as #m_rc_pools_by_id.
  boost::unordered_flat_map<Uniq_collection_id, Collection_data> m_collection_data_map;

  /**
   * Generally similar to Shm_session::m_borrower_pool_stats but across *all* `Shm_session`s (for the
   * same SHM-provider <=> #Shm_arena type).  Reads that guy's doc header first.  Then continue here:
   *
   * Regarding perf: basically same thing; there shall be potentially multiple `Shm_session`s around, but
   * not so many that the frequency-of-stat-updates picture is fundamentally changed.
   *
   * Regarding concurrency/locking: basically same thing; no locking required.  (Watch out, though,
   * as #m_per_arena_borrower_pool_stats has a different story due to maintaining a key-set, being a map.)
   * Regarding semantics: this PoV for a Borrower_pool_stats is fully non-degenerate.  That should be clear
   * having read the aforementioned doc header, but we emphasize: A first-registering of a SHM-pool here
   * (its Pool_rc_data::m_use_count goes 0=>1 <=> new Pool_rc_data is created/inserted) means
   * SHM-pool is opened/mapped; but 1=>2, 2=>3, etc., shall reuse the already-opened pool.  Hence
   * stat::Borrower_pool_stats::m_pool_open_count + `m_n_open_pools` are touched only in the 0=>1 case
   * (w/r/t register_shm_pool() API).  (Same deal with deregister_shm_pool() + unmap/close, etc.  Same deal
   * with arena/collection [de]registering.)
   */
  Borrower_pool_stats m_borrower_pool_stats;

  /**
   * Carries the totals from #m_borrower_pool_stats but broken-down by arena/collection *ever* borrowed (modulo
   * the LRU eviction below), each as identified by #Uniq_collection_id.
   *
   * LRU eviction: To avoid unbounded growth evicts the LRU (least recently touched
   * stats-wise arena, identified by `Uniq_collection_id`) entry on insertion having reached a certain max
   * size.  "Touching stats-wise" is defined as any of the 4 APIs `[de]register_{collection|shm_pool}()` invoked
   * for that `Uniq_collection_id`.
   *
   * @note Key semantic point: (Assume no eviction has occurred.)
   *       A particular Uniq_collection_id X being in m_collection_data_map does imply
   *       it is in `m_per_arena_borrower_pool_stats`; but the reverse is not necessarily the case:
   *       The key-set here is historical; an arena being borrowed, then unborrowed, does not delete it from
   *       this map.  Its then being borrowed again would, for example, increment its
   *       Borrower_pool_stats::m_arena_register_count by a total of 2 (1 at the first borrowing, 1 at the second).
   *       (Among other things that implies we cannot just shove a Borrower_pool_stats into Collection_data
   *       amd therefore into #m_collection_data_map.)
   *       The only vaguely degenerate semantic thing is that here Borrower_pool_stats::m_n_borrowed_arenas
   *       cannot exceed 1: an arena is either currently being borrowed by 1+ `Shm_session`s, or it is not.
   *
   * @note Usually the entries in #m_per_arena_borrower_pool_stats combined match the totals
   *       in #m_borrower_pool_stats.  This stops being fully true, *if* and (past) when the former grows
   *       large enough to require the LRU-removal of entries from then on, on subsequent insertions.
   *
   * ### Thread safety ###
   * While at the level of an individual Borrower_pool_stats there are no particular worries -- it is all `atomic`s
   * already -- here we also have the key-set of this map; it can be modified on behalf of, e.g., 2+
   * `Shm_session`s concurrently.  In short:
   *
   * Use the same mutex + lock-sections as #m_rc_pools_by_id and/or #m_collection_data_map when reading or
   * modifying #m_per_arena_borrower_pool_stats.
   *
   * Reasons:
   *   - LRU-eviction (discussed some more below) means keys are sometimes removed from the map.  Hence it's
   *     not safe to lock, get key set, unlock, do things to entries in the key set; use-after-free could occur.
   *     Even if that were not true (no removal ever):
   *   - It's, in subtle fashion, still wrong, even though we store `Own<>` wrappers which gives pointer stability.
   *     Details omitted, but it means semantically at least temporarily unpleasant values could be saved for
   *     some GAUGEs (and therefore even possibly permanently unpleasant results in derived
   *     HI_WMARKs could be saved), particularly if arena X is un-borrowed through one session while being borrowed
   *     through another right then.  If locking in-tandem with #m_rc_pools_by_id et al, though, we always
   *     save the actual statistical picture.
   *
   * ### The LRU eviction: Rationale ###
   * This is pretty straightforward, meaning the above explanation pretty much covers it; as usual minutiae are
   * at least explained in methods.  One thing to add: What are we trying to avoid, exactly, when eventually
   * evicting old entries?  We said it's about unbounded growth, which is true by definition, but it's really
   * about two things:
   *   - The obvious one: RAM use on account of the global `*this`.
   *   - The less obvious one: In stats() (and stats_reset() for that matter), it is necessary to iterate
   *     through all of `m_per_arena_borrower_pool_stats`.  If it can get huge, then that'll take a while.
   *     Also stats() copies stuff from there onto a user container.  (There's also then a lengthy, if rare, lock
   *     involved.)
   *
   * So that's why we do it at all.
   *
   * Another question concerns the choice of what determines the LRU/MRU order.  To restate, touching arena =
   * register_collection(), deregister_collection() of a given arena +
   * register_shm_pool(), deregister_shm_pool() of a pool pertaining to a given arena.  In other words:
   * touched/LRU/MRU = "the stats for it changed."  So if we're forced to forget an arena, then its stats have
   * been unchanged for the longest.
   *
   * We chose this ultimately because it's simple to understand and, hopefully, within reason as to what
   * is useful to a user.  Taking these 1 at a time:
   *   - Simple?  Yes: A `Stat_set` is modified <=> touched.  Hard to be simpler.
   *     - It also exactly matches the order in which stats() optionally emits these stats up to a
   *       (presumably lower) readability limit of `per_arena_stats_sz_limit_or_0` per-arena `Stat_set`s.
   *   - Usable?  More or less, but it's arguable.  An uptime/use-case resulting in (as of this writing)
   *     1,000 borrowed arenas since process start means we are justified in throwing some away.  At that point
   *     as long as the eviction-criterion is not plainly silly, I as a user am probably OK.  "It hasn't changed,
   *     so it's less active, so let's get rid of that one, if we must" feels not-plainly-silly.  It's definitely
   *     not a slam dunk either though: it ignores whether an arena is *currently* being borrowed or not; so
   *     arena A that was recently unborrowed (<=> session closed as of this writing) would stay in, while arena
   *     B that is still borrowed, but with zero further activity (these stats-wise... the SHM objects in the
   *     pools may well be being read, borrowed, etc.) for a longer time, would be evicted.
   *
   * That was written before deploying this in the field for the first time, so we may get field feedback to
   * modify this policy.
   *
   * @todo Consider adding a public `stats_forget_by_arena()` which would lock, clear
   * Borrower_shm_pool_collection_repository::m_per_arena_borrower_pool_stats, unlock; this would be available
   * for tests that would like to start with/leave a clean slate for this set of stat; possibly even
   * available via forward (like stats_reset()) by a public-facing API like `Shm_session`.  The impl is
   * simple and correct (won't create imbalanced stats or anything like that).  For tests it may be quite helpful
   * (as of this writing has not been needed though).  Would tentatively avoid making a public-facing forwarder,
   * though, as it feels fussy; the whole mechanism should already just work, when it comes to production use.
   */
  flow::util::Linked_hash_map<Uniq_collection_id, Own<Borrower_pool_stats>> m_per_arena_borrower_pool_stats;
}; // class Borrower_shm_pool_collection_repository

// Template implementations.

template<typename Shm_arena_t>
Borrower_shm_pool_collection_repository<Shm_arena_t>::Borrower_shm_pool_collection_repository() :
  flow::log::Log_context_mt(nullptr, Log_component::S_SESSION)
{
  ipc::shm::arena_lend::Set_logger_registry::register_action([this](flow::log::Logger* logger_ptr)
  {
    set_logger(logger_ptr);
  });
}

template<typename Shm_arena_t>
Borrower_shm_pool_collection_repository<Shm_arena_t>&
  Borrower_shm_pool_collection_repository<Shm_arena_t>::get_instance() // Static.
{
  static Borrower_shm_pool_collection_repository s_repository; // Thread-safe in C++17 at least.
  return s_repository;
}

template<typename Shm_arena_t>
void* Borrower_shm_pool_collection_repository<Shm_arena_t>::to_address(pool_id_t pool_id,
                                                                       pool_offset_t pool_offset) // Static.
{
  // Fast-path.
  return reinterpret_cast<void*>
           (reinterpret_cast<uintptr_t>(Tl_copy_fwd::s_registry.this_thread_state()->lookup(pool_id))
            + pool_offset);
}

template<typename Shm_arena_t>
void* Borrower_shm_pool_collection_repository<Shm_arena_t>::to_address_safe(pool_id_t pool_id,
                                                                            pool_offset_t pool_offset) // Static.
{
  const auto base_vaddr = Tl_copy_fwd::s_registry.this_thread_state()->lookup_safe(pool_id);
  return base_vaddr ? reinterpret_cast<void*>
                        (reinterpret_cast<uintptr_t>(base_vaddr) + pool_offset)
                    : static_cast<void*>(nullptr);

  /* As advertised/cautioned that only checks for pool_id's existence; the addr could be out of bounds.
   * @todo For the rationale in the doc header it *would* be good to eliminate that too (and update doc header).
   * This is however not totally straightforward.  We *can* do it by checking m_rc_pools_by_id, but that
   * requires currently an awful central mutex lock: not acceptable whatsoever, even if "only" called from
   * borrow_object() (hence on the first-class borrowed handles themselves, not all pointers such as in
   * STL containers).  We could however store pool sizes in Tl_copy_fwd a/k/a Thread_local_pool_lookup_fwd
   * and rejigger its API somewhat.  It's more complex and would only be used in this context, but there should
   * be little to no perf effect generally. */
}

template<typename Shm_arena_t>
void Borrower_shm_pool_collection_repository<Shm_arena_t>::from_address(const void* address,
                                                                        pool_id_t& pool_id,
                                                                        pool_offset_t& pool_offset) // Static.
{
  // Fast-path.
  Core_rev::get_instance().lookup(address, pool_id, pool_offset);
}

template<typename Shm_arena_t>
void Borrower_shm_pool_collection_repository<Shm_arena_t>::register_collection(owner_id_t owner_id,
                                                                               collection_id_t collection_id,
                                                                               Shared_name&& pool_name_base)
{
  using flow::util::stat::fetch_add;
  using flow::util::stat::update_hi_wmark;
  using std::make_shared;

  // Locks and unlocks central mutex 1.
  Tl_copy_fwd::s_registry.while_locked([&](auto&&...)
  {
    const Uniq_collection_id key{owner_id, collection_id};
    const auto iter = m_collection_data_map.find(key);

    bool novel;

    if (iter == m_collection_data_map.end())
    {
      // New collection.
      novel = true;
      /* Pass nullptr logger: this borrower-side collection only opens/maps + unmaps/closes pools; we log all of
       * that here in `*this` (which has a thread-safe Log_context_mt), around the calls.  So the collection itself
       * need not -- and does not -- log; this sidesteps having to give it a concurrently-valid logger. */
      m_collection_data_map.emplace(key,
                                    Collection_data{ make_shared<Borrower_shm_pool_collection>
                                                       (nullptr, collection_id, std::move(pool_name_base)),
                                                     1 });
      FLOW_LOG_TRACE_LOCKED("Registered new collection [" << collection_id << "], owner [" << owner_id << "].");
    }
    else
    {
      // Existing collection.
      novel = false;
      auto& coll = iter->second;
      ++coll.m_use_count;
      assert((coll.m_collection->get_pool_name_base() == pool_name_base)
             && "Registered same owner/collection (allowed) but with different pool-name-base?  Bug.");
      FLOW_LOG_TRACE_LOCKED("Incremented use of collection [" << collection_id << "], owner [" << owner_id << "] to "
                            "[" << coll.m_use_count << "].");
    }

    { // Stats.
      Borrower_pool_stats* per_arena_stats_ptr = nullptr; // There is a case where this stays as-is.
      const auto iter = m_per_arena_borrower_pool_stats.find(key);
      if (iter == m_per_arena_borrower_pool_stats.end())
      {
        if (novel)
        {
          m_per_arena_borrower_pool_stats[key].reset(per_arena_stats_ptr = new Borrower_pool_stats);
          per_arena_stats_ptr->m_uniq_arena_id.m_id1 = uint64_t(owner_id);
          per_arena_stats_ptr->m_uniq_arena_id.m_id2 = uint64_t(collection_id);

          // That new per-arena stat-set is MRU... but have we reached the size where we'll need to evict LRU?
          if (m_per_arena_borrower_pool_stats.size() > S_N_PER_ARENA_BORROWER_POOL_STATS_SZ_LIMIT)
          {
            m_per_arena_borrower_pool_stats.erase(--m_per_arena_borrower_pool_stats.past_oldest());
            assert((m_per_arena_borrower_pool_stats.size() == S_N_PER_ARENA_BORROWER_POOL_STATS_SZ_LIMIT)
                   && "Some bug in the simple LRU/MRU algorithm?");
          }

          /* Note: That (combined with the 4 touch() calls throughout this class) = our LRU-eviction policy;
           * but it's not the only possible policy.  See m_per_arena_borrower_pool_stats doc header for discussion
           * of this choice/possible alternative. */
        }
        /* else if (!novel): // && (key not in m_per_arena_borrower_pool_stats)
         *   Do not insert `key` yet; and leave `per_arena_stats_ptr == nullptr` for below.  The reason is
         *   a bit subtle.  Firstly, !novel here wouldn't even be possible normally; it can only happen if
         *   the LRU-ejection seen above happened for this `key` earlier (so m_per_arena_borrower_pool_stats got too
         *   big, etc.).  So if that did happen, and since then `key`-arena did get unborrowed/deregistered the
         *   same # of times that it had been registered, and now that `key`-arena is being borrowed/registered
         *   again (hence `novel == false` now), suppose we allowed m_per_arena...[key] to be inserted now.
         *   Seems fine, but now suppose later it gets deregistered.  ACCUMULATORs will be fine as always, more or
         *   less, but take a GAUGE -- say, m_n_borrowed_arenas -- which is (naturally) unsigned.  We wouldn't
         *   ++ it below during this register_collection(); but the later deregister_collection() would `--` it.
         *   So it would become -1 (underflow).  We could make it signed, but then it would be a weird-looking
         *   result.  We just don't want any of that junk, not to even reason about it further; in other places
         *   too (like we can't claim it's always either 1 or 0 anymore, which as of this writing is assert()ed
         *   and stated in docs).  Essentially: we were forced to junk that arena's data by the LRU considerations --
         *   we didn't want to (all else being equal), but we had to so as to avoid unbounded growth of the map.
         *   So continuing that, we are applying that junk-it policy for that arena... until things "even out."
         *   Once the # of register_collection()s and deregister_collection()s for it become equal -- `novel`
         *   becomes true for it -- then there's no reason not to keep those stats.  (In any case, if novel=true
         *   then we don't even know if `key` was junked in the past, so we have to insert it, even if we didn't
         *   want to for some reason.) */
      } // if (key not in m_per_arena_borrower_pool_stats)
      else // if (key in m_per_arena_borrower_pool_stats)
      {
        per_arena_stats_ptr = iter->second.get();
        m_per_arena_borrower_pool_stats.touch(iter);
        /* It is now MRU again.  Perf: touch() is a constant-time few ptr assignments.  Plus there aren't typically
         * so many arenas being juggled at a given time as to make this a frequent non-no-op anyway. */
      }

      /* This next part could -- one could say -- be outside of the lock-section.  It is not.  Why?
       *   - m_per_arena_borrower_pool_stats: See its doc header for why this is inside the lock-section.
       *     (In short: perf-wise does not matter.  Correctness-wise: it could matter due to our fairly-unique
       *     semantic situation w/r/t an arena being potentially borrowed and un-borrowed concurrently through
       *     different sessions.  So keep it simple and definitely-correct by mirroring m_collection_data_map.)
       *   - m_borrower_pool_stats: Keep it here as well to avoid having to worry about it (and perf-wise still
       *     does not matter) and for consistency. */

      if (per_arena_stats_ptr)
      {
        fetch_add(&per_arena_stats_ptr->m_arena_register_count, 1);
        if (novel)
        {
          fetch_add(&per_arena_stats_ptr->m_arena_first_register_count, 1);
          update_hi_wmark(&per_arena_stats_ptr->m_n_borrowed_arenas_hi_wmark,
                          fetch_add(&per_arena_stats_ptr->m_n_borrowed_arenas, 1) + 1);
        }
      }
      { // Same deal, for the total-stats guy.
        fetch_add(&m_borrower_pool_stats.m_arena_register_count, 1);
        if (novel)
        {
          fetch_add(&m_borrower_pool_stats.m_arena_first_register_count, 1);
          update_hi_wmark(&m_borrower_pool_stats.m_n_borrowed_arenas_hi_wmark,
                          fetch_add(&m_borrower_pool_stats.m_n_borrowed_arenas, 1) + 1);
        }
      }
    } // Stats.
  }); // Tl_copy_fwd::s_registry.while_locked() // Locks and unlocks central mutex 1.
} // Borrower_shm_pool_collection_repository::register_collection()

template<typename Shm_arena_t>
void Borrower_shm_pool_collection_repository<Shm_arena_t>::deregister_collection(owner_id_t owner_id,
                                                                                 collection_id_t collection_id)
{
  using flow::util::stat::fetch_add;
  using flow::util::stat::fetch_sub;

  // Locks and unlocks central mutex 1.
  Tl_copy_fwd::s_registry.while_locked([&](auto&&...)
  {
    const Uniq_collection_id key{owner_id, collection_id};
    const auto iter = m_collection_data_map.find(key);
    assert((iter != m_collection_data_map.end()) && "Deregistering unknown collection; bug?");
    bool novel;

    auto& coll = iter->second;
    if (coll.m_use_count == 1)
    {
      novel = true;
      m_collection_data_map.erase(iter);
      FLOW_LOG_TRACE_LOCKED("Removed collection [" << collection_id << "], owner [" << owner_id << "].");
    }
    else
    {
      novel = false;
      --coll.m_use_count;
      FLOW_LOG_TRACE_LOCKED("Decremented use of collection [" << collection_id << "], owner [" << owner_id << "] "
                            "to [" << coll.m_use_count << "].");
    }

    { // Stats.
      const auto iter = m_per_arena_borrower_pool_stats.find(key);
      if (iter != m_per_arena_borrower_pool_stats.end())
      {
        const auto per_arena_stats_ptr = iter->second.get();

        // See note in register_collection() w/r/t why the following is inside lock-section instead of outside.

        fetch_add(&per_arena_stats_ptr->m_arena_deregister_count, 1);
        if (novel)
        {
          fetch_add(&per_arena_stats_ptr->m_arena_last_deregister_count, 1);
#ifndef NDEBUG
          const auto prev_val =
#endif
          fetch_sub(&per_arena_stats_ptr->m_n_borrowed_arenas, 1);
          assert((prev_val == 1)
                 && "Per-arena m_n_borrowed_arenas should only ever be 0 or 1.  Bug in register_collection()?");
        } // if (novel)

        m_per_arena_borrower_pool_stats.touch(iter);
        // It is now MRU again.  Perf: same comment as in register_collection().
      } // if (key in m_per_arena_borrower_pool_stats)
      /* else if (key not in m_per_arena_borrower_pool_stats):
       *   Normally this won't happen, as a thing must be registered before it can be deregistered, and the former
       *   would have inserted `key`, but there is a corner case: an, apparently, incredibly long-lived
       *   session/arena, such that a massive # of others have been more recently messed-with than the last time
       *   this one was touched stats-wise.  It would then get evicted; and it is conceivable that finally the
       *   borrowing finally ended now... but too late: no per-arena stats to update.  There isn't a great way
       *   to deal with it without producing some odd-looking values in the individual `Borrower_pool_stats`.  This
       *   case is basically pathological, so they'll just have to make do without this particular arena's stats.
       *   (It is even conceivable that *another* session would be opened to borrow that same arena -- hence
       *   register_collection(), in the !novel case, will insert it and even make it MRU anew.  Why only if !novel?
       *   See register_collection() itself; but in short: it also helps avoid odd-looking values due to imbalance
       *   of [de]register_collection() calls.)
       *
       *   (By "odd-looking" we mean potentially documented-invariant- (possibly assert()-) breaking.  E.g.
       *   m_n_borrowed_arenas, per arena, should be in [0, 1] according to docs and an assert() as of this writing.) */

      { // Same deal, for the total-stats guy.
        fetch_add(&m_borrower_pool_stats.m_arena_deregister_count, 1);
        if (novel)
        {
          fetch_add(&m_borrower_pool_stats.m_arena_last_deregister_count, 1);
          fetch_sub(&m_borrower_pool_stats.m_n_borrowed_arenas, 1);
        }
      }
    } // Stats.
  }); // Tl_copy_fwd::s_registry.while_locked() // Locks and unlocks central mutex 1.
} // Borrower_shm_pool_collection_repository::deregister_collection()

template<typename Shm_arena_t>
void Borrower_shm_pool_collection_repository<Shm_arena_t>::register_shm_pool(owner_id_t owner_id,
                                                                             collection_id_t collection_id,
                                                                             pool_id_t pool_id,
                                                                             std::size_t pool_size)
{
  using ipc::shm::arena_lend::Shm_pool;
  using Shm_pool_ptr = std::shared_ptr<Shm_pool>;
  using flow::util::stat::fetch_add;
  using flow::util::stat::update_hi_wmark;

  Shm_pool_ptr new_shm_pool;

  // Locks and unlocks central mutex 1.
  Tl_copy_fwd::s_registry.while_locked([&](const auto& state_per_thread)
  {
    const Uniq_collection_id key{owner_id, collection_id};
    const auto rc_iter = m_rc_pools_by_id.find(pool_id);
    if (rc_iter == m_rc_pools_by_id.end())
    {
      // New pool -- open it via the collection.
      const auto coll_iter = m_collection_data_map.find(key);
      assert((coll_iter != m_collection_data_map.end()) && "Registering pool for unknown collection; bug?");

      Error_code err_code;
      new_shm_pool = coll_iter->second.m_collection->open_shm_pool(pool_id, pool_size, &err_code);
      if (!new_shm_pool) // open_shm_pool() set err_code; we log it just below (the collection itself stays silent).
      {
        FLOW_LOG_FATAL_LOCKED("Could not open/memory-map (as borrower) SHM-pool.  "
                              "This should have just been created owner-side, so failure to open implies a massive "
                              "breakdown of Flow-IPC SHM-arena-lend system.  If the failure was at the memory-map "
                              "stage, something is likely borked about the machine by now.  Either way we cannot "
                              "meaningfully recover.  Aborting.  Precipitating "
                              "error: [" << err_code << "] [" << err_code.message() << "].");
        assert(false && "Could not open/memory-map (as borrower) SHM-pool (details should be logged just above).  "
                          "This should have just been created owner-side, so failure to open implies a massive "
                          "breakdown of Flow-IPC SHM-arena-lend system.  If the failure was at the memory-map stage, "
                          "something is likely borked about the machine by now.  Either way we cannot meaningfully "
                          "recover.");
        std::abort();
      }
      // else

      m_rc_pools_by_id.emplace(pool_id, Pool_rc_data{ new_shm_pool, 1, { owner_id, collection_id } });
      tl_copy_fwd_push_pools(state_per_thread);
    }
    else
    {
      // Already registered (by another Shm_session to the same pid.cid) -- increment use-count.
      auto& rc = rc_iter->second;
      ++rc.m_use_count;

      assert(rc.m_shm_pool);
      assert(std::size_t(rc.m_shm_pool->get_size()) == pool_size);

      FLOW_LOG_TRACE_LOCKED("Incremented use of SHM pool ID [" << pool_id << "], name "
                            "[" << rc.m_shm_pool->get_name() << "], owner [" << owner_id << "], "
                            "collection [" << collection_id << "].");
    }

    { // Stats.
      const auto iter = m_per_arena_borrower_pool_stats.find(key);
      if (iter != m_per_arena_borrower_pool_stats.end())
      {
        const auto per_arena_stats_ptr = iter->second.get();

        // See note in register_collection() w/r/t why the following is inside lock-section instead of outside.

        fetch_add(&per_arena_stats_ptr->m_pool_register_count, 1);
        if (new_shm_pool)
        {
          fetch_add(&per_arena_stats_ptr->m_pool_open_count, 1);
          update_hi_wmark(&per_arena_stats_ptr->m_n_open_pools_hi_wmark,
                          fetch_add(&per_arena_stats_ptr->m_n_open_pools, 1) + 1);
          update_hi_wmark(&per_arena_stats_ptr->m_mapped_sz_hi_wmark,
                          fetch_add(&per_arena_stats_ptr->m_mapped_sz, pool_size) + pool_size);
        }

        m_per_arena_borrower_pool_stats.touch(iter);
        // It is now MRU again.  Perf: same comment as in register_collection().
      } // if (key in m_per_arena_borrower_pool_stats)
      /* else if (key not in m_per_arena_borrower_pool_stats):
       *   Normally this won't happen, as... <see comment similar spot in deregister_collection()>.
       *   So in this case some arena gets registered and then not touched for ages stats-wise, and then
       *   suddenly some allocation owner-side triggers a SHM-pool creation.  We'll just have to not record
       *   that stats-event for it either. */

      { // Same deal, for the total-stats guy.
        fetch_add(&m_borrower_pool_stats.m_pool_register_count, 1);
        if (new_shm_pool)
        {
          fetch_add(&m_borrower_pool_stats.m_pool_open_count, 1);
          update_hi_wmark(&m_borrower_pool_stats.m_n_open_pools_hi_wmark,
                          fetch_add(&m_borrower_pool_stats.m_n_open_pools, 1) + 1);
          update_hi_wmark(&m_borrower_pool_stats.m_mapped_sz_hi_wmark,
                          fetch_add(&m_borrower_pool_stats.m_mapped_sz, pool_size) + pool_size);
        }
      }
    } // Stats.
  }); // Tl_copy_fwd::s_registry.while_locked() // Locks and unlocks central mutex 1.

  if (new_shm_pool)
  {
    Core_rev::get_instance().insert(*new_shm_pool); // Locks and unlocks central mutex 2.

    FLOW_LOG_TRACE_LOCKED("Inserted SHM pool ID [" << pool_id << "], name [" << new_shm_pool->get_name() << "], "
                          "size [" << pool_size << "], owner [" << owner_id << "], "
                          "collection [" << collection_id << "].");
  }
  // else { Pool structure unchanged; no rev-lookup (or fwd-lookup for that matter) update needed. }
} // Borrower_shm_pool_collection_repository::register_shm_pool()

template<typename Shm_arena_t>
void Borrower_shm_pool_collection_repository<Shm_arena_t>::deregister_shm_pool(owner_id_t owner_id,
                                                                               collection_id_t collection_id,
                                                                               pool_id_t pool_id)
{
  using flow::util::stat::fetch_add;
  using flow::util::stat::fetch_sub;

  void* removed_pool_base = {};

  // Locks and unlocks central mutex 1.
  Tl_copy_fwd::s_registry.while_locked([&](const auto& state_per_thread)
  {
    const Uniq_collection_id key{owner_id, collection_id};
    const auto rc_iter = m_rc_pools_by_id.find(pool_id);
    assert((rc_iter != m_rc_pools_by_id.end()) && "Deregistering unknown pool; bug?");
    size_t removed_pool_size = {}; // Init to avoid some false compiler/sanitizer warnings.

    auto& rc = rc_iter->second;
    if (rc.m_use_count == 1)
    {
      // No more references -- close the pool.
      const auto coll_iter = m_collection_data_map.find(key);
      assert((coll_iter != m_collection_data_map.end()) && "Collection not found during pool deregistration; bug?");

      removed_pool_size = rc.m_shm_pool->get_size();

      if (!coll_iter->second.m_collection->release_shm_pool(rc.m_shm_pool))
      {
        FLOW_LOG_FATAL_LOCKED("Failed to fully close/unmap (as borrower) SHM pool [" << pool_id << "], "
                              "size [" << removed_pool_size << "], in collection [" << collection_id << "], "
                              "owner [" << owner_id << "].  Something is likely borked about the machine now.  "
                              "Bailing out.");
        assert(false && "Failed to fully close/unmap (as borrower) SHM pool.  "
                          "Something is likely borked about the machine now.");
        std::abort();
      }
      // else
      FLOW_LOG_TRACE_LOCKED("Successfully closed SHM pool [" << pool_id << "], size [" << removed_pool_size << "], "
                            "in collection [" << collection_id << "], owner [" << owner_id << "].");

      removed_pool_base = rc.m_shm_pool->get_address();

      m_rc_pools_by_id.erase(rc_iter);
      tl_copy_fwd_push_pools(state_per_thread);
    }
    else
    {
      // Still registered (by another Shm_session to the same pid.cid) -- just decrement use-count.
      --rc.m_use_count;
      FLOW_LOG_TRACE_LOCKED("Decremented counter to [" << rc.m_use_count << "] for SHM pool [" << pool_id << "] "
                            "in collection [" << collection_id << "], owner [" << owner_id << "].");
    }

    { // Stats.
      const auto iter = m_per_arena_borrower_pool_stats.find(key);
      if (iter != m_per_arena_borrower_pool_stats.end())
      {
        const auto per_arena_stats_ptr = iter->second.get();

        // See note in register_collection() w/r/t why the following is inside lock-section instead of outside.

        fetch_add(&per_arena_stats_ptr->m_pool_deregister_count, 1);
        if (removed_pool_base)
        {
          fetch_add(&per_arena_stats_ptr->m_pool_close_count, 1);
          fetch_sub(&per_arena_stats_ptr->m_n_open_pools, 1);
          fetch_sub(&per_arena_stats_ptr->m_mapped_sz, removed_pool_size);
        }

        m_per_arena_borrower_pool_stats.touch(iter);
        // It is now MRU again.  Perf: same comment as in register_collection().
      } // if (key in m_per_arena_borrower_pool_stats)
      /* else if (key not in m_per_arena_borrower_pool_stats):
       *   Normally this won't happen, as... <see comment similar spot in register_shm_pool()>.
       *   So in this case some arena gets registered and then not touched for ages stats-wise, and then
       *   perhaps a relevant session ends; each pool in the arena gets deregistered.
       *   We'll just have to not record that stats-event for it either. */

      { // Same deal, for the total-stats guy.
        fetch_add(&m_borrower_pool_stats.m_pool_deregister_count, 1);
        if (removed_pool_base)
        {
          fetch_add(&m_borrower_pool_stats.m_pool_close_count, 1);
          fetch_sub(&m_borrower_pool_stats.m_n_open_pools, 1);
          fetch_sub(&m_borrower_pool_stats.m_mapped_sz, removed_pool_size);
        }
      }
    } // Stats.
  }); // Tl_copy_fwd::s_registry.while_locked() // Locks and unlocks central mutex 1.

  if (removed_pool_base)
  {
    Core_rev::get_instance().erase(removed_pool_base); // Locks and unlocks central mutex 2.

    FLOW_LOG_TRACE_LOCKED("Deregistered SHM pool [" << pool_id << "] in collection [" << collection_id << "], "
                          "owner [" << owner_id << "].");
  }
  // else { Pool structure unchanged; no rev-lookup (or fwd-lookup for that matter) update needed. }
} // Borrower_shm_pool_collection_repository::deregister_shm_pool()

template<typename Shm_arena_t>
Shared_name Borrower_shm_pool_collection_repository<Shm_arena_t>::recompute_pool_name(owner_id_t owner_id,
                                                                                      collection_id_t collection_id,
                                                                                      pool_id_t pool_id) const
{
  /* Really just need to lookup the Borrower_shm_pool_collection which has the (immutable) pool-name-base
   * (and hence ability to just .recompute_pool_name(pool_id)) but do need to lock briefly in order to do that. */
  std::shared_ptr<Borrower_shm_pool_collection> coll;
  Tl_copy_fwd::s_registry.while_locked([&](auto&&...)
  {
    const auto iter = m_collection_data_map.find(Uniq_collection_id{owner_id, collection_id});
    assert((iter != m_collection_data_map.end()) && "Collection not found for recompute_pool_name(); bug?");
    coll = iter->second.m_collection;
  });
  return coll->recompute_pool_name(pool_id);
}

template<typename Shm_arena_t>
void Borrower_shm_pool_collection_repository<Shm_arena_t>::tl_copy_fwd_push_pools
       (const typename decltype(Tl_copy_fwd::s_registry)::State_per_thread_map& caches_per_thread) const
{
  using Map = typename Tl_copy_fwd::Map;

  // This continues [de]register_shm_pool().  Tl_copy_fwd::s_registry.while_locked() is in effect.

  /* Prep an (essentially) copy of central m_rc_pools_by_id; then move a copy of that into
   * each thread-local Tl_copy_fwd. */

  // Attn!  We are using tl_copy_fwd_pull_pools() -- called during *pull* from elsewhere -- as a helper here.
  Map pool_bases_by_id;
  tl_copy_fwd_pull_pools(&pool_bases_by_id);

  // As a small optimization, for the last one move the temp pool_bases_by_id itself.
  std::size_t n = caches_per_thread.size();
  for (const auto& [cache, nil] : caches_per_thread)
  {
    // The per-thread mutex is locked inside here (reminder: to protect against simultaneous read in thread Ti).
    cache->push_pools(((--n) == 0) ? std::move(pool_bases_by_id)
                                   : Map{pool_bases_by_id});
  }
} // Borrower_shm_pool_collection_repository::tl_copy_fwd_push_pools()

template<typename Shm_arena_t>
void Borrower_shm_pool_collection_repository<Shm_arena_t>::tl_copy_fwd_pull_pools
       (typename Tl_copy_fwd::Map* pool_bases_by_id) const
{
  /* If called during actual pull directly from Thread_local_pool_lookup_fwd: FYI:
   * By contract this is called from Tl_copy_fwd() (the ctor), meaning in this thread Ti a new
   * thread-local map-copy is being created on-demand.  Tl_copy_fwd::s_registry.while_locked() is in effect (by contract
   * of how Thread_local_state_registry works).
   *
   * If called from tl_copy_fwd_push_pools() as a helper: FYI:
   * We ourselves ensured Tl_copy_fwd::s_registry.while_locked() is in effect. */

  assert(pool_bases_by_id->empty() && "By contract we are to load an empty map-copy.");
  pool_bases_by_id->reserve(m_rc_pools_by_id.size()); // Slight optimization.  Should compile with any hash-map `Map`.
  for (const auto& id_and_rc : m_rc_pools_by_id)
  {
    pool_bases_by_id->emplace(id_and_rc.first, id_and_rc.second.m_shm_pool->get_address());
  }
}

template<typename Shm_arena_t>
std::vector<ipc::shm::arena_lend::stat::Shm_pool_info>
  Borrower_shm_pool_collection_repository<Shm_arena_t>::shm_pool_live_info() const
{
  using boost::range::sort;
  using std::vector;

  vector<Shm_pool_info> shm_pools;

  // Locks and unlocks central mutex 1.
  Tl_copy_fwd::s_registry.while_locked([&](auto&&...)
  {
    for (const auto& [id, rc] : m_rc_pools_by_id)
    {
      shm_pools.push_back({ uint64_t(id),
                            // (@todo The null check must be a defensive thing... maybe remove... but can't hurt?)
                            size_t(rc.m_shm_pool ? rc.m_shm_pool->get_size() : 0),
                            { uint64_t(rc.m_uniq_collection_id.first), uint64_t(rc.m_uniq_collection_id.second) },
                            rc.m_use_count });
    }
  }); // Tl_copy_fwd::s_registry.while_locked() // Locks and unlocks central mutex 1.

  sort(shm_pools, [](const auto& shm_pool_info_a, const auto& shm_pool_info_b) -> bool
                    { return shm_pool_info_a.m_id < shm_pool_info_b.m_id; });
  return shm_pools;
} // Borrower_shm_pool_collection_repository::shm_pool_live_info()

template<typename Shm_arena_t>
const ipc::shm::arena_lend::stat::Borrower_pool_stats&
  Borrower_shm_pool_collection_repository<Shm_arena_t>::stats
    (Borrower_pool_stats_list* per_arena_stats, size_t per_arena_stats_sz_limit_or_0, size_t* n_arenas_omitted) const
{
  using flow::util::stat::stats_assign;
  using boost::adaptors::map_values;
  using boost::adaptors::transformed;
  using boost::range::sort;
  using std::vector;
  using std::tie;

  if (per_arena_stats)
  {
    // Locks and unlocks central mutex 1.
    Tl_copy_fwd::s_registry.while_locked([&](auto&&...)
    {
      /* Reminder: m_per_arena_borrower_pool_stats goes from newest()/begin() to past_oldest()/end(); hence
       * so will this range. */
      const auto stats_ptrs_rng
        = m_per_arena_borrower_pool_stats | map_values | transformed([](const auto& own) -> auto { return own.get(); });

      // Stop after n_ptrs most-recently-touched entries.
      size_t n_ptrs = m_per_arena_borrower_pool_stats.size();
      (per_arena_stats_sz_limit_or_0 != 0) && (per_arena_stats_sz_limit_or_0 < n_ptrs)
        && (n_ptrs = per_arena_stats_sz_limit_or_0);

      n_arenas_omitted && (*n_arenas_omitted = (m_per_arena_borrower_pool_stats.size() - n_ptrs));

      // Could instead compactly do make_iterator_range(rng.begin(), next(rng.begin(), n)).  Longer but IMO clearer:
      vector<Borrower_pool_stats*> stats_ptrs;
      stats_ptrs.reserve(n_ptrs);
      for (const auto stats_ptr : stats_ptrs_rng)
      {
        stats_ptrs.push_back(stats_ptr);
        if (stats_ptrs.size() == n_ptrs) { break; }
      }

      /* Careful... even though stats() is a rare call, one might still prefer to not do the remaining steps
       * under lock unnecessarily.  Own<> ensures pointer stability for the ptrs in stats_ptrs, so that's all
       * fine.  Unfortunately, though, the LRU-eviction in register_collection() can erase elements from
       * m_per_arena_borrower_pool_stats, so dereferencing a saved ptr from stats_ptrs can be a use-after-free.
       * So just stay under lock here until done-done. */

      sort(stats_ptrs, [](auto stats_ptr_a, auto stats_ptr_b) -> bool
      {
        return tie(stats_ptr_a->m_uniq_arena_id.m_id1, stats_ptr_a->m_uniq_arena_id.m_id2)
               <
               tie(stats_ptr_b->m_uniq_arena_id.m_id1, stats_ptr_b->m_uniq_arena_id.m_id2);
      });

      per_arena_stats->resize(stats_ptrs.size());
      auto stats_ptr_it = stats_ptrs.begin();
      for (auto& target_stats : *per_arena_stats)
      {
        target_stats.reset(new Borrower_pool_stats);
        stats_assign(target_stats.get(), **(stats_ptr_it++));
      }
    }); // Tl_copy_fwd::s_registry.while_locked() // Locks and unlocks central mutex 1.
  } // if (per_arena_stats)

  return m_borrower_pool_stats;
} // Borrower_shm_pool_collection_repository::stats()

template<typename Shm_arena_t>
void Borrower_shm_pool_collection_repository<Shm_arena_t>::stats_reset()
{
  using flow::util::stat::stats_reset;
  using boost::adaptors::map_values;
  using boost::adaptors::transformed;

  stats_reset(&m_borrower_pool_stats, Borrower_pool_stats{}); // The across-all-arenas totals.

  /* Reset each per-arena slab too, for coherence with the totals.  As in stats(): the key-set is mutated only
   * under central mutex 1, and ptrs are stable, but entries are sometimes LRU-erased; so do everything under
   * the lock.  (Orthogonally: The resets write live `atomic`s -- standard under-concurrency stat behavior.) */
  Tl_copy_fwd::s_registry.while_locked([&](auto&&...)
  {
    const auto rng = m_per_arena_borrower_pool_stats | map_values
                       | transformed([](const auto& own) -> auto { return own.get(); });
    for (auto* const stats_ptr : rng)
    {
      stats_reset(stats_ptr, Borrower_pool_stats{});
    }
  });

  /* One might wonder, here, why not "just" m_per_arena_borrower_pool_stats.clear() here instead of the
   * range-fu + stats_reset() dance above?  It might be particularly tempting knowing how we have that LRU-eviction
   * mechanism in register_collection(), trying to keep this thing at most a certain size.  A reset opportunistically
   * making that size=0 sounds pretty nice!
   *
   * It would be wrong, just because of how flow::util::stat stats work.  Only ACCUMULATORs get zeroed by resets,
   * so if that's all we had, then it would be fine (if brittle, as ACCs-only now doesn't mean ACCs only forever).
   * GAUGEs in particular are untouched by proper stat-resets, so blowing them away (effectively zeroing them)
   * would be wrong.  (Why do we blow them away in the LRU-eviction case anyway?  Answer: We're forced to do it,
   * and we give up those stat-chunks willingly to avoid unbounded growth of m_per_arena_borrower_pool_stats.) */
} // Borrower_shm_pool_collection_repository::stats_reset()

} // namespace ipc::session::shm::arena_lend::detail
