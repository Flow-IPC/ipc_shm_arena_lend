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
#include "ipc/shm/arena_lend/shm_pool_repository.hpp"
#include "ipc/shm/arena_lend/arena_lend_fwd.hpp"
#include "ipc/test/test_logger.hpp"

using std::make_shared;
using std::shared_ptr;
using std::string;
using std::string_view;
using std::size_t;

namespace ipc::shm::arena_lend::test
{

using ipc::test::Test_logger;

/**
 * Class interface tests. As a class instance cannot be constructed by itself, the Owner_shm_pool_repository class
 * is used.
 */
TEST(Shm_pool_repository_test, Interface)
{
  const Shm_pool::pool_id_t NON_EXISTENT_SHM_POOL_ID = 2;
  const string NON_EXISTENT_SHM_POOL_NAME("Shm_pool_repository_test_non_existent_pool");
  const Shm_pool::pool_id_t SHM_POOL_ID = 1;
  const Shm_pool::pool_id_t SHM_POOL_DUPLICATE_ID = 3;
  const string SHM_POOL_NAME("Shm_pool_repository_test_pool");
  const string SHM_POOL_DUPLICATE_ADDRESS_NAME("m_pool_repository_test_duplicate_pool");
  void* const SHM_POOL_ADDRESS = reinterpret_cast<void*>(0x10);
  constexpr size_t FAKE_OBJECT_OFFSET = 2;
  void* const FAKE_OBJECT_ADDRESS = static_cast<char*>(SHM_POOL_ADDRESS) + FAKE_OBJECT_OFFSET;
  void* const SHM_POOL_ADDRESS_2 = reinterpret_cast<void*>(reinterpret_cast<size_t>(SHM_POOL_ADDRESS) * 2);
  const size_t SHM_POOL_SIZE = 4096;
  const int SHM_POOL_FD = 1;

  Test_logger logger;
  Owner_shm_pool_repository repository;
  auto shm_pool = make_shared<Shm_pool>(SHM_POOL_ID, SHM_POOL_NAME, SHM_POOL_ADDRESS, SHM_POOL_SIZE, SHM_POOL_FD);
  auto duplicate_address_shm_pool =
    make_shared<Shm_pool>(SHM_POOL_DUPLICATE_ID, SHM_POOL_DUPLICATE_ADDRESS_NAME,
                          SHM_POOL_ADDRESS, SHM_POOL_SIZE, SHM_POOL_FD);

  // Ensure conversion fails
  EXPECT_EQ(repository.to_address(SHM_POOL_ID, 0), nullptr);
  shared_ptr<Shm_pool> converted_shm_pool;
  Shm_pool::size_t converted_offset;
  repository.from_address(nullptr, converted_shm_pool, converted_offset);
  EXPECT_FALSE(converted_shm_pool);
  // Insert and ensure conversion succeeds
  EXPECT_TRUE(repository.insert(shm_pool));
  EXPECT_EQ(repository.to_address(SHM_POOL_ID, 0), SHM_POOL_ADDRESS);
  repository.from_address(SHM_POOL_ADDRESS, converted_shm_pool, converted_offset);
  EXPECT_TRUE(converted_shm_pool);
  EXPECT_EQ(converted_shm_pool->get_name(), SHM_POOL_NAME);
  EXPECT_EQ(converted_offset, 0UL);
  repository.from_address(FAKE_OBJECT_ADDRESS, converted_shm_pool, converted_offset);
  EXPECT_TRUE(converted_shm_pool);
  EXPECT_EQ(converted_shm_pool->get_name(), SHM_POOL_NAME);
  EXPECT_EQ(converted_offset, FAKE_OBJECT_OFFSET);
  // Lookup non-existent name
  EXPECT_EQ(repository.to_address(NON_EXISTENT_SHM_POOL_ID, 0), nullptr);
  // Insert again into repository, which is not allowed
  EXPECT_FALSE(repository.insert(shm_pool));
  // Insert a pool with a different name, but same address
  EXPECT_FALSE(repository.insert(duplicate_address_shm_pool));
  // Ensure conversion fails for different name
  EXPECT_EQ(repository.to_address(SHM_POOL_DUPLICATE_ID, 0), nullptr);
  // Ensure conversion still succeeds for original name
  EXPECT_EQ(repository.to_address(SHM_POOL_ID, 0), SHM_POOL_ADDRESS);
  repository.from_address(SHM_POOL_ADDRESS, converted_shm_pool, converted_offset);
  EXPECT_TRUE(converted_shm_pool);
  EXPECT_EQ(converted_shm_pool->get_name(), SHM_POOL_NAME);
  EXPECT_EQ(converted_offset, 0UL);
  // Remove from repository and ensure conversion fails
  EXPECT_TRUE(repository.erase(SHM_POOL_ID));
  EXPECT_EQ(repository.to_address(SHM_POOL_ID, 0), nullptr);
  repository.from_address(nullptr, converted_shm_pool, converted_offset);
  EXPECT_FALSE(converted_shm_pool);

  // Reinsert with different address
  EXPECT_TRUE(
    repository.insert(make_shared<Shm_pool>(SHM_POOL_ID, SHM_POOL_NAME, SHM_POOL_ADDRESS_2, SHM_POOL_SIZE, SHM_POOL_FD)));
  // Duplicate address with different name should work now
  EXPECT_TRUE(repository.insert(duplicate_address_shm_pool));
  // Ensure conversion succeeds for different name
  EXPECT_EQ(repository.to_address(SHM_POOL_DUPLICATE_ID, 0), SHM_POOL_ADDRESS);
  repository.from_address(SHM_POOL_ADDRESS, converted_shm_pool, converted_offset);
  EXPECT_TRUE(converted_shm_pool);
  EXPECT_EQ(converted_shm_pool->get_name(), SHM_POOL_DUPLICATE_ADDRESS_NAME);
  EXPECT_EQ(converted_offset, 0UL);
  // Ensure conversion succeeds
  EXPECT_EQ(repository.to_address(SHM_POOL_ID, 0), SHM_POOL_ADDRESS_2);
  repository.from_address(SHM_POOL_ADDRESS_2, converted_shm_pool, converted_offset);
  EXPECT_TRUE(converted_shm_pool);
  EXPECT_EQ(converted_shm_pool->get_name(), SHM_POOL_NAME);
  EXPECT_EQ(converted_shm_pool->get_id(), SHM_POOL_ID);
  EXPECT_EQ(converted_offset, 0UL);
  // Remove from repository
  EXPECT_TRUE(repository.erase(SHM_POOL_ID));
  // Ensure conversion fails
  EXPECT_EQ(repository.to_address(SHM_POOL_ID, 0), nullptr);
}

} // namespace ipc::shm::arena_lend::test
