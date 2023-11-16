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
#include "ipc/session/standalone/shm/arena_lend/borrower_collection.hpp"
#include "ipc/shm/arena_lend/test/test_shm_pool_collection.hpp"
#include "ipc/test/test_logger.hpp"

using namespace ipc::test;
using std::shared_ptr;
using std::size_t;
using std::string;
using std::vector;
using ipc::shm::arena_lend::Borrower_shm_pool_collection;
using ipc::shm::arena_lend::Shm_pool;
using ipc::shm::arena_lend::test::Test_shm_pool_collection;

namespace ipc::session::shm::arena_lend::test
{

/**
 * Class interface tests.
 * We perform the following:
 * 1. Register fake shared memory pools
 * 2. Register fake objects
 * 3. Deregister the fake objects
 * 4. Deregister the fake shared memory pools
 */
TEST(Borrower_collection_test, Interface)
{
  // The identifiers for the SHM pools to be created
  const vector<string> SHM_POOL_NAMES = { "id_0", "id_1", "id_2" };
  // The size of the SHM pools to be created
  const size_t SHM_POOL_SIZE = 100;
  // The creation of fake SHM pools will start at this address and increment by the pool size
  uint8_t* const BASE_SHM_POOL_ADDRESS = reinterpret_cast<uint8_t*>(0x1000UL);
  // The creation of fake SHM pools will start at this fd and monotonically increment
  const int BASE_FD = 1;
  // Offset of objects to be created in each SHM pool
  const vector<size_t> OBJECT_OFFSETS = { 0, 10, 20, 30 };
  // The identifier for a SHM pool that will not be registered; used for negative testing
  const Shm_pool::pool_id_t UNREGISTERED_SHM_POOL_ID = 5000;
  // The offset of an object that will not be registered\; used for negative testing
  const size_t UNREGISTERED_OBJECT_OFFSET = 40;

  Test_logger logger;
  shared_ptr<Borrower_shm_pool_collection> borrower_shm_pool_collection =
    std::make_shared<Borrower_shm_pool_collection>(&logger, Test_shm_pool_collection::S_DEFAULT_COLLECTION_ID);
  Borrower_collection collection(&logger, borrower_shm_pool_collection);
  vector<shared_ptr<Shm_pool>> shm_pools;

  EXPECT_EQ(collection.get_borrower_shm_pool_collection(), borrower_shm_pool_collection);

  // Register SHM pools
  size_t i = 0;
  for (const auto& cur_shm_pool_name : SHM_POOL_NAMES)
  {
    const Shm_pool::pool_id_t cur_shm_pool_id = i + 1;
    void* cur_shm_pool_address = reinterpret_cast<void*>(BASE_SHM_POOL_ADDRESS + (i * SHM_POOL_SIZE));
    int cur_fd = BASE_FD + i;
    auto cur_shm_pool = std::make_shared<Shm_pool>(cur_shm_pool_id,
                                                   cur_shm_pool_name, cur_shm_pool_address, SHM_POOL_SIZE, cur_fd);
    EXPECT_EQ(collection.find_shm_pool(cur_shm_pool_id), nullptr);
    EXPECT_TRUE(collection.register_shm_pool(cur_shm_pool));
    EXPECT_FALSE(collection.register_shm_pool(cur_shm_pool));
    EXPECT_EQ(collection.find_shm_pool(cur_shm_pool_id), cur_shm_pool);

    shm_pools.push_back(cur_shm_pool);

    // Register offsets
    for (auto cur_object_offset : OBJECT_OFFSETS)
    {
      EXPECT_TRUE(collection.register_object(cur_shm_pool_id, cur_object_offset));
      // Register same object
      EXPECT_TRUE(collection.register_object(cur_shm_pool_id, cur_object_offset));
    }
    ++i;
  }

  // Check SHM pools versus repository
  {
    auto shm_pool_ids = collection.get_shm_pool_ids();
    EXPECT_EQ(shm_pool_ids.size(), SHM_POOL_NAMES.size());
    for (Shm_pool::pool_id_t cur_shm_pool_id = 1; cur_shm_pool_id <= SHM_POOL_NAMES.size(); ++cur_shm_pool_id)
    {
      EXPECT_TRUE(shm_pool_ids.find(cur_shm_pool_id) != shm_pool_ids.end());
    }
  }

  // Register offset at an unregistered SHM pool
  EXPECT_FALSE(collection.register_object(UNREGISTERED_SHM_POOL_ID, 0));

  // Deregister a SHM pool that was never registered
  EXPECT_EQ(collection.deregister_shm_pool(UNREGISTERED_SHM_POOL_ID), nullptr);
  // Deregister an object for a SHM pool that was never registered
  EXPECT_FALSE(collection.deregister_object(UNREGISTERED_SHM_POOL_ID, OBJECT_OFFSETS[0]));

  // Deregister SHM pool ids
  i = 0;
  for (Shm_pool::pool_id_t cur_shm_pool_id = 1; cur_shm_pool_id <= SHM_POOL_NAMES.size(); ++cur_shm_pool_id)
  {
    // Deregister an offset that was never registered
    EXPECT_FALSE(collection.deregister_object(cur_shm_pool_id, UNREGISTERED_OBJECT_OFFSET));

    // Deregister SHM pool before deregistering offsets, which is not allowed
    EXPECT_EQ(collection.find_shm_pool(cur_shm_pool_id), shm_pools[i]);
    EXPECT_EQ(collection.deregister_shm_pool(cur_shm_pool_id), nullptr);
    EXPECT_EQ(collection.find_shm_pool(cur_shm_pool_id), shm_pools[i]);

    // Deregister offsets
    for (auto cur_object_offset : OBJECT_OFFSETS)
    {
      EXPECT_TRUE(collection.deregister_object(cur_shm_pool_id, cur_object_offset));
      // We registered twice, so we can deregister twice
      EXPECT_TRUE(collection.deregister_object(cur_shm_pool_id, cur_object_offset));
      // Object already deregistered, so it will be missing
      EXPECT_FALSE(collection.deregister_object(cur_shm_pool_id, cur_object_offset));
    }

    // Deregister SHM pool after deregistering objects
    EXPECT_EQ(collection.find_shm_pool(cur_shm_pool_id), shm_pools[i]);
    EXPECT_EQ(collection.deregister_shm_pool(cur_shm_pool_id), shm_pools[i]);
    // SHM pool already deregistered, so it will be missing
    EXPECT_EQ(collection.deregister_shm_pool(cur_shm_pool_id), nullptr);
    EXPECT_EQ(&collection.find_shm_pool(cur_shm_pool_id), &Borrower_collection::S_EMPTY_SHM_POOL);

    ++i;
  }

  // Make sure repository is empty
  {
    auto shm_pool_ids = collection.get_shm_pool_ids();
    EXPECT_EQ(shm_pool_ids.size(), 0UL);
  }
}

} // namespace ipc::session::shm::arena_lend::test
