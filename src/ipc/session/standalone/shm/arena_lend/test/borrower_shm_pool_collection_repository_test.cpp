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
#include "ipc/session/standalone/shm/arena_lend/arena_lend_fwd.hpp"
#include "ipc/shm/arena_lend/arena_lend_stats.hpp"
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

/* Fake owner IDs (owner ID = owner process's PID): all above Linux's PID ceiling (2^22), so none can equal a real
 * process's PID -- notably our own, which (with small in-process ordinals as collection IDs) keys the real arenas
 * of any live sessions.  Distinct offsets keep different TESTs' (or groups of TESTs') entries apart. */
const owner_id_t FAKE_OWNER_ID_BASE = 1000000000;
const owner_id_t OWNER_ID_0 = FAKE_OWNER_ID_BASE + 10;
const owner_id_t OWNER_ID_1 = FAKE_OWNER_ID_BASE + 20;
// Used by the lookup-oriented TESTs only (state-independence from the others):
const owner_id_t OWNER_ID_2 = FAKE_OWNER_ID_BASE + 30;
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

/* The bounded per-arena stats breakdown: stats() reports per-arena stat-sets for at most a fixed number of
 * arenas, forgetting the least-recently-touched one when that number would be exceeded.  ("Touched" = any of
 * [de]register_collection(), [de]register_shm_pool() for that arena.)  Covered here:
 *   - The cap holds; a stat-touched live arena survives churn that evicts an untouched live arena of the same age.
 *     (The touch is via register_shm_pool(), so that path's touch is covered too.)
 *   - stats() trimming: `per_arena_stats_sz_limit_or_0` keeps the N most recently touched (output still sorted by
 *     ID); `*n_arenas_omitted` counts the rest; a limit at or above the size (or 0) omits nothing; and with a null
 *     list neither is touched.
 *   - An arena evicted while still borrowed: its further register/deregister events skip its (absent) per-arena
 *     stat-set without re-inserting it (so no unbalanced gauges; e.g., no assert trip on the last deregister);
 *     once fully unborrowed, a fresh borrowing gets a fresh stat-set, its accumulators starting from zero.
 *   - The totals stay exact throughout, eviction notwithstanding.
 * Uses a fake owner ID and fake collection IDs, except for the one real pool (needed for register_shm_pool()).
 * Leaves (dead) fake per-arena entries behind in the process-wide repository; harmless for other TESTs, which
 * only look for their own arenas' entries. */
