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

/* Unit test of jemalloc::Thread_cache: the per-thread manager of explicitly identified jemalloc tcaches,
 * whose core promise is a strict 1-to-1 tcache-per-arena-per-thread mapping (see its class doc header for
 * why that matters: a tcache shared between arenas can hand out memory from the "wrong" arena).
 *
 * What is covered here:
 *   - The thread-local-state semantics: this_thread_cache() lazily creates exactly one cache-object per
 *     thread; this_thread_cache_or_null() reports (never creates) it; id() lazily creates exactly one
 *     tcache per distinct arena ID per thread; id_or_none() reports (never creates) likewise.
 *     [TEST Laziness_and_memoization]
 *   - Distinct threads get fully independent cache-objects (and distinct thread_token()s).
 *     [TEST Thread_independence]
 *   - The 1-to-1 mapping itself, asserted at its *reason*: with real (heap) arenas and real allocation
 *     traffic through the per-arena tcaches -- including the treacherous case of tcaches seeded by
 *     deallocations, and a mid-stream flush_tcache() -- each pointer's originating arena is verified, via
 *     jemalloc's own arenas.lookup, to be the arena it was requested from.  [TEST Per_arena_tcaches]
 *   - destroy_arena_safely(), synchronous cases: with the only relevant tcache belonging to the calling
 *     thread, everything happens immediately; on_done_func receives the caller's own log_ctx and an F()
 *     whose invocation destroys the arena; and for a nonexistent arena F() throws as documented.
 *     [TEST Destroy_arena_synchronous]
 *   - destroy_arena_safely(), deferred case: with holdout threads owning relevant tcaches, nothing happens
 *     until each crosses itself off -- one via the opportunistic this_thread_cache*() path, the last via
 *     thread exit, which finalizes: arena destroyed, on_done_func run with the internal (not the
 *     requester's) log_ctx.  [TEST Destroy_arena_deferred]
 *
 * Intentionally untested, as documented-undefined-behavior, contract-forbidden, or not deterministically
 * provokable:
 *   - Using a tcache ID from a thread other than the one that created it (crash, per jemalloc).
 *   - Flushing/using a tcache after its associated arena's destruction (crash, per class doc header).
 *   - Directly constructing a Thread_cache (public ctor is an implementation artifact; see its @warning).
 *   - The anti-abort interlock between arena destruction/creation and concurrent jemalloc stats-dumping
 *     (see destroy_arena_safely() @warning): a timing race by nature; no deterministic test exists here. */

#include "ipc/shm/arena_lend/jemalloc/thread_cache.hpp"
#include "ipc/shm/arena_lend/jemalloc/memory_manager.hpp"
#include "ipc/shm/arena_lend/jemalloc/detail/jemalloc.hpp"
#include "ipc/test/test_logger.hpp"
#include <flow/async/single_thread_task_loop.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <vector>

