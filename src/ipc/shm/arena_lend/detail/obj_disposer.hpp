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
#include <flow/util/util.hpp>
#include <memory>

namespace ipc::shm::arena_lend::detail
{

// Types.

/**
 * Internal-use `shared_ptr` disposer object, attached to every cross-process-lendable object handle
 * returned by `A.construct<T>()`, where `A` is a lend-capable arena object.
 *
 * @note In the case of SHM-jemalloc, `A` -- #Shm_arena -- is jemalloc::Ipc_arena.
 *
 * As a disposer, it executes synchronously when the owner-side handle (`shared_ptr<T>` group) reaches ref-count 0,
 * in whichever end-user thread where this happens to occur.  A SHM-lendable handle points to an object
 * that may be held by multiple entities (usually processes, though it is also possible to lend to one's own
 * process, typically for testing/debugging), and this ref-count pertains only to the original owner-arena-created
 * handle.  Therefore reaching ref-count 0 does not necessarily mean the `T` shall be deallocated then; the same
 * must occur for every borrowed handle's ref-count also.  Therefore the disposer (`operator()()`) shall decrement
 * the conceptual entity/process ref-count; and if *that* also reached zero, then the `T` is indeed destroyeda
 * as soon as possible.
 *
 * @note The destruction, if it occurs, may itself be synchronous -- or not.  As of this writing it will indeed
 *       be synchronous if and only if the thread in which the disposer happens to run is the same thread that
 *       `construct()`ed it.  This is the result of the Thread_lcl_obj_db_admin design.  See its doc header for
 *       details on how that all works.  This note is informational, in the sense that it gets into white-box
 *       behaviors to help you form a bigger picture.  In the contractual sense we merely promise that on reaching
 *       0 cross-process handles holding the `T`, in this disposer, that disposer shall trigger its destruction
 *       (at some point).
 *
 * A secondary role (hence the `_and_mdt`) of this object is to provide, via `shared_ptr::get_deleter()`, a
 * few key immutable scalars required to implement Shm_session::lend_object().  That method's impl grabs such
 * things as #m_lend_tracker_pool_id and #m_use_ct_idx and shoves them into the small handle-serialization returned
 * by `lend_object()`.  The user then IPC-transmits the handle-serialization to a borrowing process; and
 * in that process Shm_session::borrow_object() obtains those values with which it can create a borrower-side
 * counterpart to the owner-side-`construct()`-returned `shared_ptr<T>` handle.  The nearby free function
 * construct_with_borrower_obj_disposer() does this; note that it takes values largerly matching the ones
 * stored in a `*this` (including the aforementioned #m_lend_tracker_pool_id and #m_use_ct_idx).
 *
 * ### Subtleties about #Shm_arena lifetime versus handle lifetime versus object lifetime ###
 * Last but not least `*this` stores a ref-counting pointer to the `.construct<T>()`ing #Shm_arena.  As a result
 * that #Shm_arena (jemalloc::Ipc_arena being a prime example) will outlive the owning *handle* (the constructed
 * `shared_ptr` group).  The minimal reason for this is simply this: I can `p = arena->construct<T>(...)`
 * and then simply not (yet?) `session->lend_object(p)`; in fact here might not even *be* a (relevant) `session`.
 * Well, `*arena` is required to "physically" deallocate `*p`.  So by SHM-jemalloc's design, the system must
 * keep `*arena` around.
 *
 * Key subtlety: That, in itself, does *not* keep `*arena` alive past the lifetime of the object itself; the object
 * may be lent via a session, and thus borrowed by another process; if the disposer's linked local handle reaches
 * ref-count zero, that borrower may still hold a handle too; only once that handle and all like it also go away
 * can `*arena` deallocate the object.  So `*arena` must outlive that lifecycle too: we just aren't what
 * guarantees that.  What guarantees that, as of this writing is this: `*session` stores a `shared_ptr` ref to
 * `*arena` (much like a `*this` does), at the time one calls `session->lend_arena(arena)`.  So while the session
 * to process X is around, every arena lent through it also stays around.  Hence:
 *   - Owner_obj_disposer_and_mdt holds reference to constructing #Shm_arena...
 *     - ...to protect against the latter's disappearance before `*this` linked handle is destroyed (in the event
 *       handle-pointee is not handle-referenced in 1+ borrowing processes).
 *     - Once `*this` linked handle goes away, the #Shm_arena no longer will be needed to destroy the actual object
 *       in that no-borrowers scenario.
 *   - `Shm_session` holds reference to lent #Shm_arena...
 *      - ...to protect against the latter's disappearance while the `Shm_session` peer process is connected to us
 *        and therefore tracking current (and future) objects constructed by #Shm_arena.
 *      - Once that `Shm_session` goes away, it indicates we're no longer dealing with the formerly-connected
 *        entity (process), so the #Shm_arena no longer will be needed to destroy currently-borrowed object(s).
 */
template<typename Shm_arena_t>
struct Owner_obj_disposer_and_mdt
{
  // Types.

