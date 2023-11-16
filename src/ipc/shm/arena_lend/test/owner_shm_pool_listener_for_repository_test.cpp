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
#include "ipc/shm/arena_lend/owner_shm_pool_listener_for_repository.hpp"
#include "ipc/shm/arena_lend/arena_lend_fwd.hpp"
#include "ipc/shm/arena_lend/shm_pool.hpp"
#include "ipc/shm/arena_lend/shm_pool_repository.hpp"
#include "ipc/shm/arena_lend/shm_pool_repository_singleton.hpp"
#include "ipc/test/test_logger.hpp"

using std::make_shared;
using std::set;
using std::shared_ptr;
using std::string;
using std::size_t;
using std::to_string;

namespace ipc::shm::arena_lend
{

using test::Test_logger;

/// Class interface tests.
TEST(Owner_shm_pool_listener_for_repository_test, Interface)
{
  const string SHM_POOL_NAME_PREFIX("Owner_shm_pool_listener_for_repository_test_pool");
  char* const FIRST_SHM_POOL_ADDRESS = reinterpret_cast<char*>(0x10);
  const size_t SHM_POOL_SIZE = 4096;
  const int SHM_POOL_FD = 1;

  set<shared_ptr<Shm_pool>> initial_shm_pools;
  const size_t INITIAL_SHM_POOLS_SIZE = 2;
  size_t i;
  for (i = 0; i < INITIAL_SHM_POOLS_SIZE; ++i)
  {
    auto result = initial_shm_pools.emplace(make_shared<Shm_pool>(i + 1,
                                                                  SHM_POOL_NAME_PREFIX + "_" + to_string(i),
                                                                  FIRST_SHM_POOL_ADDRESS + (SHM_POOL_SIZE * i),
                                                                  SHM_POOL_SIZE,
                                                                  SHM_POOL_FD));
    EXPECT_TRUE(result.second);
  }

  Test_logger logger;
  Owner_shm_pool_listener_for_repository listener(&logger);
  const string SHM_POOL_NAME(SHM_POOL_NAME_PREFIX + "_" + to_string(INITIAL_SHM_POOLS_SIZE));
  void* const SHM_POOL_ADDRESS = FIRST_SHM_POOL_ADDRESS + (SHM_POOL_SIZE * INITIAL_SHM_POOLS_SIZE);
  auto shm_pool = make_shared<Shm_pool>(++i, SHM_POOL_NAME, SHM_POOL_ADDRESS, SHM_POOL_SIZE, SHM_POOL_FD);
  Owner_shm_pool_repository& repository = Owner_shm_pool_repository_singleton::get_instance();

  auto check_initial_functor =
    [&](bool expect_success) -> bool
    {
      bool result = true;
      for (const auto& cur_shm_pool : initial_shm_pools)
      {
        void* expected_address = (expect_success ? cur_shm_pool->get_address() : nullptr);
        if (repository.to_address(cur_shm_pool->get_id(), 0) != expected_address)
        {
          result = false;
        }
      }
      return result;
    };

  // Ensure conversions fail
  EXPECT_TRUE(check_initial_functor(false));
  // Initial list into global repository
  listener.notify_initial_shm_pools(initial_shm_pools);
  // Ensure conversions succeed
  EXPECT_TRUE(check_initial_functor(true));
  // Insert into global repository
  listener.notify_created_shm_pool(shm_pool);
  // Ensure conversion succeeds
  EXPECT_EQ(repository.to_address(i, 0), SHM_POOL_ADDRESS);
  // Remove from global repository
  listener.notify_removed_shm_pool(shm_pool);
  // Ensure conversion fails
  EXPECT_EQ(repository.to_address(i, 0), nullptr);
  // Ensure initial conversions still succeed
  EXPECT_TRUE(check_initial_functor(true));
  // Remove initial SHM pools
  for (const auto& cur_shm_pool : initial_shm_pools)
  {
    listener.notify_removed_shm_pool(cur_shm_pool);
  }
  // Ensure conversions fail
  EXPECT_TRUE(check_initial_functor(false));
}

} // namespace ipc::shm::arena_lend
