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

#include "ipc/shm/arena_lend/detail/lend_tracker_pool.hpp"
#include "ipc/shm/arena_lend/detail/use_count_registry.hpp"
#include "ipc/shm/classic/pool_arena.hpp"
#include "ipc/common.hpp"
#include "ipc/test/test_logger.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <optional>
#include <flow/async/single_thread_task_loop.hpp>
#include <flow/util/util.hpp>
#include <flow/log/buffer_logger.hpp>
#include <flow/log/log.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <memory>
#include <vector>
#ifdef FLOW_OS_LINUX
#  include <sys/mman.h>
#endif

namespace ipc::shm::arena_lend::detail::test
{

namespace
{

static const auto S_TEST_POOL_NAME = util::Shared_name::ct("lendTrackerTestPool");
using ipc::test::Test_logger;
using flow::util::ceil_div;
using flow::util::round_to_multiple;
using flow::log::Buffer_logger;
using flow::log::Config;
using flow::log::Sev;
using flow::log::Log_context_mt;
using boost::system::system_category;
using std::atomic;
using std::make_unique;
using std::optional;
using std::unique_ptr;
using std::vector;
using Synchronicity = flow::async::Synchronicity;
using Thread_loop = flow::async::Single_thread_task_loop;

}

/* Basic single-threaded test that is written in an "explaratory" fashion: Set up an admin and a couple clients
 * and check the behavior of the search-and-allocate algorithm + inc/dec/return + things opportunistically
 * conceptually adjacent to those.  It is not exhaustive in the sense of methodically hitting a matrix of things
 * but makes its way through various non-trivial parts of the Lend_tracker_pool + Use_count_registry (used inside
 * the former to allocate/return use-count slots) lifecycle.  The logging isn't verified, per se (other than
 * the fact it doesn't crash/etc.); but the standard Logger is supplied, so that a human can see how things
 * progress -- for learning/understanding and such).
 *
 * Further tests hit some of the specific subtleties to gain more thorough coverage. */
TEST(Lend_tracker_pool_test, Interface)
{
  // Pre-clean (in case of preceding crash or some-such).
  Error_code ec;
  ipc::shm::classic::Pool_arena::remove_persistent(nullptr, S_TEST_POOL_NAME, &ec);

  Test_logger logger;
  FLOW_LOG_SET_CONTEXT(&logger, Log_component::S_UNCAT);
  const Log_context_mt log_ctx{&logger, Log_component::S_UNCAT};
  const atomic<bool> skip_trace{false};

  Lend_tracker_pool tracker_admin{&log_ctx, &skip_trace, S_TEST_POOL_NAME, util::CREATE_ONLY, nullptr};
  Lend_tracker_pool tracker_cli1{&log_ctx, &skip_trace, S_TEST_POOL_NAME, util::OPEN_ONLY};
  EXPECT_EQ(tracker_admin.n_unused(), 0u);
  auto idx1 = tracker_admin.use_count_new();
  auto idx2 = tracker_admin.use_count_new();
  auto idx3 = tracker_admin.use_count_new();
  EXPECT_EQ(tracker_admin.n_unused(), 0u);
  EXPECT_GE(idx1, sizeof(uint64_t)); // Internally, metadata header thing will use up 1 word (64-bits) at least.
  EXPECT_EQ(idx2, idx1 + 1);
  EXPECT_EQ(idx3, idx2 + 1);

  Lend_tracker_pool tracker_cli2{&log_ctx, &skip_trace, S_TEST_POOL_NAME, util::OPEN_ONLY};
  EXPECT_EQ(tracker_cli1.use_count_inc(idx1), 2u);
  EXPECT_EQ(tracker_cli2.use_count_inc(idx1), 3u);
  EXPECT_EQ(tracker_cli1.use_count_dec(idx1), 2u);
  EXPECT_EQ(tracker_cli2.use_count_dec(idx1), 1u);
  EXPECT_EQ(tracker_admin.n_unused(), 0u);
  EXPECT_EQ(tracker_cli1.use_count_dec(idx3), 0u);
  EXPECT_EQ(tracker_admin.n_unused(), 1u);
  EXPECT_EQ(tracker_cli2.use_count_dec(idx1), 0u);
  EXPECT_EQ(tracker_admin.n_unused(), 2u);

  vector<use_ct_idx_t> unused_idxs;
  tracker_admin.emit_unused_ct_idx_hints(&unused_idxs);
  EXPECT_EQ(unused_idxs.size(), tracker_admin.n_unused());
  const auto cap = unused_idxs.capacity();
  EXPECT_GT(cap, unused_idxs.size());
  EXPECT_EQ(unused_idxs[0], idx3);
  EXPECT_EQ(unused_idxs[1], idx1);
  tracker_admin.emit_unused_ct_idx_hints(&unused_idxs);
  EXPECT_EQ(unused_idxs.capacity(), cap);
  EXPECT_TRUE(unused_idxs.empty());
  EXPECT_EQ(tracker_admin.n_unused(), 2u);

  auto saved = idx3;
  EXPECT_TRUE(tracker_admin.use_count_return_if_unused(&idx3));
  EXPECT_EQ(idx3, 0u);
  idx3 = saved;
  EXPECT_EQ(tracker_admin.n_unused(), 1u);

  saved = idx1;
  EXPECT_TRUE(tracker_admin.use_count_return_if_unused(&idx1));
  EXPECT_EQ(idx1, 0u);
  idx1 = saved;
  EXPECT_EQ(tracker_admin.n_unused(), 0u);

  constexpr size_t ROUGH_N = 1024 * 1024;
  vector<use_ct_idx_t> idxs;

  bool got_bad_alloc;
  {
    got_bad_alloc = false;
    try
    {
      const auto sev_override_auto = Config::this_thread_verbosity_override_auto(Sev::S_NONE); // Prevent log spam.
      do
      {
        idxs.push_back(tracker_admin.use_count_new());
      }
      while (idxs.size() <= ROUGH_N);
      EXPECT_TRUE(false); // Shouldn't get here: should throw bad_alloc.
    }
    catch (const bipc::bad_alloc& exc)
    {
      got_bad_alloc = true;
    }
    EXPECT_TRUE(got_bad_alloc);

    EXPECT_EQ(round_to_multiple(idxs.size(), 1024), ROUGH_N); // Should be this many unique idxs minus some change.
    EXPECT_EQ(idxs[0], idx1); // idx2 was left-in; but idx1 and idx3 would have been available immediately.
    for (size_t idx = 1; idx != idxs.size(); ++idx)
    {
      EXPECT_EQ(idxs[idx], idx3 + idx - 1); // After that it'd get idx3 + 1, ...+2, ...+3, ....
    }
  }

  FLOW_LOG_INFO("tracker_admin = [" << tracker_admin << "].");
  FLOW_LOG_INFO("tracker_cli1 = [" << tracker_cli1 << "].");
  FLOW_LOG_INFO("tracker_cli2 = [" << tracker_cli2 << "].");

  auto& mid_idx = idxs[idxs.size() / 2];
  EXPECT_EQ(tracker_cli1.use_count_dec(mid_idx), 0u);
  EXPECT_EQ(tracker_admin.n_unused(), 1u);
  auto& low_idx = *(idxs.begin() + 20);
  saved = mid_idx;
  EXPECT_TRUE(tracker_admin.use_count_return_if_unused(&mid_idx));
  EXPECT_EQ(tracker_admin.n_unused(), 0u);
  mid_idx = tracker_admin.use_count_new(); // Search will start from middle next...
  EXPECT_EQ(tracker_admin.n_unused(), 0u);
  EXPECT_EQ(mid_idx, saved);
  // ...will search to end, wrap around, search to middle, fail.

  FLOW_LOG_INFO("Search reset to middle: [" << tracker_admin << "].");

  got_bad_alloc = false;
  try
  {
    tracker_admin.use_count_new();
    EXPECT_TRUE(false);
  }
  catch (const bipc::bad_alloc& exc)
  {
    got_bad_alloc = true;
  }
  EXPECT_TRUE(got_bad_alloc);
  // Search will thus against start from middle...

  FLOW_LOG_INFO("Search wrapped around, failed, reset back to middle again: [" << tracker_admin << "].");

  saved = low_idx;
  EXPECT_EQ(tracker_admin.n_unused(), 0u);
  EXPECT_EQ(tracker_cli2.use_count_dec(low_idx), 0u);
  EXPECT_EQ(tracker_admin.n_unused(), 1u);
  EXPECT_TRUE(tracker_admin.use_count_return_if_unused(&low_idx));
  EXPECT_EQ(tracker_admin.n_unused(), 0u);
  // ...but this time after the wrap-around pretty soon it'll find it near left end.

  low_idx = tracker_admin.use_count_new(); // Search will start from middle next...
  EXPECT_EQ(tracker_admin.n_unused(), 0u);
  EXPECT_EQ(low_idx, saved);

  FLOW_LOG_INFO("Search wrapped around, succeeded around the left side: [" << tracker_admin << "].");

  /* Important to exercise wrap-around of search; and we did; but we didn't do so for the (indeed more common)
   * case where only some of the quanta (left-hand-side parts, basically) of the slot space are in-play (a
   * RAM-saving measure).  In that case any search/wrap occurs only within a subset of all the possible slots,
   * and another chunk is added to the in-play area if the search utterly fails even after wrapping.
   * We test that in Quantum_reclaim test. */
} // TEST(Lend_tracker_pool_test, Interface)

/* Tests use_count_dec_admin(): the optimized admin-thread decrement that
 * immediately returns the slot when the count reaches 0 -- no hint/scan needed.
 * (The existing Interface test only exercises client-mode use_count_dec().) */
TEST(Lend_tracker_pool_test, Dec_admin)
{
  Error_code ec;
  ipc::shm::classic::Pool_arena::remove_persistent(nullptr, S_TEST_POOL_NAME, &ec);

  Test_logger logger;
  const Log_context_mt log_ctx{&logger, Log_component::S_UNCAT};
  const atomic<bool> skip_trace{false};
  Lend_tracker_pool admin{&log_ctx, &skip_trace, S_TEST_POOL_NAME, util::CREATE_ONLY, nullptr};

  auto idx1 = admin.use_count_new(); // count=1.
  auto idx2 = admin.use_count_new(); // count=1.

  // Simulate a lend: inc idx1.
  EXPECT_EQ(admin.use_count_inc(idx1), 2u);

  // Admin-dec idx1: 2 -> 1.  Slot still in use, so idx1 is unchanged.
  EXPECT_EQ(admin.use_count_dec_admin(&idx1), 1u);
  EXPECT_NE(idx1, use_ct_idx_t(0));

  // Admin-dec idx1: 1 -> 0.  Slot returned immediately; idx1 zeroed.
  EXPECT_EQ(admin.use_count_dec_admin(&idx1), 0u);
  EXPECT_EQ(idx1, use_ct_idx_t(0));
  /* Key distinction from client-mode dec: n_unused() stays 0 because
   * the slot was returned inline (no hint/scan path needed). */
  EXPECT_EQ(admin.n_unused(), 0u);

  // Same for idx2 (count was 1, never inc'd).
  EXPECT_EQ(admin.use_count_dec_admin(&idx2), 0u);
  EXPECT_EQ(idx2, use_ct_idx_t(0));
  EXPECT_EQ(admin.n_unused(), 0u);

  // Verify the returned slots can be re-allocated.
  auto idx3 = admin.use_count_new();
  auto idx4 = admin.use_count_new();
  EXPECT_NE(idx3, use_ct_idx_t(0));
  EXPECT_NE(idx4, use_ct_idx_t(0));

  // Clean up.
  EXPECT_EQ(admin.use_count_dec_admin(&idx3), 0u);
  EXPECT_EQ(admin.use_count_dec_admin(&idx4), 0u);
} // TEST(Lend_tracker_pool_test, Dec_admin)

/* Tests the dead() flag and the admin-dtor's SHM-pool cleanup (shm_unlink()).
 *
 * Lifecycle being tested:
 *   -# Admin creates pool; client opens it.
 *   -# dead() is false on both handles.
 *   -# Admin is destroyed (dtor sets m_dead=true, then shm_unlink()'s the name).
 *   -# Client's dead() now returns true.
 *   -# A new Open_only ctor fails (name is gone).
 *   -# Client's existing mapping is still valid (POSIX shm_unlink() semantics:
 *      unlink removes the name, not existing mappings). */
TEST(Lend_tracker_pool_test, Dead_flag_and_dtor_cleanup)
{
  Error_code ec;
  ipc::shm::classic::Pool_arena::remove_persistent(nullptr, S_TEST_POOL_NAME, &ec);

  Test_logger logger;
  const Log_context_mt log_ctx{&logger, Log_component::S_UNCAT};
  const atomic<bool> skip_trace{false};

  /* We need the client handle to outlive the admin handle, so we control the
   * admin's lifetime explicitly via std::optional. */
  optional<Lend_tracker_pool> admin;
  admin.emplace(&log_ctx, &skip_trace, S_TEST_POOL_NAME, util::CREATE_ONLY, nullptr);

  // Allocate a slot so the pool isn't trivially empty.
  const auto idx = admin->use_count_new();

  // Open a client handle while the admin is still alive.
  Lend_tracker_pool client{&log_ctx, &skip_trace, S_TEST_POOL_NAME, util::OPEN_ONLY};

  EXPECT_FALSE(admin->dead());
  EXPECT_FALSE(client.dead());
  EXPECT_EQ(client.use_count(idx), 1u); // Readable from the client side.

  // --- Destroy the admin. ---
  admin.reset();

  // The client's dead() reads the flag the admin dtor just set.
  EXPECT_TRUE(client.dead());

  // The mapping is still valid (POSIX semantics); use-count data is still accessible.
  EXPECT_EQ(client.use_count(idx), 1u);

  // The pool name is gone: a new Open_only ctor should throw.
  bool open_threw = false;
  try
  {
    Lend_tracker_pool should_fail{&log_ctx, &skip_trace, S_TEST_POOL_NAME, util::OPEN_ONLY};
  }
  catch (...)
  {
    open_threw = true;
  }
  EXPECT_TRUE(open_threw);
} // TEST(Lend_tracker_pool_test, Dead_flag_and_dtor_cleanup)

/* Tests hint-array saturation: when more slots reach count=0 via client-mode
 * dec than the hint array can hold (S_N_UNUSED_IDX_HINTS), the excess are silently un-hinted.
 * The admin must find them via the fallback scan (use_count_return_if_unused() on each known
 * index, bounded by n_unused()). */
TEST(Lend_tracker_pool_test, Hint_saturation)
{
  Error_code ec;
  ipc::shm::classic::Pool_arena::remove_persistent(nullptr, S_TEST_POOL_NAME, &ec);

  Test_logger logger;
  auto logger_ptr = &logger;
#if 1 // It's rather spammy.
  logger_ptr = {};
#endif
  const Log_context_mt log_ctx{logger_ptr, Log_component::S_UNCAT};
  const atomic<bool> skip_trace{false};
  Lend_tracker_pool admin{&log_ctx, &skip_trace, S_TEST_POOL_NAME, util::CREATE_ONLY, nullptr};
  Lend_tracker_pool client{&log_ctx, &skip_trace, S_TEST_POOL_NAME, util::OPEN_ONLY};

  // Allocate 2x the hint-array capacity.
  constexpr size_t N_HINTS = Lend_tracker_pool::S_N_UNUSED_IDX_HINTS;
  constexpr size_t N = 2 * N_HINTS;
  vector<use_ct_idx_t> idxs;
  for (size_t i = 0; i != N; ++i)
  {
    idxs.push_back(admin.use_count_new());
  }

  /* Client decrements all to 0.  The first N_HINTS will each find a free hint slot
   * and register themselves.  The remaining N_HINTS find the hint array full and
   * drop their hint silently.  All N increment n_unused(). */
  for (const auto idx : idxs)
  {
    EXPECT_EQ(client.use_count_dec(idx), 0u);
  }
  EXPECT_EQ(admin.n_unused(), N);

  // --- Phase 1: consume hints; should get exactly N_HINTS. ---
  vector<use_ct_idx_t> hints;
  admin.emit_unused_ct_idx_hints(&hints);
  EXPECT_EQ(hints.size(), N_HINTS);

  /* Return the hinted slots.  We find each in idxs and return_if_unused there
   * (which zeroes the entry, so the Phase 3 scan skips it). */
  unsigned int total_returned = 0;
  for (const auto hint_idx : hints)
  {
    for (auto& idx : idxs)
    {
      if (idx == hint_idx)
      {
        EXPECT_TRUE(admin.use_count_return_if_unused(&idx));
        ++total_returned;
        break;
      }
    }
  }
  EXPECT_EQ(total_returned, unsigned(N_HINTS));
  EXPECT_EQ(admin.n_unused(), unsigned(N - N_HINTS));

  // --- Phase 2: no more hints (the remaining N_HINTS never made it into the array). ---
  admin.emit_unused_ct_idx_hints(&hints);
  EXPECT_TRUE(hints.empty());

  // --- Phase 3: fallback scan over all remaining non-zero indices. ---
  for (auto& idx : idxs)
  {
    if (idx != 0)
    {
      EXPECT_TRUE(admin.use_count_return_if_unused(&idx));
      ++total_returned;
    }
  }
  EXPECT_EQ(total_returned, unsigned(N));
  EXPECT_EQ(admin.n_unused(), 0u);
} // TEST(Lend_tracker_pool_test, Hint_saturation)

/* Tests concurrent use-count operations from multiple threads.
 *
 * Two scenarios in sequence, sharing one admin pool:
 *
 * (A) "No one reaches zero": N_CLIENT_THREADS client threads race to dec
 * the same N_SLOTS slots.  Each slot starts at count = N_CLIENT_THREADS + 1
 * (one for each client, plus the admin).  After clients join, every slot
 * must be at count=1 (the admin's ref).  Admin then cleans up via dec_admin.
 * This verifies atomic dec correctness without the hint/n_unused() path.
 *
 * (B) "Race to zero": Each slot starts at count=2.  Two client threads each
 * dec every slot once.  Which thread "wins" (gets 1->0) is non-deterministic.
 * After joining, all counts are 0.  Verifies the hint mechanism and
 * n_unused() bookkeeping under concurrency. */
TEST(Lend_tracker_pool_test, Multithread)
{
  Error_code ec;
  ipc::shm::classic::Pool_arena::remove_persistent(nullptr, S_TEST_POOL_NAME, &ec);

  Test_logger logger;
  auto logger_ptr = &logger;
#if 1 // It's rather spammy.
  logger_ptr = {};
#endif
  const Log_context_mt log_ctx{logger_ptr, Log_component::S_UNCAT};
  const atomic<bool> skip_trace{false};
  Lend_tracker_pool admin{&log_ctx, &skip_trace, S_TEST_POOL_NAME, util::CREATE_ONLY, nullptr};

  constexpr size_t N_SLOTS = 50;
  constexpr unsigned int N_CLIENT_THREADS = 4;

  // ---- Scenario A: concurrent dec, admin holds final ref. ----
  {
    vector<use_ct_idx_t> idxs;
    for (size_t i = 0; i != N_SLOTS; ++i)
    {
      idxs.push_back(admin.use_count_new());
      for (unsigned int k = 0; k != N_CLIENT_THREADS; ++k)
      {
        admin.use_count_inc(idxs.back());
      }
      // count = N_CLIENT_THREADS + 1.
    }

    atomic<bool> go{false};

    vector<unique_ptr<Thread_loop>> cli_threads;
    for (unsigned int t = 0; t != N_CLIENT_THREADS; ++t)
    {
      cli_threads.push_back(make_unique<Thread_loop>(logger_ptr, "client"));
      cli_threads.back()->start();
    }
    for (auto& thr : cli_threads)
    {
      thr->post([&]()
      {
        const Log_context_mt log_ctx{logger_ptr, Log_component::S_UNCAT};
        const atomic<bool> skip_trace{false};
        Lend_tracker_pool client{&log_ctx, &skip_trace, S_TEST_POOL_NAME, util::OPEN_ONLY};
        while (!go.load(std::memory_order_acquire)) {} // Spin-wait for release.

        for (size_t i = 0; i != N_SLOTS; ++i)
        {
          client.use_count_dec(idxs[i]);
        }
      }, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_START); // Ensure task is spinning before we proceed.
    }

    go.store(true, std::memory_order_release);

    for (auto& thr : cli_threads)
    {
      thr->stop();
    }

    /* Each slot was decremented exactly N_CLIENT_THREADS times concurrently.
     * If any atomic dec was lost, some count would be != 1. */
    for (const auto idx : idxs)
    {
      EXPECT_EQ(admin.use_count(idx), 1u);
    }
    EXPECT_EQ(admin.n_unused(), 0u); // No client reached 0 -- admin still holds a ref.

    // Admin cleans up: dec_admin brings each from 1 to 0, returning inline.
    for (auto& idx : idxs)
    {
      EXPECT_EQ(admin.use_count_dec_admin(&idx), 0u);
      EXPECT_EQ(idx, use_ct_idx_t(0));
    }
    EXPECT_EQ(admin.n_unused(), 0u);
  } // Scenario A.

  // ---- Scenario B: race to zero. ----
  {
    vector<use_ct_idx_t> idxs;
    for (size_t i = 0; i != N_SLOTS; ++i)
    {
      idxs.push_back(admin.use_count_new()); // count = 1.
      admin.use_count_inc(idxs.back()); // count = 2.
    }

    Thread_loop t1{logger_ptr, "client1"};
    Thread_loop t2{logger_ptr, "client2"};
    t1.start();
    t2.start();

    const auto client_func = [&]()
    {
      const Log_context_mt log_ctx;
      const atomic<bool> skip_trace{false};
      Lend_tracker_pool client{&log_ctx, &skip_trace, S_TEST_POOL_NAME, util::OPEN_ONLY};
      for (size_t i = 0; i != N_SLOTS; ++i)
      {
        client.use_count_dec(idxs[i]);
      }
    };
    // S_ASYNC_AND_AWAIT_CONCURRENT_START: each task is guaranteed running before post() returns.
    t1.post(client_func, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_START);
    t2.post(client_func, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_START);

    t1.stop();
    t2.stop();

    // Each slot was decremented exactly twice (from 2): all are at 0.
    EXPECT_EQ(admin.n_unused(), N_SLOTS);

    /* Return all slots.  Some may be hinted (up to S_N_UNUSED_IDX_HINTS); rest need a scan.
     * The exact hint count is non-deterministic (depends on which thread
     * reached 0 first and whether the hint array had room at that instant). */
    vector<use_ct_idx_t> hints;
    admin.emit_unused_ct_idx_hints(&hints);
    EXPECT_LE(hints.size(), Lend_tracker_pool::S_N_UNUSED_IDX_HINTS);

    for (const auto hint_idx : hints)
    {
      for (auto& idx : idxs)
      {
        if (idx == hint_idx)
        {
          EXPECT_TRUE(admin.use_count_return_if_unused(&idx)); // Zeroes idx.
          break;
        }
      }
    }
    // Scan remainder.
    for (auto& idx : idxs)
    {
      if (idx != 0)
      {
        EXPECT_TRUE(admin.use_count_return_if_unused(&idx));
      }
    }
    EXPECT_EQ(admin.n_unused(), 0u);
  } // Scenario B.
} // TEST(Lend_tracker_pool_test, Multithread)

/* Exercises every LTP operation with a null Logger to verify nothing crashes.
 * (Historically, manual should_log() checks before FLOW_LOG_*_WITHOUT_CHECKING() could
 * dereference a null Logger if the guard was not coded carefully.) */
TEST(Lend_tracker_pool_test, Logging_null_logger)
{
  Error_code ec;
  ipc::shm::classic::Pool_arena::remove_persistent(nullptr, S_TEST_POOL_NAME, &ec);

  // Every operation that could possibly log, all with nullptr logger.
  const Log_context_mt log_ctx;
  const atomic<bool> skip_trace{false};
  Lend_tracker_pool admin{&log_ctx, &skip_trace, S_TEST_POOL_NAME, util::CREATE_ONLY, nullptr};
  Lend_tracker_pool client{&log_ctx, &skip_trace, S_TEST_POOL_NAME, util::OPEN_ONLY};

  auto idx1 = admin.use_count_new();
  admin.use_count_inc(idx1);
  client.use_count_dec(idx1); // 2 -> 1.
  admin.use_count(idx1);
  admin.n_unused();

  auto idx2 = admin.use_count_new();
  admin.use_count_dec_admin(&idx2); // 1 -> 0, returned inline.

  client.use_count_dec(idx1); // 1 -> 0, via client path.

  vector<use_ct_idx_t> hints;
  admin.emit_unused_ct_idx_hints(&hints);
  admin.use_count_return_if_unused(&idx1);

  EXPECT_FALSE(admin.dead());
  EXPECT_FALSE(client.dead());
  // Dtors run with null logger -- must not crash.
} // TEST(Lend_tracker_pool_test, Logging_null_logger)

/* Verifies that the steady-state fast-path operations (new(), inc(), dec(), dec_admin(),
 * return_if_unused(), emit_hints()) produce zero INFO-or-higher log output.
 * Only ctor and dtor are expected to log at INFO; we drive the logger via the owning `log_ctx`.
 * This is the key perf-regression guard: if someone accidentally adds an INFO/WARNING
 * statement to a hot path, this test catches it. */
TEST(Lend_tracker_pool_test, Logging_no_info_on_fast_path)
{
  Error_code ec;
  ipc::shm::classic::Pool_arena::remove_persistent(nullptr, S_TEST_POOL_NAME, &ec);

  // Construct with null logger (ctor INFO output goes nowhere).
  Log_context_mt log_ctx;
  const atomic<bool> skip_trace{false};
  Lend_tracker_pool admin{&log_ctx, &skip_trace, S_TEST_POOL_NAME, util::CREATE_ONLY, nullptr};
  Lend_tracker_pool client{&log_ctx, &skip_trace, S_TEST_POOL_NAME, util::OPEN_ONLY};

  // Attach a Buffer_logger at INFO level, then snapshot the buffer size.
  Config cfg_info{Sev::S_INFO};
  cfg_info.init_component_to_union_idx_mapping<Log_component>
    (100, Config::standard_component_payload_enum_sparse_length<Log_component>(), true);
  Buffer_logger buf_logger{&cfg_info};
  log_ctx.set_logger(&buf_logger);

  const auto baseline_sz = buf_logger.buffer_str().size();

  // --- Exercise every fast-path operation. ---
  auto idx1 = admin.use_count_new();
  auto idx2 = admin.use_count_new();
  admin.use_count_inc(idx1);
  client.use_count_dec(idx1); // 2 -> 1.
  admin.use_count(idx1);
  admin.n_unused();

  client.use_count_dec(idx1); // 1 -> 0.
  vector<use_ct_idx_t> hints;
  admin.emit_unused_ct_idx_hints(&hints);
  admin.use_count_return_if_unused(&idx1);
  admin.use_count_dec_admin(&idx2); // 1 -> 0, returned inline.

  // No new INFO output should have appeared.
  EXPECT_EQ(buf_logger.buffer_str().size(), baseline_sz)
    << "Fast-path operations produced INFO-level log output -- performance regression!";

  // Detach before dtor (dtor logs at INFO; we don't want to pollute the check).
  log_ctx.set_logger(nullptr);
} // TEST(Lend_tracker_pool_test, Logging_no_info_on_fast_path)

/* Tests the fast-path TRACE-skip behavior of Lend_tracker_pool.
 *
 * Lend_tracker_pool reads its owner's `Log_context_mt` (here `log_ctx`) plus an external `atomic<bool>` skip-flag
 * (here `skip_trace`).  Two independent gates control whether a fast-path TRACE statement is emitted:
 *   - `skip_trace`: if `true`, the fast path short-circuits *before* even consulting should_log() -- no output.
 *   - else: emission defers to a `should_log()` read (under lock, fresh each call), so it tracks the current
 *     logger and its live config.
 * This test exercises both gates and the fresh-read property. */
TEST(Lend_tracker_pool_test, Logging_trace_skip_cache)
{
  Error_code ec;
  ipc::shm::classic::Pool_arena::remove_persistent(nullptr, S_TEST_POOL_NAME, &ec);

  // Non-const: this test drives both the logger (via log_ctx) and the skip-flag.
  Log_context_mt log_ctx; // Let's not spam the console (until we attach Buffer_loggers below).
  atomic<bool> skip_trace{false};
  Lend_tracker_pool admin{&log_ctx, &skip_trace, S_TEST_POOL_NAME, util::CREATE_ONLY, nullptr};

  // One operation is enough to probe whether TRACE output is emitted.
  const auto do_one_op = [&]()
  {
    auto idx = admin.use_count_new();
    admin.use_count_dec_admin(&idx);
  };

  /* Helper: set the skip-flag; attach a fresh Buffer_logger at the given severity; run one op;
   * return whether any TRACE output appeared. */
  const auto probe_trace = [&](Sev sev, bool skip) -> bool
  {
    skip_trace.store(skip, std::memory_order_relaxed);
    Config cfg{sev};
    cfg.init_component_to_union_idx_mapping<Log_component>
      (100, Config::standard_component_payload_enum_sparse_length<Log_component>(), true);
    Buffer_logger buf{&cfg};
    log_ctx.set_logger(&buf);

    const auto after_set = buf.buffer_str().size();
    do_one_op();
    return buf.buffer_str().size() > after_set;
  };

  // --- Gate 1: should_log() decides (skip-flag off). ---
  EXPECT_FALSE(probe_trace(Sev::S_INFO, false))
    << "TRACE output appeared at INFO level (should_log() should suppress it).";
  EXPECT_TRUE(probe_trace(Sev::S_TRACE, false))
    << "TRACE output missing at TRACE level.";
  EXPECT_TRUE(probe_trace(Sev::S_DATA, false))
    << "TRACE output missing at DATA level.";
  EXPECT_FALSE(probe_trace(Sev::S_INFO, false))
    << "TRACE output appeared at INFO level after downgrade.";

  // --- Gate 2: skip-flag short-circuits even a TRACE-enabled logger; clearing it restores emission. ---
  EXPECT_FALSE(probe_trace(Sev::S_TRACE, true))
    << "TRACE output appeared despite skip-flag = true (fast path should short-circuit before should_log()).";
  EXPECT_TRUE(probe_trace(Sev::S_TRACE, false))
    << "TRACE output missing after skip-flag cleared.";

  /* --- Fresh-read: a live config change (no set_logger() call) is reflected. ---
   * Lend_tracker_pool reads should_log() fresh each op, so flipping the *same* logger's config from INFO to TRACE
   * takes effect on the very next op. */
  skip_trace.store(false, std::memory_order_relaxed);
  Config cfg_live{Sev::S_INFO};
  cfg_live.init_component_to_union_idx_mapping<Log_component>
    (100, Config::standard_component_payload_enum_sparse_length<Log_component>(), true);
  Buffer_logger buf_live{&cfg_live};
  log_ctx.set_logger(&buf_live);

  const auto after_info = buf_live.buffer_str().size();
  do_one_op();
  EXPECT_EQ(buf_live.buffer_str().size(), after_info)
    << "TRACE output appeared at INFO level.";

  cfg_live.configure_default_verbosity(Sev::S_TRACE, true); // Elevate config in place; deliberately no set_logger().
  do_one_op();
  EXPECT_GT(buf_live.buffer_str().size(), after_info)
    << "Live config change to TRACE was not reflected; Lend_tracker_pool should read should_log() fresh.";

  log_ctx.set_logger(nullptr);
} // TEST(Lend_tracker_pool_test, Logging_trace_skip_cache)

/* Tests Use_count_registry (inside Lend_tracker_pool) bitmap wrap-around reclamation and expansion via
 * page-residency measurement.  Structured in phases; each phase verifies page-count behavior (flat vs. climbing).
 *
 * Reminder of how algorithm works roughly: A bitmap compactly stores which slots are available.
 * use_count_new() finds bit=0 slot by searching whole words at a time => (search) cursor remains on same word
 * for next time, unless bit was last one in word; then ++cursor.  There are N quanta within this space, each
 * covering Q slots.  (E.g., 32 quanta with 32ki slots each for a total of 1024ki slots.)  Only Q0 is active
 * at first, to save RAM.  Search starts at cursor (word-wise, not slot-wise -- as slot <=> individual bit within word).
 * Search looks through active quanta only (Q0 only at first), wrapping around if needed to find bit=0 slot.
 * If no 0-bits left => activate (forever) next quantum (Q1 first time; next time Q2, Q3...), return first slot
 * in new quantum; cursor saved accordingly.
 *
 * The keys here:
 *   - Pages not in RAM until touched.  If Q0, say, is untouched, and then we allocate the bejesus out of it,
 *     page 1 will go into RAM; then page 2; etc.: the entire Q0 (or Q1, ...) is *not* somehow initialized and
 *     paged-in all at the same time but only as the bitmap-cursor finds slots in it.
 *   - Therefore the point of the quanta is *not* to use-up quantized ~large (e.g. 128Ki per quantum as of this
 *     writing: 32Ki slots x 4-byte capacity per slot) amounts of RAM in a stairstep pattern.  Rather it is to
 *     have existing quantum or quanta be used up before taking more RAM for slots: it lowers amount of RAM used
 *     when many objects are extant at the cost of potentially longer searches in that situation.  In any case
 *     here we just test what's expected; see Use_count_registry docs for design discussion(s).
 *
 * Detail: Lend_tracker_pool/Use_count_registry parks a small "metadata" header at the start of the data area, and
 * since it is not possible nor attempted to return that area, the first potentially available slot index is always
 * just past that.  It's not super-important in and of itself; just be aware a few leading would-be slots are never
 * in the running.  Tests hopefully get the actual # of these unusable slots (as of this writing: 64; could change)
 * and then compensate as needed. */
TEST(Lend_tracker_pool_test, Quantum_reclaim)
{
#ifndef FLOW_OS_LINUX
  GTEST_SKIP() << "::mincore() RAM measurement relies on Linux-specific semantics (not tested elsewhere); skipping.";
#else

  Error_code ec;
  ipc::shm::classic::Pool_arena::remove_persistent(nullptr, S_TEST_POOL_NAME, &ec);

  Test_logger logger;
  FLOW_LOG_SET_CONTEXT(&logger, Log_component::S_UNCAT);

  using Ucr = Use_count_registry;
  /* As of this writing 32768 slots per quantum, 32 quanta => total 1Mi slots (minus 64 for metadata header area).
   * 4 bytes per slot (use-count goes up to 4+ billion) => total 4Mi RAM max (128Ki RAM min: 1 quantum).
   *
   * Plus bitmap area sized 128Ki (1Mi slots / 8 bits per byte; 1 bit per mapped slot).  Incidentally not-yet-used
   * pages in data area <=> not yet used (8x smaller) parts of bitmap area => a page enters RAM due to new bitmap
   * use, every now and then.  Due to this and who-knows-what rounding artifacts, when we check page-count
   * differentials below, we have some tolerance to get an inexact result.  Though, the most key check is that
   * data areas are reused within active quanta; and in that case we can check that exactly 0 pages entered RAM.
   *
   * Hopefully we compute most #s instead of hard-coding them though; that was mainly for illustration. */
  constexpr size_t Q = Ucr::S_USE_COUNTS_CAPACITY_QUANTUM_SZ;
  constexpr size_t PAGE_SZ = Ucr::S_PAGE_SZ;
  constexpr size_t DATA_PAGES_PER_QUANTUM = (Q * Ucr::S_ALLOC_SZ) / PAGE_SZ; // 32 pages.
  constexpr unsigned int N_QUANTA = 6; // Fill this many quanta in Phase 0.
  // Checkpoints per phase (for verbose logging).
  constexpr unsigned int M = 10;

  const Log_context_mt log_ctx; // Let's not spam the console output this time.
  const atomic<bool> skip_trace{false};
  Lend_tracker_pool admin{&log_ctx, &skip_trace,
                          S_TEST_POOL_NAME, util::CREATE_ONLY, nullptr};

  // Open the same SHM segment independently (read-only) so we can mincore() it.
  bipc::shared_memory_object shm_obj{bipc::open_only, S_TEST_POOL_NAME.native_str(), bipc::read_only};
  bipc::mapped_region region{shm_obj, bipc::read_only};
  const auto region_addr = static_cast<uint8_t*>(region.get_address());
  const auto region_sz = region.get_size();

  // --- Helpers. ---

  const auto count_resident_pages = [&]() -> size_t
  {
    const size_t n_pages = ceil_div(region_sz, PAGE_SZ);
    vector<unsigned char> vec(n_pages);
    const int rc = ::mincore(region_addr, region_sz, vec.data());
    if (rc != 0)
    {
      const Error_code sys_err_code{errno, system_category()};
      EXPECT_TRUE(false) << "mincore() failed: errno = [" << sys_err_code << "] [" << sys_err_code.message() << "].";
    }

    size_t resident = 0;
    for (const auto byte : vec)
    {
      (byte & 1) && (++resident);
    }
    return resident;
  };

  // --- Phase 0: Fill exactly N_QUANTA quanta. ---

  /* First alloc tells us metadata occupancy.  Metadata lives at the start of Q0; the allocator
   * marks those bitmap words as fully occupied.  The first use_count_new() returns the index
   * just past metadata.  64 slots (1 bitmap word of 64 bits) is the expected value. */
  const auto first_idx = admin.use_count_new();
  const size_t metadata_slots = first_idx; // Indices 0..first_idx-1 are permanently metadata.
  const size_t total_allocatable = (N_QUANTA * Q) - metadata_slots; // Remaining free slots in Q0-Q5.

  const size_t p_before = count_resident_pages();
  FLOW_LOG_INFO("Phase 0: first idx = [" << first_idx << "] => metadata_slots = [" << metadata_slots << "]; "
                "filling [" << total_allocatable << "] slots across [" << N_QUANTA << "] quanta "
                "(Q = [" << Q << "], data-pages/quantum = [" << DATA_PAGES_PER_QUANTUM << "]); "
                "pages before fill = [" << p_before << "].");

  vector<use_ct_idx_t> all_indices;
  all_indices.reserve(total_allocatable);
  all_indices.push_back(first_idx); // Already got one.

  const size_t alloc_step = ceil_div(total_allocatable, size_t(M));
  for (size_t i = 1; i != total_allocatable; ++i)
  {
    all_indices.push_back(admin.use_count_new());
    if (((i % alloc_step) == 0) || (i == (total_allocatable - 1)))
    {
      FLOW_LOG_INFO("Phase 0: alloc [" << (i + 1) << "/" << total_allocatable << "], "
                    "pages = [" << count_resident_pages() << "].");
    }
  }

  const size_t p_full = count_resident_pages();
  const size_t p0_growth = p_full - p_before;
  FLOW_LOG_INFO("Phase 0: filled.  Pages = [" << p_full << "] (+" << p0_growth << "), "
                "last idx = [" << all_indices.back() << "].  "
                "Expected growth ~ [" << (N_QUANTA * DATA_PAGES_PER_QUANTUM) << "].  "
                "Cursor should have wrapped to word 0 (metadata area).");

  /* Ballpark: N_QUANTA data-quanta worth of pages, plus a small bitmap/metadata overhead.  Allow -2 on the
   * lower side: the very first data page is shared between the metadata-allocate and the first user use_count_new()
   * (both performed before p_before was sampled), so one of the "N_QUANTA * DATA_PAGES_PER_QUANTUM" data pages
   * is already counted in p_before and does not appear as growth.  See also Phase 3 below for similar -2. */
  EXPECT_GE(p0_growth, (N_QUANTA * DATA_PAGES_PER_QUANTUM) - 2)
    << "Phase 0: too few pages after filling [" << N_QUANTA << "] quanta.";
  EXPECT_LE(p0_growth, (N_QUANTA * DATA_PAGES_PER_QUANTUM) + N_QUANTA + 4)
    << "Phase 0: too many pages after filling [" << N_QUANTA << "] quanta.";

  // --- Phase 1: Wrap-around reclaim (search 2 path).  Pages must stay flat. ---

  /* Step 1: Position cursor into Q3.  Free 1 slot in Q3, allocate 1.  Search 1 scans forward
   * from word 0 (where cursor sits now) through metadata (full) and Q0-Q2 (full), finds the
   * freed Q3 slot.  Cursor lands on that word in Q3.  Net slots: unchanged, all full again. */
  {
    /* Pick a slot solidly inside Q3 (not near a quantum boundary).  Q3 spans indices
     * [3*Q, 4*Q).  We pick something in the middle -- say offset Q/2 into Q3. */
    const size_t q3_mid_offset = (3 * Q) + (Q / 2) - metadata_slots; // Index into all_indices[].
    auto positioning_idx = all_indices[q3_mid_offset];
    FLOW_LOG_INFO("Phase 1 step 1: positioning cursor to Q3 via idx [" << positioning_idx << "].");

    admin.use_count_dec_admin(&positioning_idx);
    const auto repos_idx = admin.use_count_new();
    EXPECT_EQ(repos_idx, all_indices[q3_mid_offset])
      << "Cursor reposition: expected to reclaim the same Q3 slot.";
    FLOW_LOG_INFO("Phase 1 step 1: repositioned.  Reclaimed idx [" << repos_idx << "], "
                  "pages = [" << count_resident_pages() << "] (expect [" << p_full << "]).");
  }

  /* Step 2: Free 25% of Q0-Q1 slots (all well before cursor in Q3).  Then allocate same count.
   * First alloc: search 1 from Q3 cursor through Q3-Q5 (all full) => fails.
   *              search 2 wraps to word 0 => finds freed Q0 slots => wrap-around!
   * Subsequent allocs: cursor now in Q0 area, search 1 hops forward through freed Q0-Q1 slots.
   * Pages must stay at p_full throughout (all in already-paged territory). */
  {
    /* Q0 allocatable slots: indices [0, Q - metadata_slots) in all_indices[].
     * Q1 allocatable slots: indices [Q - metadata_slots, 2*Q - metadata_slots) in all_indices[].
     * Free every 4th slot in Q0-Q1 => 25% of Q0-Q1. */
    const size_t q0q1_count = (2 * Q) - metadata_slots; // Total allocatable slots in Q0-Q1.
    vector<use_ct_idx_t> freed_indices;
    freed_indices.reserve(q0q1_count / 4);
    for (size_t i = 0; i < q0q1_count; i += 4)
    {
      freed_indices.push_back(all_indices[i]);
      admin.use_count_dec_admin(&all_indices[i]);
    }
    const size_t n_freed = freed_indices.size();

    /* Sort so we know the expected reclaim order (lowest index first, since the bitmap scan
     * goes forward from word 0 after wrapping). */
    std::sort(freed_indices.begin(), freed_indices.end());

    FLOW_LOG_INFO("Phase 1 step 2: freed [" << n_freed << "] Q0-Q1 slots (25%).  "
                  "Lowest freed idx = [" << freed_indices.front() << "], "
                  "highest = [" << freed_indices.back() << "].  "
                  "Pages = [" << count_resident_pages() << "] (expect [" << p_full << "]).");

    // Re-allocate the same count.  Track page count at checkpoints.
    const size_t reclaim_step = ceil_div(n_freed, size_t(M));
    vector<use_ct_idx_t> reclaimed;
    reclaimed.reserve(n_freed);
    for (size_t i = 0; i != n_freed; ++i)
    {
      reclaimed.push_back(admin.use_count_new());
      if (((i % reclaim_step) == 0) || (i == (n_freed - 1)))
      {
        FLOW_LOG_INFO("Phase 1 step 2: reclaim [" << (i + 1) << "/" << n_freed << "], "
                      "idx = [" << reclaimed.back() << "], "
                      "pages = [" << count_resident_pages() << "].");
      }
    }

    EXPECT_EQ(reclaimed.front(), freed_indices.front())
      << "First reclaim should be the lowest freed idx (proves wrap-around).";

    EXPECT_EQ(count_resident_pages(), p_full)
      << "Phase 1: pages increased during reclaim -- slots were not reused!";

    FLOW_LOG_INFO("Phase 1: done.  Pages = [" << count_resident_pages() << "].");
  }

  // --- Phase 2: Forward reclaim (search 1 path, no wrap).  Pages must stay flat. ---

  /* Cursor is now in Q1 area (end of Phase 1 reclaim).  Free 25% of Q4-Q5 slots (all after
   * cursor).  Allocate same count.  Search 1 scans forward from Q1, skips full Q2-Q3, finds
   * freed Q4-Q5 slots.  No wrap-around needed.  Pages stay flat. */
  {
    const size_t q4_start = (4 * Q) - metadata_slots; // Index into all_indices[] for start of Q4.
    const size_t q6_start = (6 * Q) - metadata_slots; // One past last Q5 slot in all_indices[].
    vector<use_ct_idx_t> freed_indices;
    freed_indices.reserve((q6_start - q4_start) / 4);
    for (size_t i = q4_start; i < q6_start; i += 4)
    {
      freed_indices.push_back(all_indices[i]);
      admin.use_count_dec_admin(&all_indices[i]);
    }
    const size_t n_freed = freed_indices.size();
    std::sort(freed_indices.begin(), freed_indices.end());

    FLOW_LOG_INFO("Phase 2: freed [" << n_freed << "] Q4-Q5 slots (25%).  "
                  "Lowest freed idx = [" << freed_indices.front() << "], "
                  "highest = [" << freed_indices.back() << "].  "
                  "Pages = [" << count_resident_pages() << "] (expect [" << p_full << "]).");

    const size_t reclaim_step = ceil_div(n_freed, size_t(M));
    vector<use_ct_idx_t> reclaimed;
    reclaimed.reserve(n_freed);
    for (size_t i = 0; i != n_freed; ++i)
    {
      reclaimed.push_back(admin.use_count_new());
      if (((i % reclaim_step) == 0) || (i == (n_freed - 1)))
      {
        FLOW_LOG_INFO("Phase 2: reclaim [" << (i + 1) << "/" << n_freed << "], "
                      "idx = [" << reclaimed.back() << "], "
                      "pages = [" << count_resident_pages() << "].");
      }
    }

    EXPECT_EQ(reclaimed.front(), freed_indices.front())
      << "First reclaim should be the lowest freed Q4 idx (forward search).";

    EXPECT_EQ(count_resident_pages(), p_full)
      << "Phase 2: pages increased during reclaim -- slots were not reused!";

    FLOW_LOG_INFO("Phase 2: done.  Pages = [" << count_resident_pages() << "].");
  }

  // --- Phase 3: Expansion.  Pages must climb by ~DATA_PAGES_PER_QUANTUM per quantum. ---

  /* Everything is full again (Q0-Q5).  Allocate 2 more quanta worth of slots.  This forces
   * expansion to Q6 and Q7.  Pages should climb diagonally. */
  {
    constexpr unsigned int N_EXPAND = 2;
    const size_t n_expand = N_EXPAND * Q;
    FLOW_LOG_INFO("Phase 3: allocating [" << n_expand << "] slots to force [" << N_EXPAND << "] expansions.");

    const size_t expand_step = ceil_div(n_expand, size_t(M));
    for (size_t i = 0; i != n_expand; ++i)
    {
      admin.use_count_new();
      if (((i % expand_step) == 0) || (i == (n_expand - 1)))
      {
        FLOW_LOG_INFO("Phase 3: alloc [" << (i + 1) << "/" << n_expand << "], "
                      "pages = [" << count_resident_pages() << "].");
      }
    }

    const size_t p_expanded = count_resident_pages();
    const size_t pages_added = p_expanded - p_full;
    FLOW_LOG_INFO("Phase 3: done.  Pages [" << p_full << "] -> [" << p_expanded << "] "
                  "(+" << pages_added << "); "
                  "expected ~[" << (N_EXPAND * DATA_PAGES_PER_QUANTUM) << "].");

    /* Tolerance: each quantum adds exactly DATA_PAGES_PER_QUANTUM (32) data pages, plus
     * possibly 1 bitmap page (the bitmap for Q6-Q7 hasn't been touched until now).
     * Allow -2 in case rounding or page sharing absorbs a boundary page. */
    EXPECT_GE(pages_added, (N_EXPAND * DATA_PAGES_PER_QUANTUM) - 2)
      << "Phase 3: too few new pages after expansion.";
    EXPECT_LE(pages_added, (N_EXPAND * (DATA_PAGES_PER_QUANTUM + 1)) + 2)
      << "Phase 3: too many new pages after expansion.";
  }

#endif
} // TEST(Lend_tracker_pool_test, Quantum_reclaim)

/* Tests intra-word cursor behavior in the Use_count_registry (inside Lend_tracker_pool) bitmap search.
 * See header for Quantum_reclaim test for summary of relevant algorithm.  What's tested here is a detail
 * of what happens around bitmap word boundaries.  It's a touchy algorithm; it'd be easy to change something
 * and unintentionally mess up white-box behaviors that wouldn't necessarily come off as functional bugs
 * (a slot would be still found, probably, even if the search cursor is jumping not how we want) but could
 * cause insidious perf issues (or at least perf-pretinent entropy/lack of understanding).
 *
 * After allocating a bit that is NOT the last bit (bit 63) in its word, the cursor stays on that
 * same word.  The next search re-scans from bit 0 of that word via <find-first-set of ~bits> intrinsic,
 * which always yields the lowest free bit.  Three scenarios:
 *
 * (A) "Apparent backtrack": free two bits -- one LOWER and one HIGHER than the previously-found bit.
 * Next alloc finds the lower one (ffs).  The cursor didn't advance, and the result is at a
 * lower bit position than the last successful find in this word.
 *
 * (B) "Forward within word": free only a bit HIGHER than the previously-found bit.
 * Next alloc finds it (the only option).  Same word, higher bit -- the intuitive direction.
 *
 * (C) "Cursor advance on last bit": free the last bit (bit 63) of the word.  After allocating it,
 * the cursor advances to the next word.  Next alloc comes from that next word. */
TEST(Lend_tracker_pool_test, Intra_word_cursor)
{
  Error_code ec;
  ipc::shm::classic::Pool_arena::remove_persistent(nullptr, S_TEST_POOL_NAME, &ec);

  Test_logger logger;

  const Log_context_mt log_ctx; // Let's not spam from the Lend_tracker_pool(s).
  const atomic<bool> skip_trace{false};

  FLOW_LOG_SET_CONTEXT(&logger, Log_component::S_UNCAT);

  Lend_tracker_pool admin{&log_ctx, &skip_trace, S_TEST_POOL_NAME, util::CREATE_ONLY, nullptr};

  // Fill to capacity so every bit is taken and cursor wraps to word 0.
  constexpr size_t BPW = 64; // Bits per bitmap word.
  vector<use_ct_idx_t> all;
  {
    try
    {
      for (;;) { all.push_back(admin.use_count_new()); }
    }
    catch (const bipc::bad_alloc&) {}
  }
  ASSERT_FALSE(all.empty());

  /* Metadata occupies word(s) at the start; first allocatable index tells us how many slots that is.
   * all[k] has slot index (metadata_slots + k).  So all[N] is bit (N % BPW) of word ((metadata_slots + N) / BPW). */
  const size_t metadata_slots = all.front();
  ASSERT_EQ((metadata_slots % BPW), 0u) << "Expected metadata to occupy a whole number of words.";

  // Helper: slot index -> {word, bit} for human-readable logging.
  const auto slot_word = [](use_ct_idx_t idx) { return idx / BPW; };
  const auto slot_bit  = [](use_ct_idx_t idx) { return idx % BPW; };

  /* Run the 3 scenarios at a few different word locations to increase coverage.
   * base_word is relative to the first allocatable word (word 0 = first word past metadata).
   * The all[] index for bit B of base_word W is: (W * BPW) + B. */
  const size_t test_base_words[] = { 0, // Very first allocatable word.
                                     100, // Somewhere in the middle.
                                     (all.size() / BPW) - 2 }; // Near the end (penultimate word).

  for (const size_t base_word : test_base_words)
  {
    // all[] index for bit B of the target word.  +BPW gives the next word.
    const auto ai = [&](size_t bit) { return (base_word * BPW) + bit; };
    const size_t target_word = (metadata_slots / BPW) + base_word;

    FLOW_LOG_INFO("=== Intra_word_cursor: base_word [" << base_word << "] "
                  "(target bitmap word [" << target_word << "], "
                  "slot range [" << (metadata_slots + ai(0)) << ".." << (metadata_slots + ai(BPW - 1)) << "]) ===");

    /* --- Position cursor onto target word at a mid-range bit. ---
     * Free bit 30, alloc it back.  Search from wherever cursor is will eventually land here. */
    {
      admin.use_count_dec_admin(&all[ai(30)]);
      const auto idx = admin.use_count_new();
      EXPECT_EQ(idx, metadata_slots + ai(30))
        << "base_word [" << base_word << "] positioning: "
           "expected slot [" << (metadata_slots + ai(30)) << "] (bit 30).";
      // Target word full again.  Cursor on target word.  Last-found bit was 30.
    }

    /* Canary: free a slot in the *next* word.  If the cursor ever wrongly advances past
     * the target word during A or B, alloc would land on the canary instead. */
    admin.use_count_dec_admin(&all[ai(BPW + 7)]); // Bit 7 of (target_word + 1).

    /* --- (A) Apparent backtrack within same word. ---
     * Free bits 10 and 50.  ffs finds bit 10 first -- lower than previously-found bit 30. */
    {
      admin.use_count_dec_admin(&all[ai(10)]);
      admin.use_count_dec_admin(&all[ai(50)]);

      const auto a1 = admin.use_count_new();
      EXPECT_EQ(a1, metadata_slots + ai(10))
        << "base_word [" << base_word << "] A first: "
           "expected bit 10 (lower than previously-found bit 30).";

      const auto a2 = admin.use_count_new();
      EXPECT_EQ(a2, metadata_slots + ai(50))
        << "base_word [" << base_word << "] A second: expected bit 50.";

      FLOW_LOG_INFO("  (A) backtrack: [" << a1 << "] (bit [" << slot_bit(a1)
                    << "]) then [" << a2 << "] (bit [" << slot_bit(a2) << "]).");
      // Target word full again.  Last-found bit was 50.
    }

    /* --- (B) Forward within same word. ---
     * Free only bit 55.  Higher than previously-found bit 50. */
    {
      admin.use_count_dec_admin(&all[ai(55)]);

      const auto b1 = admin.use_count_new();
      EXPECT_EQ(b1, metadata_slots + ai(55))
        << "base_word [" << base_word << "] B: "
           "expected bit 55 (higher than previously-found bit 50).";

      FLOW_LOG_INFO("  (B) forward:   [" << b1 << "] (bit [" << slot_bit(b1) << "]).");
      // Target word full again.
    }

    // Reclaim the canary before (C) sets up its own next-word slot.
    {
      const auto canary = admin.use_count_new();
      EXPECT_EQ(canary, metadata_slots + ai(BPW + 7))
        << "base_word [" << base_word << "] canary reclaim: expected bit 7 of next word.";
    }

    /* --- (C) Last bit (63) triggers cursor advance to next word. ---
     * Free bit 63 (last in word).  Alloc takes it; cursor advances.
     * Then free a slot in BOTH target word and next word.  If cursor advanced correctly,
     * alloc finds the next-word slot (not the target-word one). */
    {
      admin.use_count_dec_admin(&all[ai(BPW - 1)]); // Bit 63 of target word.

      const auto c1 = admin.use_count_new();
      EXPECT_EQ(c1, metadata_slots + ai(BPW - 1))
        << "base_word [" << base_word << "] C first: expected bit 63 (last bit).";
      // Cursor should now be on (target_word + 1).

      // Free one slot in each word.
      admin.use_count_dec_admin(&all[ai(20)]);       // Bit 20 of target word.
      admin.use_count_dec_admin(&all[ai(BPW + 3)]);  // Bit 3 of next word.

      const auto c2 = admin.use_count_new();
      EXPECT_EQ(c2, metadata_slots + ai(BPW + 3))
        << "base_word [" << base_word << "] C second: "
           "expected bit 3 of next word (cursor advanced past target word).";

      // Clean up the leftover target-word slot.
      const auto c3 = admin.use_count_new();
      EXPECT_EQ(c3, metadata_slots + ai(20))
        << "base_word [" << base_word << "] C cleanup: expected bit 20 of target word.";

      FLOW_LOG_INFO("  (C) advance:   [" << c1 << "] (bit [" << slot_bit(c1) << "], "
                    "word [" << slot_word(c1) << "]) then [" << c2 << "] (bit "
                    "[" << slot_bit(c2) << "], word [" << slot_word(c2) << "]).");
    }
  } // for (base_word)
} // TEST(Lend_tracker_pool_test, Intra_word_cursor)

} // namespace ipc::shm::arena_lend::detail::test
