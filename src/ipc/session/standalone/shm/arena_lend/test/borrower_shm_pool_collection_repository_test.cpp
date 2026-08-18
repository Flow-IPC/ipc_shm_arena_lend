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

#include <gtest/gtest.h>
#include "ipc/session/standalone/shm/arena_lend/detail/borrower_shm_pool_collection_repository.hpp"
#include "ipc/shm/arena_lend/borrower_shm_pool_collection.hpp"
#include "ipc/shm/arena_lend/jemalloc/ipc_arena.hpp"
#include "ipc/shm/arena_lend/jemalloc/jemalloc_pages.hpp"
#include "ipc/test/test_logger.hpp"
#include "ipc/shm/arena_lend/test/test_shm_object.hpp"
#include "ipc/shm/arena_lend/test/test_shm_pool_collection.hpp"
#include <flow/async/single_thread_task_loop.hpp>

using ipc::test::Test_logger;
using std::make_shared;

using ipc::shm::arena_lend::Shared_name;
using ipc::shm::arena_lend::test::Test_shm_pool_collection;
using ipc::shm::arena_lend::test::create_test_pool_name_base;
using ipc::shm::arena_lend::test::ensure_empty_collection_at_destruction;

namespace ipc::session::shm::arena_lend::test
{

namespace
{
using Repository = detail::Borrower_shm_pool_collection_repository<ipc::shm::arena_lend::jemalloc::Ipc_arena>;
using pool_id_t = ipc::shm::arena_lend::Borrower_shm_pool_collection::pool_id_t;
using detail::owner_id_t;
using detail::collection_id_t;

const owner_id_t OWNER_ID_0 = 10;
const owner_id_t OWNER_ID_1 = 20;
const owner_id_t OWNER_ID_2 = 30; // Used by the lookup-oriented TESTs only (state-independence from the others).
const collection_id_t COLLECTION_ID_0 = 1;
const collection_id_t COLLECTION_ID_1 = 2;

// Runs task() in the given loop's thread and returns once it has completed.
template<typename Task>
void post_wait(flow::async::Single_thread_task_loop* loop, Task&& task)
{
  loop->post(std::forward<Task>(task), flow::async::Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION);
}

using pool_offset_t = ipc::shm::arena_lend::Shm_pool::size_t;

/* Round-trip identity check, usable from any thread: with `base = to_address(id, 0)`, the forward and
 * reverse lookups must agree at various offsets.  (The borrower-side vaddr of a pool is internal -- the
 * repository maps the pool wherever the OS says -- so round-trip identity, not any absolute address, is the
 * assertable truth.)  Also: to_address_safe() must agree with to_address() for a live pool. */
void check_round_trips(pool_id_t pool_id, std::size_t pool_size)
{
  auto* const base = static_cast<char*>(Repository::to_address(pool_id, 0));
  ASSERT_TRUE(base);
  EXPECT_EQ(Repository::to_address_safe(pool_id, 0), base);

  for (const auto offset : { pool_offset_t(0), pool_offset_t(0x8), pool_offset_t(pool_size - 1) })
  {
    EXPECT_EQ(Repository::to_address(pool_id, offset), base + offset);

    pool_id_t rev_pool_id;
    pool_offset_t rev_offset;
    Repository::from_address(base + offset, rev_pool_id, rev_offset);
    EXPECT_EQ(rev_pool_id, pool_id);
    EXPECT_EQ(rev_offset, offset);
  }
}
} // Anonymous namespace

/// Exercises the collection register/deregister (use-count-based) API.
TEST(Borrower_shm_pool_collection_repository_test, Collection_interface)
{
  auto& repository = Repository::get_instance();
  const auto pool_name_base = create_test_pool_name_base();

  // Register collections (void -- no return to check).
  repository.register_collection(OWNER_ID_0, COLLECTION_ID_0, Shared_name(pool_name_base));
  // Registering same owner/collection again increments use-count.
  repository.register_collection(OWNER_ID_0, COLLECTION_ID_0, Shared_name(pool_name_base));
  repository.register_collection(OWNER_ID_0, COLLECTION_ID_1, Shared_name(pool_name_base));

  // Different owner, same collection_id -- distinct collection.
  repository.register_collection(OWNER_ID_1, COLLECTION_ID_0, Shared_name(pool_name_base));

  // Deregister (use-count-based).
  repository.deregister_collection(OWNER_ID_0, COLLECTION_ID_0); // use_count 2 -> 1.
  repository.deregister_collection(OWNER_ID_0, COLLECTION_ID_0); // use_count 1 -> removed.
  // Deregistering again would assert -- not tested here.

  repository.deregister_collection(OWNER_ID_0, COLLECTION_ID_1);
  repository.deregister_collection(OWNER_ID_1, COLLECTION_ID_0);
}

/// Exercises the pool interface: register, use-count dedup, deregister, plus owner-side cleanup.
TEST(Borrower_shm_pool_collection_repository_test, Pool_interface)
{
  auto& repository = Repository::get_instance();
  Test_logger logger;
  const auto SHM_POOL_SIZE = ipc::shm::arena_lend::jemalloc::Jemalloc_pages::get_page_size();
  /* Owner and borrower must agree on pool_name_base: register_shm_pool() internally does
   * Borrower_shm_pool_collection::open_shm_pool() which reconstructs the SHM object name as
   * pool_name_base / pool_id.  If the borrower's pool_name_base doesn't match the owner's,
   * the shm_open() fails because the name doesn't exist. */
  const auto pool_name_base = create_test_pool_name_base();

  // Create owner-side memory pools (real SHM -- the borrower will open_shm_pool() these by name).
  auto owner_collection_0_0 = make_shared<Test_shm_pool_collection>(&logger, COLLECTION_ID_0,
                                                                    Shared_name(pool_name_base));
  auto owner_collection_0_1 = make_shared<Test_shm_pool_collection>(&logger, COLLECTION_ID_1,
                                                                    Shared_name(pool_name_base));
  auto owner_collection_1_0 = make_shared<Test_shm_pool_collection>(&logger, COLLECTION_ID_0,
                                                                    Shared_name(pool_name_base));
  auto owner_shm_pool_0_0_0 = owner_collection_0_0->create_shm_pool(SHM_POOL_SIZE);
  auto owner_shm_pool_0_0_1 = owner_collection_0_0->create_shm_pool(SHM_POOL_SIZE);
  auto owner_shm_pool_0_1_0 = owner_collection_0_1->create_shm_pool(SHM_POOL_SIZE);
  auto owner_shm_pool_1_0_0 = owner_collection_1_0->create_shm_pool(SHM_POOL_SIZE);

  // Same pool_name_base as owner collections above.
  repository.register_collection(OWNER_ID_0, COLLECTION_ID_0, Shared_name(pool_name_base));
  repository.register_collection(OWNER_ID_0, COLLECTION_ID_1, Shared_name(pool_name_base));
  repository.register_collection(OWNER_ID_1, COLLECTION_ID_0, Shared_name(pool_name_base));

  /* Register pools (first registration opens the pool; subsequent ones increment use-count).
   * register_shm_pool() is void -- aborts on failure (catastrophic). */
  const auto pool_id_0_0_0 = owner_shm_pool_0_0_0->get_id();
  const auto pool_id_0_0_1 = owner_shm_pool_0_0_1->get_id();
  const auto pool_id_0_1_0 = owner_shm_pool_0_1_0->get_id();
  const auto pool_id_1_0_0 = owner_shm_pool_1_0_0->get_id();

  repository.register_shm_pool(OWNER_ID_0, COLLECTION_ID_0, pool_id_0_0_0, SHM_POOL_SIZE);
  // Registering same pool again increments use-count.
  repository.register_shm_pool(OWNER_ID_0, COLLECTION_ID_0, pool_id_0_0_0, SHM_POOL_SIZE);

  repository.register_shm_pool(OWNER_ID_0, COLLECTION_ID_0, pool_id_0_0_1, SHM_POOL_SIZE);
  repository.register_shm_pool(OWNER_ID_0, COLLECTION_ID_1, pool_id_0_1_0, SHM_POOL_SIZE);
  repository.register_shm_pool(OWNER_ID_1, COLLECTION_ID_0, pool_id_1_0_0, SHM_POOL_SIZE);

  // Deregister pools (void -- asserts on unknown pool).
  repository.deregister_shm_pool(OWNER_ID_0, COLLECTION_ID_0, pool_id_0_0_0); // use_count 2 -> 1.
  repository.deregister_shm_pool(OWNER_ID_0, COLLECTION_ID_0, pool_id_0_0_0); // 1 -> closed.
  repository.deregister_shm_pool(OWNER_ID_0, COLLECTION_ID_0, pool_id_0_0_1);
  repository.deregister_shm_pool(OWNER_ID_0, COLLECTION_ID_1, pool_id_0_1_0);
  repository.deregister_shm_pool(OWNER_ID_1, COLLECTION_ID_0, pool_id_1_0_0);

  // Deregister collections.
  repository.deregister_collection(OWNER_ID_1, COLLECTION_ID_0);
  repository.deregister_collection(OWNER_ID_0, COLLECTION_ID_1);
  repository.deregister_collection(OWNER_ID_0, COLLECTION_ID_0);

  // Remove owner-side memory pools.
  EXPECT_TRUE(owner_collection_1_0->remove_shm_pool(owner_shm_pool_1_0_0));
  EXPECT_TRUE(owner_collection_0_1->remove_shm_pool(owner_shm_pool_0_1_0));
  EXPECT_TRUE(owner_collection_0_0->remove_shm_pool(owner_shm_pool_0_0_1));
  EXPECT_TRUE(owner_collection_0_0->remove_shm_pool(owner_shm_pool_0_0_0));

  EXPECT_TRUE(ensure_empty_collection_at_destruction(owner_collection_0_0));
  EXPECT_TRUE(ensure_empty_collection_at_destruction(owner_collection_0_1));
  EXPECT_TRUE(ensure_empty_collection_at_destruction(owner_collection_1_0));
}

/* The repository's main reason for existing: the lookups -- untouched by the preceding TESTs.  Covered here:
 *   - Forward/reverse round-trip identity per pool at various offsets (see check_round_trips()); distinct
 *     pools resolve to distinct bases.
 *   - to_address_safe(): null for a never-registered pool ID; and its live/dead flip tied to the pool
 *     use-count -- doubly-registered pool stays resolvable after one deregister, goes null after the last
 *     (the use-count semantics observed through the lookup, not merely by absence of crashes).
 *     (No reverse-lookup miss cases: on the borrower side S_LOOKUP_CAN_FAIL = false -- only in-SHM
 *     addresses are in-contract for from_address().)
 *   - shm_pool_live_info(): lists exactly the live pools; shrinks on deregistration.
 * (recompute_pool_name() is exercised implicitly and sharply by all of this: register_shm_pool() opens the
 * real SHM object by the recomputed name -- a wrong name = failed open = abort.) */
TEST(Borrower_shm_pool_collection_repository_test, Lookups_and_live_info)
{
  auto& repository = Repository::get_instance();
  Test_logger logger;
  const auto SHM_POOL_SIZE = ipc::shm::arena_lend::jemalloc::Jemalloc_pages::get_page_size();
  const auto pool_name_base = create_test_pool_name_base();

  auto owner_collection = make_shared<Test_shm_pool_collection>(&logger, COLLECTION_ID_0,
                                                                Shared_name(pool_name_base));
  auto owner_pool_a = owner_collection->create_shm_pool(SHM_POOL_SIZE);
  auto owner_pool_b = owner_collection->create_shm_pool(SHM_POOL_SIZE);
  const auto pool_id_a = owner_pool_a->get_id();
  const auto pool_id_b = owner_pool_b->get_id();

  repository.register_collection(OWNER_ID_2, COLLECTION_ID_0, Shared_name(pool_name_base));
  repository.register_shm_pool(OWNER_ID_2, COLLECTION_ID_0, pool_id_a, SHM_POOL_SIZE);
  repository.register_shm_pool(OWNER_ID_2, COLLECTION_ID_0, pool_id_a, SHM_POOL_SIZE); // Use-count -> 2.
  repository.register_shm_pool(OWNER_ID_2, COLLECTION_ID_0, pool_id_b, SHM_POOL_SIZE);

  // The lookups.
  check_round_trips(pool_id_a, SHM_POOL_SIZE);
  check_round_trips(pool_id_b, SHM_POOL_SIZE);
  EXPECT_NE(Repository::to_address(pool_id_a, 0), Repository::to_address(pool_id_b, 0));
  EXPECT_FALSE(Repository::to_address_safe(pool_id_a + pool_id_b + 1, 0)); // Never-registered ID.

  { // Live-info: exactly our 2 pools (this test's collection is state-isolated via OWNER_ID_2).
    const auto live = repository.shm_pool_live_info();
    int n_found = 0;
    for (const auto& info : live)
    {
      n_found += ((info.m_id == pool_id_a) || (info.m_id == pool_id_b)) ? 1 : 0;
    }
    EXPECT_EQ(n_found, 2);
  }

  // Use-count observed through the lookup: 2 -> 1 keeps pool A resolvable; 1 -> 0 kills it.
  repository.deregister_shm_pool(OWNER_ID_2, COLLECTION_ID_0, pool_id_a);
  EXPECT_TRUE(Repository::to_address_safe(pool_id_a, 0));
  check_round_trips(pool_id_a, SHM_POOL_SIZE);
  repository.deregister_shm_pool(OWNER_ID_2, COLLECTION_ID_0, pool_id_a);
  EXPECT_FALSE(Repository::to_address_safe(pool_id_a, 0));
  check_round_trips(pool_id_b, SHM_POOL_SIZE); // Pool B is unbothered.

  repository.deregister_shm_pool(OWNER_ID_2, COLLECTION_ID_0, pool_id_b);
  EXPECT_FALSE(Repository::to_address_safe(pool_id_b, 0));
  repository.deregister_collection(OWNER_ID_2, COLLECTION_ID_0);

  EXPECT_TRUE(owner_collection->remove_shm_pool(owner_pool_a));
  EXPECT_TRUE(owner_collection->remove_shm_pool(owner_pool_b));
  EXPECT_TRUE(ensure_empty_collection_at_destruction(owner_collection));
} // TEST(Borrower_shm_pool_collection_repository_test, Lookups_and_live_info)

/* Mutation visibility across *already-active* threads: the borrower forward caches are push-model
 * (register/deregister push the change into every extant per-thread map under lock; cf. the owner side's
 * lazy-pull forward caches), and the reverse caches are push-updated likewise.  So: a worker whose
 * per-thread caches were born *before* a pool's registration must see the pool on its next lookup; and
 * un-see it after final deregistration. */
TEST(Borrower_shm_pool_collection_repository_test, Mutation_visibility)
{
  auto& repository = Repository::get_instance();
  Test_logger logger;
  const auto SHM_POOL_SIZE = ipc::shm::arena_lend::jemalloc::Jemalloc_pages::get_page_size();
  const auto pool_name_base = create_test_pool_name_base();

  auto owner_collection = make_shared<Test_shm_pool_collection>(&logger, COLLECTION_ID_1,
                                                                Shared_name(pool_name_base));
  auto owner_pool_a = owner_collection->create_shm_pool(SHM_POOL_SIZE);
  auto owner_pool_b = owner_collection->create_shm_pool(SHM_POOL_SIZE);
  const auto pool_id_a = owner_pool_a->get_id();
  const auto pool_id_b = owner_pool_b->get_id();

  repository.register_collection(OWNER_ID_2, COLLECTION_ID_1, Shared_name(pool_name_base));
  repository.register_shm_pool(OWNER_ID_2, COLLECTION_ID_1, pool_id_a, SHM_POOL_SIZE);

  flow::async::Single_thread_task_loop worker{&logger, "brwRepo"};
  worker.start();

  // The worker's per-thread caches are born here, knowing only pool A.
  post_wait(&worker, [&]() { check_round_trips(pool_id_a, SHM_POOL_SIZE); });

  repository.register_shm_pool(OWNER_ID_2, COLLECTION_ID_1, pool_id_b, SHM_POOL_SIZE); // Pushed to worker...
  post_wait(&worker, [&]()
  {
    check_round_trips(pool_id_b, SHM_POOL_SIZE); // ...which sees it without any cache rebirth.
  });

  repository.deregister_shm_pool(OWNER_ID_2, COLLECTION_ID_1, pool_id_a); // Ditto removal.
  post_wait(&worker, [&]()
  {
    EXPECT_FALSE(Repository::to_address_safe(pool_id_a, 0));
    check_round_trips(pool_id_b, SHM_POOL_SIZE);
  });

  worker.stop();

  repository.deregister_shm_pool(OWNER_ID_2, COLLECTION_ID_1, pool_id_b);
  repository.deregister_collection(OWNER_ID_2, COLLECTION_ID_1);
  EXPECT_TRUE(owner_collection->remove_shm_pool(owner_pool_a));
  EXPECT_TRUE(owner_collection->remove_shm_pool(owner_pool_b));
  EXPECT_TRUE(ensure_empty_collection_at_destruction(owner_collection));
} // TEST(Borrower_shm_pool_collection_repository_test, Mutation_visibility)

} // namespace ipc::session::shm::arena_lend::test
