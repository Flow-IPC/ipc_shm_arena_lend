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

#include "ipc/shm/arena_lend/detail/thread_lcl_pool_lookup.hpp"
#include "ipc/shm/arena_lend/detail/arena_lend_fwd.hpp"
#include "ipc/shm/arena_lend/shm_pool.hpp"
#include <flow/util/thread_lcl_obj.hpp>
#include <flow/util/util.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <map>
#include <memory>
#include <vector>
#include <type_traits>

namespace ipc::shm::arena_lend::detail
{

// Types.

/**
 * (Internal-use) Helper singleton for each of at least the 2 essential central flat SHM-pool repositories
 * (Owner_shm_pool_repository, `Borrower_shm_pool_collection_repository`), `Shm_pool_repo_lookup_core_rev<R>`
 * (where `R` is a repo type) encapsulates `R`'s central flat set of SHM-pool IDs and base-vaddrs
 * and, therein, a performant multi-threaded reverse-lookup from vaddr to the containing SHM-pool's ID and
 * offset in bytes from the pool-base vaddr.
 *
 * @note This guy is for reverse-lookup (basically vaddr -> containing pool).  How come there isn't
 *       a `Shm_pool_repo_lookup_core_fwd`, a similar thing but for forward lookup, as of this writing?
 *       Answer: perhaps there should be; but the plain truth is at least 2 places need access to
 *       the Thread_local_pool_lookup_rev engine; while only 1 place (`Borrower_shm_pool_collection_repository`
 *       as of this writing) needs access to Thread_local_pool_lookup_fwd.  Hence there is no code duplication
 *       by doing the legwork required by Thread_local_pool_lookup_fwd directly in that one place; and it
 *       does cut down on boiler-plate.  Though the alternative would be more symmetrical, and the consumer
 *       of Thread_local_pool_lookup_fwd would be shorter and cleaner.  YMMV, they say.
 *
 * This is arguably "just" glue code, so we need not pontificate further, and the method
 * doc headers should make clear-enough how to use a `*this`.  To gain relevant background, one could start
 * with doc headers for either or both of our 2 known consumers noted at the top of our doc header.  That would
 * be the approach from a high level.  Alternatively or additionally one could approach from the low level:
 * Thread_local_pool_lookup_rev.
 *
 * ### Impl ###
 * The great majority of our insides is dictated by the requirements of Thread_local_pool_lookup_rev; we are
 * a glorified wrapper around that singleton (w/r/t #Repository) and the canonical map #m_pools_by_base.
 *
 * @tparam Repository_t
 *         See #Repository.
 * @tparam LOOKUP_CAN_FAIL
 *         See #S_LOOKUP_CAN_FAIL.
 * @tparam Shm_arena_t
 *         See #Shm_arena.
 */
template<typename Repository_t, bool LOOKUP_CAN_FAIL, typename Shm_arena_t>
class Shm_pool_repo_lookup_core_rev
{
public:
  // Types.

  /// The consuming repository type (e.g., Owner_shm_pool_repository, `Borrower_shm_pool_collection_repository`).
  using Repository = Repository_t;
  /// See Pool_lookup_global_stats.
  using Shm_arena = Shm_arena_t;
  /// `true` <=> lookup() can legitimately fail to find the containing pool.
  static constexpr bool S_LOOKUP_CAN_FAIL = LOOKUP_CAN_FAIL;
  /// Short-hand for pool-offset type.
  using pool_offset_t = Shm_pool::size_t;
  /// Short-hand for the thread-local reverse-lookup cache type parameterized on #Repository.
  using Tl_copy = Thread_local_pool_lookup_rev<Shm_pool_repo_lookup_core_rev, S_LOOKUP_CAN_FAIL, Shm_arena>;

  // Methods.

  /**
   * Returns the process-wide singleton instance of this class.
   * @return Reference to the singleton.
   */
  static Shm_pool_repo_lookup_core_rev& get_instance();

  /**
   * Adds a SHM-pool to the canonical map and pushes the updated state into all per-thread
   * reverse-lookup caches.  Locks `Tl_copy::s_registry` for the duration.
   *
   * @param pool
   *        Pool to register (ID and base-vaddr are extracted; no ownership taken).
   */
  void insert(const Shm_pool& pool);

  /**
   * Undoes insert().
   *
   * @param base
   *        Base vaddr of the pool to remove.
   */
  void erase(const void* base);