namespace ipc::shm::arena_lend::jemalloc::test
{

namespace
{

using ipc::test::Test_logger;
using flow::async::Single_thread_task_loop;
using flow::async::Synchronicity;
using flow::util::Thread_token;
using uint = unsigned int;

// Runs task() in the given loop's thread and returns once it has completed.
template<typename Task>
void post_wait(Single_thread_task_loop* loop, Task&& task)
{
  loop->post(std::forward<Task>(task), Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION);
}

/// Google test fixture: brackets Thread_cache's process-wide logger around each test.
class Thread_cache_test :
  public ::testing::Test
{
public:
  Thread_cache_test() :
    m_logger(flow::log::Sev::S_INFO)
  {
    Thread_cache::caches_set_logger(&m_logger);
  }

  ~Thread_cache_test() override
  {
    // Must remember to unset, so as to not mess over subsequent tests (the logger is process-wide state).
    Thread_cache::caches_set_logger(nullptr);
  }

  /**
   * The logger, e.g. for worker loops.
   * @return See above.
   */
  flow::log::Logger* get_logger()
  {
    return &m_logger;
  }

private:
  /// Used for logging to the console.
  Test_logger m_logger;
}; // class Thread_cache_test

} // Anonymous namespace

/* The thread-local-state semantics, on a virgin worker thread (the main thread's state may have been
 * created long ago by other suites in this binary -- a fresh thread gives us the from-nothing view):
 * or-null/or-none never create; the creating calls create exactly once and are memoized thereafter.
 * Uses only S_NO_ARENA_ID -- the one arena-ID value requiring no actual arena -- as the tcache key;
 * real-arena keying is covered in the other TESTs. */
TEST_F(Thread_cache_test, Laziness_and_memoization)
{
  Single_thread_task_loop worker{get_logger(), "tcache1"};
  worker.start();
  post_wait(&worker, []()
  {
    // Virgin thread: no cache-object exists until this_thread_cache(); or-null must not create one.
    EXPECT_EQ(Thread_cache::this_thread_cache_or_null(), nullptr);
    EXPECT_EQ(Thread_cache::this_thread_cache_or_null(), nullptr); // (And it did not create one just now.)

    const auto cache = Thread_cache::this_thread_cache(); // Creates.
    ASSERT_NE(cache, nullptr);
    EXPECT_EQ(Thread_cache::this_thread_cache(), cache); // Memoized.
    EXPECT_EQ(Thread_cache::this_thread_cache_or_null(), cache); // Now it reports the same one.

    // Ditto one level down: no tcache for a given arena-ID key until id(); id_or_none() must not create.
    EXPECT_EQ(cache->id_or_none(Thread_cache::S_NO_ARENA_ID), Thread_cache::S_NO_TCACHE_ID);
    EXPECT_EQ(cache->id_or_none(Thread_cache::S_NO_ARENA_ID), Thread_cache::S_NO_TCACHE_ID);

    const auto tcache_id = cache->id(Thread_cache::S_NO_ARENA_ID); // Creates (mallctl tcache.create inside).
    EXPECT_NE(tcache_id, Thread_cache::S_NO_TCACHE_ID);
    EXPECT_EQ(cache->id(Thread_cache::S_NO_ARENA_ID), tcache_id); // Memoized.
    EXPECT_EQ(cache->id_or_none(Thread_cache::S_NO_ARENA_ID), tcache_id);
  });
  worker.stop();
  /* Worker exit runs ~Thread_cache (best-effort flush + destroy of its tcaches); nothing to assert, but its
   * not blowing up is part of the point. */
} // TEST_F(Thread_cache_test, Laziness_and_memoization)

/* Distinct threads: each gets its own cache-object with its own thread_token(); their same-key tcaches are
 * created independently.  (Note: we do not compare tcache ID *values* across threads -- whether jemalloc
 * recycles numeric IDs across threads' tcache.create calls is not in any contract.  Independence is asserted
 * via the cache-objects and tokens; and via each thread's from-nothing id_or_none() answer.) */
TEST_F(Thread_cache_test, Thread_independence)
{
  Thread_cache* cache_1 = nullptr;
  Thread_cache* cache_2 = nullptr;
  Thread_token token_1;
  Thread_token token_2;

  Single_thread_task_loop worker_1{get_logger(), "tcache1"};
  Single_thread_task_loop worker_2{get_logger(), "tcache2"};
  worker_1.start();
  worker_2.start();

  post_wait(&worker_1, [&]()
  {
    cache_1 = Thread_cache::this_thread_cache();
    token_1 = cache_1->thread_token();
    EXPECT_FALSE(cache_1->thread_nickname().empty()); // The loop nicknamed this thread; it should show here.
    cache_1->id(Thread_cache::S_NO_ARENA_ID);
  });
  post_wait(&worker_2, [&]()
  {
    cache_2 = Thread_cache::this_thread_cache();
    token_2 = cache_2->thread_token();

    // Fully independent of worker_1's identically-keyed state:
    EXPECT_NE(cache_2, cache_1);
    EXPECT_EQ(cache_2->id_or_none(Thread_cache::S_NO_ARENA_ID), Thread_cache::S_NO_TCACHE_ID);
    cache_2->id(Thread_cache::S_NO_ARENA_ID);
  });

  EXPECT_NE(token_1, token_2);

  worker_1.stop();
  worker_2.stop();
} // TEST_F(Thread_cache_test, Thread_independence)

/* The core promise, tested at its reason for existing.  Recall (class doc header): jemalloc seeds a tcache
 * both from arena feedings *and from deallocations through it* -- and a tcache neither knows nor cares which
 * arena each cached pointer came from.  So a tcache shared between arenas can satisfy an
 * allocate-from-arena-B with arena-A memory.  Thread_cache's 1-to-1 tcache-per-arena mapping is the guard;
 * here we drive real allocation traffic through the guarded setup and verify -- via
 * mallctl("arenas.lookup"), which reports the arena that owns a given pointer -- that each allocation
 * originates from the arena it was requested from.  The dangerous moment is specifically wave 2: by then
 * each tcache has been seeded by wave 1's deallocations, so a shared tcache would be handing back a 50/50
 * arena mix.  Also included: distinctness of the vended tcache IDs per key (the mapping's other face); and a
 * mid-stream flush_tcache() -- the tcache re-seeds from its arena afterward, originating arenas still
 * correct. */
TEST_F(Thread_cache_test, Per_arena_tcaches)
{
  using std::vector;

  // Small size => a small jemalloc size-class => plainly tcache-eligible.
  constexpr size_t S_ALLOC_SZ = 64;
  constexpr uint S_N_PER_ARENA = 16;

  Memory_manager mem_manager;
  // Null extent-hooks => jemalloc's defaults => plain heap arenas (fine: nothing here is SHM-specific).
  const auto arena_a = mem_manager.create_arena(nullptr);
  const auto arena_b = mem_manager.create_arena(nullptr);
  ASSERT_NE(arena_a, arena_b);

  // jemalloc's own answer to: which arena owns this pointer?
  const auto arena_of = [](void* address) -> arena_id_t
  {
    uint arena_ind = 0;
    size_t out_sz = sizeof(arena_ind);
    const auto err_ret
      = IPC_SHM_ARENA_LEND_JEMALLOC_API(mallctl)("arenas.lookup", &arena_ind, &out_sz, &address, sizeof(address));
    EXPECT_EQ(err_ret, 0);
    return arena_id_t(arena_ind);
  };

  Single_thread_task_loop worker{get_logger(), "tcache1"};
  worker.start();
  post_wait(&worker, [&]()
  {
    const auto cache = Thread_cache::this_thread_cache();
    const auto tcache_a = cache->id(arena_a);
    const auto tcache_b = cache->id(arena_b);
    const auto tcache_no_arena = cache->id(Thread_cache::S_NO_ARENA_ID);
    // Distinct keys => distinct tcaches (the mapping's ID-level face).
    EXPECT_NE(tcache_a, tcache_b);
    EXPECT_NE(tcache_a, tcache_no_arena);
    EXPECT_NE(tcache_b, tcache_no_arena);

    const auto alloc_wave = [&](vector<void*>* ptrs_a, vector<void*>* ptrs_b)
    {
      for (uint idx = 0; idx != S_N_PER_ARENA; ++idx) // Alternate arenas within the wave.
      {
        ptrs_a->push_back(mem_manager.allocate(S_ALLOC_SZ, arena_a, tcache_a));
        ptrs_b->push_back(mem_manager.allocate(S_ALLOC_SZ, arena_b, tcache_b));
      }
      for (uint idx = 0; idx != S_N_PER_ARENA; ++idx)
      {
        ASSERT_TRUE((*ptrs_a)[idx]);
        ASSERT_TRUE((*ptrs_b)[idx]);
        EXPECT_EQ(arena_of((*ptrs_a)[idx]), arena_a); // The originating-arena check.
        EXPECT_EQ(arena_of((*ptrs_b)[idx]), arena_b);
      }
    };
    const auto dealloc_wave = [&](const vector<void*>& ptrs_a, const vector<void*>& ptrs_b)
    {
      for (uint idx = 0; idx != S_N_PER_ARENA; ++idx) // Interleaved, for maximum seeding-mixture.
      {
        mem_manager.deallocate(ptrs_a[idx], arena_a, tcache_a);
        mem_manager.deallocate(ptrs_b[idx], arena_b, tcache_b);
      }
    };

    // Wave 1: allocations feed from the arenas (seeding the tcaches as a side effect); origins verified.
    vector<void*> ptrs_a;
    vector<void*> ptrs_b;
    alloc_wave(&ptrs_a, &ptrs_b);
    // Return everything through the per-arena tcaches: now each tcache is full of its arena's pointers.
    dealloc_wave(ptrs_a, ptrs_b);

    // Wave 2 -- the moment a shared tcache would go wrong: these mostly hit the caches.  Origins verified.
    ptrs_a.clear();
    ptrs_b.clear();
    alloc_wave(&ptrs_a, &ptrs_b);

    // Mid-stream flush is legal; the tcache re-seeds from its own arena thereafter.
    cache->flush_tcache(tcache_a);
    auto* const post_flush_ptr = mem_manager.allocate(S_ALLOC_SZ, arena_a, tcache_a);
    ASSERT_TRUE(post_flush_ptr);
    EXPECT_EQ(arena_of(post_flush_ptr), arena_a);
    mem_manager.deallocate(post_flush_ptr, arena_a, tcache_a);

    dealloc_wave(ptrs_a, ptrs_b);
  });
  worker.stop();

  /* Cleanup, not destroy-choreography testing (that deserves -- and gets -- its own coverage): the worker,
   * and with it the only tcaches for these arenas, is gone (thread exit destroyed them); so this destroys
   * each arena immediately and synchronously. */
  const flow::log::Log_context_mt log_ctx{get_logger(), Log_component::S_TEST};
  mem_manager.destroy_arena(arena_a, &log_ctx);
  mem_manager.destroy_arena(arena_b, &log_ctx);
} // TEST_F(Thread_cache_test, Per_arena_tcaches)

/* destroy_arena_safely(), the synchronous cases.  (a) The calling thread holds the only tcache for the
 * arena: per contract that one can be flushed/destroyed on the spot, so everything happens before the call
 * returns; on_done_func receives the caller's own log_ctx (the async case instead substitutes an internal
 * one -- the caller's may be dead by then; the other TEST covers that) plus the F() whose invocation is what
 * actually destroys the arena.  (b) The documented failure mode: for an arena jemalloc knows nothing about,
 * F() throws Runtime_error. */
TEST_F(Thread_cache_test, Destroy_arena_synchronous)
{
  const flow::log::Log_context_mt log_ctx{get_logger(), Log_component::S_TEST};

  Memory_manager mem_manager;
  const auto arena_id = mem_manager.create_arena(nullptr);

  // Give the calling thread a real, seeded tcache for the arena, so the flush/destroy work is genuine.
  const auto tcache_id = Thread_cache::this_thread_cache()->id(arena_id);
  auto* const ptr = mem_manager.allocate(64, arena_id, tcache_id);
  ASSERT_TRUE(ptr);
  mem_manager.deallocate(ptr, arena_id, tcache_id);

  bool done = false;
  Thread_cache::destroy_arena_safely(arena_id, &log_ctx,
                                     [&](const flow::log::Log_context_mt* got_log_ctx, auto&& dtor_func)
  {
    EXPECT_EQ(got_log_ctx, &log_ctx); // Synchronous invocation relays the caller's own log_ctx.
    EXPECT_NO_THROW(dtor_func());
    done = true;
  });
  EXPECT_TRUE(done); // I.e., all of the above indeed happened synchronously.

  bool bogus_done = false;
  Thread_cache::destroy_arena_safely(arena_id_t(9999), &log_ctx,
                                     [&](const flow::log::Log_context_mt*, auto&& dtor_func)
  {
    EXPECT_THROW(dtor_func(), flow::error::Runtime_error);
    bogus_done = true;
  });
  EXPECT_TRUE(bogus_done);
} // TEST_F(Thread_cache_test, Destroy_arena_synchronous)

/* destroy_arena_safely(), the deferred case, exercising both documented finalization triggers.  Two workers
 * hold the only tcaches for the arena, so the destruction request (from main, which holds none) cannot run
 * immediately.  Worker 1 then crosses itself off via the opportunistic path -- any this_thread_cache*()
 * call -- but worker 2 still holds out, so the arena lives on.  Finally worker 2 exits; its ~Thread_cache is
 * the last cross-off and so finalizes: arena destroyed; on_done_func run -- in that exiting thread, hence
 * (per contract) with the internal caches_set_logger()-consistent log_ctx, not the requester's.
 * (The request goes through Memory_manager::destroy_arena(), per the @warning's recommendation to keep
 * arena create/destroy/stats ops within its interlocked family; it forwards to destroy_arena_safely().) */
TEST_F(Thread_cache_test, Destroy_arena_deferred)
{
  const flow::log::Log_context_mt log_ctx{get_logger(), Log_component::S_TEST};

  Memory_manager mem_manager;
  const auto arena_id = mem_manager.create_arena(nullptr);

  Single_thread_task_loop worker_1{get_logger(), "tcache1"};
  Single_thread_task_loop worker_2{get_logger(), "tcache2"};
  worker_1.start();
  worker_2.start();

  const auto make_seeded_tcache = [&]()
  {
    const auto tcache_id = Thread_cache::this_thread_cache()->id(arena_id);
    auto* const ptr = mem_manager.allocate(64, arena_id, tcache_id);
    ASSERT_TRUE(ptr);
    mem_manager.deallocate(ptr, arena_id, tcache_id);
  };
  post_wait(&worker_1, make_seeded_tcache);
  post_wait(&worker_2, make_seeded_tcache);

  std::atomic<bool> done{false};
  mem_manager.destroy_arena(arena_id, &log_ctx,
                            [&](const flow::log::Log_context_mt* got_log_ctx, auto&& dtor_func)
  {
    EXPECT_NE(got_log_ctx, nullptr);
    EXPECT_NE(got_log_ctx, &log_ctx); // Asynchronous invocation substitutes the internal log_ctx.
    EXPECT_NO_THROW(dtor_func());
    done = true;
  });
  EXPECT_FALSE(done); // Two holdout threads: nothing could have happened yet.

  // Finalization trigger 1 (opportunistic): any this_thread_cache*() call in a holdout thread...
  post_wait(&worker_1, []() { Thread_cache::this_thread_cache_or_null(); });
  EXPECT_FALSE(done); // ...but worker 2 still holds out, so the arena must still be alive.

  // Worker 1, meanwhile, is unharmed: unrelated tcache work proceeds.
  post_wait(&worker_1, []() { Thread_cache::this_thread_cache()->id(Thread_cache::S_NO_ARENA_ID); });

  // Finalization trigger 2 (thread exit): worker 2's ~Thread_cache is the last cross-off.
  worker_2.stop();
  EXPECT_TRUE(done);

  worker_1.stop();
} // TEST_F(Thread_cache_test, Destroy_arena_deferred)

} // namespace ipc::shm::arena_lend::jemalloc::test
