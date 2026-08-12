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
#include "ipc/shm/arena_lend/detail/lend_tracker_pool.hpp"
#include "ipc/shm/arena_lend/detail/owner_spc_impl.hpp"
#include "ipc/shm/arena_lend/detail/shm_pool_offset_ptr_data.hpp"
#include "ipc/shm/arena_lend/arena_lend_stats.hpp"
#include "ipc/shm/arena_lend/detail/arena_lend_fwd.hpp"
#include "ipc/shm/arena_lend/detail/stats.hpp"
#include "ipc/shm/shm_stats.hpp"
#include "ipc/session/standalone/shm/arena_lend/borrower_shm_pool_collection_repository.hpp"
#include "ipc/util/process_credentials.hpp"
#include "ipc/common.hpp"
#include <flow/util/linked_hash_map.hpp>
#include <flow/util/thread_lcl.hpp>
#include <flow/util/util.hpp>
#include <flow/util/stat/stat_set.hpp>
#include <flow/util/stat/stat_set_list.hpp>
#include <flow/log/log.hpp>
#include <flow/log/config.hpp>
#include <flow/async/async_fwd.hpp>
#include <boost/thread/future.hpp>
#include <boost/thread/thread_only.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/range/join.hpp>
#include <boost/range/adaptor/filtered.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/algorithm/for_each.hpp>
#include <type_traits>
#include <vector>
#include <cstdlib>
#include <optional>
#include <atomic>

