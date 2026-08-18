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

/* Unit test of the Thread_lcl_obj_db_* machinery (admin + client) -- specifically the garbage-collection
 * timing contract for owner-side SHM-constructed objects.  The facts under test:
 *   - If the *constructing* thread itself drops the last handle, the object is destroyed right then, inline
 *     (its own disposer both piggy-scans and, finding itself at use-count zero in its home thread, reaps).
 *   - If the use-count instead reaches zero due to activity *elsewhere* -- another thread dropping the
 *     handle (the client-side disposer path, exercised here) or another process returning a borrow (not
 *     re-created here) -- the object is *not* destroyed at that moment: it becomes pending-reclamation
 *     ("zombie") until the next *piggy-scan* runs in its constructing thread.  There are no background
 *     threads in normal operation; the scan piggy-backs on documented trigger operations.  Nobody asserted
 *     that trigger contract before this test (historical impetus: a test hang, when a FINISH handler
 *     expected reclamation that nothing triggered).
 *
 * We test through the *public* trigger surface (jemalloc::Ipc_arena + handle drops), not TLODB's internal
 * API: the trigger sites' presence in the public ops is precisely the contract worth defending.
 * Things-to-test list (each annotated with its TEST below):
 *   - Cross-thread drop yields a zombie (not reaped by the dropping thread); then each no-session trigger op
 *     in the constructing thread reaps it: this_thread_gc(); construct() on the same arena; construct() on an
 *     *unrelated* arena (the "unrelated action still kicks it" property); Ipc_arena::create() of a brand-new
 *     arena; an (inline-reaping) handle drop, whose piggy-scan takes any zombie along with it.  Also:
 *     this_thread_gc() with nothing pending is a harmless no-op; and the inline own-thread-drop reap itself.
 *     [TEST Triggers_reap_pending]
 *   - A *held* handle (use-count nonzero) is never reaped by scans; content stays intact; reap happens only
 *     after the (cross-thread) drop plus a subsequent constructing-thread trigger.
 *     [TEST Held_handle_defers_reap]
 *   - Only the constructing thread's scans reap its zombies: another thread's explicit GC -- or even a full
 *     construct/drop cycle of its own on the same arena -- must leave the first thread's zombie alone.
 *     [TEST Wrong_thread_reaps_nothing]
 *   - Thread-exit scanning: A constructing thread that exits with zombies still pending must reap them all:
 *     its thread-local cleanup runs one final, exhaustive scan.  The test makes that scan work maximally
 *     hard as follows.  The scan first reaps the zombies listed in a fixed-capacity hint array
 *     (Lend_tracker_pool::S_N_UNUSED_IDX_HINTS slots); anything the hints missed must then be found by
 *     walking the remaining objects one by one, oldest to newest.  So we create more zombies than the hint
 *     array can hold.  The overflow -- which by construction includes the newest object of all -- can then
 *     only be reaped by that walk running to its very end.  Reaping at the walk's last element is thus
 *     asserted deliberately.  (Inside the code there is some tricky iterator logic; in fact there was a bug
 *     there at one point.)  [TEST Thread_exit_reaps]
 *     - Ditto with the constructing thread being a raw std::thread, whose
 *       thread-local cleanup runs at a different shutdown point (after C++ thread_local deinit) than a
 *       boost::thread-based loop's.  [TEST Thread_exit_reaps_std_thread]
 *   - Degraded-admin scanning: A constructing thread exiting with objects still *live* (handles held
 *     elsewhere) hands its object-DB to a spawned *drain-thread* a/k/a degraded admin thread.  That thread
 *     then reaps within its poll period once the handles drop.  [TEST Degraded_thread_drains]
 *   - "The Gap": stat-consumption (Ipc_arena::sharded_stats()) racing _admin/_client thread-exit must
 *     never yield an incomplete aggregate -- during thread-exit a shard is briefly in no walkable registry,
 *     and thread_end_gap_mutex() exists to serialize consumption around that window (see its epic doc
 *     header).  A consumer thread hammers sharded_stats() while constructing threads rapid-cycle through
 *     both thread-exit scenarios (exit-with-zombies -> shard lands in Finalized_shards; exit-with-live-objects ->
 *     shard moves to a drain-thread's replacement _admin); accumulator totals observed by the consumer must
 *     never regress, and the quiesced totals must reconcile exactly.  (*Quiesced*, here and in that TEST:
 *     every constructing thread joined, every drain-thread's work done, the consumer stopped -- nothing
 *     in-flight remains, so the totals are frozen, and exact-value asserts become legal, whereas mid-churn
 *     only bounds/invariants can be asserted.)  [TEST Stats_consume_vs_thread_exit_gap]
 *
 * Threading setup: the main thread performs *no* arena/TLODB operations whatsoever -- all activity runs in
 * two task-loop threads ("tlodbOwn" = the constructing/owner thread; "tlodbOth" = the other guy), which are
 * stopped/joined at each test's end, taking their thread-local state with them.  This both keeps the test
 * environment clean for whatever runs next in the process and, since the ops are posted-and-awaited one at a
 * time, keeps the test logic effectively single-threaded.
 *
 * Trigger sites involving Shm_session (lend_object()/borrow_object()) are deliberately not re-created here:
 * they invoke the same this_thread_piggy_scan(), and shm_session_test's Standalone_* tests exercise those
 * paths with real sessions.
 *
 * @todo The end-of-program atexit() backstop (atexit_degraded_admin_threads_join(): The background is this.
 * Each aforementioned drain-thread a/k/a degraded admin thread is "semi-detached."  Almost nothing awaits
 * its joining with one exception: A drain-thread *still draining* when main() returns must be joined --
 * allowed to finish its reaps -- rather than be killed by process exit; so we defer any such joining
 * via the std::atexit() machinery.  The to-do is to test the actual joining occurring then, if it occurs.
 * As of this writing it is exercised only structurally: Any suite run whose tests spawn
 * drain-threads registers the handler and runs its everyone-already-retired branch at exit (our
 * drain-threads finish mid-test); the still-draining-at-exit branch is never provoked/asserted.  Doing so
 * would require a child-process harness (child: construct; exit the thread with objects live; drop the
 * handles; return from main() immediately; parent: assert clean exit code + post-main-reap evidence in
 * output).  Such harnesses exist elsewhere (e.g., shm_session_test's server launcher), so it's quite doable.
 * Deliberately deferred duet to low risk versus cost.  Nevertheless revisit and complete this. */

