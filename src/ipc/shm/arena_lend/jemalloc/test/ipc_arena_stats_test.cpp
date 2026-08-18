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

#include "ipc/transport/struc/test/test_util.hpp"
#include "ipc/shm/arena_lend/jemalloc/ipc_arena.hpp"
#include "ipc/shm/arena_lend/jemalloc/memory_manager.hpp"
#include "ipc/shm/arena_lend/jemalloc/detail/jemalloc.hpp"
#include "ipc/shm/arena_lend/detail/stats.hpp"
#include <jemalloc/jemalloc.h>
#include "ipc/util/process_credentials.hpp"
#include "ipc/test/test_logger.hpp"
#include <flow/async/single_thread_task_loop.hpp>
#include <flow/log/log.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <iostream>
#include <memory>

/* Tests of the SHM-jemalloc (Ipc_arena/Shm_session) stats/info surface.  This is a bigger area than its
 * SHM-classic counterpart (see pool_arena_stats_test.cpp -- some DNA shared); it is covered in batches; for
 * example sharded_stats() basics are covered by this file's first test.
 *
 * Common scaffolding notes:
 *   - We use the pre-made, pre-lent Session::session_shm() arena (via test_util.hpp Session_pair), not a
 *     hand-made Ipc_arena; ditto (in later batches) Session::shm_session().  We ignore the app-scope arenas
 *     and the opposing side's mirror objects; and open no channels (n_init_channels=0) and send no messages --
 *     so nothing but this test itself touches the session-scope arena.  (A loud baseline assertion verifies
 *     that; if session internals ever change to construct SHM objects during setup, it will fail right there.)
 *   - All stat-driving mutation (construct/dispose) happens in spawned Single_thread_task_loop threads --
 *     never the main thread, which does all stat *consumption* (sharded_stats() etc.).  That is both the
 *     realistic monitoring shape and keeps the main thread free of thread-local SHMJ state (which otherwise
 *     lingers and can affect unrelated tests).
 *   - Single-threaded-in-sequence: tasks are posted with wait-for-completion; nothing races.  Concurrency of
 *     stat-consumption versus thread teardown (the "Gap") is intentionally out of scope here and covered by
 *     its dedicated test: Thread_lcl_obj_db_test.Stats_consume_vs_thread_exit_gap (thread_lcl_obj_db_test.cpp).
 *     So do not "improve" this test into a race test. */

namespace ipc::shm::arena_lend::jemalloc::test
{

namespace
{

using flow::async::Single_thread_task_loop;
using flow::async::Synchronicity;
using flow::log::Logger;
using transport::struc::test::Session_pair;
using session::schema::MqType;
using session::schema::ShmType;
using transport::struc::test::make_session_channel_pair;
using flow::util::ostream_op_string;
using std::shared_ptr;
using std::vector;
using std::string;
using Sess_pair = Session_pair<MqType::NONE, false, ShmType::JEMALLOC>;
using Arena = typename Sess_pair::Client_session_t::Arena;
using Sharded_stats = typename Arena::Sharded_stats;

/* Always-on console logger for test-progress output (FLOW_LOG_INFO etc.).
 * Survives across all TESTs in this TU; object internals use `g_logger` (toggleable) instead. */
ipc::test::Test_logger g_logger_obj;
Logger* const g_logger_console = &g_logger_obj;
#if 1
Logger* const g_logger = nullptr; // Normal: Flow-IPC objects silent.
#else
Logger* const g_logger = &g_logger_obj; // Flip for debugging.
#endif

// Posts `task` onto `loop` and waits for it to complete: our tasks run sequentially, just in various threads.
template<typename Task>
void post_wait(Single_thread_task_loop* loop, Task&& task)
{
  loop->post(std::forward<Task>(task), Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION);
}

} // namespace (anon)


/* Field-coverage manifests.  For each Stat_set type that this suite is responsible for testing:
 * stats_field_names() reflects the type's full declared field list, and every field must appear in exactly
 * one of the two lists below -- covered (asserted somewhere in this suite) or skipped (deliberately
 * unasserted, with its why).  A newly-added struct field fails this test until someone classifies it here:
 * that is the point.  (The covered-claims themselves are maintained by review; this enforces completeness
 * of the classification, not the existence of the asserts.) */
TEST(Ipc_arena_stats_test, field_coverage_manifests)
{
  using flow::util::stat::stats_field_names;
  using std::sort;
  using std::string;
  using std::vector;

  FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);

  const auto check_manifest = [](util::String_view stat_set_name, vector<string> declared,
                                 vector<string> covered, const vector<string>& skipped)
  {
    // The table itself (in declaration order), for the eyeball:
    const auto in = [](const vector<string>& vec, const string& name)
                      { return std::find(vec.begin(), vec.end(), name) != vec.end(); };
    std::cout << "Field-coverage manifest [" << stat_set_name << "] "
                 "(" << covered.size() << " covered, " << skipped.size() << " skipped):\n";
    for (const auto& name : declared)
    {
      std::cout << "  " << (in(skipped, name) ? "SKIPPED: "
                                              : in(covered, name) ? "covered: "
                                                                  : "*** UNCLASSIFIED: ")
                << name << '\n';
    }

    auto& classified = covered;
    classified.insert(classified.end(), skipped.begin(), skipped.end());
    sort(classified.begin(), classified.end());
    sort(declared.begin(), declared.end());
    EXPECT_EQ(declared, classified) << "Unclassified/stale stat-field classification for "
                                       "[" << stat_set_name << "].";
  };

  check_manifest
    ("Memory_manager_stats", stats_field_names<arena_lend::stat::Memory_manager_stats>(),
     { // Covered: asserted in this suite (mostly TEST memory_manager_stats).
       "uniq_arena_id.id1", "uniq_arena_id.id2", "native_arena_id",
       "vaddr.mapped_sz", "vaddr.mapped_sz_hi_wmark", "vaddr.resident_sz", "vaddr.resident_sz_hi_wmark",
       "vaddr.active_sz",
       "vaddr.retained_sz", "vaddr.retained_sz_hi_wmark", // (TEST retained_pool_space_decline_and_reuse.)
       "alloc.alloc_count", "alloc.dealloc_count", "alloc.alloc_sz", "alloc.dealloc_sz",
       "alloc.live_count", "alloc.live_count_hi_wmark", "alloc.live_sz", "alloc.live_sz_hi_wmark",
       "alloc.histo_alloc_count_by_sz" },
     { /* Skipped deliberately: jemalloc decay-policy/timing-dependent quantities -- their values depend on
        * background purging behavior, not on our machinery; printed for the eyeball, while the essential
        * gauges are covered via the mapped >= resident >= active chain (see TEST memory_manager_stats). */
       "vaddr.active_sz_hi_wmark",
       "vaddr.inactive_resident_sz", "vaddr.inactive_resident_sz_hi_wmark",
       "vaddr.inactive_muzzy_sz", "vaddr.inactive_muzzy_sz_hi_wmark" });

  check_manifest
    ("Borrower_pool_stats", stats_field_names<arena_lend::stat::Borrower_pool_stats>(),
     { // Covered: asserted in this suite (lend_borrow_stats + the lifecycle/re-borrow/concurrent tests).
       "uniq_arena_id.id1", "uniq_arena_id.id2",
       "arena_register_count", "arena_deregister_count",
       "arena_first_register_count", "arena_last_deregister_count",
       "n_borrowed_arenas", "n_borrowed_arenas_hi_wmark",
       "pool_register_count", "pool_deregister_count", "pool_open_count", "pool_close_count",
       "n_open_pools", "n_open_pools_hi_wmark", "mapped_sz", "mapped_sz_hi_wmark" },
     {}); // Nothing skipped: the borrower-side story is fully asserted.

  check_manifest
    ("Sharded_stats", stats_field_names<Arena::Sharded_stats>(),
     { // Covered: asserted in this suite (largely the sharding/zombie/drain/lend-borrow tests).
       "obj.own.n_shards", "obj.own.n_shards_hi_wmark", "obj.own.construct_count", "obj.own.disposer_count",
       "obj.own.live_handle_groups", "obj.own.live_handle_groups_hi_wmark",
       "obj.own.destroy_count", "obj.own.non_owner_destroy_count",
       "obj.own.sync_destroy_count", "obj.own.drain_destroy_count",
       "obj.lnd.lend_count",
       "obj.live_objects", "obj.live_objects_hi_wmark",
       "obj.live_object_zombies", "obj.live_object_zombies_hi_wmark",
       "obj.zombie.scans", "obj.zombie.exhaustive_scans", "obj.zombie.hints_sufficient_scans",
       "obj.zombie.fifo_scans", "obj.zombie.fifo_exhausted_scans", "obj.zombie.histo_zombie_ct_estimate",
       "obj.zombie.events.hints_checked", "obj.zombie.events.hints_stale",
       "obj.zombie.events.hints_found_resurrected_zombie",
       "obj.zombie.events.fifo_objs_checked", "obj.zombie.events.fifo_objs_live",
       "obj.zb.drn.scans", "obj.zb.drn.exhaustive_scans", "obj.zb.drn.hints_sufficient_scans",
       "obj.zb.drn.fifo_scans", "obj.zb.drn.fifo_exhausted_scans", "obj.zb.drn.histo_zombie_ct_estimate" },
     { /* Skipped deliberately:
        * The drain reaper's event-level detail rides the same machinery asserted via the main reaper's
        * events; the drain reaper's own scan counters (and histogram) are asserted above.
        * @todo We could still spot-check this. */
       "obj.zb.drn.events.hints_checked", "obj.zb.drn.events.hints_stale",
       "obj.zb.drn.events.hints_found_resurrected_zombie",
       "obj.zb.drn.events.fifo_objs_checked", "obj.zb.drn.events.fifo_objs_live" });

  check_manifest
    ("Pool_stats", stats_field_names<arena_lend::stat::Pool_stats>(),
     { // Covered: asserted in this suite (the pool-stats/live-info test, mostly).
       "owner.pool_create_count", "owner.pool_create_sz", "owner.pool_destroy_count", "owner.pool_destroy_sz",
       "owner.pool_optional_destroy_request_count",
       "dbaux.aux_pool_count", "dbaux.aux_pool_count_hi_wmark",
       "dbaux.use_ct_active_quanta", "dbaux.use_ct_active_quanta_hi_wmark",
       "dbaux.use_ct_quanta", "dbaux.use_ct_quanta_hi_wmark",
       "dbaux.resident_sz", "dbaux.resident_sz_hi_wmark", "dbaux.mapped_sz" },
     { // Skipped deliberately: the HI_WMARK mechanics are asserted via the resident-size twin.
       "dbaux.mapped_sz_hi_wmark" });

  check_manifest
    ("Obj_db_aux_pool_global_stats", stats_field_names<arena_lend::stat::Obj_db_aux_pool_global_stats>(),
     { // Covered: asserted in this suite (batch-5 borrow-side + the lifecycle test's reap phase).
       "client_tl_aux_pool_live_hndls", "client_tl_aux_pool_live_hndls_hi_wmark",
       "client_tl_aux_pool_hndl_open_count",
       "client_tl_aux_pool_hndl_reap_count", "client_tl_aux_pool_hndl_reap_check_count" },
     { /* Skipped -- here.  Delta-asserted in Shm_session_test instead (its Disconnected_external_process
        * increments it via a genuine owner-process-gone release; its Crash_external_process asserts
        * non-increment); provoking the underlying open-failure requires a session/process-death scenario
        * this arena-focused suite has no business setting up. */
       "client_tl_aux_pool_hndl_open_fail_count" });

  check_manifest
    ("Owner_pool_lookup_global_stats", stats_field_names<arena_lend::stat::Owner_pool_lookup_global_stats>(),
     { // Covered: asserted in this suite.
       "pool_rev_lookup_tl_cache_count", "pool_rev_lookup_tl_cache_count_hi_wmark" },
     {}); // Nothing skipped.

  check_manifest
    ("Borrower_pool_lookup_global_stats",
     stats_field_names<arena_lend::stat::Borrower_pool_lookup_global_stats>(),
     { // Covered: asserted in this suite.
       "pool_rev_lookup_tl_cache_count", "pool_rev_lookup_tl_cache_count_hi_wmark",
       "pool_fwd_lookup_tl_cache_count", "pool_fwd_lookup_tl_cache_count_hi_wmark" },
     {}); // Nothing skipped.

  check_manifest
    ("Shm_pool_info", stats_field_names<arena_lend::stat::Shm_pool_info>(),
     { // Covered: asserted in this suite (the live-info contract checks).
       "id", "sz", "uniq_arena_id.id1", "uniq_arena_id.id2", "use_count" },
     {}); // Nothing skipped.
} // TEST(Ipc_arena_stats_test, field_coverage_manifests)

/* Batch 1: sharded_stats() fundamentals.  The stat-set is reconstructed, at consumption time, from per-thread
 * shards living in Thread_lcl_obj_db_admin/client thread-local state -- plus Finalized_shards, which retains
 * dead threads' shards (a thread's contribution must not vanish just because the thread did).  Key contracts
 * asserted (see flow::util::stat::stats_aggregate_shards() doc header for the underlying semantics):
 *   - ACCUMULATORs/GAUGEs = sum across shards (a fresh consume overwrites the bundle's non-HWM values).
 *   - Consume is non-destructive => back-to-back consumes with no activity in-between are identical.
 *   - HI_WMARKs are *consume-sampled*: the max of the summed gauge as sampled across
 *     consume/sample_hi_wmarks() calls -- a peak *between* samplings is missed, by documented design.
 *     We assert both the captured and the missed case.
 *   - Reset: ACCUMULATORs zero (including inside the shards), GAUGEs persist (they reflect reality),
 *     HI_WMARKs re-seed to the current summed gauge.
 *   - Thread death: its shard's contributions persist (via Finalized_shards) in all subsequent consumes.
 * All disposals here happen in the object's constructing thread => the synchronous-destroy path throughout;
 * zombies/cross-thread disposal/drain paths are a later batch's subject. */
