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

/* Unit test of detail::Owner_shm_pool_repository: the owner-side singleton providing the
 * high-performance forward (pool-ID + offset -> vaddr) and reverse (vaddr -> pool-ID + offset) lookups that
 * `Shm_pool_offset_ptr`s lean on.  Notes on scope:
 *   - We instantiate the repository template with a test-local tag type: same real machinery -- canonical
 *     maps, per-thread forward caches, the shared reverse-lookup engine (Shm_pool_repo_lookup_core_rev +
 *     Thread_local_pool_lookup_rev) with its push-updated per-thread caches -- but a fresh instance fully
 *     isolated from the production (per-`Ipc_arena`) instantiation.  No jemalloc/sessions needed; and the
 *     repository never dereferences pool addresses (Shm_pool is a passive record), so pool specimens use
 *     fake vaddrs.
 *   - Contract-excluded (hence untested): to_address() of an unknown/erased pool ID (undefined behavior --
 *     the per-thread forward caches are deliberately never pruned, which is sound because pool IDs are
 *     never reused; see class doc header); duplicate/re-insertion (violates the same invariants).
 *   - The reverse-lookup engine is shared with the borrower side but parameterized differently there
 *     (S_LOOKUP_CAN_FAIL = false there; `true` -- with the miss => `pool_id = 0` branch -- here); we test the
 *     owner-side parameterization on purpose and leave borrower-side coverage to its own suite. */

#include <gtest/gtest.h>
#include "ipc/shm/arena_lend/detail/owner_shm_pool_repository.hpp"
#include "ipc/shm/arena_lend/shm_pool.hpp"
#include "ipc/test/test_logger.hpp"
#include <flow/async/single_thread_task_loop.hpp>
#include <memory>

