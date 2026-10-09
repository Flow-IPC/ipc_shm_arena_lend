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

#include "ipc/shm/arena_lend/jemalloc/ipc_arena.hpp"
#include "ipc/shm/arena_lend/jemalloc/memory_manager.hpp"
#include "ipc/shm/arena_lend/detail/use_count_registry.hpp"
#include "ipc/shm/arena_lend/test/test_shm_object.hpp"
#include "ipc/test/test_logger.hpp"
#include "ipc/util/util_fwd.hpp"
#include <flow/async/single_thread_task_loop.hpp>
#include <flow/common.hpp>
#include <flow/error/error.hpp>
#include <flow/log/log.hpp>
#include <gtest/gtest.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>
#include <array>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>

/* Tests of SHM-jemalloc Ipc_arena::allocate() and construct<T>() behavior on failure:
 *   - allocation failure (allocate() and, hence, construct<T>()) => std::bad_alloc;
 *   - T's ctor throws => it propagates; the allocation is undone;
 *   - this thread's lend-tracker (aux) SHM-pool runs out of use-count slots => std::bad_alloc;
 *   - this thread's lend-tracker SHM-pool cannot be created => flow::error::Runtime_error;
 * and in each case the arena is left as it was (no leaked buffer/object; still fully usable).
 *
 * As in ipc_arena_stats_test.cpp: construct()ing is done in spawned Single_thread_task_loop threads, not the main
 * thread (whose thread-local SHM-jemalloc state would otherwise linger beyond the TEST).  Tasks are posted with
 * wait-for-completion; nothing races. */

#ifndef FLOW_OS_LINUX
static_assert(false, "Some of these tests rely on platform specifics, tested only in Linux: capping RLIMIT_NOFILE "
                       "at the lowest unused descriptor number to make the next descriptor-opening fail (EMFILE); "
                       "and jemalloc's largest size class on 64-bit (for a request jemalloc rejects outright).  "
                       "These are POSIX/jemalloc things, so they may well work elsewhere (e.g., macOS); but they "
                       "need checking when porting.");
#endif