  /// Alias for template parameter.
  using Shm_arena = Shm_arena_t;

  /// Short-hand for ref-counted handle to #Shm_arena.
  using Shm_arena_ptr = std::shared_ptr<Shm_arena>;

  // Constructors/destructor.

  /**
   * Constructs disposer, memorizing the args -- including saving a reference to the #Shm_arena so as to keep it
   * alive while the linked `shared_ptr` group is alive.
   *
   * This *must* be called in the same thread as the `Shm_arena::construct()` call that is presumably constructing
   * `*this` as part of that procedure.  (Among the memorized items is that thread's thread-token -- see
   * #m_cting_thread_token -- which is in sync with this requirement.)
   *
   * @param shm_arena
   *        See #m_shm_arena.  Note the arg is nullified.
   * @param lend_tracker_pool_id
   *        See #m_lend_tracker_pool_id.
   * @param use_ct_idx
   *        See #m_use_ct_idx.
   */
  explicit Owner_obj_disposer_and_mdt(Shm_arena_ptr&& shm_arena, pool_id_t lend_tracker_pool_id,
                                      use_ct_idx_t use_ct_idx);

  // Methods.

  /**
   * The disposer function that runs at most once: when the linked `shared_ptr` group's ref-count reaches zero.
   *
   * It informs the appropriate module (spoiler alert: `Thread_lcl_obj_db_{admin|client}` depending on the current
   * thread at the time of ref-count-reaching-zero-and-thus-calling-us) that this handle's disappearing.
   * See class doc header for details about what effect this has w/r/t actually disposing of the linked handle's
   * target object in the future.
   *
   * @param addr
   *        Address of in-SHM object.
   */
  template<typename T>
  void operator()(T* addr);

  // Data.  Note: it is public and immutable.

  /**
   * Ref-cted handle to the #Shm_arena that `->construct<T>()`ed, creating us as part of that.  See class doc header.
   *
   * While, as noted there, we store the actual arena-object handle (to keep it alive, while we are alive), it
   * also notably provides access to `m_shm_arena->get_id()` (the arena ID, unique within this owner process).
   * It is needed for lending to an opposing process, like #m_use_ct_idx for example.
   *
   * So one can/should picture a `collection_id_t m_collection_id` member here.
   */
  const Shm_arena_ptr m_shm_arena;

  /// Aux, use-count-tracking SHM-pool's ID, as output by Thread_lcl_obj_db_admin::constructing_obj().
  const pool_id_t m_lend_tracker_pool_id;

  /// The use-count slot within pool identifier by #m_lend_tracker_pool_id, as output by the same obj-DB-admin method.
  const use_ct_idx_t m_use_ct_idx;

