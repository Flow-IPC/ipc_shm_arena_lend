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
#include "ipc/shm/arena_lend/jemalloc/ipc_arena.hpp"
#include "ipc/shm/arena_lend/jemalloc/jemalloc_pages.hpp"
#include "ipc/shm/arena_lend/jemalloc/thread_cache.hpp"
#include "ipc/shm/arena_lend/jemalloc/stat_info_dump.hpp"
#include "ipc/shm/arena_lend/jemalloc/detail/stat_substrate.hpp"
#include "ipc/shm/arena_lend/owner_shm_pool_listener.hpp"
#include "ipc/shm/arena_lend/detail/owner_shm_pool_repository.hpp"
#include "ipc/shm/arena_lend/shm_pool.hpp"
#include "ipc/shm/arena_lend/detail/thread_lcl_obj_db.hpp"
#include "ipc/util/process_credentials.hpp"
#include "ipc/common.hpp"
#include <flow/util/util.hpp>
#include <flow/util/stat/stat_set.hpp>
#include <flow/log/log.hpp>
#include <boost/range/algorithm/sort.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/algorithm/string.hpp>

namespace ipc::shm::arena_lend::jemalloc
{

// Local helpers.

namespace
{

/**
 * (Internal-use) Returns `true` <=> it is safe to consume jemalloc memory-manager stats (`Memory_manager_stats`)
 * given `call_timing` *and* the build environment.
 *
 * @param call_timing
 *        See util::Call_timing doc header(s).
 * @return See above.
 */
static bool mem_mgr_consume_ok([[maybe_unused]] util::Call_timing call_timing)
{
#if IPC_SHM_ARENA_LEND_JEMALLOC_CONCURRENT_ARENAS_UNSAFE
  return call_timing != util::Call_timing::S_POSSIBLY_UNSAFE;
#else
  return true; // Clean jemalloc linked: the bug does not exist; safe regardless of `call_timing`.
#endif
  // (Notice we are in .cpp as currently required for IPC_SHM_ARENA_LEND_JEMALLOC_CONCURRENT_ARENAS_UNSAFE checking.)
}

} // namespace (anon)

// Static initializers.

std::atomic<collection_id_t> Ipc_arena::m_collection_id_counter{0};

// Implementations.

std::shared_ptr<Ipc_arena> Ipc_arena::create(flow::log::Logger* logger, // Static.
                                             const std::shared_ptr<Memory_manager>& memory_manager,
                                             Shared_name&& pool_name_base,
                                             const util::Permissions& permissions)
{
  using std::shared_ptr;
  using std::unique_ptr;
  using ipc::shm::arena_lend::detail::Thread_lcl_obj_db_admin;

  Thread_lcl_obj_db_admin<Ipc_arena>::this_thread_piggy_scan(); // Opportunistic!

  // unique_ptr rather than raw pointer: start() can throw (jemalloc-arena creation failure); do not leak then.
  unique_ptr<Ipc_arena> arena{new Ipc_arena{logger, memory_manager, std::move(pool_name_base), permissions}};
  arena->start();

  /* We need to set up a special disposer for the returned shared_ptr.  Unfortunately this is a subtle situation.
   * Explanation is in destroy().  For now note merely that this disposer calls this_arena->destroy() -- does not
   * simply `delete this_arena` here.  destroy() might... but see its insides. */
  return shared_ptr<Ipc_arena>{arena.release(),
                               [](auto* this_arena) { this_arena->destroy(); }};
} // Ipc_arena::create()

void Ipc_arena::destroy()
{
  using ipc::shm::arena_lend::detail::Thread_lcl_obj_db_admin;
  using ipc::shm::arena_lend::detail::Thread_lcl_obj_db_client;
  using util::Call_timing;
  using flow::log::Log_context_mt;
  using flow::util::ostream_op_string;

  using std::shared_ptr;
  namespace str_algo = boost::algorithm;
  using boost::adaptors::transformed;

  /* Reminder: This is not some normal method executed normally.  Explanation:
   *
   * When is a *this destroyed?  Answer: Once the shared_ptr ref-count for the group returned
   * by create() factory reaches 0; and that occurs not merely when the user's outer shared_ptr copies/etc.
   * all disappear; but also (possibly among more things) all the shared_ptr<T>s returned by e.g.
   * construct<T>() also disappear: we intentionally capture a copy of this->shared_from_this() in the
   * disposer for each such shared_ptr<T>.  So they have to drop `*this` *and* all the objects
   * this->construct<T>()ed from it.  That is when, by default, the default deleter runs which simply
   * performs `delete this`.  In our case, though, this may need to be deferred for a few different activities
   * to complete first.
   *
   * At the current stage.  X=shared_ptr<Ipc_arena> handle reaching ref-count 0 implies 3 key things
   * (indeed that ref-count is 0 now):
   *   - `Shm_session`s to which X was added have all been destroyed (which by the way itself implies that
   *     all SHM-object-shared_ptr-handles borrow_object()ed through them have also reached their ref-count=0).
   *   - User has dropped X and any copies.
   *   - User has dropped any SHM-object-shared_ptr-handles construct()ed through *this. <-- ATTN
   *     A copy of X was placed into the shared_ptr disposer for each one; but now they are all gone.
   *
   * The last one (ATTN) is somewhat subtle: It means in this process there are no more references to every
   * object Y acquired as `Y = this->construct<T>(...)`.  It however does not mean every such object Y has
   * actually been deallocated (from SHM!).  There are at least two reasons for this, although one really
   * subsumes the other; but we mention both in the interests of education/explication.  The lesser reason is
   * that even if deallocation always occurred the moment it was warranted (no more users in either the lender
   * or the borrower remain), we might have lost contact with an opposing Shm_session prematurely -- e.g.,
   * just simply an opposing-process crash -- so the use-count decrements w/r/t objects borrow_object()ed by
   * that side never happened and might never happen; so a deallocation hasn't been warranted for them
   * (use-count must reach 0).
   *
   * But the greater/subsuming reason is that, assuming no crashing or anything weird, deallocations do *not*
   * occur instantly upon a given object Y's use-count reaching 0.  In fact only the Thread_lcl_obj_db_admin
   * per-thread object belonging to the thread that originally allocated Y -- or its degraded-mode thread
   * (see Thread_lcl_obj_db_admin; but we digress) -- is the one, exclusively, performing scans of
   * Y's use-count.  It can only do that opportunistically -- when a user calls into any of our APIs, and we
   * opportunistically perform Thread_lcl_obj_db_admin::piggy_scan().  We don't control the event loop of
   * that thread -- it's the user's thread! -- so this will only happen when it will happen; we can't, like,
   * .post() a thing that will do it.
   *
   * Naturally, deleting Y cannot occur once ~Ipc_arena() dtor executes; the jemalloc-arenas would be
   * potentially eliminated immediately (or a bit later depending on Thread_cache situation).  So then we
   * have a few options ostensibly.  1, we could just... not delete Y.  After all if the arena holding it
   * will be destroyed wholesale, who cares?  Well, no: User did nothing wrong whatsoever, and there haven't
   * been any IPC-errors necessarily; they at least expect the dtor of type *Y to run.  Of course we could
   * just execute dtor of the type *Y, but that brings us back essentially to the original problem; if the
   * type is an STL-compliant thing that was using our Stateless_allocator, then invoking that dtor will still
   * deallocate stuff, just not the outer object pointed-at by Y itself.
   *
   * Well then, 2, why *don't* we just delete Y right now, from *this* thread, while we have the instruction
   * pointer pointing into us?!  Answer: Ignoring the perf issues with that (it foregoes thread caching),
   * consider that now it's possible for another thread to be completely validly right now detecting that
   * the use-count for the Y pointee *is* zero -- it might be getting around to deleting Y right now itself.
   * So we'd have to synchronize, using a mutex or something both in here and in
   * Thread_lcl_obj_db_admin::piggy_scan().  This blows up the simplicity and lock-freeness of the existing
   * design; though given that only 2 threads can ever contend for this mutex, it isn't the most awful idea
   * actually.  Still, I (ygoldfel) am not a fan at least.
   *
   * So that leaves 3: Since we must wait for the proper thread to delete Y, and this will happen at the
   * earlier of either the user calling into *a* SHM-jemalloc API from that thread, or that thread exiting
   * normally, why not delay the actual destruction of *this until then?  So that's why we'll chain the
   * "retirement" of a *this as follows:
   *   - create()-returned shared_ptr<Ipc_arena> reaches ref-count 0 =>
   *     - our custom disposer runs... but doesn't delete the Ipc_arena; instead =>
   *   - it creates a new shared_ptr<Ipc_arena> Z with a different custom disposer; and
   *   - calls static Thread_lcl_obj_db_admin::forgetting_shm_arena(Z) which memorizes moved()d Z
   *     inside itself; and
   *   - sets up a cross-thread assignment to per-thread Thread_lcl_obj_db_admin thread-local objects,
   *     specifically to (1) opportunistically forget all objects originated-from arena Z; and (2) once done:
   *   - forget shared_ptr Z itself; which causes Z=shared_ptr<Ipc_arena> to reach ref-count 0 =>
   *     - our 2nd custom disposer runs... but still doesn't delete the Ipc_arena; instead =>
   *   - ...continue procedure.  Namely execute helper destroy_on_obj_db_forgot_us(); see further commentary there.
   *
   * Worth noting, even if it's not strictly within our purview here:
   *   - Due to the "lesser reason" a few paragraphs back,
   *     Thread_lcl_obj_db_admin::forgetting_shm_arena() shouldn't be looking for use-count=0 objects
   *     like a normal this_thread_piggy_scan(): due to e.g. an opposing-side crash, that might never be reached --
   *     but an Ipc_arena going away by contract means we consider that moot.  So it should get rid of
   *     everything to do with our `m_arena` ID(s), whether or not use-count 0 is in effect for a given thing.
   *   - Specifically, take object Y this->construct()ed by thread T1; all such objects must be deleted
   *     before Thread_lcl_obj_db_admin will be able to move on-to Ipc_arena::destroy_on_obj_db_forgot_us().
   *     When will this occur?
   *     - If thread T1 is still live: The earlier of:
   *       - T1 exits (~Thread_lcl_obj_db_admin() dtor runs for that thread-local obj-DB);
   *       - In T1, user calls into any SHM-jemalloc API.
   *     - If thread T1 has exited (then it would have launched degraded-mode thread T1' of its own):
   *       - Next time T1' periodically wakes up (as of this writing every 100msec). */

  Thread_lcl_obj_db_admin<Ipc_arena>::this_thread_piggy_scan(); // Opportunistic!

  // Log ~final info/stats.
  FLOW_LOG_INFO("Ipc_arena [" << this << "] handle has reached ref-count=0.  "
                "Before totally destroying it, we issue order to the cross-thread obj-DB admin/client-modules "
                "to forget this arena; admin-part shall occur potentially asynchronously, piggybacked onto "
                "user API calls and/or relevant threads exiting; once all have finished this "
                "forget-arena op, the last one to do so shall continue to destroy the Ipc_arena.");
  FLOW_LOG_INFO("Ipc_arena [" << this << "] shutdown: jemalloc-arena-ID list: " // Very useful diagnostic.
                "[" << str_algo::join(m_arenas | transformed([](auto arena_id) -> auto
                                                               { return ostream_op_string(arena_id); }),
                            ", ") << "]."); // BTW: reminder: they're in sorted order in m_arenas.
  {
    Info_dump dump; // Full verbosity, multi-line.
    dump.m_fmt.m_verbose = false; // Don't need the many-pages jemalloc-dump.  They can log it themselves if desired.
    info_dump(&dump,
              Call_timing::S_POSSIBLY_UNSAFE); // See jemalloc::Memory_manager doc header for explanation.
    FLOW_LOG_INFO("Ipc_arena [" << this << "] shutdown: "
                  "~Final state (includes ~final this-arena + ~current global):"
                  "\n" << dump << '.'); // Note: no newline at end of info_dump.
  }

  /* This is also required but is orthogonal to the much-discussed _admin-oriented stuff and lacks its complexities.
   * Here it's entirely about cleanliness.  At any rate: it's documented as being required, so do it synchronously. */
  Thread_lcl_obj_db_client<Ipc_arena>::forgetting_shm_arena(get_id());
  // Now back to complicated _admin stuff.

  /* Logging possibilities are about to get async-weird; and the first thing to potentially trigger asyncness
   * if the following _admin::forgetting_shm_arena() call.  So it's time to lock things down.
   *
   * Firstly save this; get_logger() is safe until the current method destroy() returns (to reiterate: it's on
   * the user to ensure resources given to Ipc_arena stay alive while Ipc_arena's `shared_ptr` group is alive --
   * which is still the case at the moment; we are in the disposer for that smart-pointer group).  So just make a
   * silly Log_contex_mt whose contents are valid and cannot change until the present method returns.
   *
   * We can give it to forgetting_shm_arena() et al; and for sync logging we can use it ourselves. */
  const Log_context_mt log_ctx_sync{get_logger(), get_log_component()};
  FLOW_LOG_SET_LOCKED_CONTEXT(&log_ctx_sync);
  /* Secondly, before asyncness might begin, ensure that any "regular" *this logging code (that uses get_logger()
   * such as FLOW_LOG_INFO/TRACE/WARNING()) sees null get_logger().  At this stage it is thread-safe: user cannot be
   * messing with *this while its original handle has ref-count 0, and we haven't kicked off anything
   * async/concurrent yet.  (Recall also that if *this is shutting down, then no construct()ed objects remain either,
   * so by now there should be no concurrent allocs/pool creations/etc.) */
  set_logger(nullptr);
  // Now, any "regular" logging => no-op; but _LOCKED() logging shall work synchronously.

  // Kick off the (potential) async-craziness.
  const bool obj_db_forgot_us_synchronously
    = Thread_lcl_obj_db_admin<Ipc_arena>
        ::forgetting_shm_arena(get_id(),
                               get_logger(), // For synchronous logging only (get_logger() is safe while in destroy()).
                               [this_arena_on_death_row_ptr = this]
                                 (const Log_context_mt* log_ctx) mutable
  {
    Thread_lcl_obj_db_admin<Ipc_arena>::this_thread_piggy_scan(); // Opportunistic!

    /* Careful.  If this is executing, then forgetting_shm_arena() did *not* complete its stuff synchronously,
     * and we're not executing within Ipc_arena::destroy() but rather form another thread.
     * User is allowed to let that *get_logger() be destroyed: they don't know the Ipc_arena
     * is still hanging around due to Thread_lcl_obj_db_admin (TLODBA) technicalities.  To help us with that
     * TLODBA::forgetting_shm_arena() gives us its Log_context_mt* (Logger*
     * therein could still be null, but the point is it's something
     * the user is at least indirectly in charge of ensuring exists while TLODBA is in action).  So
     * we log the following message using that Logger*, and we set the Ipc_arena's Logger to that.
     * Then inside the .reset() below this->destroy_on_obj_db_forgot_us() will be executing -- synchronously --
     * with a valid Logger*.  At least, the synchronous part of it (but not our problem here). */

    FLOW_LOG_SET_LOCKED_CONTEXT(log_ctx);
    FLOW_LOG_INFO_LOCKED("Ipc_arena death-row disposer executing for one of the per-thread admins, in thread "
                         "different from the one in which the shared_ptr<Ipc_arena> disposer initiated shutdown; "
                         "according to that last obj-DB admin: arena is safe to continue to destroy.");

    this_arena_on_death_row_ptr->destroy_on_obj_db_forgot_us(log_ctx);
  }); // Thread_lcl_obj_db_admin::forgetting_shm_arena()

  if (!obj_db_forgot_us_synchronously)
  {
    return; // The above lambda is memorized by _admin and will execute from another thread.  Done here.
  }
  /* else: The above lambda is forgotten: it is safe for us to proceed synchronously.
   *
   * We do basically the same thing, but as for logging: we are still synchronously in destroy(); get_logger()
   * is still safe; use the Log_context_mt we've set-up for that purpose.  (Our own log-call here uses it too.) */

  FLOW_LOG_INFO_LOCKED("shared_ptr<Ipc_arena> disposer initiated shutdown; obj-DB reports that no async "
                       "arena-forgetting work remains, so we can immediately/synchronously continue to "
                       "destroy arena.");
  destroy_on_obj_db_forgot_us(&log_ctx_sync);

  /* Careful!  It's very much possible this->destroy() *also* synchronously is able to finish
   * what it must -- and *right now here* *this is gone.  So no more this-> statements! */
} // Ipc_arena::destroy()

void Ipc_arena::destroy_on_obj_db_forgot_us(const flow::log::Log_context_mt* log_ctx)
{
  using Owner_shm_pool_repository = ipc::shm::arena_lend::detail::Owner_shm_pool_repository<Ipc_arena>;
  using flow::log::Log_context_mt;
  using std::shared_ptr;

  assert((!get_logger()) && "Regular get_logger()-based logging should have been disabled earlier by "
                              "destroy(); it's *log_ctx or bust from now on.");

  /* Per contract, any synchronous logging must go through *log_ctx.  (In practice, it is either
   * the regular get_logger() in-effect when destroy() was called, and we are still in destroy(); or this
   * is happening asynchronously/past destroy(), and *log_ctx is some Thread_lcl_obj_db_admin's.) */
  FLOW_LOG_SET_LOCKED_CONTEXT(log_ctx);

  /* What exactly happens from this point is in my (ygoldfel) opinion notoriously difficult to grasp from
   * reading the code.
   *
   * At risk of redundancy (versus destroy*() doc header(s) and further comments below) here is an
   * annotated recap to help navigate this touchy area.
   *
   *   -# m_destroy_started = true.  (Effect below.)
   *   -# For each of m_shm_pools, for each of m_listeners, .notify_removed_shm_pool().
   *      - If N = # of `Shm_session`s we are lent-through: There are N listeners through which this does:
   *        Shm_session::remove_lender_shm_pool().
   *        - As of this writing: no-op.  Might change to inform opposing process of pool-removal.
   *   -# For each of m_shm_pools: Owner_shm_pool_repository::get_instance().erase(pool_id).
   *   -# For each jemalloc-arena A (as of this writing: 1 of them):
   *      jemalloc::Memory_manager::destroy_arena(A)
   *   -# (The following occurs [a]synchronously in 1+ threads including this one.
   *       It is potentially async, because via jemalloc::Thread_cache::destroy_arena_safely(), this must
   *       occur via piggy-backing in 0+ other threads with jemalloc tcaches.  If only this thread has tcache(s),
   *       then it is synchronous.)
   *      For each arena A:
   *      - At least some (presumably all?) jemalloc-extents (vaddr-areas) are deinitialized/let-go-of.
   *        This triggers, via still-registered jemalloc-extent-hooks:
   *        pool-removal method(s) execute, cleaning up book-keeping; closing SHM-pool handles; deleting
   *        SHM-pool names from file-system (SHM-RAM returned to OS once all of that, plus SHM-pool handle freeing,
   *        also occurs in borrower(s), at latest on Shm_session destruction that
   *        might follow soon/already happened/happening now (but not necessarily, depending on user behavior; might
   *        be later).
   *        - Ipc_arena::on_shm_pool_removed() is called.  No-op: m_destroy_started is true.
   *          We already did what this would do, in step 3 above.  (Trying it again is not harmless; in particular
   *          various pools' base vaddrs are no longer in Owner_shm_pool_repository -- step 3 did it -- so it'd
   *          try to remove non-existent things and crash.)
   *   -# (This [a]sync-follows the last step of the destruction of the last arena A to be destroyed.)
   *      `delete this`.  `*this` Ipc_arena is destroyed.  Destructor is essentially no-op other than freeing RAM;
   *      The per-pool SHM-pool-handles should have been closed via arena destruction.
   *      Note: This last step is deferred, because the registered jemalloc-extents (their required bookkeeping) are
   *      in *this.
   *
   * We will explain hopefully each step again, but without the above tactical recap it has proven difficult
   * to see the forest for the trees in the past. */

  /* Prevents at least Owner_shm_pool_repository erase and listener notification from happening
   * below in the arena destruction (on_shm_pool_removed() early-returns when m_destroy_started). */
  m_destroy_started.store(true);

  /* Run listeners that would presumably otherwise run in arena destruction below as triggered
   * via jemalloc extent-hooks.  We've prevented that now and choose to do so all in one group from this one thread
   * and with no possibility of skipping any SHM-pools (if jemalloc for whatever reason doesn't trigger that).
   * We feel this involves less entropy overall.
   *
   * Then deregister from Owner_shm_pool_repository (after listeners, for consistency with on_shm_pool_removed()). */
  {
    Lock lock{m_shm_pools_and_listeners_mutex}; // Probably not necessary by now but just in case....

    FLOW_LOG_TRACE_LOCKED("Notifying listeners of [" << m_shm_pools.size() << "] shared memory pool removals; these "
                          "will not run via jemalloc extent-hooks anymore (though extent-hooks will execute soon; but "
                          "listeners will be skipped).");
    for (const auto& cur_shm_pool : m_shm_pools)
    {
      for (auto cur_listener : m_listeners)
      {
        cur_listener->notify_removed_shm_pool(cur_shm_pool);
        /* Logging safety mental check: Formally speaking the logging by whatever that triggers is that thing's
         * responsibility; we do not log there -- just invoke their virtual handler.
         *
         * Just in case let's think what "that thing" might be: As of this writing `Shm_session`s register
         * themselves for these updates; that's it.  Each Shm_session we are lent-through (.lend_arena(this)
         * essentially) does so.  That said, as of this writing, the Ipc_arena would not be destroyed until
         * those `Shm_session`s have all dropped their shared_ptr refs to *this; and that (again... as of this
         * writing) only happens when the `Shm_session`s go away which would unregister from m_listeners
         * (but so would a hypothetical .unlend_arena() which does not exist as of this writing).  So really
         * this should not even be executing.
         *
         * If other registering listeners exist, it might be a different story, but even then "their" logging is
         * "their" problem. */
      }
    } // for (cur_shm_pool : m_shm_pools)

    for (const auto& cur_shm_pool : m_shm_pools) // Pool-repository deregistration.
    {
      Owner_shm_pool_repository::get_instance().erase(cur_shm_pool->get_id());
      /* Logging safety mental check: Owner_shm_pool_repository has its own logging setup as a singleton; it
       * (and subordinate things like pool-lookup thread-local machinery) are responsible for maintaining
       * safe Logger*s and all. */
    }
  } // Lock lock{m_shm_pools_and_listeners_mutex}

  /* The rest: destroy jemalloc arenas.  See the big comment recap above for context.
   *
   * Please see destroy() for key background; then come back here and keep reading.
   * What does our dtor need to do?  Just this: For each ID in m_arenas, just:
   *   get_jemalloc_memory_manager()->destroy_arena(arena_id);
   *
   * It's actually important to know what it does.  Consider one arena
   * (it's the same for each one; plus realistically we just have one in practice as of this writing, per *this).
   * It won't just blow-up the vaddr areas a/k/a extents in the arena, and that's it.  That'd be nice arguably but...
   * actually it'll in civilized fashion, like, unmap/remove extents and things of that nature -- which in fact
   * will trigger the relevant extent hooks (see ctor), most notably remove_shm_pool_handler() which calls
   * remove_shm_pool() which calls super-class Owner_shm_pool_collection::remove_range_and_pool_if_empty()
   * which unmaps the extent and closes the SHM-pool (SHM-object) handle and even unlinks the pool name
   * (it disappears from file system, and once all handles across the system are also closed, the RAM is returned
   * fully).
   *
   * No problem though, right?  Just call ->destroy_arena().  Sadly -- see its doc header -- that doesn't
   * simply do the mallctl("...destroy") call; and that's because it potentially cannot.  You can read
   * its doc header and/or that for Thread_cache::destroy_arena_safely() (which is what it calls really), but
   * long story short: It is mandatory to destroy (really merely flush, but at this point why not destroy too)
   * every tcache that has ever been used in 1+ allocations against arena_id.
   *
   * Well, who cares?  Fine then, let ->destroy_arena() do it, however long it takes asynchronously; why should we
   * care?  Sigh: nope.  By that time, if we let our dtor run *now*, the extent hooks we'd registered
   * will refer to a non-existent *this; so that'll crash probably; and anyway the extents (pools) might be around
   * and mapped, but our Shm_pool objects, etc., are gone.
   *
   * Once that becomes clear the solution isn't that hard; we "just" can't let *this die yet.  ->destroy_arena()
   * takes an optional functor to execute right after it's actually safe to destroy the arena.  So we can capture
   * `this` and delete it in the functor.  destroy(), which led to us, used a similar technique, but it couldn't
   * delete `this` once done with is charge; so it called us.  We are the last thing though and can delete `this`
   * once done. */

  shared_ptr<Ipc_arena> this_arena_on_death_row{this};

  const auto disposer_func = [this_arena_on_death_row]
                               (const Log_context_mt* log_ctx, auto&& destroy_arena_func) mutable
  {
    /* Careful.  This might be running synchronously from destroy_on_obj_db_forgot_us() (which itself might
     * be, or might not be, running synchronously from destroy()), or it might be running from another thread
     * due to straggling Thread_cache-owning thread(s) needing to flush stuff first.  log_ctx is valid either
     * way; if synchronously from destroy_on_obj_db_forgot_us() then it is that guy's passed-in one; if async
     * then it is some Thread_cache's Log_context_mt instead.  (Meanwhile we reiterate: Ipc_arena's get_logger()
     * is null.  Again: log_ctx or bust from now on.)
     *
     * If `m_arenas.size() == 1`, then there is one disposer_func copy and one this_arena_on_death_row copy.
     * So it'll reach ref-count 0 below, causing finally the destruction of the Ipc_arena *this.
     *
     * If `m_arenas.size() > 1`, then there are a couple possibilities.  There are then N disposer_func()
     * (and this_arena_on_death_row) copies out there, and they might execute serially or (at least partially)
     * concurrently.  If serially: nothing special to say.  If concurrency occurs:
     * Ipc_arena and its super-classes can handle concurrent extent hooks executing; after all
     * an allocate() might coincide with another in another thread and a deallocate() in another, etc.
     * And that's for the *same* jemalloca-arena (e.g., m_arena0); if they're separate (the case here)
     * then definitely fine. / Regardless: this_arena_on_death_row is 1 of N copies, so if it's the last
     * of the N to be handled, then ref-count 0 belos is reached + *this dies.  Otherwise not. */

    FLOW_LOG_SET_LOCKED_CONTEXT(log_ctx);
    FLOW_LOG_INFO_LOCKED("Ipc_arena death-row disposer executing for one of the jemalloc-arenas; it is safe to "
                         "destroy.  For internal reasons logging from resulting extent-hooks firing during this "
                         "arena-destruction has been disabled.  Destroying jemalloc arena now.");

    /* A bunch of SHM-pool removals (might) occur synchronously in here, triggering hooks => calling
     * stuff in *this Ipc_arena which may well log... but we've set get_logger() to null in destroy(); so
     * sadly that stuff will not be visible.
     *
     * @todo It would be better if it were visible: if those hooks could log.  It would merely require us to
     * hook up log_ctx to those log call sites.  Tactically this is a bit difficult: Consider at least two parts
     * of *this hierarchy: Ipc_arena itself (extent hooks logging) and Shm_pool_collection super-super-class
     * (pool-ops, like closing of pool, logging).  At the moment they just use Log_context super-class's stuff
     * for regular FLOW_LOG_INFO()/etc. calls.  We'd need to set something somewhere that would cause such
     * log-call-sites to use `Log_context_mt* log_ctx` (which we've spent some serious effort in obtaining across
     * dangerous territory) instead of the normal thing.  (Having Shm_pool_collection/Ipc_arena/et-al derive
     * from Log_context_mt does not do it: *log_ctx might be some Thread_cache's log-context; loading the get_logger()
     * from there into a hypothetical *this Log_context_mt defeats the point: If user does set_logger(), it will
     * be against Thread_cache/its Log_context_mt -- not whatever we copied out of it.)  It is definitely doable
     * but requires auditing all of the potentially-affected code and having it do the proper hoop-jumping to
     * select the proper Log_context_mt (without forgetting about perf).  Worth looking into: the arena-end
     * logging could be helpful in practice.  We do at least log here ourselves (but the hooks won't). */
    destroy_arena_func();

    /* If `m_arenas.size() == 1` then: The following *will* trigger the default deleter, namely,
     *   delete this_arena_on_death_row.get()
     * If `m_arenas.size() > 1` then: The following *might* do that, but across the disposer_func() copies
     * it'll do that exactly once.
     *
     * (We could omit the line, but it'd happen momentarily anyway.  This way at least it's nice for
     * debugger sessions.) */
    this_arena_on_death_row.reset();
  }; // const auto disposer_func =
  using Disposer_func = decltype(disposer_func);

  /* So now, for each arena_id in series, it will either actually destroy the arena right now or defer it
   * until thread(s) get a chance to do it.  Since we still have this_arena_on_death_row on the stack, `*this` won't be
   * destroyed within the loop, even if all of the arenas are such that they got destroyed synchronously. */
  for (const auto arena_id : m_arenas)
  {
    FLOW_LOG_INFO_LOCKED("Destroying arena (possibly deferred due to tcache dependency), "
                         "jemalloc-ID [" << arena_id << "].");
    get_jemalloc_memory_manager()->destroy_arena(arena_id, log_ctx,
                                                 // Make a copy, as it's a destructive && arg.
                                                 Disposer_func{disposer_func});
  }

  assert(this_arena_on_death_row);
  /* Now this_arena_on_death_row will go out of scope, so it's possible `delete this` happens right here.
   * E.g.: `m_arenas.size() == 1`; Thread_cache was only ever used (allocations only ever happened) right in this
   * thread; so the single arena just got destroyed. */
} // Ipc_arena::destroy_on_obj_db_forgot_us()

Ipc_arena::Ipc_arena(flow::log::Logger* logger,
                     const std::shared_ptr<Memory_manager>& memory_manager,
                     Shared_name&& pool_name_base,
                     const util::Permissions& permissions) :
  Owner_shm_pool_collection(logger,
                             ++m_collection_id_counter,
                             memory_manager,
                             std::move(pool_name_base),
                             permissions),
  m_owner_id(uint64_t(util::Process_credentials::own_process_id())),
  m_arena0(0), // Not valid.
  m_extent_hooks_wrapper({ .alloc = &create_shm_pool_handler,
                           .dalloc = &optional_remove_shm_pool_handler,
                           .destroy = &remove_shm_pool_handler,
                           .commit = &commit_memory_pages_handler,
                           .decommit = &decommit_memory_pages_handler,
                           .purge_lazy = nullptr,
                           .purge_forced = &purge_forced_memory_pages_handler,
                           .split = &split_memory_pages_handler,
                           .merge = &merge_memory_pages_handler },
                         this),
  m_destroy_started(false)
{
  // Yep.
}

void Ipc_arena::start()
{
  start_impl(1); // This probably looks silly, and it is, but see the 1 doc header(s).
}

void Ipc_arena::start_impl(unsigned int n_arenas)
{
  assert((n_arenas != 0) && "At least 1 arena please.");
  assert(m_arenas.empty() && "Do not call start() twice.");
  for (unsigned int i = 0; i != n_arenas; ++i)
  {
    FLOW_LOG_INFO("Creating arena (idx [" << i << "]/0-based of [" << n_arenas << "]/1-based).");

    const arena_id_t arena_id = get_jemalloc_memory_manager()->create_arena(&m_extent_hooks_wrapper);

    /* @todo Catch exception if that fails... return false....  There's a to-do as of this writing in create()
     * doc header about this.
     *
     * Informationally:
     *
     * That call itself just created our first SHM-pool for this arena (via the extent-alloc hook,
     * synchronously inside): jemalloc's *base* (metadata) block, wherein the arena's internal metadata lives --
     * including the native-arena structure itself.  Sibling blocks (pools) can appear later if metadata grows enough.
     * User data never live in these (nothing we know of relies on this fact per se, but it's good to know).
     * The block extents (<=> SHM-pools for us) are also destroyed (during arena shutdown; see destroy())
     * differently from data-storing extents (that will appear due to subsequent allocate()s triggering that same
     * extent-alloc hook); that fact is actually salient for us -- see optional_remove_shm_pool(). */

#ifndef NDEBUG
    const auto result =
#endif
    m_arenas.emplace(arena_id);
    assert(result.second);

    m_mem_mgr_base_stats.try_emplace(arena_id); // Per-arena reset-state baseline (zero-init).

    FLOW_LOG_INFO("Created arena, jemalloc-ID [" << arena_id << "].");
  }

  m_arena0 = *(m_arenas.begin());

  /* Establish the per-arena reset baselines from the freshly-created (zero-count) arenas.  Beyond zeroing the
   * scalar baselines, this importantly gives the base histograms their bucket structure -- required for the
   * per-bucket since-reset deltas (`-=`) computed in memory_manager_stats(). */
  memory_manager_stats_reset();
} // Ipc_arena::start_impl()

void* Ipc_arena::allocate(size_t size)
{
  assert(!m_arenas.empty() && "start() must have been called by now.");
  const auto& arena_id = m_arena0;

#if IPC_SHM_ARENA_LEND_JEMALLOC_NO_TCACHE
  // ^-- Tcache support is stable but turning it off can simplify specific profiling/debugging.
  if (skip_fast_path_verbose_logging())
  {
    return get_jemalloc_memory_manager()->allocate(size, arena_id);
  }
  // else

  FLOW_LOG_DATA("Allocating size [" << size << "], arena [" << arena_id << "], no tcache.");
  void* const buf = get_jemalloc_memory_manager()->allocate(size, arena_id);
  FLOW_LOG_DATA("Allocated size [" << size << "], arena [" << arena_id << "], no tcache, "
                "resulting in [" << buf << "].");
  return buf;
#else // #if !IPC_SHM_ARENA_LEND_JEMALLOC_NO_TCACHE
  // Suggest reading Thread_cache class doc header for background on jemalloc-tcache.

  /* Get (and first create if needed) cache object (Thread_cache) for this thread.
   * Get (and first create if needed) jemalloc tcache for this thread for this arena.
   *
   * Reminder: jemalloc docs don't make this clear, and it doesn't appear to be widely known, but both
   * empirical evidence and links like https://github.com/jemalloc/jemalloc/issues/460 show that, while
   * a given tcache (and tcache ID X) can be used to jemalloc-mallocx() against 2+ arenas A and B, this leads
   * to potential trouble: such an mallocx(B) shall absolutely sometimes yield an address in an A-owned pool.
   * For us this is totally unacceptable.  (Perhaps when allocating is always in heap, without arenas ever dying
   * throughout, it would have been fine; not fine for us at all.)
   *
   * It does make sense, if one has the properly simple conception of how jemalloc-tcache
   * works; namely that it is essentially nothing more than a thread-local intermediary store of
   * what would normally be mallocx()-returned pointers, sitting between (1) us with our actual-mallocx() calls
   * (get_j_m_m()->allocate() above) and actual-dallocx() calls (in this->deallocate()) and (2) jemalloc and its
   * jemalloc-arenas (this arena_id but also all the other ones).  So if I do an initial mallocx() here,
   * it might seed this thread's TL-cache with (across a single mutex-lock) hundreds of
   * what-would-normally-be-mallocx()es' returned pointers from arena_id, yes.  But if I do another initial mallocx()
   * here, for another arena -- arena_id2 -- it may well do the same thing; now this TL-cache of allocated-buffer
   * pointers has pointers into arena_id and arena_id2; jemalloc's tcache does not auto-segregate by source-arena.
   * Now a subsequent mallocx(arena_id) might see the cache has lots of juicy pointers to give out -- no need to
   * lock any mutex to do it -- and will hand out one of the ones in the TL-cache... which may well have come
   * from arena_id2.  As stated before, we can't have that: arena_id2's Ipc_arena might go away, while arena_id's
   * Ipc_arena is still around; the (one would expect) perfectly-alive mallocx()-result pointer from (one would
   * expect) arena_id (which is indeed perfectly alive) now points at destroyed-arena-backed garbage. */
  const auto tcache_id_for_arena = Thread_cache::this_thread_cache()->id(arena_id);

  if (skip_fast_path_verbose_logging())
  {
    return get_jemalloc_memory_manager()->allocate(size, arena_id, tcache_id_for_arena);
  }
  // else

  FLOW_LOG_DATA("Allocating size [" << size << "], arena [" << arena_id << "], "
                "tcache [" << tcache_id_for_arena << "].");
  void* const buf = get_jemalloc_memory_manager()->allocate(size, arena_id, tcache_id_for_arena);
  FLOW_LOG_DATA("Allocated size [" << size << "], arena [" << arena_id << "], tcache "
                "[" << tcache_id_for_arena << "], resulting in [" << buf << "].");
  return buf;
#endif // #elif !IPC_SHM_ARENA_LEND_JEMALLOC_NO_TCACHE
} // Ipc_arena::allocate()

void Ipc_arena::deallocate(void* address)
{
  assert(!m_arenas.empty() && "start() must have been called by now.");
  const auto& arena_id = m_arena0;

#if IPC_SHM_ARENA_LEND_JEMALLOC_NO_TCACHE
  if (skip_fast_path_verbose_logging())
  {
    get_jemalloc_memory_manager()->deallocate(address, arena_id);
  }
  else
  {
    FLOW_LOG_DATA("Deallocating address [" << address << "], arena [" << arena_id << "], no tcache.");
    get_jemalloc_memory_manager()->deallocate(address, arena_id);
    FLOW_LOG_DATA("Deallocated address [" << address << "], arena [" << arena_id << "], no tcache.");
  }
#else // #if !IPC_SHM_ARENA_LEND_JEMALLOC_NO_TCACHE
  // Suggest reading Thread_cache class doc header for background on jemalloc-tcache.

  /* It would seem we could, much as in allocate(), just do
   *    tcache_id_for_arena = Thread_cache::this_thread_cache()->id(arena_id);
   * so that the Thread_cache and inside it the per-arena jemalloc-tcache are created if not currently in existence.
   * We intentionally avoid this, treating this thread's Thread_cache read-only (our data structure, not
   * the jemalloc-internal tcache: it is certainly potentially written-to -- if it exists, but the point is
   * we don't make it exist, if it doesn't already).  Reason:
   *
   * In many cases, in and of itself, it would be fine and good.  Say allocate(arena_id) returned `address` in
   * thread T1, and we are deallocate()ing in thread T2, and one has never allocate(arena_id)ed in T2 so far.
   * Creating the tcache here would push `address` into a new tcache, and further deallocate(arena_id)s would
   * keep pushing onto it sans mutex-locking -- better than the normal mutex-locking no-tcache dallocx().
   *
   * The danger area is during, generally speaking, "things" going down -- the user would not be triggering
   * deallocate() themselves, but our obj-DB tracking system might.  1, at thread exit
   * detail::Thread_lcl_obj_db_admin can (much as during thread's lifetime) delete construct()ed objects that
   * lack live user handles (a/k/a zombie objects; note these do not represent an error or anomaly, despite the
   * name).  2, when *this Ipc_arena is going down (destroy() path), it unconditionally similarly deletes
   * any still-live construct()ed objects.  For (1), that thread's Thread_cache might have been cleaned-up
   * already, so at least the outer deallocate() during said deletion would re-create Thread_cache.  It should
   * still be cleaned-up again quite soon by Thread_local_state_registry/Thread_local_ptr/thread_specific_ptr
   * stubborn-cleanup machinery, but this is hard to reason about and harder to maintain over time.  For (2),
   * as of this writing it is probably safe actually: first
   * Thread_lcl_obj_db_admin::forgetting_shm_arena() does the deletes, then triggers the relevant Thread_cache
   * cleaning, so it should be still around when needed (during the deletes).  Again, though, it is hard to
   * reason about and maintain.  We just don't want that trouble.
   *
   * There is a cost to this, all else being equal: Until the first arena_id allocate() in thread T,
   * any deallocate()s (of things allocate()d in other threads: totally possible and common enough) will
   * do the full-mutex-locking tcache-bypassing operation that returns the buffer to jemalloc's arena-set.
   * With an active tcache, it could skip that and return to the tcache intermediary store instead.  This is a
   * real cost, but it is well mitigated.
   *   - Consider a given deallocate().  Often (not always), it is part of an outer-object's
   *     deletion by Thread_lcl_obj_db_admin (TLODBA): at least the deallocate() of the object's raw sizeof().
   *     If not (e.g., an in-SHM `vector` being move-assigned-to => deletion of overwritten buffer), the same
   *     thread T probably (not always) would've TLODBA-deleted *some* outer object at least once.  So what?
   *     They're still DEallocations; doesn't help.  True: but TLODBA-deletion of object X in thread T
   *     means X *must* have been construct()ed in thread T.  If so, then the construct() would first-thing
   *     have allocate(sizeof())d... and that first-thing would have create the tcache after all.
   *   - Supposing the "often" and "probably" in the prev bullet are accurate, there is the possible corner case
   *     (totall allowed and not an error though) that TLODBA-deletion in thread T is actually handling object X
   *     that was created in another thread T2, not T: but this specifically only happens in the
   *     degraded-admin drain-thread TLODBAs.  For that reason
   *     Thread_lcl_obj_db_admin::degraded_admin_thread_body(), in controlled fashion, pre-creates the tcache
   *     ahead of any TLODBA-deletes it would then do.
   *
   * @todo This does still leave a hole as noted: It is possible -- hard to say how common but "feels" not-too-common --
   * that some thread might *just* deallocate() lots and lots, without allocate()ing once throughout.  It is OK:
   * it just won't get the tcache perf-bonus for those deallocate()s.  Nevertheless it'd be nice to plug the hole.
   * All it would take is either some heuristic... but let's aim higher in this to-do... or knowing to be
   * read-only (like now) only in the danger-zone -- we think, as explained above, around thread exit and
   * arena destruction.  Any mechanism for determining this is TBD; it is unlikely to be trivial but feels doable.
   * The degraded_admin_thread_body() heuristic can then also be removed. */

  const auto this_thread_cache_or_null = Thread_cache::this_thread_cache_or_null();
  const auto tcache_id_for_arena = this_thread_cache_or_null ? this_thread_cache_or_null->id_or_none(arena_id)
                                                             : Thread_cache::S_NO_TCACHE_ID;

  if (tcache_id_for_arena != Thread_cache::S_NO_TCACHE_ID)
  {
    /* In this thread, tcache (for relevant arena) exists.  Use it -- regardless of which thread allocated
     * the buffer: a tcache is merely a thread-local store of pointers to this arena's regions awaiting
     * reuse; a region carries no memory of the tcache (if any) that handed it out; so any this-arena region
     * may be sitting in the calling thread's tcache.  (See discussion at end of function for how it comes to
     * be -- and how commonly -- that the allocating thread differs from ours.) */

    // Fast-path:
    if (skip_fast_path_verbose_logging())
    {
      get_jemalloc_memory_manager()->deallocate(address, arena_id, tcache_id_for_arena);
    }
    else
    {
      FLOW_LOG_DATA("Deallocating address [" << address << "], arena [" << arena_id << "], tcache "
                    "[" << tcache_id_for_arena << "].");
      get_jemalloc_memory_manager()->deallocate(address, arena_id, tcache_id_for_arena);
      FLOW_LOG_DATA("Deallocated address [" << address << "], arena [" << arena_id << "], tcache "
                    "[" << tcache_id_for_arena << "].");
    }
    return;
  } // if (tcache_id_for_arena != Thread_cache::S_NO_TCACHE_ID)
  // else: No tcache available in this thread (yet), for this arena.

  // Do a similar thing to the NO_TCACHE snippet above, just with modified logging.
  if (skip_fast_path_verbose_logging())
  {
    get_jemalloc_memory_manager()->deallocate(address, arena_id);
  }
  else
  {
    FLOW_LOG_TRACE("jemalloc::deallocate[" << address << "] will proceed with tcache disabled, even though (like all "
                   "allocations at this layer) it was allocated with tcache enabled; reason: "
                   "we are in a thread that has not yet requested tcache creation "
                   "for arena [" << arena_id << "] (presumably because it has in this arena "
                   "not yet allocated anything at this layer).  Slight perf loss results.");

    FLOW_LOG_DATA("Deallocating address [" << address << "], arena [" << arena_id << "], no tcache.");
    get_jemalloc_memory_manager()->deallocate(address, arena_id);
    FLOW_LOG_DATA("Deallocated address [" << address << "], arena [" << arena_id << "], no tcache.");
  }

  /* Discussion for context: Suppose end user's actual code *outside any SHM-aware allocators* refrains from making
   * direct [de]allocate() calls (which isn't disallowed either incidentally).  That represents not-guaranteed
   * but definitely recommended behavior.  So in that case, can deallocate() even occur in a thread other than
   * the buffer's allocate() thread?  (Not that anything above requires a particular answer; but it affects
   * tcache-usage patterns, so it is worth understanding.)
   *
   * At first it probably seems like it's not, and that would be good; various reasoning becomes much simpler.
   * It seems like it's not possible: user must do construct<T>() which triggers allocate(sizeof(T).
   * Thread_lcl_obj_db* machinery is built on the idea that *once it is determined the T goes out of scope, in
   * whichever thread or process*, only the original constructing thread will (opportunistically) destroy the T,
   * calling deallocate().  As of this writing you can see that in construct() where it passes a really-destroy-T
   * functor to Thread_lcl_obj_db_admin->constructing_obj().  Awesome!
   *
   * Indeed, when T is simple, without allocating/deallocating buffers on its behalf, the above holds, and that's
   * great.  It might not hold, though, if T allocates/deallocates on its behalf.  We're assuming things are done
   * in recommended fashion (assumption at top of this comment); in which case T is/contains container(s).
   * Take 1 container, a vector<char>.  In the simplest case one might construct<T>(10): so the outer [de]allocate()
   * of sizeof(T) occurs as explained above, all good.  In addition though when T{10} executes it'll
   * ask the SHM-aware allocator (our Stateless_allocator recommended) to allocate 10.  Allocator calls
   * allocate(10), that same thread: cool.  Eventually handle to the T go away, and _admin deallocate(sizeof(T))s
   * as explained, then calls ~T(); that asks Stateless_allocator to deallocate the 10; allocator
   * thus calls deallocate().  Awesome!  All still in the proper thread.
   *
   * However, one could resize the vector in some other thread in-between, requiring a bigger buffer.  That'll
   * deallocate() (<-- "wrong" thread), allocate() ("wrong" thread).  At that point the possibilities are endless;
   * they could let it live until ~T(); then the 2nd allocate() is matched with original-thread deallocate()
   * (mismatch).  Or they could do more allocs/deallocs, in various threads that may or may not match each other.
   * Bottom line: If the user does allocs/deallocs via T=vector<char>, and happens to do so in other threads than
   * where they chose to originally construct() => mismatch.  Totally possible.
   *
   * For completeness (?) consider also that one need not even use construct().  One can declare a
   * `vector<char, Stateless_allocator<...>> v{100, 'A'};` on the stack, or similarly on the heap.  The SHM-allocating
   * allocator will do allocate(100) there.  Granted, why do this, if this `v` cannot be lent/borrowerd (shared
   * with different process -- the point of using SHM)?  Well, by itself, true; but it's allowed and normal
   * to have such temp guys sitting around; one can then construct() -- in some other thread -- another vector
   * and move v's buffer into it: `construct<vector<...>>(std::move(v))`.  Or maybe it'd be more realistically into
   * a construct()ed struct with a vector<char, Stateless_allocator<...>> member.  Either way: might be different
   * thread than the construct()ing one -- in this case the buffer in question even predates the construct()!
   *
   * So that's how. */
#endif // #elif !IPC_SHM_ARENA_LEND_JEMALLOC_NO_TCACHE
} // Ipc_arena::deallocate()

void Ipc_arena::this_thread_ensure_tcache_exists() const
{
#if !IPC_SHM_ARENA_LEND_JEMALLOC_NO_TCACHE
  Thread_cache::this_thread_cache()->id(m_arena0);
#endif
}

void* Ipc_arena::create_shm_pool_handler(extent_hooks_t* extent_hooks, // Static.
                                         void* address, size_t size, size_t alignment, bool* zero,
                                         bool* commit, unsigned arena_id)
{
  assert(extent_hooks);
  return static_cast<Extent_hooks*>(extent_hooks)->get_owner()
           ->create_shm_pool(address, size, alignment, zero, commit, arena_id);
}

bool Ipc_arena::optional_remove_shm_pool_handler(extent_hooks_t* extent_hooks, // Static.
                                                 void* address, size_t size,
                                                 bool committed, unsigned arena_id)
{
  assert(extent_hooks);
  return !static_cast<Extent_hooks*>(extent_hooks)->get_owner()
            ->optional_remove_shm_pool(address, size, committed, arena_id);
}

void Ipc_arena::remove_shm_pool_handler(extent_hooks_t* extent_hooks, // Static.
                                        void* address, size_t size, bool committed, unsigned arena_id)
{
  assert(extent_hooks);
  static_cast<Extent_hooks*>(extent_hooks)->get_owner()
    ->remove_shm_pool(address, size, committed, arena_id);
}

bool Ipc_arena::commit_memory_pages_handler(extent_hooks_t* extent_hooks, // Static.
                                            void* address, size_t size, size_t offset,
                                            size_t length, unsigned arena_id)
{
  assert(extent_hooks);
  return !static_cast<Extent_hooks*>(extent_hooks)->get_owner()
            ->commit_memory_pages(address, size, offset, length, arena_id);
}

bool Ipc_arena::decommit_memory_pages_handler(extent_hooks_t* extent_hooks, // Static.
                                              void* address, size_t size, size_t offset, size_t length,
                                              unsigned arena_id)
{
  assert(extent_hooks);
  return !static_cast<Extent_hooks*>(extent_hooks)->get_owner()
            ->decommit_memory_pages(address, size, offset, length, arena_id);
}

bool Ipc_arena::purge_forced_memory_pages_handler(extent_hooks_t* extent_hooks, // Static.
                                                  void* address, size_t size, size_t offset, size_t length,
                                                  unsigned arena_id)
{
  assert(extent_hooks);
  return !static_cast<Extent_hooks*>(extent_hooks)->get_owner()
            ->purge_forced_memory_pages(address, size, offset, length, arena_id);
}

bool Ipc_arena::split_memory_pages_handler(extent_hooks_t* extent_hooks, // Static.
                                           void* address, size_t size, size_t size_a, size_t size_b,
                                           bool committed, unsigned arena_id)
{
  assert(extent_hooks);
  return !static_cast<Extent_hooks*>(extent_hooks)->get_owner()
            ->split_memory_pages(address, size, size_a, size_b, committed, arena_id);
}

bool Ipc_arena::merge_memory_pages_handler(extent_hooks_t* extent_hooks, // Static.
                                           void* address_a, size_t size_a, void* address_b, size_t size_b,
                                           bool committed, unsigned arena_id)
{
  assert(extent_hooks);
  return !static_cast<Extent_hooks*>(extent_hooks)->get_owner()
            ->merge_memory_pages(address_a, size_a, address_b, size_b, committed, arena_id);
}

void* Ipc_arena::create_shm_pool(void* address, size_t size, size_t alignment, bool* zero,
                                 bool* commit, arena_id_t arena_id)
{
  using ipc::shm::arena_lend::detail::Shm_pool_offset_ptr_data_base;
  using flow::util::stat::fetch_add;
  using std::string;
  using std::shared_ptr;

  assert(zero);
  assert(commit);

  /* Ultra-unique ID generated here. It's used as a key in various maps including borrower-side global maps.
   *
   * For context: at least Lend_tracker_pool uses the same pool-ID namespace (same pool name prefix for a given
   * Ipc_arena generating these "real" SHM-pools and the Lend_tracker_pool "aux" SHM-pools that we use to track
   * objects within the "real" ones; both use this generator function).  This at least allows wholesale cleanup
   * (via file-system) of both types of things when needed.  If we had some other type of aux pool, we could
   * have it use the same technique for the same reason(s). */
  const auto id = Shm_pool_offset_ptr_data_base::generate_pool_id();
  // It may also be encoded in the name, to provide uniqueness and for convenience in debugging and such.
  const string name = generate_shm_object_name(id).str(); // @todo Should be using Shared_name ~throughout.

  shared_ptr<Shm_pool> pool
    = Owner_shm_pool_collection::create_shm_pool(id, name, size, address,
                                                 [&](int fd, size_t size, void* address) -> void*
  {
    void* const actual_address = Jemalloc_pages::map(address, size, alignment, *commit, fd);
    if (!actual_address)
    {
      return nullptr; // Handle error. @todo Fill in.
    }

    // Extracted (somewhat) from extent_mmap.c
    *zero = *commit;

    FLOW_LOG_TRACE("Mapped SHM pool at address [" << actual_address << "].");

    return actual_address;
  });

  void* pool_address;
  if (pool)
  {
    pool_address = pool->get_address();
    FLOW_LOG_TRACE("Created SHM pool at address [" << pool_address << "], name [" << name << "], "
                   "size [" << size << "], arena [" << arena_id << "].");

    { // Stats.  (See also to-do in the `else` branch.)
      fetch_add(&m_pool_stats.m_owner_pool.m_pool_create_count, 1);
      fetch_add(&m_pool_stats.m_owner_pool.m_pool_create_sz, size);
    }
  }
  else
  {
    // Failed to create pool.
    pool_address = nullptr;
    FLOW_LOG_WARNING("Failed to create SHM pool of size [" << size << "].");

    /* @todo Stat-member doc header says pool-create request is always satisfied.  This WARNING/code-path looks to me
     * (ygoldfel) like it indicates pretty much a catastrophic/unexpected situation that is hardly recoverable.
     * We are somewhere in the middle of a jemalloc alloc itself in the middle of a user SHM-alloc, too, so
     * it's not like reporting the problem (whatever it even might be) to the user is straightforward -- how *would*
     * we even do it, and what % of users would even try to handle this tiny possibility every time?
     * The point: Probably we should FLOW_LOG_FATAL() and std::abort() here and in places like it.
     * There's probably a bigger-scope to-do to that effect elsewhere (as SHM-jemalloc is littered with attempts
     * at civilized handling of errors that doesn't really reach the user in an intended way and is therefore
     * arguably more trouble than a crash-out); I nevertheless wanted to emphasize this particular one.
     *
     * Historical note: Code originally written by another developer (echan), and while we have refactored much of
     * *this class since then, this extent-hook area is quite deep/low-level, and generally works well, so beyond
     * cosmetic tweaks we have not touched it in similar ways.  This to-do is a way in which there is still some
     * refactoring (at least arguably) still left. */
  }

  const auto logger = get_logger();
  if (logger && logger->should_log(flow::log::Sev::S_TRACE, get_log_component()))
  {
    print_shm_pool_map();
  }

  return pool_address;
} // Ipc_arena::create_shm_pool()

bool Ipc_arena::optional_remove_shm_pool([[maybe_unused]] void* address,
                                         [[maybe_unused]] size_t size,
                                         [[maybe_unused]] bool committed,
                                         [[maybe_unused]] arena_id_t arena_id)
{
  using flow::util::stat::fetch_add;

  /* @todo MGCOGS-385 - Create decision algorithm
   * @todo Track down preceding ticket (echan-filed)/file in project ticket database/possibly update comment. */

  /* Always retain for now.
   *
   * Notes by ygoldfel (original author: echan):
   *
   * jemalloc is saying, "you can unmap this vaddr area [a range of a SHM-pool in our
   * case; possibly a whole one], if you want; or you can refuse, and I'll memorize it -- still mapped -- in my
   * *retained* set and reuse it for allocations later."  (On our declining, jemalloc shall also -- via our
   * decommit/purge hooks -- release the range's backing physical pages by SHM-object hole-punching; so
   * retention costs vaddr and SHM-pool-count footprint, not RAM.  See also `stats.*.retained` discussion in
   * Memory_manager::create_arena().)  At some point
   * users reported pools (note: vaddr areas; does not mean actual RAM use until allocation in there happens)
   * growing to huge sizes, so echan did a couple of basic things w/r/t keeping existing areas for reuse.  This was
   * one of them, I think.  Another was capping the size of any single such vaddr area (SHM-pool):
   * `retain_grow_limit`, set at arena creation (again see Memory_manager::create_arena()).
   *
   * The above concerns mid-arena-life requests.  This hook also fires during native-arena destruction (see
   * destroy()); there:
   *   - For data extents declining is moot: they proceed to the mandatory-removal (extent-destroy) hook, which
   *     removes their pools properly.
   *   - The arena's *base* (metadata) block extents (see start_impl()), though, are offered *only* through
   *     this hook (jemalloc's internal base_unmap() never uses the destroy hook as of jemalloc-5.3.1 at least) --
   *     so declining here leaves their pool(s) registered, un-removed.
   *     That is fine: ~Owner_shm_pool_collection() eliminates any such straggler pools all the same.  Intentionally
   *     we don't complicate our logic here for the sake of the destroy-time cleanup of those pools; the
   *     aforementioned dtor does so.  By definition nothing by that point is allowed to require any pools,
   *     base-block or otherwise, to persist.
   *     @todo *Possibly* consider extending that philosophy to *all* destroy-time pool removal.  That is,
   *     one could argue that once destroy() begins we can stop really participating in jemalloc's extent-dance
   *     too much; do the minimal stuff so jemalloc-arena-destroy API can proceed, but don't remove anything through
   *     those hooks anymore; and then simply let ~Owner_shm_pool_collection() dtor remove them all.
   *     All that said: No need to fix what isn't broken.  It's best to pursue this only if it makes something
   *     else simpler. */

  { // Stats.
    fetch_add(&m_pool_stats.m_owner_pool.m_pool_optional_destroy_request_count, 1);
  }
  return false;

  // Reminder: Try to keep various Memory_manager_stats doc headers accurate if changing logic around here.
}

bool Ipc_arena::remove_shm_pool(void* address, size_t size, bool committed, arena_id_t arena_id)
{
  using flow::util::stat::fetch_add;
  using std::shared_ptr;
  size_t pool_size;

  Memory_decommit_functor decommit_functor
    = [](const shared_ptr<Shm_pool>& shm_pool, size_t offset, size_t length) -> bool
  {
    return Jemalloc_pages::decommit(reinterpret_cast<void*>(reinterpret_cast<uint8_t*>(shm_pool->get_address())
                                                            + offset),
                                    shm_pool->get_fd(), offset, length, true);
  };

  bool removed_range;
  bool unmapped_pool;
  const bool result
    = remove_range_and_pool_if_empty
        (address,
         size,
         committed ? &decommit_functor : nullptr,
         removed_range,
         [&pool_size](const shared_ptr<Shm_pool>& shm_pool) -> bool
           { Jemalloc_pages::unmap(shm_pool->get_address(), pool_size = shm_pool->get_size()); return true; },
         unmapped_pool);
  if (!result)
  {
    FLOW_LOG_WARNING("Failure when performing range removal at address [" << address << "], size "
                     "[" << size << "], arena [" << arena_id << "], removed-range [" << removed_range << "], "
                     "unmapped-pool [" << unmapped_pool << "].");
    return removed_range;
  }

  if (unmapped_pool)
  {
    FLOW_LOG_TRACE("Unmapped SHM pool when removing range at address [" << address << "], "
                   "size [" << size << "], arena [" << arena_id << "].");
    print_shm_pool_map();

    { // Stats.
      fetch_add(&m_pool_stats.m_owner_pool.m_pool_destroy_count, 1);
      /* Technically (maybe even for realz) `size` merely = sz of last range to be removed from pool, if
       * for whatever reason (maybe upon splitting original extent/pool into 2+ extents?) jemalloc then
       * (during arena shutdown, we are pretty sure) told us to remove the whole thing piecemeal by specifying
       * those sub-extents one-by-one.  So obtain the entire pool's size opportunistically above. */
      fetch_add(&m_pool_stats.m_owner_pool.m_pool_destroy_sz, pool_size);
    }
  }

  // If we removed the range, no physical memory would persist, although virtual memory leak may occur
  return removed_range;

  // Reminder: Try to keep various Memory_manager_stats doc headers accurate if changing logic around here.
} // Ipc_arena::remove_shm_pool()

bool Ipc_arena::commit_memory_pages(void* address, size_t size, size_t offset, size_t length,
                                    [[maybe_unused]] arena_id_t arena_id)
{
  std::shared_ptr<Shm_pool> pool;
  // The offset from the start of the originally created pool (i.e., file offset).
  size_t pool_offset;
  if (!compute_pool_and_offset(address, size, offset, length, "committing", pool, pool_offset))
  {
    return false;
  }

  void* const page_address = reinterpret_cast<void*>(reinterpret_cast<uint8_t*>(address) + offset);
  const bool result = Jemalloc_pages::commit(page_address, length);

  if (!skip_fast_path_verbose_logging())
  {
    FLOW_LOG_TRACE("Committing with success [" << result << "], page address "
                   "[" << page_address << "], length [" << length << "] in pool [" << *pool << "].");
  }

  return result;

  // Reminder: Try to keep various Memory_manager_stats doc headers accurate if changing logic around here.
}

bool Ipc_arena::decommit_memory_pages(void* address, size_t size, size_t offset, size_t length,
                                      [[maybe_unused]] arena_id_t arena_id)
{
  std::shared_ptr<Shm_pool> pool;
  // The offset from the start of the originally created pool (i.e., file offset).
  size_t pool_offset;
  if (!compute_pool_and_offset(address, size, offset, length, "decommitting", pool, pool_offset))
  {
    return false;
  }

  void* const page_address = reinterpret_cast<void*>(reinterpret_cast<uint8_t*>(address) + offset);
  const bool result = Jemalloc_pages::decommit(page_address, pool->get_fd(), pool_offset, length);

  if (!skip_fast_path_verbose_logging())
  {
    FLOW_LOG_TRACE("Decommitting with success [" << result << "], page address [" << page_address <<
                   "], pool offset [" << pool_offset << "], length [" << length <<
                   "] in pool [" << *pool << "].");
  }

  return result;

  // Reminder: Try to keep various Memory_manager_stats doc headers accurate if changing logic around here.
}

bool Ipc_arena::purge_forced_memory_pages(void* address, size_t size, size_t offset, size_t length,
                                          [[maybe_unused]] arena_id_t arena_id)
{
  std::shared_ptr<Shm_pool> pool;
  // The offset from the start of the originally created pool (i.e., file offset).
  size_t pool_offset;
  if (!compute_pool_and_offset(address,
                               size,
                               offset,
                               length,
                               "force purging",
                               pool,
                               pool_offset))
  {
    return false;
  }

  const bool result = Jemalloc_pages::purge_forced(pool->get_fd(), pool_offset, length);

  if (!skip_fast_path_verbose_logging())
  {
    FLOW_LOG_TRACE("Force-purge with success [" << result << "], pool offset [" << pool_offset << "], "
                   "length [" << length << "] in pool [" << *pool << "].");
  }

  return result;

  // Reminder: Try to keep various Memory_manager_stats doc headers accurate if changing logic around here.
}

bool Ipc_arena::split_memory_pages(const void* address, size_t size, size_t size_a, size_t size_b,
                                   [[maybe_unused]] bool committed, arena_id_t arena_id)
{
  // There should not be an existing region that spans across segment boundaries, so it should always be okay to split
  if (!skip_fast_path_verbose_logging())
  {
    FLOW_LOG_TRACE("Allowed split of memory pages at [" << address << "], size [" << size << "] "
                   "to sizes [" << size_a << ", " << size_b << "], arena [" << arena_id << "].");
  }

  return true;
}

bool Ipc_arena::merge_memory_pages(const void* address_a, size_t size_a, const void* address_b, size_t size_b,
                                   [[maybe_unused]] bool committed,
                                   arena_id_t arena_id)
{
  using std::shared_ptr;

  shared_ptr<Shm_pool> pool_a = lookup_shm_pool(address_a);
  if (!pool_a)
  {
    FLOW_LOG_WARNING("Could not find SHM pool for address [" << address_a << "] in merge request.");
    return false;
  }

  shared_ptr<Shm_pool> pool_b = lookup_shm_pool(address_b);
  if (!pool_b)
  {
    FLOW_LOG_WARNING("Could not find SHM pool for address [" << address_b << "] in merge request.");
    return false;
  }

  if (pool_a != pool_b)
  {
    /* Pools don't match, so we cannot merge.  This situation can arise, if the pools were originally created
     * adjacent to each other. */
    if (!skip_fast_path_verbose_logging())
    {
      FLOW_LOG_TRACE("Could not merge distinct pools from address A [" << address_a << "] and "
                     "B [" << address_b << "].");
    }
    return false;
  }

  if (!Shm_pool::is_adjacent(address_a, size_a, address_b, size_b))
  {
    /* Address is not adjacent, which should not occur.
     * @todo So then assert()?  Check other extent-hook error paths too. */
    FLOW_LOG_WARNING("Merge request for non-adjacent regions (address, size) [" << address_a << ", " << size_a << "] "
                     "and [" << address_b << ", " << size_b << "], arena [" << arena_id << "].");
    return false;
  }

  // Ensure both ranges are within the pool
  if ((!pool_a->is_subset(address_a, size_a)) || (!pool_a->is_subset(address_b, size_b)))
  {
    // One of the ranges is not a subset, which should not occur.
    FLOW_LOG_WARNING("Merge request for a region (address, size) [" << address_a << ", " << size_a << "] or "
                     "[" << address_b << ", " << size_b << "] that is not wholly within pool [" << *pool_a << "].");
    return false;
  }

  if (!skip_fast_path_verbose_logging())
  {
    FLOW_LOG_TRACE("Allowed merge of memory pages at [" << address_a << "], size [" << size_a << "] "
                   "with [" << address_b << "], size [" << size_b << "], arena [" << arena_id << "].");
  }

  return true;
} // Ipc_arena::merge_memory_pages()

bool Ipc_arena::compute_pool_and_offset(void* address, size_t size, size_t offset,
                                        size_t length, util::String_view use_case,
                                        std::shared_ptr<Shm_pool>& pool, size_t& pool_offset) const
{
  // A bit confusing what the mission is; but doc header should make it clear.  See that.

  assert(length > 0);

  pool = lookup_shm_pool(address);
  if (!pool)
  {
    // Failed
    FLOW_LOG_WARNING("When " << use_case << " pages, could not find pool with address [" << address << "].");
    return false;
  }

  Shm_pool::size_t address_offset;
  if (!pool->is_subset(address, size, &address_offset))
  {
    // Alert - specified range is not complete resident in pool
    FLOW_LOG_WARNING("Requested pool [" << address << "], size [" << size << "] does not completely "
                     "reside in pool.");
    return false;
  }

  // Avoid overflow in checks.  @todo What does this comment mean?  Figure out/clarify. -ygoldfel
  if ((offset > size) || (length > size) || (offset + length) > size)
  {
    FLOW_LOG_WARNING("Requested offset [" << offset << "] and length [" << length << "] is > size "
                     "[" << size << "].");
    return false;
  }

  pool_offset = address_offset + offset;
  return true;
} // Ipc_arena::compute_pool_and_offset()

bool Ipc_arena::add_shm_pool_listener(Owner_shm_pool_listener* listener)
{
  {
    Lock lock{m_shm_pools_and_listeners_mutex};

    const auto result_pair = m_listeners.emplace(listener);
    if (!result_pair.second) // @todo assert() and/or abort() would probably be OK and make things simpler.
    {
      FLOW_LOG_WARNING("Could not add already existing SHM pool listener [" << listener << "] to "
                       "collection [" << get_id() << "].");
      return false;
    }

    listener->notify_initial_shm_pools(m_shm_pools);
  }

  FLOW_LOG_TRACE("Successfully added SHM pool listener [" << listener << "] to collection [" << get_id() << "].");

  return true;
}

bool Ipc_arena::remove_shm_pool_listener(Owner_shm_pool_listener* listener)
{
  {
    Lock lock{m_shm_pools_and_listeners_mutex};

    if (m_listeners.erase(listener) == 0) // @todo assert() and/or abort() would probably be OK and make things simpler.
    {
      FLOW_LOG_WARNING("Could not remove non-existent SHM pool listener [" << listener << "] from "
                       "collection [" << get_id() << "].");
      return false;
    }
  }

  FLOW_LOG_TRACE("Successfully removed SHM pool listener [" << listener << "] from collection [" << get_id() << "].");

  return true;
}

void Ipc_arena::on_shm_pool_created(const std::shared_ptr<Shm_pool>& shm_pool)
{
  using Owner_shm_pool_repository = ipc::shm::arena_lend::detail::Owner_shm_pool_repository<Ipc_arena>;
  using std::shared_ptr;

  if (m_destroy_started.load())
    // See destroy_on_obj_db_forgot_us().  Perf: full-synchronized flag access, but it is rare.
  {
    return;
  }
  // else:

  const auto shm_pool_id = shm_pool->get_id();

  {
    // Register SHM pool
    Lock lock{m_shm_pools_and_listeners_mutex};

    auto result_pair = m_shm_pools.emplace(shm_pool);
    if (!result_pair.second)
    {
      // This will eventually lead to inconsistency and issues, so abort.
      FLOW_LOG_FATAL("Could not insert SHM pool [" << *shm_pool << "], existing registration "
                     "[" << *(result_pair.first) << "] in collection [" << get_id() << "].  Bug?");
      assert(false && "Could not insert SHM pool; existing registration; bug?");
      std::abort();
      return;
    }
    FLOW_LOG_TRACE("Registered SHM pool [" << shm_pool_id << "] in collection [" << get_id() << "].");

    // Register in the global owner-side pool repository (enables Shm_pool_offset_ptr resolution).
    Owner_shm_pool_repository::get_instance().insert(shared_ptr<Shm_pool>{shm_pool});

    // Notify listeners.
    for (auto cur_listener : m_listeners)
    {
      cur_listener->notify_created_shm_pool(shm_pool);
    }
  }

  FLOW_LOG_TRACE("Successfully handled SHM pool creation notification of pool [" << shm_pool_id << "] in collection "
                 "[" << get_id() << "].");
} // Ipc_arena::on_shm_pool_created()

void Ipc_arena::on_shm_pool_removed(const std::shared_ptr<Shm_pool>& shm_pool, bool)
{
  using Owner_shm_pool_repository = ipc::shm::arena_lend::detail::Owner_shm_pool_repository<Ipc_arena>;

  if (m_destroy_started.load())
    // See destroy_on_obj_db_forgot_us().  Perf: full-synchronized flag access, but it is rare.
  {
    return;
  }
  // else:

  const auto shm_pool_id = shm_pool->get_id();

  {
    // Remove SHM pool
    Lock lock{m_shm_pools_and_listeners_mutex};

    if (m_shm_pools.erase(shm_pool) == 0) // @todo assert() and/or abort()?
    {
      FLOW_LOG_WARNING("SHM pool [" << shm_pool_id << "] not found in collection [" << get_id() << "].");
      return;
    }

    FLOW_LOG_TRACE("Deregistered SHM pool [" << shm_pool_id << "] from collection [" << get_id() << "].");

    // Notify listeners.
    for (auto cur_listener : m_listeners)
    {
      cur_listener->notify_removed_shm_pool(shm_pool);
    }

    // Deregister from the global owner-side pool repository (undoes on_shm_pool_created() registration).
    Owner_shm_pool_repository::get_instance().erase(shm_pool_id);
  }

  FLOW_LOG_TRACE("Successfully handled SHM pool removal notification of pool [" << shm_pool_id << "] in "
                 "collection [" << get_id() << "].");
} // Ipc_arena::on_shm_pool_removed()

arena_id_t Ipc_arena::get_jemalloc_arena_id() const
{
  return m_arena0;
}

std::shared_ptr<Memory_manager> Ipc_arena::get_jemalloc_memory_manager() const
{
  return std::static_pointer_cast<Memory_manager>(get_memory_manager());
}

Ipc_arena::Uniq_arena_id Ipc_arena::uniq_arena_id() const
{
  /* @todo The owner-PID half is captured at construction (#m_owner_id); we haven't really looked into whether
   * PID could change over time in exotic situations.  So -- kinda sorta maybe? -- this might not match the owner_id the
   * way an opposing Shm_session records it (its `m_remote_process_id`) when IDing opposing arenas; which would
   * matter only if one cross-referenced this owner-side output against borrower-side reporting.  This is mere
   * reporting regardless, so it is OK until proven otherwise; but maybe revisit sometime. */
  return { m_owner_id, get_id() };
}

void Ipc_arena::info_dump(Info_dump* target_info_dump, util::Call_timing call_timing)
{
  using flow::util::stat::stats_assign;
  using std::vector;

  assert(target_info_dump);
  target_info_dump->m_mem_mgr_stats = mem_mgr_consume_ok(call_timing) ? memory_manager_stats()
                                                                      : vector<Memory_manager_stats>{};
  target_info_dump->m_mem_mgr_stats_dump = get_jemalloc_memory_manager()->stats_dump_to_string();

  sharded_stats(&target_info_dump->m_sharded_stats);
  stats_assign(&target_info_dump->m_pool_stats, pool_stats());
  stats_assign(&target_info_dump->m_obj_db_aux_pool_global_stats, obj_db_aux_pool_global_stats());
  stats_assign(&target_info_dump->m_owner_pool_lookup_global_stats, owner_pool_lookup_global_stats());
  target_info_dump->m_shm_pool_live_info = shm_pool_live_info();
} // Ipc_arena::info_dump()

std::vector<Ipc_arena::Shm_pool_info> Ipc_arena::shm_pool_live_info() const
{
  using std::vector;

  const auto uniq_id = uniq_arena_id();

  vector<Shm_pool_info> shm_pools_id_sorted;
  for_each_shm_pool([&](auto&& shm_pool_ptr)
  {
    shm_pools_id_sorted.push_back({ shm_pool_ptr->get_id(), static_cast<size_t>(shm_pool_ptr->get_size()),
                                    uniq_id, 1 });
  });

  return shm_pools_id_sorted;
}

void Ipc_arena::sharded_stats(Sharded_stats* target_stats)
{
  namespace stat = ipc::shm::arena_lend::detail::stat;
  using flow::util::stat::stats_assign;

  Lock lock{m_sharded_stats_mutex};
  stat::sharded_stats(*this, &m_sharded_stats);
  if (target_stats) // See sample_hi_wmarks();
  {
    stats_assign(target_stats, m_sharded_stats);
  }
}

void Ipc_arena::sharded_stats_reset()
{
  Lock lock{m_sharded_stats_mutex};
  ipc::shm::arena_lend::detail::stat::sharded_stats_reset(*this, &m_sharded_stats);
}

const Ipc_arena::Pool_stats& Ipc_arena::pool_stats() const
{
  return m_pool_stats;
}

void Ipc_arena::pool_stats_reset()
{
  flow::util::stat::stats_reset(&m_pool_stats, {});
}

const Ipc_arena::Owner_pool_lookup_global_stats& Ipc_arena::owner_pool_lookup_global_stats() // Static.
{
  using ipc::shm::arena_lend::detail::Pool_lookup_global_stats;

  return Pool_lookup_global_stats<Ipc_arena, true>::stats();
}

const Ipc_arena::Obj_db_aux_pool_global_stats& Ipc_arena::obj_db_aux_pool_global_stats() // Static.
{
  using ipc::shm::arena_lend::detail::Thread_lcl_obj_db_client;

  return Thread_lcl_obj_db_client<Ipc_arena>::obj_db_aux_pool_global_stats();
}

void Ipc_arena::global_stats_reset() // Static.
{
  using ipc::shm::arena_lend::detail::Pool_lookup_global_stats;
  using ipc::shm::arena_lend::detail::Thread_lcl_obj_db_client;

  Pool_lookup_global_stats<Ipc_arena, true>::stats_reset();
  Thread_lcl_obj_db_client<Ipc_arena>::obj_db_aux_pool_global_stats_reset();
}

void Ipc_arena::this_thread_gc() // Static.
{
  using ipc::shm::arena_lend::detail::Thread_lcl_obj_db_admin;

  Thread_lcl_obj_db_admin<Ipc_arena>::this_thread_piggy_scan();
}

std::vector<Ipc_arena::Memory_manager_stats> Ipc_arena::memory_manager_stats()
{
  using detail::stat::config_stats_enabled;
  using detail::stat::epoch_refresh;
  using flow::util::stat::stats_since_reset_state;
  using std::vector;

  vector<Memory_manager_stats> result_id_sorted;
  result_id_sorted.reserve(m_arenas.size());

  // The arena-identifying ID is ours to know regardless of jemalloc's build flags; set it on every row.
  const auto uniq_id = uniq_arena_id();

  /* The jemalloc-sourced gauges require a stats-enabled jemalloc build.  If absent: still emit a row per arena
   * (with IDs set), but leave the real stat-members at defaults (zeroes probably) -- as advertised. */
  const bool stats_on = config_stats_enabled();
  if (stats_on)
  {
    epoch_refresh(); // Once => a consistent snapshot across all the arenas read just below.
  }

  /* m_arenas is a std::set => iteration is ascending by native (jemalloc) arena-ID; hence the result is sorted
   * that way, as promised by our contract. */
  for (const auto arena_id : m_arenas)
  {
    auto& row = result_id_sorted.emplace_back();
    row.m_uniq_arena_id = uniq_id;
    row.m_native_arena_id = uint64_t(arena_id);

    if (!stats_on)
    {
      continue; // Leave this row's gauges + HWMs at default (all zero).
    }
    // else: populate the raw stat-members, then fold them into the since-reset view (including HI_WMARKs).

    memory_manager_vaddr_stats_read(arena_id, &row.m_vaddr); // Raw values; no lock needed (jemalloc is thread-safe).
    memory_manager_alloc_stats_read(arena_id, &row.m_alloc); // Ditto.

    /* Formally speaking we do here what flow::util::stat::stats_since_reset_state() doc header prescribes:
     * read the raw values from data-source (jemalloc-stats), then do the following against -- and potentially
     * updating -- m_mem_mgr_base_stats[arena_id].  As a reminder though: ACCUMULATORs are -=ed against the base values;
     * GAUGEs are left alone; HI_WMARKs are chosen as the higher of the new GAUGE value and the one in
     * m_mem_mgr_base_stats; and lastly if the former then the HI_WMARK in base struct is updated to the new max too
     * (for next time we do this if any). */
    Lock lock{m_mem_mgr_base_stats_mutex};
    stats_since_reset_state(&row, &(m_mem_mgr_base_stats.at(arena_id)));
  } // for (arena_id : m_arenas)

  return result_id_sorted;
} // Ipc_arena::memory_manager_stats()

void Ipc_arena::memory_manager_stats_reset()
{
  using detail::stat::config_stats_enabled;
  using detail::stat::epoch_refresh;
  using flow::util::stat::stats_mark_reset_state;

  /* Formally speaking we do here what flow::util::stat::stats_since_reset_state() doc header prescribes.
   * Read that guy please for details of how it all works.  Basically, though, we are to grab the raw
   * values for the data-source, which results in the following items used in subsequent stat-consumptions
   * (memory_manager_stats()): ACCUMULATOR saved for a delta computation; HI_WMARK saved to current GAUGEd
   * value, since by definition the max since right-now = value right-now.  That last part is achieved
   * via stats_mark_reset_state().
   *
   * Only meaningful with a stats-enabled jemalloc; otherwise there is nothing to read (bases stay zero). */
  if (!config_stats_enabled())
  {
    return;
  }
  // else:

  epoch_refresh();

  for (const auto arena_id : m_arenas)
  {
    /* Install current raw reading as the new baseline, then mark (sets each HI_WMARK = its gauge => running
     * max restarts at "now").  Lock: we write the shared base value.  See #m_mem_mgr_base_stats_mutex. */
    Lock lock{m_mem_mgr_base_stats_mutex};
    auto& base = m_mem_mgr_base_stats.at(arena_id);
    memory_manager_vaddr_stats_read(arena_id, &base.m_vaddr);
    memory_manager_alloc_stats_read(arena_id, &base.m_alloc);
    stats_mark_reset_state(&base);
  } // for (arena_id : m_arenas)
} // Ipc_arena::memory_manager_stats_reset()

void Ipc_arena::memory_manager_vaddr_stats_read(arena_id_t arena_id, Memory_manager_stats::Vaddr* vaddr) const
{
  using flow::util::ostream_op_string;
  using detail::stat::page_size;
  using detail::stat::mallctl_read;

  assert(vaddr);

  const auto page_sz = page_size();
  const auto read_sz = [&](const char* leaf, size_t* target)
  {
    mallctl_read(ostream_op_string("stats.arenas.", arena_id, '.', leaf).c_str(), target);
  };
  const auto read_pages_sz = [&](const char* leaf) -> size_t
  {
    size_t n_pages = 0;
    mallctl_read(ostream_op_string("stats.arenas.", arena_id, '.', leaf).c_str(), &n_pages);
    return n_pages * page_sz;
  };

  read_sz("mapped", &vaddr->m_mapped_sz);
  read_sz("retained", &vaddr->m_retained_sz);
  read_sz("resident", &vaddr->m_resident_sz);
  vaddr->m_active_sz = read_pages_sz("pactive");
  vaddr->m_inactive_resident_sz = read_pages_sz("pdirty");
  vaddr->m_inactive_muzzy_sz = read_pages_sz("pmuzzy");
} // Ipc_arena::memory_manager_vaddr_stats_read()

void Ipc_arena::memory_manager_alloc_stats_read(arena_id_t arena_id, Memory_manager_stats::Alloc* alloc) const
{
  using flow::util::ostream_op_string;
  using detail::stat::mallctl_read;

  assert(alloc);

  const auto read_u64 = [&](const char* leaf) -> uint64_t
  {
    uint64_t val = 0;
    mallctl_read(ostream_op_string("stats.arenas.", arena_id, '.', leaf).c_str(), &val);
    return val;
  };
  const auto read_sz = [&](const char* leaf) -> size_t
  {
    size_t val = 0;
    mallctl_read(ostream_op_string("stats.arenas.", arena_id, '.', leaf).c_str(), &val);
    return val;
  };

  alloc->m_alloc_count = read_u64("small.nmalloc") + read_u64("large.nmalloc");
  alloc->m_dealloc_count = read_u64("small.ndalloc") + read_u64("large.ndalloc");
  alloc->m_live_count = alloc->m_alloc_count - alloc->m_dealloc_count;
  alloc->m_live_sz = read_sz("small.allocated") + read_sz("large.allocated");

  /* Cumulative bytes alloc-ed/dealloc-ed + the by-size-class alloc-count histogram: all derived via the
   * per-size-class (bins + lextents) walk. */
  detail::stat::cumulative_alloc_dealloc_sz(arena_id,
                                            &alloc->m_alloc_sz, &alloc->m_dealloc_sz,
                                            &alloc->m_histo_alloc_count_by_sz);
} // Ipc_arena::memory_manager_alloc_stats_read()

void Ipc_arena::sample_hi_wmarks(util::Call_timing call_timing)
{
  FLOW_LOG_TRACE("Ipc_arena [" << this << "]: Stat-consuming a couple things: high-water-mark refresh requested.");

  sharded_stats(nullptr);
  if (mem_mgr_consume_ok(call_timing))
  {
    memory_manager_stats();
  }
}

std::ostream& operator<<(std::ostream& os, const Ipc_arena& val)
{
  // @todo Something more useful than just this?
  return os << '@' << &val;
}

} // namespace ipc::shm::arena_lend::jemalloc
