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
#include "ipc/session/standalone/shm/arena_lend/lender_collection.hpp"
#include "ipc/shm/arena_lend/jemalloc/jemalloc_pages.hpp"
#include "ipc/shm/arena_lend/test/test_shm_object.hpp"
#include "ipc/shm/arena_lend/test/test_shm_pool_collection.hpp"
#include "ipc/test/test_fake_object.hpp"
#include "ipc/test/test_logger.hpp"
#include <unordered_set>

using namespace ipc::test;
using std::make_shared;
using std::shared_ptr;
using std::size_t;
using std::string;
using std::unordered_map;
using std::unordered_set;
using std::vector;

namespace ipc::session::shm::arena_lend::test
{

using ipc::shm::arena_lend::Shm_pool;
using ipc::shm::arena_lend::jemalloc::Jemalloc_pages;
using ipc::shm::arena_lend::test::Test_shm_pool_collection;
using pool_id_t = Shm_pool::pool_id_t;
using pool_offset_t = Shm_pool::size_t;

/**
 * Class interface tests.
 * We perform the following:
 * 1. Create shared memory pools
 * 2. Register the shared memory pools
 * 3. Register fake objects
 * 4. Deregister the fake objects
 * 5. Deregister the shared memory pools
 * 6. Remove shared memory pools
 */
TEST(Lender_collection_test, Interface)
{
  const size_t NUM_SHM_POOLS = 3;
  const size_t SHM_POOL_SIZE = Jemalloc_pages::get_page_size();
  const vector<size_t> OBJECT_OFFSETS = { 0, 10, 20, 30 };
  const pool_id_t UNREGISTERED_SHM_POOL_ID = 154154;
  const size_t UNREGISTERED_OBJECT_OFFSET = 40;

  Test_logger logger;
  shared_ptr<Test_shm_pool_collection> shm_pool_collection = make_shared<Test_shm_pool_collection>(&logger);

  // Create SHM pools
  using Object_list = unordered_set<shared_ptr<void>>;
  unordered_map<shared_ptr<Shm_pool>, Object_list> object_map;
  for (size_t shm_pool_counter = 0; shm_pool_counter < NUM_SHM_POOLS; ++shm_pool_counter)
  {
    shared_ptr<Shm_pool> cur_shm_pool = shm_pool_collection->create_shm_pool(SHM_POOL_SIZE);
    if (cur_shm_pool == nullptr)
    {
      ADD_FAILURE() << "Could not create SHM pool, so aborting test";
      ipc::shm::arena_lend::test::remove_test_shm_objects_filesystem();
      return;
    }
    auto cur_result_pair = object_map.emplace(cur_shm_pool, Object_list());
    EXPECT_TRUE(cur_result_pair.second);
    auto& cur_object_list = cur_result_pair.first->second;

    // Create fake objects at specified offsets in the SHM pool
    for (size_t i = 0; i < OBJECT_OFFSETS.size(); ++i)
    {
      auto result = cur_object_list.emplace(
        create_fake_shared_object(static_cast<uint8_t*>(cur_shm_pool->get_address()) + OBJECT_OFFSETS[i]));
      EXPECT_TRUE(result.second);
    }
  }

  Lender_collection collection(&logger, shm_pool_collection);

  // Register SHM pool ids
  for (const auto& cur_object_map_pair : object_map)
  {
    const shared_ptr<Shm_pool>& cur_shm_pool = cur_object_map_pair.first;
    const auto cur_shm_pool_id = cur_shm_pool->get_id();
    const Object_list& cur_object_list = cur_object_map_pair.second;

    EXPECT_TRUE(collection.register_shm_pool(cur_shm_pool));
    EXPECT_FALSE(collection.register_shm_pool(cur_shm_pool));

    // Register objects
    for (const auto& cur_object : cur_object_list)
    {
      // Compute object offset from the base of the pool
      pool_offset_t cur_object_offset = 0;
      EXPECT_TRUE(cur_shm_pool->determine_offset(cur_object.get(), cur_object_offset));

      pool_id_t cur_registered_shm_pool_id;
      pool_offset_t cur_registered_pool_offset;
      EXPECT_TRUE(collection.register_object(cur_object, cur_registered_shm_pool_id, cur_registered_pool_offset));
      // Check that the shared memory pool id matches expected
      EXPECT_EQ(cur_shm_pool_id, cur_registered_shm_pool_id);
      // Check that the object offset matches expected
      EXPECT_EQ(cur_object_offset, cur_registered_pool_offset);

      // Register same object twice
      EXPECT_TRUE(collection.register_object(cur_object, cur_registered_shm_pool_id, cur_registered_pool_offset));
      // Check that the shared memory pool id matches expected
      EXPECT_EQ(cur_shm_pool_id, cur_registered_shm_pool_id);
      // Check that the object offset matches expected
      EXPECT_EQ(cur_object_offset, cur_registered_pool_offset);
    }

    // Register an object residing at a location not in a SHM pool
    {
      pool_id_t cur_registered_shm_pool_id;
      pool_offset_t cur_registered_pool_offset;
      shared_ptr<uint8_t> fake_object(create_fake_shared_object(static_cast<uint8_t*>(nullptr)));
      EXPECT_FALSE(collection.register_object(fake_object, cur_registered_shm_pool_id, cur_registered_pool_offset));
    }
  }

  // Deregister a SHM pool that was never registered
  bool was_empty;
  EXPECT_FALSE(collection.deregister_shm_pool(UNREGISTERED_SHM_POOL_ID, was_empty));
  // Deregister an object for the SHM pool that was never registered
  EXPECT_EQ(collection.deregister_object(UNREGISTERED_SHM_POOL_ID, OBJECT_OFFSETS[0]), nullptr);

  // Deregister SHM pool ids
  bool is_first_shm_pool = true;
  for (auto& cur_object_map_pair : object_map)
  {
    const shared_ptr<Shm_pool>& cur_shm_pool = cur_object_map_pair.first;
    Object_list& cur_object_list = cur_object_map_pair.second;
    const auto cur_shm_pool_id = cur_shm_pool->get_id();

    // Deregister an object that was never registered
    EXPECT_EQ(collection.deregister_object(cur_shm_pool_id, UNREGISTERED_OBJECT_OFFSET), nullptr);

    if (is_first_shm_pool)
    {
      // Deregister SHM pool before deregistering offsets
      bool was_empty = true;
      EXPECT_TRUE(collection.deregister_shm_pool(cur_shm_pool_id, was_empty));
      EXPECT_FALSE(was_empty);
      EXPECT_FALSE(collection.deregister_shm_pool(cur_shm_pool_id, was_empty));
      cur_object_list.clear();
    }

    // Deregister objects
    for (auto cur_object_offset : OBJECT_OFFSETS)
    {
      // If it is the first SHM pool, we deregistered the SHM pool already
      if (!is_first_shm_pool)
      {
        auto cur_object = collection.deregister_object(cur_shm_pool_id, cur_object_offset);
        EXPECT_NE(cur_object, nullptr);
        // We registered twice, so we can deregister twice
        auto cur_object_2 = collection.deregister_object(cur_shm_pool_id, cur_object_offset);
        EXPECT_EQ(cur_object, cur_object_2);
        EXPECT_NE(cur_object_list.erase(cur_object), 0UL);
      }
      EXPECT_EQ(collection.deregister_object(cur_shm_pool_id, cur_object_offset), nullptr);
    }

    if (!is_first_shm_pool)
    {
      // The list should be empty now
      EXPECT_EQ(cur_object_list.size(), 0UL);
      // Deregister SHM pool after deregistering objects
      bool was_empty = false;
      EXPECT_TRUE(collection.deregister_shm_pool(cur_shm_pool_id, was_empty));
      EXPECT_TRUE(was_empty);
      EXPECT_FALSE(collection.deregister_shm_pool(cur_shm_pool_id, was_empty));
    }
    else
    {
      is_first_shm_pool = false;
    }
  }

  // Remove all the SHM pools
  for (const auto& cur_object_map_pair : object_map)
  {
    EXPECT_TRUE(shm_pool_collection->remove_shm_pool(cur_object_map_pair.first));
  }  
}

} // namespace ipc::session::shm::arena_lend::test