namespace ipc::shm::arena_lend::detail
{

// Types.

/**
 * (Internal-use) `Thread_lcl_obj_db_admin<A>::this_thread_obj_db()` is the current thread's module that tracks
 * extant `a.construct<>()`ed (in this thread) objects in SHM, where `a` represents all SHM-arenas of type `A`.
 *
 * ### What to use ###
 * In particular, in the case of SHM-jemalloc (the first example of an arena-lending SHM-provider):
 *   - `A` (#Shm_arena) is jemalloc::Ipc_arena.
 *   - `Thread_lcl_obj_db_admin<A>` represents all SHM-jemalloc arenas' stuff.
 *     - This would be fully independent of `Thread_lcl_obj_db_admin<B>`, where B is some other arena-type that isn't
 *       SHM-jemalloc's jemalloc::Ipc_arena.
 *   - In current thread U, `Thread_lcl_obj_db_admin<A>::this_thread_obj_db()` is the compendium of
 *     objects constructed via *all* `a.construct<T>(...)` calls (hence each object is a *first-class* or *high-level*
 *     or *outer* object, unlike, say, additional buffers allocated by the T ctor via STL-allocator-magic; these
 *     are *not* tracked by `Thread_lcl_obj_db_*`).  Here `a` are *all* instances of `A` (of jemalloc::Ipc_arena)
 *     to have had `.construct<T>(...)` called on them from thread U.
 *     - So if in thread X no jemalloc::Ipc_arena has SHM-constructed a first-class object, then there has been
 *       no need to call this_thread_obj_db() from thread X, and no this_thread_obj_db() exists for that thread.
 *       The user hasn't, essentially, done SHM-jemalloc work from that thread.
 *   - While `auto x = this_thread_obj_db()` (for a given thread U) tracks *all* (type `A`) arenas to have
 *     SHM-constructed things, within `x` (conceptually speaking) what is relevant to arena `a1` is fully
 *     independent of what is relevant to arena `a2`.  In terms of one's mental model, it's best to take a particular
 *     arena type (jemalloc::Ipc_arena in this case) and particular arena of that type as a given... then contemplate
 *     various (non-`static`) APIs.
 *
 * A particular `Shm_arena` is specified to our non-`static` APIs as either a `Shm_arena*` or a #collection_id_t.
 * Reminder: a collection ID, as pertains to SHM-arena owned by *this* process, uniquely identifies a `Shm_arena`.
 * (A given `collection_id_t` value might equal some that of some other arena owned by/allocating in another process;
 * but in the context of Thread_lcl_obj_db_admin -- as it is an *admin*, meaning owner-oriented, module -- we
 * are always talking of the current process.)
 *
 * ### How to use it ###
 * A `*this` supports the key ipc::shm::arena_lend (arena-lend-oriented) operations (a/k/a *events*):
 *   - `p = a.construct<T>(...)`: #Shm_arena `a` allocating `sizeof(T)` bytes in SHM managed by `a`; and executing
 *     ctor `T(...)`.  (If `T` is STL-compliant container, with SHM-aware allocator set, then the ctor may further
 *     allocate in-SHM; e.g. `vector` would allocate a buffer.  However such allocs do not, also, `.construct()`.
 *     Unlike `p` the resulting buffers are *inner*; `p` is *outer*`.) `p` is of this writing a `shared_ptr<T>`.
 *     - Invoke constructing_obj().
 *   - `session.lend_object<T>(p)`: Intent to transmit an encoding of handle `p` to opposing process, where an
 *      identically-behaving handle shall be created via `p2 = session2.borrow_object(encoding_of_p)`.
 *     - Invoke lending_obj() (in sending process, on `session.lend_object()`).
 *     - Do so *only* from the `p`-constructing thread U!  If in another thread you must use Thread_lcl_obj_db_client
 *       instead.
 *   - The disposal of a given handle `p` (as of this writing, when the `shared_ptr` group of `p` reaches
 *     ref-count zero): I.e., in this process, the constructed (in original process) or borrowed (in opposing
 *     process) handle to the object is to be disposed.  Once *all* processes' extant such handles have been disposed,
 *     the owner-process shall (internally to Thread_lcl_obj_db_admin machinery) garbage-collect the datum.
 *     - Invoke disposing_obj().
 *     - Again: do so only from `p`-constructing thread U!  Else use Thread_lcl_obj_db_client.
 *
 * @see Thread_lcl_obj_db_client which handles each event (lend, dispose) such that that event pertains to an object
 *      originally constructed in another thread versus where said event occurred.
 *
 * You'll note mention of "garbage-collect[ing] the datum," once all handles for a particular `*this`-assigned
 * thread's object have been disposed of (disposing_obj() or Thread_lcl_obj_db_client::disposing_obj()).
 * Formally this is the purview of `*this`, but our user's help is required to make this work.  By design we do *not*
 * start background-threads to handle such things; we rely (much like most memory-allocator libs) on
 * *piggy-backing*.  That is:
 *   - you *must* frequently-enough invoke a so-called *piggy-scan*.  In short this will dispose of any
 *     no-handles-remaining `*this`-thread-constructed objects.  (For each guy this also invokes its destructor,
 *     so any *inner* buffers allocated throughout its lifetime would also get deallocated.  Not our purview though.)
 *     - Invoke `static` this_thread_piggy_scan()...
 *     - ...but when is "frequently-enough"?  Formally: not for us to say.  In reality: recommend doing it
 *       opportunistically ahead of any related op such as construct, dispose, lend, borrow (for any object).
 *       Perf-wise: this_thread_piggy_scan() that has nothing to do (which is more typical than not) has
 *       very low cost.  One that does have something to do (objects to dispose) costs ~as much as the actual
 *       disposing does (reasonable low overhead).
 *
 * A higher-scope event is
 *   - the decision to shut down a particular #Shm_arena `a`.  That is, none of the objects constructed from `a`
 *     shall be touched by this process or, to the extent we can assume it, other process(es).  Hence (1)
 *     we should dispose of any `a`-constructed objects (having stopped the regular-operation garbage-collection
 *     ops described above) that might remain (though ideally none should; but we cannot assume it; our user
 *     such as jemalloc::Ipc_arena can make a best effort at it); and (2) clean up any internal book-keeping
 *     relevant to `a`.
 *     - Invoke `static` forgetting_shm_arena() from any thread.
 *     - It is potentially an async op; hence caller is to give it a function to execute on completion.
 *       In particular that function might dispose of the lower-level arena `a`-related resources; this would not
 *       be safe to do until forgetting_shm_arena() disposes any remaining constructed-by-`a` objects.
 *       (In the case of SHM-jemalloc that would include at least destroying, via the raw jemalloc API, the relevant
 *       jemalloc-arena(s) associated with jemalloc::Ipc_arena `a`.)
 *
 * ### Impl: Across multiple `*this`es ###
 * Firstly, per above, a `*this` <=> a particular thread that has performed at least one
 * `construct()`.  We use a `static flow::util::Thread_local_state_registry` (Static_state::m_obj_db_registry)
 * to maintain such thread-local `*this`es with the ability to enumerate all extant ones.  As ever, enumeration
 * of thread-local items is unnatural and annoying; in our case we need it, as of this writing, for:
 *   - forgetting_shm_arena() (for: ping each `*this` to free objects et al w/r/t the specific shutting-down arena);
 *   - sharded_stats() et al (for: walk across each `*this`, summing its stat-shards into the current resulting
 *     stat-`struct`);
 *   - dbs_set_logger() (for: setting the `Logger*` of each `*this`).
 *
 * In general things are fairly elegant, except for the forgetting_shm_arena() flow, at which point we must contend
 * with the aforementioned thread-local-item enumeration/looping and related yuckiness.  Particularly yucky
 * is the corner case wherein, at forgetting_shm_arena() time from thread X, threads which aren't X have extant
 * `Thread_lcl_obj_db_admin`s with extant objects belonging to the #Shm_arena that's going away.
 *
 * Yuckier still is the case where a thread X is joined by the end user's application, but that thread has an
 * extant Thread_lcl_obj_db_admin (it has `.construct()`ed objects) with extant objects (they have not all been
 * disposed).  This creates the basic issue: only thread X, in our design, can dispose of items constructed by
 * thread X; but there is no thread X; but the rest of the processes -- and/or other (borrowing) process(es) --
 * still hold handle(s) to 1+ objects.  Well, it is okay for another thread to do it, as long as it doesn't
 * intermix with other threads' activities in some bad way.  Our answer to all this is, just before thread X is joined,
 * create a *degraded* thread X-prime, whose purpose is to be <=> with death-row-inhabiting thread X; and to
 * perform only the disposal of X's objects.  New ones by definition cannot appear; and once the extant ones are all
 * gone, X-prime can also end.  Only problem is, when to do this disposal?  There's no piggy-backing (it's our thread,
 * unlike X, which is/was an end-user thread).  Answer: we do it periodically.  See degraded_admin_thread_body().
 *
 * Another tricky aspect concerns stat-collection (referenced above already).  See Sharded_stats doc header for
 * an intro to that design.  In short: we use TL-sharding, wherein ongoing ops only make stat-updates to
 * thread-local stat::Sharded_stat `struct`s; at stat-consume time (et al) the extant such `struct`s are basically
 * summed member-by-member.
 *
 * ### Impl: For a given `*this` ###
 * Back to the relatively-elegant design of a particular `*this`.  The following gives the flavor of what to
 * expect.  The code itself should be reasonably easy to follow, but the following might help as an intro.
 *
 * The main building block is Lend_tracker_pool, which in our case is used in admin-mode;
 * see its doc header.  Lend_tracker_pool accesses (and in our -- admin -- case, creates, namely the first time
 * a given thread `.construct()`s something in a given `Shm_arena`) a SHM-pool-stored high-perf resource with
 * use-counts pertaining to all of a single `Shm_arena`'s objects (constructed by `*this`-assigned thread).
 * Hence we store one of these per distinct #Shm_arena.  constructing_obj() creates a use-count therein and
 * sets it to 1; lending_obj() increments it; disposing_obj() decrements it and potentially (if that makes
 * use-count reach zero) returns it for use by another future use-count.
 *
 * The other main thing stored = each live object's info including its address, disposing functor (invoked once
 * a live object's use-count reaches zero), and #use_ct_idx_t (identifies a use-count slot within the relevant
 * arena's Lend_tracker_pool).  The key thing there is that Lend_tracker_pool is only aware of use-counts, not
 * anything about what object each use-count pertains to; all of that is higher-level and hence up to us.
 *
 * ### Impl: cleanup ###
 * We turn your attention to the place where a Lend_tracker_pool object is destroyed; actually two places:
 * inactive_arenas_scan() (from dtor code-path) and forget_shm_arena() (from forgetting_shm_arena() code path).
 * We won't recap what's said there; but it is of some importance.
 *
 * @tparam Shm_arena_t
 *         The arena type which an end user instantiates to then `.construct<T>()` things that we
 *         handle via `.constructing_obj()` and subsequent ops.  As of this writing it's going to be a sub-class
 *         of arena_lend::Owner_shm_pool_collection (such as jemalloc::Ipc_arena in the case of SHM-jemalloc).
 *         `Thread_lcl_obj_db_admin<A1>` and `Thread_lcl_obj_db_admin<A2>` are fully independent singleton
 *         registries if `A1 != A2`.
 */
template<typename Shm_arena_t>
class Thread_lcl_obj_db_admin :
  public flow::log::Log_context_mt,
  private boost::noncopyable
{
public:
  // Types.

  /// Short-hand for template parameter type.  See our class doc header for requirements.
  using Shm_arena = Shm_arena_t;

  // Constructors/destructor.

  /**
   * Constructs thread-local `*this` as invoked internally by Static_state::m_obj_db_registry (on first
   * this_thread_obj_db() call -- in fact `this_thread_obj_db()from a given thread).
   *
   * @param logger_ptr
   *        Logger to use for logging subsequently; taken from Static_state::m_obj_db_registry.
   */
  explicit Thread_lcl_obj_db_admin(flow::log::Logger* logger_ptr);

  /**
   * Destroys this per-thread admin.  This runs when the owning thread exits (thread-local destruction).
   * It performs an exhaustive unused-object scan and arena-forgetting pass.  If live objects remain
   * (their use-counts have not yet reached zero), a *degraded-admin thread* a/k/a *drain-thread* is spawned to
   * periodically continue scanning and deleting them until none remain; the exiting thread's object data is moved
   * into that replacement thread's `*this`.  See degraded_admin_thread_body().
   *
   * The degraded-admin thread is semi-detached: it is recorded so that an `std::atexit()` handler
   * can join it at program exit, preventing forcible OS termination of mid-cleanup work.
   */
  ~Thread_lcl_obj_db_admin();

  // Methods.

  /**
   * Returns the calling thread's Thread_lcl_obj_db_admin, creating it on first call from that thread.
   * The returned pointer is valid for the lifetime of the calling thread (it is destroyed during
   * thread-local cleanup on thread exit).
   *
   * @return Non-null pointer to this thread's admin object-DB.
   */
  static Thread_lcl_obj_db_admin* this_thread_obj_db();

  /**
   * If the calling thread has a Thread_lcl_obj_db_admin (i.e., this_thread_obj_db() has been called
   * at least once from this thread), performs an opportunistic scan: first reclaiming any
   * constructed objects whose use-counts have reached zero (*piggy-scan*), then handling any
   * pending arena-forgetting requests.  If no admin exists for this thread, this is a no-op.
   *
   * This is the entry point for the piggy-backing pattern described in the class doc header:
   * callers should invoke it frequently (e.g., ahead of construct/lend/dispose/borrow operations)
   * so that garbage-collection occurs without the use of background threads.  The perf overhead beyond
   * any actually-necessary disposal ops is minimal.
   */
  static void this_thread_piggy_scan();
  /* XXX Add dedicated TLODB (admin+client) unit test: piggy-scan trigger matrix, adversarially (trigger via
   * unrelated arena; via each documented trigger op; via Ipc_arena::this_thread_gc(); assert quiet thread and
   * wrong thread do NOT reap); plus the backstops (thread-exit reap; degraded-mode per-exited-thread thread;
   * end-of-program atexit() path). */

  /**
   * Propagates the given logger to the extant (and future) per-thread Thread_lcl_obj_db_admin objects
   * (via their set_logger()).
   *
   * @see It participates in arena_lend::set_logger(), so you may not need to call it directly.
   *      In fact it's probably best not to, as we are an internal-use module.
   *
   * @param logger_ptr
   *        New logger; may be null.
   */
  static void dbs_set_logger(flow::log::Logger* logger_ptr);

  /**
   * Sets (replaces) the logger used by `*this`.  In addition to the base-class behavior, this:
   * snapshots whether TRACE-level logging is enabled; if not, fast-path logging is entirely
   * skipped until the next set_logger() call (trading responsiveness for perf).
   *
   * @param logger_ptr
   *        New logger; may be null.
   * @return Previous logger.
   */
  flow::log::Logger* set_logger(flow::log::Logger* logger_ptr);

  /**
   * Records a newly `construct<T>(...)`ed object.  Must be called from the constructing thread (the thread
   * that owns `*this`): `this_thread_obj_db()->constructing_obj()`.
   *
   * The outputs `lend_tracker_pool_id` and `use_ct_idx` are not meant for the caller to directly use
   * according to what they actually mean; rather they are needed to be passed-to future operations:
   * lending_obj(), disposing_obj() and their eponymous Thread_lcl_obj_db_client counterparts.  In fact,
   * they need to travel to any borrowing process via IPC (that process's Thread_lcl_obj_db_client::disposing_obj()
   * would take them).
   *
   * @tparam Deleter_func_obj
   *         Functor type compatible with `void (void* addr, Shm_arena*)`: called to destroy/deallocate
   *         the object when its use-count reaches zero.
   * @param lend_tracker_pool_id
   *        Out-arg: to be passed-to future lending_obj(), disposing_obj() and Thread_lcl_obj_db_client counterparts.
   * @param use_ct_idx
   *        Similar to `lend_tracker_pool_id`.
   * @param shm_arena
   *        The arena in which the object was constructed.
   * @param obj_db_aux_pool_stats
   *        The stat::Obj_db_aux_pool_stats object in `*shm_arena`.  Updated a low number of times over `*this` lifetime
   *        per the concurrent, non-sharded design from `flow::util::stat` doc header.  This pointer is internally
   *        recorded in the first constructing_obj() call per `*this`; ignored after that.  Informally speaking:
   *        it only makes sense to pass-in the same pointer value per `*this`.
   *        (Mini-rationale: We considered instead adding a privileged accessor to `Shm_arena` requirements instead;
   *        but that is more onerous for the `Shm_arena` coder, and the perf cost of an oft-unused extra method
   *        arg is low.)
   * @param addr
   *        Process-local address of the constructed object.
   * @param del_func
   *        Deleter functor: invoked with `(addr, shm_arena)` to destroy/deallocate the object when
   *        garbage-collected.
   */
  template<typename Deleter_func_obj>
  void constructing_obj(pool_id_t* lend_tracker_pool_id, use_ct_idx_t* use_ct_idx,
                        Shm_arena* shm_arena, arena_lend::stat::Obj_db_aux_pool_stats* obj_db_aux_pool_stats,
                        void* addr, Deleter_func_obj&& del_func);

  /**
   * Records that a constructed object is being lent (IPC-transmitted) to another process.  The object's
   * use-count is incremented to prevent premature garbage-collection before the borrowing process
   * creates its own handle via the borrow-object op.  Must be called from the constructing thread (the
   * thread that owns `*this`); if called from another thread, use Thread_lcl_obj_db_client::lending_obj() instead.
   *
   * @note Reminder: lending from a borrowed handle (in a borrower process) is not, as of this writing, supported.
   *
   * @param collection_id
   *        See constructing_obj() out-arg.
   * @param use_ct_idx
   *        See constructing_obj() out-arg.
   */
  void lending_obj(collection_id_t collection_id, use_ct_idx_t use_ct_idx);

  /**
   * Records disposal of a handle to a constructed object (as of this writing: `shared_ptr` group reaching ref-count
   * zero).  The object's use-count is decremented; if it thereby reaches zero, the object is immediately
   * destroyed/deallocated (via the deleter supplied to constructing_obj()).  Must be called from the constructing
   * thread; if called from another thread, use Thread_lcl_obj_db_client::disposing_obj() instead.
   *
   * @param collection_id
   *        See constructing_obj() out-arg.
   * @param use_ct_idx
   *        See constructing_obj() out-arg.
   */
  void disposing_obj(collection_id_t collection_id, use_ct_idx_t use_ct_idx);

  /**
   * Initiates the process of forgetting (cleaning up) a given #Shm_arena across all per-thread
   * Thread_lcl_obj_db_admin instances.  Any remaining live objects for that arena are destroyed
   * (even if their use-counts are non-zero).
   *
   * This is potentially asynchronous: each per-thread admin must perform its share of the cleanup
   * from its own thread.  Once all per-thread admins have completed this procedure, that is to say
   * the last per-thread admin to have done so realizes that was the case, it invokes
   * `on_done_func` (giving it a safe `Log_context_mt` for logging); meanwhile this method returns `false`
   * (indicating async work remains <=> `on_done_func()` shall be invoked from another thread).  If all work
   * completes synchronously, then `on_done_func` is forgotten and never invoked, and this method returns
   * `true` (indicating no async work remains <=> `on_done_func()` shall never run).
   *
   * ### When can you call this? ###
   * Since object disposal is asynchronous, take care to not bring down any resource required for
   * object disposers (constructing_obj() `del_func` arg) to run, before `on_done_func()` runs, assuming
   * this method returns `false` (if `true`: can proceed immediately).
   * What the heck is "any resource" in this context?  Answer in plainer language: e.g.: SHM-jemalloc
   * (`Shm_arena` = jemalloc::Ipc_arena) requires the jemalloc-arena to exist; so you must ensure
   * you don't invoke jemalloc::Memory_manager::destroy_arena() / jemalloc::Thread_cache::destroy_arena_safely()
   * until entry to `on_done_func()` (if we return `false`).  We mention this, since typically a call
   * like that would be along the same code flow as forgetting_shm_arena().  It makes sense to use
   * `on_done_func()` to perhaps execute those `destroy_arena...()` items.
   *
   * So let's assume that is not an issue.  Are there more times when one must not call forgetting_shm_arena()?
   * Yes:
   *
   * Firstly: do not call it concurrently to a *user* thread exiting such that that thread has ever
   * `constructing_obj()`ed (<=> `Shm_arena::construct<T>()`ed) w/r/t the arena `collection_id`.  This can
   * result in undefined behavior.  In practice *from the end user's PoV* (as of this writing): For SHM-jemalloc:
   * "Calling this" equals calling jemalloc::Ipc_arena::destroy() which equals the last `Ipc_arena` ref-counted-ptr
   * reference being dropped; we document this accordingly in public-facing `Ipc_arena` doc header(s).  (For
   * other #Shm_arena types -- probably same or similar.)
   *
   * It is however fine to call it concurrently with a degraded_admin_thread_body() replacement-`_admin` thread's
   * teardown.  Internal reasoning for this paragraph and the preceding is found inside ~Thread_lcl_obj_db_admin()
   * dtor body.
   *
   * ### Impl discussion for context ###
   * We cannot control when a different thread's admin-object cleans up: They are generally end user threads:
   * we cannot schedule them to do things except opportunistically (this_thread_piggy_scan()).  (There are also
   * degraded-admin threads, but such a thread only enters existence, if a user-thread with extant live objects happens
   * to be joined.  This might happen, or it might not happen.  So the general case is as noted.)
   * If the calling thread has an admin, its portion is done synchronously within this call; other threads'
   * portions are deferred (flagged for execution on their next piggy-scan or degraded-admin-thread wakeup).
   *
   * @tparam On_done_func
   *         Functor type compatible with `void (const flow::log::Log_context_mt*)`.
   * @param collection_id
   *        Identifies the arena to forget.
   * @param logger_ptr
   *        Logger to use for log messages during the synchronous part of the operation.
   * @param on_done_func
   *        Completion handler: if `false` returned, then `on_done_func()` is invoked (from another thread) once
   *        all per-thread admins have forgotten this arena (or more precisely: by the last admin to have done
   *        so, cross-thread).  Never invoked (and not memorized) if `true` returned.  Typically used to continue
   *        tearing down the `Shm_arena` itself (which must remain alive until this fires).  The `Log_context_mt*`
   *        passed to it is obtained at that time from the relevant thread's Thread_lcl_obj_db_admin's
   *        `Log_context_mt`.  If `true` returned then caller shall decide about their logging situation
   *        as appropriate (probably they can just use `*logger_ptr` still).
   * @return See above.  `true` means `on_done_func` was not memorized, and it'll never be executed; caller
   *         can synchronously proceed with further arena shutdown steps.  `false` means `on_done_func()`
   *         shall be called by another thread when possible (caller shall arrange that function to continue
   *         arena shutdown).
   */
  template<typename On_done_func>
  static bool forgetting_shm_arena(collection_id_t collection_id, flow::log::Logger* logger_ptr,
                                   On_done_func&& on_done_func);

  /**
   * Intended for use during stat-consumption or stat-reset, updates certain values (namely some GAUGEs) of the given
   * arena's stats-shard in `*this` to currently-correct values for `*this` shard; the updated stat-members are
   * those that are not live-updated during regular operation but are queryable at any time.  Returns ref to the
   * modified stats-shard; or null if `*this` lacks one (no constructing_obj() yet) and thus nothing was updated
   * after all.  (Recall that the latter situation is entirely normal.)
   *
   * stat::Sharded_stats doc header discusses this small subset of stat-members a bit.
   *
   * Can be invoked from any thread -- not necessarily `*this` thread.  `*this` may or may not be assigned
   * to a degraded-admin thread; it will work regardless.
   *
   * Must not be invoked concurrently to itself on the same `*this`; else behavior undefined.  (As of this writing
   * stat-consumptions and stat-resets are forced to be non-concurrent, so this should be no problem.)
   *
   * @param collection_id
   *        See constructing_obj().  This arena must not be shutting down.
   */
  stat::Sharded_stats* stats_update_pre_consumption(collection_id_t collection_id);

private:
  // Types.

  /// Short-hand for single-ownership pointer (`unique_ptr` of some sort).
  template<typename T>
  using Own = boost::movelib::unique_ptr<T>;

  /**
   * Concrete type for how we store `Deleter_func_obj` functors supplied in constructing_obj().
   *
   * ### Rationale for using `Function` ###
   * To specify via constructing_obj() what to invoke, potentially much later, when all processes have dropped
   * their handles to the `void*`-arg pointee object, we use `Function` a/k/a (as of this writing) `std::function`.
   *
   * Style-wise this is great; direct and simple.  Perf is of high import here, however, so something
   * uglier-but-faster could be conceivably worthwhile.  At least we could use a simple polymorphic-callable
   * with a `virtual run()`.  Does that rise to the level of a formal to-do?  Maybe, maybe not.  The case for "not":
   * As of this writing the actual lambda passed to constructing_obj() has no captures; due to a typical SBO
   * impl of `std::function` this would incur no additional heap use, the `Deleter_func` essentially just storing
   * a function pointer.  That's quite similar to a polymorphic-callable in the first place.  Maybe it's somewhat
   * bigger and with a little internal `std::function` book-keeping overhead.
   *
   * @todo If and only if perf results show a hot-spot to do with storage or execution of
   * Thread_lcl_obj_db_admin::Deleter_func functor, consider using an even lighter-weight definition
   * of `Deleter_func` than the existing choice `Function<>`.  See idea(s) above this to-do.
   */
  using Deleter_func = Function<void (void*, Shm_arena*)>;

  /**
   * Per-arena (per-collection-ID) tracking state owned by this per-thread admin.  One of these exists
   * for each arena from which at least one object has been `construct<>()`ed in this thread (or, if this
   * is a degraded-admin thread, that thread's spawning -- now dead -- thread).
   */
  struct Collection_db
  {
    // Data.

    /// A single constructed object's tracking data: its address and the functor to destroy/dealloc it.
    struct Object
    {
      // Data.

      /// Process-local address of the constructed object (as returned by `construct<T>()`).
      void* m_addr;
      /// Functor invoked as `m_del_func(m_addr, Collection_db::m_shm_arena)` to destroy the object.
      Deleter_func m_del_func;
    };

    /// The arena that owns the SHM backing for objects tracked by this `Collection_db`.
    Shm_arena* m_shm_arena;

    /// Pool-ID of #m_lend_tracker_pool; generated once on first construct from this arena + cting thread.
    pool_id_t m_lend_tracker_pool_id;

    /**
     * Admin-mode Lend_tracker_pool for use-count tracking of objects constructed in this arena + cting thread.
     * It is `optional` so that inactive_arenas_scan() can delete the Lend_tracker_pool (key step in freeing that
     * RAM) without removing/destroying Collection_db entirely (due to concurrent stat-consumption possibility
     * w/r/t #m_stats_shard).
     *
     * @warning Careful; when modifying the `optional`, or reading it from not-tracked-thread, must lock
     *          #m_collection_dbs_mutex as explained in its doc header.
     */
    std::optional<Lend_tracker_pool> m_lend_tracker_pool;

    /**
     * Map from use-count-slot index to #Object, in insertion (construction) order.  The insertion-order
     * property (via `Linked_hash_map`) enables the oldest-to-newest scan in unused_obj_scan():
     * older objects are likelier to have reached use-count zero.
     */
    flow::util::Linked_hash_map<use_ct_idx_t, Object> m_objs;

    /**
     * For `*this` arena, for the containing-`*this`-constructing thread, the stat-members (all atomic)
     * directly updated during thread-local operations, including along the fast-paths of this_thread_piggy_scan(),
     * constructing_obj(), disposing_obj(), lending_obj().
     *
     * @see stat::Sharded_stats doc header for some background on the TL-sharding technique in our context.
     */
    stat::Sharded_stats m_stats_shard;
  }; // struct Collection_db

  /**
   * For each arena currently being forgotten via forgetting_shm_arena(), the state tracking the progress
   * of that operation.
   */
  struct Arena_forget_progress
  {
    // Data.

    /**
     * Which Thread_lcl_obj_db_admin extant objects have yet to invoke
     * if_requested_forget_arena_related_resources() before the arena is considered forgotten.
     *
     * Note that each given Thread_lcl_obj_db_admin might be in non-degraded state; or in degraded-admin
     * state -- that is, currently executing potentially long-running degraded_admin_thread_body().
     * Either way, though, each will do the same thing: invoke if_requested_forget_arena_related_resources()
     * which will check the atomic-flag state and if armed perform its part of the forget-op and so on.
     * The only difference is how if_requested_forget_arena_related_resources() is triggered
     * (non-degraded: opportunistically a-la this_thread_piggy_scan(); degraded: periodically on wakeup from sleep).
     */
    boost::unordered_flat_set<Thread_lcl_obj_db_admin*> m_dbs_that_still_must_forget;

    /// Functor to invoke, and then immediately destroy, once #m_dbs_that_still_must_forget is `.empty()`.
    Function<void (const flow::log::Log_context_mt*)> m_on_done_func;
  }; // struct Arena_forget_progress

  /// Singleton state shared across all per-thread Thread_lcl_obj_db_admin instances (for a given #Shm_arena type).
  struct Static_state
  {
    // Constructors/destructor.

    /// Constructor.
    Static_state();

    // Data.

    /**
     * State shared -- and acted upon using the `Polled_shared_state` pattern -- between extant
     * Thread_lcl_obj_db_admin per-thread objects, consisting of: which of them must destroy/deinit relevant
     * parts of #m_collection_dbs, as a response to a SHM-arena shutting down
     * (owner-side).  See forgetting_shm_arena().
     *
     * Generally/probably a #Shm_arena (e.g., jemalloc::Ipc_arena) will need to be around, until procedure is completed
     * fully (for all the admin objects in this structure).  Therefore we store a caller-provided functor to invoke
     * when done; presumably to continue the #Shm_arena destruction at that point.
     *
     * Each key is a collection ID (1<=>1 SHM-arena in *this* process)
     * being destroyed; that key's value is the progress state of what
     * is yet to be done before completing the destruction of that arena fully.  Namely it is the set of
     * Thread_lcl_obj_db_admin pointers showing which ones (for that arena's cleanup) have yet to do their thing.
     *
     * That operation is if_requested_forget_arena_related_resources() and is gated
     * on `Thread_lcl_obj_db_admin::m_forget_resources_requested_poll_flag.poll_armed()` returning `true`;
     * the fast-path is it returns `false`, and thus there is nothing for that method to do.
     */
    flow::util::Polled_shared_state<boost::unordered_flat_map<collection_id_t, Arena_forget_progress>>
      m_arenas_to_forget_map;

    /// Registry of all extant per-thread Thread_lcl_obj_db_admin objects; manages their lifecycle and logger.
    flow::util::Thread_local_state_registry<Thread_lcl_obj_db_admin> m_obj_db_registry;

    /// Protects #m_degraded_admin_threads.
    mutable flow::util::Mutex_non_recursive m_degraded_admin_threads_mutex;

    /**
     * Degraded-admin threads spawned by dtors of Thread_lcl_obj_db_admin objects whose owning threads
     * exited while live objects remained.  Stored here so the `std::atexit()` handler can `.join()`
     * any still-running ones at program exit.
     */
    std::vector<Own<flow::util::Thread>> m_degraded_admin_threads;
  }; // struct Static_state

  // Friends.

  /**
   * Friend of Thread_lcl_obj_db_admin: specifically of `Static_state::m_obj_db_registry.while_locked(F)`
   * and within the `S` passed to `F(S)`: `m_collection_dbs[...].m_stats_shard`.  I.e., this function
   * needs to lock extant-thread set of Thread_lcl_obj_db_admin; and to walk each thread's stats shard.
   */
  template<bool, typename Shm_arena2>
  friend void stat::sharded_stats_impl(const Shm_arena2& shm_arena, stat::Sharded_stats* target_stats);

  // Methods.

  /**
   * Core of the piggy-scan (+ certain special-case triggers): for each arena tracked by `*this`,
   * checks for objects whose use-counts have reached zero and garbage-collects them (invokes their deleters,
   * returns their use-count slots, removes them from the in-heap tracking map).
   *
   * @param exhaustive
   *        If `false` (mainstream case, as for piggy-scan): Acts under the assumption that
   *        Lend_tracker_pool::n_unused() may concurrently grow (as other threads/processes happen to drop their
   *        object handles).  Therefore uses important compute-saving techniques
   *        including stopping the scan (for a given arena) once `.n_unused()`-snapshot
   *        no-longer-used extant objects have been found/disposed.
   *        If `true` (when thread exiting, etc.): Makes no such assumption; therefore scans every object, period.
   *        Slow but fine for rare triggers.
   * @param draining
   *        Used for stats/logging only: If `true`, being called from degraded_admin_thread_body() -- so
   *        the algorithm is being run in the drain phase, when the original constructing-thread has exited
   *        (user app's prerogative) but needed to launch the replacement-thread to handle destroying any
   *        remaining live objects, as they become un-live (user app's prerogative).  Otherwise `false`:
   *        mainstream case.
   */
  void unused_obj_scan(bool exhaustive, bool draining);

  /**
   * Post-scan scan and cleanup for a thread that is exiting (or in degraded-admin mode): for each arena
   * whose object map is now empty (all objects disposed), tears down the Lend_tracker_pool (which also
   * triggers the freeing of that RAM by cooperating with Thread_lcl_obj_db_client via Lend_tracker_pool::dead()).
   * If all arenas become empty, the dtor (or degraded-admin loop) can exit cleanly without spawning (or
   * continuing) a degraded-admin thread; do so if and only if this helper returns `true`.
   *
   * @return `true` if and only if every arena that has ever `construct()`ed in `*this` thread has no more
   *         live objects.
   */
  bool inactive_arenas_scan();

  /**
   * If #m_forget_resources_requested_poll_flag has been armed (by forgetting_shm_arena() from another thread),
   * performs this thread's share of the arena-forgetting operation: removes `this` from the
   * per-arena set of admins still needing to act; invokes forget_shm_arena() for the relevant
   * arena(s); and if `this` was the last admin to complete, invokes the `on_done_func()` completion handler.
   *
   * The fast-path (flag not armed) is intentionally ultra-minimal: a single atomic-flag check.
   *
   * @param assume_requested
   *        If `true`, skips the flag check and unconditionally processes all pending forget requests.
   *        Used in the dtor and in degraded-admin mode where the flag is not consumed.
   */
  void if_requested_forget_arena_related_resources(bool assume_requested = false);

  /**
   * Performs the actual per-arena cleanup for a single arena being forgotten: destroys all remaining
   * live objects (even those with non-zero use-counts -- with a WARNING logged for each such object),
   * and erases the arena's entry from #m_collection_dbs (which also destroys the admin-mode
   * Lend_tracker_pool, unlinking its SHM-pool name).
   *
   * @param collection_id
   *        The arena to forget.
   * @param last_one
   *        `true` if this is the last per-thread admin to complete this arena's forget operation
   *        (informational; used for log messages).
   */
  void forget_shm_arena(collection_id_t collection_id, bool last_one);

  /**
   * Body of a degraded-admin thread: periodically (every ~100ms as of this writing) performs
   * unused_obj_scan(), inactive_arenas_scan(), and if_requested_forget_arena_related_resources()
   * until #m_collection_dbs holds no more live objects, then exits.  Called from the thread spawned in our dtor
   * when the owning thread exits with live objects remaining.
   *
   * On entry, moves the exiting thread's #m_collection_dbs into `*this`, then signals
   * `done_with_exiting_thread_obj_db` so the exiting thread can proceed.
   *
   * @param exiting_thread_obj_db
   *        The about-to-be-destroyed admin of the exiting thread; its #m_collection_dbs is moved
   *        into `*this`.  Must not be dereferenced after `done_with_exiting_thread_obj_db` is signaled.
   * @param done_with_exiting_thread_obj_db
   *        Promise to fulfill once the move is complete, unblocking the exiting thread's dtor.
   */
  void degraded_admin_thread_body(Thread_lcl_obj_db_admin* exiting_thread_obj_db,
                                  boost::promise<void>* done_with_exiting_thread_obj_db);

  /**
   * `std::atexit()` handler: joins any degraded-admin threads that are still running at program exit.
   * Registered once, on the first degraded-admin thread spawn.
   */
  static void atexit_degraded_admin_threads_join();

  /**
   * Reads #m_skip_fast_path_verbose_logging.
   * @return See above.
   */
  bool skip_fast_path_verbose_logging() const;

  // Data.

  /// The singleton shared state for all per-thread admins of this #Shm_arena type.
  static Static_state s_state;

  /**
   * Iff `true`, TRACE-or-more-verbose logging was disabled at the last set_logger() call; fast-path
   * logging is entirely skipped without consulting `should_log()`.  While more efficient it means it is
   * not possible to re-enable verbose messages until `set_logger()` is called; but due to the perf-sensitive
   * nature of these paths we consider this reduced responsiveness to be worthwhile.
   */
  std::atomic<bool> m_skip_fast_path_verbose_logging;

  /**
   * Per-thread poll-flag for the `Polled_shared_state` pattern on
   * `Static_state::m_arenas_to_forget_map`.  When forgetting_shm_arena() arms this flag,
   * if_requested_forget_arena_related_resources() will detect it on its next invocation from
   * this thread.
   */
  flow::util::Poll_flag m_forget_resources_requested_poll_flag;

  /**
   * The (unique across time, within this process) ID of the thread that owns `*this` (the thread from
   * which `*this` was constructed).
   */
  const flow::util::Thread_token m_thread_token;

  /**
   * Helper state for the stat::thread_end_gap_mutex() mechanism:
   * Starts `false`; becomes `true` permanently just before thread/app-exit-triggered invocation of `*this` dtor,
   * via `at_thread_exit()`-registered locking of stat::thread_end_gap_mutex(), if and only if in fact that
   * occurred.
   *   - Normally it will occur: thread is about to exit; Boost runs the `at_thread_exit()` items (including
   *     ours as registered in ctor(s) of `_admin` and `_client`); then it runs `thread_specific_ptr` cleanup
   *     callbacks which includes dtors including `*this` dtor.  Said dtor checks `m_locked_gap_mutex == true`;
   *     sees that is the case; and knows to unlock it.  The reason for all of that is explained therein
   *     (or see stat::thread_end_gap_mutex() doc header for a larger overview of that situation).
   *   - At least in some modern Linux, specifically for the OS thread when `exit()`ing (typically after `main()`),
   *     Boost thread-end cleanup does not run: neither `at_thread_exit()` callbacks nor the
   *     `thread_specific_ptr` cleanup.  Instead, when the regular program-end `static` dtors do run,
   *     `Thread_local_state_registry` dtor (in our case Static_state::m_obj_db_registry dtor) indeed runs
   *     and properly cleans-up by deleting `*this` (and other `_admin`s and `_client`s if any apply to the
   *     OS-thread).  So our dtor runs: but `m_locked_gap_mutex == false` is still the case, so we know
   *     stat::thread_end_gap_mutex() was not locked.
   */
  bool m_locked_gap_mutex;

  /**
   * Protects map `m_collection_dbs`; but don't *always* lock when accessing map; see #m_collection_dbs doc header
   * and the following note.
   *
   * "The following note": As explained in `m_collection_dbs` doc header, this protects that map's key-set; but
   * also we formally hereby declare: it also protects the value inside each Collection_db::m_lend_tracker_pool
   * `optional`; that is whether it contains nothing (is null) or (the one) Lend_tracker_pool.
   *
   * Rationale for the last paragraph: During stat consumption stats_update_pre_consumption() shall, from
   * likely-not-`*this`-thread, access certain values inside Collection_db::m_lend_tracker_pool -- but
   * `*this` thread (in degraded_admin_thread_body() only) may nullify that `optional` at any moment
   * (inactive_arenas_scan()).  Therefore, when doing so, inactive_arenas_scan() must lock this mutex; and
   * when reading as just-noted stats_update_pre_consumption() must do the same.  Perf note: Both are rare.
   */
  mutable flow::util::Mutex_non_recursive m_collection_dbs_mutex;

  /**
   * Per-arena tracking map: keyed by collection-ID, each value is the arena's #Collection_db
   * containing the Lend_tracker_pool and the set of live objects constructed from that arena
   * in this thread.  Entries are created on first constructing_obj() for a given arena and
   * removed by forget_shm_arena().  In addition, inactive_arenas_scan() destroys Collection_db::m_lend_tracker_pool
   * but does *not* remove that arena's entry.  (The latter is because of stat-consumption; explained below.)
   *
   * ### Rationale: Choice of container ###
   * It's a hash-map for standard reasons; but why specifically do we use `unordered_flat_map<K, Own<V>>`,
   * (where `V` is Collection_db)?  To answer: there are essentially 3 choices for the basic template:
   *   -# `unordered_map`, the classic.
   *   -# `unordered_node_map`, the classic modernized.
   *   -# `unordered_flat_map`, the standard for performance -- if certain behaviors of the other two are
   *      not required.
   *
   * Throw out (1), as in simple terms (2) is better.  Details are out there for perusal, and there are some
   * niggles, but all in all that sufficiently covers it.  So that leaves (2) versus (3).
   *
   * In general these days (3) `unordered_flat_map` is the highest-performance hash-map (omitting details which
   * are well publicized).  However, `u_f_m<K, V>`, for us, would be insufficient: we need pointer stability
   * of `V` in one form or another.  (See Design below for the key explanations.)  We don't need it consistently
   * but really just one in one situation (again, see Design below); thus simply changing it to
   * `u_f_m<K, Own<V>>` gives us what we need.  In particular, in the one place where we need a `V`
   * (Collection_db) to not move in memory, we save the `.get()` -- a raw pointer `V*` -- until we (quite soon)
   * dereference it and do some work; and done.  This is required for correctness, but we also get
   * significant syntactic benefit: we can use `[]` for lookup and auto-insertion, plus there's no need
   * to use `.emplace()`-and-friends (which would require a ctor for Collection_db -- cannot construct with
   * member-by-member init).  The perf cost is acceptable: an extra use of the heap, rarely (insertion is rare);
   * and a single extra pointer deref per #Shm_arena in the fast-path (of this_thread_piggy_scan()) which
   * fades into the background versus the rest of the required work.
   *
   * The alternative would have been (2); or really two sub-alternatives:
   *   - 2a: `unordered_node_map<K, V>.
   *   - 2b: `unordered_node_map<K, Own<V>>.
   *
   * (2b) is plainly worse; it still has the added cost of `Own`, while the base container performance is worse.
   * So that leaves (2a).  That one is at least worse syntactically (in the ways mentioned), but ignoring that:
   * We come down to, what's the better trade-off: avoid heap/indirection of `Own<V>`, or get the significantly
   * better base performance (for lookup especially) of `...flat_...` over `...node_...`?  The answer is that
   * the latter is better.
   *
   * Let's emphasize one key point about perf here: #m_collection_dbs is a perf-relevant data structure that
   * figures in ~every this_thread_piggy_scan() which in turn is *the* perf-relevant part of
   * Thread_lcl_obj_db_admin... one could even reasonably argue of SHM-jemalloc at large (probably jemalloc, or
   * the memory manager of choice, is at least as important, but they're definitely mutually within the ballpark
   * of perf-relevance).  Consider: every this_thread_piggy_scan(), called on ~every SHM-jemalloc API,
   * opportunistically, must:
   *   -# loop through #m_collection_dbs;
   *      -# for each arena (#collection_id_t = integer = the key), grab the mapped Collection_db;
   *      -# then access its Lend_tracker_pool, grab the aux-pool base-vaddr, at a certain constant offset
   *         grab an integer (Lend_tracker_pool::n_unused());
   *      -# then based on the value of that integer, do a roughly proportional-to-it amount of further work.
   *
   * Now, the (reasonable) hope is that for most invocations of this algorithm, for most arenas touched
   * by a thread-local `*this`, this `n_unused` integer will equal zero, meaning no further work is required
   * (specifically: there are no first-class zombie-objects in SHM to destroy).  However the steps up to that
   * are mandatory per ever-having-`.construct()`ed `Shm_arena`!  Therefore the speed of those steps is important.
   * So choosing the proper container is important.
   *
   * @note constructing_obj(), disposing_obj(), lending_obj() are also frequent and also access `m_collection_dbs`.
   *
   * ### Design / Thread safety ###
   * If one ignores the topic of stat-keeping (stat::Sharded_stats), the situation is straightforward.
   * We need a Collection_db per arena for reasons that are hopefully clear (in short, we're serving the needs
   * of owner-arenas like jemalloc::Ipc_arena).  All access is thread-local; even forgetting_shm_arena(),
   * invoked from one thread but charged with removing the given arena's node from #m_collection_dbs, uses
   * a special tool (Flow's `Polled_shared_state`) to outsource this removal to thread-local code
   * (forget_shm_arena()).  Hence no locking is necessary, and there is no thread safety about which to worry.
   *
   * The situation somewhat complicates when adding `Sharded_stats` to the mix (namely into
   * Collection_db).  While the stats within that `struct` are being updated during regular work
   * (e.g., constructing_obj(), this_thread_piggy_scan()), it's all simple (in terms of concurrency); they're
   * `atomic`s so that during stat-consumption (from some other thread) no locking is needed -- this is
   * standard (see `flow::util::stat` doc header for discussion of various stat-keeping techniques).  The
   * updating itself is all strictly thread-local.  No problem.
   *
   * The problem: Consider stat-consumption from some thread U, while a thread-local `*this` corresponds to
   * thread V.  If we simply had a single stat::Sharded_stats in `*this`: no issue; but we have N of them:
   * one per arena (per #collection_id_t / Collection_db pair).  In fact stat-consumption executes for a
   * given #Shm_arena A of the stat-consumption API's caller's choice; so only a particular Collection_db C
   * is of interest.  Hence, in U, that code must grab V's (among others) Collection_db C from
   * `this->m_collection_dbs`; that is if there even *is* a C (which is the case only if
   * `this->constructing_obj()` has ever executed on behalf of arena A -- `A.construct<T>(...)`).
   *
   * So in U we must access the key-set of #m_collection_dbs; but in thread V, constructing_obj() may
   * at any moment concurrently modify this key-set (occurs when a new-to-`*this` arena first constructs
   * from V).  Therefore, at least, both the thread-U key-set read and the thread-V key-set write must
   * be mutex-protected.  That's #m_collection_dbs_mutex.
   *
   * To continue: upon grabbing the value `&C` (address of the Collection_db for key A), in U we then
   * access the `Stat_set` stat::Sharded_stats, aggregate it with those of other V-like threads' `*this`es,
   * and return the assembled stat-values.  Note that #m_collection_dbs_mutex need not be locked for that
   * part; and not-locking it for that part is good for perf, since a concurrent thread-V activity can be
   * blocked by longer lock (granted: not frequently, and still for a split-second).  Such a lock-less access
   * of `C->...` is safe as long as these hold:
   *   - Node A is not to be removed from the key-set of #m_collection_dbs during thread-U stat-consumption.
   *     (It won't be: If they're accessing A's stats, they are not allowed to let arena A disappear, which is
   *     the only thing that can cause the removal of that node: forgetting_shm_arena() path.)
   *   - `&C` -- the location of Collection_db -- cannot change.  Stat-consumption will grab `&C` and then
   *     read `*&C` as part of aggregation.  If `&C` became wrong during this short section => catastrophe.
   *     (This is the pointer-stability requirement which we have assured; see preceding doc header section.)
   *
   * @warning Restating for maintainers!  Do not remove keys from #m_collection_dbs, except in forget_shm_arena().
   *          In particular that's why inactive_arenas_scan() only checks whether the map has only
   *          empty `Collection_db`s, as opposed to removing them and then checking for
   *          `m_collection_dbs.empty()`.
   *
   * ### When must we lock #m_collection_dbs_mutex? ###
   * Above we explained why it is necessary to have it; and one situation in which it must be locked.
   * The naive assumption would be that every access to the key-set of #m_collection_dbs requires a lock.
   * This is not the case.  (Though, if it were the case, it would still not be catastrophic for perf:
   * Stat-consumption is rare, so there is almost never contention, so the locks/unlocks along the fast-paths
   * of this_thread_piggy_scan() + `{construct|dispos|lend}ing_obj()` -- if one were always required -- would
   * be quite quick.  Nevertheless it would still be unpleasant: We go to all this trouble to TL-shard these
   * damned stats and TL-distribute all the main work of the obj-DB, and yet still we have to lock some mutex
   * every time?  Just for stats at that?  No worries though: we don't have to lock it.  So back to that.)
   *
   * So far we've shown that (1) thread U at stat-consumption time (rare) must lock it when obtaining the
   * arena's-of-interest Collection_db for its stats-`struct`; and (2) thread V must lock it when *inserting*
   * a new (to `*this` thread) arena's new Collection_db (also rare).  What other places must lock for safety?
   * That question is equal to the question, "when am I reading X, while someone might concurrently write X; or
   * when am I writing X, when someone might concurrently read X?" -- where X is `m_collection_dbs`'s key-set.
   * The answer is, respectively, as follows.
   *   - Thread U during stat-consumption -- the *only* time `*this` is accessed by a thread other than V.
   *     Already discussed above.
   *   - Thread V when *adding* to the key-set.  Already discussed above.  It is constructing_obj().
   *     (Reading it -- the fast-path -- is fine; by design it's the only thread that accesses `*this`'s key
   *     set and writes to it; and it can't concurrently read and write.)  Plus:
   *     - Thread V when *removing* from the key-set.  That is, as of this writing, forget_shm_arena(),
   *       triggered by forgetting_shm_arena()s `static` API (and rare).
   *       - Again... removing it is fine, as long as stat-consumption isn't reading the thing being
   *         removed.  In case of forget_shm_arena() it won't be.  So it is fine.
   *
   * So, again: The fast-paths of this_thread_piggy_scan() + `{construct|dispos|lend}ing_obj()` never
   * lock #m_collection_dbs_mutex.
   */
  boost::unordered_flat_map<collection_id_t, Own<Collection_db>> m_collection_dbs;
}; // class Thread_lcl_obj_db_admin

/**
 * (Internal-use) Where a Thread_lcl_obj_db_admin cannot handle lend-object and dispose-object tracking, because
 * the end user triggering those ops happens to be in a thread different from the one in which the object
 * was `.construct()`ed, use Thread_lcl_obj_db_client: whether in the owner process (but not-constructing thread) or
 * a borrower process (any thread).
 *
 * @see Thread_lcl_obj_db_admin first.  Then come back here.
 *
 * We keep this doc header short: It is best to understand the doc header of Thread_lcl_obj_db_admin.  This should
 * make clear when that class template is not sufficient; at that point invoke the relevant method of
 * Thread_lcl_obj_db_client.  Namely:
 *   - this_thread_obj_db()->lending_obj(): From owner process but thread that is *not* the one where
 *     Thread_lcl_obj_db_admin::constructing_obj() was called.  Otherwise similar trigger to eponymous admin-method
 *     (which can only be invoked from the constructing-thread).
 *   - this_thread_obj_db()->disposing_obj() (`Shm_arena`-taking overload): Ditto.
 *   - this_thread_obj_db()->disposing_obj() (non-`Shm_arena`-taking overload): From a borrower process.
 *     Otherwise similar trigger to eponymous admin-method (which can only be invoked from the constructing
 *     thread, necessarily in the owner process... not any borrower(s)).
 *
 * ### Impl notes ###
 * The docs on the data members and on/inside methods should be sufficient.  Generally Thread_lcl_obj_db_admin has
 * to orchestrate book-keeping about a particular thread's constructed objects; plus certain tricky cleanup-y
 * activities.  Our task is much simpler; really the bottom line is, some other threads need to either `++` or `--`
 * certain use-counts; so we need to maintain client-mode `Lend_tracker_pool`s; that's about it.  The caller must,
 * essentially, supply (1) the lend-tracker-pool's ID (recall that `pool_id_t`s are completely unique/1-1 across the
 * system) and (2) a use-count slot index within that pool.  We store nothing per-object; and all we need to do
 * beyond forwarding the dispose/lend request to a Lend_tracker_pool is, the first time a given `pool_id_t` is
 * mentioned to a `*this`, create/save a Lend_tracker_pool (client-mode: so it'll open a SHM-pool).
 *
 * There is one additional task a `*this` must handle: the cleanup of `Lend_tracker_pool`s that will never be used
 * again; this is potentially important to prevent SHM-RAM leaks over time, as a Lend_tracker_pool existing means
 * a SHM-pool handle being open, which prevents the underlying RAM (in-SHM) to be returned to the OS for general use.
 *
 * Such cleanup is handled, and explained, inside helper new_pool_data().  If interested in cleanup please read that;
 * then come back here.
 *
 * Lastly w/r/t cleanup: new_pool_data() self-cleans as explained; that covers #m_lend_tracker_pools which is
 * by far the bulkiest aspect of a `*this`.  What about #m_per_arena_stats_shards?  Indeed: per-arena stat-shards
 * need cleanup albeit triggered *not* by a `*this` disappearing (user thread exiting); in fact `*this` dtor
 * instead offloads any stat-shards into stat::Finalized_shards.  Instead their cleanup is triggered only by
 * respective arena(s) shutting down (which is controlled by the user).  Therefore we have a `static`
 * forgetting_shm_arena(), superficially similar to `_admin`'s.  However we emphasize that both its impl and its
 * importance (especially the latter) are far lesser than for `_admin`.
 *
 * @tparam Shm_arena_t
 *         `Thread_lcl_obj_db_client<A1>` and `Thread_lcl_obj_db_client<A2>` are fully independent singleton
 *         registries if `A1 != A2`, so the parameter serves as a compile-time discriminator.  The type's API may
 *         also be accessed (e.g. `.get_id()`).  In practice (SHM-jemalloc): `A` = jemalloc::Ipc_arena.
 */
template<typename Shm_arena_t>
class Thread_lcl_obj_db_client :
  public flow::log::Log_context_mt,
  private boost::noncopyable
{
public:
  // Types.

  /// Short-hand alias for the template parameter.
  using Shm_arena = Shm_arena_t;

  // Constructors/destructor.

  /**
   * Constructs thread-local `*this` as invoked internally by Static_state::m_obj_db_registry (on first
   * this_thread_obj_db() call from a given thread).
   *
   * @param logger_ptr
   *        Logger to use for logging subsequently; taken from Static_state::m_obj_db_registry.
   */
  explicit Thread_lcl_obj_db_client(flow::log::Logger* logger_ptr);

  /// Closes any client-mode Lend_tracker_pool handles we hold; thread is exiting so they won't be needed.
  ~Thread_lcl_obj_db_client();

  // Methods.

  /**
   * Identical to Thread_lcl_obj_db_admin::this_thread_obj_db() but for the client-side DB.
   * @return See above.
   */
  static Thread_lcl_obj_db_client* this_thread_obj_db();

  /**
   * Identical to Thread_lcl_obj_db_admin::dbs_set_logger().
   * @param logger_ptr
   *        See above.
   */
  static void dbs_set_logger(flow::log::Logger* logger_ptr);

  /**
   * Identical to Thread_lcl_obj_db_admin::set_logger().
   * @param logger_ptr
   *        See above.
   * @return See above.
   */
  flow::log::Logger* set_logger(flow::log::Logger* logger_ptr);

  /**
   * Client-side (owner process, non-constructing thread) counterpart to
   * Thread_lcl_obj_db_admin::lending_obj().  Increments the object's use-count via a client-mode
   * Lend_tracker_pool (opened lazily on first use of a given `lend_tracker_pool_id`).
   *
   * @param shm_arena
   *        Arena owning the object; used to derive the pool name on first access.
   * @param lend_tracker_pool_id
   *        See Thread_lcl_obj_db_admin::constructing_obj() out-arg.
   * @param use_ct_idx
   *        See above.
   */
  void lending_obj(const Shm_arena& shm_arena, pool_id_t lend_tracker_pool_id, use_ct_idx_t use_ct_idx);

  /**
   * Client-side (owner process, non-constructing thread) counterpart to
   * Thread_lcl_obj_db_admin::disposing_obj().  Decrements the object's use-count via a client-mode
   * Lend_tracker_pool.
   *
   * @param shm_arena
   *        See lending_obj().
   * @param lend_tracker_pool_id
   *        See lending_obj().
   * @param use_ct_idx
   *        See lending_obj().
   */
  void disposing_obj(const Shm_arena& shm_arena, pool_id_t lend_tracker_pool_id, use_ct_idx_t use_ct_idx);

  /**
   * Client-side (**borrower** process) counterpart to Thread_lcl_obj_db_admin::disposing_obj().
   * Like the owner-side overload but takes `owner_id` and `collection_id` instead of a `Shm_arena&`,
   * since the borrower lacks a direct arena reference and must reconstruct the pool name via
   * `Borrower_shm_pool_collection_repository`.
   *
   * @param lend_tracker_pool_id
   *        See lending_obj().
   * @param use_ct_idx
   *        See lending_obj().
   * @param owner_id
   *        Owner process ID; needed to reconstruct the SHM-pool name.
   * @param collection_id
   *        Arena/collection ID in the owner process; needed to reconstruct the SHM-pool name.
   */
  void disposing_obj(pool_id_t lend_tracker_pool_id, use_ct_idx_t use_ct_idx,
                     owner_id_t owner_id, collection_id_t collection_id);

  /**
   * Initiates the process of forgetting (cleaning up) a given #Shm_arena across all per-thread
   * Thread_lcl_obj_db_client instances.  As of this writing it's merely deleting some relatively small
   * book-keeping, lest it be leaked.
   *
   * This is potentially asynchronous: each per-thread client shall perform its share of the cleanup
   * from its own thread.
   *
   * ### When can you call this? ###
   * Do not call it concurrently to a user thread exiting such that that thread has ever called
   * `*_obj()` w/r/t the arena `collection_id`.  Comments inside ~Thread_lcl_obj_db_client() dtor explain
   * why.  (To recap: at worst it'd leak some `Sharded_stats` w/r/t that arena; but (1) leaks (even small ones)
   * are unsightly, and (2) Thread_lcl_obj_db_admin::forgetting_shm_arena() (in practice called right before/after
   * `_client`'s) has a similar restriction anyway (but to guard against a much greater danger).)
   *
   * ### Impl ##
   * See inside; but in short for your reading convenience: We could do it synchronously by
   * performing a `Thread_local_state_registry::while_locked()` + `...::State_per_thread_map` and
   * performing the deletion og the given arena's stuff from each `*this`'s #m_per_arena_stats_shards -- but
   * then the key-set of a `*this` would be potentially modified from not-the-owning-thread; which
   * would mean #m_per_arena_stats_shards_mutex would need to be locked even whenever merely searching
   * #m_per_arena_stats_shards in fast-path constructing_obj() and disposing_obj() (owner-process overload).
   * (See #m_per_arena_stats_shards doc header for concurrency-related relevant design.)
   * Though rarely contended and therefore cheap, we'd still rather keep the niceness of avoiding that.
   * So we use `Polled_shared_state` to distribute this (simple) work to the corresponding threads.
   *
   * That technique is similar to that used in Thread_lcl_obj_db_admin::forgetting_shm_arena(), but in our
   * case it is significantly simpler, as (1) there's no freeing of user objects/user-anything-else, and (2) there's
   * nothing in particular to do on completion of the cross-thread procedure.  There is no `on_done_func()` or
   * jemalloc::Thread_cache interaction or...  We're just removing a node from each #m_per_arena_stats_shards.
   *
   * @param collection_id
   *        Identifies the arena to forget.
   */
  static void forgetting_shm_arena(collection_id_t collection_id);

  /**
   * Returns reference-to-`const` stat-set, carrying information that is global but per-template-instance-type.
   *
   * @return See above.  See also stat::Obj_db_aux_pool_global_stats doc header.
   */
  static const arena_lend::stat::Obj_db_aux_pool_global_stats& obj_db_aux_pool_global_stats();

  /// Resets obj_db_aux_pool_global_stats().  Formal meaning of a reset is discussed in `flow::util::stat` doc header.
  static void obj_db_aux_pool_global_stats_reset();

private:
  // Types.

  /// Short-hand for Thread_lcl_obj_db_admin::Own.
  template<typename T>
  using Own = boost::movelib::unique_ptr<T>;

  /// Per-lend-tracker-pool bookkeeping on the client side: the pool handle itself and its identity.
  struct Pool_data
  {
    // Data.

    /// Identifies the arena/collection this pool belongs to (owner PID + collection ID).  For logging.
    Uniq_collection_id m_uniq_collection_id;
    /// Client-mode handle to the SHM-pool containing use-count slots.
    Lend_tracker_pool m_lend_tracker_pool;
  };

  /// Singleton state shared across all per-thread Thread_lcl_obj_db_client instances (for a given #Shm_arena type).
  struct Static_state
  {
    // Constructors/destructor.

    /// Constructor.
    Static_state();

    // Data.

    /**
     * State shared -- and acted upon using the `Polled_shared_state` pattern -- between extant
     * Thread_lcl_obj_db_client per-thread objects, consisting of: which of them must erase relevant
     * nodes of #m_per_arena_stats_shards, as a response to a SHM-arena shutting down
     * (owner-side).  See forgetting_shm_arena().
     *
     * Each key is a collection ID (1<=>1 SHM-arena in *this* process)
     * being destroyed; that key's value is the progress state of what
     * is yet to be done before completing the destruction of that arena fully.  Namely it is the set of
     * Thread_lcl_obj_db_client pointers showing which ones (for that arena's cleanup) have yet to do their thing
     * (which is, again, simply the removal of that `collection_id_t`-node from #m_per_arena_stats_shards).
     *
     * That operation is if_requested_forget_arena_related_resources() and is gated
     * on `Thread_lcl_obj_db_client::m_forget_resources_requested_poll_flag.poll_armed()` returning `true`;
     * the fast-path is it returns `false`, and thus there is nothing for that method to do.
     */
    flow::util::Polled_shared_state<boost::unordered_flat_map<collection_id_t,
                                                              boost::unordered_flat_set<Thread_lcl_obj_db_client*>>>
      m_arenas_to_forget_map;

    /// Registry of all extant per-thread Thread_lcl_obj_db_client objects; manages their lifecycle and logger.
    flow::util::Thread_local_state_registry<Thread_lcl_obj_db_client> m_obj_db_registry;
  }; // struct Static_state

  // Friends.

  /**
   * Friend of Thread_lcl_obj_db_client: specifically of `Static_state::m_obj_db_registry.while_locked(F)`
   * and within the `S` passed to `F(S)`: `m_per_arena_stats_shards[...]`.  I.e., this function
   * needs to lock extant-thread set of Thread_lcl_obj_db_client; and to walk each thread's stats shard.
   */
  template<bool, typename Shm_arena2>
  friend void stat::sharded_stats_impl(const Shm_arena2& shm_arena, stat::Sharded_stats* target_stats);

  // Methods.

  /**
   * Adds a new entry to #m_lend_tracker_pools for the given pool ID (opening a client-mode Lend_tracker_pool);
   * and, first, opportunistically deletes any now-dead `Lend_tracker_pool`s et al.
   *
   * See class doc header Impl section for background about the latter action.
   *
   * Pre-condition: `m_lend_tracker_pools[lend_tracker_pool_id]` does not yet exist (that is the actual node in
   * the map).
   *
   * ### Errors (attention) ###
   * Outside of the usual ambient dangers -- out-of-memory and such -- there is exactly one reason new_pool_data()
   * can fail:
   *   -# This arena (IDed by `owner_id` + `collection_id`) has not yet been touched, in the `_client` sense,
   *      by this thread, so we must among other book-keeping open the `_admin`-created pool named `new_pool_name`.
   *      This is a pre-condition of new_pool_data() being called.
   *   -# However we failed to open the pool by that name.
   *
   * In that case this method `new_pool_data()` throws an exception originating in
   * the client-mode Lend_tracker_pool ctor.  This occurs regardless of why, exactly, we could not open the pool.
   * However, depending on the context of who's calling `new_pool_data()`, it *might* be appropriate to
   * detect the specific cause wherein `new_pool_name` is not in the file-system (any longer) -- as opposed to
   * others (permission error; who knows what else).  See Lend_tracker_pool client-mode ctor doc header for
   * simple instructions w/r/t how to detect that case from the caught exception, if desired.
   *
   * @param lend_tracker_pool_id
   *        Pool ID to add.
   * @param owner_id
   *        Owner process ID; used to identify the pool's arena/collection and reconstruct the pool name.
   * @param collection_id
   *        Arena/collection ID; see `owner_id`.
   * @param new_pool_name
   *        SHM-pool name to open.
   * @return Pointer to the new Pool_data.  #m_lend_tracker_pools holds the same pointer within an `Own`.
   */
  Pool_data* new_pool_data(pool_id_t lend_tracker_pool_id, owner_id_t owner_id,
                           collection_id_t collection_id, const Shared_name& new_pool_name);

  /**
   * Helper that returns the TL-shard `m_per_arena_stats_shards[C]`, where `C`
   * identifies `*shm_arena`, an arena within our process -- for the object in lending_obj() or (owner-side overload)
   * disposing_obj() -- creating/inserting the required stat::Sharded_stats if necessary first.
   *
   * @param shm_arena
   *        See lending_obj() and/or (owner-side overload) disposing_obj().
   * @return See above.  Not null.
   */
  stat::Sharded_stats* stats_shard(const Shm_arena& shm_arena);

  /**
   * If #m_forget_resources_requested_poll_flag has been armed (by forgetting_shm_arena() from potentially
   * another thread), performs this thread's share of the arena-forgetting operation: removes `this` from the
   * per-arena set of clients still needing to act; and erases each being-forgotten arena's stuff
   * from #m_per_arena_stats_shards (which was the goal of forgetting_shm_arena()).
   *
   * The fast-path (flag not armed) is intentionally ultra-minimal: a single atomic-flag check.
   */
  void if_requested_forget_arena_related_resources();

  /**
   * Mutable version of obj_db_aux_pool_global_stats(), for internal stat-updating.
   * @return See above.
   */
  static arena_lend::stat::Obj_db_aux_pool_global_stats* obj_db_aux_pool_global_stats_mutable();

  // Data.

  /// The singleton shared state for all per-thread clients of this #Shm_arena type.
  static Static_state s_state;

  /// Analogous to Thread_lcl_obj_db_admin::m_skip_fast_path_verbose_logging.
  std::atomic<bool> m_skip_fast_path_verbose_logging;

  /// Analogous to eponymous Thread_lcl_obj_db_admin::m_forget_resources_requested_poll_flag.
  flow::util::Poll_flag m_forget_resources_requested_poll_flag;

  /// Analogous to Thread_lcl_obj_db_admin::m_thread_token.
  const flow::util::Thread_token m_thread_token;

  /// Identical to Thread_lcl_obj_db_admin::m_locked_gap_mutex.  It is a bit hairy so do read that doc header.
  bool m_locked_gap_mutex;

  /**
   * Map from lend-tracker-pool ID to its client-side bookkeeping.  Entries are added lazily
   * (on first lend or dispose involving a given pool ID) and removed opportunistically
   * (when dead, during new_pool_data()) or on `*this` destruction (thread exit).
   */
  boost::unordered_flat_map<pool_id_t, Own<Pool_data>> m_lend_tracker_pools;

  /// Protects map #m_per_arena_stats_shards but see its doc header regarding when the locking is necessary.
  mutable flow::util::Mutex_non_recursive m_per_arena_stats_shards_mutex;

  /**
   * For each arena for which 1+ lending_obj() or (owner-process overload) disposing_obj() has been
   * performed for `*this` thread, and which has not yet been removed via forgetting_shm_arena(),
   * this holds the stat-members (all atomic) directly updated during those thread-local operations.
   *
   * Protected by #m_per_arena_stats_shards_mutex; but see Design below.
   *
   * @see stat::Sharded_stats doc header for some background on the TL-sharding technique in our context.
   *
   * ### Rationale: choice of container / When must we lock #m_per_arena_stats_shards_mutex? ###
   * This + #m_per_arena_stats_shards_mutex should be viewed as a far-smaller/simpler analog of
   * Thread_lcl_obj_db_admin::m_collection_dbs and Thread_lcl_obj_db_admin::m_collection_dbs_mutex;
   * instead of a `struct Collection_db` with various things that include, but certainly aren't limited to,
   * stats (in fact the non-stats are considerably more critical) we have just the stats.  So it's as-if
   * Thread_lcl_obj_db_admin::Collection_db::m_stats_shard were the only member of `Collection_db`.
   *
   * Hence the notes in the Rationale + When-must-we-lock... sections of the
   * Thread_lcl_obj_db_admin::m_collection_dbs doc header apply here too.  We don't mean this in a literal sense;
   * we don't have a `this_thread_piggy_scan()` nor a `constructing_obj()` for example.  Bottom line is, if one
   * grasps the design there, then one will quickly grasp the relevant parallels here.
   */
  boost::unordered_flat_map<collection_id_t, Own<stat::Sharded_stats>> m_per_arena_stats_shards;
}; // class Thread_lcl_obj_db_client

// Free functions: in *_fwd.hpp.

// Template static initializers.

template<typename Shm_arena_t>
typename Thread_lcl_obj_db_admin<Shm_arena_t>::Static_state Thread_lcl_obj_db_admin<Shm_arena_t>::s_state;

template<typename Shm_arena_t>
typename Thread_lcl_obj_db_client<Shm_arena_t>::Static_state Thread_lcl_obj_db_client<Shm_arena_t>::s_state;

// Template implementations: Thread_lcl_obj_db_admin.

template<typename Shm_arena_t>
Thread_lcl_obj_db_admin<Shm_arena_t>::Thread_lcl_obj_db_admin(flow::log::Logger* logger_ptr) :
  flow::log::Log_context_mt(nullptr, Log_component::S_SHM), // Really we init logger ptr in set_logger() just below.

