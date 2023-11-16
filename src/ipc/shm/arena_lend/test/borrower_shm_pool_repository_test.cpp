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
#include "ipc/shm/arena_lend/borrower_shm_pool_repository.hpp"
#include "ipc/test/test_logger.hpp"

using std::make_shared;
using std::shared_ptr;
using std::string;
using std::size_t;

namespace ipc::shm::arena_lend::test
{

using ipc::test::Test_logger;

/// Class interface tests. The focus is primarily on the added interface and not the parent interface.
TEST(Borrower_shm_pool_repository_test, Interface)
{
  const Shm_pool::pool_id_t SHM_POOL_ID = 1;
  const string SHM_POOL_NAME("Borrower_shm_pool_repository_test_pool");
  void* const SHM_POOL_ADDRESS = reinterpret_cast<void*>(0x10);
  constexpr size_t SHM_POOL_SIZE = 4096;
  constexpr int SHM_POOL_FD = 1;

  Test_logger logger;
  Borrower_shm_pool_repository repository;
  auto shm_pool = make_shared<Shm_pool>(SHM_POOL_ID, SHM_POOL_NAME, SHM_POOL_ADDRESS, SHM_POOL_SIZE, SHM_POOL_FD);

  // Ensure conversion fails
  EXPECT_EQ(repository.to_address(SHM_POOL_ID, 0), nullptr);
  shared_ptr<Shm_pool> converted_shm_pool;
  Shm_pool::size_t converted_offset;
  repository.from_address(nullptr, converted_shm_pool, converted_offset);
  EXPECT_FALSE(converted_shm_pool);
  // Insert and ensure conversion succeeds
  EXPECT_TRUE(repository.insert(shm_pool));
  EXPECT_EQ(repository.to_address(SHM_POOL_ID, 0), SHM_POOL_ADDRESS);
  // Increment use of the SHM pool
  auto stored_shm_pool = repository.increment_use(SHM_POOL_ID);
  EXPECT_EQ(*stored_shm_pool, *shm_pool);
  // Ensure conversion succeeds
  EXPECT_EQ(repository.to_address(SHM_POOL_ID, 0), SHM_POOL_ADDRESS);
  // Decrement use
  unsigned int use_count = 0;
  stored_shm_pool = repository.erase_or_decrement_use(SHM_POOL_ID, use_count);
  if (stored_shm_pool != nullptr)
  {
    EXPECT_EQ(*stored_shm_pool, *shm_pool);
    EXPECT_EQ(use_count, 1UL);
  }
  else
  {
    ADD_FAILURE() << "Could not find SHM pool [" << SHM_POOL_ID << "]";
  }
  // Ensure conversion succeeds
  EXPECT_EQ(repository.to_address(SHM_POOL_ID, 0), SHM_POOL_ADDRESS);

  // Really remove
  stored_shm_pool = repository.erase_or_decrement_use(SHM_POOL_ID, use_count);
  if (stored_shm_pool != nullptr)
  {
    EXPECT_EQ(*stored_shm_pool, *shm_pool);
    EXPECT_EQ(use_count, 0UL);
  }
  else
  {
    ADD_FAILURE() << "Could not find SHM pool [" << SHM_POOL_ID << "]";
  }
  // Ensure conversion fails
  EXPECT_EQ(repository.to_address(SHM_POOL_ID, 0), nullptr);
}

} // namespace ipc::shm::arena_lend::test