TEST(Ipc_arena_stats_test, sharded_stats_basics)
{
  FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);

  // Sessions only (no channels).  We exercise the client side's session-scope arena; all else is left alone.
  auto raw_pair = make_session_channel_pair<MqType::NONE, false, ShmType::JEMALLOC>(g_logger, 0);
  auto& sessions = *raw_pair.m_sessions;
  Arena* const arena = sessions.m_cli_session.session_shm();
  ASSERT_TRUE(arena);

  Sharded_stats out;
  const auto consume = [&]() { arena->sharded_stats(&out); };

  /* Baseline: untouched arena.  (Loud assumption-check: session setup itself constructs no objects here.)
   * Note m_n_shards == 0: a consume over zero shards.  (The struct's `= 1` default applies to a live shard --
   * each self-reports 1 -- not to an aggregate; the aggregation machinery supplies proper from-0-shards
   * values for this degenerate case.) */
  consume();
  EXPECT_EQ(out.m_owner_obj.m_construct_count.load(), 0u);
  EXPECT_EQ(out.m_owner_obj.m_disposer_count.load(), 0u);
  EXPECT_EQ(out.m_live_obj.m_live_objects.load(), 0u);
  EXPECT_EQ(out.m_live_obj.m_live_objects_hi_wmark.load(), 0u);
  EXPECT_EQ(out.m_live_obj.m_live_object_zombies.load(), 0u);
  EXPECT_EQ(out.m_owner_obj.m_n_shards, 0u); // See comment above.
  EXPECT_EQ(out.m_owner_obj.m_n_shards_hi_wmark, 0u); // Ditto.

  // Two mutation threads.  (Loop dtor implies .stop(); we stop explicitly when testing shard finalization.)
  Single_thread_task_loop loop1{g_logger, "shard1"};
  Single_thread_task_loop loop2{g_logger, "shard2"};
  loop1.start();
  loop2.start();

  /* Phase 1: loop1 constructs 2, disposes 1 (so its live-gauge briefly peaks at 2 -- unobserved!);
   * loop2 constructs 1, disposes it.  All disposals in the constructing thread. */
  shared_ptr<uint64_t> obj1; // Created and (later) disposed strictly within loop1's thread.
  post_wait(&loop1, [&]()
  {
    obj1 = arena->construct<uint64_t>(11);
    auto obj_fleeting = arena->construct<uint64_t>(12); // Live-gauge peak of 2 exists ~now...
    obj_fleeting.reset();
  }); // ...but no one sampled during that; relevant below.
  post_wait(&loop2, [&]()
  {
    auto obj = arena->construct<uint64_t>(21);
    obj.reset();
  });

  consume();
  EXPECT_EQ(out.m_owner_obj.m_construct_count.load(), 3u);
  EXPECT_EQ(out.m_owner_obj.m_disposer_count.load(), 2u);
  EXPECT_EQ(out.m_owner_obj.m_live_handle_groups.load(), 1);
  EXPECT_EQ(out.m_owner_obj_arena_lend.m_sync_destroy_count.load(), 2u); // Same-thread disposals: sync path.
  EXPECT_EQ(out.m_owner_obj.m_n_shards, 2u); // A shard per mutation thread (main has none: it only consumes).
  EXPECT_EQ(out.m_owner_obj.m_n_shards_hi_wmark, 2u); // (Sampled just now at 2.)
  EXPECT_EQ(out.m_live_obj.m_live_objects.load(), 1u);
  /* The consume-sampled missed-peak case: the true peak was 2 (inside phase 1), but no consume/sample happened at that
   * moment; the HWM is the max *sampled* summed-gauge, which is 1 (just now). */
  EXPECT_EQ(out.m_live_obj.m_live_objects_hi_wmark.load(), 1u);
  EXPECT_EQ(out.m_owner_obj.m_live_handle_groups_hi_wmark.load(), 1); // Ditto: same missed-peak trajectory.

  // Phase 2: drive the gauge to 3 and *sample at the peak* this time; then back down.
  shared_ptr<uint64_t> obj2, obj3;
  post_wait(&loop1, [&]()
  {
    obj2 = arena->construct<uint64_t>(13);
    obj3 = arena->construct<uint64_t>(14);
  });
  arena->sample_hi_wmarks(); // The whole point of this API: catch the peak without copying a bundle out.
  post_wait(&loop1, [&]()
  {
    obj2.reset();
    obj3.reset();
  });

  consume();
  EXPECT_EQ(out.m_owner_obj.m_construct_count.load(), 5u);
  EXPECT_EQ(out.m_owner_obj.m_disposer_count.load(), 4u);
  EXPECT_EQ(out.m_live_obj.m_live_objects.load(), 1u); // Just obj1.
  EXPECT_EQ(out.m_live_obj.m_live_objects_hi_wmark.load(), 3u); // The consume-sampled captured-peak case.

  // Consume is non-destructive: an immediate re-consume with no activity in-between is identical.
  Sharded_stats out2;
  arena->sharded_stats(&out2);
  EXPECT_EQ(out2.m_owner_obj.m_construct_count.load(), 5u);
  EXPECT_EQ(out2.m_owner_obj.m_disposer_count.load(), 4u);
  EXPECT_EQ(out2.m_live_obj.m_live_objects.load(), 1u);
  EXPECT_EQ(out2.m_live_obj.m_live_objects_hi_wmark.load(), 3u);

  /* Reset: ACCUMULATORs (including within the live shards) zero; GAUGEs persist (obj1 *is* still live;
   * the 2 shards *do* still exist); HI_WMARKs re-seed to the current summed gauge. */
  arena->sharded_stats_reset();
  consume();
  EXPECT_EQ(out.m_owner_obj.m_construct_count.load(), 0u);
  EXPECT_EQ(out.m_owner_obj.m_disposer_count.load(), 0u);
  EXPECT_EQ(out.m_owner_obj_arena_lend.m_sync_destroy_count.load(), 0u);
  EXPECT_EQ(out.m_live_obj.m_live_objects.load(), 1u);
  EXPECT_EQ(out.m_live_obj.m_live_objects_hi_wmark.load(), 1u); // Re-seeded: 3 is forgotten.
  EXPECT_EQ(out.m_owner_obj.m_n_shards, 2u);

  // Post-reset accumulation, from loop2, works from the new baseline.
  post_wait(&loop2, [&]()
  {
    auto obj = arena->construct<uint64_t>(22);
    obj.reset();
  });
  consume();
  EXPECT_EQ(out.m_owner_obj.m_construct_count.load(), 1u);
  EXPECT_EQ(out.m_owner_obj_arena_lend.m_sync_destroy_count.load(), 1u);

  /* Thread death => Finalized_shards: kill loop2's thread; its (post-reset) contributions must persist in
   * subsequent consumes -- including its m_n_shards=1 gauge contribution (the shard exists, finalized). */
  loop2.stop();
  consume();
  EXPECT_EQ(out.m_owner_obj.m_construct_count.load(), 1u);
  EXPECT_EQ(out.m_owner_obj_arena_lend.m_sync_destroy_count.load(), 1u);
  EXPECT_EQ(out.m_owner_obj.m_n_shards, 2u);
  EXPECT_EQ(out.m_live_obj.m_live_objects.load(), 1u);

  // Dispose obj1 (in its constructing thread), then kill loop1 too: everything settled, totals stable.
  post_wait(&loop1, [&]() { obj1.reset(); });
  loop1.stop();
  consume();
  EXPECT_EQ(out.m_owner_obj.m_disposer_count.load(), 2u); // loop2's post-reset disposal + obj1's just now.
  EXPECT_EQ(out.m_live_obj.m_live_objects.load(), 0u);
  EXPECT_EQ(out.m_owner_obj.m_n_shards, 2u); // Both shards live on, finalized.
  EXPECT_EQ(out.m_live_obj.m_live_object_zombies.load(), 0u); // Never any cross-thread disposal: no zombies ever.

  // And once more: idempotent even when all contributing threads are gone.
  arena->sharded_stats(&out2);
  EXPECT_EQ(out2.m_owner_obj.m_disposer_count.load(), 2u);
  EXPECT_EQ(out2.m_owner_obj.m_n_shards, 2u);

  FLOW_LOG_INFO("sharded_stats() after full sequence (for the eyeball): "
                "[" << flow::util::stat::print(out) << "].");
} // TEST(Ipc_arena_stats_test, sharded_stats_basics)

/* Batch 2a: zombies + the main (piggy-scan) reaper.  Background/the rules being asserted:
 *   - A *final* disposer (use-count -> 0) running in a thread other than the object's constructing thread
 *     cannot destroy the object (jemalloc arena/tcache state is thread-tied): it only decrements the in-SHM
 *     use-count, making the object a *zombie* -- destroyed later by a scan in the "right" (constructing) thread.
 *     Stats-wise the "wrong"-thread disposer bumps m_disposer_count only -- in the *disposing* thread's
 *     (lazily-created) client shard, which also joins the n_shards head-count.
 *   - m_live_objects counts zombies too (the decrement rides destruction, not disposal).
 *   - m_live_object_zombies is computed at consume-time from the in-SHM unused-count -- so a zombie's
 *     existence is directly assertable via sharded_stats().
 *   - Reap trigger: this_thread_piggy_scan() rides Ipc_arena API calls opportunistically -- so any arena op
 *     in the constructing thread (we use a construct) forces a scan; the scan finds the zombie via the
 *     hints array (m_hints_sufficient_scans outcome) and destroys it: m_destroy_count bumps, but not
 *     m_sync_destroy_count/m_drain_destroy_count (a plain main-mode reap).
 *   - Scan-outcome bookkeeping: only estimate-driven scans record an outcome or a histogram sample; a scan
 *     short-circuiting on estimate==0 records neither (but counts in m_scans); an *exhaustive* scan (thread
 *     exit) always lands in m_fifo_exhausted_scans -- by design m_fifo_exhausted_scans == m_exhaustive_scans,
 *     and only *exceeding* it is a canary.
 *   - m_non_owner_destroy_count is always 0 in SHM-jemalloc by design (destroys only ever run in owner admin
 *     threads). */
TEST(Ipc_arena_stats_test, zombie_and_main_reaper)
{
  FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);

  auto raw_pair = make_session_channel_pair<MqType::NONE, false, ShmType::JEMALLOC>(g_logger, 0);
  auto& sessions = *raw_pair.m_sessions;
  Arena* const arena = sessions.m_cli_session.session_shm();
  ASSERT_TRUE(arena);

  Sharded_stats out;
  const auto consume = [&]() { arena->sharded_stats(&out); };

  Single_thread_task_loop loop_ctor{g_logger, "zmb_ctor"}; // Constructing (owner-admin) thread.
  Single_thread_task_loop loop_disp{g_logger, "zmb_disp"}; // Wrong-thread disposer.
  loop_ctor.start();
  loop_disp.start();

  // Construct in loop_ctor; hold the handle main-side (touched only inside posted tasks).
  shared_ptr<uint64_t> obj;
  post_wait(&loop_ctor, [&]() { obj = arena->construct<uint64_t>(1); });
  consume();
  EXPECT_EQ(out.m_owner_obj.m_construct_count.load(), 1u);
  EXPECT_EQ(out.m_live_obj.m_live_objects.load(), 1u);
  EXPECT_EQ(out.m_live_obj.m_live_object_zombies.load(), 0u);
  EXPECT_EQ(out.m_owner_obj.m_n_shards, 1u); // Just the admin shard; loop_disp has not touched the arena.

  // Final-dispose from the "wrong" thread: a zombie is born (and remains among the living, per m_live_objects).
  post_wait(&loop_disp, [&]() { obj.reset(); });
  consume();
  EXPECT_EQ(out.m_owner_obj.m_disposer_count.load(), 1u); // Disposer ran (in loop_disp's new client shard)...
  EXPECT_EQ(out.m_owner_obj.m_destroy_count.load(), 0u); // ...but nothing was destroyed...
  EXPECT_EQ(out.m_live_obj.m_live_objects.load(), 1u); // ...so the object still counts as live...
  EXPECT_EQ(out.m_live_obj.m_live_object_zombies.load(), 1u); // ...as a zombie (via in-SHM unused-count).
  EXPECT_EQ(out.m_live_obj.m_live_object_zombies_hi_wmark.load(), 1u); // Consume-sampled at the peak just now.
  EXPECT_EQ(out.m_owner_obj.m_n_shards, 2u); // The client shard joined the head-count.

  /* Any arena op in the constructing thread piggy-triggers a scan; the hint emitted at disposal leads it
   * straight to our zombie.  (The op itself: a construct+dispose cycle, itself sync-destroyed.)
   * This exercises trigger #1, Ipc_arena::construct(); more trigger types below. */
  post_wait(&loop_ctor, [&]()
  {
    auto obj_fleeting = arena->construct<uint64_t>(2); // The piggy-scan on this call reaps the zombie.
    obj_fleeting.reset(); // Right-thread final dispose: the sync path.
  });
  consume();
  EXPECT_EQ(out.m_live_obj.m_live_object_zombies.load(), 0u); // Reaped.
  EXPECT_EQ(out.m_live_obj.m_live_objects.load(), 0u);
  EXPECT_EQ(out.m_owner_obj.m_construct_count.load(), 2u);
  EXPECT_EQ(out.m_owner_obj.m_disposer_count.load(), 2u);
  EXPECT_EQ(out.m_owner_obj.m_destroy_count.load(), 2u); // = 1 reap + 1 sync...
  EXPECT_EQ(out.m_owner_obj_arena_lend.m_sync_destroy_count.load(), 1u); // ...decomposed: the sync one...
  EXPECT_EQ(out.m_owner_obj_arena_lend.m_drain_destroy_count.load(), 0u); // ...no drain in this test...
  EXPECT_EQ(out.m_owner_obj.m_non_owner_destroy_count.load(), 0u); // ...and this is always 0 in SHM-jemalloc.
  { // The reaping scan's outcome bookkeeping (main reaper; no thread has exited yet):
    const auto& reaper = out.m_zombie_obj_reaper_main;
    EXPECT_EQ(reaper.m_hints_sufficient_scans.load(), 1u); // The reap: the hint led straight to the zombie.
    EXPECT_EQ(reaper.m_fifo_scans.load(), 0u);
    EXPECT_EQ(reaper.m_fifo_exhausted_scans.load(), 0u);
    EXPECT_EQ(reaper.m_exhaustive_scans.load(), 0u);
    EXPECT_GE(reaper.m_scans.load(), 2u); // Piggy scans ride several arena ops; most short-circuit on estimate==0.
    EXPECT_GE(reaper.m_events.m_hints_checked.load(), 1u);
    EXPECT_EQ(reaper.m_events.m_hints_found_resurrected_zombie.load(), 0u); // The canary's canary.
    /* m_events.m_hints_stale: deterministically staging a stale hint -- one that outlives its object's
     * deletion -- is ~impossible from a test; intentionally unexercised.  Like the resurrected-zombie
     * canary above, just assert the expected quiet. */
    EXPECT_EQ(reaper.m_events.m_hints_stale.load(), 0u);
    // The estimate histogram: exactly one sample so far -- the estimate (1) read by the reaping scan.
    EXPECT_EQ(reaper.m_histo_zombie_ct_estimate.count_for_bucket_containing_outcome(1), 1u);
  }

  /* Trigger diversity: this_thread_piggy_scan() rides ~every SHM-jemalloc lifecycle API; beyond construct()
   * (just exercised) we exercise Ipc_arena::create(), the Ipc_arena ref-count-0 destroy path, and the
   * object-disposer path -- each as the sole reap trigger for a fresh zombie, so that eliminating one of
   * those opportunistic calls fails here.  (Not exercised: the async forget-arena completion callback --
   * not deterministically reachable from a test; and the Shm_session lend/borrow triggers -- a later,
   * sessions-exercising batch's natural territory.)
   * The throwaway arena is created/destroyed in the owner thread; its own lifecycle does not touch
   * `*arena`'s stats (separate arena), but the piggy-scans it triggers walk *all* the thread's
   * arenas -- including ours. */
  std::shared_ptr<Ipc_arena> arena_fleeting;
  post_wait(&loop_ctor, [&]() { obj = arena->construct<uint64_t>(3); });
  post_wait(&loop_disp, [&]() { obj.reset(); }); // Zombie #2.
  consume();
  EXPECT_EQ(out.m_live_obj.m_live_object_zombies.load(), 1u);
  post_wait(&loop_ctor, [&]()
  {
    arena_fleeting
      = Ipc_arena::create(g_logger, std::make_shared<Memory_manager>(),
                          Shared_name::ct("ipcArenaStatsTestPiggyTrig"),
                          util::shared_resource_permissions(util::Permissions_level::S_GROUP_ACCESS));
  }); // Ipc_arena::create() piggy-triggered the reap:
  consume();
  EXPECT_EQ(out.m_live_obj.m_live_object_zombies.load(), 0u);
  EXPECT_EQ(out.m_zombie_obj_reaper_main.m_hints_sufficient_scans.load(), 2u);
  EXPECT_EQ(out.m_owner_obj.m_destroy_count.load(), 3u);

  post_wait(&loop_ctor, [&]() { obj = arena->construct<uint64_t>(4); });
  post_wait(&loop_disp, [&]() { obj.reset(); }); // Zombie #3.
  post_wait(&loop_ctor, [&]() { arena_fleeting.reset(); }); // Ipc_arena destroy path piggy-triggers the reap:
  consume();
  EXPECT_EQ(out.m_live_obj.m_live_object_zombies.load(), 0u);
  EXPECT_EQ(out.m_zombie_obj_reaper_main.m_hints_sufficient_scans.load(), 3u);
  EXPECT_EQ(out.m_owner_obj.m_destroy_count.load(), 4u);
  EXPECT_EQ(out.m_owner_obj.m_n_shards, 2u); // The throwaway arena's lifecycle added no shards to *arena*.

  // Disposer-path trigger: a bystander object, "right"-thread-disposed, reaps zombie #4 along the way.
  shared_ptr<uint64_t> obj_bystander;
  post_wait(&loop_ctor, [&]() { obj_bystander = arena->construct<uint64_t>(5); });
  post_wait(&loop_ctor, [&]() { obj = arena->construct<uint64_t>(6); });
  post_wait(&loop_disp, [&]() { obj.reset(); }); // Zombie #4.
  post_wait(&loop_ctor, [&]() { obj_bystander.reset(); }); // Disposer piggy-triggers the reap (+ its own sync kill):
  consume();
  EXPECT_EQ(out.m_live_obj.m_live_object_zombies.load(), 0u);
  EXPECT_EQ(out.m_zombie_obj_reaper_main.m_hints_sufficient_scans.load(), 4u);
  EXPECT_EQ(out.m_owner_obj.m_destroy_count.load(), 6u); // +1 reap, +1 the bystander's own sync-destroy...
  EXPECT_EQ(out.m_owner_obj_arena_lend.m_sync_destroy_count.load(), 2u); // ...to wit.

  /* Wind down: the admin thread's exit runs an exhaustive scan (which finds nothing -- all destroyed):
   * m_exhaustive_scans 0 -> 1 and, by design, m_fifo_exhausted_scans rides along.  (loop_disp has no
   * admin state; its exit adds no scans.) */
  loop_ctor.stop();
  loop_disp.stop();
  consume();
  EXPECT_EQ(out.m_zombie_obj_reaper_main.m_exhaustive_scans.load(), 1u);
  EXPECT_EQ(out.m_zombie_obj_reaper_main.m_fifo_exhausted_scans.load(), 1u);
  EXPECT_EQ(out.m_zombie_obj_reaper_main.m_hints_sufficient_scans.load(), 4u); // Unchanged.
  // One histogram sample per estimate-driven reap; the exhaustive scan added none (no estimate exists there).
  EXPECT_EQ(out.m_zombie_obj_reaper_main.m_histo_zombie_ct_estimate.count_for_bucket_containing_outcome(1), 4u);
  EXPECT_EQ(out.m_owner_obj.m_n_shards, 2u); // Both shards live on, finalized.

  FLOW_LOG_INFO("sharded_stats() after zombie/main-reaper sequence (for the eyeball): "
                "[" << flow::util::stat::print(out) << "].");
} // TEST(Ipc_arena_stats_test, zombie_and_main_reaper)