  // (Avoid compiler warnings; initialize.) Really we init this in set_logger() just below.
  m_skip_fast_path_verbose_logging(false),

  m_thread_token(flow::util::this_thread_unique_token()),
  m_locked_gap_mutex(false)
{
  using boost::this_thread::at_thread_exit;

  set_logger(logger_ptr);

  /* On this thread's exit, take stat::thread_end_gap_mutex() *before* the Thread_local_state_registry removes
   * *this (boost runs at_thread_exit() strictly before the tss-cleanup that runs our ~dtor()); ~dtor() adopts +
   * releases it after our shard-handoff, closing the consume-vs-teardown "Gap".  Recursive, since this thread may
   * also own a Thread_lcl_obj_db_client doing the same.  See stat::thread_end_gap_mutex(). */
  at_thread_exit([this]()
  {
    /* We are in thread m_thread_token.  The array of at_thread_exit(F) F()s is executing in some unknown
     * order; we are one of the F()s.  Right after that completes, Boost shall execute thread_specific_ptr
     * cleanup functions/deleters (therefore Thread_local_ptr cleanup functions/deleters, therefore
     * Thread_local_state_registry cleanup, therefore our ~_admin() dtor -- see below -- among other TL-dtors).
     * You will note that (in the normal case, where that dtor runs in m_thread_token-thread) ~_admin() dtor will
     * in fact take-over the locked mutex we are about to lock; and it will therefore .unlock() it as the
     * last thing in ~_admin().
     *
     * Why?  Answer: See full explanation in thread_end_gap_mutex() doc header.  In short:
     *
     * We serialize all thread-teardown procedures from all `_admin`s including `*this` one (plus similarly
     * `_client` teardowns), as a single unit; versus any sharded_stats_impl() which might concurrently execute.
     *
     * Without that measure we could in sharded_stats_impl() miss 0+ `Shard`s from 0+ `_admin`s (including *this)
     * and 0+ `_client`s: A given m_collection_dbs[]->m_stats_shard might be, for a split second, in no
     * registry-walkable _admin or _client or Finalized_shards, even though it'll absolutely
     * end up there (in our case right near where ~_admin() returns; see below).  So this ensures such walks
     * execute strictly before or strictly after this short gap.
     *
     * There are various considerations: such as: the mutex is recursive and global across all <Shm_arena> types
     * (multiple `_admin`s, not all with our <Shm_arena>;plus `_client`s simlarly).  thread_end_gap_mutex() doc
     * header explains it all. */

    FLOW_LOG_TRACE_LOCKED("Tl_obj_db_admin: The Gap mutex: locking for this TL-object.");
    m_locked_gap_mutex = true; // Safety flag; exact reason (beyond being defensive) is explained in its doc header.
    stat::thread_end_gap_mutex().lock();
  }); // at_thread_exit()
} // Thread_lcl_obj_db_admin::Thread_lcl_obj_db_admin()

template<typename Shm_arena_t>
Thread_lcl_obj_db_admin<Shm_arena_t>::~Thread_lcl_obj_db_admin()
{
  using flow::util::Thread;
  using flow::log::Logger;
  using flow::log::Sev;
  using flow::util::ostream_op_string;
  using flow::util::Lock_guard;
  using flow::util::Mutex_recursive;
  using flow::util::stat::store;
  using flow::async::reset_this_thread_pinning;
  using Log_config = flow::log::Config;
  using boost::promise;
  using std::optional;

  const auto this_thread_unique_token = flow::util::this_thread_unique_token();
  if (this_thread_unique_token != m_thread_token)
  {
    FLOW_LOG_WARNING_LOCKED
      ("Tl_obj_db_admin: "
       "Shutting down from different thread (unique-token [" << this_thread_unique_token << "]) "
       "than the one (unique-token [" << m_thread_token << "]) that created us; "
       "bailing out.  Honestly this is strange; we are a `static` thread-local registry, and per-thread "
       "dtors should run before the central static object's.  In any case, this can conceivably -- "
       "in the absence of bugs -- only occur when program is exiting, at which "
       "point graceful shutdown of resources is arguably less essential.");
    assert((!m_locked_gap_mutex) && "Tl_obj_db_admin: We have a misdesign/bug; something is off thread-wise.");
    // We "even" skip the stats-saving stuff below.  Just don't feel like dealing with anything in this odd situation.
    return;
  }
  // else

  /* Take over the (recursive) already-locked mutex; the corresponding .lock() would have occurred very recently
   * in the F() we registered via at_thread_exit(F) in the _admin ctor above.  Pleae read the comment over there;
   * then come back here.  Now: this continues The Gap wherein sharded_stats() cannot concurrently execute, locking
   * the same mutex in a different thread.  (In the long comment below we reiterate why that function executing
   * concurrently could be trouble.)  So the question is, when should the RAII-lock expire?  On our (`*this`'s)
   * account the answer is: once either all still-live `m_stats_shard`s in *this have either (1) ended up
   * in Finalized_shards singleton or (2) ended up in replacement _admin (degraded_admin_thread_body() type)
   * (which occurs soon after that _admin being constructed and added to the registry: obj_db_per_thread()).
   * Conveniently that is simply whenever -- from this point on -- this dtor returns.
   *   - Possibility 1: inactive_arenas_scan() below returns true; we off-load all stat-shards into Finalized_shards;
   *     and return.
   *   - Possibility 2: it returns false; so we start drain-thread, create-and-register new _admin in that thread,
   *     move() all of m_collection_dbs (including all [A]->m_stats_shard) into new _admin (also in that thread),
   *     and signal (also from that thread) the future-wait below, indicating that ~_admin() can now return...
   *     which it immediately does.
   *
   * So, great: Unlocking at return from this dtor is both safe (happens no earlier than is safe) and tight (happens
   * pretty much as early as possible).  Also -- to restate comment(s) from ctor et al -- there could be other
   * teardowns happening in this thread in interlaved fashion (`_client`(s), `_admin`(s) from different <Shm_arena>);
   * then thread_end_gap_mutex() (recursive) would be locked potentially multiple times.  The last .unlock()
   * like ours would let a concurrent sharded_stats() to proceed.  Again: thread_end_gap_mutex() doc header gets into
   * everything. */
  optional<Lock_guard<Mutex_recursive>> gap_lock;
  if (m_locked_gap_mutex) // See this guy's doc header for explanation about this guard.
  {
    FLOW_LOG_TRACE_LOCKED("Tl_obj_db_admin: The Gap mutex is locked: adopting until dtor returns.");
    gap_lock.emplace(stat::thread_end_gap_mutex(), boost::adopt_lock);
  }
  else
  {
    FLOW_LOG_INFO_LOCKED("Tl_obj_db_admin: The Gap mutex is *not* locked; presumably this is the OS/main thread; "
                         "probably application exit()ing/typically post-main(); no real danger of concurrent "
                         "stat-touching.");
  }

  /* The thread is exiting, and Thread_local_state_registry<_admin> therefore executed this ~_admin() as a cleanup
   * hook.  First thing to realize is that -- for an original _admin per-thread object created in the *user* thread,
   * when they triggered this_thread_obj_db()->constructing_obj() for the first time in that *user* thread --
   * there are either exactly 1 or exactly 2 _admin objects to ever exist, and therefore ~_admin() dtors to ever
   * execute, and therefore this is one of those exactly 1-2 such dtor invocations.
   *  - Invocation/SCENARIO 1 (happens): When user thread exits (user application chose to do so).  ~dtor() runs.
   *  - Invocation/SCENARIO 2 (may happen): If SCENARIO-1 ~dtor() had to create a degraded-admin thread before
   *    exiting, then this ~dtor() shall later run when *that* thread exits.
   *
   * If you're reading this linearly for the 1st time, the 2nd bullet might make no sense, since you haven't seen
   * the part that spawns that 2nd thread (which is *not* a user thread, but it is performing work for us on that
   * ex-thread's behalf).  So, in that case, for now don't worry about it.  We shall comment for each scenario
   * below, and you can at first ignore the SCENARIO-2's comments; focus on SCENARIO-1 ones.
   * ---
   *
   * SCENARIO 1: This ~dtor() executing in user's thread, due to user's thread exiting.
   *
   * The thread exiting can happen while the relevant cting arena(s) is/are still up.
   * It can also happen far after that.  In the latter case they should (must) have erased/cleaned everything
   * to do with those arena(s) that have been destroyed, from like Ipc_arena dtor, before we can execute.
   * So we shall assume that if there is anything left, it's because some arena(s) are still up, and we must keep
   * doing our thing (more on that later, when we get to it).
   *
   * First things first though: Let's do a thorough scan (once isn't a big deal) and see if anything is in fact
   * left.  An unused_obj_scan() now, in and of itself, is the right thing to do: this dtor would be invoked only
   * when thread is exiting, which the user cannot have triggered themselves, but it is an opportunity, and
   * we should take it while knowing it's not redundant or recursive or anything like that.  So we should do it;
   * and it would not be wrong/surprising, if we indeed do find things to delete/clean.  As for making it an
   * exhaustive scan, it's an anti-entropy measure; as if anything is left after, we're gonna perform a complex
   * maneuver involving spawning a new, semi-detached thread, and the less stuff to carry-over the better -- all else
   * being equal.
   *
   * As in this_thread_piggy_scan(), do a form of forgetting_shm_arena()-tail-end work before the scans; this
   * (bottom-line) may erase more stuff, so the scans don't even see it/worry about it.
   *
   * Possibly we also need to do a thing about remaining stat-struct(s); but more on that below, when we do it.
   *
   * SCENARIO 2: This ~dtor() executing after degraded_admin_thread_body() exited, ending the replacement thread.
   *
   * That method ~is the thread body, so we fully control its behavior, and its purpose is to keep running
   * (sleeping/scanning/sleeping/...) until a scan detects that no live objects remain (and, by definition,
   * no new ones may appear: user thread is gone by now).  So inactive_arenas_scan() *will* return true.
   * This ~dtor() is the very last thing (of ours) to execute w/r/t the original user thread.  All that's left
   * is (potentially) the stats-transfer into _finalized_shards.
   *
   * Can we detect whether this is the active SCENARIO?  We probably could by using a flag or something, but
   * no need: We are still an _admin, and this is still ~_admin().  So just let the same scans as in
   * SCENARIO 1 run; inactive_arenas_scan() shall return true; we'll save the stats -- which, incidentally,
   * did *not* happen in invocation 1 of ~_admin() earlier (which spawned the new thread/_admin) -- and that's it.
   * It is identical to the handling of SCENARIO 1, in the event that no live objects remained then. */

  /* Don't bother checking the flag -- just assume it is armed (thread exit is infrequent; the scan is cheap
   * when nothing is pending) -- and delete the state to do with arena(s) whose destruction is in-progress. */
  if_requested_forget_arena_related_resources(true);

  // Now the key scans.
  unused_obj_scan(true, false); // Exhaustive; not drain-mode.
  if (inactive_arenas_scan())
  {
    // Fantastic.  All done.  (There will be no new _admin object or another/2nd ~dtor() either.)

    if (!m_collection_dbs.empty()) // Don't even touch it, if no arenas registered/all forget_shm_arena()ed away.
    { /* Stats.
       * Admittedly above where we say "all done," we're overstating the situation slightly; we have to
       * do this stats-related procedure, so that the stats we've accumulated do not go *poof*.
       * See ~Thread_lcl_obj_db_client() dtor similar spot which explains it in more detail.
       *
       * At any rate: Finalized_shards takes care of it; we just have to give it whatever
       * arena/stats pairs we've got still, before they go *poof*.
       *
       * SCENARIO 1: As written just above.
       * SCENARIO 2: This path did not execute in SCENARIO 1; now it can/should.  So: as written just above.  */

      // (No need to lock.  As noted, stat-consumption TL-state walk won't see us by now.)
      for (const auto& [collection_id, collection_db] : m_collection_dbs)
      {
        /* One correction first: m_live_object_zombies is not live-updated but stored at stat-consumption time
         * (see stats_update_pre_consumption()); so at the moment it holds whatever the last consumption stored --
         * possibly a stale non-zero count (or 0, if no consumption yet).  The truth, here and now, is 0:
         * inactive_arenas_scan() returned true, hence every m_lend_tracker_pool is null -- no zombies exist,
         * and none can appear.  Without this store: a stale non-zero value would pollute the aggregated
         * zombies gauge permanently (a stat-reset would not even clear it: it is a GAUGE).  With it:
         * sharded_stats_impl() will not execute any stats_update_pre_consumption() (done only on `_admin`s),
         * will see 0 here, and will use this accurate value when summing the total.
         *
         * Caution!  Keep this in sync with stats_update_pre_consumption().
         * @todo Adapt the latter to do the below as well, then use it in both places.  Better maintainability.
         *
         * By the way -- _client does not touch that field (always 0), so it does not need equivalents of
         * stats_update_pre_consumption() or this. */
        store(&collection_db->m_stats_shard.m_live_obj.m_live_object_zombies, 0);

        stat::Finalized_shards<Shm_arena>::get_instance().stats_insert(collection_id,
                                                                       collection_db->m_stats_shard);
      }
    } // Stats.

    /* It should be clear from logs that thread was allowed to exit.  Let's not log more.
     * Also: stat::thread_end_gap_mutex() unlocks here. */
    return;
  }
  /* else:
   *
   * SCENARIO 1: If got here, it is the active SCENARIO.
   * SCENARIO 2: If got here, it is not the active SCENARIO... but now we will trigger SCENARIO 2 in the future.
   *
   * Before doing doing what we gotta do, discussion of caveat:
   *
   * By Thread_local_state_registry rules, since we're in our dtor, `this` has been removed from
   * the registry of `this`-like guys, and therefore if arena(s) still remain in *this then
   * static forgetting_shm_arena() will *never* target `this` as part of its mission.
   * However, the moment the degraded-admin thread spawned/semi-detached below invokes this_thread_obj_db(),
   * *that* guy will be targetable by static forgetting_shm_arena().  That's good and right.
   *
   * However the caveat: During the short time period between when Thread_local_state_registry, during thread exit,
   * removed us from the registry (and then immediately invoked this dtor), and when the new thread
   * calls this_thread_obj_db() below, if user manages to get Shm_arena (Ipc_arena as of this writing) deletion
   * to start -- meaning static forgetting_shm_arena() is called during this short time period -- then there will
   * be trouble.  Neither `this` nor the new this_thread_obj_db() were in the registry in that moment;
   * yet we (potentially) hold resources to-do with the Shm_arena being destroyed.  Behavior is undefined in that
   * case -- and we advertised this in forget_shm_arena() doc header -- but, e.g.,
   * if an object later reaches ref-count 0, the degraded-admin guy replacing
   * *this will try to call ~T() and deallocate its backing memory (but the vaddr space belongs
   * to a potentially now-gone arena, its getting destroyed prematurely, because one of the stakeholders was not
   * included in the forgetting_shm_arena() op => trouble).
   *
   * Actually, suppose inactive_arenas_scan() returned true above which normally means we would've `return`ed
   * by now.  Is it still a problem if forgetting_shm_arena(A) were called around then?  Answer: Probably, yes;
   * the unused_obj_scan() might have found use-count=0 object(s) from A and tried to free them -- undefined behavior
   * again.  However!  We suggest not worrying about what exactly is OK, and what isn't; it is a waste of cognitive
   * resources.  Instead we've made the just-mentioned sufficiently-conservative rule: forgetting_shm_arena(A)
   * is OK, unless thread shutdown is ongoing for a thread that has ever constructing_obj(A)ed.  That includes
   * this entire destructor body, if it would have ever touched A-related resources-that-need-actual-arena-alive.
   * Disallowing that is (1) not too onerous on the end user and (2) avoids any issues.
   *
   * However: we are not disallowing forgetting_shm_arena() while a degraded/drain thread and replacement _admin --
   * which we're about to spawn and create respectively -- is shutting down.  That is SCENARIO 2.  So is it safe?
   * Answer: Yes.  Let's see why.  SCENARIO 2 means we've returned above already: inactive_arenas_scan()
   * returned true, meaning no live objects remained in any known-to-`*this` arena at that time.  The question,
   * though, is again whether unused_obj_scan() preceding that could have touched A-related
   * resources-that-need-actual-arena-alive.  The answer is no; the whole point is -- restating this --
   * degraded_admin_thread_body() returns (letting thread exit) only once inactive_arenas_scan() returns
   * `true` in the first place.  (forgetting_shm_arena() during that time is perfect safe and good.)
   * So unused_obj_scan() will, at worst, note a bunch of m_collection_dbs per-arena entries each of which
   * lacks an m_lend_tracker_pool (inactive_arenas_scan() would have shut them all down upon detecting
   * no-live-objects left); so the scan short-circuits for each arena (if any), doing nothing: there's nothing
   * to do.  Therefore it is safe.
   *
   * It is fortunate we need not disallow the thing in the preceding paragraph: It would be difficult for an
   * end-user to know when not to let `Shm_arena` shut down relatively to degraded_admin_thread_body() thread
   * shutdowns, whose lifetimes they do not directly control (and which even depend on timer schedules).
   *
   * Another potential problem that comes from the same source -- The Gap -- has to do with
   * m_collection_dbs[A]->m_stats_shard (which is kept around even past `inactive_arenas_scan() == true`).
   * I.e., a sharded_stats*(A) (stat consumption/similar) would right now not detect A's m_stats_shard, as
   * `this` is not in Thread_local_state_registry, and the replacement _admin is not either (plus later it
   * will be in-registry but not yet have move()d m_collection_dbs[A]->m_stats_shard into itself; same prob).
   * In short: No problem: That is why we do the thread_end_gap_mutex()-lock above.  Again: see
   * thread_end_gap_mutex() doc header for all details about this.  Bottom line is it's fine to do
   * sharded_stats*() even during this teardown.
   * (End of forgetting_shm_arena() caveats discussion.)
   *
   * --- OK. So:
   *
   * Gotta start thread; and since we must exit gotta detach from it.  We can't *really* .detach() though;
   * or rather we can, but this creates a bad situation, if by the time the program is otherwise ready to
   * std::exit() normally (probably main() `return`s): Any detached threads will be forcibly killed at that point.
   * Just in and of itself that's uncool/entropy-laden; but also in actual reality the following is not-uncommon:
   * program is about to exit and starts sanely joining normal threads; *this dtor runs; an object or two remain live
   * at this time; we launch the thread below and detach; just then the program is able to free whatever it is that
   * was still live; and then immediately main() `return`s; in ~100msec (as of this writing; see
   * degraded_admin_thread_body()) it would've detected the live objects are unused; free them nicely; and exit
   * the thread peacefully.  Instead OS murders the thread before it can do that.
   *
   * So we "semi-detach": Add it to a global/static vector of `Thread`s; and when program exits we use an
   * std::atexit() handler to join those threads.  Unless there is some bug, at that point they'll either already
   * be stopped/joined (!<...>.joinable()); or will soon-enough wake up, clean up what's left, and become thus.
   * So the atexit() handler can .join() the .joinable() one(s) if any and itself return.  By C++1x rules,
   * the atexit() handler will run fully-before any of this static stuff is destroyed, because it has been constructed
   * by the time we invoke std::exit() to register the handler.
   *
   * Lastly:
   * 99.999999% of the time when launching threads we use the delightful (if I do say so myself)
   * flow::async::Single_thread_task_loop; however that (as of this writing) does not support detaching thread
   * from the object, so this is the rare case where we'll make the Thread ourselves.
   * In doing so do copy a couple of (logging- and pinning-related) niceties that would have happened there.  See
   * flow::async::Task_qing_thread for inspiration. */

  promise<void> init_done;

  auto& deg_mutex = s_state.m_degraded_admin_threads_mutex;
  auto& deg_threads = s_state.m_degraded_admin_threads;
  Sev sev_override; // (Careful: Needs to be outside the following block, so it's alive when .wait() happens.)
  {
    // std::atexit() is apparently thread-safe, but access to deg_threads isn't so let's just:
    Lock_guard<std::remove_reference_t<decltype(deg_mutex)>> deg_lock{deg_mutex};

    if (deg_threads.empty())
    {
      FLOW_LOG_INFO_LOCKED("Tl_obj_db_admin: Degraded-admin thread starting to take over from current "
                           "exiting thread; and it is the first such thread; so registering std::atexit() handler "
                           "that will, as needed, at program std::exit() (main() return) join any such thread(s) "
                           "remaining at that time.");
      std::atexit(atexit_degraded_admin_threads_join);
    }

    // Carry-over the thread-local verbosity override if any.  Rather save it here...
    sev_override = *(Log_config::this_thread_verbosity_override());

    deg_threads.emplace_back(new Thread{[&]()
    {
      // ...then apply it here in new thread.  Reminder: it can be overridden further in RAII-fashion.
      const auto sev_override_auto = Log_config::this_thread_verbosity_override_auto(sev_override);

      /* This INFO-logs a nice message.
       * (In dtor -- set_logger() cannot happen concurrently -- but we still have to log_while_locked() to get at
       * get_logger().  It's fine; the logging done by the set-nickname guy is uncontroversial; won't deadlock.) */
      log_while_locked([&](auto&& get_logger, auto&&)
      {
        Logger::this_thread_set_logged_nickname(ostream_op_string("JAd-", // Brief pfx to maybe fit into OS-thread name.
                                                                  m_thread_token), get_logger());
      });

      reset_this_thread_pinning(); // Don't inherit any strange core-affinity!  Thread shall float free.

      this_thread_obj_db() // New thread => this generates a `new Thread_lcl_obj_db_admin` and returns ptr to it.
        ->degraded_admin_thread_body(this, // Copy/move key info into new object, so as to carry on with mission...
                                     &init_done); // ...then fulfill this promise to indicate that's done.
      /* ...and then degraded_admin_thread_body() having fulfilled promise will keep running/being the new thread's
       * body -- while we will have semi-detached from it. */
    }});
  } // Lock_guard deg_lock{deg_mutex};

  init_done.get_future().wait();

  // stat::thread_end_gap_mutex() unlocks here.  The future fired => new _admin is in registry and has the stat-shards.
} // Thread_lcl_obj_db_admin::~Thread_lcl_obj_db_admin()

template<typename Shm_arena_t>
Thread_lcl_obj_db_admin<Shm_arena_t>* Thread_lcl_obj_db_admin<Shm_arena_t>::this_thread_obj_db() // Static.
{
  return s_state.m_obj_db_registry.this_thread_state();
}

template<typename Shm_arena_t>
void Thread_lcl_obj_db_admin<Shm_arena_t>::this_thread_piggy_scan() // Static.
{
  // Fast-path: conserve all resources.

  const auto obj_db = s_state.m_obj_db_registry.this_thread_state_or_null();
  if (obj_db)
  {
    obj_db->if_requested_forget_arena_related_resources(); // Do it first to potentially reduce stuff...
    obj_db->unused_obj_scan(false, false); // ...for this to scan.  Not a big thing, but it's cleaner logically.
  }
} // Thread_lcl_obj_db_admin::this_thread_piggy_scan()

template<typename Shm_arena_t>
void Thread_lcl_obj_db_admin<Shm_arena_t>::dbs_set_logger(flow::log::Logger* logger_ptr) // Static.
{
  s_state.m_obj_db_registry.set_logger(logger_ptr); // It'll propagate it to extant and future per-thread objects.
}

template<typename Shm_arena_t>
flow::log::Logger* Thread_lcl_obj_db_admin<Shm_arena_t>::set_logger(flow::log::Logger* logger_ptr)
{
  // Reminder: This is happening *not* (necessarily) in the thread matching m_thread_token.  No willy-nilly m_ access.

  const auto prev_logger_ptr = flow::log::Log_context_mt::set_logger(logger_ptr); // Do the normal thing...

  // ...but also do this stuff as advertised...
  m_skip_fast_path_verbose_logging.store(!(logger_ptr
                                           && logger_ptr->should_log(flow::log::Sev::S_TRACE, Log_component::S_SHM)),
                                         std::memory_order_relaxed);

  return prev_logger_ptr;
}

template<typename Shm_arena_t>
bool Thread_lcl_obj_db_admin<Shm_arena_t>::skip_fast_path_verbose_logging() const
{
  return m_skip_fast_path_verbose_logging.load(std::memory_order_relaxed);
}

template<typename Shm_arena_t>
template<typename Deleter_func_obj>
void Thread_lcl_obj_db_admin<Shm_arena_t>::constructing_obj(pool_id_t* lend_tracker_pool_id_ptr,
                                                            use_ct_idx_t* use_ct_idx,
                                                            Shm_arena* shm_arena,
                                                            arena_lend::stat::Obj_db_aux_pool_stats*
                                                              obj_db_aux_pool_stats,
                                                            void* addr, Deleter_func_obj&& del_func)
{
  using flow::util::Lock_guard;
  using flow::util::stat::fetch_add;
  using std::optional;
  using std::in_place;

  auto& lend_tracker_pool_id = *lend_tracker_pool_id_ptr;
  const auto collection_id = shm_arena->get_id();

  /* Our core, fast-path task here is:
   *   - Grab X = m_collection_dbs[collection_id], the Collection_db for the owner-side arena with ID collection_id.
   *   - Register a new use-count slot in X->m_lend_tracker_pool (in-SHM aux pool storing use-counts per-object).
   *   - Create an Object struct-instance, saving some basic info about the new object for later reference,
   *     such as for wehn the object is to be destroyed; and add it to LRU-ordered x->m_objs.
   *
   * The first step, in the fast-path, is read-only.  However, the first time constructing_obj() is called on
   * arena collection_id (that is the first time this thread does (that arena)->construct() in this thread),
   * X won't exist, so we must (1) create the fresh Collection_db (no objects in it yet) and (2) insert
   * it into m_collection_dbs.  Now, if you read m_collection_dbs doc header, you'll see that due to
   * certain aspects to do with concurrency and stats, (2) has to be done in a very specific way.
   * Simply doing the syntactically-nice `X = m_collection_dbs[collection_id]` and checking for X being null
   * (just-inserted in our case) is tempting but would be wrong.  Specifically, we have to write this out
   * the long way: if key=collection_id is there, great; if not, then add it.  The reason is that the
   * "add it" part -- and that part only -- has to be protected by m_collection_dbs_mutex. */

  Collection_db* collection_db;
  const auto it_collection_id_and_db = m_collection_dbs.find(collection_id);
  if (it_collection_id_and_db == m_collection_dbs.end())
  {
    // Slow-path.  Create fresh Collection_db.
    lend_tracker_pool_id = Shm_pool_offset_ptr_data_base::generate_pool_id();
    FLOW_LOG_INFO_LOCKED("Tl_obj_db_admin: For arena/collection (collection-ID [" << collection_id << "]), "
                         "first constructed-obj in this thread; will create this thread+arena's lend-tracker SHM-pool "
                         "(pool-ID [" << lend_tracker_pool_id << "]) and related tracking structures in heap.");
    collection_db
      = new Collection_db{ shm_arena,
                           lend_tracker_pool_id,
                           optional<Lend_tracker_pool>
                             {std::in_place,
                              this, &m_skip_fast_path_verbose_logging, // For its logging needs.
                              Owner_spc_impl<const Shm_arena>{*shm_arena}
                                .generate_shm_object_name(lend_tracker_pool_id),
                              util::CREATE_ONLY,
                              obj_db_aux_pool_stats, // Saved here once.  Ignored in future calls.
                              shm_arena->get_permissions()},
                           {}, {} };
    // ^-- Assumes any Shm_arena_t is an Owner_shm_pool_collection; true by arena-lend design.

    // As noted above, insert under lock -- stat-consumption might be reading key-set under same lock right now.
    {
      Lock_guard<decltype(m_collection_dbs_mutex)> lock{m_collection_dbs_mutex};
      m_collection_dbs[collection_id].reset(collection_db);
    }
  } // if (m_collection_dbs[collection_id] not found)
  else // if (m_collection_dbs[collection_id] found)
  {
    // Fast-path.
    collection_db = it_collection_id_and_db->second.get();
    lend_tracker_pool_id = collection_db->m_lend_tracker_pool_id;
  }
  /* Either way, we can now do the rest of it: register use-count, save Object.
   *
   * Subtlety: constructing_obj() cannot be invoked once inactive_arenas_scan()s start (dtor or
   * degraded_admin_thread_body()), so inactive_arenas_scan() could not have nullified
   * collection_db->m_lend_tracker_pool; so it is safe to just deref it without a null check. */
  *use_ct_idx = collection_db->m_lend_tracker_pool->use_count_new(); // (Can throw bad_alloc.)

  typename Collection_db::Object& obj = collection_db->m_objs[*use_ct_idx];
  assert((!obj.m_del_func) && "Double-used use_ct_idx somehow?  Tracking bug?  Investigate!");

  obj.m_addr = addr;
  obj.m_del_func = std::move(del_func);

  if (!skip_fast_path_verbose_logging())
  {
    FLOW_LOG_TRACE_LOCKED("Tl_obj_db_admin: For arena/collection (ID [" << collection_id << "]), constructed-obj "
                          "registered in lend-tracker SHM-pool and related tracking structures in heap; use count = 1; "
                          "addr = [" << addr << "]; use-count index in lend-tracker-pool = [" << *use_ct_idx << "]; "
                          "lend-tracker-pool ID = [" << lend_tracker_pool_id << "].");
  }
  // else { Skip ~any cycle use on logging along fast-path. }

  { // Stats.
    auto& stats_shard = collection_db->m_stats_shard;
    auto& owner_obj_stats = stats_shard.m_owner_obj;
    fetch_add(&owner_obj_stats.m_construct_count, 1);
    fetch_add(&owner_obj_stats.m_live_handle_groups, 1);
    fetch_add(&stats_shard.m_live_obj.m_live_objects, 1);
    // Reminder: HI_WMARKs, when sharding as we are, are computed at stat-consumption time.  No update here.
  }
} // Thread_lcl_obj_db_admin::constructing_obj()

template<typename Shm_arena_t>
void Thread_lcl_obj_db_admin<Shm_arena_t>::disposing_obj(collection_id_t collection_id, use_ct_idx_t use_ct_idx)
{
  using flow::util::stat::fetch_add;
  using flow::util::stat::fetch_sub;

  Own<Collection_db>& collection_db = m_collection_dbs[collection_id];
  assert(collection_db
         && "This thread did constructing_obj(), and we should only be called from same thread, "
              "so how could the arena not have an entry yet?");

  auto use_ct_idx_copy = use_ct_idx;

  /* Subtlety: disposing_obj() cannot be invoked once inactive_arenas_scan()s start (dtor or
   * degraded_admin_thread_body()), so inactive_arenas_scan() could not have nullified
   * collection_db->m_lend_tracker_pool; so it is safe to just deref it without a null check. */

  if (collection_db->m_lend_tracker_pool->use_count_dec_admin(&use_ct_idx_copy) == 0)
  {
    auto& objs = collection_db->m_objs;
    const auto obj_it = objs.find(use_ct_idx);
    assert(obj_it != objs.end());
    const auto& obj = obj_it->second;

    if (!skip_fast_path_verbose_logging())
    {
      FLOW_LOG_TRACE_LOCKED("Tl_obj_db_admin: For arena/collection (ID [" << collection_id << "]), constructed-obj "
                            "disposer executed in same thread as we cted it; upon decrementing use count it reached 0; "
                            "we shall immediately delete it and remove it from in-heap tracking and removed it from "
                            "lend-tracker SHM-pool; addr = [" << obj.m_addr << "]; use-count index in "
                            "lend-tracker-pool = [" << use_ct_idx << "]; "
                            "lend-tracker-pool ID = [" << collection_db->m_lend_tracker_pool_id << "].");
    }
    // else { Skip ~any cycle use on logging along fast-path. }

    obj.m_del_func(obj.m_addr, collection_db->m_shm_arena);
    objs.erase(obj_it);
  } // if (lend_tracker_pool.use_count_dec_admin(use_ct_idx) == 0)
  // else { Life goes on for it for now. }

  { // Stats.
    auto& stats_shard = collection_db->m_stats_shard;
    auto& owner_obj_stats = stats_shard.m_owner_obj;

    fetch_add(&owner_obj_stats.m_disposer_count, 1);
    fetch_sub(&owner_obj_stats.m_live_handle_groups, 1);

    if (use_ct_idx_copy == 0) // This occurs <=> .use_count_dec_admin() returned 0.  So: we destroyed object.
    {
      fetch_add(&owner_obj_stats.m_destroy_count, 1);
      fetch_add(&stats_shard.m_owner_obj_arena_lend.m_sync_destroy_count, 1);
      fetch_sub(&stats_shard.m_live_obj.m_live_objects, 1);
    }
  }
} // Thread_lcl_obj_db_admin::disposing_obj()

template<typename Shm_arena_t>
void Thread_lcl_obj_db_admin<Shm_arena_t>::lending_obj(collection_id_t collection_id, use_ct_idx_t use_ct_idx)
{
  using flow::util::stat::fetch_add;

  Own<Collection_db>& collection_db = m_collection_dbs[collection_id];
  assert(collection_db
         && "This thread did constructing_obj(), and we should only be called from same thread, "
              "so how could the arena not have an entry yet?");

  /* Subtlety: lending_obj() cannot be invoked once inactive_arenas_scan()s start (dtor or
   * degraded_admin_thread_body()), so inactive_arenas_scan() could not have nullified
   * collection_db->m_lend_tracker_pool; so it is safe to just deref it without a null check. */

  const auto use_ct = collection_db->m_lend_tracker_pool->use_count_inc(use_ct_idx);

  if (!skip_fast_path_verbose_logging())
  {
    FLOW_LOG_TRACE_LOCKED("Tl_obj_db_admin: For arena/collection (ID [" << collection_id << "]), constructed-obj "
                          "being lent in same thread as we cted it; upon incrementing use count it reached "
                          "[" << use_ct << "]; addr = [" << collection_db->m_objs[use_ct_idx].m_addr << "]; use-count "
                          "index in lend-tracker-pool = [" << use_ct_idx << "]; "
                          "lend-tracker-pool ID = [" << collection_db->m_lend_tracker_pool_id << "].");
  }
  // else { Skip ~any cycle use on logging along fast-path. }

  { // Stats.
    fetch_add(&collection_db->m_stats_shard.m_lend_obj.m_lend_count, 1);
  }
} // Thread_lcl_obj_db_admin::lending_obj()

template<typename Shm_arena_t>
void Thread_lcl_obj_db_admin<Shm_arena_t>::unused_obj_scan(bool exhaustive, bool draining)
{
  using arena_lend::stat::Zombie_obj_reaper_stats;
  using flow::util::stat::fetch_add;
  using flow::util::stat::fetch_sub;
  using std::vector;
  using std::atomic;

  // Reminder: This entire method is "fast-path"; we need to zealously conserve compute (unless `exhaustive`).

  vector<use_ct_idx_t> unused_ct_idxs; // (Try to optimize a bit by declaring these non-ptrs/refs on the outside.)
  typename decltype(Collection_db::m_objs)::Iterator obj_it;
  typename decltype(Collection_db::m_objs)::Reverse_iterator obj_r_it;
  for (const auto& collection_id_and_db : m_collection_dbs)
  {
    /* Aliases: FLOW_LOG_..._LOCKED() messages below may not name structured bindings (C++17).
     * @todo Move to C++20 => Use bindings after all for pithiness.  (Some C++17-mode compilers already OK; not all.)
     *       Remove the @warning about this from doc header of FLOW_LOG_WARNING_LOCKED() (in Flow, not Flow-IPC). */
    const auto collection_id = collection_id_and_db.first;
    const auto& collection_db = collection_id_and_db.second;

    auto& lend_tracker_pool_opt = collection_db->m_lend_tracker_pool;
    if (!lend_tracker_pool_opt) // inactive_arenas_scan() may have nullified it.
    {
      continue; // It's a dead Collection_db, kept-around only for m_stats_shard.  Don't count the scan (as doc-ed).
    }
    // else
    auto& lend_tracker_pool = *lend_tracker_pool_opt;

    auto& stats_shard = collection_db->m_stats_shard;
    auto& zombie_obj_reaper_stats = draining ? stats_shard.m_zombie_obj_reaper_drain
                                             : stats_shard.m_zombie_obj_reaper_main;
    { // Stats.  Get this one out of the way before any short-circuiting.
      fetch_add(&zombie_obj_reaper_stats.m_scans, 1);
      exhaustive && fetch_add(&zombie_obj_reaper_stats.m_exhaustive_scans, 1);
    }

    auto& objs = collection_db->m_objs;

    signed int n_unused; // Intentionally signed, as it can even become negative (see below near end of hint loop).
    if (exhaustive)
    {
      n_unused = std::numeric_limits<decltype(n_unused)>::max();
    }
    else
    {
      n_unused = static_cast<decltype(n_unused)>(lend_tracker_pool.n_unused());
      if (n_unused == 0)
      {
        /* An optimization.  Technically emit_unused_ct_idx_hints() can yield stuff nevertheless, and is a pretty
         * quick constant-time search through the hints array; but if right this moment the count is 0 then don't
         * waste a single cycle (that there is non-zero chance hints array will have something if we look is true
         * but unlikely/not worth it). */
        continue;
      }
      // else: There *must* be some unused live objs.
    }
    /* We check the hints array first for speed; then if n_unused still shows there shall be more,
     * do the FIFO-order search of objs. */

    auto n_unused_saved = n_unused; // For logging/stats only.
    enum { HINTS_SUFFICIENT, FIFO_SUFFICIENT, EXHAUSTED } scan_outcome; // Ditto.
    /* Most members of zombie_obj_reaper_stats are not each ++ing 0-1 per-arena-per-scan but per-arena can each be
     * incremented any number of times or not at all, depending on n_unused, the hint array's contents, and what's
     * at the front of .m_objs.  So instead of ++ing the actual `atomic`s each time, we cache the total here
     * and then += the non-zero totals onto the appropriate `atomic`s.  This cache starts at all-zeroes. */
    Zombie_obj_reaper_stats::Events<false> batched_stats; // false => non-atomic members.
    unsigned int batched_destroy_count = 0;

    lend_tracker_pool.emit_unused_ct_idx_hints(&unused_ct_idxs); // (It is cleared at start.)
    batched_stats.m_hints_checked += unused_ct_idxs.size();
    for (auto unused_ct_idx_possibly : unused_ct_idxs)
    {
      obj_it = objs.find(unused_ct_idx_possibly);
      if (obj_it == objs.end())
      {
        // Hint did not work out (perhaps we got it in an earlier pass).  Anyway it is a hint; no harm done.
        if (!skip_fast_path_verbose_logging())
        {
          FLOW_LOG_TRACE_LOCKED("Tl_obj_db_admin: For arena/collection (ID [" << collection_id << "]), piggy-scan "
                                "got hint about live object with use-count=0; but we'd already deleted it; it happens; "
                                "(former) use-count index in lend-tracker-pool = [" << unused_ct_idx_possibly << "]; "
                                "lend-tracker-pool ID = [" << collection_db->m_lend_tracker_pool_id << "].");
        }
        // else { Skip ~any cycle use on logging along fast-path. }

        ++batched_stats.m_hints_stale;
        continue;
      }
      // else if: An object at that slot is live.  Is its use-count zero right now though?

      auto use_ct_idx_copy = unused_ct_idx_possibly;
      if (!lend_tracker_pool.use_count_return_if_unused(&use_ct_idx_copy))
      {
        // Nope.
        if (!skip_fast_path_verbose_logging())
        {
          FLOW_LOG_TRACE_LOCKED("Tl_obj_db_admin: For arena/collection (ID [" << collection_id << "]), piggy-scan "
                                "got hint about live object with use-count=0; and one is live at that index; but it "
                                "must be another object by now, as its use-count is >0; "
                                "(former) use-count index in lend-tracker-pool = [" << unused_ct_idx_possibly << "]; "
                                "lend-tracker-pool ID = [" << collection_db->m_lend_tracker_pool_id << "].");
        }
        // else { Skip ~any cycle use on logging along fast-path. }

        /* @todo As noted in the doc header for this stat, this shouldn't actually be possible.  We are nevertheless
         * being defensive and not crashing out if it does somehow happen.  Eventually this should be revisited;
         * a WARNING might be in order, at least; maybe something more drastic.  For now though we'll just count
         * it as a canary stat (if it's non-zero, look into it, something is not how we expected. */

        ++batched_stats.m_hints_found_resurrected_zombie;
        continue;
      }
      /* else: Found one!  It reached 0 (end state), and it is live, and we are the only ones who can remove it, so
       * there's no danger of concurrent competition. */

      auto& obj = obj_it->second;

      if (!skip_fast_path_verbose_logging())
      {
        FLOW_LOG_TRACE_LOCKED("Tl_obj_db_admin: For arena/collection (ID [" << collection_id << "]), piggy-scan "
                              "got hint about live object with use-count=0; and one is live at that index; and the "
                              "use-count *is* zero; we shall immediately delete it from in-heap tracking/removed it "
                              "from lend-tracker SHM-pool; addr = [" << obj.m_addr << "]; use-count index in "
                              "lend-tracker-pool = [" << unused_ct_idx_possibly << "]; "
                              "lend-tracker-pool ID = [" << collection_db->m_lend_tracker_pool_id << "].");
      }
      // else { Skip ~any cycle use on logging along fast-path. }

      obj.m_del_func(obj.m_addr, collection_db->m_shm_arena);
      objs.erase(obj_it);
      ++batched_destroy_count;

      --n_unused;
      // Might even become negative (hints array and N-unused metadata are not modified atomically in concert)!
    } // for (auto unused_ct_idx_possibly : unused_ct_idxs)

    if (n_unused <= 0)
    {
      scan_outcome = HINTS_SUFFICIENT; // The search-avoiding hints phase was [more than] sufficient to be done.
    }
    else // if (n_unused > 0)
    {
      /* As explained in class doc header Impl section: search from oldest to newest to hopefully maximize
       * speed of getting n_unused down to 0 (though it might already be at 0 even!). */
      obj_r_it = objs.oldest();

      /* scan_outcome shall end up either EXHAUSTED (unlikely/impossible unless `exhaustive`) or FIFO_SUFFICIENT.
       * We'll be pessimistic for tactical convenience and start with assuming the former; but if
       * (likely/guaranteed unless `exhaustive`) we see `n_unused == 0` (not the case right at the moment), then
       * we'll set scan_outcome otherwise. */
      scan_outcome = EXHAUSTED;
      bool done = obj_r_it == objs.past_newest();
      while (!done) // First time around, `done` can only be `true` due to EXHAUSTED.  After that -- see below.
      {
        ++batched_stats.m_fifo_objs_checked;

        auto unused_ct_idx_possibly = obj_r_it->first;
        auto use_ct_idx_copy = unused_ct_idx_possibly;
        if (!lend_tracker_pool.use_count_return_if_unused(&use_ct_idx_copy))
        {
          done = ((++obj_r_it) == objs.past_newest()); // n_unused unchanged, so `true` would be due to EXHAUSTED.

          ++batched_stats.m_fifo_objs_live;
          continue; // Just a live object.  Nothing to see here for now.
        }
        // else if (found and returned use-count=0): Much like in the above hints-array pass:

        /* A reverse iterator actually stores an iterator at base, so we'll erase where the reverse iterator is
         * pointing.  To do this, we need to convert to a regular iterator and go back one.  The reverse iterator
         * obj_r_it will point to "next" automatically after erasure. */
        auto obj_it_to_erase = obj_r_it.base();
        --obj_it_to_erase;

        if ((--n_unused) == 0)
        {
          done = true; scan_outcome = FIFO_SUFFICIENT;
        }
        else
        {
          done = (obj_r_it == objs.past_newest()); // n_unused still non-zero, so `true` would be due to EXHAUSTED.
        }
        // Loop-end condition + scan_outcome handled; back to the erasure: Actually destroy obj.

        auto& obj = obj_it_to_erase->second;

        if (!skip_fast_path_verbose_logging())
        {
          FLOW_LOG_TRACE_LOCKED("Tl_obj_db_admin: For arena/collection (ID [" << collection_id << "]), piggy-scan "
                                "was performing oldest-to-newest scan phase through live objects; and one's "
                                "use-count *is* zero; we shall immediately delete it from in-heap tracking/removed it "
                                "from lend-tracker SHM-pool; addr = [" << obj.m_addr << "]; use-count index in "
                                "lend-tracker-pool = [" << unused_ct_idx_possibly << "]; "
                                "lend-tracker-pool ID = [" << collection_db->m_lend_tracker_pool_id << "].");
        }
        // else { Skip ~any cycle use on logging along fast-path. }

        obj.m_del_func(obj.m_addr, collection_db->m_shm_arena);
        objs.erase(obj_it_to_erase);
        ++batched_destroy_count;
      } // while (!done)

      if (!skip_fast_path_verbose_logging())
      {
        FLOW_LOG_TRACE_LOCKED("Tl_obj_db_admin: For arena/collection (ID [" << collection_id << "]), piggy-scan "
                              "was performing oldest-to-newest scan phase through live objects; "
                              "pre-scan unused-count was [" << n_unused_saved << "] (exhaustive scan? = "
                              "[" << exhaustive << "]); we then found enough to decrement that temp-count-copy to "
                              "[" << n_unused << "]; delta = [" << (n_unused_saved - n_unused) << "]; "
                              "outcome{HINTS_SUFFICIENT|FIFO_SUFFICIENT|EXHAUSTED} = [" << int(scan_outcome) << "].");
      }
      // else { Skip ~any cycle use on logging along fast-path. }
    } // if (n_unused <= 0) [after hints phase] [but it may have decreased inside {body}]

    /* Reminder: actual lend_tracker_pool.n_unused() may well have changed (increased) concurrently to the above,
     * even as we decrement it at the same time as we do so with our local n_unused.
     * We define our workload off a snapshot of it (our n_unused), to prevent any pathological strangeness
     * wherein we keep scanning due to continuous concurrent n_unused() growth.  It's fine: we'll get any new ones
     * next time this->unused_obj_scan() executes. */

    { /* Stats.  [if n_unused was not originally 0]
       * These are 0-1 per-scan per-arena (and we handled .m_scans already). */
      if (!exhaustive)
      {
        /* An `exhaustive` scan has no estimate N -- in that mode n_unused_saved is a huge sentinel -- and
         * recording that would only pollute the histogram, whose purpose is tuning against typical
         * estimate values (see its doc header). */
        zombie_obj_reaper_stats.m_histo_zombie_ct_estimate.record_value(n_unused_saved);
      }
      {
        atomic<uint64_t>* tgt_ctr = {}; // (Avoid no-init (false) warning by some compilers.)
        switch (scan_outcome)
        {
        case HINTS_SUFFICIENT: tgt_ctr = &zombie_obj_reaper_stats.m_hints_sufficient_scans; break;
        case FIFO_SUFFICIENT: tgt_ctr = &zombie_obj_reaper_stats.m_fifo_scans; break;
        case EXHAUSTED: tgt_ctr = &zombie_obj_reaper_stats.m_fifo_exhausted_scans;
        /* EXHAUSTED is the normal outcome of an `exhaustive` scan: its sentinel n_unused cannot reach 0.
         * For an estimate-driven scan it should not be possible; hence this stat exceeding m_exhaustive_scans
         * is a canary.  See doc header for m_fifo_exhausted_scans. */
        }
        fetch_add(tgt_ctr, 1);
      }

      // These can be incremented by ~anything.  Hence we cached the totals, and now we can register non-zeroes.
      (batched_stats.m_hints_checked == 0)
        || fetch_add(&zombie_obj_reaper_stats.m_events.m_hints_checked,
                     batched_stats.m_hints_checked);
      (batched_stats.m_hints_stale == 0)
        || fetch_add(&zombie_obj_reaper_stats.m_events.m_hints_stale,
                     batched_stats.m_hints_stale);
      (batched_stats.m_hints_found_resurrected_zombie == 0)
        || fetch_add(&zombie_obj_reaper_stats.m_events.m_hints_found_resurrected_zombie,
                     batched_stats.m_hints_found_resurrected_zombie);
      (batched_stats.m_fifo_objs_checked == 0)
        || fetch_add(&zombie_obj_reaper_stats.m_events.m_fifo_objs_checked,
                     batched_stats.m_fifo_objs_checked);
      (batched_stats.m_fifo_objs_live == 0)
        || fetch_add(&zombie_obj_reaper_stats.m_events.m_fifo_objs_live,
                     batched_stats.m_fifo_objs_live);
      if (batched_destroy_count != 0)
      {
        fetch_add(&stats_shard.m_owner_obj.m_destroy_count, batched_destroy_count);
        draining && fetch_add(&stats_shard.m_owner_obj_arena_lend.m_drain_destroy_count, batched_destroy_count);
        fetch_sub(&stats_shard.m_live_obj.m_live_objects, batched_destroy_count);
      }
    } // Stats.
  } // for ([collection_id, collection_db] : m_collection_dbs)
} // Thread_lcl_obj_db_admin::unused_obj_scan()

template<typename Shm_arena_t>
bool Thread_lcl_obj_db_admin<Shm_arena_t>::inactive_arenas_scan()
{
  using flow::util::Lock_guard;
  using std::vector;

  /* Let's survey the situation after deleting every unused object.  As promised -- two tasks:
   *   - For each no-live-objects arena forever (empty arena forever), delete the Lend_tracker_pool.
   *   - See if they're all empty forever and return whether it is so. */
  bool no_live_objs_left = true;
  for (const auto& collection_id_and_db : m_collection_dbs)
  {
    // (See similar C++17/20 note higher up in similar situation.)
    const auto collection_id = collection_id_and_db.first;
    const auto& collection_db = collection_id_and_db.second;

    if (!collection_db->m_objs.empty())
    {
      // Live objects persist for this collection_id (arena).  Next.
      no_live_objs_left = false;
      continue;
    }
    /* else: No objects left for this collection_id (arena).  What does it mean?  It means, of objects constructed
     * from *this* thread, in *this* arena, all objects have now been accounted-for and deleted.  The key thing
     * is thread is inactive, meaning exiting or degraded-admin; so by definition there's nothing left
     * for *this to do:
     *   - No more constructing relevant to us shall occur.
     *   - Nothing to destroy remains.
     *   - (Furthermore, no `Thread_lcl_obj_db_client`s shall ever touch the Lend_tracker_pool.)
     * So clean up the remaining resource (the Lend_tracker_pool).  Do not however delete surrounding
     * Collection_db object from the m_collection_dbs map; its m_stats_shard must live until
     * forget_shm_arena(). */

    { // See m_collection_dbs_mutex doc header for explanation of this lock.
      Lock_guard<decltype(m_collection_dbs_mutex)> lock{m_collection_dbs_mutex};

      auto& lend_tracker_pool_opt = collection_db->m_lend_tracker_pool;
      if (lend_tracker_pool_opt)
      {
        FLOW_LOG_INFO_LOCKED
          ("Tl_obj_db_admin: Thread exiting; after exhaustive scan of live objects constructed in this "
           "thread none remain for arena/collection (ID [" << collection_id << "]); hence "
           "deleting lend-tracker SHM-pool (leaving stats alone, as they keep counting).  If we can do "
           "this for *all* relevant arenas/IDs, this thread can exit peacefully.  Else will spawn, or "
           "keep running, degraded-admin thread.");
        lend_tracker_pool_opt.reset();
      }
    }

    /* Note: Lend_tracker_pool (admin-type) dtor performs removal of pool name from file-system.
     *
     * Also it makes it so that any Lend_tracker_pool (that is, the client-mode ones in potentially several/many
     * extant `Lend_tracker_pool`s in this and borrower process(es)) accessing the same SHM-pool shall yield
     * `.dead() == true`.  Thread_lcl_obj_db_client has logic that will detect this reasonably soon and destroy
     * *those* `Lend_tracker_pool`s too.  In the bird's eye view of this system it means all SHM-pool-handles
     * to that pool-name will soon-enough be closed; and the underlying SHM-pool's RAM shall thus be returned
     * to the system for general use.  Hence we just triggered a system-wide procedure that averts leaking the
     * relevant SHM-pools' RAM over time.
     *
     * Without that, what might happen is (e.g.) a borrower with a long up-time,
     * that has had to tear down/reconnect many sessions over time to a lender application that has restarted many times
     * during said long up-time, might build up many no-longer-used but RAM-taking lend-tracker-SHM-pools
     * (via client-mode Lend_tracker_pool objects).  Thus would manifest a memory leak. */
  } // for ([collection_id, collection_db] : m_collection_dbs)

  return no_live_objs_left;
} // Thread_lcl_obj_db_admin::inactive_arenas_scan()

template<typename Shm_arena_t>
template<typename On_done_func>
bool Thread_lcl_obj_db_admin<Shm_arena_t>::forgetting_shm_arena(collection_id_t collection_id, // Static.
                                                                flow::log::Logger* logger_ptr,
                                                                On_done_func&& on_done_func)
{
  using stat::Finalized_shards;
  using flow::log::Log_context_mt;
  using flow::util::this_thread_unique_token;

  FLOW_LOG_SET_CONTEXT(logger_ptr, Log_component::S_SHM); // For synchronous logging only (as advertised).

  bool done_synchronously = true;
  /* If by the end of the function done_synchronously is still true, then we've fully forgotten what was required
   * to forget -- if there was anything in the first place -- and did not memorize on_done_func. */

  s_state.m_obj_db_registry.while_locked([&](const auto& obj_db_per_thread)
  {
    if (obj_db_per_thread.empty())
    {
      assert(done_synchronously);
      return; // No extant thread-local obj-DBs.  Nothing to forget among them then... yay.
    }
    // else
    done_synchronously = false; // ...but it becomes true if `last_one` clause below executes.

    s_state.m_arenas_to_forget_map.while_locked([&](auto* arenas_to_forget_map_ptr)
    {
      auto& arenas_to_forget_map = *arenas_to_forget_map_ptr;

      Arena_forget_progress& arena_forget_progress = arenas_to_forget_map[collection_id];
      auto& db_set = arena_forget_progress.m_dbs_that_still_must_forget;
      auto& saved_on_done_func = arena_forget_progress.m_on_done_func;

      assert(db_set.empty() && saved_on_done_func.empty()
             && "forgetting_shm_arena() called twice on the same owner+collection ID?  Bug?");

      saved_on_done_func = std::move(on_done_func);

      for (const auto& [obj_db, nil] : obj_db_per_thread)
      {
        db_set.insert(obj_db);
        /* It means: "Yo, `obj_db`, if you've got any stuff pertaining to arena collection_id, remove/deinit it;
         * but either way you-dun-good, so remove yourself from s_state.m_arenas_to_forget_map[collection_id].
         * And if that makes s_state.m_arenas_to_forget_map[collection_id] empty (you're the last one to have
         * dun-good), then finish what we started: execute, and forget, m_on_done_func()." */
      }
    }); // s_state.m_arenas_to_forget_map.while_locked()

    /* For each extant _admin set up flag to trigger forgetting-of-arena in that guy's thread opportunistically.
     * Except that if we are in the thread corresponding to one of the `_admin`s, we can do that part synchronously.
     * If that happens, *and* it's the only one, then we are done and therefore done_synchronously=true. */
    for (const auto& obj_db_and_nil : obj_db_per_thread)
    {
      // (See similar C++17/20 note higher up in similar situation.)
      const auto obj_db = obj_db_and_nil.first;

      if (this_thread_unique_token() == obj_db->m_thread_token)
      {
        s_state.m_arenas_to_forget_map.while_locked([&](auto* arenas_to_forget_map_ptr)
        {
          auto& arenas_to_forget_map = *arenas_to_forget_map_ptr;

          const auto it_arena_and_progress = arenas_to_forget_map.find(collection_id);
          assert((it_arena_and_progress != arenas_to_forget_map.end())
                 && "What could have possibly already handled this thread's entry that we just inserted?  Bug?");

          auto& arena_forget_progress = it_arena_and_progress->second;
          auto& db_set = arena_forget_progress.m_dbs_that_still_must_forget;
#ifndef NDEBUG
          const bool ok = 1 ==
#endif
          db_set.erase(obj_db);
          assert(ok && "What could have possibly already handled this thread's entry that we just inserted?  Bug?");

          const bool last_one = db_set.empty();
          if (last_one)
          {
            FLOW_LOG_INFO("Tl_obj_db_admin: Local arena/collection [" << collection_id << "] is being "
                          "forgotten; during the request opportunistically scanned caller thread's per-thread "
                          "in-heap structures, and this was the last (only) such per-thread structure that "
                          "needed handling; so this immediately completes the "
                          "forgetting of that arena/collection among the obj-DB per-thread admins; "
                          "no async work is left.");

            arenas_to_forget_map.erase(it_arena_and_progress);
            done_synchronously = true;
            /* It is now empty through-and-through.  arena_forget_progress is gone, so the saved on_done_func
             * is gone as part of it. */
          }
          // else {}

          obj_db->forget_shm_arena(collection_id, last_one);
        }); // s_state.m_arenas_to_forget_map.while_locked()
      } // if (this_thread_unique_token() == obj_db->m_thread_token)
      else // if (this thread unique-token != obj_db->m_thread_token)
      {
        FLOW_LOG_TRACE("Tl_obj_db_admin: In outside thread setting arena-forgetting-requested flag on "
                       "(local arena/collection [" << collection_id << "] "
                       "is being forgotten) for per-thread admin for thread with unique-token "
                       "[" << obj_db->m_thread_token << "].  Next time that thread polls for that flag being "
                       "true, it will forget/deinit the stuff relevant to that arena/collection.");
        obj_db->m_forget_resources_requested_poll_flag.arm_next_poll();
      } // else if (this thread unique-token != obj_db->m_thread_token)
    } // for ([obj_db, nil] : obj_db_per_thread)
  }); // s_state.m_obj_db_registry.while_locked()

  { /* Stats.
     * Finalized_shards doc headers explains this.  See also _client::forgetting_shm_arena().
     *
     * get_instance() is always safe (immortal leaked singleton; see its doc header). */
    Finalized_shards<Shm_arena>::get_instance().stats_erase(collection_id);
  }

  return done_synchronously;
  /* If false: At least one non-this-thread _admin's flag was armed, so one such thread/_admin will
   *   fire the saved on_done_func.  So it's not done synchronously.
   * If true, one of these happened:
   *   - There were no extant `_admin`s at all... so nothing to do... so no _admin's flag was armed, so
         it's all done synchronously, vacuously.
   *   - There was at least one -- this's thread's _admin -- and we had it do its share (forget_shm_arena())
   *     synchronously above, and when we did that, that was what finished the procedure (it was the last
   *     _admin that had forgetting to do).  So it's all done synchronously. */
} // Thread_lcl_obj_db_admin::forgetting_shm_arena()

template<typename Shm_arena_t>
void Thread_lcl_obj_db_admin<Shm_arena_t>::if_requested_forget_arena_related_resources(bool assume_requested)
{
  using flow::log::Log_context_mt;
  using std::vector;

  // Avoid logging work, or any unneeded work of any kind, in the fast-path....

  if ((!assume_requested) && (!m_forget_resources_requested_poll_flag.poll_armed()))
  {
    return;
  }
  /* else if (it was requested) || assume_requested: We will do it.
   * Unless assume_requested: It is now re-marked as not-requested. */

  // Non-fast-path is in effect (can log, etc.).

  vector<decltype(Arena_forget_progress::m_on_done_func)> on_done_funcs_to_execute;
  s_state.m_arenas_to_forget_map.while_locked([&](auto* arenas_to_forget_map_ptr)
  {
    auto& arenas_to_forget_map = *arenas_to_forget_map_ptr;
    vector<collection_id_t> finished_collection_ids;

    for (auto& collection_id_and_progress : arenas_to_forget_map)
    {
      // (See similar C++17/20 note higher up in similar situation.)
      const auto collection_id = collection_id_and_progress.first;
      auto& arena_forget_progress = collection_id_and_progress.second;

      auto& db_set = arena_forget_progress.m_dbs_that_still_must_forget;
      if (db_set.erase(this) == 0)
      {
        continue; // This collection/arena being forgotten is (no longer?) dependent on our deleting it from *this.
      }
      // else

      const bool last_one = db_set.empty();

      forget_shm_arena(collection_id, last_one);

      if (last_one)
      {
        FLOW_LOG_INFO_LOCKED("Tl_obj_db_admin: Local arena/collection [" << collection_id << "] is being "
                             "forgotten; opportunistically scanned our per-thread in-heap structures, and this was "
                             "the last such per-thread structure that needed handling; so this completes the "
                             "forgetting of that arena/collection among the obj-DB per-thread admins.");
        finished_collection_ids.push_back(collection_id);

        on_done_funcs_to_execute.emplace_back(std::move(arena_forget_progress.m_on_done_func));
        arena_forget_progress.m_on_done_func.clear(); // Just in case move() didn't do it.
      }
    } // for ([collection_id, arena_forget_progress] : arenas_to_forget_map)

    // Clean out the entries we made empty (for which we'll run on_done_func() just below, finishing the exercise).
    for (const auto collection_id : finished_collection_ids)
    {
      arenas_to_forget_map.erase(collection_id);
    }
  }); // s_state.m_arenas_to_forget_map.while_locked()

  for (auto& on_done_func : on_done_funcs_to_execute)
  {
    on_done_func(static_cast<const Log_context_mt*>(this));
  }
} // Thread_lcl_obj_db_admin::if_requested_forget_arena_related_resources()

template<typename Shm_arena_t>
void Thread_lcl_obj_db_admin<Shm_arena_t>::forget_shm_arena(collection_id_t collection_id, bool last_one)
{
  using flow::util::Lock_guard;

  // Slow-path (log freely, etc.).

  const auto it_collection_id_and_db = m_collection_dbs.find(collection_id);
  if (it_collection_id_and_db == m_collection_dbs.end())
  {
    FLOW_LOG_TRACE_LOCKED("Tl_obj_db_admin: Local arena/collection [" << collection_id << "] is being "
                          "forgotten; so will delete all live objects (even if use-count>0); "
                          "close any relevant lend-tracker SHM-pool admin handles (+unlink name) "
                          "we've opened; and forget related tracking structures in heap... but we have none so no-op; "
                          "are we the last per-thread obj-DB that was remaining to do this? = [" << last_one << "].");
    return;
  }
  // else

  FLOW_LOG_INFO_LOCKED("Tl_obj_db_admin: Local arena/collection [" << collection_id << "] is being "
                       "forgotten; so will delete all live objects (even if use-count>0); "
                       "close any relevant lend-tracker SHM-pool admin handles (+unlink name) "
                       "we've opened; and forget related tracking structures in heap; are we the last "
                       "per-thread obj-DB that was remaining to do this? = [" << last_one << "].");

  const auto collection_db = std::move(it_collection_id_and_db->second); // See below for reason for the move-ct.
  for (const auto& use_ct_idx_and_obj : collection_db->m_objs)
  {
    // (See similar C++17/20 note higher up in similar situation.)
    const auto use_ct_idx = use_ct_idx_and_obj.first;
    const auto& obj = use_ct_idx_and_obj.second;

    /* Subtlety: If an iteration of this loop runs, then inactive_arenas_scan() could not have nullified
     * collection_db->m_lend_tracker_pool yet; so it is safe to just deref it without a null check. */

    const auto use_ct_informational = collection_db->m_lend_tracker_pool->use_count(use_ct_idx);
    if (use_ct_informational == 0)
    {
      FLOW_LOG_TRACE_LOCKED("Tl_obj_db_admin: For arena/collection (ID [" << collection_id << "]), while forgetting "
                            "it, deleting live object with use-count=0; addr = [" << obj.m_addr << "]; use-count "
                            "index in lend-tracker-pool = [" << use_ct_idx << "]; "
                            "lend-tracker-pool ID = [" << collection_db->m_lend_tracker_pool_id << "].");
    }
    else
    {
      FLOW_LOG_WARNING_LOCKED("Tl_obj_db_admin: For arena/collection (ID [" << collection_id << "]), while forgetting "
                              "it, deleting live object with non-zero use-count [" << use_ct_informational << "]; "
                              "perhaps opposing process crashed/session went down/etc.  "
                              "Addr = [" << obj.m_addr << "]; use-count index in "
                              "lend-tracker-pool = [" << use_ct_idx << "]; "
                              "lend-tracker-pool ID = [" << collection_db->m_lend_tracker_pool_id << "].");
    }

    obj.m_del_func(obj.m_addr, collection_db->m_shm_arena);
    // One big reason we can't have destroyed *m_shm_arena ^-- yet!

    /* Stats: Though we are indeed destroying objects here, no-op stats-wise.  We're about to erase and destroy
     * the Collection_db containing the m_stats_shard, so it'll go into the aether anyway.  This does make sense
     * from the end user's point of view too: if the arena is being forgotten, the actual Shm_arena is just about
     * dead... so they'd have to way of querying for its per-arena stats. */
  } // for ([use_ct_idx, obj] : collection_db->m_objs)

  /* Do the mutex-y song-and-dance for what is, effectively, `m_collection_dbs.erase(it_collection_id_and_db);`,
   * but safe + performant.
   *
   * Basically we want to just `m_collection_dbs.erase(...);`.  That would work fine; as part of
   * the erasure the Collection_db would get destroyed, so the Lend_tracker_pool in there would get destroyed.
   * However, if you read m_collection_dbs doc header,  you'll see that due to
   * certain aspects to do with concurrency and stats, the key-erase itself has to be done under mutex-lock.
   * Easy enough -- just add the `Lock_guard` -- but we do a bit better still: we separate the key-erasure
   * (needs lock) from the object-deletion.  It reduces the critical section's size.  Due to the frequency of
   * the ops involved this is probably not critical important perf-wise, but we prefer to stay disciplined. */
  {
    Lock_guard<decltype(m_collection_dbs_mutex)> lock{m_collection_dbs_mutex};
    m_collection_dbs.erase(it_collection_id_and_db);
  }
  /* *collection_db shall now be destroyed in peace, as the Own<Collection_db> goes out of scope.  That's why
   * we move-cted it into the Own<Collection_db> on the stack above.
   *
   * Note: Lend_tracker_pool (admin-type) dtor performs removal of pool name from file-system.
   *
   * This is the other location -- versus inactive_arenas_scan() (executed in our dtor or later within
   * degraded_admin_thread_body()) -- where this occurs.  Please read the note in inactive_arenas_scan() in a similar
   * spot; it explains certain cleanup implications relevant to Thread_lcl_obj_db_client. */
} // Thread_lcl_obj_db_admin::forget_shm_arena()

template<typename Shm_arena_t>
void Thread_lcl_obj_db_admin<Shm_arena_t>::degraded_admin_thread_body(Thread_lcl_obj_db_admin* exiting_thread_obj_db,
                                                                      boost::promise<void>*
                                                                        done_with_exiting_thread_obj_db)
{
  using flow::Fine_clock;
  using boost::chrono::milliseconds;
  using boost::chrono::seconds;
  using boost::chrono::round;
  namespace this_thread = flow::util::this_thread;

  constexpr util::Fine_duration PERIOD_BTWN_CHECKS = milliseconds{100};
  constexpr util::Fine_duration PERIOD_BTWN_INFO_LOGS = seconds{5}; // Don't spam.

  const auto exiting_thread_token = exiting_thread_obj_db->m_thread_token;
  FLOW_LOG_INFO_LOCKED("Tl_obj_db_admin: Degraded-admin thread started to take over from thread with unique-token "
                       "[" << exiting_thread_token << "] (and/or see the present thread's nickname) "
                       "which is exiting and will exit shortly, but "
                       "not all live objects constructed by that thread have become unused, so we must keep "
                       "deleting them when possible in this new thread; once all gone: exit this thread too.  "
                       "This new thread-local object has moved all live-object data from the exiting thread's.");

  {
    /* Key-set write to *this->m_collection_dbs.  Per the #m_collection_dbs doc header, key-set writes must hold
     * #m_collection_dbs_mutex: *this (the replacement admin) is already registered in the TLSR-admin set, so
     * without this lock a concurrent stat::sharded_stats_impl() -- which reads the key-set under the same mutex --
     * could observe this map mid-move, i.e. a data race / UB. */
    flow::util::Lock_guard<decltype(m_collection_dbs_mutex)> lock{m_collection_dbs_mutex};
    m_collection_dbs = std::move(exiting_thread_obj_db->m_collection_dbs);
  }
  assert(exiting_thread_obj_db->m_collection_dbs.empty());

  /* Gotta update each Lend_tracker_pool's logging resource pointers; essentially it has a couple pointers into
   * the dying/dead thread's _admin; needs to be to *this _admin.  Only we can ever touch any of these
   * `Lend_tracker_pool`s, so there's no potential concurrency involved in this op.  Also:
   *
   * Seed this (drain) thread's tcache for each arena whose objects we may be reap-destroying below: we
   * inherited the exiting thread's objects but have never allocate()d anything ourselves.  In short this
   * ensures various deallocate()s can use Shm_arena memory-manager's (at least: SHM-jemalloc <=> jemalloc)
   * tcache mechanism.  The precipitating use-case is specifically jemalloc::Ipc_arena::deallocate() (see inside it)
   * and jemalloc::Ipc_arena::this_thread_ensure_tcache_exists() (see its doc header).  There's a somewhat-tricky
   * to-do in the former that might eliminate the impetus for this_thread_ensure_tcache_exists() existing/being
   * called.  Probably the impl for that to-do would live in or around this _admin class.
   * (Here also skip dead collections: nothing left to destroy there.) */
  for (const auto& [nil, collection_db] : m_collection_dbs)
  {
    auto& lend_tracker_pool_opt = collection_db->m_lend_tracker_pool;
    if (lend_tracker_pool_opt) // inactive_arenas_scan() may have nullified it.
    {
      lend_tracker_pool_opt->update_log_context(this, &m_skip_fast_path_verbose_logging);

      Owner_spc_impl<const Shm_arena>{*collection_db->m_shm_arena}.this_thread_ensure_tcache_exists();
    }
  }

  exiting_thread_obj_db = nullptr; // Just to avoid temptation to dereference -- it won't exist after this:
  done_with_exiting_thread_obj_db->set_value();

  auto last_info_log_when = Fine_clock::now();
  do // Don't check inactive_arenas_scan() now, since we wouldn't have been launched if it returned `true` now.
  {
    this_thread::sleep_for(PERIOD_BTWN_CHECKS);

    const auto now = Fine_clock::now();
    if ((now - last_info_log_when) >= PERIOD_BTWN_INFO_LOGS)
    {
      last_info_log_when = now;
      FLOW_LOG_INFO_LOCKED("Tl_obj_db_admin: Informing you periodically: "
                           "Degraded-admin thread still alive; # of arenas/collections with 1+ live objects: "
                           "[" << m_collection_dbs.size() << "].  We scan this thread's obj-DB every "
                           "[" << round<milliseconds>(PERIOD_BTWN_CHECKS) << "] which includes right now.");
    }

    /* We're just gonna ignore the flag in degraded-admin mode and assume it's true.  In for a penny, in for a pound.
     * forgetting_shm_arena() will arm it, and we just won't disarm it.  It might arm it again which is harmless.
     * (And again, etc.)  Note that it's not a lengthy operation -- too lengthy for any fast-path, but every
     * PERIOD_BTWN_CHECKS it is totally fine.
     *
     * P.S. Why ignore it?  Answer: just seems simpler/less entropy-laden.  Though to be honest it should be fine
     * to not-ignore it too.  @todo Maybe revisit. */
    if_requested_forget_arena_related_resources(true);

    unused_obj_scan(false, true); // Non-exhaustive; drain-mode (stats attribution).
    /* Note: I (ygoldfel) considered using unused_obj_scan(true, true) here but went with the non-exhaustive one
     * after all.
     * Reminder: non-exhaustive means it saves a copy of n_unused() -- count of live-but-unused objects at that
     * moment -- and only deletes at most that many; exhaustive means ignore that and go through *every* object.
     * There's only one drawback to non-exhaustiveness: n_unused() reads an atomic counter with relaxed-order, so
     * there could be some thread-cache-lag or ordering weirdness about its value.  An exhaustive scan would defeat
     * it (but not using it), but in some kinda-random situations a non-exhaustive scan could basically leave
     * some live-unused objects around in this pass.  We consider this acceptable:
     *   - even every PERIOD_BTWN_CHECKS, we don't want to do some unnecessary huge scan, if possible; and
     *   - worst-case, in another PERIOD_BTWN_CHECKS, we'll catch anything we missed; this delay is acceptable
     *     by its very conception. */

    /* Now check again for any arenas we track for which the above have eliminated all remaining live objects.
     * In addition to freeing the Lend_tracker_pool RAM, it returns true iff no live objects remain across *all*
     * of the arenas we track.  In that case we can finally RIP.
     *
     * Too lengthy (also, stupid) for any fast-path; absolutely fine every PERIOD_BTWN_CHECKS. */
  }
  while (!inactive_arenas_scan());

  FLOW_LOG_INFO_LOCKED("Tl_obj_db_admin: Degraded-admin thread started to take over from thread with unique-token "
                       "[" << exiting_thread_token << "]: Done!  Exiting thread.");
} // Thread_lcl_obj_db_admin::degraded_admin_thread_body()

template<typename Shm_arena_t>
void Thread_lcl_obj_db_admin<Shm_arena_t>::atexit_degraded_admin_threads_join() // Static.
{
  // We are after main() but before relevant static data are destroyed.  Careful.

  for (auto& thread : s_state.m_degraded_admin_threads) // No need to lock at this stage.  Only we can touch it.
  {
    if (thread->joinable())
    {
      thread->join();
    }
    // else { It already exited in peace, before main() returned/std::exit() was called. }
  }
  // Now all are not-threads.
} // Thread_lcl_obj_db_admin::atexit_degraded_admin_threads_join()

template<typename Shm_arena_t>
stat::Sharded_stats*
  Thread_lcl_obj_db_admin<Shm_arena_t>::stats_update_pre_consumption(collection_id_t collection_id)
{
  using flow::util::stat::store;
  using flow::util::Lock_guard;

  /* Caution!  Any field updated here may need special care when doing Finalized_shards::insert() in our ~dtor().
   * Make sure they act in sync. */

  // We are *not* necessarily in `*this` thread.  Hence the lock (see m_collection_dbs doc header).

  { // Lock protects *both* the key-set *and* pointer-thing in optional<Lend_tracker_pool> m_lend_tracker_pool.
    Lock_guard<decltype(m_collection_dbs_mutex)> lock{m_collection_dbs_mutex};
    const auto it_collection_id_and_db = m_collection_dbs.find(collection_id); // <-- Lock needed.
    if (it_collection_id_and_db == m_collection_dbs.end())
    {
      return nullptr; // As advertised.  Reminder: this is not an error; just means no relevant constructing_obj() yet.
    }
    // else:

    Collection_db& collection_db = *it_collection_id_and_db->second; // &collection_db is ptr-stable.

    auto& stats_shard = collection_db.m_stats_shard;
    const auto& lend_tracker_pool_opt = collection_db.m_lend_tracker_pool; // <-- Lock needed.
    store(&stats_shard.m_live_obj.m_live_object_zombies,
          lend_tracker_pool_opt ? lend_tracker_pool_opt->n_unused() // .n_unused() is a relaxed-atomic load.
                                : 0); // inactive_arenas_scan() nullified it: no live objects and none can appear.

    return &stats_shard;
  } // Lock_guard{m_collection_dbs_mutex}

  /* Tactical design discussion: Consider Live_obj m_live_obj and its stat-members; which ones should we update
   * here and why or why not?  Answers:
   *   - The _hi_wmark x 2 fields are not to be updated, as we're doing TL-sharding (explained in flow::util::stat
   *     doc header; we follow this logic in quite a few places).  stats_aggregate_shards() will set them.
   *   - That leaves m_live_objects (call it X) and m_live_object_zombies (call it Y).
   *     - We update Y here; while X is live-updated regularly.  Why though?
   *
   * Take Y first.  There is only one source of it: m_collection_dbs[A]->m_lend_tracker_pool->n_unused(), or 0
   * if !...->m_lend_tracker_pool.  All in all, we don't really have too much choice: Ignoring temporarily the
   * no-pool-due-to-inactive_arenas_scan() possibility: ->n_unused() loads an in-SHM atomic.  To live-update a
   * stat-member based on that, we'd have to go inside Lend_tracker_pool and have it live-updating both the in-SHM
   * atomic (as it must already, for the unused_obj_scan() algorithm to work) *and* the stat-member.  We want no
   * part of that, both due to perf concerns and due to the layer-penetration involved.  It would be ugly and possibly
   * slow -- for zero user benefit (it's still a shard, so we can't get an accurate _hi_wmark out of it).  So that's
   * why.  (Now, unforget the no-pool-due-to-inactive_arenas_scan() possibility.  We must therefore contend with
   * the possibility that the TL-state's thread will nullify the optional<> concurrently.  Fortunately this is not
   * too difficult, nor it is perf-dangerous: The nullification is rare, as is stat-consumption; so as documented
   * on m_collection_dbs_mutex, we "merely" need to extend that mutex to cover also this optional<>'s modification;
   * and lock it just above (which we must anyway: key-set is accessed) + when .reset()ting the optional<>.)
   *
   * Now take X.  At a first glance it seems like a good candidate for not-live-updating it as well.  Its value
   * is "simply" m_collection_dbs[A]->m_objs.size().  Awesome!  Actually it is not awesome:  This value is not
   * atomic<>, and it can be updated anytime and *frequently* (disposing_obj(), unused_obj_scan()).  So we'd have
   * to extend m_collection_dbs_mutex to protect it; but this would mean frequently locking it in the aforementioned
   * places.  (Granted: contention would be nil, so it's pretty cheap; but we've taken some serious effort to avoid
   * doing that all over, particularly if it'd be just for stats, and we endeavour to do the same here.)
   * Therefore that means we need to live-update the stat-member; and indeed we do, wherever m_objs is modified.
   * Fortunately this is both easy (enough) and cheap; at least no more expensive than the various other
   * TL-sharded stat-member updates we perform. */
} // Thread_lcl_obj_db_admin::stats_update_pre_consumption()

template<typename Shm_arena_t>
Thread_lcl_obj_db_admin<Shm_arena_t>::Static_state::Static_state() :
  m_obj_db_registry(nullptr, "Thread_lcl_obj_db_admin")
{
  Set_logger_registry::register_action([](flow::log::Logger* logger_ptr)
  {
    dbs_set_logger(logger_ptr);
  });
  // Now arena_lend::set_logger(x) will, among others potentially, do our `dbs_set_logger(x)`.
}

// Template implementations: Thread_lcl_obj_db_client.

template<typename Shm_arena_t>
Thread_lcl_obj_db_client<Shm_arena_t>::Thread_lcl_obj_db_client(flow::log::Logger* logger_ptr) :
  flow::log::Log_context_mt(nullptr, Log_component::S_SHM), // Really we init logger ptr in set_logger() just below.

