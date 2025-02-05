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
#include "ipc/shm/arena_lend/owner_shm_pool_listener.hpp"
#include "ipc/shm/arena_lend/jemalloc/ipc_arena.hpp"
#include "ipc/shm/arena_lend/jemalloc/test/test_util.hpp"
#include "ipc/shm/arena_lend/test/test_shm_object.hpp"
#include "ipc/test/test_logger.hpp"

using ipc::test::Test_logger;

using std::make_shared;
using std::make_unique;
using std::set;
using std::shared_ptr;

namespace ipc::shm::arena_lend::jemalloc::test
{

namespace
{

/**
 * Listener to track shared memory pools.
 */
class Shm_pool_listener_impl final :
  public Owner_shm_pool_listener
{
public:
  /// Single-reader, single-writer mutex.
  using Mutex = std::mutex;
  /// Exclusive lock for the mutex.
  using Lock = std::lock_guard<Mutex>;

  /**
   * Returns the current set of SHM pools.
   *
   * @return See above.
   */
  set<shared_ptr<Shm_pool>> get_shm_pools() const
  {
    Lock lock(m_shm_pools_mutex);
    return m_shm_pools;
  }

  /**
   * This method will be called upon initial registration in a synchronous manner.
   *
   * @param shm_pools The current set of active SHM pools, which may be empty.
   */
  void notify_initial_shm_pools(const set<shared_ptr<Shm_pool>>& shm_pools) override
  {
    Lock lock(m_shm_pools_mutex);
    m_shm_pools = shm_pools;
  }

  /**
   * This method will be called upon creation of a shared memory pool in a synchronous manner. This means that
   * no objects will be created within the pool until the method returns as the memory manager is blocked
   * from completing the allocation within the shared memory pool.
   *
   * @param shm_pool The shared memory pool that was created.
   */
  void notify_created_shm_pool(const shared_ptr<Shm_pool>& shm_pool) override
  {
    Lock lock(m_shm_pools_mutex);
    auto result_pair = m_shm_pools.emplace(shm_pool);
    EXPECT_TRUE(result_pair.second);
  }

  /**
   * This method will be called upon removal of a shared memory pool in a synchronous manner. No objects should
   * reside within this pool at the time of removal.
   *
   * @param shm_pool The shared memory pool that is being removed.
   */
  void notify_removed_shm_pool(const shared_ptr<Shm_pool>& shm_pool) override
  {
    Lock lock(m_shm_pools_mutex);
    EXPECT_NE(m_shm_pools.erase(shm_pool), 0UL);
  }

private:
  /// Exclusive access lock to #m_shm_pools.
  mutable Mutex m_shm_pools_mutex;
  /// The current set of shared memory pools.
  set<shared_ptr<Shm_pool>> m_shm_pools;
}; // class Shm_pool_listener_impl

/// Google test fixture.
class Ipc_arena_test :
  public ::testing::Test
{
public:
  /// Test allocation size.
  static constexpr size_t S_ALLOC_SIZE = 10000000;

  /**
   * Constructor.
   */
  Ipc_arena_test() :
    m_logger(flow::log::Sev::S_TRACE),
    m_memory_manager(make_shared<Memory_manager>(&m_logger))
  {
  }

  /**
   * Creates an arena.
   *
   * @return The arena created.
   */
  shared_ptr<Ipc_arena> create_arena()
  {
    return test::create_arena(&m_logger, m_memory_manager);
  }

private:
  /// For logging purposes.
  Test_logger m_logger;
  /// The memory allocator.
  shared_ptr<Memory_manager> m_memory_manager;
}; // class Ipc_arena_test

} // Anonymous namespace

/// Death tests - suffixed with DeathTest per Googletest conventions, aliased to fixture.
using Ipc_arena_DeathTest = Ipc_arena_test;
TEST_F(Ipc_arena_DeathTest, Interface)
{
  auto object = make_shared<int>();
  EXPECT_THROW(Ipc_arena::get_collection_id(object), std::runtime_error);
}

/// Class interface tests.
TEST_F(Ipc_arena_test, Interface)
{
  auto arena_1 = create_arena();

  // Test listener and allocation methods
  {
    // Ensure we are notified of a new shared memory pool during allocation
    auto arena_1_listener = make_unique<Shm_pool_listener_impl>();
    EXPECT_TRUE(arena_1->add_shm_pool_listener(arena_1_listener.get()));
    auto shm_pools_before_allocation = arena_1_listener->get_shm_pools();

    // Allocate a large enough size such that a new shared memory pool would be created
    void* allocation = arena_1->allocate(S_ALLOC_SIZE);
    EXPECT_NE(allocation, nullptr);
    auto shm_pools_after_allocation = arena_1_listener->get_shm_pools();
    EXPECT_GT(shm_pools_after_allocation.size(), shm_pools_before_allocation.size());
    arena_1->deallocate(allocation);
    EXPECT_TRUE(arena_1->remove_shm_pool_listener(arena_1_listener.get()));
  }

  // Create a scope such that the objects are destructed automatically
  {
    // Ensure that we can track back a shared pointer
    auto object_1 = arena_1->construct<int>();
    EXPECT_EQ(Ipc_arena::get_collection_id(object_1), arena_1->get_id());

    // Make sure another arena doesn't cause issues
    auto arena_2 = create_arena();
    {
      auto object_2 = arena_2->construct<int>();
      EXPECT_EQ(Ipc_arena::get_collection_id(object_2), arena_2->get_id());
      auto object_3 = arena_1->construct<int>();
      EXPECT_EQ(Ipc_arena::get_collection_id(object_3), arena_1->get_id());
    }
    EXPECT_TRUE(ipc::shm::arena_lend::test::ensure_empty_collection_at_destruction(arena_2));
  }

  EXPECT_TRUE(ipc::shm::arena_lend::test::ensure_empty_collection_at_destruction(arena_1));
}

} // namespace ipc::shm::arena_lend::jemalloc::test