/* Batch 2b: the drain reaper.  When a thread exits while objects it constructed are still alive, its
 * thread-local-admin dtor spawns a *degraded-admin* (drain) thread, which periodically (100ms as of this
 * writing) scans-and-reaps until no live objects remain, then expires.  Rules asserted:
 *   - A zombie created *after* the constructing thread died is reaped by the drain thread's periodic scan:
 *     m_drain_destroy_count bumps (not m_sync_destroy_count), and the scan bookkeeping lands in
 *     m_zombie_obj_reaper_drain (not _main) -- the drain-attribution regression check.
 *   - The dying thread's own exhaustive scan (which sees the still-live object) lands in the *main* reaper:
 *     exhaustive_scans == fifo_exhausted_scans == 1, with the FIFO-walk events recording the live object.
 *   - The drain thread's own wind-down records no scans (by then every lend-tracker pool is gone).
 * Timing: the drain thread is asynchronous, so this test -- alone, deliberately -- polls with a generous
 * timeout instead of relying on strictly sequential posts. */
TEST(Ipc_arena_stats_test, drain_reaper)
{
  using flow::util::this_thread::sleep_for;
  using boost::chrono::milliseconds;

  FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);

  auto raw_pair = make_session_channel_pair<MqType::NONE, false, ShmType::JEMALLOC>(g_logger, 0);
  auto& sessions = *raw_pair.m_sessions;
  Arena* const arena = sessions.m_cli_session.session_shm();
  ASSERT_TRUE(arena);

  Sharded_stats out;
  const auto consume = [&]() { arena->sharded_stats(&out); };

  Single_thread_task_loop loop_ctor{g_logger, "drn_ctor"};
  Single_thread_task_loop loop_disp{g_logger, "drn_disp"};
  loop_ctor.start();
  loop_disp.start();

  // Construct; then kill the constructing thread while the object lives => drain thread takes over.
  shared_ptr<uint64_t> obj;
  post_wait(&loop_ctor, [&]() { obj = arena->construct<uint64_t>(1); });
  loop_ctor.stop();
  consume();
  EXPECT_EQ(out.m_live_obj.m_live_objects.load(), 1u); // Thread died; object (tracked by drain thread) lives.
  { // The dying thread's exhaustive scan saw one live (non-zombie) object; main-reaper attribution:
    const auto& reaper = out.m_zombie_obj_reaper_main;
    EXPECT_EQ(reaper.m_exhaustive_scans.load(), 1u);
    EXPECT_EQ(reaper.m_fifo_exhausted_scans.load(), 1u);
    EXPECT_GE(reaper.m_events.m_fifo_objs_checked.load(), 1u);
    EXPECT_GE(reaper.m_events.m_fifo_objs_live.load(), 1u);
  }

  // Final-dispose from another thread: zombie; only the drain thread can reap it now.
  post_wait(&loop_disp, [&]() { obj.reset(); });

  // Poll for the reap (typically 1-2 drain periods; be generous).  The deliberate exception to strict sequencing.
  for (int i = 0; i != 100; ++i)
  {
    consume();
    if ((out.m_live_obj.m_live_object_zombies.load() == 0) && (out.m_live_obj.m_live_objects.load() == 0))
    {
      break;
    }
    sleep_for(milliseconds{100});
  }
  EXPECT_EQ(out.m_live_obj.m_live_objects.load(), 0u);
  EXPECT_EQ(out.m_live_obj.m_live_object_zombies.load(), 0u);
  EXPECT_EQ(out.m_owner_obj.m_destroy_count.load(), 1u);
  EXPECT_EQ(out.m_owner_obj_arena_lend.m_drain_destroy_count.load(), 1u); // THE drain-attribution assert...
  EXPECT_EQ(out.m_owner_obj_arena_lend.m_sync_destroy_count.load(), 0u); // ...not sync...
  { // ...and the scan bookkeeping went to the drain reaper:
    const auto& reaper = out.m_zombie_obj_reaper_drain;
    EXPECT_GE(reaper.m_scans.load(), 1u);
    EXPECT_EQ(reaper.m_hints_sufficient_scans.load() + reaper.m_fifo_scans.load(), 1u); // The reaping scan.
    EXPECT_EQ(reaper.m_exhaustive_scans.load(), 0u); // Drain-loop scans are never exhaustive.
    // Its one estimate-driven scan recorded its estimate (1); the idle short-circuiting ticks record nothing.
    EXPECT_EQ(reaper.m_histo_zombie_ct_estimate.count_for_bucket_containing_outcome(1), 1u);
  }
  EXPECT_EQ(out.m_zombie_obj_reaper_main.m_hints_sufficient_scans.load(), 0u); // Main reaper never reaped a thing.

  loop_disp.stop();
  consume(); // Totals stable through both thread deaths (Finalized_shards).
  EXPECT_EQ(out.m_owner_obj.m_disposer_count.load(), 1u);
  EXPECT_EQ(out.m_owner_obj_arena_lend.m_drain_destroy_count.load(), 1u);
  EXPECT_EQ(out.m_owner_obj.m_n_shards, 2u);

  FLOW_LOG_INFO("sharded_stats() after drain-reaper sequence (for the eyeball): "
                "[" << flow::util::stat::print(out) << "].");
} // TEST(Ipc_arena_stats_test, drain_reaper)

/* Batch 3: pool_stats() + shm_pool_live_info() (+ their reset).  These count jemalloc-driven SHM-pool
 * (extent) lifecycle, so exact values are jemalloc's business -- we assert *relational invariants* and
 * force pool creation with a can't-fit-in-existing-extents allocation, rather than predicting counts:
 *   - Cross-surface invariant, absolute from arena birth (until a reset breaks it, deliberately):
 *     shm_pool_live_info().size() == m_pool_create_count - m_pool_destroy_count; ditto the sizes.
 *   - Policy: jemalloc's optional pool-destroy requests are *declined* (pools retained for reuse) --
 *     so m_pool_destroy_* stay 0 in practice until arena teardown, making m_pool_create_count effectively
 *     the live-pool count.  We assert that non-motion; if it ever fires, the policy changed -- revisit.
 *   - shm_pool_live_info() contract: sorted by pool ID; m_use_count is constant 1 on the owner side;
 *     m_uniq_arena_id matches the arena's.
 *   - The aux-pool stats (same Pool_stats bundle; maintained by the use-count registry) are
 *     *continuously* HWM-updated (unlike the sharded stats' consume-sampled HWMs): gauges rise while the
 *     constructing thread's lend-tracker aux-pool lives, HWMs ride along instantly, and everything
 *     returns to 0 when the thread dies (aux-pool freed) -- while the jemalloc pools live on. */
TEST(Ipc_arena_stats_test, pool_stats_and_live_info)
{
  FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);

  auto raw_pair = make_session_channel_pair<MqType::NONE, false, ShmType::JEMALLOC>(g_logger, 0);
  auto& sessions = *raw_pair.m_sessions;
  Arena* const arena = sessions.m_cli_session.session_shm();
  ASSERT_TRUE(arena);

  // The cross-surface invariants; call at will (before any reset).
  const auto check_pool_invariants = [&]()
  {
    const auto& stats = arena->pool_stats().m_owner_pool;
    const auto live = arena->shm_pool_live_info();
    // The unique-arena-ID halves: owner PID; arena/collection ID.
    const auto expected_id1 = uint64_t(util::Process_credentials::own_process_id());
    const auto expected_id2 = uint64_t(arena->get_id());
    EXPECT_EQ(live.size(), size_t(stats.m_pool_create_count.load() - stats.m_pool_destroy_count.load()));
    size_t total_sz = 0;
    for (size_t idx = 0; idx != live.size(); ++idx)
    {
      const auto& pool = live[idx];
      total_sz += pool.m_sz;
      EXPECT_GT(pool.m_sz, 0u);
      if (idx != 0)
      {
        EXPECT_GT(pool.m_id, live[idx - 1].m_id); // Strictly ascending: the sorted-by-ID contract.
      }
      EXPECT_EQ(pool.m_use_count, 1u); // Constant 1 on the owner side.
      EXPECT_EQ(pool.m_uniq_arena_id.m_id1, expected_id1);
      EXPECT_EQ(pool.m_uniq_arena_id.m_id2, expected_id2);
    }
    EXPECT_EQ(total_sz, size_t(stats.m_pool_create_sz.load() - stats.m_pool_destroy_sz.load()));
  };

  check_pool_invariants(); // Holds even pre-first-allocation.
  const auto destroy_count_at_start = arena->pool_stats().m_owner_pool.m_pool_destroy_count.load();

  Single_thread_task_loop loop1{g_logger, "pool1"};
  loop1.start();

  // First allocation ever: at least one pool must exist after.
  shared_ptr<uint64_t> obj_small;
  post_wait(&loop1, [&]() { obj_small = arena->construct<uint64_t>(1); });
  check_pool_invariants();
  EXPECT_GE(arena->pool_stats().m_owner_pool.m_pool_create_count.load(), 1u);
  const auto n_pools_before_big = arena->pool_stats().m_owner_pool.m_pool_create_count.load();

  // An allocation that cannot fit in any existing extent: forces 1+ new pools.
  using Big_blob = std::array<uint8_t, 8 * 1024 * 1024>;
  shared_ptr<Big_blob> obj_big;
  post_wait(&loop1, [&]() { obj_big = arena->construct<Big_blob>(); });
  check_pool_invariants();
  EXPECT_GT(arena->pool_stats().m_owner_pool.m_pool_create_count.load(), n_pools_before_big);
  { // Aux-pool stats: loop1's lend-tracker aux-pool is alive, with 2 tracked objects; continuous HWMs.
    const auto& aux = arena->pool_stats().m_obj_db_aux_pool;
    EXPECT_GE(aux.m_aux_pool_count.load(), 1u);
    EXPECT_GE(aux.m_aux_pool_count_hi_wmark.load(), aux.m_aux_pool_count.load());
    EXPECT_GE(aux.m_use_ct_active_quanta.load(), 1u);
    EXPECT_GE(aux.m_use_ct_active_quanta_hi_wmark.load(), aux.m_use_ct_active_quanta.load());
    EXPECT_GE(aux.m_use_ct_quanta.load(), 1u);
    EXPECT_GE(aux.m_use_ct_quanta_hi_wmark.load(), aux.m_use_ct_quanta.load());
    EXPECT_GT(aux.m_mapped_sz.load(), 0u);
    EXPECT_GT(aux.m_resident_sz.load(), 0u);
  }

  // Dispose everything (sync).  The pools are *retained* (optional-destroy requests declined by policy).
  post_wait(&loop1, [&]()
  {
    obj_small.reset();
    obj_big.reset();
  });
  check_pool_invariants();
  EXPECT_EQ(arena->pool_stats().m_owner_pool.m_pool_destroy_count.load(), destroy_count_at_start);
  EXPECT_GE(arena->pool_stats().m_owner_pool.m_pool_create_count.load(), n_pools_before_big); // Still there.

  /* Reset: the owner-pool ACCUMULATORs zero -- deliberately breaking the absolute cross-surface invariant
   * (live pools remain; the counters restart); aux-pool GAUGEs persist, and their HWMs re-seed. */
  const auto n_live_pools = arena->shm_pool_live_info().size();
  arena->pool_stats_reset();
  {
    const auto& stats = arena->pool_stats().m_owner_pool;
    EXPECT_EQ(stats.m_pool_create_count.load(), 0u);
    EXPECT_EQ(stats.m_pool_create_sz.load(), 0u);
    EXPECT_EQ(stats.m_pool_destroy_count.load(), 0u);
    EXPECT_EQ(stats.m_pool_optional_destroy_request_count.load(), 0u);
    EXPECT_EQ(arena->shm_pool_live_info().size(), n_live_pools); // Reality unmoved by the reset.
    const auto& aux = arena->pool_stats().m_obj_db_aux_pool;
    EXPECT_GE(aux.m_aux_pool_count.load(), 1u); // GAUGE: persists (loop1's aux-pool is still alive)...
    EXPECT_EQ(aux.m_aux_pool_count_hi_wmark.load(), aux.m_aux_pool_count.load()); // ...HWM re-seeded to it.
    EXPECT_EQ(aux.m_resident_sz_hi_wmark.load(), aux.m_resident_sz.load()); // Ditto.
  }

  // Thread death: the aux-pool dies with its thread (nothing live to drain); the jemalloc pools live on.
  loop1.stop();
  {
    const auto& aux = arena->pool_stats().m_obj_db_aux_pool;
    EXPECT_EQ(aux.m_aux_pool_count.load(), 0u);
    EXPECT_EQ(aux.m_use_ct_active_quanta.load(), 0u);
    EXPECT_EQ(aux.m_mapped_sz.load(), 0u);
    EXPECT_EQ(aux.m_resident_sz.load(), 0u);
    EXPECT_EQ(arena->shm_pool_live_info().size(), n_live_pools);
  }

  FLOW_LOG_INFO("pool_stats() after full sequence (for the eyeball): "
                "[" << flow::util::stat::print(arena->pool_stats()) << "]; "
                "live pools = [" << arena->shm_pool_live_info().size() << "].");
} // TEST(Ipc_arena_stats_test, pool_stats_and_live_info)