  /**
   * The thread-token (`flow::util::this_thread_unique_token()`) of the thread that `Shm_arena::construct()`ed the
   * linked object; captured by the ctor, per its same-thread requirement.  operator()() compares the disposing
   * thread's token against this to decide which module to inform (same thread => *admin*; any other => *client*);
   * Shm_session::lend_object() similarly, via direct member access.
   */
  const flow::util::Thread_token m_cting_thread_token;
}; //class Owner_obj_disposer_and_mdt

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Shm_arena_t>
Owner_obj_disposer_and_mdt<Shm_arena_t>::Owner_obj_disposer_and_mdt(Shm_arena_ptr&& shm_arena,
                                                                    pool_id_t lend_tracker_pool_id,
                                                                    use_ct_idx_t use_ct_idx) :
  m_shm_arena(std::move(shm_arena)),
  m_lend_tracker_pool_id(lend_tracker_pool_id),
  m_use_ct_idx(use_ct_idx),
  m_cting_thread_token(flow::util::this_thread_unique_token())
{
  // OK.
}

template<typename Shm_arena_t>
template<typename T>
void Owner_obj_disposer_and_mdt<Shm_arena_t>::operator()(T*)
{
  Thread_lcl_obj_db_admin<Shm_arena>::this_thread_piggy_scan(); // Opportunistic!

  if (flow::util::this_thread_unique_token() == m_cting_thread_token)
  {
    /* shared_ptr group reached ref-count 0 in the same thread that created the shared_ptr: we can report it
     * to the thread-local obj-DB *admin* directly.  (Not our problem but for context: it'll surely --use_ct
     * at index m_use_ct_idx for m_shm_arena... but then if that caused it to reach 0, it can finish the job
     * -- that is delete the object -- right then and there.  It's a very nice optimization; though to be fair
     * arguably most of the benefit is if `--use_ct = 0` which tends to happen on the borrower side, not the
     * owner/lender side.)
     *
     * This is pretty common (but not universal; certainly a different thread might be the one to reach ref-count 0). */
    Thread_lcl_obj_db_admin<Shm_arena>::this_thread_obj_db()->disposing_obj(m_shm_arena->get_id(), m_use_ct_idx);
  }
  else
  {
    /* shared_ptr group reached ref-count 0 in a diff thread versus one that created the shared_ptr: we can only
     * report it to the thread-local obj-DB *client* directly.  (Not our problem but for context: it'll surely --use_ct
     * at index m_use_ct_idx for m_shm_arena... and that's about it.  The *admin* in the cting thread will have
     * to detect use_ct=0 (if indeed it is) opportunistically when it can.  Borrower_obj_disposer_and_mdt for example
     * has to do this branch always; the direct-to-admin optimization is impossible by definition in the borrowing
     * process.) */
    Thread_lcl_obj_db_client<Shm_arena>::this_thread_obj_db()->disposing_obj(*m_shm_arena, m_lend_tracker_pool_id,
                                                                             m_use_ct_idx);
  }
} // Owner_obj_disposer_and_mdt::operator()()

template<typename T, typename Shm_session>
Obj_handle<T> construct_with_borrower_obj_disposer(T* addr,
                                                   pool_id_t lend_tracker_pool_id,
                                                   use_ct_idx_t use_ct_idx,
                                                   owner_id_t owner_id,
                                                   collection_id_t collection_id,
                                                   std::shared_ptr<const Shm_session>&& shm_session_to_keep_alive)
{
  // Reading doc header for background is probably helpful here.

  return Obj_handle<T>(addr, [lend_tracker_pool_id, use_ct_idx, owner_id, collection_id,
                              shm_session_to_keep_alive = std::move(shm_session_to_keep_alive)]
                               (auto&&...) mutable
  {
    Thread_lcl_obj_db_admin<typename Shm_session::Arena>::this_thread_piggy_scan(); // Opportunistic!

    Thread_lcl_obj_db_client<typename Shm_session::Arena>
      ::this_thread_obj_db()->disposing_obj(lend_tracker_pool_id, use_ct_idx, // (1)
                                            owner_id, collection_id); // (2)
    /* (1) Needed in fast-path and slow-path: Lookup that pool, decrement that offset in that pool.  Done!
     * (2) Needed in slow-path only: Need to open pool first: computing its name requires lookup of
     * opposing-process pool-collection (arena) by those bits of data. */

    /* See rationale in our doc header for why we capture it.
     * The following statement is unnecessary (this would happen anyway shortly, as the shared_ptr<> group
     * goes away on ref-count 0), but just to make it nice and deterministic/debugger-friendly: */
    shm_session_to_keep_alive.reset();
  });
}

} // namespace ipc::shm::arena_lend::detail