namespace ipc::shm::arena_lend::test
{

namespace
{

using flow::async::Single_thread_task_loop;
using flow::async::Synchronicity;
using std::make_shared;
using std::shared_ptr;

/* The test-local arena tag: gives us a fresh, isolated instantiation of the real repository machinery
 * (see file-level comment). */
struct Owner_repo_test_arena {};
using Repository = detail::Owner_shm_pool_repository<Owner_repo_test_arena>;

using pool_id_t = Shm_pool::pool_id_t;
using pool_offset_t = Shm_pool::size_t;

// Runs task() in the given loop's thread and returns once it has completed.
template<typename Task>
void post_wait(Single_thread_task_loop* loop, Task&& task)
{
  loop->post(std::forward<Task>(task), Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION);
}

// Two pools with a gap between them; fake vaddrs (never dereferenced); page-ish sizes.
constexpr pool_id_t S_ID_1 = 11;
constexpr pool_id_t S_ID_2 = 22;
constexpr pool_id_t S_ID_3 = 33; // (For the address-reuse scenario in TEST Mutation_visibility.)
char* const S_BASE_1 = reinterpret_cast<char*>(0x10000);
constexpr size_t S_SIZE_1 = 0x1000;
char* const S_BASE_2 = reinterpret_cast<char*>(0x20000); // Well past S_BASE_1 + S_SIZE_1: a gap exists.
constexpr size_t S_SIZE_2 = 0x2000;
constexpr int S_FAKE_FD = 12; // Shm_pool stores it; nothing in these tests operates on it.

shared_ptr<Shm_pool> make_pool(pool_id_t id, char* base, size_t size)
{
  return make_shared<Shm_pool>(id, "owner_shm_pool_repository_test_" + std::to_string(id), base, size, S_FAKE_FD);
}

// The rev-lookup checks, usable from any thread: from_address(addr) must yield (exp_id, exp_offset).
void check_rev(const void* address, pool_id_t exp_id, pool_offset_t exp_offset)
{
  pool_id_t pool_id;
  pool_offset_t pool_offset = 0; // (Init to avoid false warnings with some compilers.)
  Repository::from_address(address, pool_id, pool_offset);
  EXPECT_EQ(pool_id, exp_id) << "from_address([" << address << "]).";
  if (exp_id != 0) // On a miss only pool_id (= 0) is meaningful.
  {
    EXPECT_EQ(pool_offset, exp_offset) << "from_address([" << address << "]).";
  }
}

// Ditto for a miss (address in no pool).
void check_rev_miss(const void* address)
{
  check_rev(address, 0, 0);
}

} // Anonymous namespace

/* The core lookups, in one thread and then from a second thread (which gets its own per-thread caches --
 * same answers): forward round-trips including the cached fast-path; reverse-lookup geometry around 2 pools
 * with a gap (before/at-base/inside/last-byte/one-past-end/in-gap/past-everything, plus null). */
TEST(Owner_shm_pool_repository_test, Lookups)
{
  auto& repository = Repository::get_instance();
  repository.insert(make_pool(S_ID_1, S_BASE_1, S_SIZE_1));
  repository.insert(make_pool(S_ID_2, S_BASE_2, S_SIZE_2));

  const auto check_all = [&]()
  {
    // Forward: twice each -- the first call in a thread populates its cache (slow path), the second hits it.
    for (int round = 0; round != 2; ++round)
    {
      EXPECT_EQ(Repository::to_address(S_ID_1, 0), S_BASE_1);
      EXPECT_EQ(Repository::to_address(S_ID_1, 0x8), S_BASE_1 + 0x8);
      EXPECT_EQ(Repository::to_address(S_ID_1, pool_offset_t(S_SIZE_1 - 1)), S_BASE_1 + (S_SIZE_1 - 1));
      EXPECT_EQ(Repository::to_address(S_ID_2, 0), S_BASE_2);
      EXPECT_EQ(Repository::to_address(S_ID_2, pool_offset_t(S_SIZE_2 - 1)), S_BASE_2 + (S_SIZE_2 - 1));
    }

    // Reverse: the geometry.
    check_rev_miss(nullptr);
    check_rev_miss(S_BASE_1 - 1); // Before everything.
    check_rev(S_BASE_1, S_ID_1, 0); // At base.
    check_rev(S_BASE_1 + 0x8, S_ID_1, 0x8); // Inside.
    check_rev(S_BASE_1 + (S_SIZE_1 - 1), S_ID_1, pool_offset_t(S_SIZE_1 - 1)); // Last byte.
    check_rev_miss(S_BASE_1 + S_SIZE_1); // One-past-end = in the gap.
    check_rev_miss(S_BASE_2 - 1); // Still in the gap.
    check_rev(S_BASE_2, S_ID_2, 0);
    check_rev(S_BASE_2 + 0x123, S_ID_2, 0x123);
    check_rev_miss(S_BASE_2 + S_SIZE_2); // Past everything.
  };

  check_all(); // This thread.

  ipc::test::Test_logger logger;
  Single_thread_task_loop worker{&logger, "ownRepo1"};
  worker.start();
  post_wait(&worker, check_all); // A fresh thread: its own per-thread caches; identical truth.
  worker.stop();

  repository.erase(S_ID_1);
  repository.erase(S_ID_2);
  check_rev_miss(S_BASE_1); // (Erase visibility in detail: next TEST.)
}

/* Mutation visibility across *already-active* threads -- the push-update machinery: a worker's per-thread
 * reverse cache, created before a pool's insert()/erase(), must see the change on its next lookup.  Plus the
 * address-reuse scenario: erase pool 1, insert pool 3 (different ID) at pool 1's old base -- lookups must
 * resolve to pool 3 cleanly (this is the erase-bookkeeping-before-vaddr-reuse invariant's user-visible
 * face; see Owner_shm_pool_collection::remove_shm_pool() comment). */
TEST(Owner_shm_pool_repository_test, Mutation_visibility)
{
  auto& repository = Repository::get_instance();
  ipc::test::Test_logger logger;
  Single_thread_task_loop worker{&logger, "ownRepo2"};
  worker.start();

  repository.insert(make_pool(S_ID_1, S_BASE_1, S_SIZE_1));
  post_wait(&worker, [&]() { check_rev(S_BASE_1, S_ID_1, 0); }); // Worker's rev cache is born here.

  repository.insert(make_pool(S_ID_2, S_BASE_2, S_SIZE_2)); // Push-updates all extant per-thread caches...
  post_wait(&worker, [&]() { check_rev(S_BASE_2, S_ID_2, 0); }); // ...including the worker's.

  repository.erase(S_ID_1); // Ditto erasure.
  post_wait(&worker, [&]() { check_rev_miss(S_BASE_1); });
  check_rev_miss(S_BASE_1);

  // Address reuse: a new pool (new ID -- IDs never recur, addresses can) at pool 1's old base.
  repository.insert(make_pool(S_ID_3, S_BASE_1, S_SIZE_1));
  EXPECT_EQ(Repository::to_address(S_ID_3, 0), S_BASE_1);
  check_rev(S_BASE_1, S_ID_3, 0);
  post_wait(&worker, [&]() { check_rev(S_BASE_1 + 0x8, S_ID_3, 0x8); });

  worker.stop(); // (Thread death destroys its per-thread caches; count-gauges for that live in stats suites.)

  repository.erase(S_ID_2);
  repository.erase(S_ID_3);
  check_rev_miss(S_BASE_1);
  check_rev_miss(S_BASE_2);
}

} // namespace ipc::shm::arena_lend::test