  /**
   * Internal callback invoked by #Tl_copy ctor (via `Thread_local_state_registry`) during per-thread
   * cache creation.
   *
   * @note Not for direct use by callers of this class.  Public only because #Tl_copy
   *       (via `Thread_local_state_registry`) requires it.
   *
   * @param pools_sorted_by_base
   *        Out-arg: map to populate with base-vaddr -> Pool_by_base entries.
   */
  void tl_copy_rev_pull_pools(typename Tl_copy::Map* pools_sorted_by_base) const;

  /**
   * Performs a high-performance, as for (e.g.) Owner_shm_pool_repository::from_address().  The contract
   * is identical (within reason).
   *
   * @param address
   *        See above.
   * @param pool_id
   *        See above.
   * @param pool_offset
   *        See above.
   */
  void lookup(const void* address, pool_id_t& pool_id, pool_offset_t& pool_offset) const;

private:
  // Methods.

  /**
   * Helper for insert() and erase() that pushes the current canonical #m_pools_by_base state into all
   * per-thread #Tl_copy caches.  `Tl_copy::s_registry.while_locked()` must be in effect at entry.
   *
   * @param state_per_thread
   *        Arg passed to `Tl_copy::s_registry.while_locked()` in the caller.
   */
  void tl_copy_push_pools(const typename decltype(Tl_copy::s_registry)::State_per_thread_map& state_per_thread) const;

  // Data.