/* Batch 4: lending/borrowing -- the use-count>1 world -- via the pre-lent session arena + Session-level
 * lend_object()/borrow_object() (both Sessions live in this test process; only the CLI side's arena is
 * exercised, so the per-session borrower stats stay untangled).  The rules asserted:
 *   - Non-final disposers are stats-neutral.  Within one process there is no non-final disposer (local
 *     shared_ptr copies share one disposer); non-final events exist only via lend_object() (use-count +1) and
 *     the borrow-handle's disposer (-1).  The borrower-side disposer deliberately bumps *nothing*
 *     ("we [directly] track owner-side [stat] stuff only") -- so after a borrow-handle dispose that leaves use-count>0:
 *     zombies, live-count, destroy-count, even disposer-count -- all unmoved.
 *   - Only a *final* disposer (use-count -> 0) zombifies -- asserted from both directions:
 *     (a) borrow-dispose first, owner-dispose last => the owner dispose is final, right-thread (cting-thread)
 *         => sync kill;
 *     (b) owner-dispose first (wrong-thread, even!) => *no* zombie despite the cross-thread dispose;
 *         then the borrow-dispose brings it to 0 => a zombie born of a borrower disposer, reaped owner-side.
 *   - m_lend_count counts lends, attributed to the lending thread's shard: both types exercised --
 *     a right-thread lend (admin shard) and a wrong-thread lend (which also births that thread's client
 *     shard, observable via n_shards).
 *   - Borrower-side pool stats (Shm_session::borrower_pool_stats(), on the *borrowing* session): pools are
 *     lent eagerly as the owner arena creates them -- and, from the allocating side's viewpoint,
 *     synchronously: the extent-hook lend awaits the borrower's ack before the triggering allocation
 *     completes.  So these stats move on owner *allocation* activity (not on borrow_object()), and
 *     deterministically: the moment construct() returns, n_open_pools == the owner arena's live-pool
 *     count; continuous HWMs ride along; reset re-seeds them. */
TEST(Ipc_arena_stats_test, lend_borrow_stats)
{
  FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);

  auto raw_pair = make_session_channel_pair<MqType::NONE, false, ShmType::JEMALLOC>(g_logger, 0);
  auto& sessions = *raw_pair.m_sessions;
  Arena* const arena = sessions.m_cli_session.session_shm();
  ASSERT_TRUE(arena);
  const auto* const srv_shm_session = sessions.m_srv_session.shm_session(); // The borrowing side's session.
  ASSERT_TRUE(srv_shm_session);

  Sharded_stats out;
  const auto consume = [&]() { arena->sharded_stats(&out); };

  /* Note: "right" thread here means the construct()ing thread.  Wrong thread is any other thread... or
   * any thread off a borrow_object()ed handle (shared_ptr).  (Even if it is actually the constructing thread,
   * if it's off borrow_object()ed handle, then the system considers it the "wrong" thread.  It all still works;
   * it is not really inefficient -- the idea there is that in any real scenario borrow_object() would be done
   * in a different process; so there is no real point special-casing an optimization for when that's not true:
   * such as in these tests.) */

  Single_thread_task_loop loop_ctor{g_logger, "lnd_ctor"}; // Constructing/owner thread.
  Single_thread_task_loop loop_side{g_logger, "lnd_side"}; // Wrong-thread lender/disposer + borrower.
  loop_ctor.start();
  loop_side.start();

  // Sequence (a): construct; lend right-thread; borrow; borrow-dispose (non-final); owner-dispose (final).
  shared_ptr<uint64_t> obj, obj_borrowed;
  flow::util::Blob_sans_log_context blob;
  post_wait(&loop_ctor, [&]()
  {
    obj = arena->construct<uint64_t>(101);
    blob = sessions.m_cli_session.lend_object(obj); // Right-thread lend: admin-shard attribution.
  });
  EXPECT_FALSE(blob.empty());
  consume();
  EXPECT_EQ(out.m_lend_obj.m_lend_count.load(), 1u);
  EXPECT_EQ(out.m_owner_obj.m_n_shards, 1u); // Just the admin shard so far.

  /* Pool-lending is eager and -- from the allocating side's viewpoint -- synchronous: the extent-hook lend
   * awaits the borrower's ack before the triggering allocation completes.  So the construct() above has
   * fully propagated by now; the cross-side invariant holds deterministically, no polling required.
   * (If lending ever becomes fire-and-forget, this block is the loud alarm.) */
  {
    const auto& pool_stats = srv_shm_session->borrower_pool_stats();
    EXPECT_EQ(size_t(pool_stats.m_n_open_pools.load()), arena->shm_pool_live_info().size());
    EXPECT_GE(pool_stats.m_arena_register_count.load(), 1u);
    EXPECT_GE(pool_stats.m_pool_register_count.load(), 1u);
    EXPECT_GE(pool_stats.m_n_open_pools_hi_wmark.load(), pool_stats.m_n_open_pools.load()); // Continuous HWM.
    EXPECT_GT(pool_stats.m_mapped_sz.load(), 0u);
  }

  post_wait(&loop_side, [&]()
  {
    obj_borrowed = sessions.m_srv_session.borrow_object<uint64_t>(blob);
    ASSERT_TRUE(obj_borrowed);
    EXPECT_EQ(*obj_borrowed, 101u); // The data path itself: borrowed pointee is the constructed value.
    obj_borrowed.reset(); // Non-final dispose (use-count 2 -> 1), via the borrower disposer.
  });
  consume(); // Full stats-neutrality of the non-final borrower dispose:
  EXPECT_EQ(out.m_owner_obj.m_disposer_count.load(), 0u); // (Borrower disposer bumps nothing, by design.)
  EXPECT_EQ(out.m_owner_obj.m_destroy_count.load(), 0u);
  EXPECT_EQ(out.m_live_obj.m_live_objects.load(), 1u);
  EXPECT_EQ(out.m_live_obj.m_live_object_zombies.load(), 0u);
  EXPECT_EQ(out.m_owner_obj.m_n_shards, 1u); // Borrower-side ops birth no stats shard either.

  post_wait(&loop_ctor, [&]() { obj.reset(); }); // Final disposer, right-thread: the sync path.
  consume();
  EXPECT_EQ(out.m_owner_obj.m_disposer_count.load(), 1u);
  EXPECT_EQ(out.m_owner_obj.m_destroy_count.load(), 1u);
  EXPECT_EQ(out.m_owner_obj_arena_lend.m_sync_destroy_count.load(), 1u);
  EXPECT_EQ(out.m_live_obj.m_live_objects.load(), 0u);
  EXPECT_EQ(out.m_live_obj.m_live_object_zombies.load(), 0u); // Never a zombie in sequence (a).

  /* Sequence (b): construct; lend wrong-thread; borrow; owner-dispose FIRST (wrong-thread, non-final:
   * must NOT zombify); borrow-dispose (final: zombie via borrower disposer); reap owner-side. */
  post_wait(&loop_ctor, [&]() { obj = arena->construct<uint64_t>(202); });
  post_wait(&loop_side, [&]()
  {
    blob = sessions.m_cli_session.lend_object(obj); // Wrong-thread lend: client-type, client shard born.
    obj_borrowed = sessions.m_srv_session.borrow_object<uint64_t>(blob);
    ASSERT_TRUE(obj_borrowed);
  });
  consume();
  EXPECT_EQ(out.m_lend_obj.m_lend_count.load(), 2u); // Summed across admin + client shards.
  EXPECT_EQ(out.m_owner_obj.m_n_shards, 2u); // The wrong-thread lend birthed loop_side's client shard.

  post_wait(&loop_side, [&]() { obj.reset(); }); // Owner handle, wrong thread -- but NON-final (use-count 2->1):
  consume();
  EXPECT_EQ(out.m_live_obj.m_live_object_zombies.load(), 0u); // Only a FINAL disposer zombifies.
  EXPECT_EQ(out.m_live_obj.m_live_objects.load(), 1u);
  EXPECT_EQ(out.m_owner_obj.m_disposer_count.load(), 2u); // (The owner-handle disposer does count, as always.)
  EXPECT_EQ(out.m_owner_obj.m_destroy_count.load(), 1u);

  post_wait(&loop_side, [&]() { obj_borrowed.reset(); }); // Final (1 -> 0), via borrower disposer: zombie born.
  consume();
  EXPECT_EQ(out.m_live_obj.m_live_object_zombies.load(), 1u);
  EXPECT_EQ(out.m_live_obj.m_live_objects.load(), 1u); // Zombies are live until destroyed.
  EXPECT_EQ(out.m_owner_obj.m_disposer_count.load(), 2u); // Borrower disposer: still bumps nothing.

  post_wait(&loop_ctor, [&]() // Any owner-thread arena op reaps it (piggy-scan; cf. zombie_and_main_reaper).
  {
    auto obj_fleeting = arena->construct<uint64_t>(0);
    obj_fleeting.reset();
  });
  consume();
  EXPECT_EQ(out.m_live_obj.m_live_object_zombies.load(), 0u);
  EXPECT_EQ(out.m_live_obj.m_live_objects.load(), 0u);
  EXPECT_EQ(out.m_owner_obj.m_construct_count.load(), 3u);
  EXPECT_EQ(out.m_owner_obj.m_disposer_count.load(), 3u);
  EXPECT_EQ(out.m_owner_obj.m_destroy_count.load(), 3u); // = 2 sync (obj1, fleeting) + 1 reap (obj2)...
  EXPECT_EQ(out.m_owner_obj_arena_lend.m_sync_destroy_count.load(), 2u); // ...to wit.
  EXPECT_EQ(out.m_zombie_obj_reaper_main.m_hints_sufficient_scans.load(), 1u); // The hinted reap.

  // Borrower-side reset smoke: ACCUMULATORs zero; GAUGEs (open pools, mapped size) persist; HWMs re-seed.
  sessions.m_srv_session.shm_session()->borrower_pool_stats_reset();
  {
    const auto& pool_stats = srv_shm_session->borrower_pool_stats();
    EXPECT_EQ(pool_stats.m_pool_register_count.load(), 0u);
    EXPECT_EQ(pool_stats.m_arena_register_count.load(), 0u);
    EXPECT_GE(pool_stats.m_n_open_pools.load(), 1u);
    EXPECT_EQ(pool_stats.m_n_open_pools_hi_wmark.load(), pool_stats.m_n_open_pools.load()); // Re-seeded.
    EXPECT_EQ(pool_stats.m_mapped_sz_hi_wmark.load(), pool_stats.m_mapped_sz.load()); // Ditto.
  }

  loop_ctor.stop();
  loop_side.stop();
  consume();
  EXPECT_EQ(out.m_owner_obj.m_n_shards, 2u); // Both shards finalized; totals stable.
  EXPECT_EQ(out.m_lend_obj.m_lend_count.load(), 2u);
  EXPECT_EQ(out.m_owner_obj.m_disposer_count.load(), 3u); // Incl. the client shard's wrong-thread +1: finalized.
  /* The cross-thread-unbalanced GAUGE through finalization: the client shard holds a bare -1 (the wrong-thread
   * owner-handle dispose), balancing the admin shard's net +1; the zero is reachable only if the finalized fold
   * gauge-sums both. */
  EXPECT_EQ(out.m_owner_obj.m_live_handle_groups.load(), 0);

  FLOW_LOG_INFO("sharded_stats() after lend/borrow sequence (for the eyeball): "
                "[" << flow::util::stat::print(out) << "]; borrower_pool_stats (srv side, post-reset): "
                "[" << flow::util::stat::print(srv_shm_session->borrower_pool_stats()) << "].");
} // TEST(Ipc_arena_stats_test, lend_borrow_stats)

/* Batch 5: the info_dump() bundles (owner-side Arena_info_dump; borrower-side Shm_session_info_dump), the
 * process-wide globals, and the two global_stats_reset()s.  Approach:
 *   - Dump-equals-accessors: each bundle member must equal its source accessor's value at dump time
 *     (no activity in between; the sharded consume is idempotent, so back-to-back values match).
 *   - *Delta discipline* for the globals: they accumulate binary-wide (earlier tests, dead threads), so
 *     ACCUMULATORs are asserted as deltas from a baseline captured at test start; GAUGEs by lower bound.
 *   - Per-arena keying: the process-wide per-arena breakdown must contain an entry for *our* arena
 *     (matched via its Uniq_arena_id = {owner PID, collection ID}) whose values equal the borrowing
 *     session's own view -- the conflation-mitigation working as designed (the opposing direction's
 *     borrowing lands in a different arena's entry).
 *   - The jemalloc pieces of the owner dump (curated Memory_manager_stats + the stats_dump*() firehose
 *     string) must be present.  (There are some subtleties about the contents depending on jemalloc version
 *     and related jemalloc bugs/issues; but we don't get into it here and leave it to batch 6 which
 *     is about the jemalloc-originating stats.)
 *   - Print smoke-test: multiline and single-line (the latter force-suppresses the stats_dump*() firehose
 *     by contract). */
