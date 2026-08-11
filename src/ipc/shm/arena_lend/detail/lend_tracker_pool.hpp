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
#include "ipc/shm/arena_lend/detail/use_count_registry.hpp"
#include "ipc/shm/arena_lend/detail/arena_lend_fwd.hpp"
#include "ipc/util/detail/util.hpp"
#include "ipc/util/util_fwd.hpp"
#include "ipc/util/shared_name.hpp"
#include <flow/log/log.hpp>
#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/interprocess/indexes/null_index.hpp>
#include <optional>
#include <atomic>

namespace ipc::shm::arena_lend::detail
{

/**
 * This internal-use class represents, essentially, a handle to the use-count database (which the class can
 * open or create via ctor) pertaining to a SHM-arena-lending arena's objects `construct<>()`ed by a single particular
 * thread (to have constructed at least 1 such object ever) in the arena-owner process.  (As of this writing
 * SHM-jemalloc is the specific arena-lending SHM-provider that uses `*this`es.  If other arena-lend
 * SHM-providers beyond SHM-jemalloc were to exist, they should be able to use it in identical fashion.)
 *
 * ### Basic properties ###
 * The use-count database for that constructing-thread is itself stored in SHM, as a separate pool, with read/write
 * access, internally composed of essentially only atomic counts.  At the expense of some extra RAM use, this allows
 * both the lender-side process (that originally constructs objects in SHM) and the borrower-side process(es)
 * (that receives handles into such elsewhere-constructed in-SHM objects) to *performantly* and *safely* (in the face
 * of crashes at least) track how many extant handles (`shared_ptr`s as of this writing) of each given constructed
 * object remain; and once a particular object's such ref-count reaches 0 it is possible to garbage-collect it.
 * The performance comes from the fact that a handle being created just means a ref-count increment, while its
 * being destroyed (`shared_ptr` group ref-count reaches 0) is a mere decrement: No message-based IPC is required.
 * The safety comes from the nature of these updates each of which is essentially composed of an atomic increment
 * or decrement or exchange (etc.).
 *
 * ### How to use ###
 * Assume all discusion is of one specific arena (in SHM-jemalloc represented by a jemalloc::Ipc_arena object) A.
 * Broadly the users of Lend_tracker_pool can be classified as follows:
 *
 *   - (Admin mode) When A is asked to `construct<T>(...)` in thread X, it shall either construct or access
 *     a (presumably thread-local) Lend_tracker_pool via the `Create_only` ctor named #m_pool_name (A's choice).
 *     This creates, or accesses, a SHM-pool tracking all objects constructed by this same thread X, until X is joined.
 *     A currently-available use-count slot is then obtained via use_count_new(); the use-count `1` (one) is placed
 *     there (that is: one handle to this `T` is extant).
 *   - When A is asked to `lend_object<T>(p)` a particular handle (to a `T`), `p`, in thread Y (which is often the
 *     original allocating thread X -- but not necessarily), this indicates the handle will be IPC-transmitted to
 *     another process, where via `borrow_object<T>()` that process will obtain its own handle `p1`.  To avoid
 *     reaching ref-count 0 before then, the use-count for `p` must immediately (in `lend_object()`) be incremented.
 *     Hence use_count_inc() is called.  (The use-count slot index had to be memorized inside the representation of
 *     `p` obtained by `construct()` in preceding bullet point.)  E.g., `p` was only just constructed, then
 *     the use-count would reach `2`.  (Lending can be done repeatedly, so it can rise further each time.)
 *   - (Admin mode) When a particular handle `p` is invalidated (as of this writing: `shared_ptr<T>` group reaches
 *     ref-count zero, not to be confused with the use-count tracked by a `*this`) in the original constructing
 *     thread X: Such a thread by definition already has a Lend_tracker_pool available, cted as noted above
 *     in "Admin mode."  So one calls use_count_dec_admin(), opportunistically so due to thread X being the
 *     thread where this is happening.  This method can performantly both decrement the use-count; *and* if it thus
 *     reaches 0 immediately return the use-count slot into the set of available ones.
 *     - Since the use-count slot is returned, thread X can now destroy/deallocate the `T` (none of our business but
 *       that's what happens).
 *   - (Client mode) If a handle `p` or `p1` is invalidated (just as in the preceding bullet), but this occurs
 *     in a borrowing process (not the original constructing one); or in the owner process but in some thread Y
 *     that is not X: The "Admin mode"-cted Lend_tracker_pool object from thread X is thread-local to X and not
 *     applicable in this other thread (or possibly even this process, if this is borrowed `p1` as opposed to
 *     constructed `p`).  Therefore one shall either construct or access an (again, presumably thread-local)
 *     Lend_tracker_pool but via the `Open_only` ctor.  (The #m_pool_name chosen when originally creating the thing
 *     in first bullet above must be known somehow.)  Hence one uses use_count_dec() (cf. use_count_dec_admin())
 *     which shall just decrement the use-count.  However, even if it thereby reaches 0, at that moment we cannot
 *     (the way we could in "Admin mode" in bullet 3 above) return the use-count slot just yet.  We don't know that
 *     that's what our user would want.
 *
 * So far so good, but the last bullet leaves a clear hole: If it was not the original allocating process-and-thread
 * that happened to be the one to get a given object's use-count down to zero, but instead (and this is more typical)
 * another thread in the lending or borrowing process, then when/how is the use-count slot returned and/or the
 * object is destroyed/deallocated?  Technically/formally the answer is:
 *   - The user (i.e., object-DB layer above Lend_tracker_pool) must track all extant objects itself; and must
 *     (according to heuristics outside of our purview) at sufficiently frequent times go through them.  For each
 *     one, grab its use-count index `idx` (which, again, must be stored inside/alongside each handle like `p`) and
 *     simply call `use_count_return_if_unused(idx)`; this will return `idx` if and only if that use-count is indeed
 *     zero; and if so then return `true`; else `false`.  If it returns `true`, the user can destroy/deallocate.
 *     - Formally/technically this can be done by any thread in any process; but read on.
 *   - Because scanning *all* extant constructed objects raises obvious performance concerns, a `*this` provides
 *     two key tools to aid any such algorithm:
 *     - n_unused() returns the total number of extant (in-use/not-returned) use-count slots for which the use-count
 *       has decreased to `0` (zero).  Therefore one can stop any exhaustive scan upon reaching this-many
 *       use_count_return_if_unused() calls returning `true`.
 *     - emit_unused_ct_idx_hints() emits up to a small number (e.g., 16 as of this writing) of use-count slots
 *       for which the use-count has decreased to zero.  Therefore, before (typically even instead of) doing an
 *       exhaustive scan, one can begin with these emitted ones, executing use_count_return_if_unused() on each.
 *       That alone may well already reach `n_unused()` slots; in which case: all done.
 *     - Note: During any such "eviction" algorithm's execution, the value n_unused() and what is returned by
 *       emit_unused_ct_idx_hints() may well change due to concurrent work by other thread(s) and process(es).
 *       Not to worry: They should be viewed as (very good but not at-all-times-exactly-accurate-forever) hints.
 *       Basically: whatever use-count-zero slots one doesn't catch in that sweep, one will catch in the next.
 *       And, if in some situation perf is of no concern, one can always simply do the exhaustive scan.
 *
 * ### FYI: when and where the unused-slot scans occur ###
 * In reality, at least as of this writing -- though the following is not formally required on our account --
 * the heuristic-scans described just above are probably to be done by each "Admin mode" `Lend_tracker_pool`'s
 * owner thread X.  That means, e.g., the same thread that allocated a thing can naturally deallocate it (helpful
 * for thread-caching perf of memory-allocation algorithms like jemalloc).  As for the heuristic controlling *when*
 * the scans might occur: As of this writing we piggy-back onto a given thread's alloc-related events triggered
 * by the end user (e.g., `construct()` itself, `lend_object()`, whatever).  The hint tools n_unused() and
 * emit_unused_ct_idx_hints() make such scans quite quick.  Naturally events like thread-joining are also good
 * triggers for a scan.
 *
 * As of this writing these are the next-higher layer of the object-DB functionality.
 *
 * @see Thread_lcl_obj_db_admin: The "Admin mode" consumer of `Lend_tracker_pool`s.
 * @see Thread_lcl_obj_db_client: The "Client mode" consumer of `Lend_tracker_pool`s.
 *
 * ### Cleanup: destructors; dead() ###
 * The destructor of an "Admin mode"-cted Lend_tracker_pool shall remove #m_pool_name-named SHM-pool.
 * As usual, by the rules of SHM-handles, this only means that pool can no longer be opened by name
 * (hence a `*this` by that name cannot be "Client mode"-cted), but any existing SHM-handles remain valid.
 * Indeed this means already-extant "Client mode"-cted `Lend_tracker_pool`s accessing it remain valid.
 * Bottom line: This is timely cleanup for this particular resource.
 *
 * However: In the event of a crash/etc. (where that dtor does not get to run), this will not occur.  It is the
 * user's responsibility to ensure post-crash/etc. cleanup of #m_pool_name.  As of this writing SHM-jemalloc, at least,
 * does just that (by using a certain naming scheme, shared with other related kernel-persistent resources, and
 * opportunistic cleanup of all such resources at key time points).
 *
 * A key point is: when should one destroy an "Admin mode" `*this`?  Essentially by definition, if it is destroyed,
 * then (1) no more objects will be constructed (hence need use-counts) by the owner-process's thread that
 * precipitated the ction of such an admin-mode `*this`; *and* (2) no more objects tracked in it will be
 * destroyed by the owner-process either.  So once that is the case, that's when.
 *
 * This raises the question, what about a client-mode `*this`?  When it is destroyed, effectively what matters is
 * the SHM-pool handle is closed: once this happens to all such client-mode `*this`es pointing at the same pool
 * (pertaining to threads in the owner -- that aren't the admin/constructing thread -- and threads in the borrowers),
 * the SHM-memory will actually be returned to the system.  This could, over time, be important: if we just let
 * them live until their respective processes exit/abort, it could be a not-insignificant RAM leak.
 *
 * For that reason the SHM-pool itself contains a piece of `Metadata`, an atomic flag accessible by dead() accessor.
 * It starts `false` (SHM-pool is live); but just before the unlink/pool-handle-closing in admin-mode dtor,
 * that dtor shall set it to `true` (SHM-pool is dead).  No new objects will be tracked by it, and no existing
 * objects will be destroyed by it, is what `dead() == true` indicates.  Therefore our immediate client-mode user
 * (as of this writing Thread_lcl_obj_db_client in this context) can *periodically* check dead(); if `true` then
 * it is safe to destroy that Lend_tracker_pool.  If they all do this check at some reasonable frequency, then
 * the RAM-leak is averted.
 *
 * What is *periodically*?  We leave the answer to that to the (internal) user.  It could be time-based and/or
 * piggy-backed on something.  Not our purview.  See (as of this writing) Thread_lcl_obj_db_client.
 *
 * ### Impl ###
 * In this case the impl should be reasonably clear from the above description combined with various comments
 * throughout.  The main point are these:
 *   - It keeps a boost.interprocess SHM-pool `basic_managed_shared_memory` but instead of any default/general
 *     allocation-algorithm we substitute our own Use_count_registry.
 *   - Use_count_registry provides the compact use-count bitmap/array in the form of a very simple allocation-algo
 *     (it can only allocate use-count-integer-sized "objects").  This is where we plop our `atomic<>` use-counts.
 *   - It also provides some header space; in this area we place our Lend_tracker_pool::Metadata which is where we
 *     store the hint data: The count n_unused() and the extant-but-ready-to-return hint array for
 *     emit_unused_ct_idx_hints().
 */
class Lend_tracker_pool :
  private boost::noncopyable
{
public:
  // Types.

  /// Short-hand for arena_lend::stat::Obj_db_aux_pool_stats;
  using Obj_db_aux_pool_stats = arena_lend::stat::Obj_db_aux_pool_stats;

  // Constants.

  /**
   * The max number of unused-slot hints emit_unused_ct_idx_hints() can emit.
   *
   * Not necessary for algorithms; purely informational such as for logging/testing/etc.
   */
  static constexpr size_t S_N_UNUSED_IDX_HINTS = 16;

  // Data.

  /// `true` if admin-mode (create-mode ctor was used); `false` if client-mode (open-mode ctor was used).
  const bool m_is_creator;
  /// SHM pool name as set immutably at construction.
  const Shared_name m_pool_name;

  // Constructors/destructor.

  /**
   * Constructs the admin-mode (creating) handle: creates the SHM-pool named `pool_name` and initializes
   * its internal metadata.  After this, use_count_new() may be called to allocate use-count slots; and
   * this `*this` is suitable for use_count_dec_admin() as well.
   *
   * The created pool also becomes accessible to open-mode (client) handles constructed via the `Open_only`
   * overload, given the same `pool_name`.
   *
   * On failure (e.g., SHM-pool already exists, or OS resource limits hit) an exception is thrown.
   *
   * @see update_log_context(): Use it if `log_ctx` and/or `skip_fast_path_verbose_logging_ptr` pointers become
   *      obsolete.
   *
   * @param log_ctx
   *        All logging is done through this; it is assumed the `Logger*` stored inside can change at any time.
   *        A each TRACE-or-more-verbose log-call-site, `*log_ctx` is touched only if the
   *        `skip_fast_path_verbose_logging_ptr` fast-path filter passes.
   * @param skip_fast_path_verbose_logging_ptr
   *        Before any TRACE-or-more-verbose log-op is performed (including the already-cheap `.should_log()`),
   *        this flag's value is loaded (it is assumed it can change at any time); if `false` then no log-op.
   * @param pool_name
   *        Name of the SHM-pool to create (as for `shm_open()`).
   * @param mode_tag
   *        Tag type selecting this (create-mode/admin) overload.
   * @param stats
   *        Stats object to update throughout admin-mode activities.  Null will skip stat-keeping.
   * @param perms
   *        Permissions for the newly created SHM-pool.
   */
  Lend_tracker_pool(const flow::log::Log_context_mt* log_ctx,
                    const std::atomic<bool>* skip_fast_path_verbose_logging_ptr,
                    const Shared_name& pool_name, util::Create_only mode_tag,
                    Obj_db_aux_pool_stats* stats, const util::Permissions& perms = {});

  /**
   * Constructs the client-mode (opening) handle: opens the existing SHM-pool named `pool_name` that was
   * previously created by an admin-mode Lend_tracker_pool (possibly in another process).  After this,
   * use_count_inc() and use_count_dec() may be called to manipulate existing use-count slots.
   *
   * ### Error semantics (attention) ###
   * On failure (e.g., no such pool exists) an exception is thrown, namely `flow::error::Runtime_error`
   * (itself an `std::runtime_error`).
   *
   * In most cases it is best to treat it as a true exceptional condition (typically uncaught or caught somewhere
   * high-up in the call stack).  However, there may be situations (explaining which is outside our purview here)
   * where the specific error wherein *no such pool name exists* should be treated specially (perhaps seen as
   * less bad or not at all bad).  To check for this condition catch the `Runtime_error exc` and:
   * `const bool pool_name_not_found = exc.code() == boost::system::errc::no_such_file_or_directory`.  E.g.
   * can catch this; rethrow (`throw;`) if `!pool_name_not_found`; otherwise do something different.
   *
   * @param log_ctx
   *        See other ctor.
   * @param skip_fast_path_verbose_logging_ptr
   *        See other ctor.
   * @param pool_name
   *        Name of the SHM-pool to open; must have been previously created via the `Create_only` overload.
   * @param mode_tag
   *        Tag type selecting this (open-mode/client) overload.
   */
  Lend_tracker_pool(const flow::log::Log_context_mt* log_ctx,
                    const std::atomic<bool>* skip_fast_path_verbose_logging_ptr,
                    const Shared_name& pool_name, util::Open_only mode_tag);

  /**
   * Destroys this handle.  If `*this` is admin-mode (#m_is_creator is `true`), also unlinks the named
   * SHM-pool from the file system; existing open handles (client-mode `Lend_tracker_pool`s) remain valid
   * but no new ones can be opened by that name.  See the class doc header "Cleanup" section for further
   * discussion.
   */
  ~Lend_tracker_pool();

  // Methods.

  /**
   * Allocates a new use-count slot and initializes its value to 1 (one extant handle: the just-constructed
   * object).  This is to be called in admin mode upon a successful `construct<T>()`.
   *
   * If the pool has no free slots (all Use_count_registry::S_USE_COUNTS_CAPACITY` are in use), an exception
   * (likely `bad_alloc`) is thrown.
   *
   * @return The index of the newly allocated use-count slot; suitable for storage alongside the constructed
   *         object handle and subsequent use with use_count_inc(), use_count_dec(), etc.
   */
  use_ct_idx_t use_count_new();

  /**
   * Checks whether the use-count at slot `*use_ct_idx_ptr` has reached zero; if so, returns the slot to the
   * available pool (deallocates it) and sets `*use_ct_idx_ptr` to 0 (not-an-index).  Intended for use
   * in the admin thread's opportunistic-disposal scan: after emit_unused_ct_idx_hints() and n_unused() results
   * are obtained, one might:
   *   - use_count_return_if_unused() for each of the ones emitted by emit_unused_ct_idx_hints().
   *   - use_count_return_if_unused() for others, perhaps in some heuristic order (e.g., from least-to-most
   *     recently allocated).
   *   - Stop above once use_count_return_if_unused() returned `true` n_unused() times.
   *
   * @param use_ct_idx_ptr
   *        Pointer to the use-count-slot index to check.  If the slot's use-count is zero, `*use_ct_idx_ptr`
   *        is set to 0 on return.
   * @return `true` if and only if the use-count was indeed zero and the slot was returned (deallocated).
   */
  bool use_count_return_if_unused(use_ct_idx_t* use_ct_idx_ptr);

  /**
   * Atomically increments the use-count at the given slot.  To be called upon `lend_object()`: the handle is
   * about to be IPC-transmitted, so the use-count must be bumped to prevent premature zero before the
   * borrowing process creates its own handle (via `borrow_object()`).
   *
   * It is a bug to call this on a slot whose use-count is currently zero (the slot must refer to a live object).
   * If the use-count would overflow the maximum representable value, this is a fatal error (logged, then
   * `abort()`ed).
   *
   * @param use_ct_idx
   *        Index of the use-count slot to increment.
   * @return The use-count value *after* incrementing.
   */
  unsigned int use_count_inc(use_ct_idx_t use_ct_idx);

  /**
   * Atomically decrements the use-count at the given slot; for use in client mode (#m_is_creator is `false`).
   * If the use-count thereby reaches zero, this is recorded
   * internally (incrementing the count readable via n_unused() and, if possible, placing a hint for
   * emit_unused_ct_idx_hints()).
   *
   * Unlike use_count_dec_admin(), this does *not* return the slot even if the count reaches zero; the admin
   * thread shall do that via use_count_return_if_unused().
   *
   * It is a bug to call this on a slot whose use-count is currently zero.
   *
   * @param use_ct_idx
   *        Index of the use-count slot to decrement.
   * @return The use-count value *after* decrementing (0 if unused-referencing).
   */
  unsigned int use_count_dec(use_ct_idx_t use_ct_idx);

  /**
   * Atomically decrements the use-count at the given slot; for use in admin mode (#m_is_creator is `true`: the
   * in thread that created the slot via use_count_new()).  If the use-count thereby reaches zero,
   * the slot is immediately returned (deallocated), and `*use_ct_idx_ptr` is set to 0 (not-an-index).
   *
   * This is more efficient than the client-mode use_count_dec() path (see class doc header).
   *
   * It is a bug to call this on a slot whose use-count is currently zero.
   *
   * @param use_ct_idx_ptr
   *        Pointer to the use-count-slot index.  If the count reaches zero, `*use_ct_idx_ptr` is set to 0
   *        on return.
   * @return The use-count value *after* decrementing (0 if the slot was returned).
   */
  unsigned int use_count_dec_admin(use_ct_idx_t* use_ct_idx_ptr);

  /**
   * Returns the current use-count value at the given slot.  This is informational/diagnostic only;
   * the value may change concurrently, and if one merely wants the present value without intending to
   * also change it, one likely wants use_count_return_if_unused().
   *
   * @param use_ct_idx
   *        Index of the use-count slot to query.
   * @return The current use-count value (relaxed atomic load).
   */
  unsigned int use_count(use_ct_idx_t use_ct_idx) const;

  /**
   * Collects the indices of use-count slots that have been flagged (by use_count_dec()) as having reached zero
   * (but not returned to the available set, as use_count_dec_admin() or use_count_return_if_unused() might).
   * The indices are atomically consumed from internally SHM-stored data; each consumed hint is reset, so that it
   * can be reused in future similar call(s).
   *
   * Typically called by the admin thread's opportunistic-disposal scan as a fast first pass; the caller then
   * follows up with use_count_return_if_unused() on each emitted index.  See the class doc header for the
   * full suggested algorithm description.
   *
   * @tparam Container
   *         A sequence container type supporting `clear()`, `reserve()`, and `push_back()` (e.g., `vector`).
   * @param container
   *        Output: cleared, then populated with 0 or more use-count-slot indices.
   */
  template<typename Container>
  void emit_unused_ct_idx_hints(Container* container);

  /**
   * Returns the current count of use-count slots that are unused-referencing (use-count has reached zero
   * but the slot has not yet been returned/deallocated).  This can be used by the admin-thread scan algorithm
   * as a bound: stop scanning once this-many slots have been found and returned.
   *
   * ### Tips ###
   * The value may change concurrently; it is a hint, not a hard guarantee for any particular length of time.
   * However the following truth can be used to your advantage: Unless something might *concurrently return*
   * (as via use_count_return_if_unused() or use_count_dec_admin()) slots, the value this returns will not
   * concurrently go *down*.  So if your algorithm only does opportunistic-disposal scans from one thread,
   * it is never wrong to scan until one has returned N values, where N equals `n_unused()` at scan start.
   * (If it concurrently *increased*, then a future scan will "see" the new ones.)
   *
   * @return See above.
   */
  unsigned int n_unused() const;

  /**
   * Returns `false` (indicating liveness of the SHM-pool we handle) throughout; until the admin-mode
   * (#m_is_creator is `true`) `*this`'s destructor executes; then `true` forevermore.
   *
   * @see Class doc header "Cleanup" section for how and why to use this.  Spoilers: In particular note that while it
   *      will work fine regardless of #m_is_creator, we anticipate its key use would be when
   *      `m_is_creator == false` (client-mode).  Further: once this returns `true` it should be safe and indeed
   *      advisable to delete `*this`.  (For admin-mode `*this`, it would never return `true` until dtor runs
   *      in the first place.)
   *
   * The value behaves in fully-synchronized manner (as-if protected by a standard mutex).  That is it is always
   * set and (here) read with `seq_cst` (most restrictive) memory-ordering.  As such it assumed it will not be
   * accessed sufficiently frequently for this to matter.
   *
   * @return See above.
   */
  bool dead() const;

  /**
   * Updates the logging context initially provided via ctor.
   *
   * @param log_ctx
   *        See ctor.
   * @param skip_fast_path_verbose_logging_ptr
   *        See ctor.
   */
  void update_log_context(const flow::log::Log_context_mt* log_ctx,
                          const std::atomic<bool>* skip_fast_path_verbose_logging_ptr);

private:
  // Types.

  /**
   * The type of value stored in each use-count slot in #m_pool.
   *
   * This must (as enforced by a `static_assert`) have width equal to Use_count_registry::S_ALLOC_SZ, which is
   * the only allowed arg value we can pass to `m_pool->allocate()`.  Or in English: the slots that
   * the SHM-pool allocator system is designed to allocate/deallocate shall store `Atomic_use_ct`s, so
   * the size of slot it allocates/deallocates must equal the size of the value.
   *
   * ### Tuning ###
   * Why have this be larger?  Answer: The max value of `Atomic_use_ct::value_type` (2^32 minus 1, as of this writing)
   * is the max number of outstanding Shm_session::lend_object() calls + 1 Ipc_arena::construct() call (outstanding
   * meaning the resulting `shared_ptr` group has not reached ref-count 0).  If it is exceeded, everything explodes;
   * so if this is large, then that won't happen.
   *
   * Why have it be smaller?  Answer: RAM use by the underlying SHM-pool (referenced by #m_pool).  So consider,
   * for example, if `Atomic_use_ct::value_type` is `uint8_t`; max value 255.  Really it is only *required* to
   * `.lend_object(x)` if some process needs access to `x`; if a given process has already borrowed it, then it
   * is possible to avoid lending it again to *that* process, sending it to it via IPC, etc.  If this is always
   * followed, then this having value 255 means you've loaned `x` to 255 processes -- more than enough for the vast
   * majority of use cases.  That said: knowing that a certain process already has `x`, and thus we shouldn't lend
   * it again, probably sounds easy cosmically speaking -- but in reality it may not be easy at all, depending on
   * the design of the applications and all.  (We -- ygoldfel and co. -- discovered this in practice; and raised
   * the width of this value and hence the value Use_count_registry::S_ALLOC_SZ from 1 byte to 4 bytes.)
   *
   * @todo The width of Lend_tracker_pool::Atomic_use_ct and the corresponding constant Use_count_registry::S_ALLOC_SZ
   * is set to 4 bytes as of this writing; but it may be prudent to make this configurable at compile-time instead,
   * to allow for the trade-off discussed in nearby "Tuning" doc header section.
   */
  using Atomic_use_ct = std::atomic<uint32_t>;
  static_assert(sizeof(Atomic_use_ct::value_type) == Use_count_registry::S_ALLOC_SZ,
                "Size of 1 slot in the use-count-storing custom pool-allocator must equal the size of the value "
                  "we in fact store in each such slot.");

  /// Type of #m_pool.
  using Pool = ::ipc::bipc::basic_managed_shared_memory<char, Use_count_registry, ::ipc::bipc::null_index>;

  /// The header stored in #m_pool, ahead of the actual use-counts.
  struct Metadata
  {
    // Data.

    /// See Lend_tracker_pool::dead().
    std::atomic<bool> m_dead{false};

    /**
     * Essential for the perf of related algorithms, this stores the count of slots considered unused-referencing.
     * An *unused-referencing* slot is defined as follows:
     *   - It refers to an object that has been constructed but not disposed; a/k/a it is live.
     *     It is allocated in #m_pool.
     *   - The live object to which it refers no longer has any referents cross-process; that is
     *     no process to have constructed and/or borrowed the object has a live `shared_ptr` group referencing it.
     *     Therefore it can (and should) be disposed at the earliest opportunity (in our system: by the thread
     *     that constructed it; unless that thread has exited; then [details omitted here]).
     *
     * ### Rationale ###
     * Generally (though not exclusively) objects are disposed opportunistically (as of this writing by
     * Thread_lcl_obj_db_admin::this_thread_piggy_scan()).  When scanning for such objects to dispose, the algorithm
     * can stop upon encountering #m_n_unused such objects.  (Yes, `m_n_unused` can be concurrently incremented during
     * this procedure; that is fine: we'll get it next time then, when that same algorithm runs opportunistically.)
     */
    std::atomic<unsigned int> m_n_unused{0};

    /**
     * Essential for the perf of related algorithms, this stores (a limited number of) indices to unused-referencing
     * slots (by the definition of *unused-referencing* listed in #m_n_unused doc header).  This, too,
     * is used by the opportunistic-disposal algorithm (see #m_n_unused doc header for brief description):
     *   - While naively that algorithm would simply check all objects, perhaps in some likelier-to-be-effective
     *     order such as LRU-to-MRU, and stop on finding `m_n_unused` objects to dispose...
     *   - ...with #m_unused_idx_hints that algorithm can start directly with slots known to be unused-referencing.
     *     This array is as follows: in no particular order:
     *     - If an element equals 0, it is to be ignored.
     *     - If an element is 1 or higher, then the `use_ct_idx` equal to the stored value, minus 1, is likely
     *       to be unused-referencing.  The algorithm must confirm this and dispose if confirmed; and place 0 here
     *       (whether confirmed ot not).  (The check for non-zero and replacement by zero shall be done with
     *       an atomic-exchange operation.)  Hence it is a (highly likely to be accurate) hint.
     *
     * This is essentially a very high-perf communication pipe from Lend_tracker_pool (client-type) instances:
     * if I am a client-type (used open-mode/client ctor) Lend_tracker_pool and have decremented a slot's value to
     * reach 0, then I shall *communicate* the index of that slot to the admin-type Lend_tracker_pool, so that
     * the admin thread can opportunistically check the slot having *consumed* this index.
     *
     * The effectiveness of this is really only limited by the size of this array `m_unused_idx_hints`.  If it is
     * "full" (no element currently equals 0), then we cannot add a hint to it; the scan algorithm will have to
     * handle the hints that are there and then find the un-hinted unused-referencing slot(s) with a more naive
     * scan (but still bounded by #m_n_unused).
     *
     * ### Tuning/performance notes ###
     * The larger it is, the more hints can be stored.  However there is a trade-off: it takes longer to scan
     * the hints array itself -- it can be (and typically is, even) empty (storing only zeroes), yet all of it
     * makes sense to be scanned.  (That can be bounded by #m_n_unused actually; but even with that, it requires
     * scanning when inserting as well; and anyway until `m_n_unused` is exhausted, one may have had to scan
     * many empty/zero elements first.)
     *
     * So #S_N_UNUSED_IDX_HINTS should typically be chosen to make #m_unused_idx_hints fast-scannable with a
     * `find_if()` type search; perhaps 16 or 32.  Side note: for random-access-iterable containers, gcc STL
     * `find_if()` (and derived) algorithm impl has manual unrolling that'll check several elements per loop
     * iteration -- which is to say there is good reason to expect such a search to be quite quick, if
     * #S_N_UNUSED_IDX_HINTS is kept low, particularly.
     */
    boost::array<std::atomic<use_ct_idx_t>, S_N_UNUSED_IDX_HINTS> m_unused_idx_hints;
  }; // struct Metadata

  // Friends.

  // For access to our internals.
  friend std::ostream& operator<<(std::ostream&, const Lend_tracker_pool&);

  // Methods.

  /**
   * Helper that, given the pre-condition that use-count slot `*use_ct_idx` has been decremented to 0 but is
   * currently referring to a live object, makes it no longer refer to a live object.  The latter means: use_count_new()
   * in a relevant admin Lend_tracker_pool -- possibly but not necessarily `*this` -- shall be able to
   * return `*use_ct_idx`.
   *
   * For discipline, `*use_ct_idx` shall be set to 0 (not-an-index), indicating that the value at entry to this
   * function is no longer usable.
   *
   * @param was_not_counted_as_unused
   *        If `false`, Metadata::m_n_unused (in #m_metadata pointee) had been incremented to count the 0-storing
   *        slot `*use_ct_idx`.  (Therefore we shall decrement it.)  If `true`, it had not been counted (therefore
   *        we won't decrement it).  Rationale: if we are admin then for efficiency we may be able to pass `true` here.
   * @param use_ct_idx
   *        See above.
   */
  void use_count_return_bc_unused(use_ct_idx_t* use_ct_idx, bool was_not_counted_as_unused);

  /**
   * The reverse of use_ct_idx_to_ptr(), this converts a process-local vaddr pointing at a use-count slot
   * to a process-independent index referring to that slot (suitable for transmission via IPC, storage in SHM, etc.).
   *
   * @param use_ct_ptr
   *        Pointer to a use-count slot in #m_pool.
   * @return See above.
   */
  use_ct_idx_t use_ct_ptr_to_idx(const Atomic_use_ct* use_ct_ptr) const;

  /**
   * The reverse of use_ct_ptr_to_idx().
   *
   * @param use_ct_idx
   *        See use_ct_ptr_to_idx() (namely what it returns).
   * @return See above.
   */
  Atomic_use_ct* use_ct_idx_to_ptr(use_ct_idx_t use_ct_idx) const;

  /**
   * Forwards to `m_pool->allocate(sz)` while recording -- accurately but performantly -- into `*m_stats`
   * as needed.  Admin-mode only.  Can throw (if ran out of use-count slots).
   *
   * @warning Do not use `m_pool->allocate()` directly.
   *
   * @param sz
   *        As for `m_pool->allocate()`.
   * @return As from `m_pool->allocate().
   */
  void* pool_allocate(size_t sz);

  /**
   * Forwards to `m_log_ctx->log_while_locked()`.  We do not sub-class `Log_context_mt`, and doing this
   * results in quicker, or at least more direct, code than via FLOW_LOG_SET_LOCKED_CONTEXT().
   *
   * This allows the use of `FLOW_LOG_..._LOCKED()` (or `log_while_locked()` can be used directly).
   *
   * @param logging_task
   *        See `flow::log::Log_context_mt::log_while_locked()`.
   */
  template<typename Logging_task>
  void log_while_locked(Logging_task&& logging_task) const;

  /**
   * Reads #m_skip_fast_path_verbose_logging_ptr pointee's current value.
   * @return See above.
   */
  bool skip_fast_path_verbose_logging() const;

  // Data.

  /// See update_log_context().
  const flow::log::Log_context_mt* m_log_ctx;

  /// See update_log_context().
  const std::atomic<bool>* m_skip_fast_path_verbose_logging_ptr;

  /**
   * Un-nullified permanently in ctor, this is the handle to the high-perf use-count-tracking SHM-pool pertaining
   * to objects constructed from this thread (if create-mode/admin ctor invoked) or another thread in this or
   * other process (if open-mode/client ctor invoked).
   */
  std::optional<Pool> m_pool;

  /**
   * Pointer to the Metadata in #m_pool; also used as the base for #use_ct_idx indices produced or consumed
   * by `*this` APIs.
   *   - In its dereferenced `Metadata` capacity:
   *     - If we are in admin mode (create-mode/admin ctor invoked): We initially populate this in ctor right
   *       after creating the SHM-pool #m_pool.
   *     - Otherwise (client mode: open-mode/client ctor invoked): It is ready-to-go by the time we open
   *       the SHM-pool #m_pool in ctor.
   *   - As an offset base: Since Use_count_registry::get_metadata() is formally guaranteed to point to the
   *     start of the data area, with actual use-count slots stored in bulk following the #Metadata:
   *     - use_ct_ptr_to_idx(), given a pointer to a use-count slot, subtracts `m_metadata` (the pointer)
   *       to get the offset in bytes; then multiplies by the slot width to obtain the index.
   *       - It is quick: subtract, then shift left.
   *     - use_ct_idx_to_ptr(), conversely, will first divide by the slot width, then add `m_metadata` (the pointer).
   *       That gets the actual pointer to the slot.
   *       - It is quick: shift right, then add.
   *     - Hence, say `Metadata` (with padding determined by Use_count_registry) uses 256 bytes; and
   *       `sizeof(Atomic_use_ct) == 4` (the slot width).  Then the lowest possible index returned by
   *       use_ct_ptr_to_idx() is (256 / 4) = 64.  Then the next ones are 65 (at 65 * 4 bytes off base), 66, etc.
   *       It is not necessary to really be aware of that typically; use_ct_ptr_to_idx() and use_ct_idx_to_ptr()
   *       will work symmetrically, and that's that; but when reading logs it helps to be able to interpret the
   *       indices and addresses.
   */
  Metadata* m_metadata;

  /// Stats to update in admin-mode; null => skips stat-keeping.
  Obj_db_aux_pool_stats* m_stats;
}; // class Lend_tracker_pool

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Container>
void Lend_tracker_pool::emit_unused_ct_idx_hints(Container* container)
{
  constexpr auto N = decltype(Metadata::m_unused_idx_hints)::static_size;

  container->clear(); // With vector<> typically just updates a scalar size member.
  container->reserve(N); // Prevent reallocations on ->push_back().

  auto& hints = m_metadata->m_unused_idx_hints;
  use_ct_idx_t use_ct_idx;
  for (auto& hint : hints)
  {
    use_ct_idx = hint.exchange(0, std::memory_order_relaxed);
    if (use_ct_idx != 0)
    {
      // Reminder: it is saved ++ed, as 0 is a special value.
      container->push_back(--use_ct_idx);

      /* It might be tempting here to do the following (and promise it in our contract):
       * return use_ct_idx into the available pool (use_count_return()), knowing that the caller will presumably
       * clean up the actual object assigned to use_ct_idx, due to our emitting it to them.
       * That is not safe though; it is possible a client thread (while we are currently in admin thread)
       * placed the hint here just after decrementing use_ct_idx-th slot to 0, but between those two events
       * this (admin) thread had noticed (perhaps in an earlier piggy-scan pass) that slot=0 while doing
       * the non-hints-array-enabled search phase; so they might have already returned it to the pool, hence
       * it is not 0 -- it is not an active slot at all.  It might even have been assigned to another object
       * entirely!  To tell the difference, at the least we'd have to be able to check whether a slot is
       * allocate()d or not (as of this writing we can't; anyway Use_count_registry would need expose such a thing,
       * with some computational cost, checking its internal bitmap and all).
       *
       * So that'd be wrong, or at least non-trivial to make right.  So keep it simple and clean:
       * the hint array hints; our caller has the real info on which objects are live and can be returned;
       * it will use the hints to hopefully get to the slot=0 ones, but if they happen to not help, which
       * is the worst case, then that's fine.  Whereas the above mess would not have been fine. */
    }
  } // for (hint in hints)

  if (!skip_fast_path_verbose_logging())
  {
    log_while_locked([&](auto&& get_logger, auto&& get_log_component)
    {
      const auto logger_ptr = get_logger();
      if (logger_ptr && logger_ptr->should_log(flow::log::Sev::S_TRACE, get_log_component()))
      {
        for (const auto use_ct_idx : *container)
        {
          FLOW_LOG_TRACE_WITHOUT_CHECKING
            ("Lend-tracker pool [" << *this << "]: Emitted/reset unused-object-not-yet-deleted hint: "
             "use-count is/was 0 at idx [" << use_ct_idx << "], "
             "process-local addr [" << use_ct_idx_to_ptr(use_ct_idx) << "] off base addr "
             "[" << m_metadata << "].");
          /* This would be nice to log too, but we chose to optimize by not logging inside the search loop.
           *   "The hint is (was) in hint-array slot "
           *   "[" << (&hint - hints.begin()) << "] (0-based) of [" << N << "] (1-based).");
           * @todo Maybe keep an array of those if logging enabled? */
        }
        FLOW_LOG_TRACE_WITHOUT_CHECKING
          ("Lend-tracker pool [" << *this << "]: Emitted/reset [" << container->size() << "] "
           "unused-object-not-yet-deleted hints.");
      }
    });
  } // if (!skip_fast_path_verbose_logging())
  // else { Skip ~any cycle use on logging along fast-path. }
} // Lend_tracker_pool::emit_unused_ct_idx_hints()

template<typename Logging_task>
void Lend_tracker_pool::log_while_locked(Logging_task&& logging_task) const
{
  m_log_ctx->log_while_locked(logging_task);
}

} // namespace ipc::shm::arena_lend::detail