  /**
   * Canonical sorted map: pool-base vaddr -> Pool_by_base.  Guarded by `Tl_copy::s_registry`'s
   * central mutex (all accesses occur within `while_locked()`).
   */
  std::map<void*, Pool_by_base> m_pools_by_base;
}; // class Shm_pool_repo_lookup_core_rev

/**
 * (Internal-use) Singleton repository for SHM-pool data pertaining to all SHM-arenas owned by this process.
 * All arenas (each being an Owner_shm_pool_collection; for SHM-jemalloc: sub-class jemalloc::Ipc_arena)
 * in this process funnel their pool info here, enabling offset-pointer resolution
 * (to_address(), from_address()).
 *
 * @see Borrower_shm_pool_collection_repository which is a *somewhat* comparable singleton on the opposing side
 *      (the *borrower* as opposed to the *owner*).
 * @todo It might be nice to name similar ops between Borrower_shm_pool_collection_repository and
 *       Owner_shm_pool_repository similarly: `insert()` vs `register_shm_pool()`.  Though, the status quo is
 *       defensible; `Borrower_...` registers 2+ types of things; `Owner_...` only SHM-pools.
 *
 * Understanding the similarities and differences between what the two do can be very helpful in grokking
 * the SHM-arena-lend (SHM-jemalloc more specifically at least) design.  On the owner side, a `*this` is
 * straightforward, as a given SHM-pool's lifecycle is controlled by the memory-manager (e.g., jemalloc as
 * wrapped by jemalloc::Memory_manager and jemalloc::Ipc_arena).  So we just do enough to make
 * global pool-base/pool-ID lookup work.
 *
 * We shall now explain.  We'll use SHM-jemalloc (the first and as of this writing only arena-lend-type SHM-provider
 * in Flow-IPC) in the explanation, as being specific makes it easier.
 *
 * During a low-level SHM-allocation (jemalloc::Memory_manager::allocate(), arena-aware), jemalloc shall decide
 * it needs a new *extent* (contiguous vaddr area) of a certain size but instead of memory-mapping (`mmap()` in Linux
 * et al) it in process heap as normal it'll invoke an *extent hook* we (in jemalloc::Ipc_arena) register
 * from the start; now we are in charge of the memory-mapping -- but also importantly SHM-pool creation.  In short
 * that means creating a uniquely named SHM-object and a SHM-pool-handle to it (`shm_open()` in Linux et al) and
 * then memory-mapping it in a SHM-oriented way.  This is when an *owner* SHM-pool -- `Shm_pool` object more or less --
 * must be created and, relevantly to the present singleton, registered.  That's insert().  Note that this occurs
 * much more rarely than a precipitating jemalloc `allocate()`, but it is always done synchronously within it in
 * the end user's allocating thread alone.
 *
 * In subsequent operations (perhaps jemalloc::Memory_manager::deallocate() and
 * jemalloc::Memory_manager::destroy_arena(), the latter triggered by jemalloc::Ipc_arena destruction) jemalloc
 * would decide to unmap an extent and again trigger an extent hook as above.  This time it might lead to
 * the removal of a SHM-pool.  (Incidentally this implies there are zero allocated buffers within it, though
 * `*this` does not care about it.)  The SHM-pool must be deregistered.  That's erase().
 *
 * @note A key point is that an *owner* SHM-pool like this, as identified by its `pool_id_t` (which is globally unique
 *       forever within the machine boundary, up to reboot), can never come back after this moment.  This is in contrast
 *       to the borrower process, wherein (e.g.) session 1 can borrow an arena and pool within it, use it, then
 *       reach session-end, and then session 2 might start and borrow them again -- that time at a different base-vaddr!
 *       This fact drives the different implementation of the forward- and reverse-lookups in this
 *       Owner_shm_pool_repository versus the borrower-side counterpart Borrower_shm_pool_collection_repository.
 *       It is easy to miss/forget this subtlety and thus design/code bugs by making the two things too similar.
 * @note It is possible for process X to borrow from process X via an X-X session.  Things still work without
 *       corner-case code (in our context anyway), at least in Linux, because `mmap()` of already-mapped SHM-pool
 *       X shall auto-return the same base vaddr; hence there will not be 2+ vaddrs referring to the same RAM byte.
 *
 * All that said, on the owner side the principal SHM-pool lifecycle managers are *not*
 * Owner_shm_pool_repository but rather the individual arenas' per-arena `Shm_pool_collection`s
 * (jemalloc::Ipc_arena for SHM-jemalloc).  They already track all the pools.  So why do we exist?
 * Answer: exclusively to enable (performant) forward- and reverse-lookups, to_address() and from_address().
 * This enables high-performance computations for `Shm_pool_offset_ptr`s... which will absolutely litter
 * any STL-compliant-container-stored-in-SHM.
 *
 * insert() and erase() merely feed the information needed for those to work.
 *
 * @note Borrower_shm_pool_collection_repository is different, in that it *also* tracks the (borrowed) SHM-pool
 *       (handle) lifecycles.  It has no need, at this same relatively high level, to break them down by arenas.
 *       Owner is the administrator of pools; borrower is only a consumer of pools.
 *
 * ### Impl: forward/reverse lookup and the 2 mutexes ###
 * See Borrower_shm_pool_collection_repository eponymous doc header section.  Same thing here.
 *
 * ### Impl: Forward-lookup ###
 * Happily this part is extremely simple to make performant.  The reason that is the case here, but not the case
 * in Borrower_shm_pool_collection_repository, is what we wrote above re. how once a pool with its ID and base-vaddr
 * goes away, it can never come back (at all... but more saliently at a different base-vaddr).
 *
 * It is a per-thread hash-map (`Pool_bases_cache_map`, the per-thread cache).  In to_address(),
 * in the fast-path it'll find the desired `pool_id_t`, and that's it.  No lock.  The first time (slow-path) it
 * won't (a/k/a cache miss), in which case look it up in the single canonical map controlled by insert() and
 * erase(); this requires a mutex lock/unlock.
 *
 * We don't worry about removing entries from the thread-local maps on erase(), as that would be difficult; does not
 * really affect perf either way; and leaks only a bit of memory over time, in the big picture.
 *
 * @note Corollary: It is not correct to try to use to_address() to check whether a particular SHM-pool still
 *       exists (is mapped).
 *
 * ### Impl: Forward-lookup: Challenge at thread exit ###
 * There is a stealthy potential problem around thread exit.  The basic scenario is simply to_address() executing
 * during thread exit (probably in some destructor).  (Formally or informally we aren't really worrying about the
 * user doing this, per se; but with or without that possibility there is for sure internal Flow-IPC code
 * that does it: `~Thread_lcl_obj_db_admin()` dtor is an important piece of code that runs at thread exit.
 * It'll run through any objects `construct()`ed by user in that thread that are alive but have use-count=0,
 * meaning no user references left and delete them.  (These are called zombies, but despite the name their existence
 * is totally normal; Thread_lcl_obj_db_admin et al operate in a thread-local, opportunistic/piggy-backed
 * fashion.)  When deleting a user T, one runs `~T()`.  If `T` is allocator-enabled, then STL-y code is likely
 * to perform from_address() and to_address() conversions, via `pointer_traits::pointer_to()` and the like.
 * to_address() is us, on the owner side.  `~Thread_lcl_obj_db_admin()` dtor is driven by
 * `Thread_local_state_registry` which is, in this context, basically `boost::thread_specific_ptr`.) So when
 * the to_address() occurs during a `boost::thread_specific_ptr` cleanup execution (in the aforementioned particular
 * case that cleanup being the `TLODBA` dtor), this problem can manifest.
 *
 * That is because `thread_specific_ptr` cleanup runs at an interesting time.  The source code for
 * `flow::util::Thread_local_ptr` explains much of this stuff.  In short, though, at least in Linux:
 * `tsp` cleanup runs *before* `thread_local` deinit phase (when thread is a `boost::thread`) and *after*
 * `thread_local` deinit phase (when thread is an `std::thread`).
 *
 * So what's the issue with to_address() at this time?  If it executes before `thread_local` deinit (`boost::thread`):
 * no issue.  If it executes after it then, if accessing the actual `thread_local` map, it's undefined behavior:
 * the map is destroyed.
 *
 * The solution is conceptually straightforward: is the per-thread map still alive?  If yes: great, use it.
 * If no: OK, fall back to the canonical central map, with the locking.  Perf at that stage is of low importance.
 * That is exactly the service `flow::util::Thread_local_obj_deinit_safe` provides (see its doc header for the
 * full mechanism, including the first-access-near-thread-death corner cases); so we access the per-thread map
 * exclusively through it (see #Pool_bases_cache).  Death of the map is tracked per-map, not per-phase -- so
 * this resolves even `thread_local` deinit of (something else) trying to use to_address() for whatever reason.
 *
 * @note We mentioned from_address().  There is also borrower-side
 *       Borrower_shm_pool_collection_repository::to_address() (and `from_address()`).  Do these 3 have some similar
 *       problem?  Answer: Yes and no.  No, in that they do not use `thread_local` deinit but, ultimately,
 *       `thread_specific_ptr` cleanup as of this writing.  Yes, in that it means, at least, that same
 *       `~Thread_lcl_obj_db_admin()` dtor -- also off a `thread_specific_ptr` cleanup -- may try to use
 *       various `from_address()` and borrower-`to_address()` impls which use `thread_specific_ptr` cleanup too.
 *       It is a different complication, out of our purview here.  Spoiler alert: It works fine, as
 *       `tsp` cleanup executes a stubborn loop which allows cleanup functions to re-insert TL-values (which are
 *       added to the stubborn-cleanup list).  So the modules responsible for the address-lookup participate in this
 *       mechanism (as of this writing through a `Thread_local_state_registry` through a `Thread_local_ptr` which
 *       is on top of a `tsp`), and it works fine.
 *
 * ### Impl: Reverse-lookup ###
 * See Borrower_shm_pool_collection_repository eponymous doc header section.  Same thing here, with the notable
 * difference that in our case Shm_pool_repo_lookup_core_rev::S_LOOKUP_CAN_FAIL is `true` (addresses not-in-SHM --
 * such as to stack or regular heap -- are supported).
 * In short: Just use `detail::Shm_pool_repo_lookup_core_rev<..., true>` (#Core_rev).
 *
 * ### Logging ###
 * There isn't any.  The reason is that the would-be logging ops, insert() and erase(), are simple, and we
 * TRACE-log (at least) all the same info from other parts of the code such as jemalloc::Ipc_arena.
 * The per-thread-fanning-out in #Core_rev is somewhat interesting, but `Core_rev` does not log due to an extreme
 * priority on perf.  So we just don't need a `Logger`.
 *
 * @tparam Shm_arena_t
 *         The SHM-arena type (e.g. `jemalloc::Ipc_arena`).  Each distinct `Shm_arena_t` yields a fully
 *         independent singleton and thus fully independent registries.  `Shm_arena_t`'s API is not accessed;
 *         it serves as a compile-time discriminator only.
 */
template<typename Shm_arena_t>
class Owner_shm_pool_repository :
  private boost::noncopyable
{
public:
  // Types.

  /// The SHM-arena type; see class template parameter doc header.
  using Shm_arena = Shm_arena_t;
  /// Short-hand for pool-offset type.
  using pool_offset_t
    = typename Shm_pool_repo_lookup_core_rev<Owner_shm_pool_repository, true, Shm_arena>::pool_offset_t;
  /// Short-hand for `shared_ptr<Shm_pool>`.
  using Shm_pool_ptr = std::shared_ptr<Shm_pool>;

  // Methods.

  /**
   * Returns the process-wide singleton instance of this class.
   *
   * @return Reference to the singleton.
   */
  static Owner_shm_pool_repository& get_instance();

  /**
   * Key high-performance API: Given an in-SHM location as the pool and in-pool offset (values that remain constant
   * between the borrowing process(es) and us, the lending process) returns the process-local (local process being us,
   * the owner) vaddr pointing to that in-SHM location.
   *
   * Context: In particular, when processing fancy-pointers `Shm_pool_offset_ptr` (in STL-compliant structures
   * stored in SHM and owned by this process) and top-level owned objects, this shall be called.
   * E.g., when traversing some in-SHM STL-container, such lookups may be quite frequent.
   *
   * Perf: as of this writing this is a thread-local hash-map lookup of vaddr by integer; plus an offset addition.
   * A slow-path exists (once per pool per thread) wherein one locks a central mutex, inserts into thread-local
   * hash-map, and unlocks mutex.
   *
   * ### Corner cases ###
   * Formally: If `pool_id` is invalid (never owned, no longer owned): undefined behavior.  Informally:
   *   - If never owned: undefined behavior (in the classic crashy sense).
   *   - If no longer owned (removed, whether due to memory manager deciding to eliminate pool or because
   *     entire owner arena has shut down): might be classic crashy undefined-behavior; might return
   *     formerly correct result but now invalid memory.
   *
   * So do not try to use to_address() to somehow suss out whether a given pool still exists.
   *
   * Assuming `pool_id` is owned: Returned vaddr shall simply equal the pool's base-vaddr plus
   * `pool_offset`.  If it is negative or beyond the pool's size, then the returned address shall be outside
   * the specified pool; we shall not bounds-check it.
   *
   * @param pool_id
   *        The identifier of the SHM-pool.
   * @param pool_offset
   *        The offset in bytes off the pool `pool_id` base vaddr.
   * @return Non-null vaddr.
   */
  static void* to_address(pool_id_t pool_id, pool_offset_t pool_offset);

  /**
   * Key high-performance API: Essentially the reverse of to_address().
   *
   * Context: A reverse-lookup should be much less frequent than forward-lookup to_address(); but typical STL-container
   * code at times must figure out a `pointer` (in our case `Shm_pool_offset_ptr`) from a vaddr.  (In heap-land
   * the two are the same, but in SHM-land they are not.)
   *
   * Perf: identical to `Borrower_shm_pool_collection_repository::from_address()`.
   *
   * If `address` is not in any owned pool, `pool_id` is set to 0 and `pool_offset` is left untouched.
   *
   * @param address
   *        Vaddr to within a SHM-pool.
   * @param pool_id
   *        Out-arg where the identifier of the pool containing `address` shall be placed; or zero if not found.
   * @param pool_offset
   *        Out-arg where the offset of a byte within pool `pool_id` at which `address` points shall
   *        be placed, unless output `pool_id == 0`.
   */
  static void from_address(const void* address, pool_id_t& pool_id, pool_offset_t& pool_offset);

  /**
   * Registers an owned SHM-pool.  The pool is unconditionally added (no ref-counting; owner-side pool IDs are
   * globally unique and never recur).
   *
   * @param shm_pool
   *        Pool to register; moved-from on return.
   */
  void insert(Shm_pool_ptr&& shm_pool);

  /**
   * Deregisters an owned SHM-pool.  Undoes insert().
   *
   * @param pool_id
   *        Globally unique pool identifier.
   */
  void erase(pool_id_t pool_id);

private:
  // Types.

  /// Short-hand for the reverse-lookup (vaddr -> pool-ID + offset) module.  It's a singleton like us.
  using Core_rev = Shm_pool_repo_lookup_core_rev<Owner_shm_pool_repository, true, Shm_arena>;
  /// Short-hand for the mutex type guarding #m_pool_bases_by_id.
  using Pool_bases_by_id_mutex = flow::util::Mutex_non_recursive;
  /// Short-hand for the lock type for #Pool_bases_by_id_mutex.
  using Pool_bases_by_id_lock = flow::util::Lock_guard<Pool_bases_by_id_mutex>;

  /// Per-thread cache map: pool-ID -> base-vaddr.  Populated lazily in to_address().  Entries are never removed.
  using Pool_bases_cache_map = boost::unordered_flat_map<pool_id_t, void*>;

  /**
   * Death-aware access to this thread's #Pool_bases_cache_map: to_address() can run during thread exit
   * (see class doc header, section "Impl: Forward-lookup: Challenge at thread exit"), when a plain
   * `thread_local` map may be destroyed already; `this_thread_obj_or_null()` returns null at such a time, and
   * to_address() falls back to the canonical (locking) lookup.  See `Thread_local_obj_deinit_safe` doc header.
   *
   * (The tag: #Pool_bases_cache_map is the same type across our per-`Shm_arena` instantiations -- and
   * conceivably beyond -- so we disambiguate with the most natural tag available, ourselves.)
   *
   * Bottom line: It is as if we have a `static thread_local Pool_bases_cache_map s_cache` here, and to access
   * `s_cache` -- so as to do something with it -- one instead uses `*(Pool_bases_cache::this_thread_obj_or_null())`;
   * except that that accessor returns null when `s_map` has been destroyed already.  (Then we can fall-back to
   * cache-avoiding behavior that does not need would-be `s_cache`.)
   */
  using Pool_bases_cache = flow::util::Thread_local_obj_deinit_safe<Pool_bases_cache_map, Owner_shm_pool_repository>;

  // Constructors/destructor.

  /// Constructor.  Zero-arg due to singleton pattern.
  Owner_shm_pool_repository();

  // Methods.

  /**
   * Forward-lookup slow-path helper: returns the base vaddr for the given pool ID from
   * the canonical #m_pool_bases_by_id (locking #m_pool_bases_by_id_mutex).
   *
   * @param pool_id
   *        Pool to look up.
   * @return Base vaddr of the pool.
   */
  void* pull_pool_base_by_id(pool_id_t pool_id) const;

  // Data.

  /// Mutex guarding #m_pool_bases_by_id.
  mutable Pool_bases_by_id_mutex m_pool_bases_by_id_mutex;

  /**
   * Canonical map: pool-ID -> `Shm_pool` handle (and thus base-vaddr via `->get_address()`).
   * Guarded by #m_pool_bases_by_id_mutex.
   */
  boost::unordered_flat_map<pool_id_t, Shm_pool_ptr> m_pool_bases_by_id;
}; // class Owner_shm_pool_repository

// Template implementations.

// Shm_pool_repo_lookup_core_rev template implementations.

template<typename Repository_t, bool LOOKUP_CAN_FAIL, typename Shm_arena_t>
Shm_pool_repo_lookup_core_rev<Repository_t, LOOKUP_CAN_FAIL, Shm_arena_t>&
  Shm_pool_repo_lookup_core_rev<Repository_t, LOOKUP_CAN_FAIL, Shm_arena_t>::get_instance()
{
  static Shm_pool_repo_lookup_core_rev s_core; // Thread-safe in C++17 at least.
  return s_core;
}

template<typename Repository_t, bool LOOKUP_CAN_FAIL, typename Shm_arena_t>
void Shm_pool_repo_lookup_core_rev<Repository_t, LOOKUP_CAN_FAIL, Shm_arena_t>::insert(const Shm_pool& pool)
{
  /* We are not in thread Ti for all Ti (possibly one of them).  Overview:
   *   -# Lock Tl_copy::s_registry central mutex; its State_per_thread_map cannot change during that time, and
   *      we also synchronize this set with central pool collection m_pools_by_base *and* its copies in each
   *      thread Ti's Tl_copy.
   *   -# So update both of those things.
   *      -# To protect against simultaneous read of thread-local essentially-copy of m_pools_by_base
   *         in lookup() in each thread Ti within each tl_copy_push_pools(), a per-thread mutex is locked. */

  Tl_copy::s_registry.while_locked([&](const auto& state_per_thread)
  {
    m_pools_by_base[pool.get_address()] = Pool_by_base{ pool.get_id(),
                                                        S_LOOKUP_CAN_FAIL ? pool.get_size() : 0 };
    tl_copy_push_pools(state_per_thread);
  });
}

template<typename Repository_t, bool LOOKUP_CAN_FAIL, typename Shm_arena_t>
void Shm_pool_repo_lookup_core_rev<Repository_t, LOOKUP_CAN_FAIL, Shm_arena_t>::erase(const void* base)
{
  /* Overview: identical to insert(); just the nature of the update to m_pools_by_base is erasure instead
   * of insertion. */

  Tl_copy::s_registry.while_locked([&](const auto& state_per_thread)
  {
    m_pools_by_base.erase(const_cast<void*>(base));
    tl_copy_push_pools(state_per_thread);
  });
}

template<typename Repository_t, bool LOOKUP_CAN_FAIL, typename Shm_arena_t>
void Shm_pool_repo_lookup_core_rev<Repository_t, LOOKUP_CAN_FAIL, Shm_arena_t>::tl_copy_push_pools
       (const typename decltype(Tl_copy::s_registry)::State_per_thread_map& caches_per_thread) const
{
  using Map = typename Tl_copy::Map;

  // This continues insert() or erase().  Tl_copy::s_registry.while_locked() is in effect (`caches_per_thread` arg).

  /* Prep an (essentially) copy of central m_pools_by_base; then move a copy of that into
   * each thread-local Tl_copy. */

  // Attn!  We are using tl_copy_rev_pull_pools() -- called during *pull* from elsewhere -- as a helper here.
  Map pools_sorted_by_base;
  tl_copy_rev_pull_pools(&pools_sorted_by_base);

  // As a small optimization, for the last one move the temp pools_sorted_by_base itself.
  size_t n = caches_per_thread.size();
  for (const auto& [cache, nil] : caches_per_thread)
  {
    // The per-thread mutex is locked inside here (reminder: to protect against simultaneous read in thread Ti).
    cache->push_pools(((--n) == 0) ? std::move(pools_sorted_by_base)
                                   : Map{pools_sorted_by_base});
  }
} // Shm_pool_repo_lookup_core_rev::tl_copy_push_pools()

template<typename Repository_t, bool LOOKUP_CAN_FAIL, typename Shm_arena_t>
void Shm_pool_repo_lookup_core_rev<Repository_t, LOOKUP_CAN_FAIL, Shm_arena_t>::tl_copy_rev_pull_pools
       (typename Tl_copy::Map* pools_sorted_by_base) const
{
  using std::is_same_v;
  using Map = typename Tl_copy::Map;
  using Src_map = std::decay_t<decltype(m_pools_by_base)>;

  /* If called during actual pull directly from Thread_local_pool_lookup_rev: FYI:
   * By contract this is called from Tl_copy() (the ctor), meaning in this thread Ti a new
   * thread-local map-copy is being created on-demand.  Tl_copy::s_registry.while_locked() is in effect (by contract
   * of how Thread_local_state_registry works).
   *
   * If called from tl_copy_push_pools() as a helper: FYI:
   * We ourselves ensured Tl_copy::s_registry.while_locked() is in effect. */

  assert(pools_sorted_by_base->empty() && "By contract we are to load an empty map-copy.");

  // Optimize by making some assumptions about the types involved.

  static_assert(is_same_v<Map, boost::container::flat_map<void*, Pool_by_base>>,
                "Optimization below assumes target Map = sorted flat map (sorted-by-key in array).  "
                  "Try maintaining this in spirit if possible.  Do note this is not the fast-path though.");
  static_assert(is_same_v<Src_map, std::map<void*, Pool_by_base>>,
                "Optimization below assumes Src_map = sorted map of some kind (std::map is one such thing, "
                  "and there are probably others, but we are just sanity-checking here.  "
                  "Try maintaining this in spirit if possible.  Do note this is not the fast-path though.");

  pools_sorted_by_base->reserve(m_pools_by_base.size()); // Internal array pre-sized for fuss-free insertion.
  for (const auto& base_and_pool : m_pools_by_base)
  {
    // Give hint to insert at the end, since we are iterating in sorted order.  Hence in total the loop is linear-time.
    pools_sorted_by_base->emplace_hint(pools_sorted_by_base->end(),
                                       base_and_pool.first, base_and_pool.second);
  }
} // Shm_pool_repo_lookup_core_rev::tl_copy_rev_pull_pools()

template<typename Repository_t, bool LOOKUP_CAN_FAIL, typename Shm_arena_t>
void Shm_pool_repo_lookup_core_rev<Repository_t, LOOKUP_CAN_FAIL, Shm_arena_t>::lookup(const void* address,
                                                                                       pool_id_t& pool_id,
                                                                                       pool_offset_t& pool_offset) const
{
  /* Fast-path.  Internally this locks a per-thread mutex to protect against simultaneous
   * tl_copy_push_pools() (unlikely hence low chance of lock contention and only against 1 thread at worst). */
  Tl_copy::s_registry.this_thread_state()->lookup(address, pool_id, pool_offset);
}

// Owner_shm_pool_repository template implementations.

template<typename Shm_arena_t>
Owner_shm_pool_repository<Shm_arena_t>::Owner_shm_pool_repository() = default;

template<typename Shm_arena_t>
Owner_shm_pool_repository<Shm_arena_t>&
  Owner_shm_pool_repository<Shm_arena_t>::get_instance() // Static.
{
  static Owner_shm_pool_repository s_repository; // Thread-safe in C++17 at least.
  return s_repository;
}

template<typename Shm_arena_t>
void* Owner_shm_pool_repository<Shm_arena_t>::to_address(pool_id_t pool_id, pool_offset_t pool_offset) // Static.
{
  void* pool_base;
  const auto pool_bases_by_id = Pool_bases_cache::this_thread_obj_or_null();
  if (pool_bases_by_id)
  {
    auto& pool_base_ref = (*pool_bases_by_id)[pool_id];
    if (!pool_base_ref)
    {
      // Slow-path.  (E.g., it will lock m_pool_bases_by_id_mutex to access central m_pool_bases_by_id.)
      pool_base_ref = get_instance().pull_pool_base_by_id(pool_id);
    }
    // else { Fast-path }
    pool_base = pool_base_ref;
  }
  else
  {
    /* Thread teardown, past this thread's `thread_local` deinit: the per-thread cache map is gone.
     * Skip it; use the canonical lookup -- the very same one the cache-miss slow-path uses below.  Perf does not
     * matter much here.  (Why would this ever happen?  See class doc header Impl section(s) for details.) */
    pool_base = get_instance().pull_pool_base_by_id(pool_id);
  }
  return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pool_base) + pool_offset);
}