namespace ipc::shm::arena_lend::jemalloc::test
{

namespace
{

using flow::async::Single_thread_task_loop;
using flow::log::Logger;

/* Always-on console logger for test-progress output (FLOW_LOG_INFO etc.).
 * Survives across all TESTs in this TU; object internals use `g_logger` (toggleable) instead. */
ipc::test::Test_logger g_logger_obj;
Logger* const g_logger_console = &g_logger_obj;
#if 1
Logger* const g_logger = nullptr; // Normal: Flow-IPC objects silent.
#else
Logger* const g_logger = &g_logger_obj; // Flip for debugging.
#endif

/* A request size jemalloc rejects outright: beyond its largest size class (as of jemalloc-5.3.0 on 64-bit:
 * 2^62 + 3 * 2^60 = 7 * 2^60), yet within what a C++ object type may have (at most PTRDIFF_MAX ~= 2^63); so it is
 * usable for allocate() and construct<T>() alike.  (Merely huge requests -- e.g., 1Ti -- would succeed, as pools are
 * sparse; so this is essentially the one allocation failure triggerable in a test.)  Here: 7.5 * 2^60. */
constexpr size_t ABSURD_SZ = size_t(15) << 59;

// An object type whose ctor throws on request.  Sized to be a large allocation (not served by jemalloc thread-cache).
struct Thrower
{
  explicit Thrower(bool do_throw)
  {
    if (do_throw)
    {
      throw std::runtime_error{"Thrower ctor threw as requested."};
    }
  }
  std::array<uint8_t, 1024 * 1024> m_data;
};

// Posts `task` onto `loop` and waits for it to complete.
template<typename Task>
void post_wait(Single_thread_task_loop* loop, Task&& task)
{
  loop->post(std::forward<Task>(task), flow::async::Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION);
}

std::shared_ptr<Ipc_arena> make_arena()
{
  return Ipc_arena::create(g_logger, std::make_shared<Memory_manager>(),
                           arena_lend::test::create_test_pool_name_base(),
                           util::shared_resource_permissions(util::Permissions_level::S_USER_ACCESS));
}

// Live-object count and construct count, from the arena's sharded stats.
struct Obj_counts
{
  uint64_t m_live;
  uint64_t m_constructs;
};

Obj_counts obj_counts(Ipc_arena* arena)
{
  Ipc_arena::Sharded_stats stats;
  arena->sharded_stats(&stats);
  return { stats.m_live_obj.m_live_objects.load(), stats.m_owner_obj.m_construct_count.load() };
}

} // namespace (anon)

/* An absurdly large request: allocate() and construct<T>() throw std::bad_alloc; jemalloc rejects it outright, so
 * no SHM-pool is even attempted; and the arena works on. */
TEST(Ipc_arena_construct_test, Bad_alloc)
{
  using Absurd = std::array<uint8_t, ABSURD_SZ>;

  const auto arena = make_arena();
  Single_thread_task_loop loop{g_logger, "ictBadAlc"};
  loop.start();

  post_wait(&loop, [&]()
  {
    const auto n_pools_0 = arena->pool_stats().m_owner_pool.m_pool_create_count.load();
    const auto counts_0 = obj_counts(arena.get());

    EXPECT_THROW(arena->allocate(ABSURD_SZ), std::bad_alloc);
    EXPECT_THROW(arena->construct<Absurd>(), std::bad_alloc);

    EXPECT_EQ(arena->pool_stats().m_owner_pool.m_pool_create_count.load(), n_pools_0);
    EXPECT_EQ(obj_counts(arena.get()).m_live, counts_0.m_live);
    EXPECT_EQ(obj_counts(arena.get()).m_constructs, counts_0.m_constructs);

    auto obj = arena->construct<uint64_t>(42); // Sanity: business as usual.
    ASSERT_TRUE(obj);
    EXPECT_EQ(*obj, 42u);
  });
} // TEST(Ipc_arena_construct_test, Bad_alloc)

/* construct<T>(): if T's ctor throws, the exception propagates, the object is not counted, and the allocation is
 * undone.  The latter is checked by repetition: were each failed (1Mi) allocation leaked, the arena's SHM-pools would
 * have to grow by Gi bytes; as they are freed, the same memory is reused instead. */
TEST(Ipc_arena_construct_test, Ctor_throws)
{
  constexpr size_t N_TRIES = 2000;
  constexpr size_t MAX_POOL_GROWTH = 256 * 1024 * 1024; // Far below N_TRIES * sizeof(Thrower) ~= 2Gi.

  const auto arena = make_arena();
  Single_thread_task_loop loop{g_logger, "ictCtrThr"};
  loop.start();

  post_wait(&loop, [&]()
  {
    auto warm_up = arena->construct<Thrower>(false); // Get the initial SHM-pool(s) etc. in place first.
    ASSERT_TRUE(warm_up);
    warm_up.reset();

    const auto pools_sz_0 = arena->pool_stats().m_owner_pool.m_pool_create_sz.load();
    const auto counts_0 = obj_counts(arena.get());

    for (size_t idx = 0; idx != N_TRIES; ++idx)
    {
      EXPECT_THROW(arena->construct<Thrower>(true), std::runtime_error);
    }

    EXPECT_LT(arena->pool_stats().m_owner_pool.m_pool_create_sz.load() - pools_sz_0, MAX_POOL_GROWTH)
      << "Failed construct()s' allocations appear to have leaked.";
    EXPECT_EQ(obj_counts(arena.get()).m_live, counts_0.m_live);
    EXPECT_EQ(obj_counts(arena.get()).m_constructs, counts_0.m_constructs);

    auto obj = arena->construct<Thrower>(false); // Sanity: business as usual.
    ASSERT_TRUE(obj);
    EXPECT_EQ(obj_counts(arena.get()).m_live, counts_0.m_live + 1);
  });
} // TEST(Ipc_arena_construct_test, Ctor_throws)

/* This thread's lend-tracker SHM-pool (one per thread per arena) holds a fixed number of use-count slots, one per
 * live construct()ed object.  Construct objects until that runs out: then construct<T>() throws std::bad_alloc,
 * having cleaned up its object; the arena remains usable; and freeing one object frees a slot for the next.
 *
 * This creates ~1Mi objects (with ~1Mi Handles in heap): ~10 seconds at -O0 on a fast machine; slower under
 * sanitizers (estimated ~1 minute under thread-sanitizer) which we deem acceptable. */
TEST(Ipc_arena_construct_test, Lend_tracker_slots_exhausted)
{
  using std::vector;
  using Use_count_registry = arena_lend::detail::Use_count_registry;

  constexpr size_t CAPACITY = Use_count_registry::S_USE_COUNTS_CAPACITY;
  // The lend-tracker header occupies the first few slots (64 as of this writing); allow for those, with slack.
  constexpr size_t MAX_UNUSABLE_SLOTS = 128;

  FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);