#include "ipc/shm/arena_lend/jemalloc/ipc_arena.hpp"
#include "ipc/shm/arena_lend/jemalloc/memory_manager.hpp"
#include "ipc/shm/arena_lend/detail/lend_tracker_pool.hpp"
#include "ipc/shm/arena_lend/arena_lend_fwd.hpp"
#include "ipc/shm/arena_lend/test/test_shm_object.hpp"
#include "ipc/test/test_logger.hpp"
#include "ipc/common.hpp"
#include <flow/async/single_thread_task_loop.hpp>
#include <boost/thread/future.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <string>
#include <thread> // For std::thread: the deliberate species probe in Thread_exit_reaps_std_thread.
#include <vector>

namespace ipc::shm::arena_lend::detail::test
{

namespace
{

using arena_lend::test::create_test_pool_name_base;
using ipc::test::Test_logger;
using jemalloc::Ipc_arena;
using jemalloc::Memory_manager;
using flow::async::Single_thread_task_loop;
using flow::async::Synchronicity;
using boost::promise;
using std::atomic;
using std::make_shared;
using std::shared_ptr;
using std::vector;

/* In-SHM payload: carries a canary value; counts its destructions, so a test can assert exactly when the
 * owner-side reclamation machinery ran.  (Mirrors the eponymous struct in shm_session_test.cpp.)
 * Reminder: tests must reset s_dtor_ct before relying on it. */
struct Reclaim_probe
{
  // Total ~Reclaim_probe() invocations in this process.
  inline static atomic<unsigned int> s_dtor_ct{0};

  // The canary payload.
  int m_value;