TEST(Ipc_arena_stats_test, info_dumps_and_globals)
{
  using Shm_session_t = std::decay_t<decltype(*std::declval<Sess_pair&>().m_srv_session.shm_session())>;

  FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);

  auto raw_pair = make_session_channel_pair<MqType::NONE, false, ShmType::JEMALLOC>(g_logger, 0);
  auto& sessions = *raw_pair.m_sessions;
  Arena* const arena = sessions.m_cli_session.session_shm();
  ASSERT_TRUE(arena);
  auto* const srv_shm_session = sessions.m_srv_session.shm_session();
  ASSERT_TRUE(srv_shm_session);

  // Baselines for the process-wide ACCUMULATORs (delta discipline: earlier tests have moved these).
  const auto aux_hndl_open_count_0
    = Arena::obj_db_aux_pool_global_stats().m_client_tl_aux_pool_hndl_open_count.load();

  Single_thread_task_loop loop_ctor{g_logger, "nfo_ctor"};
  Single_thread_task_loop loop_side{g_logger, "nfo_side"};
  loop_ctor.start();
  loop_side.start();

  // A compact activity arc populating every surface: construct, lend, borrow+read, borrow-dispose, kill.
  shared_ptr<uint64_t> obj, obj_borrowed;
  flow::util::Blob_sans_log_context blob;
  post_wait(&loop_ctor, [&]()
  {
    obj = arena->construct<uint64_t>(7);
    blob = sessions.m_cli_session.lend_object(obj);
  });
  post_wait(&loop_side, [&]()
  {
    obj_borrowed = sessions.m_srv_session.borrow_object<uint64_t>(blob);
    ASSERT_TRUE(obj_borrowed);
    EXPECT_EQ(*obj_borrowed, 7u);
    obj_borrowed.reset(); // Non-final; opens loop_side's client aux-pool handle (a global-stats mover).
  });
  post_wait(&loop_ctor, [&]() { obj.reset(); }); // Final, right-thread (construct()ing-thread): sync kill.

  // Owner-side dump: every member == its source accessor, sampled with no activity in between.
  Arena::Info_dump dump;
  arena->info_dump(&dump, util::Call_timing::S_ALWAYS_SAFE);
  EXPECT_EQ(dump.m_sharded_stats.m_owner_obj.m_construct_count.load(), 1u);
  EXPECT_EQ(dump.m_sharded_stats.m_owner_obj_arena_lend.m_sync_destroy_count.load(), 1u);
  EXPECT_EQ(dump.m_sharded_stats.m_lend_obj.m_lend_count.load(), 1u);
  EXPECT_EQ(dump.m_pool_stats.m_owner_pool.m_pool_create_count.load(),
            arena->pool_stats().m_owner_pool.m_pool_create_count.load());
  EXPECT_EQ(dump.m_shm_pool_live_info.size(), arena->shm_pool_live_info().size());
  EXPECT_GE(dump.m_shm_pool_live_info.size(), 1u);
  EXPECT_EQ(dump.m_owner_pool_lookup_global_stats.m_pool_rev_lookup_tl_cache_count.load(),
            Arena::owner_pool_lookup_global_stats().m_pool_rev_lookup_tl_cache_count.load());
  { // The global aux-pool stats: the borrow-dispose above opened a client aux-pool handle.
    const auto& aux_global = dump.m_obj_db_aux_pool_global_stats;
    EXPECT_GE(aux_global.m_client_tl_aux_pool_hndl_open_count.load(), aux_hndl_open_count_0 + 1); // Delta.
    EXPECT_GE(aux_global.m_client_tl_aux_pool_live_hndls.load(), 1u); // loop_side is alive, holding one.
    EXPECT_GE(aux_global.m_client_tl_aux_pool_live_hndls_hi_wmark.load(),
              aux_global.m_client_tl_aux_pool_live_hndls.load());
  }
  /* The jemalloc pieces.  They are present in a non-buggy jemalloc build regardless of Call_timing... but we
   * use ALWAYS_SAFE to avoid the issue entirely; anyway nothing evil (see info_dump() doc header) is going on
   * at the moment, so we're fine. */
  EXPECT_FALSE(dump.m_mem_mgr_stats_dump.empty());
  EXPECT_GE(dump.m_mem_mgr_stats.size(), 1u);

  // Borrower-side dump, ditto; plus the per-arena breakdown keyed by our arena's unique ID.
  Shm_session_t::Info_dump session_dump;
  srv_shm_session->info_dump(&session_dump, util::Call_timing::S_ALWAYS_SAFE);
  EXPECT_EQ(session_dump.m_borrower_pool_stats.m_pool_open_count.load(),
            srv_shm_session->borrower_pool_stats().m_pool_open_count.load());
  EXPECT_GE(session_dump.m_borrower_pool_stats.m_n_open_pools.load(), 1u);
  EXPECT_GE(session_dump.m_borrowed_shm_pool_live_info.size(), 1u);
  // Process-wide >= our session's contribution (the opposing direction's borrowing also lands in there).
  EXPECT_GE(session_dump.m_borrower_pool_stats_process_wide_total.m_pool_open_count.load(),
            session_dump.m_borrower_pool_stats.m_pool_open_count.load());
  {
    const auto expected_id1 = uint64_t(util::Process_credentials::own_process_id());
    const auto expected_id2 = uint64_t(arena->get_id());
    size_t n_found = 0;
    for (const auto& per_arena_stats : session_dump.m_borrower_pool_stats_process_wide_per_arena)
    {
      if ((per_arena_stats.m_uniq_arena_id.m_id1 == expected_id1)
          && (per_arena_stats.m_uniq_arena_id.m_id2 == expected_id2))
      {
        ++n_found;
        // Our arena is borrowed by exactly one session (srv); so its entry == that session's own view.
        EXPECT_EQ(per_arena_stats.m_n_open_pools.load(),
                  session_dump.m_borrower_pool_stats.m_n_open_pools.load());
      }
    }
    EXPECT_EQ(n_found, 1u); // Exactly one entry for our arena; opposing-direction borrowing is elsewhere.
  }

  // Print spot-check: multiline (verbose: includes the firehose) and single-line (firehose suppressed by contract).
  dump.m_fmt.m_multiline = true;
  FLOW_LOG_INFO("Owner-side info-dump, multiline (for the eyeball):\n" << dump);
  dump.m_fmt.m_multiline = false;
  FLOW_LOG_INFO("Same, single-line: [" << dump << "].");
  session_dump.m_fmt.m_multiline = true;
  FLOW_LOG_INFO("Borrower-side info-dump, multiline (for the eyeball):\n" << session_dump);
  session_dump.m_fmt.m_multiline = false;
  FLOW_LOG_INFO("Same, single-line: [" << session_dump << "].");

  /* The global resets (both static): ACCUMULATORs zero absolutely (delta discipline moot from here);
   * GAUGEs (live TL-cache entries, live aux handles) persist; their HWMs re-seed. */
  Arena::global_stats_reset();
  {
    const auto& aux_global = Arena::obj_db_aux_pool_global_stats();
    EXPECT_EQ(aux_global.m_client_tl_aux_pool_hndl_open_count.load(), 0u);
    EXPECT_EQ(aux_global.m_client_tl_aux_pool_hndl_reap_count.load(), 0u);
    EXPECT_GE(aux_global.m_client_tl_aux_pool_live_hndls.load(), 1u); // loop_side still holds its handle.
    EXPECT_EQ(aux_global.m_client_tl_aux_pool_live_hndls_hi_wmark.load(),
              aux_global.m_client_tl_aux_pool_live_hndls.load()); // Re-seeded.
    const auto& lookup_global = Arena::owner_pool_lookup_global_stats();
    EXPECT_EQ(lookup_global.m_pool_rev_lookup_tl_cache_count_hi_wmark.load(),
              lookup_global.m_pool_rev_lookup_tl_cache_count.load()); // Ditto.
  }
  {
    const auto& lookup_global = Shm_session_t::borrower_pool_lookup_global_stats();
    EXPECT_GE(lookup_global.m_pool_fwd_lookup_tl_cache_count.load(), 1u); // The borrow deref cached a mapping.
    EXPECT_GE(lookup_global.m_pool_fwd_lookup_tl_cache_count_hi_wmark.load(),
              lookup_global.m_pool_fwd_lookup_tl_cache_count.load());
  }
  Shm_session_t::global_stats_reset();
  {
    const auto& lookup_global = Shm_session_t::borrower_pool_lookup_global_stats();
    EXPECT_EQ(lookup_global.m_pool_rev_lookup_tl_cache_count_hi_wmark.load(),
              lookup_global.m_pool_rev_lookup_tl_cache_count.load()); // Re-seeded.
    EXPECT_EQ(lookup_global.m_pool_fwd_lookup_tl_cache_count_hi_wmark.load(),
              lookup_global.m_pool_fwd_lookup_tl_cache_count.load()); // Ditto.
  }
} // TEST(Ipc_arena_stats_test, info_dumps_and_globals)

/* Batch 6: memory_manager_stats() -- the jemalloc-sourced per-arena stats -- value assertions (these
 * outputs, while long since printing fine, have never been asserted-on before).  The ground rules, from
 * the jemalloc source (verified against 5.3.0, the min version modulo escape hatches; 5.2.1/5.3.1
 * identical in all ways relevant here):
 *   - SHM-jemalloc routes every allocation through a per-(thread, arena) tcache.  The alloc-family
 *     counters (m_alloc_count/_sz etc.) account at the slab boundary: a small-class allocation may pull a
 *     *batch* of regions into the cache (inflating alloc-count beyond the user's call count), and a free
 *     saves the region in the cache (deferring dealloc-count).  Thread exit drains the cache, truing
 *     everything up.
 *   - jemalloc does track true user-call counts (`nrequests`), but with explicit tcaches it files those
 *     tallies under the tcache's *associated* arena -- an internal binding that cannot be pointed at our
 *     arena (see Memory_manager_stats::Alloc doc header) -- so no request-count stat exists here.
 *   - Allocations above the max thread-cached size class (32KiB by default) bypass the cache entirely:
 *     for such classes every counter is exact and immediate -- 1 user call = 1 alloc-count = 1 request.
 * Hence the choreography: a large (cache-bypassing) phase asserts exact values warm; a small phase
 * asserts inflation-tolerant bounds warm, then exact trued-up values after the owner thread dies.
 * Both histogram buckets are addressed by *request size* = sizeof(T) (allocate() forwards it unmodified).
 * Also covered: Sigma(count-histo) == alloc_count; live = alloc - dealloc; the quiesced byte-tie;
 * vaddr gauge sanity (mapped >= resident >= active); row identity; consume idempotence; the
 * mark/since-reset-state machinery (its first asserted outing); and the S_POSSIBLY_UNSAFE-in-clean-build
 * dump gate.  (An UNSAFE-tier jemalloc build's suppression path is not exercisable from the normal
 * suite; and the no-tcache build's stronger identities live in the #ifdef block below.) */