  const auto arena = make_arena();
  Single_thread_task_loop loop{g_logger, "ictLtSlot"};
  loop.start();

  post_wait(&loop, [&]()
  {
    vector<Ipc_arena::Handle<uint64_t>> objs;
    objs.reserve(CAPACITY);
    bool ran_out = false;
    while (!ran_out)
    {
      try
      {
        objs.emplace_back(arena->construct<uint64_t>(objs.size()));
        ASSERT_LE(objs.size(), CAPACITY) << "Constructed more objects than there are use-count slots?";
      }
      catch (const std::bad_alloc&)
      {
        ran_out = true;
      }
    }
    FLOW_LOG_INFO("Ran out of lend-tracker use-count slots after [" << objs.size() << "] live objects "
                  "(capacity [" << CAPACITY << "]).");
    EXPECT_GE(objs.size(), CAPACITY - MAX_UNUSABLE_SLOTS);
    EXPECT_EQ(obj_counts(arena.get()).m_live, objs.size()) << "The failed object should not be counted.";

    EXPECT_THROW(arena->construct<uint64_t>(0), std::bad_alloc); // Still out.

    objs.pop_back(); // Frees a slot...
    auto obj = arena->construct<uint64_t>(42); // ...so this works.
    ASSERT_TRUE(obj);
    EXPECT_EQ(*obj, 42u);

    obj.reset();
    objs.clear();
    EXPECT_EQ(obj_counts(arena.get()).m_live, 0u);
  });
} // TEST(Ipc_arena_construct_test, Lend_tracker_slots_exhausted)

/* A thread's first construct() in an arena creates that thread's lend-tracker SHM-pool for the arena; if that fails,
 * construct<T>() throws flow::error::Runtime_error, having cleaned up its object; and a later attempt can succeed.
 * We make the creation fail by temporarily capping the process's open-file-descriptor limit at the current usage
 * (creating a SHM-pool needs a descriptor).  The arena's main SHM-pool is set up beforehand (in another thread), so
 * that the object itself is allocated without needing a new descriptor.
 *
 * Note: The descriptor limit is process-wide; so for the moment nothing else in the process can open anything.
 * Fine in this (sequential) test suite. */
TEST(Ipc_arena_construct_test, Lend_tracker_pool_creation_fails)
{
  const auto arena = make_arena();
  Single_thread_task_loop loop_warm{g_logger, "ictLtWarm"};
  Single_thread_task_loop loop_fresh{g_logger, "ictLtFrsh"};
  loop_warm.start();
  loop_fresh.start(); // Started now: its own setup may need descriptors.

  Ipc_arena::Handle<uint64_t> warm_obj;
  post_wait(&loop_warm, [&]() { warm_obj = arena->construct<uint64_t>(1); });
  ASSERT_TRUE(warm_obj);
  const auto counts_0 = obj_counts(arena.get());

  // Cap the descriptor limit at the lowest unused descriptor number: then no new descriptor can be opened.
  const int lowest_free_fd = ::open("/dev/null", O_RDONLY);
  ASSERT_GE(lowest_free_fd, 0);
  ::close(lowest_free_fd);
  struct ::rlimit limit_orig;
  ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &limit_orig), 0);
  auto limit_capped = limit_orig;
  limit_capped.rlim_cur = rlim_t(lowest_free_fd);
  ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &limit_capped), 0);

  post_wait(&loop_fresh, [&]()
  {
    EXPECT_THROW(arena->construct<uint64_t>(2), flow::error::Runtime_error);
  });

  ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &limit_orig), 0);

  EXPECT_EQ(obj_counts(arena.get()).m_live, counts_0.m_live) << "The failed object should not be counted.";
  EXPECT_EQ(obj_counts(arena.get()).m_constructs, counts_0.m_constructs);

  post_wait(&loop_fresh, [&]() // With descriptors available again, the same thread succeeds.
  {
    auto obj = arena->construct<uint64_t>(3);
    ASSERT_TRUE(obj);
    EXPECT_EQ(*obj, 3u);
  });
  post_wait(&loop_warm, [&]() { warm_obj.reset(); });
} // TEST(Ipc_arena_construct_test, Lend_tracker_pool_creation_fails)

} // namespace ipc::shm::arena_lend::jemalloc::test