  // (Avoid compiler warnings; initialize.) Really we init this in set_logger() just below.
  m_skip_fast_path_verbose_logging(false),

  m_thread_token(flow::util::this_thread_unique_token()),
  m_locked_gap_mutex(false)
{
  using boost::this_thread::at_thread_exit;

  set_logger(logger_ptr);

  at_thread_exit([this]() // Same deal as in _admin() ctor.  Please see that for explanation.
  {
    FLOW_LOG_TRACE_LOCKED("Tl_obj_db_client: The Gap mutex: locking for this TL-object.");
    m_locked_gap_mutex = true;  // Safety flag; exact reason (beyond being defensive) is explained in its doc header.
    stat::thread_end_gap_mutex().lock();
  });
}

template<typename Shm_arena_t>
Thread_lcl_obj_db_client<Shm_arena_t>::~Thread_lcl_obj_db_client()
{
  using flow::util::Lock_guard;
  using flow::util::Mutex_recursive;
  using std::optional;

  const auto this_thread_unique_token = flow::util::this_thread_unique_token();
  if (this_thread_unique_token != m_thread_token)
  {
    FLOW_LOG_WARNING_LOCKED("Tl_obj_db_client: "
                            "Shutting down from different thread (unique-token [" << this_thread_unique_token << "]) "
                            "than the one (unique-token [" << m_thread_token << "]) that created us; "
                            "bailing out.  Honestly this is strange; we are a `static` TL-registry, and per-thread "
                            "dtors should run before the central static object's.  In any case this can conceivably -- "
                            "in the absence of bugs -- only occur when program is exiting, at which "
                            "point graceful shutdown of resources is arguably less essential.");
    assert((!m_locked_gap_mutex) && "Tl_obj_db_client: We have a misdesign/bug; something is off thread-wise.");
    // We "even" skip the stats-saving stuff below.  Just don't feel like dealing with anything in this odd situation.
    return;
  }
  // else

  /* See explanation of identical thing in ~_admin() at identical spot.  This is part of that same technique.  Though:
   * Our situation is simpler in that there is only one way out of this dtor + only one place for our stat-shards
   * from m_per_arena_stats_shards to end up: Finalized_shards.  (_admin dtor may need to instead start a replacement
   * thread + new _admin; we lack anything like that.)  So yes: last thing we do is insert our shards into
   * Finalized_shards; then we return, right after unlocking this lock. */
  optional<Lock_guard<Mutex_recursive>> gap_lock;
  if (m_locked_gap_mutex) // See this guy's doc header for explanation about this guard.
  {
    FLOW_LOG_TRACE_LOCKED("Tl_obj_db_client: The Gap mutex is locked: adopting until dtor returns.");
    gap_lock.emplace(stat::thread_end_gap_mutex(), boost::adopt_lock);
  }
  else
  {
    FLOW_LOG_INFO_LOCKED("Tl_obj_db_client: The Gap mutex is *not* locked; presumably this is the OS/main thread; "
                         "probably application exit()ing/typically post-main(); no real danger of concurrent "
                         "stat-touching.");
  }

  /* *this exists specifically to execute this thread's SHM-pool use_ct++ and use_ct--, because the objects
   * involved were constructed in some other thread.  So by definition our thread exiting means nothing we do
   * matters from this point on... except the stats-dumping.  So just clean up all the resources; that's it...
   * aside from the stats-dumping. */
  FLOW_LOG_INFO_LOCKED("Tl_obj_db_client: Thread exiting; will simply close any lend-tracker SHM-pool client handles "
                       "we've opened.");

  // Note: Lend_tracker_pool (client-type) dtor does not perform removal of pool name from file-system.

  if (!m_per_arena_stats_shards.empty()) // Don't even touch it, if all arenas already forgetting_shm_arena()ed away.
  { /* Stats.
     * Last thing is we have to do this stats-related procedure, so that the stats we've accumulated do not go *poof*.
     * Take a particular Shm_arena A and, say, stats_shard->m_lend_obj.m_lend_count we've counted in
     * m_per_arena_stats_shards[A].  That shard is merely a shard for the
     * thread that is now exiting; arena A may well still exist, and
     * our contribution to m_lend_count at stat-consumption that counted a split-second ago should equally count a
     * split-second after the dtor exits and thread exits.  It should keep counting until that arena goes
     * away, at which point all such contributions would be thrown out (for cleanliness/leak avoidance).
     *
     * At any rate: Finalized_shards takes care of it; we just have to give it whatever
     * arena/stats pairs we've got still, before they go *poof*.
     *
     * There is one caveat, analogous to (though a bit simpler than) ~_admin() dtor's similar short time window:
     * Right now `this` has been removed from the Thread_local_state_registry<_client>, so our stats
     * won't participate in any aggregation.  We'll fix that via _finalized_shards soon, but in the meantime
     * stat-consumption and stat-resetting will be incomplete if executed right now... but that is why
     * we're doing the thread_end_gap_mutex() thing above.  So in fact if it's executing right now then it is waiting
     * for that lock to become available; all is cool.
     *
     * Lastly: There could be another caveat, again sorta-analogous to ~_admin()'s: During this time window
     * forgetting_shm_arena(), in looping through extant `_client`s, will not see `*this`.  Is it an issue?
     * The answer is no-ish.  There's no *this needing access to the arena itself, nor is there any
     * replacement _client possibility; _client::forgetting_shm_arena(A) is only about erasing each
     * m_per_arena_stats_shards[A], for cleanliness + avoiding small RAM-leak.  So before this ongoing teardown,
     * [A] would be erased fine; after it, Finalized_shards::stats_erase(A) will clean it out of Finalized_shards
     * (where we would have put it just below).  During it -- The Gap -- though, it would leak into Finalized_shards
     * where it would stay forever.  It's certainly a bit ugly and slightly leaky but not really a big deal
     * practically speaking... as of this writing.  That said formally we disallow it.  1, ugly+leak = let's not.
     * 2, in _admin() it is a *real* problem, and for that one we disallow it... but _admin and _client teardowns
     * are part of the same thing, really: in practice _admin::f_s_a() and _client::f_s_a() are called one after
     * the other in Shm_arena (e.g., jemalloc::Ipc_arena) shutdown.  So let's just avoid all this entropy.  No
     * forgetting_shm_arena() during _client teardown (now). */

    // (No need to lock.  As noted, stat-consumption TL-state walk won't see us by now.)
    for (const auto& [collection_id, stats_shard] : m_per_arena_stats_shards)
    {
      stat::Finalized_shards<Shm_arena>::get_instance().stats_insert(collection_id, *stats_shard);
    }
  } // Stats.

   // stat::thread_end_gap_mutex() unlocks here.
 } // Thread_lcl_obj_db_client::~Thread_lcl_obj_db_client()

template<typename Shm_arena_t>
Thread_lcl_obj_db_client<Shm_arena_t>*
  Thread_lcl_obj_db_client<Shm_arena_t>::this_thread_obj_db() // Static.
{
  return s_state.m_obj_db_registry.this_thread_state(); // Create it if necessary; and return it either way.
}

template<typename Shm_arena_t>
void Thread_lcl_obj_db_client<Shm_arena_t>::dbs_set_logger(flow::log::Logger* logger_ptr) // Static.
{
  s_state.m_obj_db_registry.set_logger(logger_ptr);
}

template<typename Shm_arena_t>
flow::log::Logger*
  Thread_lcl_obj_db_client<Shm_arena_t>::set_logger(flow::log::Logger* logger_ptr)
{
  // Reminder: This is happening *not* (necessarily) in the thread matching m_thread_token.  No willy-nilly m_ access.

  const auto prev_logger_ptr = flow::log::Log_context_mt::set_logger(logger_ptr); // Do the normal thing...

  // ...but also do this stuff as advertised...
  m_skip_fast_path_verbose_logging.store(!(logger_ptr
                                           && logger_ptr->should_log(flow::log::Sev::S_TRACE, Log_component::S_SHM)),
                                         std::memory_order_relaxed);

  return prev_logger_ptr;
}

template<typename Shm_arena_t>
void Thread_lcl_obj_db_client<Shm_arena_t>::disposing_obj(const Shm_arena& shm_arena,
                                                          pool_id_t lend_tracker_pool_id, use_ct_idx_t use_ct_idx)
{
  using flow::util::stat::fetch_add;
  using flow::util::stat::fetch_sub;

  // This is the owner-side disposer action in non-admin (i.e., allocating, for the given object) thread.

  const auto it = m_lend_tracker_pools.find(lend_tracker_pool_id);
  const auto pool_data = (it == m_lend_tracker_pools.end()) // ? slow-path : fast-path
                           ? new_pool_data(lend_tracker_pool_id, util::Process_credentials::own_process_id(),
                                           shm_arena.get_id(),
                                           Owner_spc_impl<const Shm_arena>{shm_arena}
                                             .generate_shm_object_name(lend_tracker_pool_id))
                           : it->second.get();
  pool_data->m_lend_tracker_pool.use_count_dec(use_ct_idx); // It logs enough.
  /* Even if it reached 0, it'll require ..._admin counterpart to complete the disposal of the object
   * in the proper thread.  ->m_lend_tracker_pool did try to help it do so faster by writing some hint stuff. */

  { // Stats.
    auto& owner_obj_stats = stats_shard(shm_arena)->m_owner_obj;
    fetch_add(&owner_obj_stats.m_disposer_count, 1);
    fetch_sub(&owner_obj_stats.m_live_handle_groups, 1);
  }

  /* Slow-path only (new_pool_data() called above): Mini-discussion about error handling:
   * What if it threw, as (per contract) it can?  The cause can be split into 2 categories; in all cases only
   * due to failure to open the client-mode Lend_tracker_pool, pointing to an already-prepared (by
   * Thread_lcl_obj_db_admin, not us) underlying named SHM-pool.
   *   - Permission error or worse: Permission error, of some kind, means at best that some sort of config is
   *     wrong -- our application lacks credentials to open what _admin application created.  Worse are even
   *     more exceptional errors; running out of native-handles -- that kind of thing.  Either way: By this point
   *     in time this is shocking and unrecoverable; the SHM-provider (e.g., SHM-jemalloc) should have already
   *     detected any civilized permissions problem when opening the actual borrowed pool.  So: at this point
   *     we throw our hands up and throw an exception.  That's why exceptions, classically, exist: exceptional
   *     circumstances.
   *     - In this particular case, in fact: We should only be invoked from within a shared_ptr disposer;
   *       throwing an uncaught exception therein causes terminate() (abort).  So that's what will happen in
   *       practice.
   *   - No such named pool exists.  In *our* context this is exceptional too: This overload by definition is
   *     invoked owner-side.  constructing_obj() must have worked, and therefore created the tracker-pool, for us
   *     to be called.  Tracker-pool removed (by name, from file-system) since then <=> arena destroyed, or
   *     all constructed-objects destroyed normally + original construct()ing thread is gone; either way it
   *     is not possible we would be called (all owner-handles being dropped already is pre-condition for those).
   *     So, this is bad/unrecoverable/exceptional.
   * Therefore: Simply let the exception, if any, be thrown. */
} // Thread_lcl_obj_db_client::disposing_obj(owner-side)

template<typename Shm_arena_t>
void Thread_lcl_obj_db_client<Shm_arena_t>::disposing_obj(pool_id_t lend_tracker_pool_id, use_ct_idx_t use_ct_idx,
                                                          owner_id_t owner_id, collection_id_t collection_id)
{
  using ipc::session::shm::arena_lend::Borrower_shm_pool_collection_repository;
  using flow::error::Runtime_error;

  // This is the borrower-side disposer action.

  Pool_data* pool_data = {};

  const auto it = m_lend_tracker_pools.find(lend_tracker_pool_id);
  if (it == m_lend_tracker_pools.end())
  {
    /* Slow-path.  Mostly similar to owner-side overload's slow-path; just to get the pool name we have to take
     * different steps (gotta find the borrower-side view of the allocating arena a/k/a pool-collection).
     *
     * The other difference is how one handles the possibility new_pool_data() throws.  Owner-side, as discussed
     * therein (do read it), if it throws, then it's always bad/unrecoverable, so we just let the throw happen.
     * In our case, though, there is one case where it is benign, so we handle that differently.  We get into
     * that below, but in the first place need to catch the exception, if any.  Hence `try`. */
    try
    {
      pool_data = new_pool_data(lend_tracker_pool_id, owner_id, collection_id,
                                Borrower_shm_pool_collection_repository<Shm_arena>::get_instance()
                                  .recompute_pool_name(owner_id, collection_id, lend_tracker_pool_id));
      // Note: It already logs WARNING if something goes bad + it throws.
    }
    catch (const Runtime_error& exc)
    {
      if (exc.code() == boost::system::errc::no_such_file_or_directory)
      {
        FLOW_LOG_INFO_LOCKED("Tl_obj_db_client: Borrower-side: In this thread (process ID "
                             "[" << util::Process_credentials::own_process_id() << "]) while accounting disposal "
                             "of borrowed handle, discovered that this thread has not yet accessed the lend-tracker "
                             "pool from the owner-side thread that constructed handle/object (normal); but "
                             "that lend-tracker-pool has been removed from the file-system; only the owner side "
                             "can do this, and only after all objects (including therefore ours) tracked by the "
                             "lend-tracker-pool have been freed.  Probably: user is currently reacting to end of "
                             "session (connection to opposing process, caused by them, detected by our process) by "
                             "nullifying (forgetting) all handles originating in arena(s) lent through the "
                             "now-ended session.  In that (probable) case, it is not an error/is not a bad state, "
                             "and the tracked-object is already freed (or being freed) owner-side, so we need not "
                             "do anything and can safely no-op.  (If something bad *did* happen, this no-op at worst "
                             "chooses not to explode, when it could; but it is likely something closer to the "
                             "source of the hypothetical bad state will soon explode more usefully.)  "
                             "Lend-tracker has pool ID [" << lend_tracker_pool_id << "] "
                             "pertaining to arena/collection [" << owner_id << '/' << collection_id << "]; "
                             "object use-count index [" << use_ct_idx << "].");
        /* Ideally that message text already explained this, but just to be extra explicit: That scenario (owner ended
         * session, then immediately destroyed arena lent through it; borrower here detected ended session,
         * nullified an object handle -- disposer ran and called us -- but _admin managed to already delete the
         * arena-associated Lend_tracker_pool from file-system; so we got no-such-file error) is a real,
         * formally possible scenario; and it has been observed empirically. */
        return;
      }
      /* else:
       * We `throw;`, essentially as-if we let new_pool_data() throw without catching it.  The alternative
       * is mainly to no-op (modulo logging, but new_pool_data() already did as noted above).  No-op is actually
       * defensible here; we do not know of any reason we couldn't open the pool by name (other than not_found_error;
       * eliminated if got here) that's valid -- but the most we would have done anyway is to decrement the use-count,
       * downstream of which owner-side the object would get reaped upon its reaching zero (now or later); so
       * just skipping that and letting the rest of the system deal with whatever weirdness caused this situation =
       * not half bad.
       *
       * We choose nevertheless to be loud.  This is not normal, that we know of, so be proactive. */
      throw;
    }

    // Got here: new_pool_data() worked great.
    assert(pool_data);
  } // m_lend_tracker_pools.find(lend_tracker_pool_id) failed.
  else
  {
    // Fast-path.
    pool_data = it->second.get();
  }

  pool_data->m_lend_tracker_pool.use_count_dec(use_ct_idx); // It logs enough.
  /* Even if it reached 0, it'll take ..._admin on the opposing (owner) side to handle it in the
   * proper thread (and process).
   * *lend_tracker_pool did try to help it do so faster by writing some hint stuff. */

  // Stats: none.  We track owner-side stuff only.
} // Thread_lcl_obj_db_client::disposing_obj(borrower-side)

template<typename Shm_arena_t>
void Thread_lcl_obj_db_client<Shm_arena_t>::lending_obj(const Shm_arena& shm_arena,
                                                        pool_id_t lend_tracker_pool_id, use_ct_idx_t use_ct_idx)
{
  using flow::util::stat::fetch_add;

  const auto it = m_lend_tracker_pools.find(lend_tracker_pool_id);
  const auto pool_data = (it == m_lend_tracker_pools.end()) // ? slow-path : fast-path
                           ? new_pool_data(lend_tracker_pool_id, util::Process_credentials::own_process_id(),
                                           shm_arena.get_id(),
                                           Owner_spc_impl<const Shm_arena>{shm_arena}
                                             .generate_shm_object_name(lend_tracker_pool_id))
                           : it->second.get();
  pool_data->m_lend_tracker_pool.use_count_inc(use_ct_idx); // It logs enough.

  { // Stats.
    fetch_add(&(stats_shard(shm_arena)->m_lend_obj.m_lend_count), 1);
  }

  /* Slow-path only (new_pool_data() called above): Mini-discussion about error handling:
   * Same as for owner-side disposing_obj() overload:
   *   disposing_obj(const Shm_arena&, pool_id_t, use_ct_idx_t).
   * Same logic applies.  One practical difference: disposing_obj() would be called from inside shared_ptr
   * disposer, so a throw there causes terminate()/abort for sure.  We however are called from session.lend_object(),
   * so the ultimate effect of the (potential) throw is <who knows what exactly>;
   * that is fine/expected; just pointing it out, since it is different. */
} // Thread_lcl_obj_db_client::lending_obj()

template<typename Shm_arena_t>
typename Thread_lcl_obj_db_client<Shm_arena_t>::Pool_data*
  Thread_lcl_obj_db_client<Shm_arena_t>::new_pool_data(pool_id_t lend_tracker_pool_id, owner_id_t owner_id,
                                                       collection_id_t collection_id,
                                                       const Shared_name& new_pool_name)
{
  using flow::util::stat::fetch_add;
  using flow::util::stat::fetch_sub;
  using flow::util::stat::update_hi_wmark;
  using std::remove_reference_t;
  using std::vector;

  /* Slow-path.
   *
   * Our essential purpose is to add the new (to us) tracker-pool's stuff to m_lend_tracker_pools.
   * We do that at the end.
   *
   * First, though, we opportunistically clean out any such stuff to do with *existing* pools such that
   * they are no longer active, because Thread_lcl_obj_db_admin has destroyed the admin-mode
   * `Lend_tracker_pool`s, which would would cause our client-mode `Lend_tracker_pool`s accessing their
   * respective pools to yield `Lend_tracker_pool::dead() == true`.  To show that this is the right thing to do --
   * and note this is the only place we do it -- really we need to show 2 things:
   *   - That the cleaning, as we do it, is good and safe.
   *     Good: it is good, because (as noted in _admin::inactive_arenas_scan()) letting unused client-mode
   *     `Lend_tracker_pool`s sit around over time can leak SHM-pool-contained RAM.  Safe: it is safe, because
   *     by definition of Lend_tracker_pool::dead(), once it's true, that pool won't be used to allocate or deallocate
   *     objects.
   *   - That triggering the cleaning (exclusively) here is both (1) sufficiently responsive (averts SHM-pool-RAM
   *     leaking for too long) and (2) sufficiently perf-cheap (we don't spend too many cycles doing it nor
   *     slow-down our callers' code paths too much -- disposing_obj(), lending_obj()).
   *
   * W/r/t (2): This operation isn't free but is certainly a split-second thing.  There is normally only a handful
   * SHM-arenas being owned or borrowed, let alone with the need to do disposing_obj() and lending_obj() on each of
   * them in a particular thread.  Looping through them is okay, for some definition of okay, and the actual freeing
   * of the resources (including closing Lend_tracker_pool) has to happen sometime/has to be piggy-backed onto
   * something -- so really it's only the looping that's the potential costly issue.  It's not that costly; recall
   * that this is the slow-path in the first place; we'll need to open a *new* Lend_tracker_pool, with its
   * SHM-pool-handle and everything.  The overhead we're adding with this opportunistic looping likely doesn't
   * massively change the perf profile of this slow-path.
   *
   * That leaves (1).  That's subjective; however there's a pleasing symmetry here: A new Lend_tracker_pool is
   * created -- and that's when we (potentially) eliminate all old/crusty ones.  It means that the "leak" lasts
   * no longer than what's dictated by the rhythm of the same kind of resources being added.  In plainer language
   * (arguably): as new events (new sessions, new use of those sessions) load more SHM-pools, we unload ones
   * pertaining to older such events at the same time.  Pretty good!  (Other ideas we've entertained involved
   * piggy-backing it in every disposing_obj()/lending_obj(), but gated on a timer -- do it no more frequently
   * than every 30 seconds, or something.  Using the new_pool_data() rhythm instead seems more elegant.)
   *
   * Also then all the stat-updating is in one place... noice. */

  size_t stat_reap_ct;
  size_t stat_reap_check_ct;

  { // Clean dead stuff: m_lend_tracker_pools.
    vector<pool_id_t> dead_lend_tracker_pool_ids;
    for (const auto& dead_pool_id_and_data : m_lend_tracker_pools)
    {
      // (See similar C++17/20 note higher up in similar situation.)
      const auto dead_lend_tracker_pool_id = dead_pool_id_and_data.first;
      const auto& pool_data = dead_pool_id_and_data.second;

      if (pool_data->m_lend_tracker_pool.dead()) // Else it's an extant guy that's perfectly alive.
      {
        FLOW_LOG_INFO_LOCKED("Tl_obj_db_client: Opportunistic scan: In this thread (process ID "
                             "[" << util::Process_credentials::own_process_id() << "]) forgetting "
                             "owner's per-constructing-thread's now-dead lend-tracker pool "
                             "(ID [" << dead_lend_tracker_pool_id << "]) "
                             "tracking objects in arena/collection "
                             "[" << pool_data->m_uniq_collection_id.first
                                 << '/' << pool_data->m_uniq_collection_id.second << "]; "
                             "we are either a borrower process or different thread in owner's process.  "
                             "Owner constructing-thread had marked underlying tracker-pool as dead; presumably either "
                             "because thread (or degraded admin-thread) exited with no live objects remaining; or "
                             "because arena was destroyed wholesale (with or without live objects remaining).");
        dead_lend_tracker_pool_ids.push_back(dead_lend_tracker_pool_id);
      }
    } // for ([dead_lend_tracker_pool_id, pool_data] : m_lend_tracker_pools)

    { // Stats-prep (before reducing m_lend_tracker_pools.size()).
      stat_reap_check_ct = m_lend_tracker_pools.size();
      stat_reap_ct = dead_lend_tracker_pool_ids.size();
    }

    // Erase from m_lend_tracker_pools (avoided it before, so it could be looped-through in peace).
    for (const auto dead_lend_tracker_pool_id : dead_lend_tracker_pool_ids)
    {
      m_lend_tracker_pools.erase(dead_lend_tracker_pool_id);
      // Note: Lend_tracker_pool (client-type) dtor does not perform removal of pool name from file-system.
    }
  } // Clean dead stuff: m_lend_tracker_pools.

  /* Clean dead stuff: m_per_arena_stats_shards.
   *
   * Explanation: See forgetting_shm_arena(), as far as what would trigger the following to not be a no-op.
   * Why here though?  Answer: While it is not pleasingly symmetrical in the same way that the m_lend_tracker_pools
   * cleanup above is, it is also a much lower-stakes operation.  We're just deleting stats for an arena that no
   * longer exists (hence user can't query for it anyway).  The fast-path here is an atomic flag check + GTFO.
   * The slow-path is to remove some stats structures.  It seems prudent to keep the cleanup-triggerable ops
   * (m_lend_tracker_pools versus m_per_arena_stats_shards) in the same code-path.  Further increasing
   * responsiveness -- by placing this call in each lending_obj()/disposing_obj() instead -- would bring little
   * noticeable practical benefit.  */
  if_requested_forget_arena_related_resources();

  { // Add new pool + record stats.
    Own<Pool_data> pool_data_or_none;

    /* Helper we'll invoke at the end, no matter what happens.  The dichotomy as to "what happens" is merely:
     * Either we successfully opened 1 pool-handle; or we failed and opened zero.  How to react to the latter
     * is, by contract, on the caller; but we will record the stats of it appropriately. */
    const auto update_stats = [&]()
    { // Stats.  Avoid touching atomic<>s (no add 0/sub 0); and batch into fewest ops: (add/sub x <=4) + (store x <= 1).
      auto& stats = *(obj_db_aux_pool_global_stats_mutable());
      const auto pool_opened_else_failed = bool(pool_data_or_none);

      if (pool_opened_else_failed)
      {
        // Definitely opened 1 pool-hndl.
        fetch_add(&stats.m_client_tl_aux_pool_hndl_open_count, 1);
      }

      // Possibly reap-checked 1+ pool-hndls and reaped <= that-many.
      if (stat_reap_check_ct != 0)
      {
        fetch_add(&stats.m_client_tl_aux_pool_hndl_reap_check_count, stat_reap_check_ct);
        (stat_reap_ct == 0) || fetch_add(&stats.m_client_tl_aux_pool_hndl_reap_count, stat_reap_ct);
      }

      if (pool_opened_else_failed) // I.e., opened 1 pool-hndls.
      {
        // May have affected the balance-gauge up 1, down 1+, or neither (<= opened 1 for sure... but also closed 1).
        if (stat_reap_ct == 0)
        {
          update_hi_wmark(&stats.m_client_tl_aux_pool_live_hndls_hi_wmark,
                          fetch_add(&stats.m_client_tl_aux_pool_live_hndls, 1) + 1);
        }
        else if (stat_reap_ct != 1) // I.e., `>= 2`.
        {
          fetch_sub(&stats.m_client_tl_aux_pool_live_hndls, stat_reap_ct - 1);
        }
        // else { Equilibrium.  Harmony.  Zen. }
      }
      else // if (!pool_opened_else_failed) [I.e., opened 0 pool-hndls.]
      {
        /* Opened 0, so definitely no `stats.m_client_tl_aux_pool_live_hndls +=`; we are either to no-op (equilibrium),
         * or `stats.m_client_tl_aux_pool_live_hndls -=`. */
        if (stat_reap_ct != 0) // I.e., `>= 1`.
        {
          fetch_sub(&stats.m_client_tl_aux_pool_live_hndls, stat_reap_ct);
        }
        // else { Equilibrium.  Harmony.  Zen. }

        /* Also track the failure itself.  (The catch clause that invoked us shall rethrow; whether the
         * ultimate caller treats that as benign -- the not-found-tolerant borrower-disposer path -- or as a
         * grave problem, the count is of interest either way; see the stat's doc header.) */
        fetch_add(&stats.m_client_tl_aux_pool_hndl_open_fail_count, 1);
      } // else // if (!pool_opened_else_failed)
    }; // update_stats =

    FLOW_LOG_INFO_LOCKED("Tl_obj_db_client: In this thread (process ID "
                         "[" << util::Process_credentials::own_process_id() << "]) making first access to an "
                         "owner's per-constructing-thread's lend-tracker pool (ID [" << lend_tracker_pool_id << "]) "
                         "tracking objects in arena/collection "
                         "[" << owner_id << '/' << collection_id << "]; " // owner_id is also a PID as of this writing.
                         "we are either a borrower process or different thread in owner's process.");

    try
    {
      pool_data_or_none.reset(new Pool_data{ Uniq_collection_id{owner_id, collection_id},
                                             Lend_tracker_pool
                                               {this, &m_skip_fast_path_verbose_logging, // For its logging needs.
                                                new_pool_name, util::OPEN_ONLY} });
    }
    catch (...) // By contract: If caught: we're gonna rethrow regardless; just want to do some book-keeping first.
    {
      // Lend_tracker_pool ctor threw; there is no Pool_data; as promised we will insert nothing and (re)throw.
      update_stats();
      throw;
    }

    /* Attn: update_stats() inspects pool_data_or_none to distinguish opened-1 from opened-0; so it must run
     * before the move-out just below empties it. */
    update_stats();

    auto& pool_data = m_lend_tracker_pools[lend_tracker_pool_id];
    assert((!pool_data) && "Breaking contract of this helper internal method.");
    pool_data = std::move(pool_data_or_none);
    assert(pool_data && "Logic bug above?  No exception must mean successful pool-opening and non-null Pool_data.");

    return pool_data.get();
  } // Add new pool + record stats.
} // Thread_lcl_obj_db_client::new_pool_data()

template<typename Shm_arena_t>
stat::Sharded_stats* Thread_lcl_obj_db_client<Shm_arena_t>::stats_shard(const Shm_arena& shm_arena)
{
  using stat::Sharded_stats;
  using flow::util::Lock_guard;

  /* See doc header for m_per_arena_stats_shards which explains why we organize this the way we do,
   * especially why we lock the mutex but only if adding (elsewhere, removing) an arena-key.
   * Accordingly: This faintly echoes what happens in _admin::constructing_obj() when looking up the
   * Collection_db by collection_id. */

  const auto collection_id = shm_arena.get_id();

  const auto it_collection_id_and_stats = m_per_arena_stats_shards.find(collection_id);
  if (it_collection_id_and_stats == m_per_arena_stats_shards.end())
  {
    // Slow-path.  Create fresh Stat_set.
    const auto shard = new Sharded_stats;

    // As noted above, insert under lock -- stat-consumption might be reading key-set under same lock right now.
    {
      Lock_guard<decltype(m_per_arena_stats_shards_mutex)> lock{m_per_arena_stats_shards_mutex};
      m_per_arena_stats_shards[collection_id].reset(shard);
    }

    return shard;
  } // if (m_per_arena_stats_shards[collection_id] not found)
  // else // if (m_per_arena_stats_shards[collection_id] found):

  // Fast-path.
  return it_collection_id_and_stats->second.get();
} // Thread_lcl_obj_db_client::stats_shard()

template<typename Shm_arena_t>
void Thread_lcl_obj_db_client<Shm_arena_t>::forgetting_shm_arena(collection_id_t collection_id) // Static.
{
  namespace this_thread = flow::util::this_thread;

  /* See our doc header for impl overview.
   *
   * As noted there, ultimately this is similar to _admin::forgetting_shm_arena() but simpler.
   * There's no point in looking there; what we're doing is simple enough to where it's very close to
   * the flow::util::Polled_shared_state doc header's example of how to use Polled_shared_state.
   *
   * If you *do* look in _admin's counterpart, you might notice we don't do the thing where we'd:
   * [[[ Short-circuit stuff if the current thread = the one pertaining to one of the _client `*this`es;
   *     then we can do the removal synchronously and possibly even not need to do anything further,
   *     if that's the only extant *this. ]]]
   * Instead we handle that case like any other; it'll just be deferred until the next
   * if_requested_forget_arena_related_resources(), even if that is in the very same calling thread.  This is for
   * simplicity of the code -- there isn't much at stake: it's fine to delay the erasure of
   * one m_per_arena_stats_shards[X] until a bit later.  Cf. _admin where there is stuff at stake... but we digress. */

  s_state.m_obj_db_registry.while_locked([&](const auto& obj_db_per_thread)
  {
    if (obj_db_per_thread.empty())
    {
      return; // No extant thread-local obj-DBs.  Nothing to forget among them then... yay.
    }
    // else

    s_state.m_arenas_to_forget_map.while_locked([&](auto* arenas_to_forget_map)
    {
      auto& db_set = (*arenas_to_forget_map)[collection_id];

      assert(db_set.empty()
             && "forgetting_shm_arena() called twice on the same owner+collection ID?  Bug?");

      for (const auto& [obj_db, nil] : obj_db_per_thread)
      {
        db_set.insert(obj_db);
        /* It means: "Yo, `obj_db`, if you've got any stuff pertaining to arena collection_id, remove/deinit it;
         * but either way you-dun-good, so remove yourself from s_state.m_arenas_to_forget_map[collection_id]. */
      }
    }); // s_state.m_arenas_to_forget_map.while_locked()

    for (const auto& [obj_db, nil] : obj_db_per_thread)
    {
      obj_db->m_forget_resources_requested_poll_flag.arm_next_poll();
    }
  }); // s_state.m_obj_db_registry.while_locked()

  { /* Stats:
     * Nothing to do here.  Should we perhaps, like _admin::forgetting_shm_arena(), do the following?
     *   Finalized_shards<Shm_arena>::get_instance()::stats_erase(collection_id);
     * Well, no: For one thing, in practice that method is invoked at ~the same time as us; for example
     * see jemalloc::Ipc_arena destruction code-path.  So our doing it here would be a no-op.
     *
     * The deeper point is, though, this: The segregating compile-time discriminator -- the template arg
     * to _finalized_shards -- is our (and _admin's) own Shm_arena tparam.  I.e., both the _admin and the _client
     * for the SHM-provider (SHM-jemalloc being one of them; Shm_arena = jemalloc::Ipc_arena) dump their
     * shards into the same singleton, into the same respective per-collection_id buckets in its map.
     * Where the stats come-from doesn't matter; they're all aggregated-together anyway as shards.
     *
     * So the stats_insert() just has to happen once, when the given arena is being forgotten.  Since
     * _admin is the big daddy, if it has to be one of us, it should be _admin.
     * @todo Though (very) arguably, perhaps it would be safer maintenance-wise to do it in both places
     * anyway, since both *do* dump shards in there.  Revisit maybe. */
  }
} // Thread_lcl_obj_db_client::forgetting_shm_arena()

template<typename Shm_arena_t>
void Thread_lcl_obj_db_client<Shm_arena_t>::if_requested_forget_arena_related_resources()
{
  using flow::util::Lock_guard;
  using std::vector;

  /* Avoid logging work, or any unneeded work of any kind, in our fast-path... though as of this writing
   * we are only invoked from new_pool_data() which by definition is not the fast-path of lending_obj() or
   * disposing_obj() (owner-side).  So we're already not called that often. */

  if (!m_forget_resources_requested_poll_flag.poll_armed())
  {
    return;
  }
  // else if (it was requested): We will do it, and it is now re-marked as not-requested.

  // Non-fast-path is in effect (can log, etc.).

  s_state.m_arenas_to_forget_map.while_locked([&](auto* arenas_to_forget_map_ptr)
  {
    auto& arenas_to_forget_map = *arenas_to_forget_map_ptr;
    vector<collection_id_t> finished_collection_ids;

    for (auto& collection_id_and_db_set : arenas_to_forget_map)
    {
      // (See similar C++17/20 note higher up in similar situation.)
      const auto collection_id = collection_id_and_db_set.first;
      auto& db_set = collection_id_and_db_set.second;

      if (db_set.erase(this) == 0)
      {
        continue; // This collection/arena being forgotten is (no longer?) dependent on our deleting it from *this.
      }
      // else

      const bool last_one = db_set.empty();
      bool erased;
      {
        /* The goal of all this unpleasantness is in this block.  To repeat our doc header Impl mini-section:
         * We change the key-set of m_per_arena_stats_shards; and therefore we must lock it; but the fact
         * we do so in the proper thread w/r/t *this -- and no other thread changes the key-set -- means
         * that when *reading* m_per_arena_stats_shards in the lending_obj()/disposing_obj() fast-paths we
         * don't need to lock said mutex.
         * @todo Honestly... locking that mutex there, under zero contention, is pretty damned cheap; and then
         * forgetting_shm_arena() would be just a few lines and synchronous at that; the present method
         * wouldn't exist at all.  The gain in simplicity/maintainability might be worth it.  Revisit. */
        Lock_guard<decltype(m_per_arena_stats_shards_mutex)> lock{m_per_arena_stats_shards_mutex};
        erased = (m_per_arena_stats_shards.erase(collection_id) == 1);
      }
      if (erased)
      {
        FLOW_LOG_INFO_LOCKED("Tl_obj_db_client: Local arena/collection [" << collection_id << "] is being "
                             "forgotten; so will forget related tracking structures in heap; are we the last "
                             "per-thread obj-DB that was remaining to do this? = [" << last_one << "].");
      }
      // else { For the arena-being-forgotten, we don't have any data anyway. }

      if (last_one)
      {
        FLOW_LOG_INFO_LOCKED("Tl_obj_db_client: Local arena/collection [" << collection_id << "] is being "
                             "forgotten; opportunistically scanned our per-thread in-heap structures, and this was "
                             "the last such per-thread structure that needed handling; so this completes the "
                             "forgetting of that arena/collection among the obj-DB per-thread clients.");
        finished_collection_ids.push_back(collection_id);
      }
    } // for ([collection_id, db_set] : arenas_to_forget_map)

    // Clean out the entries we made empty (arena-IDs such that nothing left to do for it; arena forgotten).
    for (const auto collection_id : finished_collection_ids)
    {
      arenas_to_forget_map.erase(collection_id);
    }
  }); // s_state.m_arenas_to_forget_map.while_locked()
} // Thread_lcl_obj_db_client::if_requested_forget_arena_related_resources()

template<typename Shm_arena_t>
const arena_lend::stat::Obj_db_aux_pool_global_stats&
  Thread_lcl_obj_db_client<Shm_arena_t>::obj_db_aux_pool_global_stats() // Static.
{
  using arena_lend::stat::Obj_db_aux_pool_global_stats;
  using flow::util::stat::Global_stats;

  /* Rationale for using the Flow Global_stats facility (as opposed to just shoving a `public: static`
   * Obj_db_aux_pool_global_stats member in the class): Same as in Thread_local_pool_lookup_rev. */

  return Global_stats<Thread_lcl_obj_db_client, Obj_db_aux_pool_global_stats>::get().stats_default();
}

template<typename Shm_arena_t>
arena_lend::stat::Obj_db_aux_pool_global_stats*
  Thread_lcl_obj_db_client<Shm_arena_t>::obj_db_aux_pool_global_stats_mutable() // Static.
{
  using arena_lend::stat::Obj_db_aux_pool_global_stats;

  return &(flow::util::stat::Global_stats<Thread_lcl_obj_db_client, Obj_db_aux_pool_global_stats>::get()
             .stats_mutable_default());
}

template<typename Shm_arena_t>
void Thread_lcl_obj_db_client<Shm_arena_t>::obj_db_aux_pool_global_stats_reset() // Static.
{
  using arena_lend::stat::Obj_db_aux_pool_global_stats;

  flow::util::stat::stats_reset(obj_db_aux_pool_global_stats_mutable(), Obj_db_aux_pool_global_stats{});
}

template<typename Shm_arena_t>
Thread_lcl_obj_db_client<Shm_arena_t>::Static_state::Static_state() :
  m_obj_db_registry(nullptr, "Thread_lcl_obj_db_client")
{
  Set_logger_registry::register_action([](flow::log::Logger* logger_ptr)
  {
    dbs_set_logger(logger_ptr);
  });
  // Now arena_lend::set_logger(x) will, among others potentially, do our `dbs_set_logger(x)`.
}

} // namespace ipc::shm::arena_lend::detail