TEST(Ipc_arena_stats_test, memory_manager_stats)
{
  FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);

  auto raw_pair = make_session_channel_pair<MqType::NONE, false, ShmType::JEMALLOC>(g_logger, 0);
  auto& sessions = *raw_pair.m_sessions;
  Arena* const arena = sessions.m_cli_session.session_shm();
  ASSERT_TRUE(arena);

  // Request sizes (= user sizes: allocate() forwards the user's size unmodified) for histogram buckets.
  using Big_blob = std::array<unsigned char, 64 * 1024>; // Request = 64KiB: above max-cached class.
  constexpr size_t BIG_REQUEST_SZ = sizeof(Big_blob);
  constexpr size_t SMALL_REQUEST_SZ = sizeof(uint64_t); // Request = 8B: the 8-byte small class.

  const auto histo_sum = [](const flow::util::stat::Histogram_counter& histo) -> uint64_t
  {
    uint64_t sum = 0;
    for (size_t idx = 0; idx != histo.size(); ++idx)
    {
      sum += histo.count_for_bucket(idx);
    }
    return sum;
  };
  const auto row = [&]() // The stats: a 1-vector today (1 jemalloc-arena per Ipc_arena).
  {
    auto rows = arena->memory_manager_stats();
    EXPECT_EQ(rows.size(), 1u);
    return std::move(rows.at(0));
  };

  // Baseline: row identity + all-quiet; and consume idempotence.
  auto stats = row();
  EXPECT_EQ(stats.m_uniq_arena_id.m_id1, uint64_t(util::Process_credentials::own_process_id()));
  EXPECT_EQ(stats.m_uniq_arena_id.m_id2, uint64_t(arena->get_id()));
  EXPECT_GE(stats.m_native_arena_id, 1u); // The jemalloc-arena-ID: some real index.
  EXPECT_EQ(stats.m_alloc.m_alloc_count, 0u);
  EXPECT_EQ(stats.m_alloc.m_live_count, 0u);
  EXPECT_EQ(histo_sum(stats.m_alloc.m_histo_alloc_count_by_sz), 0u);
  {
    const auto stats2 = row(); // No activity in between: identical.
    EXPECT_EQ(stats2.m_alloc.m_alloc_count, 0u);
    EXPECT_EQ(stats2.m_vaddr.m_mapped_sz, stats.m_vaddr.m_mapped_sz);
    EXPECT_EQ(stats2.m_vaddr.m_mapped_sz_hi_wmark, stats.m_vaddr.m_mapped_sz_hi_wmark);
  }

  Single_thread_task_loop loop_ctor{g_logger, "jstats1"};
  loop_ctor.start();

  /* Phase L: cache-bypassing allocations => exact-and-immediate everything, even with objects live
   * and the thread alive. */
  constexpr size_t N_BIG = 3;
  vector<shared_ptr<Big_blob>> big_objs;
  post_wait(&loop_ctor, [&]()
  {
    for (size_t idx = 0; idx != N_BIG; ++idx)
    {
      big_objs.emplace_back(arena->construct<Big_blob>());
    }
  });
  stats = row();
  EXPECT_EQ(stats.m_alloc.m_alloc_count, N_BIG);
  EXPECT_EQ(stats.m_alloc.m_dealloc_count, 0u);
  EXPECT_EQ(stats.m_alloc.m_live_count, N_BIG);
  EXPECT_EQ(stats.m_alloc.m_live_count_hi_wmark, N_BIG); // Consume-sampled at the peak just now.
  EXPECT_EQ(stats.m_alloc.m_alloc_sz, N_BIG * BIG_REQUEST_SZ); // Size-class-quantized == exact here.
  EXPECT_EQ(stats.m_alloc.m_live_sz, N_BIG * BIG_REQUEST_SZ);
  EXPECT_EQ(stats.m_alloc.m_live_sz_hi_wmark, N_BIG * BIG_REQUEST_SZ); // Consume-sampled at the peak, like count.
  EXPECT_EQ(stats.m_alloc.m_histo_alloc_count_by_sz.count_for_bucket_containing_outcome(BIG_REQUEST_SZ),
            N_BIG);
  EXPECT_EQ(histo_sum(stats.m_alloc.m_histo_alloc_count_by_sz), stats.m_alloc.m_alloc_count);
  // Vaddr gauge sanity: what is mapped includes what is physically backed includes what is actively used.
  EXPECT_GE(stats.m_vaddr.m_mapped_sz, stats.m_vaddr.m_resident_sz);
  EXPECT_GE(stats.m_vaddr.m_resident_sz, stats.m_vaddr.m_active_sz);
  EXPECT_GT(stats.m_vaddr.m_active_sz, 0u);

  post_wait(&loop_ctor, [&]() { big_objs.clear(); }); // Dispose all (sync); still cache-bypassing:
  stats = row();
  EXPECT_EQ(stats.m_alloc.m_dealloc_count, N_BIG);
  EXPECT_EQ(stats.m_alloc.m_dealloc_sz, N_BIG * BIG_REQUEST_SZ);
  EXPECT_EQ(stats.m_alloc.m_live_count, 0u);
  EXPECT_EQ(stats.m_alloc.m_live_sz, 0u);
  EXPECT_EQ(stats.m_alloc.m_live_count_hi_wmark, N_BIG); // The peak, retained.

  /* Phase S: small-class allocations => the cache in play.  Warm asserts are inflation-tolerant;
   * exact true-ups wait for the thread's death (cache drain). */
  constexpr size_t N_SMALL = 5;
  vector<shared_ptr<uint64_t>> small_objs;
  post_wait(&loop_ctor, [&]()
  {
    for (size_t idx = 0; idx != N_SMALL; ++idx)
    {
      small_objs.emplace_back(arena->construct<uint64_t>(uint64_t(idx)));
    }
  });
  stats = row();
  {
    const auto count_bucket
      = stats.m_alloc.m_histo_alloc_count_by_sz.count_for_bucket_containing_outcome(SMALL_REQUEST_SZ);
    EXPECT_GE(count_bucket, N_SMALL); // Batch-fill: at least our 5; likely far more.
#ifdef IPC_SHM_ARENA_LEND_JEMALLOC_NO_TCACHE
    EXPECT_EQ(count_bucket, N_SMALL); // No-tcache build: no batch fills; exactly our 5.
#endif
  }
  EXPECT_EQ(histo_sum(stats.m_alloc.m_histo_alloc_count_by_sz), stats.m_alloc.m_alloc_count); // Always.
  EXPECT_GE(stats.m_alloc.m_live_count, N_SMALL); // Live includes cached regions; >= what we hold.
  EXPECT_EQ(stats.m_alloc.m_live_count, stats.m_alloc.m_alloc_count - stats.m_alloc.m_dealloc_count);

  // Dispose + kill the thread: the cache drains; every counter trues up.
  post_wait(&loop_ctor, [&]() { small_objs.clear(); });
  loop_ctor.stop();
  stats = row();
  EXPECT_EQ(stats.m_alloc.m_dealloc_count, stats.m_alloc.m_alloc_count);
  EXPECT_EQ(stats.m_alloc.m_live_count, 0u);
  EXPECT_EQ(stats.m_alloc.m_live_sz, 0u);
  EXPECT_EQ(stats.m_alloc.m_alloc_sz, stats.m_alloc.m_dealloc_sz); // The byte-tie, quiesced form.
  EXPECT_EQ(histo_sum(stats.m_alloc.m_histo_alloc_count_by_sz), stats.m_alloc.m_alloc_count);

  /* Reset: the mark/since-reset-state machinery.  ACCUMULATORs (including histograms) restart from zero;
   * gauges persist; HWMs re-seed to current gauges. */
  arena->memory_manager_stats_reset();
  stats = row();
  EXPECT_EQ(stats.m_alloc.m_alloc_count, 0u);
  EXPECT_EQ(stats.m_alloc.m_dealloc_count, 0u);
  EXPECT_EQ(stats.m_alloc.m_alloc_sz, 0u);
  EXPECT_EQ(histo_sum(stats.m_alloc.m_histo_alloc_count_by_sz), 0u);
  EXPECT_EQ(stats.m_alloc.m_live_count_hi_wmark, stats.m_alloc.m_live_count); // Re-seeded (to 0 here).
  EXPECT_EQ(stats.m_vaddr.m_mapped_sz_hi_wmark, stats.m_vaddr.m_mapped_sz); // Ditto.

  // Post-reset activity counts from the new baseline (a fresh thread; the old one is gone).
  Single_thread_task_loop loop2{g_logger, "jstats2"};
  loop2.start();
  post_wait(&loop2, [&]()
  {
    auto obj_fleeting = arena->construct<Big_blob>();
    obj_fleeting.reset();
  });
  stats = row();
  EXPECT_EQ(stats.m_alloc.m_alloc_count, 1u); // Since-reset.
  EXPECT_EQ(stats.m_alloc.m_dealloc_count, 1u);
  EXPECT_EQ(stats.m_alloc.m_histo_alloc_count_by_sz.count_for_bucket_containing_outcome(BIG_REQUEST_SZ), 1u);
  loop2.stop();

  /* The dump gate: in a clean-jemalloc build (the only kind this suite runs under), even
   * S_POSSIBLY_UNSAFE consume-OKs; the UNSAFE-tier suppression path is compile-time dead here and
   * intentionally unexercised. */
  Arena::Info_dump dump;
  arena->info_dump(&dump, util::Call_timing::S_POSSIBLY_UNSAFE);
  EXPECT_GE(dump.m_mem_mgr_stats.size(), 1u);
  EXPECT_FALSE(dump.m_mem_mgr_stats_dump.empty());

  FLOW_LOG_INFO("memory_manager_stats() after full sequence (for the eyeball): "
                "[" << flow::util::stat::print(row()) << "].");
} // TEST(Ipc_arena_stats_test, memory_manager_stats)

/* The *retained* mechanism end-to-end: jemalloc's optional pool-space-removal request, our always-decline
 * policy, and the resulting still-mapped-but-not-"mapped" retention -- driven deterministically (forced purge,
 * rather than waiting out decay timers).  Background: see Ipc_arena::optional_remove_shm_pool() and the
 * `stats.*.retained` discussion in Memory_manager::create_arena().  In short: when jemalloc concludes a
 * wholly-unused extent (SHM-pool space) could be let go, it asks via the extent-dalloc hook; we decline; the
 * space moves, still mapped, from the `mapped` accounting to `retained` (physical pages released via
 * SHM-object hole-punching); suitable later allocations then reuse it from there -- no new pool, no
 * memory-map op.  The asserts tell that story in order:
 *   - the optional-destroy-request counter moves (jemalloc really asked);
 *   - retained goes 0 -> positive while mapped drops (an accounting move, not an unmapping);
 *   - the pool set is untouched throughout (create/destroy counts frozen: the decline held);
 *   - re-allocating drains retained back into mapped, the pool set *still* frozen (reuse, not new mapping).
 * (This also promotes the 2 vaddr.retained fields out of the eyeball-only tier; and settles empirically that
 * the decline policy *feeds* `retained` -- as opposed to keeping it 0, as one might guess.) */
TEST(Ipc_arena_stats_test, retained_pool_space_decline_and_reuse)
{
  FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);

  auto raw_pair = make_session_channel_pair<MqType::NONE, false, ShmType::JEMALLOC>(g_logger, 0);
  auto& sessions = *raw_pair.m_sessions;
  Arena* const arena = sessions.m_cli_session.session_shm();
  ASSERT_TRUE(arena);

  /* 1MiB apiece: far above any cached size class (frees reach the arena synchronously); and 8 of them, so
   * that -- whatever the extent geometry -- multi-MiB of contiguous extent space is wholly unused post-free. */
  using Big_blob = std::array<unsigned char, 1024 * 1024>;
  constexpr int N_BIG = 8;

  const auto row = [&]() // The stats: a 1-vector today (1 jemalloc-arena per Ipc_arena).
  {
    auto rows = arena->memory_manager_stats();
    EXPECT_EQ(rows.size(), 1u);
    return std::move(rows.at(0));
  };
  const auto pool_counts = [&]() -> std::pair<uint64_t, uint64_t>
  {
    const auto& pool_stats = arena->pool_stats().m_owner_pool;
    return { pool_stats.m_pool_create_count.load(), pool_stats.m_pool_destroy_count.load() };
  };
  const auto optional_destroy_requests = [&]() -> uint64_t
  {
    return arena->pool_stats().m_owner_pool.m_pool_optional_destroy_request_count.load();
  };

  Single_thread_task_loop loop_ctor{g_logger, "retained1"};
  loop_ctor.start();

  // Allocate big; then free it all.  The freed pages sit dirty -- still counted in `mapped`.
  vector<shared_ptr<Big_blob>> big_objs;
  post_wait(&loop_ctor, [&]()
  {
    for (int idx = 0; idx != N_BIG; ++idx)
    {
      big_objs.emplace_back(arena->construct<Big_blob>());
    }
  });
  auto stats = row();
  const auto native_arena_id = stats.m_native_arena_id;
  const auto mapped_at_peak = stats.m_vaddr.m_mapped_sz;
  EXPECT_GE(mapped_at_peak, size_t(N_BIG) * sizeof(Big_blob));
  const auto optional_destroy_requests_0 = optional_destroy_requests();
  const auto [create_count_0, destroy_count_0] = pool_counts();

  post_wait(&loop_ctor, [&]() { big_objs.clear(); });

  /* The trigger: `arena.<i>.purge` = forced full decay; wholly-unused extents go through the
   * extent-dalloc-hook gauntlet right now (as opposed to after decay-timer expiries). */
  {
    const auto ec = IPC_SHM_ARENA_LEND_JEMALLOC_API(mallctl)
                      (ostream_op_string("arena.", native_arena_id, ".purge").c_str(),
                       nullptr, nullptr, nullptr, 0);
    ASSERT_EQ(ec, 0);
  }

  stats = row();
  EXPECT_GT(optional_destroy_requests(), optional_destroy_requests_0); // jemalloc asked...
  EXPECT_GT(stats.m_vaddr.m_retained_sz, 0u); // ...we declined: the space is retained (and still mapped)...
  EXPECT_GE(stats.m_vaddr.m_retained_sz_hi_wmark, stats.m_vaddr.m_retained_sz);
  EXPECT_LT(stats.m_vaddr.m_mapped_sz, mapped_at_peak); // ...having left the `mapped` *accounting* only.
  {
    const auto [create_count, destroy_count] = pool_counts(); // The decline held: pool set untouched.
    EXPECT_EQ(create_count, create_count_0);
    EXPECT_EQ(destroy_count, destroy_count_0);
  }
  const auto retained_after_purge = stats.m_vaddr.m_retained_sz;

  // Reuse: allocate the same amount again; jemalloc takes the retained space back.
  post_wait(&loop_ctor, [&]()
  {
    for (int idx = 0; idx != N_BIG; ++idx)
    {
      big_objs.emplace_back(arena->construct<Big_blob>());
    }
  });
  stats = row();
  EXPECT_LT(stats.m_vaddr.m_retained_sz, retained_after_purge); // Drained back...
  {
    const auto [create_count, destroy_count] = pool_counts(); // ...with the pool set *still* untouched.
    EXPECT_EQ(create_count, create_count_0);
    EXPECT_EQ(destroy_count, destroy_count_0);
  }

  post_wait(&loop_ctor, [&]() { big_objs.clear(); });
  loop_ctor.stop();

  FLOW_LOG_INFO("Memory_manager_stats after retained-cycle (for the eyeball): "
                "[" << flow::util::stat::print(row()) << "].");
} // TEST(Ipc_arena_stats_test, retained_pool_space_decline_and_reuse)

/* Batch 7 (lifecycle): what perishes and what persists -- across arena death and session death, which in
 * SHM-jemalloc land arrive together: ending the Session destroys Shm_session and the session-scope Ipc_arena
 * (the latter only once every object handle is dropped too -- necessary but not sufficient, which we also
 * demonstrate).  Meanwhile the app-scope arena (Session_server-owned Server_session::app_shm()) survives, by
 * design (which isn't what we're testing -- but we use it to test the effect on stats).  The rules asserted:
 *   - Arena A's (cli-side session_shm()) death must not perturb still-alive arena B (srv-side app_shm()) in
 *     any way: B's entire sharded-stats printout is bit-identical across the kill.  (A's death fires the
 *     per-arena-eraser paths -- finalized-shard erasure, per-thread obj-db forgetting -- adjacent to B's data.)
 *   - Borrower-side (process-wide) per-arena stat entries are *historical*: A's and B's entries remain after
 *     both are deregistered, with their accumulators telling the full story (register == deregister == 1,
 *     first == last == 1, pools opened == closed, mapped-bytes high-water retained) while gauges read 0.
 *     The process-wide totals meanwhile: n_borrowed_arenas returns exactly to its pre-test value; the
 *     deregister-family accumulators advance by exactly our contribution.
 *   - The client-side per-thread obj-db aux-pool bookkeeping reaps A's now-dead tracker-pools lazily: on a
 *     thread's next first-contact-with-a-new-tracker-pool (and only then).  We provoke all interesting
 *     branches of that arithmetic: a thread holding 2 dead A-pools opens a B-pool (reap-check +2, reap +2,
 *     live-handles net -1), and a thread holding 1 does the same (reap-check +1, reap +1, live-handles
 *     net 0 -- the open and the reap cancel).
 * Choreography notes: the ctor threads (which allocated in A, hence hold (thread, A) caches) stop *before*
 * the kill, so A's teardown is not deferred to a later poll; the side threads never allocate in A (dispose/lend
 * only => no cache), so they survive the kill still holding A's dead client tracker-pools -- the reap
 * bait.  The kill itself replicates ~Session_pair dtor rendezvous dance (srv session offloaded to a
 * short-lived thread; cli+srv dtors must not be in same thread and in the real world are in diff processes)
 * while keeping the Session_server -- and thus arena B -- alive. */