template<typename Shm_arena_t>
void Owner_shm_pool_repository<Shm_arena_t>::from_address(const void* address,
                                                          pool_id_t& pool_id,
                                                          pool_offset_t& pool_offset) // Static.
{
  // Fast-path.  It will lock a mutex but with ~zero lock contention (see .lookup() body).
  Core_rev::get_instance().lookup(address, pool_id, pool_offset);
}

template<typename Shm_arena_t>
void Owner_shm_pool_repository<Shm_arena_t>::insert(Shm_pool_ptr&& shm_pool)
{
  const auto& pool = *shm_pool; // Save ref before potential move.

  { // Locks and unlocks central mutex 1.
    Pool_bases_by_id_lock pool_bases_by_id_lock{m_pool_bases_by_id_mutex};
    m_pool_bases_by_id.emplace(pool.get_id(), std::move(shm_pool));
  }

  Core_rev::get_instance().insert(pool); // Locks and unlocks central mutex 2.

  /* Hence insert()/erase() can interleave with another insert()/erase().
   * Class doc header section "Impl: forward/reverse lookup and the 2 mutexes" talks about this. */
}

template<typename Shm_arena_t>
void Owner_shm_pool_repository<Shm_arena_t>::erase(pool_id_t pool_id)
{
  /* As in insert(), essentially do two independent ops, with their own locking each.
   * Though in this case we get the pool-base before op 1 so as to use it in op 2. */

  void* base;
  { // Locks and unlocks central mutex 1.
    Pool_bases_by_id_lock pool_bases_by_id_lock{m_pool_bases_by_id_mutex};

    const auto it = m_pool_bases_by_id.find(pool_id);
    base = it->second->get_address();

    m_pool_bases_by_id.erase(it);
  }

  Core_rev::get_instance().erase(base); // Locks and unlocks central mutex 2.
}

template<typename Shm_arena_t>
void* Owner_shm_pool_repository<Shm_arena_t>::pull_pool_base_by_id(pool_id_t pool_id) const
{
  // Protect against concurrent slow-path lookup().
  Pool_bases_by_id_lock pool_bases_by_id_lock{m_pool_bases_by_id_mutex};

  return m_pool_bases_by_id.find(pool_id)->second->get_address();
  // Unlock central mutex here.
}

} // namespace ipc::shm::arena_lend::detail