TEST(Borrower_shm_pool_collection_repository_test, Per_arena_stats_limits)
{
  using ipc::session::shm::arena_lend::Borrower_pool_stats_list;
  using ipc::shm::arena_lend::stat::Borrower_pool_stats;

  const owner_id_t OWNER_ID = FAKE_OWNER_ID_BASE + 40; // See FAKE_OWNER_ID_BASE.
  constexpr collection_id_t COLL_ID_L = 1; // Live throughout; touched mid-churn => survives eviction.
  constexpr collection_id_t COLL_ID_V = 2; // Live but untouched during churn => evicted while live.
  constexpr collection_id_t COLL_ID_CHURN_0 = 100; // Churned (register + deregister) arenas: from here up.
  constexpr size_t SZ_LIMIT = 1000; // Per docs: the internal limit, "as of this writing."
  constexpr size_t N_CHURN_1 = 500;
  constexpr size_t N_CHURN_2 = 600; // N_CHURN_1 + N_CHURN_2 > SZ_LIMIT: V gets evicted; L (touched between) not.

  auto& repository = Repository::get_instance();
  Test_logger logger;
  const auto pool_name_base = create_test_pool_name_base();
  const auto SHM_POOL_SIZE = ipc::shm::arena_lend::jemalloc::Jemalloc_pages::get_page_size();

  const auto totals = [&]() -> const Borrower_pool_stats& { return repository.stats(nullptr, 0, nullptr); };
  const auto t0_reg = totals().m_arena_register_count.load();
  const auto t0_first_reg = totals().m_arena_first_register_count.load();
  const auto t0_dereg = totals().m_arena_deregister_count.load();
  const auto t0_last_dereg = totals().m_arena_last_deregister_count.load();
  const auto t0_n_arenas = totals().m_n_borrowed_arenas.load();
  const auto t0_n_pools = totals().m_n_open_pools.load();

  // Full per-arena list (no trimming); and find-our-entry-in-it (null if absent).
  const auto all_entries = [&]() -> Borrower_pool_stats_list
  {
    Borrower_pool_stats_list list;
    size_t n_omitted = 12345;
    repository.stats(&list, 0, &n_omitted);
    EXPECT_EQ(n_omitted, 0u);
    return list;
  };
  const auto find_entry
    = [&](const Borrower_pool_stats_list& list, collection_id_t coll_id) -> const Borrower_pool_stats*
  {
    for (const auto& entry : list)
    {
      if ((entry->m_uniq_arena_id.m_id1 == uint64_t(OWNER_ID)) && (entry->m_uniq_arena_id.m_id2 == uint64_t(coll_id)))
      {
        return entry.get();
      }
    }
    return nullptr;
  };

  collection_id_t next_churn_id = COLL_ID_CHURN_0;
  const auto churn = [&](size_t n)
  {
    for (size_t idx = 0; idx != n; ++idx)
    {
      repository.register_collection(OWNER_ID, next_churn_id, Shared_name(pool_name_base));
      repository.deregister_collection(OWNER_ID, next_churn_id);
      ++next_churn_id;
    }
  };

  // L and V borrowed (same age); churn; touch L (via a pool opening); churn more.
  repository.register_collection(OWNER_ID, COLL_ID_L, Shared_name(pool_name_base));
  repository.register_collection(OWNER_ID, COLL_ID_V, Shared_name(pool_name_base));
  churn(N_CHURN_1);

  auto owner_collection = make_shared<Test_shm_pool_collection>(&logger, COLL_ID_L, Shared_name(pool_name_base));
  auto owner_pool = owner_collection->create_shm_pool(SHM_POOL_SIZE);
  const auto pool_id = owner_pool->get_id();
  repository.register_shm_pool(OWNER_ID, COLL_ID_L, pool_id, SHM_POOL_SIZE); // Touches L.

  churn(N_CHURN_2);
  const auto last_churn_id = next_churn_id - 1;

  { // The cap; who survived.
    const auto list = all_entries();
    EXPECT_EQ(list.size(), SZ_LIMIT);

    const auto entry_l = find_entry(list, COLL_ID_L);
    ASSERT_TRUE(entry_l) << "Touched mid-churn: must have survived eviction.";
    EXPECT_EQ(entry_l->m_arena_register_count.load(), 1u);
    EXPECT_EQ(entry_l->m_n_borrowed_arenas.load(), 1u);
    EXPECT_EQ(entry_l->m_pool_open_count.load(), 1u);
    EXPECT_EQ(entry_l->m_n_open_pools.load(), 1u);
    EXPECT_EQ(entry_l->m_mapped_sz.load(), SHM_POOL_SIZE);

    EXPECT_FALSE(find_entry(list, COLL_ID_V)) << "Untouched during churn: must have been evicted (while live).";
    EXPECT_FALSE(find_entry(list, COLL_ID_CHURN_0)) << "Oldest churned: must have been evicted.";
    EXPECT_TRUE(find_entry(list, last_churn_id));
    /* Exactly SZ_LIMIT survive = L + all of churn-2 + the newest (SZ_LIMIT - 1 - N_CHURN_2) of churn-1; so the
     * boundary inside churn-1 is precisely known. */
    const auto oldest_surviving_churn_1_id = collection_id_t(COLL_ID_CHURN_0 + N_CHURN_1 - (SZ_LIMIT - 1 - N_CHURN_2));
    EXPECT_TRUE(find_entry(list, oldest_surviving_churn_1_id));
    EXPECT_FALSE(find_entry(list, oldest_surviving_churn_1_id - 1));
  }

  { // stats() trimming.
    constexpr size_t N_SHOWN = 10;
    Borrower_pool_stats_list list;
    size_t n_omitted = 12345;
    repository.stats(&list, N_SHOWN, &n_omitted);
    ASSERT_EQ(list.size(), N_SHOWN);
    EXPECT_EQ(n_omitted, SZ_LIMIT - N_SHOWN);
    // The N_SHOWN most recently touched = the last N_SHOWN churned; output sorted ascending by ID.
    for (size_t idx = 0; idx != N_SHOWN; ++idx)
    {
      EXPECT_EQ(list[idx]->m_uniq_arena_id.m_id1, uint64_t(OWNER_ID));
      EXPECT_EQ(list[idx]->m_uniq_arena_id.m_id2, uint64_t(last_churn_id - (N_SHOWN - 1) + idx));
    }

    n_omitted = 12345;
    repository.stats(&list, SZ_LIMIT * 5, &n_omitted); // Limit above size: nothing omitted.
    EXPECT_EQ(list.size(), SZ_LIMIT);
    EXPECT_EQ(n_omitted, 0u);

    n_omitted = 12345;
    repository.stats(nullptr, N_SHOWN, &n_omitted); // Null list: out-arg not touched.
    EXPECT_EQ(n_omitted, 12345u);
  }

  /* V, evicted while live: borrow it again (use-count 2) -- must not re-insert it (that would be a stat-set
   * missing its first borrowing); then unborrow twice (the last time would trip the per-arena 0-or-1 gauge
   * assert, if V had been wrongly re-inserted). */
  repository.register_collection(OWNER_ID, COLL_ID_V, Shared_name(pool_name_base));
  EXPECT_FALSE(find_entry(all_entries(), COLL_ID_V));
  repository.deregister_collection(OWNER_ID, COLL_ID_V);
  repository.deregister_collection(OWNER_ID, COLL_ID_V);
  EXPECT_FALSE(find_entry(all_entries(), COLL_ID_V));
  EXPECT_EQ(totals().m_n_borrowed_arenas.load(), t0_n_arenas + 1); // Just L now.

  // V borrowed afresh (fully unborrowed in between): new stat-set, from zero.
  repository.register_collection(OWNER_ID, COLL_ID_V, Shared_name(pool_name_base));
  {
    const auto list = all_entries();
    const auto entry_v = find_entry(list, COLL_ID_V);
    ASSERT_TRUE(entry_v);
    EXPECT_EQ(entry_v->m_arena_register_count.load(), 1u);
    EXPECT_EQ(entry_v->m_arena_first_register_count.load(), 1u);
    EXPECT_EQ(entry_v->m_arena_deregister_count.load(), 0u);
    EXPECT_EQ(entry_v->m_n_borrowed_arenas.load(), 1u);
  }
  repository.deregister_collection(OWNER_ID, COLL_ID_V);

  // Unborrow L (pool first, as in real life).
  repository.deregister_shm_pool(OWNER_ID, COLL_ID_L, pool_id);
  repository.deregister_collection(OWNER_ID, COLL_ID_L);
  {
    const auto list = all_entries();
    const auto entry_l = find_entry(list, COLL_ID_L);
    ASSERT_TRUE(entry_l);
    EXPECT_EQ(entry_l->m_n_borrowed_arenas.load(), 0u);
    EXPECT_EQ(entry_l->m_n_open_pools.load(), 0u);
    EXPECT_EQ(entry_l->m_pool_close_count.load(), 1u);
    EXPECT_EQ(entry_l->m_mapped_sz.load(), 0u);
  }

  /* Totals: exact despite all the eviction.  Registrations: L 1; V 3 (initial, again, afresh); churn.  Of
   * those, "first" (0 -> 1) ones: all but V's second.  Likewise for deregistrations/"last" ones. */
  const size_t n_churn = N_CHURN_1 + N_CHURN_2;
  EXPECT_EQ(totals().m_arena_register_count.load() - t0_reg, 1 + 3 + n_churn);
  EXPECT_EQ(totals().m_arena_first_register_count.load() - t0_first_reg, 1 + 2 + n_churn);
  EXPECT_EQ(totals().m_arena_deregister_count.load() - t0_dereg, 1 + 3 + n_churn);
  EXPECT_EQ(totals().m_arena_last_deregister_count.load() - t0_last_dereg, 1 + 2 + n_churn);
  EXPECT_EQ(totals().m_n_borrowed_arenas.load(), t0_n_arenas);
  EXPECT_EQ(totals().m_n_open_pools.load(), t0_n_pools);

  EXPECT_TRUE(owner_collection->remove_shm_pool(owner_pool));
  EXPECT_TRUE(ensure_empty_collection_at_destruction(owner_collection));
} // TEST(Borrower_shm_pool_collection_repository_test, Per_arena_stats_limits)

} // namespace ipc::session::shm::arena_lend::test