TEST(Ipc_arena_stats_test, arena_and_session_lifecycle)
{
  using arena_lend::stat::Uniq_arena_id;
  using flow::util::stat::print;

  FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);

  using Shm_session_t = session::shm::arena_lend::jemalloc::Shm_session;

  /* Process-wide borrower-repository totals + per-arena breakdown.  Assert via deltas and open-vs-kill
   * symmetry, never absolutes: earlier tests in the suite contribute, and the session-open machinery itself
   * registers (and even deregisters) collections beyond the obvious per-side arenas.  We snapshot around
   * each phase instead of hard-coding that machinery's shape; the dumps make the breakdown eyeball-able. */
  struct Br_totals
  {
    uint64_t m_reg, m_first_reg, m_dereg, m_last_dereg, m_n_arenas, m_pool_close, m_n_pools;
  };
  const auto br_totals = [&]() -> Br_totals
  {
    const auto& t = Shm_session_t::borrower_pool_stats_process_wide();
    return { t.m_arena_register_count.load(), t.m_arena_first_register_count.load(),
             t.m_arena_deregister_count.load(), t.m_arena_last_deregister_count.load(),
             t.m_n_borrowed_arenas.load(), t.m_pool_close_count.load(), t.m_n_open_pools.load() };
  };
  // Find an arena's per-arena entry in the historical breakdown (entries linger post-deregister, by design).
  const auto br_per_arena_entry
    = [&](const Uniq_arena_id& id) -> shared_ptr<const Shm_session_t::Borrower_pool_stats>
  {
    Shm_session_t::Borrower_pool_stats_list list;
    Shm_session_t::borrower_pool_stats_process_wide(&list);
    for (auto& entry : list)
    {
      if ((entry->m_uniq_arena_id.m_id1 == id.m_id1) && (entry->m_uniq_arena_id.m_id2 == id.m_id2))
      {
        return shared_ptr<const Shm_session_t::Borrower_pool_stats>(std::move(entry));
      }
    }
    return {};
  };
  const auto br_dump = [&](util::String_view label)
  {
    Shm_session_t::Borrower_pool_stats_list list;
    Shm_session_t::borrower_pool_stats_process_wide(&list);
    for (const auto& entry : list)
    {
      FLOW_LOG_INFO("Borrower per-arena breakdown (" << label << "): arena "
                    "[" << entry->m_uniq_arena_id.m_id1 << '/' << entry->m_uniq_arena_id.m_id2 << "]: "
                    "[" << print(*entry) << "].");
    }
  };
  const auto b0 = br_totals(); // Before any session machinery of ours.

  auto raw_pair = make_session_channel_pair<MqType::NONE, false, ShmType::JEMALLOC>(g_logger, false, 0, 0);
  auto& sessions = *raw_pair.m_sessions;
  Arena* const arena_a = sessions.m_cli_session.session_shm(); // Dies with the session (once handles dropped).
  Arena* const arena_b = sessions.m_srv_session.app_shm(); // Session_server-owned: survives the session.
  ASSERT_TRUE(arena_a);
  ASSERT_TRUE(arena_b);

  const auto pid = uint64_t(util::Process_credentials::own_process_id());
  const auto a_id = Uniq_arena_id{ pid, arena_a->get_id() };
  const auto b_id = Uniq_arena_id{ pid, arena_b->get_id() };

  /* Arenas are lent *eagerly at session-open* (both session_shm()s, plus app_shm()): the borrowing is
   * already done right here -- our 3 arenas are the gauge delta. */
  const auto b1 = br_totals();
  br_dump("post-open");
  EXPECT_EQ(b1.m_n_arenas, b0.m_n_arenas + 3); // cli session arena (A), srv session arena, srv app arena (B).
  EXPECT_GE(b1.m_reg, b0.m_reg + 3);
  EXPECT_EQ(b1.m_reg - b0.m_reg, b1.m_first_reg - b0.m_first_reg); // Whatever registered, registered freshly.

  /* The cast: 2 ctor threads own A-objects (2 distinct tracker-pools); side thread X disposes/lends from *both*
   * (2 client-pools), side thread Z from one (1 client-pool); thread B owns B-objects.  The side threads
   * survive the kill. */
  Single_thread_task_loop loop_ctor_1{g_logger, "lcy_ctor1"};
  Single_thread_task_loop loop_ctor_2{g_logger, "lcy_ctor2"};
  Single_thread_task_loop loop_side_x{g_logger, "lcy_sideX"};
  Single_thread_task_loop loop_side_z{g_logger, "lcy_sideZ"};
  Single_thread_task_loop loop_b{g_logger, "lcy_b"};
  loop_ctor_1.start();
  loop_ctor_2.start();
  loop_side_x.start();
  loop_side_z.start();
  loop_b.start();

  // Arena-A activity: construct x3 (two ctor threads); lend A over the session once (registers A with BR).
  shared_ptr<uint64_t> obj_a1a, obj_a1b, obj_a2, obj_borrowed;
  flow::util::Blob_sans_log_context blob;
  post_wait(&loop_ctor_1, [&]()
  {
    obj_a1a = arena_a->construct<uint64_t>(11);
    obj_a1b = arena_a->construct<uint64_t>(12);
  });
  post_wait(&loop_ctor_2, [&]() { obj_a2 = arena_a->construct<uint64_t>(21); });
  post_wait(&loop_side_x, [&]()
  {
    blob = sessions.m_cli_session.lend_object(obj_a1a); // Wrong-thread lend: client-pool (ctor-1, A) born here.
  });
  post_wait(&loop_b, [&]()
  {
    obj_borrowed = sessions.m_srv_session.borrow_object<uint64_t>(blob);
    ASSERT_TRUE(obj_borrowed);
    EXPECT_EQ(*obj_borrowed, 11u);
    obj_borrowed.reset(); // Borrower-side (stats-neutral) dispose; owner handle remains.
  });

  // Arena-B activity: construct x3; lend B over the session once (registers B with BR).
  shared_ptr<uint64_t> obj_b1, obj_b2, obj_b3;
  post_wait(&loop_b, [&]()
  {
    obj_b1 = arena_b->construct<uint64_t>(1);
    obj_b2 = arena_b->construct<uint64_t>(2);
    obj_b3 = arena_b->construct<uint64_t>(3);
    blob = sessions.m_srv_session.lend_object(obj_b1);
  });
  obj_borrowed = sessions.m_cli_session.borrow_object<uint64_t>(blob);
  ASSERT_TRUE(obj_borrowed);
  EXPECT_EQ(*obj_borrowed, 1u);
  obj_borrowed.reset();

  // lend_object() registered nothing new: the arenas were pre-lent at open (sharp eagerness check).
  {
    const auto now = br_totals();
    EXPECT_EQ(now.m_reg, b1.m_reg);
    EXPECT_EQ(now.m_first_reg, b1.m_first_reg);
    EXPECT_EQ(now.m_n_arenas, b1.m_n_arenas);
    EXPECT_GE(now.m_n_pools, b0.m_n_pools + 3); // At least 1 pool open per (our) arena; likely more.
    const auto a_entry = br_per_arena_entry(a_id);
    ASSERT_TRUE(a_entry);
    EXPECT_EQ(a_entry->m_n_borrowed_arenas.load(), 1u);
    EXPECT_EQ(a_entry->m_arena_register_count.load(), 1u);
  }

  /* The remaining client-pool births: side thread X disposes ctor-2's object (client-pool (ctor-2, A) --
   * its 2nd); side thread Z disposes ctor-1's other object (client-pool (ctor-1, A) -- its 1st and only).
   * Both wrong-thread
   * final disposes (zombies in A; A's wholesale teardown handles them -- destruction with live objects is
   * legal, and these are not what we assert here). */
  post_wait(&loop_side_x, [&]() { obj_a2.reset(); });
  post_wait(&loop_side_z, [&]() { obj_a1b.reset(); });

  // Drop the last A handle (right-thread => clean sync kill); A now has *zero* user objects...
  post_wait(&loop_ctor_1, [&]() { obj_a1a.reset(); });

  // ...yet A is alive and well: handle-dropping is necessary but *not* sufficient; the session holds it.
  Sharded_stats out;
  arena_a->sharded_stats(&out);
  EXPECT_EQ(out.m_live_obj.m_live_objects.load(), 1u); // (Just ctor-2's un-reaped zombie, in fact.)
  EXPECT_EQ(out.m_owner_obj.m_construct_count.load(), 3u);

  // Pre-kill snapshot of B's entire stat world (as a canonical printout).
  arena_b->sharded_stats(&out);
  const string b_pre = ostream_op_string(print(out));

  /* Unpin A: the ctor threads (the only user threads that *allocated* in A => hold (thread, A) caches) exit,
   * so A's teardown inside the session kill below is not deferred.  (The side threads only disposed/lent -- no
   * cache created on that path, by design -- so they keep running, dead A client-pools in hand.) */
  loop_ctor_1.stop();
  loop_ctor_2.stop();

  /* The Kill: destroy both sessions -- but not the Session_server (so B lives).  Both Shm_sessions die
   * (deregistering all 3 arenas from the borrower repository); A's Ipc_arena dies with the cli session. */
  sessions.destroy_sessions();

  // The Crown: arena B's stats are bit-identical across arena A's (and both sessions') violent end.
  arena_b->sharded_stats(&out);
  EXPECT_EQ(ostream_op_string(print(out)), b_pre);

  { // Borrower-repository post-mortem: totals return/advance exactly; per-arena history is retained.
    const auto now = br_totals();
    EXPECT_EQ(now.m_n_arenas, b0.m_n_arenas); // Gauge: full round trip.
    EXPECT_EQ(now.m_dereg, b1.m_dereg + 3); // The kill deregistered our 3 still-borrowed arenas...
    EXPECT_EQ(now.m_last_dereg, b1.m_last_dereg + 3); // ...each a use-count 1->0.
    EXPECT_EQ(now.m_n_pools, b0.m_n_pools); // All our pools closed...
    EXPECT_GE(now.m_pool_close, b1.m_pool_close + 3); // ...which shows in the close-accumulator.

    for (const auto id : { a_id, b_id })
    {
      const auto entry = br_per_arena_entry(id);
      ASSERT_TRUE(entry); // Deregistered -- yet present: the breakdown is historical, by design.
      EXPECT_EQ(entry->m_arena_register_count.load(), 1u);
      EXPECT_EQ(entry->m_arena_deregister_count.load(), 1u);
      EXPECT_EQ(entry->m_arena_first_register_count.load(), 1u);
      EXPECT_EQ(entry->m_arena_last_deregister_count.load(), 1u);
      EXPECT_EQ(entry->m_n_borrowed_arenas.load(), 0u);
      EXPECT_EQ(entry->m_n_borrowed_arenas_hi_wmark.load(), 1u);
      EXPECT_EQ(entry->m_n_open_pools.load(), 0u);
      EXPECT_EQ(entry->m_pool_open_count.load(), entry->m_pool_close_count.load());
      EXPECT_GE(entry->m_pool_open_count.load(), 1u);
      EXPECT_EQ(entry->m_mapped_sz.load(), 0u);
      EXPECT_GT(entry->m_mapped_sz_hi_wmark.load(), 0u); // The high-water outlives the water.
    }
  }

  /* The Lazy Reap: each side thread's next first-contact-with-a-new-tracker-pool (a B one) scans that thread's
   * client-pool map and reaps the dead A entries.  Baseline captured post-kill, so deltas below are exactly
   * our doing.  Aux-global bookkeeping (see new_pool_data()):
   *   open-count +1 always; reap-check += (pre-add map size); reap += (dead count);
   *   live-handles net = 1 - reap-count (the two nontrivial branches of which we now hit in turn).
   * Piggy-backed onto the same first-contact: the arena-forget procedure (see forgetting_shm_arena()) --
   * each side thread drops its per-thread A-stats-shard record.  That cleanup is statless (its product is
   * mere absence of garbage); it is verified via the Finalized_shards check after the thread-stops below. */
  const auto& aux = Arena::obj_db_aux_pool_global_stats();
  struct Aux { uint64_t m_open, m_check, m_reap, m_live; };
  const auto aux_now = [&]() -> Aux
  {
    return { aux.m_client_tl_aux_pool_hndl_open_count.load(), aux.m_client_tl_aux_pool_hndl_reap_check_count.load(),
             aux.m_client_tl_aux_pool_hndl_reap_count.load(), aux.m_client_tl_aux_pool_live_hndls.load() };
  };
  const auto aux_base = aux_now();

  post_wait(&loop_side_x, [&]() { obj_b2.reset(); }); // Thread X: 2 dead A-pools in map; opens (thread-B, B) pool.
  {
    const auto now = aux_now();
    EXPECT_EQ(now.m_open, aux_base.m_open + 1);
    EXPECT_EQ(now.m_check, aux_base.m_check + 2);
    EXPECT_EQ(now.m_reap, aux_base.m_reap + 2);
    EXPECT_EQ(now.m_live, aux_base.m_live - 1); // Net: +1 open, -2 reaped.
  }

  post_wait(&loop_side_z, [&]() { obj_b3.reset(); }); // Thread Z: 1 dead A-pool; same B-pool first-contact.
  {
    const auto now = aux_now();
    EXPECT_EQ(now.m_open, aux_base.m_open + 2);
    EXPECT_EQ(now.m_check, aux_base.m_check + 3);
    EXPECT_EQ(now.m_reap, aux_base.m_reap + 3);
    EXPECT_EQ(now.m_live, aux_base.m_live - 1); // Net zero this time: the open and the reap cancel.
  }

  // B remains a fully functional arena after all of the above (and reaps its zombies fine).
  post_wait(&loop_b, [&]()
  {
    obj_b1.reset();
    auto obj_fleeting = arena_b->construct<uint64_t>(0);
    obj_fleeting.reset();
  });
  arena_b->sharded_stats(&out);
  EXPECT_EQ(out.m_owner_obj.m_construct_count.load(), 4u);

  loop_side_x.stop();
  loop_side_z.stop();
  loop_b.stop();

  /* The verify of the client-side arena-forget procedure (statless; see Lazy Reap comment above).  Careful
   * attribution: it was _admin's forgetting_shm_arena(), at The Kill, that erased A's Finalized_shards
   * bucket; what belongs to the *client* forget procedure is that X's and Z's per-thread A-stats-shard
   * records were dropped while those threads lived -- so their dtor-dumps into Finalized_shards, just now
   * performed as they exited, had no A-record left to re-create the bucket from.  A no-op'd client forget
   * (the sort of bug this guards against) would resurrect it: dead-arena stats accumulating in the
   * immortal singleton forever. */
  arena_lend::detail::stat::Finalized_shards<Arena>::get_instance().stats_while_locked
    (a_id.m_id2, [&](auto* fold_shard_or_null) { EXPECT_FALSE(fold_shard_or_null); });

  br_dump("final");
  FLOW_LOG_INFO("Post-lifecycle survivor-arena sharded_stats() (for the eyeball): "
                "[" << print(out) << "]; borrower totals: "
                "[" << print(Shm_session_t::borrower_pool_stats_process_wide()) << "].");
} // TEST(Ipc_arena_stats_test, arena_and_session_lifecycle)

/* Batch 8 (re-borrow): one arena, multiple session generations.  A session-scope arena by definition cannot
 * outlive its session; the app-scope arena (Server_session::app_shm()) is *designed* to: the Session_server
 * owns it, and each new session to the same Client_app lends that same arena anew.  So: open a session, use
 * the app arena, kill the sessions (Session_server lives), connect fresh sessions to the same server, use it
 * again.  The rules asserted:
 *   - The app arena survives and is *the same arena* across generations: same object, same unique ID; and it
 *     is functionally re-lent (an object constructed post-reconnect round-trips through lend/borrow).
 *   - Its (historical, accumulating) borrower-side per-arena entry spans the generations: register-count
 *     accumulates 1 -> 2 across the re-borrow, deregister-count likewise on each kill; pools re-open (the
 *     open-accumulator grows) and re-close.  Session-scope arenas, by contrast, do not recur: each
 *     generation's session_shm() is a fresh collection, the dead one's entry retained.
 *   - The first-register/last-deregister semantics, per their doc headers: they exclude only
 *     borrowed-while-*already*-borrowed events.  Sequential generations have none -- a full unborrow
 *     intervenes -- so first == register and last == deregister throughout, per-arena *and* in the global
 *     totals.  (Their non-degenerate case needs an arena borrowed by 2+ live sessions at once.)
 *   - The n_borrowed_arenas gauge round-trips per generation (1 -> 0 -> 1 -> 0) while its high-water stays 1. */
