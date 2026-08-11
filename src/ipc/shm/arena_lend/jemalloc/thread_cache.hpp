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
#include "ipc/shm/arena_lend/jemalloc/detail/jemalloc_fwd.hpp"
#include "ipc/shm/arena_lend/arena_lend_fwd.hpp"
#include "ipc/common.hpp"
#include <flow/util/action_registry.hpp>
#include <flow/util/thread_lcl.hpp>
#include <flow/log/log.hpp>
#include <boost/unordered_map.hpp>
#include <boost/unordered_set.hpp>
#include <string>
#include <atomic>

namespace ipc::shm::arena_lend::jemalloc
{

/**
 * jemalloc central thread cache manager, maintaining an explicitly identified tcache per allocating thread per
 * arena.  While a public-API part of SHM-jemalloc, the following is true:
 *   - When using SHM-jemalloc, Thread_cache will be used automatically without need for the user to mention it
 *     or even enable it.  Perf should be maximized in this context automatically.
 *   - You actually can use it directly for your own needs, and in fact it is not limited to supporting allocation
 *     in shared memory (SHM); it will work equally well with the regular heap as is the jemalloc default.
 *     - However, in that case, it is probably best to either:
 *       - never use #S_NO_ARENA_ID as argument to the key method id(); or
 *       - use it explicitly but not use destroy_arena_safely().  The reason will likely be apparent on perusing
 *         doc headers for id() and destroy_arena_safely() and/or the below Background section.
 *
 * ### Terminology ###
 * This might be a bit confusing; our apologies for that in advance.
 *   - The term "thread cache object" or "thread cache" refers to a Thread_cache object.  However, such an object
 *     shall maintain one actual jemalloc *tcache* per arena (as specified to id() method) per thread.
 *   - The term "tcache" shall refer to a single jemalloc tcache (a/k/a tcache ID a/k/a #tcache_id_t value).
 *
 * Thus one *Thread_cache object* or *thread cache* shall hold handles to 0 or more jemalloc *tcaches*
 * (to be of any use, for a given thread, 1 or more).  Naturally there shall be one cache object per thread
 * (or none, if you never call id() in a given thread).
 *
 * ### Background: arena versus tcache relationship in jemalloc ###
 * The following information is surprisingly obscure, in that the main jemalloc docs do not state it clearly,
 * and it does not appear to be common knowledge.  However it can be found in links like
 * https://github.com/jemalloc/jemalloc/issues/460.  To wit:
 *
 * Consider one given thread T.  If you use at least two arenas A and B, and *one* jemalloc tcache (ID) X,
 * for your jemalloc `mallocx()` and `dallocx()` calls, then beware that it is entirely possible that
 * the call `p = mallocx(..., MALLOCX_ARENA(B) | MALLOCX_TCACHE(X))` will yield vaddr `p` belonging to
 * an extent (vaddr range) in arena A!
 *
 * This is (1) surprising to at least some; (2) possibly okay if the arenas are in heap, and/or it does not
 * matter where something is allocated other than maybe perf; (3) definitely not okay in at least some contexts
 * such as SHM-jemalloc's needs (and arguably SHM in general).
 *
 * That is why, unless one only uses #S_NO_ARENA_ID, Thread_cache maintains a stringent one-to-one (in a given thread)
 * relationship between #arena_id_t values passed to id() + id_or_none() and the resulting returned
 * #tcache_id_t values.  Thus a given `id()` shall be used with only 1 jemalloc-arena, and there is no danger
 * of seeing the aforementioned arguably-surprising behavior.
 *
 * ### Background: jemalloc tcache mechanism ###
 * For brevity let's pretend there are just the jemalloc alloc-function `p = a(tc)`,
 * which returns a `void*` to a buffer of some constant size, w/r/t tcache with ID `tc`; and `d(p, tc)` which
 * deallocates; all from some agreed-upon single arena.  Also let `a()` and `d(p)` be the no-tcache versions.
 * (In reality these are jemalloc's `mallocx()` and `dallocx()` with the APIs shown in the next section.)  Let
 * us assume also 2 threads T1 and T2 with respectively already-created tcache-IDs `tc1` and `tc2`.
 *
 * First consider the no-tcache scenario.  `p = a()` might be called in T1 and, perhaps, `p2 = a()` or `d(p2)`
 * simultaneously in T2.  jemalloc's internal what's-available-in-arena data structure -- call this `ARENA` --
 * basically knows the various values `a()` is allowed to return -- some are still available, others are taken,
 * and the structure is perhaps a bitmap, but no matter: Basically, every possible `p` value it could ever
 * return is initially *owned* by `ARENA`.  Essentially this `ARENA` then has 2 ops: `p = ARENA.a()` and
 * `ARENA.d(p)`; these un-own that `p` and give them to the invoker-entity and, respectively, the opposite
 * (re-own it).  So then `a()` is simply `ARENA.a()` and `d(p2)` is `ARENA.d(p2)`.  However, such things can
 * happen concurrently from T1 and T2, so `ARENA.*()` both have to bracket themselves with a lock-section on
 * mutex `ARENA.mtx`.  Simple -- but there can be `ARENA.mtx` contention.
 *
 * Now with tcache: At its core, which is what we describe here, a given tcache `tc` is more-or-less a
 * `Set<void*>`: you can `tc.add(p)` and `tc.take(p)`.  In reality in jemalloc it's a stack, but we don't care here;
 * let's just say `tc.take()` grabs a random `void*` and returns it, unless empty.
 *
 * Initially `tc1` and `tc2` are empty.  Say we now stop dealing directly with `a()` and `d()` and instead
 * now will do only `a(tc...)` and `d(..., tc...)`, meaning we will now go through tcaches.  To wit: say we
 * start over and do in thread T1: `p = a(tc1)`.  This goes through `tc1`; but we cannot do `p = tc1.take()` yet:
 * `tc1` is empty.  So jemalloc *seeds* `tc1`: it performs many -- let's say 100 -- `ARENA.a()`s and places
 * the resulting pointers into `tc1`.  (Since these all happen at one time, `ARENA.mtx` lock can be across all
 * 100 of these ops.)  We aren't done yet though: `ARENA` no longer owns these pointers, but the user (=we) does
 * not own them either yet.  However, now `tc1` is non-empty, so `a(tc1)` can proceed and `tc1.take()` 1 of the
 * 100 ptrs -- now `tc1` un-owns it while owning the other 99 still -- but we (the user) get it and own it:
 * `a(tc1)` returns that ptr.  If we were to do 50 more `a(tc1)`s right now, each one sees a non-empty
 * (and thread-local!) `Set tc1`, performs a lock-free `tc1.take()`; done: no mutex touched.  (Had this been
 * done without tcache involvement, there would've been 51 locks/unlocks total so far -- instead of 1.  Thus
 * the perf win.)  Further, `d(p, tc1)` simply does `tc1.add(p)` -- in our minimalistic model this cannot fail.
 * If we do that now, we then now own 50 ptrs, `tc1` now owns 100-1-50+1=50; and `ARENA` owns the rest.
 * The key point: the 4 *own-sets* (us, `tc1`, `tc2`, `ARENA`) are always *disjoint*.  If generalized, the
 * disjoint *own-sets* are (us, disjoint `tc*` tcaches, `ARENA`).
 *
 * Naturally, similar stuff can be applied to `tc2` -- so far we've kept it empty, but we could do similar
 * ops on it.
 *
 * With no loss of generality, we can kick around a given `p` between us, `tc1`, `tc2`.  We don't interact
 * directly with `ARENA` anymore -- it just feeds the `tc`s as-needed, when we want to do `tc.take()` on
 * an empty `tc` (as part of `a(tc)`) -- but `p` "travels" (ownership-wise) between us, `tc1`, `tc2`, `ARENA`.
 * (There is also a `flush(tc)` op which is the opposite of the *seed* op: lock `ARENA.mtx`, do N `p = tc.take()`s +
 * N `ARENA.d(p)`s, unlock.)
 *
 * @note Reminder: The only restriction: A `tc` is strictly thread-local.  So `a(tc2)` and `d(p, tc2)` can *only*
 *       be done in thread T2; trying it in T1 => crash.  Again, though, that aside -- a pointer can be
 *       allocated from any `tc` and deallocated into any `tc`.
 *
 * Crucially, the only ops that actually touch `ARENA` -- therefore lock `ARENA.mtx` -- are *seeding*
 * and `flush()`ing.  Seeding occurs automatically within jemalloc, at least roughly as described above.
 * Flushing is intentional -- we provide flush_tcache() API -- but typically is not necessary to do explicitly.
 * (We are agnostic as to whether jemalloc might sometimes do it automatically.)
 *
 * So: that is how it works.  `a(tc)` and `d(p, tc)` -- really
 * `mallocx(sz, MALLOCX_ARENA(arena_id) | MALLOCX_TCACHE(tc))` and
 * `dallocx(p, MALLOCX_ARENA(arena_id) | MALLOCX_TCACHE(tc))` respectively -- use the `tc`-identified tcache as a
 * thread-local intermediary pointer-store, allocating from/deallocating into which is faster since
 *   - deallocating requires no mutex-locking;
 *   - allocating rarely requires mutex-locking, batching many pointer-grabs-from-`arena_id` under a single
 *     mutex-lock (as needed).
 *
 * @note Already you might note the reality is more complex than our described model; `sz` alone -- as opposed
 *       to our model's single constant size -- complicates things.  However, this is not at its core that important.
 *       (Spoiler alert: It's a per-size-class setup.)  Other details can also be omitted here (but may well be
 *       important when tuning for perf).
 *
 * Incidentally it may now be clear(er) why intermixing 2 arenas and 1 tcache can (will) lead to trouble, at least
 * if one, black-boxily, expects `mallocx(..., MALLOCX_ARENA(arena_id))` to obtain a ptr from within
 * `arena_id`-arena and not another one.  For example: say we did some allocs from arena A1 and A2, thus seeding
 * the cache with 100 ptrs from A1 and 100 ptrs from A2.  Now say we do an alloc, through the given tcache,
 * from A2.  The tcache just stores pointers; does not know or care from which arena each one originated.  So
 * jemalloc looks in the tcache... it is not empty... it does `.take()`... and that returns a random ptr from the
 * 100+100=200 in there... but that random one may have come from A1: we said A2 though.  Thus the issue.
 * Hence in Thread_cache (reiterating) it is (outside of #S_NO_ARENA_ID) strictly 1-to-1 between arena IDs and
 * tcache IDs (per thread).
 *
 * ### How to use Thread_cache directly ###
 * Suppose you are in thread T and would like to allocate (jemalloc `mallocx()`) and deallocate (`dallocx()`)
 * while using an explicit jemalloc tcache.  We support only the use case wherein in both cases one passes
 * a `flags` argument that includes `MALLOCX_ARENA(arena_id)` for some user-created `arena_id` (256 or higher, it
 * appears, as of this writing).  Per jemalloc API, to use thread caching:
 *   - With `mallocx()`: In addition to `MALLOCX_ARENA(arena_id)`, add-in the mask `MALLOCX_TCACHE(tcache_id)`, where
 *     you shall obtain `tcache_id_t tcache_id` from a `*this`.
 *   - With `dallocx()` for the address returned by `mallocx()`: Similarly.  That said:
 *     - If you're in the same thread as where you did the `mallocx()`: Simple as it gets; use the
 *       same `tcache_id`.
 *     - If you're in a different thread:
 *       - Do *not* try to reuse the memorized `tcache_id` from the original thread.
 *       - Obtain the `tcache_id_t` for this current/different thread and use that instead.
 *       - Alternatively: forego tcache use: `MALLOCX_ARENA(arena_id) | MALLOCX_TCACHE_NONE`.
 *
 * As should be clear from the preceding section, there is nothing wrong with `TCACHE_NONE` (forgoing tcache)
 * when deallocating, even if it came from a tcache when allocating.  It would just mean doing the full
 * arena-mutex-lock/return-pointer/arena-mutex-unlock op.  Naturally, all else being equal, that is slower;
 * so why do it?
 *
 * Answer: Suppose there is no tcache in the current thread when deallocating.  `this_thread_cache()->id(arena_id)`
 * creates it: no problem.  However, it is this creation that, conceivably, could itself be dangerous.  That we
 * know of, there is at least one danger-zone: around thread exit; namely `*(this_thread_cache())` might have
 * been cleaned-up already.  It should be safe in some cases; the `flow::util::Thread_local_state_registry`
 * machinery we use to implement cleanup-at-thread-exit will do stubborn-cleanups, so if you're using
 * the same facility for *your* per-thread cleanup (wherein you attempt deallocation)
 * (`Thread_local_ptr` or `boost::thread_specific_ptr` = also fine) it should interoperate properly, briefly
 * re-creating Thread_cache + tcache and quickly destroying them again.  Otherwise it depends on the per-thread
 * deinit order; get it wrong, and it'll probably crash.  Even the "it should be safe" situation feels
 * dicey/entropy-laden, so informally we recommend against that too.  So: at least during thread-deinit, it is
 * prudent to avoid deallocating via this_thread_cache() and id(); use this_thread_cache_or_null() and id_or_none()
 * instead (see just below).
 *
 * So that leaves the question: what #tcache_id_t value to use?  Answer:
 *   -# Call `static` Thread_cache::this_thread_cache() which returns a pointer `x` to a thread-local Thread_cache*
 *      (cache object).  It shall be created the first time it is called (in a given thread T).
 *   -# Call `x->id(arena_id)`, where `arena_id` is the arena ID you intend to pass to `mallocx()`
 *      (and probably `dallocx()`).  This shall return the `id` to use.  It shall be generated the first time it
 *      is called (in a given thread T) for that particular `arena_id`.
 *      - Internally this means `mallctl("tcache.create")` is invoked that first time, and the result is stored
 *        internally in thread-local storage, in a map by arena ID.
 *   -# Pass the resulting `tcache_id_t id` to `mallocx()` and (ideally) `dallocx()` in thread T.
 *
 * In the case where you want to use the tcache, but only if it exists (we recommend this above during
 * danger-zones: around thread exit): this_thread_cache_or_null() to `x`; if not null: `x->id_or_none()`.
 * If that returns an ID (not #S_NO_TCACHE_ID): use that ID.  In the other 2 cases: use `MALLOCX_TCACHE_NONE`.
 *
 * You may also obtain the thread unique-token via thread_token() -- in a sense a replacement for
 * `flow::util::this_thread_unique_token()` -- as well as thread_nickname() (useful for logging and reporting).
 * We use `Thread_token` instead of `Thread_id` (<=> `this_thread::get_id()`) because of its uniqueness over time
 * which `Thread_id` lacks (OS thread IDs are recycled after thread exit).  This can be useful algorithmically,
 * whereas outside logging/monitoring `Thread_id` can lead to subtle trouble.
 *
 * ### Important: Destroying arenas safely ###
 * The following is non-obvious to new jemalloc users (though the main doc page does say it explicitly).
 * If you have ever used tcache X with arena A, and you want to destroy arena A (presumably to return RAM/etc.),
 * then you *must* first flush (and if your tcaches and arena are 1-to-1, as Thread_cache assumes, probably
 * destroy -- which also auto-flushes) tcache X.  If you do not:
 *   - Trying to flush or destroy X *after* arena A is destroyed will probably crash inside jemalloc.
 *   - Trying to use X for further allocations/deallocations (especially deallocations, it appears empirically)
 *     after A is destroyed -- even with some other arena B -- will probably crash inside jemalloc.
 *
 * This is a surprisingly stringent requirement.  It is also, normally, potentially difficult to accomodate
 * because:
 *   - The call to destroy an arena A (`mallctl()`, etc.) will of course have to happen in some specific thread Y...
 *   - ...but there may be tcaches associated with A in threads other than Y -- possibly many threads -- depending
 *     on where you were allocating stuff until then.
 *   - Therefore you'll need to do tcache-destroying for tcache ID X in 1+ threads... and only then is it safe
 *     to destroy the arena A.
 *   - (If you just let everything live until after `main()`, then you're fine.)
 *
 * However we have provided a facility to make this straightforward.  When you need to destroy arena A, and
 * you have used Thread_cache to allocate/deallocate objects, across various threads, in A, simply call
 * `static` Thread_cache::destroy_arena_safely(), passing to it the #arena_id_t A.  We shall take care of the
 * needed tcache-destroying as soon as possible (but without starting any background threads, much like jemalloc
 * does things -- opportunistic piggy-backing); and upon destroying the last relevant tcache, Thread_cache
 * will internally actually destroy arena A.
 *
 * ### Flushing cache manually ###
 * You may call `x->flush_tcache(id)`, from the thread to which `*x` belongs (where you called
 * Thread_cache::this_thread_cache()), to synchronously flush the tcache `id`.
 */
class Thread_cache :
  public flow::log::Log_context_mt,
  private boost::noncopyable
{
public:
  // Constants.

  /**
   * Value to always pass to id() or id_or_none(), if you do not wish for Thread_cache to worry about 1-to-1
   * mapping of tcaches and arenas (in a given thread).  That is e.g. use it if you're keeping 1 tcache per thread,
   * period, and either
   *    - are willing to deal with subtleties arising from how a tcache works when used with 2+ arenas, or
   *    - you only use one arena, period.
   *
   * @see Thread_cache doc header "Background: arena versus tcache relationship."
   */
  static constexpr arena_id_t S_NO_ARENA_ID = 0;

  /// Special value returned by id_or_none() indicating no tcache yet exists for the given #arena_id_t.
  static constexpr tcache_id_t S_NO_TCACHE_ID = tcache_id_t(-1);

  // Constructors/destructor.

  /**
   * Constructs cache for this thread as invoked internally by this_thread_cache().
   *
   * @warning Do not directly construct a `*this`.  It is public only as a requirement for
   *          `flow::util::Thread_local_state_registry` which we use internally.  Behavior is undefined
   *          if you break this prohibition.  Always use `static this_thread_cache()` instead (it performs
   *          lazy construction per thread).
   *
   * @param logger_ptr
   *        Logger to use for logging subsequently.
   *
   * @internal
   *
   * `logger_ptr` is taken from Static_state::m_caches.
   *
   * @todo The doc-header warning for the the Thread_cache::Thread_cache() ctor could be removed, if
   *       (ideally) `flow::util::Thread_local_state_registry` could compile-time-check for existence of
   *       a public such ctor and avoid compiling (optional) code that would use it -- and in
   *       static-init of Static_state::m_caches we supplied a `new Thread_cache` optional-arg in that `private`
   *       context; or (less ideally) if we made a facade sub-class of Thread_cache that would expose this ctor but only
   *       to `Thread_local_state_registry`.  For now the ctor is public but with that `@warning` and this to-do.
   */
  explicit Thread_cache(flow::log::Logger* logger_ptr);

  /**
   * Destructor that runs in each thread (potentially even after `main()` -- the thread had not been joined by then);
   * it does its best to flush and destroy all tcaches created via id() in that thread.
   */
  ~Thread_cache();

  // Methods.

  /**
   * Returns pointer to the one cache-object in the calling thread, first creating it if it is the first
   * such call.  This does not necessarily create (nor return) a tcache -- only a Thread_cache.
   * However the next step is usually to in fact return/create-on-demand an actual tcache: call
   * id() (or id_or_none()) on the returned Thread_cache.
   *
   * @return See above.
   */
  static Thread_cache* this_thread_cache();

  /**
   * Returns pointer to the one cache-object in the calling thread, or null if none exists (this_thread_cache()
   * has not yet been invoked).
   *
   * @see this_thread_cache() for the lazy-init variation.  See class doc header for some suggestion(s) as
   *      to how this_thread_cache_or_null() and id_or_none() are useful.
   *
   * @return See above.
   */
  static Thread_cache* this_thread_cache_or_null();

  /**
   * Returns tcache ID of the tcache in this thread with a 1-1 relationship to the given arena,
   * creating the tcache on the first such call for a given distinct `arena_id` in this thread.
   *
   * @see class doc header for explanation as to how to use that with jemalloc alloc and dealloc APIs.
   *
   * If you prefer to maintain 1 tcache per thread, so that mapping (if any) between arena and tcache (in
   * this thread) is left to you, use `arena_id` set to #S_NO_ARENA_ID.
   *
   * @param arena_id
   *        See above.  Either a value as from `mallctl("arena.create")` or #S_NO_ARENA_ID.
   * @return ID of tcache ID usable in this thread (and this thread only).
   */
  tcache_id_t id(arena_id_t arena_id);

  /**
   * Returns tcache ID of the tcache in this thread with a 1-1 relationship to the given arena,
   * or special value #S_NO_TCACHE_ID, if no such tcache has yet been created via id().
   *
   * @see id() for the lazy-init variation.  See class doc header for some suggestion(s) as
   *      to how this_thread_cache_or_null() and id_or_none() are useful.
   *
   * @param arena_id
   *        See above.  Either a value as from `mallctl("arena.create")` or #S_NO_ARENA_ID.
   * @return ID of tcache ID usable in this thread (and this thread only); or #S_NO_TCACHE_ID.
   */
  tcache_id_t id_or_none(arena_id_t arena_id) const;

  /**
   * Unique-token of thread in which `*this` was originally created via this_thread_cache() call.
   * Can be used as a replacement for `flow::util::this_thread_unique_token()`.
   *
   * @return See above.
   */
  flow::util::Thread_token thread_token() const;

  /**
   * Thread nickname, as from `flow::log::Logger::set_thread_info()`, corresponding to thread in which `*this`
   * was originally created via this_thread_cache() call.
   *
   * @return See above.  Recall that this can be the empty string, if the thread nickname was never set
   *         (which occurs automatically for flow.async-create threads; or if one explicitly calls
   *         `flow::log::Logger::this_thread_set_logged_nickname()`).  Can be cross-referenced with
   *         thread nicknames logged by flow.log.
   */
  const std::string& thread_nickname() const;

  /**
   * Flushes the given tcache as specified by its ID.  You may obtain said ID, among other ways, by
   * invoking `id_or_none(arena_id)` for some `arena_id`.
   *
   * @param id
   *        The tcache to flush.  Behavior is undefined if `id` is invalid.
   *
   * @todo jemalloc::Thread_cache internals are as of this writing intentionally intolerant of
   * failed jemalloc calls, such as due to getting a bad ID, essentially because in practice it was not
   * important to contend with such scenarios; but it'd be nice to be more resilient about it nevertheless.
   * As of this writing jemalloc errors within Thread_cache cause a logged message, assertion-trip if enabled,
   * and finally a hard `std::abort()` if the assertion-trip attempt did not do it.  Update: We explicitly
   * make an exception (no pun intended) to this in the case of arena-destruction failing; that throws.  See
   * destroy_arena_safely() for details.
   */
  void flush_tcache(tcache_id_t id);

  /**
   * Sets the `Logger` to use (or null to skip logging) subsequently for logging in `static` calls;
   * non-`static` Thread_cache calls in existing objects in all threads; and same for future-spawned such
   * objects in all threads.
   *
   * This is safe against concurrent logging in other threads.
   *
   * @see It participates in arena_lend::set_logger(), so you may not need to call it directly.
   *
   * @note Since Thread_cache may log after `main()`, even before `main()`,
   *       and in parts of `main()` when no `Logger` is likely available typically, this may be more subtle
   *       than dealing with setting the logger usually is.  E.g., one might
   *       bracket much of `main()` with a `caches_set_logger(X)` and `caches_set_logger(nullptr)`, where
   *       `X` points to a `Logger` that is constructed near the top and destroyed near the bottom of `main()`.
   *
   * @param logger_ptr
   *        Logger to use for logging subsequently.  As usual null is allowed and turns off logging.
   */
  static void caches_set_logger(flow::log::Logger* logger_ptr);

  /**
   * Destroys (as by `mallctl("arena.X.destroy")`) the given jemalloc arena, as soon as it is safe without
   * causing stability problems w/r/t tcaches currently tracked by Thread_cache in *all* threads.
   * If it is safe (e.g., there are no relevant thread caches; or there is 1, but it belongs to the calling thread
   * and can therefore be flushed/destroyed immediately) then the arena is destroyed immediately (synchronously).
   *
   * Either way -- including the synchronous case -- immediately after destroying the arena, invokes
   * `on_done_func(log_ctx, F)`.
   *
   * @warning There is a jemalloc-endemic (at least in 5.2.1 through 5.3.1, the latter being the newest version as
   *          of 4/2026) problem wherein invoking a jemalloc stats-dump (jemalloc API `malloc_stats_print()`)
   *          concurrently to arena-destruction (as sooner-or-later invoked by the present method) or
   *          arena-creation can trigger a jemalloc abort (`<jemalloc>: Failure in xmallctlbymib()` or similar).
   *          The present method guards against it via tight global-mutex at actual-arena-destruction time;
   *          but this is only effective if the other ops -- especially stat-dumping itself -- do the same.
   *          Therefore we recommend using Memory_manager::create_arena() and `Memory_manager::...stats_dump...()`.
   *          (Incidentally Memory_manager::destroy_arena() simply forwards to us, so for cosmetic reasons that one
   *          might be good to call.)  Save yourself some pain.
   *
   * `log_ctx` to `on_done_func()` shall be as follows:
   *   - If invoked synchronously: it will equal the `log_ctx` from the caller to this method.
   *     Since it was given to us as safe to use synchronously, it should continue to be safe in synchronously-called
   *     handler.
   *   - If invoked asynchronously (other thread): it will be one consistent with caches_set_logger().
   *     By then the `log_ctx` given to this method may be invalid; so we give it our valid one.
   *
   * More importantly: `F()` shall destroy the arena in question, so you should synchronously invoke it.  Do note that
   *   - `on_done_func()` can be called in any thread within which this_thread_cache() has been called;
   *   - but only once total;
   *   - possibly even while the program is exiting;
   *   - never in parallel with *another* `on_done_func()` from *another* destroy_arena_safely() call.
   *     - We shall arrange synchronization on your behalf among the `on_done_func()` calls.
   *
   * ### Exception possibility ###
   * If the actual arena destruction, which occurs only within `F()` passed to `on_done_func()` and therefore
   * only in `on_done_func()` via your explicit call of `F()`, fails -- if jemalloc reports such failure, that is --
   * then `F()` shall throw `flow::error::Runtime_error` with appropriate details.  In practice we only know of this
   * occurring if `arena_id` is invalid.
   *
   * ### Mechanism ###
   * If the destruction must be deferred due to other threads' tcaches, it shall occur at the earliest possible
   * future this_thread_cache() or this_thread_cache_or_null() call.  Each such call shall (opportunistically)
   * flush/destroy as many relevant tcaches as are accessible in that thread; and the last such needed
   * procedure will then finalize by destroying the arena `arena_id`.
   *
   * Since the arena and all relevant tcaches apply only to `arena_id`, none of it should affect the rest of
   * your work.  In terms of perf, any slowdown shall be only due to the destroying/flushing itself -- which
   * presumably will be quite rare -- while the fast-path (wherein in no such work is required) is a single
   * atomic-check per `this_thread_cache*()`.
   *
   * ### Suggestion for `on_done_func()` ###
   * Destroying the jemalloc arena is not a drop-everything-suddenly type of operation.  It will potentially write
   * to currently mapped vaddr-segments a/k/a extents a/k/a pools; it will certainly try to operate on extents
   * such as to close/unmap them; and in particular this will invoke various extent hooks, if you've registered
   * on on the arena `arena_id`.  Bottom line: until the arena is actually destroyed, jemalloc may offically
   * require that certain resources that *you* potentially registered with it using the extent-hook system --
   * SHM-pools, extent hooks themselves -- still be live.  Therefore it would then make sense to capture
   * such items in `on_done_func` functor object (perhaps lambda) and only let them expire once the functor
   * is invoked.
   *
   * As for the `const Log_context_mt*` arg passed to `on_done_func()`: It is possible that by the time the
   * arena can be safely destroyed, the `Logger` (if any) that your `on_done_func()` is capturing would use is no
   * longer valid; this gives you the opportunity for `on_done_func()` to log-through a known-safe
   * `Log_context_mt` (recommend `FLOW_LOG_SET_LOCKED_CONTEXT()`).  (It's of course optional; just an informal
   * point.)  Having done that one would typically invoke `F()` which would destroy the arena (and perhaps lead
   * to synchronous extent-hook-driven behavior inside `F()`).
   *
   * ### Design rationale: Why `Log_context_mt*`?  Why not just `Logger*`? ###
   * In practice destroy_arena_safely() can be called at touchy times: past `main()`, during thread exit -- that
   * kind of thing -- and even supposing not, `on_done_func()` might be asynchronously called at some unpredictable
   * time.  We've found by then -- even right at this call, but equally or more so in async-`on_done_func()` --
   * it is quite difficult to have the user ensure their `Logger` is still around.  Even communicating this
   * requirement is hard, but assuming the documentation aspect is no issue, it just becomes an onerous/impossible
   * thing.  However `Log_context_mt` does support user's concurrently changing the `Logger*`, and:
   *   - Synchronously logging in this method, we can then support that use-case.  If it is not required, it is
   *     trivial for the caller to pass-in a not-really-concurrently-mutable `Log_context_mt` containing
   *     a synchronously-valid `Logger*`.
   *   - Asynchronously logging from another thread, we cannot count on the user-given `Log_context_mt` anymore;
   *     but we can offer our own.
   *
   * So it allows for the maximally required flexibility; and if no flexibility is required, then just make a
   * nominal `Log_context_mt` with whatever logger you've got (null is fine too) and pass that in as `log_ctx`.
   *
   * @see class doc header section about this topic for key background.
   *
   * @tparam On_done_func
   *         Function object that matches the signature: `void (const Log_context_mt*, auto&& destroy_arena_func)`; and
   *         `destroy_arena_func` is to be invoked as: `destroy_arena_func()`.
   * @param arena_id
   *        Arena to destroy.  Do not pass #S_NO_ARENA_ID.
   * @param log_ctx
   *        Potentially-concurrently-mutable `Log_context_mt` to use for synchronous logging.
   * @param on_done_func
   *        See above.
   */
  template<typename On_done_func>
  static void destroy_arena_safely(arena_id_t arena_id, const flow::log::Log_context_mt* log_ctx,
                                   On_done_func&& on_done_func);

private:
  // Types.

  /**
   * For each arena currently being destroyed via destroy_arena_safely(), the state tracking the progress
   * of that operation.
   */
  struct Arena_death_progress
  {
    // Data.

    /**
     * Which Thread_cache extant objects have yet to invoke
     * if_requested_destroy_tcaches_and_possibly_finish_arena_kills() before the arena can be destroyed safely.
     */
    boost::unordered_set<Thread_cache*> m_caches_left_to_destroy;

    /**
     * Functor to invoke, and then immediately destroy, once #m_caches_left_to_destroy is `.empty()`,
     * and therefore can actually destroy the arena.
     */
    Function<void (const flow::log::Log_context_mt*)> m_on_done_func;
  }; // struct Arena_death_progress

  /// Bundles `static` state used among the instances of this class; this allows us to ensure order of init/de-init.
  struct Static_state
  {
    // Constructors/destructor.

    /// Constructor.
    Static_state();

    // Data.

    /// Protects against concurrent execution of Arena_death_progress::m_on_done_func.
    flow::util::Mutex_non_recursive m_arenas_death_row_map_on_done_func_mutex;

    /**
     * State shared -- and acted upon using the new `Polled_shared_state` pattern -- between extant Thread_cache
     * per-thread objects, consisting of which of them must destroy relevant tcaches in order to finish
     * 1+ destroy_arena_safely() ops.
     *
     * Each key is an arena being destroyed; that key's value is the progress state of what is yet to be done
     * before completing the destruction of that arena fully.  The star is
     * Arena_death_progress::m_caches_left_to_destroy: set of Thread_cache objects that
     * must, at the next opportunity (this_thread_cache(), this_thread_cache_or_null(), dtor calls in the respective
     * threads) destroy the tcache (if any) corresponding to that arena ID; and if doing so
     * makes the set empty finally destroy_arena() for good.
     *
     * That operation is if_requested_destroy_tcaches_and_possibly_finish_arena_kills() and is gated
     * on `Thread_cache::m_destroy_tcaches_requested_poll_flag.poll_armed()` returning `true`; the fast-path is it
     * returns `false`, and thus there is nothing for that method to do.
     */
    flow::util::Polled_shared_state<boost::unordered_map<arena_id_t, Arena_death_progress>> m_arenas_death_row_map;

    /// The work-horse: registry of per-thread Thread_cache objects, each holding 0+ jemalloc tcaches.
    flow::util::Thread_local_state_registry<Thread_cache> m_caches;
  }; // struct Static_state

  // Friends.
  friend std::ostream& operator<<(std::ostream&, const Thread_cache&);

  // Methods.

  /**
   * Perform deferred cross-thread tcache and arena-destruction as triggered by destroy_arena_safely() call(s)
   * made earlier.  This is piggy-backed onto this_thread_cache() and this_thread_cache_or_null().
   *
   * @note The fast-path -- wherein no deferred tcache/arena destruction processing is in fact required -- is
   *       intentionally ultra-minimal computationally.  See #m_destroy_tcaches_requested_poll_flag and such.
   *
   * @param assume_requested
   *        If and only if `true` act as-if the poll-flag is armed; do not check it or attempt to disarm it.
   *        This is to be used if and only if invoked from dtor.  (Thread exit is infrequent, and the scan
   *        is cheap when nothing is pending; so it is simpler to just do it than reason about flag state.
   *        Worst-case, and this is fairly likely, it'll merely lead to a no-op.)
   */
  void if_requested_destroy_tcaches_and_possibly_finish_arena_kills(bool assume_requested = false);

  /**
   * Flushes/destroys the given tcache.
   * @param id
   *        See id().
   */
  void destroy_tcache(tcache_id_t id);

  /**
   * Destroys the given arena now.  Protects against detail::jemalloc_arena_list_mutex() issue (see its doc header).
   * @param arena_id
   *        See destroy_arena_safely().
   * @param log_ctx
   *        See destroy_arena_safely().
   */
  static void destroy_arena(arena_id_t arena_id, const flow::log::Log_context_mt* log_ctx);

  // Data.

  /// See doc header and especially member doc headers for Static_state.
  static Static_state s_state;

  /// Poll-flag corresponding to Static_state::m_arenas_death_row_map `Polled_shared_state` pattern.
  flow::util::Poll_flag m_destroy_tcaches_requested_poll_flag;

  /// See thread_token().
  flow::util::Thread_token m_thread_token;

  /// See thread_nickname().
  std::string m_thread_nickname;

  /**
   * See id(): for this thread, maps arena ID (that has been passed to
   * id() at least once) to the jemalloc tcache dedicated to that arena ID that was created by `*this`
   * as a result of that first `id(arena_id)` call.
   */
  boost::unordered_map<arena_id_t, tcache_id_t> m_tcaches_by_arena;
}; // class Thread_cache

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename On_done_func>
void Thread_cache::destroy_arena_safely(arena_id_t arena_id, const flow::log::Log_context_mt* log_ctx, // Static.
                                        On_done_func&& on_done_func_arg)
{
  using flow::util::this_thread_unique_token;

  FLOW_LOG_SET_LOCKED_CONTEXT(log_ctx);


  decltype(Arena_death_progress::m_on_done_func) on_done_func
    = [on_done_func_arg = std::move(on_done_func_arg), arena_id]
        (auto log_ctx) mutable
  {
    // As promised protect against concurrent on_done_func()s from different destroy_arena_safely()s.
    flow::util::Lock_guard<decltype(s_state.m_arenas_death_row_map_on_done_func_mutex)>
      lock{s_state.m_arenas_death_row_map_on_done_func_mutex};

    on_done_func_arg(log_ctx, [&]()
    {
      destroy_arena(arena_id, log_ctx);
    });
  };
  assert(!on_done_func.empty());
  /* If by the end of the function on_done_func is still not .empty(), we'll execute it synchronously to destroy arena!
   * Otherwise we've saved it, to invoke/delete once this can be done safely. */

  s_state.m_caches.while_locked([&](const auto& state_per_thread)
  {
    if (state_per_thread.empty())
    {
      assert(!on_done_func.empty()); // Will run it immediately!
      return;
    }
    // else

    s_state.m_arenas_death_row_map.while_locked([&](auto* arenas_death_row_map)
    {
      Arena_death_progress& arena_death_progress = (*arenas_death_row_map)[arena_id];

      arena_death_progress.m_on_done_func = std::move(on_done_func);
      on_done_func.clear(); // Just in case the move() didn't do it.

      for (const auto& [cache, nil] : state_per_thread)
      {
        arena_death_progress.m_caches_left_to_destroy.insert(cache);
        /* It means: "Yo, `cache`, if you've got a tcache for `arena_id`, destroy it; but either way you-dun-good,
         * so remove yourself from s_state.m_arenas_death_row_map[arena_id].
         * And if that makes s_state.m_arenas_death_row_map[arena_id] empty (you're the last one to have dun-good), then
         * finish what we started: execute, and forget, m_on_done_func() (this will destroy_arena(arena_id))." */
      }
    }); // s_state.m_arenas_death_row_map.while_locked()

    for (const auto& cache_and_nil : state_per_thread)
    {
      /* Alias: FLOW_LOG_..._LOCKED() messages below and other code may not name structured bindings (C++17).
       * @todo Move to C++20 => Use bindings after all for pithiness.  (Some C++17-mode compilers already OK; not all.)
       *       Remove the @warning about this from doc header of FLOW_LOG_WARNING_LOCKED() (in Flow, not Flow-IPC). */
      auto* const cache = cache_and_nil.first;

      if (this_thread_unique_token() == cache->thread_token())
      {
        FLOW_LOG_INFO_LOCKED("Jem_tcache[" << *cache << "]: "
                             "There was a typically-cross-thread tcache-destruction request "
                             "(arena [" << arena_id << "] being destroyed); "
                             "but we are in the relevant thread *now*, so we shall simply tcache-destroy the "
                             "tcache for that arena (if it exists) and skip the "
                             "flag mechanism; possibly even finish destroying arena.");
        s_state.m_arenas_death_row_map.while_locked([&](auto* arenas_death_row_map_ptr)
        {
          auto& arenas_death_row_map = *arenas_death_row_map_ptr;

          const auto it_arena_and_progress = arenas_death_row_map.find(arena_id);
          assert((it_arena_and_progress != arenas_death_row_map.end())
                 && "What could have possibly already handled this thread's entry that we just inserted?  Bug?");

          /* This next part is kinda similar to the slow-path of
           * if_requested_destroy_tcaches_and_possibly_finish_arena_kills(), but code reuse is not really
           * that obvious.  Let's just do this....  (@todo Maybe break it out into a non-static method though?) */

          auto& arena_death_progress = it_arena_and_progress->second;
          auto& caches = arena_death_progress.m_caches_left_to_destroy;
#ifndef NDEBUG
          const bool ok = 1 ==
#endif
          caches.erase(cache);
          assert(ok && "What could have possibly already handled this thread's entry that we just inserted?  Bug?");

          assert(on_done_func.empty() && "Sanity check of nearby algorithm.");
          if (caches.empty())
          {
            on_done_func = std::move(arena_death_progress.m_on_done_func); // Get it back!
            arenas_death_row_map.erase(it_arena_and_progress); // It is now empty through-and-through.
          }
          // else {}

          const auto it_arena_and_tcache = cache->m_tcaches_by_arena.find(arena_id);
          if (it_arena_and_tcache != cache->m_tcaches_by_arena.end())
          {
            const auto id = it_arena_and_tcache->second;
            FLOW_LOG_INFO_LOCKED
              ("Jem_tcache[" << *cache << "]: Indeed we do hold tcache [" << id << "] for that arena.  "
               "Proceeding with destruction to cross ourselves off the list.  If that makes "
               "the list empty (does it? = [" << (!on_done_func.empty()) << "]), "
               "we shall imminently destroy arena.");
            cache->destroy_tcache(id); // @todo Technically this should be using log_ctx to fully follow contract.
            cache->m_tcaches_by_arena.erase(it_arena_and_tcache);
          }
        }); // s_state.m_arenas_death_row_map.while_locked()
      } // if (this_thread_unique_token() == cache->thread_token())
      else // if (this thread unique-token != cache->thread_token())
      {
        FLOW_LOG_INFO_LOCKED("Jem_tcache[" << *cache << "]: In outside thread (unique-token "
                             "[" << this_thread_unique_token() << "]) setting tcache-destruction-requested "
                             "flag on (arena [" << arena_id << "] being destroyed) for per-thread for thread with "
                             "unique-token [" << cache->thread_token() << "], " // *cache includes these but why not....
                             "nickname [" << cache->thread_nickname() << "].  "
                             "Next time that thread polls for that flag being true, it will tcache-destroy the "
                             "relevant tcache if any and possibly finish destroying arena for real.");
        cache->m_destroy_tcaches_requested_poll_flag.arm_next_poll();
      } // else if (this thread unique-token != cache->thread_token())
    } // for (cache : state_per_thread)
  }); // s_state.m_caches.while_locked()

  if (!on_done_func.empty())
  {
    /* destroy_arena_safely(X) equals on_done_func() (which inside will also destroy_arena(X)) in our case.
     * (To summarize the above as of this writing: no tcaching threads exist yet => no problem; or:
     * one tcaching thread exists... and it is us, so we destroyed the tcache synchronously => no problem.)
     *
     * As promised: Have them -- since this is synchronous -- use the Log_context_mt they gave us
     * to (synchronously) use for various logging seen above. */
    on_done_func(log_ctx);
  }
  // else { It has been deferred, until it is safe. }
} // Thread_cache::destroy_arena_safely()

} // namespace ipc::shm::arena_lend::jemalloc