  explicit Reclaim_probe(int value) : m_value(value) {}
  ~Reclaim_probe() { ++s_dtor_ct; }
}; // struct Reclaim_probe

/* Google test fixture: brackets the arena_lend process-wide logger around each test; zeroes the probe; runs
 * the two worker threads (see file-top comment re. why the main thread does no arena work itself).
 * Note tests must drop any arena handles from within the owner thread (see end_arenas()) before
 * returning; our dtor then stops the threads in the safe order (arena-toucher "oth" strictly before the
 * arena-owner "own", so no cross-thread tcache lingers to defer the arenas' actual destruction). */
class Thread_lcl_obj_db_test :
  public ::testing::Test
{
public:
  Thread_lcl_obj_db_test() :
    m_logger(flow::log::Sev::S_INFO),
    m_owner_loop(&m_logger, "tlodbOwn"),
    m_other_loop(&m_logger, "tlodbOth")
  {
    arena_lend::set_logger(&m_logger);
    m_owner_loop.start();
    m_other_loop.start();
    Reclaim_probe::s_dtor_ct = 0;
  }

  ~Thread_lcl_obj_db_test() override
  {
    m_other_loop.stop();
    m_owner_loop.stop();
    arena_lend::set_logger(nullptr);
  }

  // Runs task() in the owner (constructing) thread and returns once it has completed.
  template<typename Task>
  void own_wait(Task&& task)
  {
    m_owner_loop.post(std::forward<Task>(task), Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION);
  }

  // Runs task() in the other (non-constructing) thread and returns once it has completed.
  template<typename Task>
  void oth_wait(Task&& task)
  {
    m_other_loop.post(std::forward<Task>(task), Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION);
  }

  /* Creates a fresh arena (from within the owner thread) for test use.
   * pool_name_infix distinguishes this arena's SHM pool names (and only that). */
  shared_ptr<Ipc_arena> make_arena(const std::string& pool_name_infix)
  {
    shared_ptr<Ipc_arena> arena;
    own_wait([&]()
    {
      arena = Ipc_arena::create(nullptr, // Quiet by default; flip nullptr to &m_logger when debugging.
                                make_shared<Memory_manager>(),
                                create_test_pool_name_base(pool_name_infix),
                                util::shared_resource_permissions(util::Permissions_level::S_GROUP_ACCESS));
    });
    EXPECT_NE(arena, nullptr);
    return arena;
  }

  /* Constructs a Reclaim_probe in the given arena in the owner thread; then drops the sole handle in the
   * *other* thread: the use-count hits zero over there (client-side disposer), so the object becomes a
   * zombie in the owner thread's DB -- pending until the next piggy-scan there (asserted by callers). */
  void make_pending(const shared_ptr<Ipc_arena>& arena)
  {
    shared_ptr<Reclaim_probe> handle;
    own_wait([&]() { handle = arena->construct<Reclaim_probe>(42); });
    EXPECT_NE(handle, nullptr);
    oth_wait([&]() { handle.reset(); });
  }

  // Drops the given arena handle(s) from within the owner thread (see class doc comment).
  template<typename... Arena_handle>
  void end_arenas(Arena_handle&... arena)
  {
    own_wait([&]() { (..., arena.reset()); });
  }

  /* Polls (a few seconds max) until the probe's dtor-count reaches `expected`; returns whether it did.
   * (For observing reaps performed by threads we do not post-and-await on: e.g., the degraded-admin
   * drain-thread's periodic scan.) */
  static bool await_probe_count(unsigned int expected)
  {
    namespace this_thread = flow::util::this_thread;
    using boost::chrono::milliseconds;

    for (unsigned int attempt_idx = 0; attempt_idx != 200; ++attempt_idx)
    {
      if (Reclaim_probe::s_dtor_ct == expected)
      {
        return true;
      }
      this_thread::sleep_for(milliseconds(25));
    }
    return Reclaim_probe::s_dtor_ct == expected;
  }

protected:
  // Logger for the machinery under test (via arena_lend::set_logger()) and the test's own narration.
  Test_logger m_logger;
  // The constructing/owner thread: all construct()s (hence all reaping) happen here.
  Single_thread_task_loop m_owner_loop;
  // The other thread: cross-thread handle drops (zombie-makers) and wrong-thread negative checks.
  Single_thread_task_loop m_other_loop;
}; // class Thread_lcl_obj_db_test

} // namespace (anon)

/* Walk each no-session trigger op and assert it -- and only it -- reaps a pending (zombie) object; plus the
 * inline reap on an own-thread drop.  Sub-cases share the pattern: make an object pending via cross-thread
 * drop; assert it is still un-destroyed (the dropping thread must not reap it -- that is the
 * zombie-until-owner-scan property); fire exactly one trigger op in the owner thread; assert reaped. */
TEST_F(Thread_lcl_obj_db_test, Triggers_reap_pending)
{
  auto arena_a = make_arena("tlodbTriggersA");
  auto arena_b = make_arena("tlodbTriggersB");
  auto& ct = Reclaim_probe::s_dtor_ct;
  ct = 0;

  // Trigger: explicit gc.
  make_pending(arena_a);
  EXPECT_EQ(ct, 0u); // The cross-thread drop must not have reaped: zombie until owner-thread scan.
  own_wait([]() { Ipc_arena::this_thread_gc(); });
  EXPECT_EQ(ct, 1u);

  // Explicit gc with nothing pending: harmless (and per contract cheap) no-op.
  own_wait([]() { Ipc_arena::this_thread_gc(); Ipc_arena::this_thread_gc(); });
  EXPECT_EQ(ct, 1u);

  // Trigger: construct() on the *same* arena.  (Also: the subsequent own-thread drop reaps itself, inline.)
  make_pending(arena_a);
  EXPECT_EQ(ct, 1u);
  {
    shared_ptr<Reclaim_probe> handle;
    own_wait([&]() { handle = arena_a->construct<Reclaim_probe>(43); });
    EXPECT_EQ(ct, 2u); // The construct() reaped the zombie...
    EXPECT_EQ(handle->m_value, 43); // ...while the new object is alive and well...
    own_wait([&]() { handle.reset(); });
    EXPECT_EQ(ct, 3u); // ...until its own-thread drop, which reaps it right then, inline.
  }

  // Trigger: construct() on an *unrelated* arena.  (The DB spans all arenas of the type, by design.)
  make_pending(arena_a);
  EXPECT_EQ(ct, 3u);
  {
    shared_ptr<Reclaim_probe> handle;
    own_wait([&]() { handle = arena_b->construct<Reclaim_probe>(44); });
    EXPECT_EQ(ct, 4u); // arena_b activity reaped arena_a's zombie.
    own_wait([&]() { handle.reset(); });
    EXPECT_EQ(ct, 5u); // (Inline again.)
  }

  // Trigger: Ipc_arena::create() of a brand-new arena.
  make_pending(arena_a);
  EXPECT_EQ(ct, 5u);
  auto arena_c = make_arena("tlodbTriggersC");
  EXPECT_EQ(ct, 6u);

  /* Trigger: an own-thread handle drop's piggy-scan takes a zombie along with the inline reap of the dropped
   * object itself.  (Construct the to-be-dropped handle *first*: doing it after would itself reap the
   * zombie, per the sub-case above, spoiling the isolation.) */
  {
    shared_ptr<Reclaim_probe> handle;
    own_wait([&]() { handle = arena_b->construct<Reclaim_probe>(45); });
    EXPECT_EQ(ct, 6u); // (Nothing was pending; nothing reaped.)
    make_pending(arena_a);
    EXPECT_EQ(ct, 6u);
    own_wait([&]() { handle.reset(); });
    EXPECT_EQ(ct, 8u); // +2: the zombie (via the drop's piggy-scan) and the dropped object (inline).
  }

  end_arenas(arena_a, arena_b, arena_c);
} // TEST_F(Thread_lcl_obj_db_test, Triggers_reap_pending)

/* A held handle (use-count nonzero) must survive any number of scans; reap happens only once it is dropped
 * *and* -- the drop being cross-thread here -- a subsequent owner-thread scan runs.  (This is the unit-level
 * analog of the disconnect-flavored fact asserted at session level elsewhere: scans never destroy in-use
 * objects.) */
TEST_F(Thread_lcl_obj_db_test, Held_handle_defers_reap)
{
  auto arena = make_arena("tlodbHeld");
  auto& ct = Reclaim_probe::s_dtor_ct;
  ct = 0;

  shared_ptr<Reclaim_probe> handle;
  own_wait([&]() { handle = arena->construct<Reclaim_probe>(77); });
  own_wait([]() { Ipc_arena::this_thread_gc(); Ipc_arena::this_thread_gc(); });
  EXPECT_EQ(ct, 0u); // Scans with the handle held: no-ops w/r/t this object.
  EXPECT_EQ(handle->m_value, 77); // And it is fully usable.

  oth_wait([&]() { handle.reset(); });
  EXPECT_EQ(ct, 0u); // Cross-thread-dropped => zombie; still not destroyed.
  own_wait([]() { Ipc_arena::this_thread_gc(); });
  EXPECT_EQ(ct, 1u); // Now it is.

  end_arenas(arena);
} // TEST_F(Thread_lcl_obj_db_test, Held_handle_defers_reap)

/* Only the constructing thread's scans reap its zombies (the DB is per-thread by design -- lock-free
 * thread-local).  Another thread may trigger-scan all it wants -- even run a full construct/drop cycle of
 * its own on the same arena -- without touching ours. */
TEST_F(Thread_lcl_obj_db_test, Wrong_thread_reaps_nothing)
{
  auto arena = make_arena("tlodbWrongThread");
  auto& ct = Reclaim_probe::s_dtor_ct;
  ct = 0;

  make_pending(arena); // Owner thread's zombie awaits.
  EXPECT_EQ(ct, 0u);

  // Other thread scans explicitly: must not reap the owner thread's zombie.
  oth_wait([]() { Ipc_arena::this_thread_gc(); });
  EXPECT_EQ(ct, 0u);

  /* Other thread runs a full cycle of its own on the same arena: for *that* object the other thread is the
   * constructing thread, so its own-thread drop reaps it inline (+1) -- while the owner thread's zombie
   * stays untouched.  (Per-thread independence in both directions, on one shared arena.) */
  oth_wait([&]()
  {
    auto handle = arena->construct<Reclaim_probe>(88);
    handle.reset();
  });
  EXPECT_EQ(ct, 1u);

  own_wait([]() { Ipc_arena::this_thread_gc(); }); // The owner's own scan finally reaps its zombie.
  EXPECT_EQ(ct, 2u);

  end_arenas(arena);
} // TEST_F(Thread_lcl_obj_db_test, Wrong_thread_reaps_nothing)

/* Thread-exit scanning: A constructing thread that exits with zombies pending reaps them all during its
 * thread-local cleanup (exhaustive scan; no explicit trigger anywhere).  We use more zombies than the
 * lend-tracker pool's use-count-hint-array capacity and make *every* object -- the newest included -- a
 * zombie: the exhaustive scan's oldest-to-newest phase must then reap beyond the hinted ones, all the way
 * through the newest map element (a regression guard on that final-element edge). */
TEST_F(Thread_lcl_obj_db_test, Thread_exit_reaps)
{
  constexpr unsigned int N_OBJS = Lend_tracker_pool::S_N_UNUSED_IDX_HINTS + 4;

  auto arena = make_arena("tlodbExitReap");
  auto& ct = Reclaim_probe::s_dtor_ct;
  ct = 0;

  vector<shared_ptr<Reclaim_probe>> handles;
  {
    Single_thread_task_loop dying_loop{&m_logger, "tlodbDie"};
    dying_loop.start();
    dying_loop.post([&]()
    {
      for (unsigned int idx = 0; idx != N_OBJS; ++idx)
      {
        handles.emplace_back(arena->construct<Reclaim_probe>(int(idx)));
      }
    }, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION);

    oth_wait([&]() { handles.clear(); }); // All N_OBJS now zombies in the dying thread's DB.
    EXPECT_EQ(ct, 0u);

    dying_loop.stop(); // Thread exits here (join inside): its cleanup must reap everything.
  }
  EXPECT_EQ(ct, N_OBJS);

  end_arenas(arena);
} // TEST_F(Thread_lcl_obj_db_test, Thread_exit_reaps)

/* Same thing, but the constructing thread is a raw std::thread: Its thread-local cleanup runs at a
 * different point in the thread-exit sequence (after C++ `thread_local` deinit) than a
 * boost::thread-based loop's (before it) -- so this asserts the reap works on that path too. */
TEST_F(Thread_lcl_obj_db_test, Thread_exit_reaps_std_thread)
{
  constexpr unsigned int N_OBJS = 3;

  auto arena = make_arena("tlodbExitReapStd");
  auto& ct = Reclaim_probe::s_dtor_ct;
  ct = 0;

  vector<shared_ptr<Reclaim_probe>> handles;
  promise<void> constructed;
  promise<void> dropped;
  std::thread dying_thread{[&]()
  {
    for (unsigned int idx = 0; idx != N_OBJS; ++idx)
    {
      handles.emplace_back(arena->construct<Reclaim_probe>(int(idx)));
    }
    constructed.set_value();
    dropped.get_future().wait(); // Exit only once the zombies exist (cross-thread drops below).
  }};
  constructed.get_future().wait();

  oth_wait([&]() { handles.clear(); }); // All N_OBJS now zombies in dying_thread's DB.
  EXPECT_EQ(ct, 0u);

  dropped.set_value();
  dying_thread.join(); // Thread exits: its cleanup must reap everything.
  EXPECT_EQ(ct, N_OBJS);

  end_arenas(arena);
} // TEST_F(Thread_lcl_obj_db_test, Thread_exit_reaps_std_thread)

/* Degraded-admin scanning: A constructing thread exits while its objects are still *live* (handles held
 * elsewhere) -- so exit-time cleanup cannot reap; instead a drain-thread takes over the object-DB and
 * periodically scans.  Once the handles drop (cross-thread), it must reap within its poll period (~100ms;
 * we allow generously more). */
TEST_F(Thread_lcl_obj_db_test, Degraded_thread_drains)
{
  constexpr unsigned int N_OBJS = 2;

  auto arena = make_arena("tlodbDrain");
  auto& ct = Reclaim_probe::s_dtor_ct;
  ct = 0;

  vector<shared_ptr<Reclaim_probe>> handles;
  {
    Single_thread_task_loop dying_loop{&m_logger, "tlodbDie2"};
    dying_loop.start();
    dying_loop.post([&]()
    {
      for (unsigned int idx = 0; idx != N_OBJS; ++idx)
      {
        handles.emplace_back(arena->construct<Reclaim_probe>(int(idx)));
      }
    }, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION);

    dying_loop.stop(); // Thread exits with N_OBJS *live* objects: drain-thread spawns and takes over.
  }
  EXPECT_EQ(ct, 0u); // Live objects: neither the exit nor the drain-thread may reap them.

  oth_wait([&]() { handles.clear(); }); // Now they are zombies -- in the drain-thread's inherited DB.
  EXPECT_TRUE(await_probe_count(N_OBJS)); // Its periodic scan reaps them; it then exits on its own.
  EXPECT_EQ(ct, N_OBJS);

  end_arenas(arena);
} // TEST_F(Thread_lcl_obj_db_test, Degraded_thread_drains)

/* "The Gap" (see file-top comment and, for the full saga, the thread_end_gap_mutex() doc header): a shard of
 * stats briefly belongs to no walkable registry during its thread's exit; sharded_stats() consuming right
 * then -- absent the gap-mutex -- could return an aggregate missing that shard's addends (accumulators
 * regressing vs an earlier consume; high-watermark peaks lost).  So: hammer sharded_stats() from a dedicated
 * consumer thread, no pauses to speak of, while constructing threads rapid-cycle through both thread-exit
 * scenarios; record (never assert -- gtest stays on the main thread) any accumulator regression; then assert
 * zero regressions plus exact reconciliation at quiesce (see definition of *quiesce* at first mention higher-up).
 *
 * Note on division of labor: While here we test issues relevant to stats, the general value-level stats testing
 * is in ipc_arena_stats_test.cpp.  Here the subject is only aggregate *completeness under concurrency*, particularly
 * around thread exit.
 *
 * Notes on assertions:
 *
 * Every object-handle drop in this test is cross-thread, so *zero* destructions may take
 * the sync path (_admin::disposing_obj() => use-count=0); the live-exit objects are reaped by drain-threads (so
 * m_drain_destroy_count = exactly those), while the zombie-exit objects are reaped by the exiting threads'
 * dtor scans (counted with the piggy-backed reaper); and while the live-exit handles are all held, the
 * live-objects gauge sits at their total -- one deliberate consume right then (high-watermarks advance at
 * consumption time) makes the final _hi_wmark provably at least that (a lost peak would also indicate
 * Gap trouble). */
TEST_F(Thread_lcl_obj_db_test, Stats_consume_vs_thread_exit_gap)
{
  using flow::util::Thread;
  using stat::Sharded_stats;

  constexpr unsigned int N_CHURN_THREADS = 24; // Alternating zombie-exit / live-exit; so 12 of each.
  constexpr unsigned int N_OBJS_EACH = 3;
  constexpr unsigned int N_OBJS_LIVE_EXIT = (N_CHURN_THREADS / 2) * N_OBJS_EACH;
  constexpr unsigned int N_OBJS_TOTAL = N_CHURN_THREADS * N_OBJS_EACH;

  auto arena = make_arena("tlodbGap");
  auto& ct = Reclaim_probe::s_dtor_ct;
  ct = 0;

  // The consumer.  Loops until told to stop; records observations into `atomic`s; main thread asserts later.
  atomic<bool> consumer_stop{false};
  atomic<unsigned int> n_consumes{0};
  atomic<unsigned int> n_regressions{0};
  Single_thread_task_loop consumer_loop{&m_logger, "tlodbCons"};
  consumer_loop.start();
  consumer_loop.post([&]()
  {
    namespace this_thread = flow::util::this_thread;
    using boost::chrono::milliseconds;

    Sharded_stats stats; // (Reused across consumes; each sharded_stats() overwrites its contents.)
    uint64_t prev_destroys = 0;
    uint64_t prev_disposers = 0;
    while (!consumer_stop)
    {
      arena->sharded_stats(&stats);
      ++n_consumes;

      const uint64_t destroys = stats.m_owner_obj.m_destroy_count;
      const uint64_t disposers = stats.m_owner_obj.m_disposer_count;
      ((destroys < prev_destroys) || (disposers < prev_disposers))
        && (++n_regressions);
      prev_destroys = destroys;
      prev_disposers = disposers;

      this_thread::sleep_for(milliseconds(1));
    }
  });

  // The churn.  (Each constructing thread is raw: linear body; species irrelevant here; quieter than a loop.)
  vector<shared_ptr<Reclaim_probe>> held; // The live-exit threads' handles, outliving their threads.
  for (unsigned int thread_idx = 0; thread_idx != N_CHURN_THREADS; ++thread_idx)
  {
    const bool live_exit = (thread_idx % 2) != 0;

    vector<shared_ptr<Reclaim_probe>> handles;
    promise<void> constructed;
    promise<void> proceed;
    Thread churn_thread{[&]()
    {
      for (unsigned int idx = 0; idx != N_OBJS_EACH; ++idx)
      {
        handles.emplace_back(arena->construct<Reclaim_probe>(int(idx)));
      }
      constructed.set_value();
      proceed.get_future().wait();
    }};
    constructed.get_future().wait();

    if (live_exit)
    {
      // Keep them alive past the thread: it shall exit live => drain-thread takes over its DB.
      held.insert(held.end(), handles.begin(), handles.end());
      handles.clear();
    }
    else
    {
      oth_wait([&]() { handles.clear(); }); // Zombify: it shall exit with zombies => dtor-scan reap.
    }
    proceed.set_value();
    churn_thread.join();
  }
  // The zombie-exit threads' objects are reaped by now (at each respective join); the live ones await.
  EXPECT_EQ(ct, N_OBJS_TOTAL - N_OBJS_LIVE_EXIT);

  { // Pin the live-objects peak into the high-watermark: consume once while all N_OBJS_LIVE_EXIT are live.
    Sharded_stats stats;
    own_wait([&]() { arena->sharded_stats(&stats); });
    EXPECT_EQ(stats.m_live_obj.m_live_objects, N_OBJS_LIVE_EXIT);
  }

  oth_wait([&]() { held.clear(); }); // Now the (12) drain-threads reap their inherited objects...
  EXPECT_TRUE(await_probe_count(N_OBJS_TOTAL)); // ...within their poll periods; and then retire themselves.

  consumer_stop = true;
  consumer_loop.stop();

  // Check results.  First: the consumer must have seen no going-backwards accumulators, ever.
  EXPECT_GT(n_consumes, 0u);
  EXPECT_EQ(n_regressions, 0u);

  // And at quiesce (defined earlier) everything must reconcile exactly.
  Sharded_stats stats;
  own_wait([&]() { arena->sharded_stats(&stats); });
  EXPECT_EQ(stats.m_owner_obj.m_destroy_count, N_OBJS_TOTAL);
  EXPECT_EQ(stats.m_owner_obj.m_disposer_count, N_OBJS_TOTAL);
  EXPECT_EQ(stats.m_owner_obj.m_live_handle_groups, 0);
  EXPECT_EQ(stats.m_live_obj.m_live_objects, 0u);
  EXPECT_EQ(stats.m_live_obj.m_live_object_zombies, 0u);
  EXPECT_GE(stats.m_live_obj.m_live_objects_hi_wmark, N_OBJS_LIVE_EXIT);
  EXPECT_EQ(stats.m_owner_obj_arena_lend.m_sync_destroy_count, 0u); // No own-thread drops anywhere above.
  EXPECT_EQ(stats.m_owner_obj_arena_lend.m_drain_destroy_count, N_OBJS_LIVE_EXIT);

  end_arenas(arena);
} // TEST_F(Thread_lcl_obj_db_test, Stats_consume_vs_thread_exit_gap)

} // namespace ipc::shm::arena_lend::detail::test