TEST(Ipc_arena_stats_test, arena_re_borrow)
{
  using arena_lend::stat::Uniq_arena_id;
  using flow::util::stat::print;

  FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);

  using Shm_session_t = session::shm::arena_lend::jemalloc::Shm_session;
  struct Br_totals { uint64_t m_reg, m_first_reg, m_dereg, m_last_dereg, m_n_arenas; };
  const auto br_totals = [&]() -> Br_totals
  {
    const auto& t = Shm_session_t::borrower_pool_stats_process_wide();
    return { t.m_arena_register_count.load(), t.m_arena_first_register_count.load(),
             t.m_arena_deregister_count.load(), t.m_arena_last_deregister_count.load(),
             t.m_n_borrowed_arenas.load() };
  };
  const auto br_per_arena_entry
    = [&](const Uniq_arena_id& id) -> shared_ptr<const Shm_session_t::Borrower_pool_stats>
  {
    Shm_session_t::Borrower_pool_stats_list list;
    Shm_session_t::borrower_pool_stats_process_wide(&list);
    for (auto& entry : list)
    {
      if ((entry->m_uniq_arena_id.m_id1 == id.m_id1) && (entry->m_uniq_arena_id.m_id2 == id.m_id2))
      {
        return shared_ptr<const Shm_session_t::Borrower_pool_stats>(std::move(entry));
      }
    }
    return {};
  };
  const auto b0 = br_totals();

  // Generation 1.
  auto raw_pair = make_session_channel_pair<MqType::NONE, false, ShmType::JEMALLOC>(g_logger, false, 0, 0);
  auto& sessions = *raw_pair.m_sessions;
  Arena* const arena_b = sessions.m_srv_session.app_shm(); // The central subject of this test.
  ASSERT_TRUE(arena_b);

  const auto pid = uint64_t(util::Process_credentials::own_process_id());
  const auto b_id = Uniq_arena_id{ pid, arena_b->get_id() };
  const auto a1_id = Uniq_arena_id{ pid, sessions.m_cli_session.session_shm()->get_id() };

  Single_thread_task_loop loop_own{g_logger, "rbw_own"}; // App-arena owner-side ops (survives generations).
  Single_thread_task_loop loop_bor{g_logger, "rbw_bor"}; // Borrower-side ops.
  loop_own.start();
  loop_bor.start();

  // Use the app arena: construct, lend srv -> cli, borrow, verify, drop handles.
  shared_ptr<uint64_t> obj, obj_borrowed;
  flow::util::Blob_sans_log_context blob;
  post_wait(&loop_own, [&]()
  {
    obj = arena_b->construct<uint64_t>(1001);
    blob = sessions.m_srv_session.lend_object(obj);
  });
  post_wait(&loop_bor, [&]()
  {
    obj_borrowed = sessions.m_cli_session.borrow_object<uint64_t>(blob);
    ASSERT_TRUE(obj_borrowed);
    EXPECT_EQ(*obj_borrowed, 1001u);
    obj_borrowed.reset();
  });
  post_wait(&loop_own, [&]() { obj.reset(); });

  { // Generation-1 state: borrowed once, freshly.
    const auto entry = br_per_arena_entry(b_id);
    ASSERT_TRUE(entry);
    EXPECT_EQ(entry->m_arena_register_count.load(), 1u);
    EXPECT_EQ(entry->m_arena_first_register_count.load(), 1u);
    EXPECT_EQ(entry->m_n_borrowed_arenas.load(), 1u);
    EXPECT_GE(entry->m_pool_open_count.load(), 1u);
  }

  // The unborrow: kill the sessions; the Session_server -- and with it the app arena -- lives.
  sessions.destroy_sessions();
  {
    const auto entry = br_per_arena_entry(b_id);
    ASSERT_TRUE(entry);
    EXPECT_EQ(entry->m_arena_deregister_count.load(), 1u);
    EXPECT_EQ(entry->m_arena_last_deregister_count.load(), 1u);
    EXPECT_EQ(entry->m_n_borrowed_arenas.load(), 0u);
    EXPECT_EQ(entry->m_n_open_pools.load(), 0u);
    EXPECT_EQ(entry->m_pool_open_count.load(), entry->m_pool_close_count.load());
  }

  // Generation 2: fresh sessions against the same still-listening Session_server.
  sessions.connect_sessions(g_logger);
  ASSERT_TRUE(sessions.m_srv_session.app_shm());
  EXPECT_EQ(sessions.m_srv_session.app_shm(), arena_b); // Same arena OBJECT, not merely a same-ID revival...
  EXPECT_EQ(uint64_t(arena_b->get_id()), b_id.m_id2); // ...(and the unique ID indeed did not budge).
  // Meanwhile the session-scope arenas do NOT recur: generation 2's is a fresh collection...
  const auto a2_id = Uniq_arena_id{ pid, sessions.m_cli_session.session_shm()->get_id() };
  EXPECT_NE(a2_id.m_id2, a1_id.m_id2);
  ASSERT_TRUE(br_per_arena_entry(a1_id)); // ...while the dead one's entry is retained (history).

  { // The re-borrow, in numbers: the entry ACCUMULATED across generations.
    const auto entry = br_per_arena_entry(b_id);
    ASSERT_TRUE(entry);
    EXPECT_EQ(entry->m_arena_register_count.load(), 2u);
    /* A sequential re-borrow is a fresh 0 -> 1 borrowing -- a full unborrow intervened -- so it counts as a
     * *first* register again, per that stat's doc header (it excludes only borrowed-while-already-borrowed). */
    EXPECT_EQ(entry->m_arena_first_register_count.load(), 2u);
    EXPECT_EQ(entry->m_n_borrowed_arenas.load(), 1u); // The gauge's second round trip, up-phase.
    EXPECT_EQ(entry->m_n_borrowed_arenas_hi_wmark.load(), 1u); // Never 2 borrowers at once, ever.
    EXPECT_GE(entry->m_pool_open_count.load(), 2u); // Pools re-opened: the accumulator grew.
    EXPECT_GE(entry->m_n_open_pools.load(), 1u);
  }

  // And functionally: the re-lent arena round-trips objects just like generation 1.
  post_wait(&loop_own, [&]()
  {
    obj = arena_b->construct<uint64_t>(2002);
    blob = sessions.m_srv_session.lend_object(obj);
  });
  post_wait(&loop_bor, [&]()
  {
    obj_borrowed = sessions.m_cli_session.borrow_object<uint64_t>(blob);
    ASSERT_TRUE(obj_borrowed);
    EXPECT_EQ(*obj_borrowed, 2002u);
    obj_borrowed.reset();
  });
  post_wait(&loop_own, [&]() { obj.reset(); });

  loop_own.stop();
  loop_bor.stop();

  // Kill generation 2 as well: the full two-generation ledger, at rest.
  sessions.destroy_sessions();
  {
    const auto entry = br_per_arena_entry(b_id);
    ASSERT_TRUE(entry);
    EXPECT_EQ(entry->m_arena_register_count.load(), 2u);
    EXPECT_EQ(entry->m_arena_deregister_count.load(), 2u);
    EXPECT_EQ(entry->m_arena_first_register_count.load(), 2u);
    EXPECT_EQ(entry->m_arena_last_deregister_count.load(), 2u);
    EXPECT_EQ(entry->m_n_borrowed_arenas.load(), 0u);
    EXPECT_EQ(entry->m_n_borrowed_arenas_hi_wmark.load(), 1u);
    EXPECT_EQ(entry->m_pool_open_count.load(), entry->m_pool_close_count.load());
    EXPECT_EQ(entry->m_mapped_sz.load(), 0u);
    EXPECT_GT(entry->m_mapped_sz_hi_wmark.load(), 0u);

    /* Global totals: with strictly sequential generations there is never a borrowed-while-borrowed event, so
     * the first/last variants track their parent counts exactly -- here too, not just per-arena. */
    const auto now = br_totals();
    EXPECT_EQ(now.m_reg - b0.m_reg, now.m_first_reg - b0.m_first_reg);
    EXPECT_EQ(now.m_dereg - b0.m_dereg, now.m_last_dereg - b0.m_last_dereg);
    EXPECT_EQ(now.m_n_arenas, b0.m_n_arenas); // Round trip x2.
    EXPECT_EQ(now.m_reg - b0.m_reg, 6u); // Generation 1: 3 arenas; generation 2: 2 fresh session arenas + B again.

    FLOW_LOG_INFO("Two-generation app-arena entry (for the eyeball): [" << print(*entry) << "].");
  }
} // TEST(Ipc_arena_stats_test, arena_re_borrow)

/* Batch 8b (concurrent borrow): the same app-scope arena borrowed via TWO live sessions at once -- the
 * session-rotation-with-overlap shape (a replacement session opens before the old one closes; same client
 * process, same apps, same Session_server).  This is the *only* scenario where the first-register /
 * last-deregister stats diverge from their parent counts (per their doc headers: they exclude
 * borrowed-while-already-borrowed / unborrowed-while-still-borrowed-elsewhere events -- and only overlap
 * produces such events).  It is also the only scenario exercising a *non-final* arena deregistration, i.e.
 * the borrower repository's use-count machinery doing nontrivial work.  Asserted per-arena (app arena B):
 *   - Overlap-open: register 2 yet first-register 1 (the second arrival was borrowed-while-borrowed);
 *     n_borrowed_arenas still 1 (an arena is either borrowed or it isn't -- gauge is not a use-count).
 *   - First kill: deregister 1 yet last-deregister 0 -- and, essentially: B's pools remain open and
 *     mapped, byte for byte, because the surviving session still uses them; B round-trips an object through
 *     the survivor to prove it.
 *   - Second kill: the ledger closes -- deregister 2, last-deregister 1, pools actually close now. */
TEST(Ipc_arena_stats_test, arena_concurrent_borrow)
{
  using arena_lend::stat::Uniq_arena_id;
  using flow::util::stat::print;

  FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);

  using Shm_session_t = session::shm::arena_lend::jemalloc::Shm_session;
  const auto br_per_arena_entry
    = [&](const Uniq_arena_id& id) -> shared_ptr<const Shm_session_t::Borrower_pool_stats>
  {
    Shm_session_t::Borrower_pool_stats_list list;
    Shm_session_t::borrower_pool_stats_process_wide(&list);
    for (auto& entry : list)
    {
      if ((entry->m_uniq_arena_id.m_id1 == id.m_id1) && (entry->m_uniq_arena_id.m_id2 == id.m_id2))
      {
        return shared_ptr<const Shm_session_t::Borrower_pool_stats>(std::move(entry));
      }
    }
    return {};
  };

  // Session pair #1; then pair #2 concurrently against the same Session_server, into our own slots.
  auto raw_pair = make_session_channel_pair<MqType::NONE, false, ShmType::JEMALLOC>(g_logger, false, 0, 0);
  auto& sessions = *raw_pair.m_sessions;
  Arena* const arena_b = sessions.m_srv_session.app_shm();
  ASSERT_TRUE(arena_b);
  const auto b_id = Uniq_arena_id{ uint64_t(util::Process_credentials::own_process_id()), arena_b->get_id() };

  typename Sess_pair::Client_session_t cli_2;
  typename Sess_pair::Server_session_t srv_2;
  sessions.connect_sessions(g_logger, &cli_2, &srv_2);
  EXPECT_EQ(srv_2.app_shm(), arena_b); // Both live sessions serve the one app arena.

  { // The overlap-open divergence:
    const auto entry = br_per_arena_entry(b_id);
    ASSERT_TRUE(entry);
    EXPECT_EQ(entry->m_arena_register_count.load(), 2u);
    EXPECT_EQ(entry->m_arena_first_register_count.load(), 1u); // Arrival #2 = borrowed-while-borrowed.
    EXPECT_EQ(entry->m_n_borrowed_arenas.load(), 1u); // Borrowed-or-not gauge, not a use-count.
    EXPECT_EQ(entry->m_n_borrowed_arenas_hi_wmark.load(), 1u);
  }

  // Round-trip an object through EACH session: both are fully live, on the same arena.
  Single_thread_task_loop loop_own{g_logger, "ccb_own"};
  Single_thread_task_loop loop_bor{g_logger, "ccb_bor"};
  loop_own.start();
  loop_bor.start();
  shared_ptr<uint64_t> obj, obj_borrowed;
  flow::util::Blob_sans_log_context blob;
  const auto round_trip = [&](auto* srv_session, auto* cli_session, uint64_t val)
  {
    post_wait(&loop_own, [&]()
    {
      obj = arena_b->construct<uint64_t>(val);
      blob = srv_session->lend_object(obj);
    });
    post_wait(&loop_bor, [&]()
    {
      obj_borrowed = cli_session->template borrow_object<uint64_t>(blob);
      ASSERT_TRUE(obj_borrowed);
      EXPECT_EQ(*obj_borrowed, val);
      obj_borrowed.reset();
    });
    post_wait(&loop_own, [&]() { obj.reset(); });
  };
  round_trip(&sessions.m_srv_session, &sessions.m_cli_session, 111);
  round_trip(&srv_2, &cli_2, 222);

  // Pool snapshot, then kill pair #1 only.
  uint64_t n_pools_before, mapped_before;
  {
    const auto entry = br_per_arena_entry(b_id);
    ASSERT_TRUE(entry);
    n_pools_before = entry->m_n_open_pools.load();
    mapped_before = entry->m_mapped_sz.load();
    EXPECT_GE(n_pools_before, 1u);
    EXPECT_GT(mapped_before, 0u);
  }
  sessions.destroy_sessions();

  { // The non-final deregistration: counted, but nothing closes -- session #2 still uses those pools.
    const auto entry = br_per_arena_entry(b_id);
    ASSERT_TRUE(entry);
    EXPECT_EQ(entry->m_arena_deregister_count.load(), 1u);
    EXPECT_EQ(entry->m_arena_last_deregister_count.load(), 0u); // Unborrowed-while-still-borrowed-elsewhere.
    EXPECT_EQ(entry->m_n_borrowed_arenas.load(), 1u);
    EXPECT_EQ(entry->m_n_open_pools.load(), n_pools_before); // The essential assert: still open...
    EXPECT_EQ(entry->m_mapped_sz.load(), mapped_before); // ...still mapped, byte for byte.
    EXPECT_EQ(entry->m_pool_close_count.load(), 0u); // Deregistered (by the dead session) != closed...
    EXPECT_GE(entry->m_pool_deregister_count.load(), 1u); // ...and deregistrations did happen, to be clear.
  }
  round_trip(&srv_2, &cli_2, 333); // And the arena works fine through the survivor.

  loop_own.stop();
  loop_bor.stop();

  // Kill pair #2: now the ledger closes for real.
  sessions.destroy_sessions(&cli_2, &srv_2);
  {
    const auto entry = br_per_arena_entry(b_id);
    ASSERT_TRUE(entry);
    EXPECT_EQ(entry->m_arena_register_count.load(), 2u);
    EXPECT_EQ(entry->m_arena_first_register_count.load(), 1u);
    EXPECT_EQ(entry->m_arena_deregister_count.load(), 2u);
    EXPECT_EQ(entry->m_arena_last_deregister_count.load(), 1u);
    EXPECT_EQ(entry->m_n_borrowed_arenas.load(), 0u);
    EXPECT_EQ(entry->m_n_open_pools.load(), 0u);
    EXPECT_EQ(entry->m_pool_open_count.load(), entry->m_pool_close_count.load());
    EXPECT_EQ(entry->m_mapped_sz.load(), 0u);
    EXPECT_GE(entry->m_mapped_sz_hi_wmark.load(), mapped_before); // (GE: post-kill traffic may add a pool.)

    FLOW_LOG_INFO("Concurrently-borrowed app-arena entry, at rest (for the eyeball): [" << print(*entry) << "].");
  }
} // TEST(Ipc_arena_stats_test, arena_concurrent_borrow)


} // namespace ipc::shm::arena_lend::jemalloc::test
