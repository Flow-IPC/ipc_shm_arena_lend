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

#include "ipc/shm/arena_lend/owner_shm_pool_collection.hpp"
#include "ipc/shm/arena_lend/jemalloc/memory_manager.hpp"
#include "ipc/shm/arena_lend/jemalloc/stat_info_dump.hpp"
#include "ipc/shm/arena_lend/jemalloc/jemalloc_fwd.hpp"
#include "ipc/shm/arena_lend/shm_pool_offset_ptr.hpp"
#include "ipc/shm/arena_lend/arena_lend_stats.hpp"
#include "ipc/shm/arena_lend/arena_lend_fwd.hpp"
#include "ipc/shm/arena_lend/detail/thread_lcl_obj_db.hpp"
#include "ipc/shm/arena_lend/detail/obj_disposer.hpp"
#include "ipc/shm/stl/stateless_allocator.hpp"
#include "ipc/shm/stl/arena_activator.hpp"
#include "ipc/util/util.hpp"
#include "ipc/util/util_fwd.hpp"
#include <flow/log/log.hpp>
#include <flow/util/util.hpp>
#include <unordered_map>
#include <map>
#include <set>
#include <type_traits>
#include <memory>
#include <vector>

namespace ipc::shm::arena_lend::jemalloc
{

// Types.

/**
 * Represents/manages a segregated collection of in-SHM (shared memory) objects and buffers allocated using
 * the memory manager *jemalloc*, acting according to the SHM-arena-lend design.  It is arguably the central
 * class of SHM-jemalloc, a type SHM-arena-lending SHM-provider in Flow-IPC.  SHM-jemalloc's key objects
 * (Ipc_arena and ipc::session::shm::arena_lend::Shm_session, principally) can be plugged into a
 * general Flow-IPC session, as established via ipc::session machinery; or they can be used on a standalone basis.
 * It is generally significantly easier to do the former: a successfully established `Session` will get you
 * an Ipc_arena (2, even, one of session-scope, another of app-scope) and Shm_session for free (~no additional
 * coding effort is required).  You may access jemalloc::Session::session_shm() (`Ipc_arena*`),
 * jemalloc::Server_session::app_shm() (ditto), and jemalloc::Session::shm_session() (`Shm_session*`).
 *
 * @note These notes (in Ipc_arena doc header) are written from the standpoint of presenting it (and the rest of
 *       SHM-jemalloc) as a standalone system.  In most situations a general Flow-IPC user can think of
 *       SHM-providers, whether SHM-jemalloc or another one (such as SHM-classic in shm::classic), interchangeably.
 *       That said each SHM-provider has certain properties specific to it.  We mention them below under
 *       "Properties of SHM-jemalloc."
 *
 * @see ipc::session::shm::arena_lend::jemalloc::Shm_session, an important class required for sharing objects
 *      constructed via Ipc_arena with other process(es).
 *
 * The key relevant ops are as follows.  Let `*arena` be an Ipc_arena.  Let `*session` be a
 * jemalloc::Shm_session connected to an entity (process, usually not the process holding `*arena`) P.  Both are
 * `shared_ptr`s but never mind for now.  Let `*session2` be the peer object of `*session`, in process P.
 *   - `shared_ptr<T> p = arena->construct<T>(...)`: Constructs a `T{...}` in SHM as *owned* by `*this` arena; jemalloc
 *     is used for allocation of actual buffers.
 *   - `session->lend_arena(arena)`: Makes it possible to share -- *lend* -- `p` (and others) to P.
 *   - `auto blob = session->lend_object(p)`: Registers that `*p` is planned to be *borrowed* by P.
 *     - IPC-transmit the short `blob` to P (call the received copy `blob2`).
 *   - (In opposing process P) `shared_ptr<T> p2 = session2->borrow_object(blob2)`.
 *   - Now: `*p` is garbage-collected *across processes* (ours and P in this scenario; but it is possible to
 *     lend to other processes via other `Shm_session`s, 1 of the latter for each).  Only once the `p` and `p2`
 *     ref-counted-pointer groups each reaches ref-count zero does `*p` get destroyed (`~T()` runs, then
 *     `sizeof(T)` memory is jemalloc-deallocated).
 *
 * ### Interrelated lifetimes of Ipc_arena, its `construct()`ed objects, and lent-through `Shm_session`s ###
 * `Ipc_arena`s are publicly accessed exclusively via `shared_ptr` handles (use create() factory method instead of
 * any direct ctor).  This semantic is used to ensure it will outlive each of the following.
 *   - 1, each (garbage-collectable) object `arena->construct<T>()`ed (in SHM), even while no process has borrowed
 *     it.  Therefore you may picture that after `shared_ptr<T> p = arena->construct<T>(...)`, the `shared_ptr` group
 *     of `p` maintains an invisible `shared_ptr` copy of `arena`, live while the group is live.
 *   - 2, each (also `shared_ptr`-accessed) `Shm_session` object for which one has called `session->lend_arena(arena)`.
 *     Thus picture that the latter call saves a copy of `shared_ptr<Ipc_arena> arena` inside `*session`, live
 *     while `*session` is live.  (The user should let go of `session` if and only if the connection to the opposing
 *     process is lost, and/or we want to imminently end that connection.)
 *
 * Hence, assuming no crashes/etc.:
 *   - A `*this` is required for constructing future objects to be shared via each given `Shm_session`.  Therefore
 *     it outlives all `Shm_session`s through which it has been `lend_arena()`d.
 *   - A `*this` is required for destroying objects it has constructed, and which may or may not have been
 *     lent via 1+ `Shm_session`s.  Therefore it outlives (...see prev bullet...); and it outlives all
 *     `shared_ptr` group(s) returned by `this->construct()`.
 *
 * @todo ipc::shm::arena_lend::jemalloc::Ipc_arena has public methods for end users but also an API, including
 * (e.g.) add_shm_pool_listener(), used by Flow-IPC internals to make things work properly; these should
 * be made non-`public` (accessible by said internals via a facade/attorney/etc. pattern of choice).  A careful
 * effort is required to clean up the API and then classify what's left as either public-facing or otherwise.
 * Suggest also converting currently-used `protected`-for-unit-tests pattern to the facade/attorney/etc.
 * pattern of choice (if necessary a separate facade/whatever could be used for internal-code consumers versus
 * test-code consumers).  E.g., start() is such a test-accessed `protected`-only-for-that-reason
 * item. / Lastly: the public super-class Owner_shm_pool_collection, and its super-class(es), likely expose
 * some public things that too should be gated-off from end-users.
 *
 * ### Cleanup (of in-SHM resources) ###
 * As explained above, a borrowing/lending scheme + GC semantics ensure proper cleanup of `Ipc_arena`-constructed
 * objects.  What about the SHM-pools (a/k/a SHM objects, SHM segments, in-SHM extents) that are obtained from the
 * OS?  (Even if all buffers held in a given SHM-pool have been deallocated, at a lower level RAM may still be
 * earmarked for SHM use on `*this` behalf.  Plus there are vaddr areas that are mapped that may need to be
 * unnmapped.)  Answer:
 *
 * Assuming no crashes occur throughout, here or in borrowing process(es) if any: A `*this` shall only be destroyed
 * when safe (via aforementioned `shared_ptr` techniques).  When this occurs, the Ipc_arena destructor, synchronously
 * or otherwise, shall return/free all resources, including SHM-pool handles (in Linux et al, FDs really), mapped
 * (in Linux et al, `mmap()`ed) vaddr areas, and SHM-pool names in the file-system.  Similarly a borrowing
 * `Shm_session` (also destroyed when safe via `shared_ptr` goodness) shall handle its end of such things
 * (read-only SHM-pool handles, mapped areas).  Once all (well, a subset) of this occurs, the relevant SHM-used
 * RAM is returned to the system for general use.
 *
 * If a crash occurs, the dtor may not run (or its asynchronous aspects may not finish).  The crash itself will
 * return any SHM-pool handles and mapped extents; but a given SHM-pool's name would remain in the file-system;
 * thus the RAM is not returned.  Handling this eventuality is the responsibility of the Ipc_arena (et al) user.
 * In particular by arranging a certain pool-naming scheme -- see `pool_name_base` arg to Ipc_arena::create() --
 * one can opportunistically remove (unlink) dead process-created SHM-pools form the file-system.
 *
 * Happily ipc::session (available in Flow-IPC but optional to use with SHM-jemalloc) has implemented such
 * post-crash opportunistic cleanup (and, relatedly, determines the pool-naming scheme).  It calls
 * Ipc_arena::create() in particular (among other things) for you.  If you don't use ipc::session to establish
 * your `Ipc_arena`(s), though, you would need to handle it yourself (or risk memory leaks).
 *
 * @warning Due to certain implementation details, the `create()`-returned `shared_ptr` group (that points to
 *          `*this`) reaching ref-count-zero concurrently to a *relevant thread* exiting can lead to undefined behavior.
 *          A *relevant thread* is defined as one in this process that has touched a `this->construct<T>()`ed
 *          handle group through any of the following operations: Ipc_arena::construct() itself,
 *          Shm_session::lend_object(), and the *disposer*.  The *disposer* executes when the handle
 *          (#Handle a/k/a `shared_ptr<T>`) group reaches ref-count-zero.  Loosely speaking this means either
 *          join the threads that work with SHM-objects `construct()`ed by `*this` before ending `*this` arena;
 *          or end this arena before joining such threads.
 *
 * ### Properties of SHM-jemalloc (and likely other SHM-arena-lending SHM-providers, if/when they exist) ###
 * Please see the doc header for namespace ipc::shm.  It contrasts the `Ipc_arena`-centered SHM-jemalloc, which
 * is an *arena-lending* provider, to the `Pool_arena`-centered SHM-classic (an *arena-sharing* provider).
 *
 * Spoiler alert: e.g., in our case borrower-side access of a SHM-stored object is read-only.
 *
 * @internal
 *
 * The warning just above, in mentioning "certain implementation details," is actually referring to the limitations
 * on when detail::Thread_lcl_obj_db_admin::forgetting_shm_arena() (ditto `Thread_lcl_obj_db_client`) can be safely
 * called.  This is documented in the doc headers for those two `static` methods.  The danger zone begins when
 * a relevant thread begins shutdown -- `Thread_local_state_registry` removes an `_admin` and/or `_client`
 * (that has worked with `*this` arena's objects) from its extant-TL-states list -- and ends basically when
 * the `_admin` and/or `_client`'s destructor has finished.  This is a brief period called The Gap in some internal
 * documentation in thread_lcl_obj_db.hpp-land.
 */
class Ipc_arena :
  public Owner_shm_pool_collection,
  public std::enable_shared_from_this<Ipc_arena>
{
public:
  // Types.

  /**
   * Fancy pointer type used by ipc::shm::stl::Stateless_allocator.
   *
   * @tparam Pointed_type
   *         The type contained within the pointer.
   */
  template<typename Pointed_type>
  using Pointer = Shm_pool_offset_ptr<Pointed_type, arena_lend::detail::Owner_shm_pool_repository<Ipc_arena>>;

  /**
   * First-class handle -- a `shared_ptr` of some kind by definition -- to objects `construct<>()`ed by this arena
   * class.  While as of this writing no separately documented concept for SHM arena class (implemented, e.g., by
   * this Ipc_arena; or its SHM-classic counterpart shm::classic::Pool_arena) exists, certain important
   * APIs do take `Shm_arena`s like us and formally require it to contain `Handle`, which must be *a* `shared_ptr`
   * type (`std::`, `boost::`, or in theory some other impl), and be the type returned by `Shm_arena::construct<>()`
   * as well as `Shm_session::borrow_object()`.  A key such API, if working with capnp in-SHM,
   * is transport::struc::shm::Builder.
   *
   * ### Rationale ###
   * In addition to the generic-programming aspect touched-upon above, a
   * squishier alleged benefit is the stylistic attention it calls to construct() or session `borrow_object()`
   * returning a cross-process SHM handle to an outer/first-class object, as opposed to a normal heap-deleting
   * ref-counting pointer.
   */
  template<typename Pointed_type>
  using Handle = Obj_handle<Pointed_type>;

  /**
   * Convenience alias for a shm::stl::Stateless_allocator> w/r/t Ipc_arena;
   * use with #Activator.
   *
   * @tparam T
   *         Pointed-to type for the allocator.  See standard C++ `Allocator` concept.
   */
  template<typename T>
  using Allocator = stl::Stateless_allocator<T, Ipc_arena>;

  /// Alias for an Arena_activator using Ipc_arena.
  using Activator = stl::Arena_activator<Ipc_arena>;

  /// Alias for a stats/info bundle type.
  using Info_dump = stat::Arena_info_dump;

  /// Alias for a stats type.
  using Sharded_stats = arena_lend::stat::Sharded_stats;
  /// Alias for a stats type.
  using Pool_stats = arena_lend::stat::Pool_stats;
  /// Alias for a stats type.
  using Owner_pool_lookup_global_stats = arena_lend::stat::Owner_pool_lookup_global_stats;
  /// Alias for a stats type.
  using Obj_db_aux_pool_global_stats = arena_lend::stat::Obj_db_aux_pool_global_stats;
  /// Alias for a stats type.
  using Memory_manager_stats = arena_lend::stat::Memory_manager_stats;
  /// Alias for an info type.
  using Shm_pool_info = arena_lend::stat::Shm_pool_info;

  // Methods.

  /**
   * Creates an instance of this class, factory-style.  See class doc header for notes regarding lifetimes
   * of related objects (`Shm_session`, `construct()`ed objects) and how the `shared_ptr`-ownership semantics
   * help this happen.
   *
   * @warning `*logger` -- unless `logger` is null -- must exist at least past (1) all
   *          `construct()`-returned pointers' groups, (2) all `Shm_session`s through which you'd
   *          Shm_session::lend_arena() the returned `Ipc_arena`, and (3) of course the returned pointer group.
   *
   * @param logger
   *        Used for logging purposes.
   * @param memory_manager
   *        The jemalloc memory allocator.
   * @param pool_name_base
   *        Pool-name prefix; each pool's SHM object name is derived from this plus its unique ID.
   *        See "Cleanup" in class doc header for notes on how this can implement on-crash cleanup.
   * @param permissions
   *        The shared memory object file permissions when one is created (on-demand during construct() et al).
   *
   * @return A shared pointer to an instance of this class; not null.  (If jemalloc fails to
   *         create a jemalloc-arena -- a truly exceptional situation -- an exception shall propagate from
   *         inside; there is no need to catch it, any more than one just-in-case catches `bad_alloc`.)
   */
  static std::shared_ptr<Ipc_arena> create(flow::log::Logger* logger,
                                           const std::shared_ptr<Memory_manager>& memory_manager,
                                           Shared_name&& pool_name_base,
                                           const util::Permissions& permissions);

  /**
   * `allocate()`s `sizeof(T)` bytes (in SHM), invokes `T{...}` ctor with the given args, and returns a
   * ref-counted handle, so that the resulting object shall be garbage-collected w/r/t this process as well
   * as any subsequent *borrower* process(es).  That is to say, `~T()` dtor shall be called, followed by deallocate(),
   * in this process -- but only after (1) the returned handle's `shared_ptr` group reaches ref-count zero,
   * *and* (2) the same happens to any similar `Handle<T>` returned by Shm_session::borrow_object().
   *
   * @see Class doc header for an overview of related ops including lend/borrow.  That'll explain how the
   *      aforementioned `borrow_object()` call -- probably in another process! -- is connected to the
   *      handle returned here.
   *      (Spoiler alert: `p = a->construct()` => `blob = session->lend_object(p)`
   *      => `p2 = their_session->borrow_object(copy_of_blob_ipced_over_to_us)`.)
   *
   * ### [De]allocation propagation via STL-allocator ###
   * What if `T` is not a plain old data type?  Let's say specifically (as recommended for Flow-IPC users dealing
   * with non-PoDs in SHM) that `T` is (or contains... but let's keep it simple) an STL-compliant container such as
   * `vector<char>` or `flow::util::Basic_blob`.  `sizeof(T)` then likely won't be holding the actual data;
   * it'll need to allocate a buffer of size N.  Let's say you used the T ctor that would immediately allocate
   * N bytes.  Then, for this to work properly (as opposed to allocating N bytes in regular heap: unhelpful),
   * `T` must be configured with a SHM-aware allocator that will call `allocate(N)` via this `Ipc_arena` when
   * needed.  shm::stl::Stateless_allocator provides this.  So construct() itself will `allocate(sizeof(T))`,
   * invoke `T{...}`; that will propagate any allocating to the allocator; that will in turn `allocate(N)`.
   * Conversely, when SHM-jemalloc decides to garbage-collect (see above), it will invoke `~T()`; that will
   * propagate deallocation of the N-buffer to the allocator; allocator in turn will `deallocate(...)` via
   * `*this`; and lastly SHM-jemalloc shall `deallocate(p)`, where `p` is the raw pointer in the
   * handle originally returned by `construct()` here.
   *
   * It is also possible, when working with a container such as the hypothetical `T`, to require more allocations
   * and/or deallocations after ctor `T{...}` here and dtor `~T()` at GC-time.  The same principles apply:
   * `T` (e.g. `vector`) code tells allocator to [de]allocate; allocator forwards to `this->[de]allocate()`.
   * However!  The allocator must know which `Ipc_arena` to in fact use.  If you are using `Stateless_allocator`
   * (as we recommend) then you will need to use shm::stl::Arena_activator to thread-locally set the "current"
   * arena to `this`, in-scope of any potentially-[de]allocating ops (e.g.: `vector::resize()`).
   *
   * @note For ctor `T{...}` call (in construct()) and GC-time dtor `T()` call we automatically activate `*this`
   *       for purposes of `T` allocator ops.
   *
   * ### Lifetime versus `*this` arena ###
   * Please read "Interrelated lifetimes" in class doc header.
   *
   * @tparam T
   *         The object type to be created.
   * @tparam Ctor_args
   *         The parameter types that are passed to the constructor of T.
   * @param ctor_args
   *        The arguments passed to the constructor of T.
   *
   * @return A shared pointer to an object created in shared memory.
   */
  template<typename T, typename... Ctor_args>
  Handle<T> construct(Ctor_args&&... ctor_args);

  /**
   * Performs a non-garbage-collected allocation of a buffer in SHM.
   *
   * @warning It is irregular, though not formally disallowed, for the end user to invoke this except inside
   *          STL-allocator code.  We provide shm::stl::Stateless_allocator which shall do so for you, and we
   *          recommend you use that instead of rolling your own.
   *
   * ### Perf notes ###
   * A jemalloc thread cache will be used with this allocation automatically.  See deallocate() also.
   * (As of this writing preprocessor symbol `IPC_SHM_ARENA_LEND_JEMALLOC_NO_TCACHE` can be defined
   * to forego tcaching entirely.)
   *
   * @param size
   *        The allocation size, which must be greater than zero.
   * @return Upon success, a non-null pointer to the base address of the allocation; otherwise, nullptr.
   */
  void* allocate(std::size_t size) override;

  /**
   * Undoes allocate(): deallocates a previously allocated, by this arena, buffer in SHM.
   *
   * ### Perf notes ###
   * A jemalloc thread cache will be used for extra perf, assuming the calling thread has one for `*this`
   * arena -- which it does if it has ever allocate()d here (a thread's first allocate() creates its cache).
   * Notably it does *not* matter which thread allocate()d `address` itself: any buffer of this arena may be
   * deallocated via any thread's cache (see Thread_cache doc header for the mechanism and why this is so).
   * In the unusual case of a thread deallocating here without *ever* having allocated here, deallocation
   * still proceeds and succeeds, albeit at a perf penalty (an arena-level mutex lock inside jemalloc) by forgoing
   * caching.  In short: currently deallocate(), for safety reasons around certain teardown phases, does not create
   * a tcache, if it does not yet exist due to a prior allocate() in the same thread.
   *
   * @param address
   *        The address to be deallocated, which must be non-null.
   */
  void deallocate(void* address) override;

  /**
   * Synchronously garbage-collects, in the calling thread, any objects `x = A.construct()`ed by this thread, for *all*
   * `Ipc_arena`s `A` in existence.  To be explicit: the garbage-collection involves only objects `x` such that (1)
   * there are no *user references* to `x` anymore (see below), and (2) SHM-jemalloc has not yet had a chance to
   * free `*x`.  Freeing an (in-SHM) object involves calling its destructor if any (which may itself free more
   * in-SHM buffers, typically through allocator #Allocator action) and then freeing its `sizeof(*x)` in-SHM buffer.
   *
   * What are *user references*?  Answer: Firstly: `x` itself (from the given `A.construct()`) is a `shared_ptr` in a
   * group; so all `shared_ptr`s in that group are references.  Secondly: there is the Shm_session::lend_object() +
   * Shm_session::borrow_object() (the latter of which returns a `shared_ptr` similar to `x`) mechanism.  Without
   * delving into exact formal details, essentially: `x2 = S2.borrow_object()`, usually invoked in another process
   * on `Shm_session S2` in a procedure starting with `S.lend_object(x)` (in our process), returns a `shared_ptr`;
   * so the `shared_ptr`s in that `shared_ptr` group are also references (albeit typically cross-process ones).
   * (If the session to that other process is gone -- no more `S` -- then effectively any borrowed references
   * lent through it are dropped.)  To reiterate: Once both types of references are all dropped, condition (1)
   * above is satisfied.
   *
   * ### When to call this ###
   * Ideally you should not need to call this, and informally we recommend against doing so willy-nilly in the
   * middle of your SHM-using algorithm(s); not because it is a costly call (it is not), but because it cuts
   * against the (we humbly hope) easy-to-use design of SHM-jemalloc.  In short: Most SHM-jemalloc activities --
   * including *any* call to construct(), create(), Shm_session::lend_object(), Shm_session::borrow_object(),
   * `construct()`ed-object disposer, ditto borrowed, at thread exit -- will already perform the necessary
   * garbage-collection.  Moreover, in many cases (depending on your use patterns), the specific object that has
   * just reached cross-process ref-count-zero will be *immediately* disposed (no later GC needed).
   *
   * However: If a given object `*x` happens to reach cross-process ref-count-zero (meaning the last cross-process
   * `shared_ptr` group's last `shared_ptr` releases its reference) *not* in the thread that constructed it but
   * another thread or process, *and* (this is key) that thread stops doing **~all** SHM-jemalloc work as triggered
   * by your code (see above list which may not be exhaustive), then no opportunistic GC shall occur, and `*x`
   * will not be freed yet.  (Eventually it will; worst-case on thread exit.)
   *
   * It is arguably a pathological use pattern, but it's certainly possible.  So if your thread enters an idle
   * or SHM-jemalloc-free time period, while arena(s) exist that could have this-thread-`construct()`ed objects
   * (that are no longer used) extant, then calling this will ensure such things, if any, are freed.
   *
   * ### Is it cheap? ###
   * Above we casually say "it is not" expensive.  Informally speaking this is generally true.  It *is* possible
   * to contrive a scenario where so many no-longer-referenced but not-already-GCed (through direct or
   * piggy-backed internal SHM-jemalloc algorithms alluded-to above) objects have built up, that a given call
   * is relatively heavy, just by virtue of having to do the actual freeing many times.  Even then, though, it
   * would be a one-time thing.  The point is: if there's little or nothing to GC, then it's cheap, because the
   * check is very cheap.
   *
   * ### Thread safety ###
   * Safe to call anytime.  Asterisk: We have not contemplated corner-case scenarios of calling this from a given
   * thread, when that thread is exiting (such as in some `thread_local`'s destructor).  So we formally say:
   * Do not do it (on pain of undefined behavior).  In any case it would be pointless: GC occurs during thread
   * shutdown already.
   */
  static void this_thread_gc();

  /**
   * Fills-out the stats/info contents of the given stat::Arena_info_dump: a printable bundling of stats/info relevant
   * to (but not all necessarily owned by) `*this` Ipc_arena at this point in time.  To summarize the resulting
   * `*target_info_dump`:
   *   - Stats/info stored by-value; can be queried in peace.  They won't concurrently change nor become invalid
   *     when `*this` Ipc_arena shuts down.
   *   - Can be printed to an `ostream` via `ostream<<`.  `->m_fmt` (`Info_dump_format`) has output-format knobs,
   *     adjusting which ahead of the `<<` will affect the output.
   *     - As usual: can `FLOW_LOG_...(*target_info_dump)`, `boost::lexical_cast<string>(*target_info_dump)`,
   *       `flow::util::ostream_op_string(*target_info_dump)`.
   *
   * ### Rationale ###
   * This is a "get me everything this-arena-related, all in one nicely-printable thing" operation.
   *
   * Cf.: For finer-grained access to all of the same information, you can access things individually
   * (the doc headers of each `Arena_info_dump` member states just which Ipc_arena API is the source):
   * sharded_stats(), memory_manager_stats(), etc.  They can be `<< print(...)`ed individually, too.
   * Plus on these one can perform `flow::util::stat` post-processing: `stats_sum()`, things like that.
   *
   * The (fully optional to use) reset ops for these are available on Ipc_arena as well: sharded_stats_reset(),
   * memory_manager_stats_reset(), etc.
   *
   * @param target_info_dump
   *        The non-`->m_fmt` parts shall be assigned.
   * @param call_timing
   *        See util::Call_timing doc header(s).  Reminder: By convention: If your code can guarantee that
   *        reasons for making this unsafe cannot exist in the chosen build/otherwise environment, then
   *        leave this at its default.  In this case: see jemalloc::Memory_manager doc header; it explains
   *        the specific potential danger.  (Spoiler alert: With jemalloc>=5.3.0 linked, there is no problem.)
   */
  void info_dump(Info_dump* target_info_dump,
                 util::Call_timing call_timing = util::Call_timing::S_ALWAYS_SAFE);

  /**
   * Internally samples certain stats so as to update `_hi_wmark`s that are computed at stat-consume time as
   * opposed to continuously maintained; as of this writing the ones for memory_manager_stats() and sharded_stats().
   * (All other `_hi_wmark`s are continuously maintained and don't require any such intervention.)
   *
   * Calling memory_manager_stats() and sharded_stats() does this, too; the present method is the idiomatic way
   * to do it when one does not actually need to consume the current stats.
   *
   * @note By default, `Ipc_arena` that is set-up by ipc::session (which is recommended but not required) will
   *       have this done regularly automatically; in which case there is no need for the user to call this.
   *
   * ### Thread safety ###
   * Always safe.
   *
   * @param call_timing
   *        See info_dump(): same thing here.
   */
  void sample_hi_wmarks(util::Call_timing call_timing = util::Call_timing::S_ALWAYS_SAFE);

  /**
   * Outputs the stat-set stat::Sharded_stats for `*this` arena.  See that type's doc header for the
   * meaning of its sub-`struct`s and stat-members therein.
   *
   * `*target_stats` (unless null; see below) is assigned a copy of the currently-accurate set of values.
   * It is a snapshot copy as of this call and will not change on return.  (The `atomic`ity of members is
   * required by the impl which uses the TL-sharding technique.)  Consume via `flow::util::stat::load()` for
   * best speed; use `stats_assign()` / `print()` (et al); see `flow::util::stat` doc header.
   *
   * ### Semantics ###
   * Generally, as usual, see doc header for the stat-set type (stat::Sharded_stats here).  Also:
   *
   * The `*_hi_wmark` (`Stat_type::S_HI_WMARK`) stat-members in stat::Sharded_stats have a limited
   * resolution by the nature of how they are collected (TL-sharding).  Supposing `m_x` is a GAUGE,
   * `m_x_hi_wmark` shall simply be the max value of `m_x` chosen between these points:
   *   - Last time sharded_stats_reset() was called; or `*this` construction if never.
   *   - Each sharded_stats() call since then, including the current one.
   *     - To only update the `_hi_wmark`s, without needing to obtain the resulting `*target_stats`,
   *       use `target_stats == nullptr`.  However: the more idiomatic way of doing this is
   *       sample_hi_wmarks().
   *
   * ### Thread safety ###
   * Safe to call concurrently with any other method on the same `*this`.  Also applies to our `..._reset()`.
   *
   * @param target_stats
   *        The target; see above.  If null, the internally stored `_hi_wmark`s are updated only.
   */
  void sharded_stats(Sharded_stats* target_stats);

  /// Resets sharded_stats().  The formal meaning of a reset is discussed in `flow::util::stat` doc header.
  void sharded_stats_reset();

  /**
   * Returns the stat-set stat::Pool_stats for `*this` arena -- the arena's non-sharded SHM-pool stats (obj-DB
   * auxiliary-pool RAM + the arena's own SHM-pool create/destroy).  See that type's doc header (and its members')
   * for the meaning of the stat-members.
   *
   * The returned reference is to live `atomic<>`s which can change at any moment concurrently; hence even values
   * grabbed in immediate succession can be slightly mutually incoherent.  Consume via `flow::util::stat::load()` /
   * `stats_assign()` / `print()` (et al); see `flow::util::stat` doc header.
   *
   * ### Thread safety ###
   * Safe to call concurrently with any other method on the same `*this`.  Also applies to our `..._reset()`.
   *
   * @return See above.
   */
  const Pool_stats& pool_stats() const;

  /// Resets pool_stats().  The formal meaning of a reset is discussed in `flow::util::stat` doc header.
  void pool_stats_reset();

  /**
   * Returns the memory-manager-level statistics -- one stat::Memory_manager_stats
   * per native (jemalloc) arena maintained by `*this`, sorted ascending by
   * native arena-ID -- describing the SHM-provider's supplier-side memory behavior (as of this writing the
   * page-state / virtual-address-space footprint sub-group).
   *
   * @note As of this writing `*this` maintains exactly one such arena, so the returned vector has exactly
   *       one element.
   *
   * Each row's identifying fields (stat::Memory_manager_stats::m_uniq_arena_id and `m_native_arena_id`) are
   * always set.  The jemalloc-sourced gauge fields are populated only if the linked jemalloc was built with
   * statistics support (its `config.stats`); if not, those are left at default (usually zero) rather than producing
   * partial output.  (Some -- not all -- values could potentially still be obtained, but we do not want to
   * set some and not-set others; thus: no "partial output.")
   *
   * Informational purposes only (logging/monitoring/etc.).  Values are a fresh, mutually-consistent snapshot
   * as of this call (to the extent promised by jemalloc), but the reality can change at any moment concurrently
   * with allocation/deallocation activity.
   *
   * ### Semantics ###
   * Generally, as usual, see doc header for the stat-set type (stat::Memory_manager_stats here).  Also:
   *
   * The `*_hi_wmark` (`Stat_type::S_HI_WMARK`) stat-members in stat::Memory_manager_stats have a limited
   * resolution by the nature of the data-source (jemalloc's stats system).  Supposing `m_x` is a GAUGE,
   * `m_x_hi_wmark` shall simply be the max value of `m_x` chosen between these points:
   *   - Last time memory_manager_stats_reset() was called; or `*this` construction if never.
   *   - Each memory_manager_stats() call since then, including the current one.
   *
   * Values of `m_x` between those points are not known to us, nor maintained by jemalloc; therefore we can
   * do no better.
   *
   * @note A way of maintaining fairly-useful `_hi_wmark` values here is to consume stats (call
   *       memory_manager_stats()) periodically.  Careful: this would be, e.g., second(s) apart -- you don't
   *       want to affect perf of the sytem by making it frequent.
   * @note ipc::session::shm as of this writing does this automatically to any `Ipc_arena`s it sets up for you.
   *       We recommend for this reason, and many other reasons, to set-up SHM-jemalloc via `ipc::session`.
   *
   * ### Thread safety ###
   * Safe to call concurrently with any other method on the same `*this`.  Also applies to our `..._reset()`.
   *
   * @return See above.
   */
  std::vector<Memory_manager_stats> memory_manager_stats();

  /**
   * Resets memory_manager_stats().  The formal meaning of a reset is discussed in `flow::util::stat` doc header.
   * See also memory_manager_stats() doc header, "Semantics."
   */
  void memory_manager_stats_reset();

  /**
   * Returns the process-global owner-side stat-set
   * stat::Owner_pool_lookup_global_stats, aggregated across all `Ipc_arena`s in this process.  See that type's doc
   * header for the meaning of its stat-members.
   *
   * The returned reference is to live `atomic<>`s which can change at any moment concurrently;
   * hence even values grabbed in immediate succession can be slightly mutually incoherent.  Consume via
   * `flow::util::stat::load()` / `stats_assign()` / `print()` (et al); see `flow::util::stat` doc header.
   *
   * ### Thread safety ###
   * Safe to call concurrently with anything on any `Ipc_arena` (or none).
   *
   * @return See above.
   */
  static const Owner_pool_lookup_global_stats& owner_pool_lookup_global_stats();

  /**
   * Like owner_pool_lookup_global_stats() but returns the process-global stat-set stat::Obj_db_aux_pool_global_stats.
   * See that type's doc header for the meaning of its stat-members; all notes in owner_pool_lookup_global_stats()
   * (live `atomic<>`s; consumption; thread safety; reset via global_stats_reset()) apply
   * equally here.
   *
   * @return See above.
   */
  static const Obj_db_aux_pool_global_stats& obj_db_aux_pool_global_stats();

  /**
   * Resets owner_pool_lookup_global_stats() and obj_db_aux_pool_global_stats().  The formal meaning of a reset is
   * discussed in `flow::util::stat` doc header.
   *
   * @note These are process-global stat-sets; hence this resets them across the entire process.
   *       (Cf. the non-`static` `*_stats_reset()`s.)
   *       To be clear, though, this does not touch anything but the data for the aforementioned
   *       two `static ..._globals_stats()`.  Anything accessed per-arena (via non-`static` `*_stats()`) is
   *       reset only via correspoding non-`static` `*_stats_reset()`, for each Ipc_arena of interest.
   */
  static void global_stats_reset();

  /**
   * For informational/stats-adjacent purposes, returns information on currently live SHM-pools within `*this`
   * arena, sorted by pool ID -- that is, in chronological order of creation.  By "live" we mean that these
   * pools currently have their names in the file-system and are memory-mapped; if a given pool is removed from
   * the areba, then it would not be included here.  (It does *not* necessarily mean
   * that each pool's entire size Shm_pool_info::m_sz is memory-resident.)
   *
   * Informational purposes only (logging/monitoring/etc.).  The live pool-set can change at any moment
   * concurrently, at least if allocation/deallocation activity through `*this` is possible concurrently.
   *
   * Same thread-safety notes as memory_manager_stats().
   *
   * @return See above.
   */
  std::vector<Shm_pool_info> shm_pool_live_info() const;

  /**
   * Returns the jemalloc-ID for the (only, as of this writing) jemalloc-arena maintained by `*this`;
   * this identifies that arena to the jemalloc API (e.g. `mallctl()` `"stats.arenas.(id).*"` queries).
   *
   * @note The intended use-case for this is for informational purposes, such as if one desires to query
   *       something in the jemalloc-stats API that we perhaps don't already provide in nice form
   *       through the nearby stat-consumption API.  Use with care.
   *
   * @return See above.
   */
  arena_id_t get_jemalloc_arena_id() const;

  /**
   * (Internal-use) Adds a listener to get updates on shared memory pools.  The pointer must remain valid until
   * after it is removed.  The ipc::shm::arena_lend::Owner_shm_pool_listener::notify_initial_shm_pools() method
   * will be called synchronously within this call.
   *
   * @param listener
   *        The listener to add.
   * @return Whether the listener was registered successfully, which means it wasn't registered previously.
   */
  bool add_shm_pool_listener(Owner_shm_pool_listener* listener);

  /**
   * (Internal-use) Removes a listener from getting further updates on shared memory pools.
   *
   * @param listener
   *        The listener to remove.
   * @return Whether the listener was deregistered successfully, which means it was registered previously.
   */
  bool remove_shm_pool_listener(Owner_shm_pool_listener* listener);

  /* Would be private, but as of this writing some items are used by white-boxy unit-tests.
   * There's a to-do in class doc header to perhaps change this to a facade/attorney/etc. pattern instead. */
#ifdef IPC_DOXYGEN_ONLY // Compiler ignores; Doxygen sees: document the below as the private it conceptually is.
private:
#else // Compiler sees; Doxygen ignores.
protected:
#endif
  // Constructors.

  /**
   * Constructor.  See create().  It does not call start() however.
   *
   * @param logger
   *        See above.
   * @param memory_manager
   *        See above.
   * @param pool_name_base
   *        See above.
   * @param permissions
   *        See above.
   */
  Ipc_arena(flow::log::Logger* logger,
            const std::shared_ptr<Memory_manager>& memory_manager,
            Shared_name&& pool_name_base,
            const util::Permissions& permissions);

  // Methods.

  /**
   * Important helper synchronously invoked, in lieu of `delete arena`, when the `create()`-returned `shared_ptr`
   * group reaches ref-count zero.
   *
   * Impl details for convenience/context: The goal is to indeed `delete this`, and the fact we are being called
   * implies that all `construct()`ed handles are gone, as are all lent-through `Shm_session`s -- so in that sense
   * it is okay to destroy the Ipc_arena; but certain non-user-facing items prevent it from being actually
   * possibly yet; so we must eliminate those first and then `delete this`.  Some of these items are potentially
   * asynchronous, namely when something has to be (opportunistically) done in a different thread.
   * In chronological order:
   *   -# Thread_lcl_obj_db_admin<Ipc_arena>::forgetting_shm_arena():
   *      Live-`construct()`ed-object-tracking module frees all resources (including aux non-user-data-storing
   *      SHM-pools detail::Lend_tracker_pool).
   *   -# Owner_shm_pool_repository::erase() of every SHM-pool still extant on our behalf.
   *   -# jemalloc::Memory_manager::destroy_arena() of get_jemalloc_arena_id(): Potentially-asynchronous:
   *      -# Destroy every (per-thread) jemalloc-tcache for that jemalloc-arena (creation of each previously
   *         triggered on-demand via 1st `this->allocate()` in that thread; recall that allocate() is always
   *         tcache-enabled).
   *      -# Destroy jemalloc-arena.  (Now we can: jemalloc-tcaches all had to be destroyed per jemalloc rules.)
   *         This should unmap vaddr-area + close SHM-pool-handle for every pool still extant on our behalf.
   *   -# `delete this`: (Now we can: jemalloc-arena that had hooks into `*this` = gone.)
   *
   * @see destroy_on_obj_db_forgot_us() (helper that does the last N of those steps).
   */
  void destroy();

  /**
   * Creates the jemalloc arena; create() calls it, once, immediately upon construction; and nothing else
   * must be called before it (allocate() in particular).  It is a separate thing (rather than being subsumed
   * in ctor) due to test harness technicalities; e.g., a test may want to register an event listener
   * before the first SHM-pool (jemalloc *base* block; see start_impl()) is created within this call.
   *
   * If jemalloc fails to create the jemalloc-arena -- a truly exceptional situation -- an exception
   * shall propagate from inside.
   *
   * @warning Not thread-safe.
   */
  void start();

  /**
   * Implements super-class API.
   *
   * Among potentially other things: records in #m_shm_pools and in Owner_shm_pool_repository (the latter
   * making it possible to do pool ID+offset <=> vaddr lookups for at least owner-side Shm_pool_offset_ptr to work)
   * and notifies registered listeners.
   *
   * @param shm_pool
   *        See above.
   */
  void on_shm_pool_created(const std::shared_ptr<Shm_pool>& shm_pool) override;

  /**
   * Implements super-class API: forwards to handle_created_shm_pool().
   *
   * Among potentially other things: see note on on_shm_pool_created(): undoes that.
   *
   * @param shm_pool
   *        See above.
   * @param removed_shared_memory
   *        See above.
   */
  void on_shm_pool_removed(const std::shared_ptr<Shm_pool>& shm_pool, bool removed_shared_memory) override;

  /**
   * Returns the memory manager.
   * @return See above.
   */
  std::shared_ptr<Memory_manager> get_jemalloc_memory_manager() const;

  /**
   * Creates a shared memory pool (jemalloc extent hook).
   *
   * @todo It would be nice to fill out all the doc headers for the extent hook impls in Ipc_arena with specific
   * info beyond "see jemalloc docs"; as for example was done with Ipc_arena::create_shm_pool().
   *
   * @param address
   *        The desired location to map this memory pool, which can be null for system specification.
   * @param size
   *        The size of the memory pool to be created.
   * @param alignment
   *        The value to align the resulting address on, which is generally a multiple of page size.
   * @param zero
   *        Output parameter indicating whether the contents have been zeroed.
   * @param commit
   *        Whether the system should designate the pages to be readable and writable (marked active and
   *        can be put into physical memory).  If they system is set to overcommit memory, commit is always
   *        enabled.  The value will be updated as an output parameter to indicate whether the memory was
   *        committed.
   * @param arena_id
   *        The memory area that the pool will be placed into.
   *
   * @return Upon success, the created memory pool; otherwise, nullptr.
   */
  void* create_shm_pool(void* address, std::size_t size, std::size_t alignment, bool* zero,
                        bool* commit, arena_id_t arena_id);

  /**
   * jemalloc extent hook impl.  See jemalloc docs for in/out semantics.
   *
   * @param address
   *        See above.
   * @param size
   *        See above.
   * @param committed
   *        See above.
   * @param arena_id
   *        See above.
   * @return See above.
   */
  bool optional_remove_shm_pool(void* address, std::size_t size, bool committed, arena_id_t arena_id);

  /**
   * jemalloc extent hook impl.  See jemalloc docs for in/out semantics.
   *
   * @param address
   *        See above.
   * @param size
   *        See above.
   * @param committed
   *        See above.
   * @param arena_id
   *        See above.
   * @return See above.
   */
  bool remove_shm_pool(void* address, std::size_t size, bool committed, arena_id_t arena_id);

  /**
   * jemalloc extent hook impl.  See jemalloc docs for in/out semantics.
   *
   * @param address
   *        See above.
   * @param size
   *        See above.
   * @param offset
   *        See above.
   * @param length
   *        See above.
   * @param arena_id
   *        See above.
   * @return See above.
   */
  bool commit_memory_pages(void* address, std::size_t size, std::size_t offset, std::size_t length,
                           arena_id_t arena_id);

  /**
   * jemalloc extent hook impl.  See jemalloc docs for in/out semantics.
   *
   * @param address
   *        See above.
   * @param size
   *        See above.
   * @param offset
   *        See above.
   * @param length
   *        See above.
   * @param arena_id
   *        See above.
   * @return See above.
   */
  bool decommit_memory_pages(void* address, std::size_t size, std::size_t offset,
                             std::size_t length, arena_id_t arena_id);

  /**
   * jemalloc extent hook impl.  See jemalloc docs for in/out semantics.
   *
   * @param address
   *        See above.
   * @param size
   *        See above.
   * @param offset
   *        See above.
   * @param length
   *        See above.
   * @param arena_id
   *        See above.
   * @return See above.
   */
  bool purge_forced_memory_pages(void* address, std::size_t size, std::size_t offset, std::size_t length,
                                 arena_id_t arena_id);

  /**
   * jemalloc extent hook impl.  See jemalloc docs for in/out semantics.
   *
   * @param address
   *        See above.
   * @param size
   *        See above.
   * @param size_a
   *        See above.
   * @param size_b
   *        See above.
   * @param committed
   *        See above.
   * @param arena_id
   *        See above.
   * @return See above.
   */
  bool split_memory_pages(const void* address, size_t size, size_t size_a,
                          size_t size_b, bool committed, arena_id_t arena_id);

  /**
   * jemalloc extent hook impl.  See jemalloc docs for in/out semantics.
   *
   * @param address_a
   *        See above.
   * @param size_a
   *        See above.
   * @param address_b
   *        See above.
   * @param size_b
   *        See above.
   * @param committed
   *        See above.
   * @param arena_id
   *        See above.
   * @return See above.
   */
  bool merge_memory_pages(const void* address_a, std::size_t size_a,
                          const void* address_b, std::size_t size_b,
                          bool committed, arena_id_t arena_id);


private:
  // Friends.

  /// Friend facade providing privileged access for internal Flow-IPC components.
  template<typename Base_t>
  friend struct arena_lend::detail::Owner_spc_impl;

  // Types.

  /// Short-hand for pool ID type.
  using pool_id_t = Shm_pool::pool_id_t;
  /// Short-hand for the jemalloc extent-hooks structure with a pointer back to a `this`.
  using Extent_hooks = Memory_manager::Extent_hooks_wrapper<Ipc_arena>;
  /// Short-hand for a stats type.
  using Uniq_arena_id = arena_lend::stat::Uniq_arena_id;
  /// Single-reader, single-writer mutex.
  using Mutex = flow::util::Mutex_non_recursive;
  /// Exclusive lock for the mutex.
  using Lock = flow::util::Lock_guard<Mutex>;

  // Methods.

  /**
   * Ensures the calling thread has a jemalloc tcache for `*this` arena, creating it if needed -- making
   * subsequent deallocate() calls from this thread lock-free (assuming jemalloc knobs have not disabled tcaching
   * or something).  No-op if tcache use is compiled out (`IPC_SHM_ARENA_LEND_JEMALLOC_NO_TCACHE`).
   *
   * allocate() will already ensure tcache exists; as of this writing deallocate() will not.
   * Hence this is useful if we know or suspect that there shall be potentially many `deallocate()`s
   * but not a single (same-arena) `allocate()`.  However:
   *
   * @warning Call it only at known-safe moments (calling thread not exiting -- Thread_cache deinit has not
   *          started in particular; `*this` alive and not being destroyed).  In particular deallocate() itself might
   *          be happening around that time, so it does not as of this writing do it.
   */
  void this_thread_ensure_tcache_exists() const;

  /**
   * destroy() helper; handles the steps that remain once the possibly-async `forgetting_shm_arena()` finishes.
   *
   * @param log_ctx
   *        Concurrently-mutable `Log_context_mt` to use for all remaining synchronous logging for `*this` object.
   *        Do not use `this->get_logger()` (including via regular `FLOW_LOG_INFO()`/etc. calls)!
   *        Use `log_ctx`.
   */
  void destroy_on_obj_db_forgot_us(const flow::log::Log_context_mt* log_ctx);

  /**
   * start() helper.  We lack real support for multiple jemalloc-arenas; but there is a little bit of a start;
   * e.g. destroy() properly handles shutting down all the jemalloc-arenas.
   *
   * There was no reason to blow that stuff away, and we may extend it to be actually useful/used.
   * At the moment `n_arenas == 1` always.  #m_arena0 holds its ID, so for now we can quickly access it without
   * messing with essentially-unneeded #m_arenas container.
   *
   * @param n_arenas
   *        # of jemalloc-arenas to create/maintain.
   */
  void start_impl(unsigned int n_arenas);

  /**
   * Extent hook impl helper for certain extent hooks: given a jemalloc extent at `address` of `size`
   * bytes, and a sub-range within it at `offset` bytes extending for `length` bytes, locates the SHM pool
   * in #m_shm_pools containing the extent and computes the offset of the sub-range relative to the pool's base address.
   *
   * Validates that (1) a pool is found at `address`, (2) `[address, address + size)` is entirely within
   * that pool, and (3) `offset + length <= size`.  On failure logs a warning and returns `false`.
   *
   * @param address
   *        Base address of the jemalloc extent.
   * @param size
   *        Size of the jemalloc extent in bytes.
   * @param offset
   *        Byte offset within the extent where the sub-range of interest begins.
   * @param length
   *        Length of the sub-range of interest in bytes; must be positive.
   * @param use_case
   *        Human-readable label for log messages (e.g., "committing", "decommitting").
   *        Global constant/literals are best for perf.
   * @param pool
   *        On success, set to the SHM pool containing the extent.
   * @param pool_offset
   *        On success, set to the offset of the sub-range start relative to `pool`'s base address
   *        (i.e., offset-within-pool-of(`address`) + `offset`).
   * @return `true` on success; `false` if validation fails.
   */
  bool compute_pool_and_offset(void* address, std::size_t size, std::size_t offset,
                               std::size_t length, util::String_view use_case,
                               std::shared_ptr<Shm_pool>& pool, std::size_t& pool_offset) const;

  /**
   * jemalloc extent hook impl: forwards to similarly named member function of the proper `this` (which
   * is stored near `*extent_hooks`).  See that method for docs.
   *
   * @param extent_hooks
   *        See above.
   * @param address
   *        See above.
   * @param size
   *        See above.
   * @param alignment
   *        See above.
   * @param zero
   *        See above.
   * @param commit
   *        See above.
   * @param arena_id
   *        See above.
   * @return See above.
   */
  static void* create_shm_pool_handler(extent_hooks_t* extent_hooks, void* address,
                                       std::size_t size, std::size_t alignment,
                                       bool* zero, bool* commit, unsigned arena_id);

  /**
   * jemalloc extent hook impl: forwards to similarly named member function of the proper `this` (which
   * is stored near `*extent_hooks`).  See that method for docs.
   *
   * @param extent_hooks
   *        See above.
   * @param address
   *        See above.
   * @param size
   *        See above.
   * @param committed
   *        See above.
   * @param arena_id
   *        See above.
   * @return See above.
   */
  static bool optional_remove_shm_pool_handler(extent_hooks_t* extent_hooks, void* address,
                                               std::size_t size, bool committed, unsigned arena_id);

  /**
   * jemalloc extent hook impl: forwards to similarly named member function of the proper `this` (which
   * is stored near `*extent_hooks`).  See that method for docs.
   *
   * @param extent_hooks
   *        See above.
   * @param address
   *        See above.
   * @param size
   *        See above.
   * @param committed
   *        See above.
   * @param arena_id
   *        See above.
   */
  static void remove_shm_pool_handler(extent_hooks_t* extent_hooks, void* address,
                                      std::size_t size, bool committed, unsigned arena_id);

  /**
   * jemalloc extent hook impl: forwards to similarly named member function of the proper `this` (which
   * is stored near `*extent_hooks`).  See that method for docs.
   *
   * @param extent_hooks
   *        See above.
   * @param address
   *        See above.
   * @param size
   *        See above.
   * @param offset
   *        See above.
   * @param length
   *        See above.
   * @param arena_id
   *        See above.
   * @return See above.
   */
  static bool commit_memory_pages_handler(extent_hooks_t* extent_hooks, void* address,
                                          std::size_t size, std::size_t offset,
                                          std::size_t length, unsigned arena_id);

  /**
   * jemalloc extent hook impl: forwards to similarly named member function of the proper `this` (which
   * is stored near `*extent_hooks`).  See that method for docs.
   *
   * @param extent_hooks
   *        See above.
   * @param address
   *        See above.
   * @param size
   *        See above.
   * @param offset
   *        See above.
   * @param length
   *        See above.
   * @param arena_id
   *        See above.
   * @return See above.
   */
  static bool decommit_memory_pages_handler(extent_hooks_t* extent_hooks, void* address,
                                            std::size_t size, std::size_t offset,
                                            std::size_t length, unsigned arena_id);

  /**
   * jemalloc extent hook impl: forwards to similarly named member function of the proper `this` (which
   * is stored near `*extent_hooks`).  See that method for docs.
   *
   * @param extent_hooks
   *        See above.
   * @param address
   *        See above.
   * @param size
   *        See above.
   * @param offset
   *        See above.
   * @param length
   *        See above.
   * @param arena_id
   *        See above.
   * @return See above.
   */
  static bool purge_forced_memory_pages_handler(extent_hooks_t* extent_hooks, void* address,
                                                std::size_t size, std::size_t offset,
                                                std::size_t length, unsigned arena_id);

  /**
   * jemalloc extent hook impl: forwards to similarly named member function of the proper `this` (which
   * is stored near `*extent_hooks`).  See that method for docs.
   *
   * @param extent_hooks
   *        See above.
   * @param address
   *        See above.
   * @param size
   *        See above.
   * @param size_a
   *        See above.
   * @param size_b
   *        See above.
   * @param committed
   *        See above.
   * @param arena_id
   *        See above.
   * @return See above.
   */
  static bool split_memory_pages_handler(extent_hooks_t* extent_hooks, void* address,
                                         std::size_t size, std::size_t size_a, std::size_t size_b,
                                         bool committed, unsigned arena_id);

  /**
   * jemalloc extent hook impl: forwards to similarly named member function of the proper `this` (which
   * is stored near `*extent_hooks`).  See that method for docs.
   *
   * @param extent_hooks
   *        See above.
   * @param address_a
   *        See above.
   * @param size_a
   *        See above.
   * @param address_b
   *        See above.
   * @param size_b
   *        See above.
   * @param committed
   *        See above.
   * @param arena_id
   *        See above.
   * @return See above.
   */
  static bool merge_memory_pages_handler(extent_hooks_t* extent_hooks,
                                         void* address_a, std::size_t size_a,
                                         void* address_b, std::size_t size_b,
                                         bool committed, unsigned arena_id);

  /**
   * Returns the stat::Uniq_arena_id (owner-process PID + collection-ID) identifying `*this` arena; used by
   * the informational/stats accessors (e.g. shm_pool_live_info(), memory_manager_stats()).
   *
   * @return See above.
   */
  Uniq_arena_id uniq_arena_id() const;

  /**
   * Helper that for the given jemalloc-arena `arena_id` reads the values into stat::Memory_manager_stats::Vaddr
   * by querying jemalloc.  Pre-conditions: relevant mutex locked (if `*vaddr` is concurrently accessible),
   * jemalloc-stats enabled within jemalloc, jemalloc-stats epoch refreshed.
   *
   * @param arena_id
   *        See get_jemalloc_arena_id().
   * @param vaddr
   *        Out: filled with the current gauge values.  Must not be null.
   */
  void memory_manager_vaddr_stats_read(arena_id_t arena_id, Memory_manager_stats::Vaddr* vaddr) const;

  /**
   * Similar to memory_manager_vaddr_stats_read() but applied to stat::Memory_manager_stats::Alloc group.
   *
   * @param arena_id
   *        See above.
   * @param alloc
   *        See above.
   */
  void memory_manager_alloc_stats_read(arena_id_t arena_id, Memory_manager_stats::Alloc* alloc) const;

  // Data.

  /**
   * The monotonically increasing collection (arena ID).
   *
   * One of these globally-uniquely identifies an `Ipc_arena` *of any given one process* (itself identified in relevant
   * contexts -- generally Shm_session et al when tracking borrowed arenas -- by an `owner_id_t` which as of this
   * writing is simply that process's PID).  Together those 2 make up a detail::Uniq_collection_id.
   */
  static std::atomic<collection_id_t> m_collection_id_counter;

  /// Owner (this) process's PID, captured at construction; the first half of uniq_arena_id().
  uint64_t m_owner_id;

  /// The segregated memory areas.  Empty until start(); non-empty and unchanging thereafter.
  std::set<arena_id_t> m_arenas;

  /// The first (and for now only, in practice) arena-ID in #m_arenas.  Used for perf between start() and destroy().
  arena_id_t m_arena0;

  /// The callback functions for jemalloc.  Contains the jemalloc extent-hook table + a pointer back to `*this`.
  Extent_hooks m_extent_hooks_wrapper;

  /**
   * Start at `false`; at some point in the destroy() flow becomes `true` to prevent event listeners from firing during
   * jemalloc-arena destruction which will fire pool-removed hook for each.
   *
   * It is synchronized out of paranoia: all else being equal, a pool-removed hook can be fired from any end-user
   * thread that does (directly or not) deallocate() (maybe even allocate()), and it will check this flag.
   * We only *set* this flag in destroy(), and that only should run at the earliest once all `constructed()`ed
   * handles are gone; so there would be no [de]allocation by then.  Technically if someone called allocate()
   * directly without `deallocate()`ing before letting all `Ipc_arena` handles expire, and then did deallocate()
   * during the resulting destroy()... there could be concurrency accessing this.  Of course in that case probably
   * far worse things are about to happen than the lack of perfect cross-thread sync of this silly flag.
   * Anyway!!!  This is cheap and rarely-accessed, so just whatever.  Too many words here!
   */
  std::atomic<bool> m_destroy_started;

  /// Protects access to #m_shm_pools and #m_listeners.
  mutable Mutex m_shm_pools_and_listeners_mutex;

  /**
   * Collection of created SHM-pools.  Insertion and removal is triggered by this chain of events:
   *   - `[de]allocate()` called =>
   *   - jemalloc allocate function called (via jemalloc::Memory_manager wrapper) =>
   *   - jemalloc itself determines a new extent shall be allocated (SHM-pool must be created) or
   *     ~the opposite; and invokes that extent hook from #m_extent_hooks_wrapper =>
   *   - create_shm_pool() / remove_shm_pool() is the extent hook impl which locks
   *     #m_shm_pools_and_listeners_mutex and updates #m_shm_pools + unlocks mutex.
   */
  std::set<std::shared_ptr<Shm_pool>> m_shm_pools;

  /// The SHM pool listeners, each to be informed of relevant events (basically: pool(s) added, pool removed).
  std::set<Owner_shm_pool_listener*> m_listeners;

  /**
   * For `*this` arena, the non-sharded, non-jemalloc-sourced stats, bundled into stat::Pool_stats.
   *
   * ### Design/performance ###
   * Concurrent, non-sharded (as introduced in `flow::util::stat` doc header): each sub-`struct` is one set of
   * `atomic`s, updated by various threads and stat-consumed anytime.  The aux-pool sub-`struct`'s updates are
   * farmed out via detail::Lend_tracker_pool + detail::Use_count_registry -- a handful per `construct()`ing thread
   * (1x at the first construct(), ~31x at each newly-activated detail::Use_count_registry quantum, 1x at thread
   * exit + last-object GC), i.e. negligible.  The owner-pool sub-`struct`'s updates are right in `*this`'s
   * extent-hooks (pool create/remove; stat::Owner_pool_stats doc header has more, esp. re. removal).
   *
   * @see Cf. #m_sharded_stats, which by contrast is full-on TL-sharded.
   */
  Pool_stats m_pool_stats;

  /// Protects #m_sharded_stats, so that sharded_stats() and sharded_stats_reset() can be thread-safe as advertised.
  mutable Mutex m_sharded_stats_mutex;

  /**
   * The stats for `*this` arena that are in the stat-collection purview of detail::Thread_lcl_obj_db_admin
   * and detail::Thread_lcl_obj_db_client (collectively the *obj-DB*); as recorded at last stat-consumption (if any;
   * else default-cted).
   *
   * This pattern -- a thread-locally-distributed module (the obj-DB in our case) does TL-sharded stat-collection
   * in TL-distributed fashion, while the module's user (that is we) maintains what it returned at last
   * stat-consumption (shard-aggregation) if any -- is mandated by the obj-DB module.  To wit,
   * detail::stat::sharded_stats() and detail::stat::sharded_stats_reset() doc headers specify this.
   *
   * @see `flow::util::stat` doc header for introduction/discussion of TL-sharding.
   * @see Sharded_stats doc header for how this specifically applies to these particular stats.
   *
   * ### Thread safety ###
   * Similar to #m_mem_mgr_base_stats but simpler, as there is no map here, just a single `struct`.
   */
  Sharded_stats m_sharded_stats;

  /// Protects the *mapped-values* in #m_mem_mgr_base_stats during (possibly concurrent) stat-consumption/reset.
  mutable Mutex m_mem_mgr_base_stats_mutex;

  /**
   * Per-native-arena reset-state ("base") for memory_manager_stats() / memory_manager_stats_reset(): for each
   * arena in #m_arenas it holds the since-reset baseline (presently just the running high-water marks).
   * Populated once at start() (one zero-init entry per arena); the structure is fixed thereafter (no runtime
   * insert/erase).  Its *values* are mutated during (possibly concurrent) stat-consumption under
   * #m_mem_mgr_base_stats_mutex.
   *
   * @see memory_manager_stats(), stat::Memory_manager_stats.
   *
   * Why `map<>`?  Answer: no big reason; as of this writing its `.size() == 1` anyway, because that's the case
   * for #m_arenas.  Basically #m_arenas is a semi-placeholder (size=1), and the original author used `map<>` for
   * it -- in tests arenas were thus auto-sorted by arena-ID, and that was nice? -- so we did the same here;
   * that's all.  These details will most likely be changed-around for perf (at least), when `.size()` of
   * this and of `m_arenas` grows.
   *
   * ### Thread safety ###
   * `memory_manager_stats*()` is advertised as safe to call concurrently with any method on same `*this`, hence
   * the mutex.  Why the contract though?  In short: the various other stats (e.g., #m_pool_stats) have
   * the same property advertised via their relevant API(s).  There is no black-box reason why this should be
   * any different.  To stengthen it further:
   *
   * Technically the following is not in our purview here but nevertheless --
   * `ipc::session::shm::arena_lend::jemalloc`, which is Flow-IPC's recommend method of setting up arenas
   * like `*this` (though it can be done standalone also), shall periodically stat-consume (spoiler: partially
   * to keep the HI_WMARKs decently useful by sampling regularly, partially perhaps to log) in the background.
   * So if this were not thread-safe, the user couldn't safely stat-consume on-demand, as it could be concurrent
   * to the background activity.  One could argue this "just" demonstrates the usefulness of keeping this
   * thread-safe in the first place.
   */
  std::map<arena_id_t, Memory_manager_stats> m_mem_mgr_base_stats;
}; // class Ipc_arena

// Template implementations.

template<typename T, typename... Ctor_args>
Ipc_arena::Handle<T> Ipc_arena::construct(Ctor_args&&... ctor_args)
{
  using arena_lend::detail::Thread_lcl_obj_db_admin;
  using arena_lend::detail::use_ct_idx_t;
  using Disposer = arena_lend::detail::Owner_obj_disposer_and_mdt<Ipc_arena>;
  // using flow::util::construct_at; // C++20 => can conflict with incidentally included std:: counterpart.
  constexpr bool HAS_TRIVIAL_DTOR = std::is_trivially_destructible_v<T>;

  Thread_lcl_obj_db_admin<Ipc_arena>::this_thread_piggy_scan(); // Opportunistic!

  void* const addr = allocate(sizeof(T));

  pool_id_t lend_tracker_pool_id;
  use_ct_idx_t use_ct_idx;
  Thread_lcl_obj_db_admin<Ipc_arena>::this_thread_obj_db()
    ->constructing_obj(&lend_tracker_pool_id, &use_ct_idx, this, &m_pool_stats.m_obj_db_aux_pool, addr,
                       [](void* addr, Ipc_arena* arena)
  {
    if constexpr(!HAS_TRIVIAL_DTOR)
    {
      Activator ctx{arena};

      /* Call object's destructor (works for primitives but might be a pointless perf cost; have seen it in un-good
       * STL impls before, though the context was a bit different... but can't hurt to just skip it).
       * Key context: an allocator-equipped T (usually STL-compliant container) here will (in T::~T())
       * call-through to m_pool_collection->deallocate() for each buffer that T had allocated for itself
       * (e.g., for vector<> internal buffer; for list<> the various nodes).  In the process more destructors
       * may be invoked which would quite possibly do more dtor calling and thus deallocate()ing.  And so on.
       * Hence the present operator()() is called at just the outer layer, then the inner deallocations (if any)
       * happen, and then we call deallocate() on the outer object's memory, last-thing, just below. */
      static_cast<T*>(addr)->~T();
    }
    // else { Trivially destructible => no need to... you get the point. }

    arena->deallocate(addr);
  });

  /* Here too we try to get a perf boost by not unnecessarily using an arena-activator.  After all it involves
   * a thread-local variable assignment at the start and then another at the end plus 1-2 more to remember
   * the previous value; it's quick but not nothing.  It's trickier than the dtor situation above though.
   * Ideally we'd determine something like "T would not use an allocator to allocate something on its behalf."
   * There are some ideas, like maybe checking for the presence of tell-tale STL stuff... but it's tricky and
   * might be imperfect and thus arguably not worth it (@todo perhaps revisit).  However: using
   * is_trivially_destructible_v<T> here too is safe, even though it likely won't catch all the cases -- but
   * no false negatives, so it's safe.  Basically if it's trivially destructible, it can never allocate things
   * on its behalf in any sane way; so that fits the bill. */
  if (!addr) { return nullptr; }

  auto* const obj = static_cast<T*>(addr);
  if constexpr(HAS_TRIVIAL_DTOR)
  {
    flow::util::construct_at(obj, std::forward<Ctor_args>(ctor_args)...);
  }
  else
  {
    Activator ctx{this};
    flow::util::construct_at(obj, std::forward<Ctor_args>(ctor_args)...);
  }

  /* Recommend reading Disposer a/k/a Owner_obj_disposer_and_mdt class doc header; it is quite instructive
   * about the handful of things going on in this disposer, and how it relates to subsequent local
   * Shm_session::lend_object() and opposing Shm_session::borrow_object(). */
  return Handle<T>{obj,
                   Disposer{shared_from_this(), lend_tracker_pool_id, use_ct_idx}};
} // Ipc_arena::construct()

} // namespace ipc::shm::arena_lend::jemalloc
