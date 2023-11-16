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
#include "ipc/session/standalone/shm/arena_lend/borrower_shm_pool_collection_repository.hpp"
#include "ipc/shm/arena_lend/jemalloc/jemalloc_pages.hpp"
#include "ipc/test/test_logger.hpp"
#include "ipc/shm/arena_lend/test/test_shm_object.hpp"
#include "ipc/shm/arena_lend/test/test_shm_pool_collection.hpp"

using ipc::test::Test_logger;
using std::make_shared;
using std::string;

using ipc::shm::arena_lend::test::Test_shm_pool_collection;
using ipc::shm::arena_lend::test::ensure_empty_collection_at_destruction;

namespace ipc::session::shm::arena_lend::test
{

namespace
{
const Owner_id OWNER_ID_0 = 10;
const Owner_id OWNER_ID_1 = 20;
const Owner_id UNREGISTERED_OWNER_ID = 30;
const Collection_id COLLECTION_ID_0 = 1;
const Collection_id COLLECTION_ID_1 = 2;
const Collection_id UNREGISTERED_COLLECTION_ID = 3;
const ipc::shm::arena_lend::Shm_pool::pool_id_t UNREGISTERED_SHM_POOL_ID(12121);
const string UNREGISTERED_SHM_POOL_NAME("Unregistered");
} // Anonymous namespace

/// Exercises the interface involving owner.
TEST(Borrower_shm_pool_collection_repository_test, Owner_interface)
{
  Test_logger logger;
  Borrower_shm_pool_collection_repository repository;

  repository.set_logger(&logger);
  EXPECT_EQ(repository.get_logger(), &logger);

  repository.register_owner(OWNER_ID_0);
  // Ensure we can register the same owner twice
  repository.register_owner(OWNER_ID_0);
  repository.register_owner(OWNER_ID_1);
  // Ensure that unregistered owners fail
  EXPECT_FALSE(repository.deregister_owner(UNREGISTERED_OWNER_ID));
  // Ensure that deregistering following complete deregistration fails
  EXPECT_TRUE(repository.deregister_owner(OWNER_ID_1));
  EXPECT_FALSE(repository.deregister_owner(OWNER_ID_1));
  // Ensure we can deregister an owner registered multiple times
  EXPECT_TRUE(repository.deregister_owner(OWNER_ID_0));
  EXPECT_TRUE(repository.deregister_owner(OWNER_ID_0));
  // Ensure that deregistering following complete deregistration of a multiply registered owner fails
  EXPECT_FALSE(repository.deregister_owner(OWNER_ID_0));
  // Ensure that reregistering a deregistered owner works as expected
  repository.register_owner(OWNER_ID_0);
  EXPECT_TRUE(repository.deregister_owner(OWNER_ID_0));
  EXPECT_FALSE(repository.deregister_owner(OWNER_ID_0));
}

/**
 * Exercises the interface involving collections.
 * Note: The collection suffix naming convention is: owner + "_" + index
 */
TEST(Borrower_shm_pool_collection_repository_test, Collection_interface)
{
  Test_logger logger;
  Borrower_shm_pool_collection_repository repository;

  repository.set_logger(&logger);
  EXPECT_EQ(repository.get_logger(), &logger);

  repository.register_owner(OWNER_ID_0);
  auto borrower_collection_0_0 = repository.register_collection(OWNER_ID_0, COLLECTION_ID_0);
  EXPECT_NE(borrower_collection_0_0, nullptr);
  // Ensure we can register the same owner id, collection id twice and it results in the same collection
  auto borrower_collection_0_0_copy = repository.register_collection(OWNER_ID_0, COLLECTION_ID_0);
  EXPECT_EQ(borrower_collection_0_0, borrower_collection_0_0_copy);
  auto borrower_collection_0_1 = repository.register_collection(OWNER_ID_0, COLLECTION_ID_1);
  EXPECT_NE(borrower_collection_0_1, nullptr);
  EXPECT_NE(borrower_collection_0_0, borrower_collection_0_1);

  // Ensure we can register the collection id for a different owner and it results in a different collection
  repository.register_owner(OWNER_ID_1);
  auto borrower_collection_1_0 = repository.register_collection(OWNER_ID_1, COLLECTION_ID_0);
  EXPECT_NE(borrower_collection_1_0, nullptr);
  EXPECT_NE(borrower_collection_1_0, borrower_collection_0_0);

  // Ensure that we cannot register a collection for an unregistered owner
  EXPECT_FALSE(repository.register_collection(UNREGISTERED_OWNER_ID, COLLECTION_ID_0));

  // Ensure that deregistering an unregistered collection fails
  EXPECT_FALSE(repository.deregister_collection(UNREGISTERED_OWNER_ID, UNREGISTERED_COLLECTION_ID));
  EXPECT_FALSE(repository.deregister_collection(UNREGISTERED_OWNER_ID, COLLECTION_ID_0));
  EXPECT_FALSE(repository.deregister_collection(OWNER_ID_0, UNREGISTERED_COLLECTION_ID));

  // Ensure that we can deregister a collection the same amount of times we registered it
  EXPECT_TRUE(repository.deregister_collection(OWNER_ID_0, COLLECTION_ID_0));
  EXPECT_TRUE(repository.deregister_collection(OWNER_ID_0, COLLECTION_ID_0));
  EXPECT_FALSE(repository.deregister_collection(OWNER_ID_0, COLLECTION_ID_0));
  EXPECT_TRUE(repository.deregister_collection(OWNER_ID_0, COLLECTION_ID_1));
  EXPECT_FALSE(repository.deregister_collection(OWNER_ID_0, COLLECTION_ID_1));

  // Ensure that we can reregister a collection after complete deregistration. It may result in the same pointer.
  auto borrower_collection_0_0_reregister = repository.register_collection(OWNER_ID_0, COLLECTION_ID_0);
  EXPECT_NE(borrower_collection_0_0_reregister, nullptr);
  EXPECT_TRUE(repository.deregister_collection(OWNER_ID_0, COLLECTION_ID_0));
  EXPECT_FALSE(repository.deregister_collection(OWNER_ID_0, COLLECTION_ID_0));

  // Ensure that we can register a collection if the owner deregisters, but not completely
  repository.register_owner(OWNER_ID_0);
  repository.deregister_owner(OWNER_ID_0);
  auto borrower_collection_0_1_reregister = repository.register_collection(OWNER_ID_0, COLLECTION_ID_1);
  EXPECT_NE(borrower_collection_0_0_reregister, nullptr);
  EXPECT_TRUE(repository.deregister_collection(OWNER_ID_0, COLLECTION_ID_1));
  EXPECT_FALSE(repository.deregister_collection(OWNER_ID_0, COLLECTION_ID_1));

  // Ensure that we cannot register a collection after the owner is deregistered completely
  repository.deregister_owner(OWNER_ID_0);
  auto borrower_collection_0_0_owner_deregistered = repository.register_collection(OWNER_ID_0, COLLECTION_ID_0);
  EXPECT_EQ(borrower_collection_0_0_owner_deregistered, nullptr);

  // Cleanup
  EXPECT_TRUE(repository.deregister_collection(OWNER_ID_1, COLLECTION_ID_0));
  repository.deregister_owner(OWNER_ID_1);
}

/**
 * Exercises the interface involving shared memory pools.
 *
 * Note: The collection suffix naming convention is: owner + "_" + index
 * Note: The shared memory pool suffix naming convention is: owner + "_" + collection + "_" + index
 */
TEST(Borrower_shm_pool_collection_repository_test, Pool_interface)
{
  Test_logger logger;
  Borrower_shm_pool_collection_repository repository;

  repository.set_logger(&logger);
  EXPECT_EQ(repository.get_logger(), &logger);

  const auto SHM_POOL_SIZE = ipc::shm::arena_lend::jemalloc::Jemalloc_pages::get_page_size();

  // Create memory pools
  auto owner_collection_0_0 = make_shared<Test_shm_pool_collection>(&logger, COLLECTION_ID_0);
  auto owner_collection_0_1 = make_shared<Test_shm_pool_collection>(&logger, COLLECTION_ID_1);
  auto owner_collection_1_0 = make_shared<Test_shm_pool_collection>(&logger, COLLECTION_ID_0);
  auto owner_shm_pool_0_0_0 = owner_collection_0_0->create_shm_pool(SHM_POOL_SIZE);
  auto owner_shm_pool_0_0_1 = owner_collection_0_0->create_shm_pool(SHM_POOL_SIZE);
  auto owner_shm_pool_0_1_0 = owner_collection_0_1->create_shm_pool(SHM_POOL_SIZE);
  auto owner_shm_pool_1_0_0 = owner_collection_1_0->create_shm_pool(SHM_POOL_SIZE);

  // Register owners
  repository.register_owner(OWNER_ID_0);
  repository.register_owner(OWNER_ID_1);

  // Register collections
  auto borrower_collection_0_0 = repository.register_collection(OWNER_ID_0, COLLECTION_ID_0);
  EXPECT_NE(borrower_collection_0_0, nullptr);
  auto borrower_collection_0_1 = repository.register_collection(OWNER_ID_0, COLLECTION_ID_1);
  EXPECT_NE(borrower_collection_0_1, nullptr);
  auto borrower_collection_1_0 = repository.register_collection(OWNER_ID_1, COLLECTION_ID_0);
  EXPECT_NE(borrower_collection_1_0, nullptr);

  // Register pools
  auto borrower_shm_pool_0_0_0 = repository.register_shm_pool(OWNER_ID_0,
                                                              COLLECTION_ID_0,
                                                              owner_shm_pool_0_0_0->get_id(),
                                                              owner_shm_pool_0_0_0->get_name(),
                                                              SHM_POOL_SIZE);
  EXPECT_NE(borrower_shm_pool_0_0_0, nullptr);
  // Ensure registering with the information results in the same pool
  auto borrower_shm_pool_0_0_0_copy = repository.register_shm_pool(OWNER_ID_0,
                                                                   COLLECTION_ID_0,
                                                                   owner_shm_pool_0_0_0->get_id(),
                                                                   owner_shm_pool_0_0_0->get_name(),
                                                                   SHM_POOL_SIZE);
  EXPECT_EQ(borrower_shm_pool_0_0_0, borrower_shm_pool_0_0_0_copy);
  auto borrower_shm_pool_0_0_1 = repository.register_shm_pool(OWNER_ID_0,
                                                              COLLECTION_ID_0,
                                                              owner_shm_pool_0_0_1->get_id(),
                                                              owner_shm_pool_0_0_1->get_name(),
                                                              SHM_POOL_SIZE);
  EXPECT_NE(borrower_shm_pool_0_0_1, nullptr);
  EXPECT_NE(borrower_shm_pool_0_0_1, borrower_shm_pool_0_0_0);
  auto borrower_shm_pool_0_1_0 = repository.register_shm_pool(OWNER_ID_0,
                                                              COLLECTION_ID_1,
                                                              owner_shm_pool_0_1_0->get_id(),
                                                              owner_shm_pool_0_1_0->get_name(),
                                                              SHM_POOL_SIZE);
  EXPECT_NE(borrower_shm_pool_0_0_1, nullptr);
  EXPECT_NE(borrower_shm_pool_0_0_1, borrower_shm_pool_0_0_0);
  // Ensure registering with a different owner succeeds
  auto borrower_shm_pool_1_0_0 = repository.register_shm_pool(OWNER_ID_1,
                                                              COLLECTION_ID_0,
                                                              owner_shm_pool_1_0_0->get_id(),
                                                              owner_shm_pool_1_0_0->get_name(),
                                                              SHM_POOL_SIZE);
  EXPECT_NE(borrower_shm_pool_1_0_0, nullptr);
  EXPECT_NE(borrower_shm_pool_1_0_0, borrower_shm_pool_0_0_0);

  // Ensure registration of a pool in an unregistered owner fails
  EXPECT_EQ(repository.register_shm_pool(UNREGISTERED_OWNER_ID,
                                         COLLECTION_ID_0,
                                         owner_shm_pool_0_0_0->get_id(),
                                         owner_shm_pool_0_0_0->get_name(),
                                         SHM_POOL_SIZE),
            nullptr);
  // Ensure registration of a pool in a unregistered collection fails
  EXPECT_EQ(repository.register_shm_pool(OWNER_ID_0,
                                         UNREGISTERED_COLLECTION_ID,
                                         owner_shm_pool_0_0_0->get_id(),
                                         owner_shm_pool_0_0_0->get_name(),
                                         SHM_POOL_SIZE),
            nullptr);
  // Ensure registration of a nonexistent pool fails
  EXPECT_EQ(repository.register_shm_pool(OWNER_ID_0, COLLECTION_ID_0,
                                         UNREGISTERED_SHM_POOL_ID, UNREGISTERED_SHM_POOL_NAME, SHM_POOL_SIZE),
            nullptr);

  // Ensure that conversion works
  void* address_0_0_0 = repository.to_address(borrower_shm_pool_0_0_0->get_id(), 0UL);
  EXPECT_EQ(address_0_0_0, borrower_shm_pool_0_0_0->get_address());
  void* address_0_0_1 = repository.to_address(borrower_shm_pool_0_0_1->get_id(), 0UL);
  EXPECT_EQ(address_0_0_1, borrower_shm_pool_0_0_1->get_address());
  void* address_0_1_0 = repository.to_address(borrower_shm_pool_0_1_0->get_id(), 0UL);
  EXPECT_EQ(address_0_1_0, borrower_shm_pool_0_1_0->get_address());
  void* address_1_0_0 = repository.to_address(borrower_shm_pool_1_0_0->get_id(), 0UL);
  EXPECT_EQ(address_1_0_0, borrower_shm_pool_1_0_0->get_address());
  // Ensure conversion of unregistered pool fails
  EXPECT_EQ(repository.to_address(UNREGISTERED_SHM_POOL_ID, 0UL), nullptr);

  // Ensure deregistration of an unregistered pool fails
  EXPECT_FALSE(repository.deregister_shm_pool(OWNER_ID_0, COLLECTION_ID_0, UNREGISTERED_SHM_POOL_ID));
  // Ensure deregistration of a pool in a unregistered owner fails
  EXPECT_FALSE(
    repository.deregister_shm_pool(UNREGISTERED_OWNER_ID, COLLECTION_ID_0, borrower_shm_pool_0_0_0->get_id()));
  // Ensure deregistration of a pool in a unregistered collection fails
  EXPECT_FALSE(
    repository.deregister_shm_pool(OWNER_ID_0, UNREGISTERED_COLLECTION_ID, borrower_shm_pool_0_0_0->get_id()));
  // Ensure deregistration of a pool registered in another owner fails
  EXPECT_FALSE(repository.deregister_shm_pool(OWNER_ID_0, COLLECTION_ID_0, borrower_shm_pool_1_0_0->get_id()));
  // Ensure deregistration of a pool registered in another collection fails
  EXPECT_FALSE(repository.deregister_shm_pool(OWNER_ID_0, COLLECTION_ID_0, borrower_shm_pool_0_1_0->get_id()));
  EXPECT_FALSE(repository.deregister_shm_pool(OWNER_ID_1, COLLECTION_ID_0, borrower_shm_pool_0_0_0->get_id()));
  EXPECT_FALSE(repository.deregister_shm_pool(OWNER_ID_1, COLLECTION_ID_0, borrower_shm_pool_0_0_1->get_id()));

  // Deregister the first of two registrations
  EXPECT_TRUE(repository.deregister_shm_pool(OWNER_ID_0, COLLECTION_ID_0, borrower_shm_pool_0_0_0->get_id()));
  // Ensure conversion of pool still succeeds
  void* address_0_0_0_copy = repository.to_address(borrower_shm_pool_0_0_0->get_id(), 0UL);
  EXPECT_EQ(address_0_0_0_copy, address_0_0_0);
  // Ensure deregistration of a multiply registered pool succeeds
  EXPECT_TRUE(repository.deregister_shm_pool(OWNER_ID_0, COLLECTION_ID_0, borrower_shm_pool_0_0_0->get_id()));
  EXPECT_FALSE(repository.deregister_shm_pool(OWNER_ID_0, COLLECTION_ID_0, borrower_shm_pool_0_0_0->get_id()));
  // Ensure conversion of pool fails
  EXPECT_EQ(repository.to_address(borrower_shm_pool_0_0_0->get_id(), 0UL), nullptr);

  EXPECT_TRUE(repository.deregister_shm_pool(OWNER_ID_0, COLLECTION_ID_0, borrower_shm_pool_0_0_1->get_id()));
  EXPECT_FALSE(repository.deregister_shm_pool(OWNER_ID_0, COLLECTION_ID_0, borrower_shm_pool_0_0_1->get_id()));
  EXPECT_EQ(repository.to_address(borrower_shm_pool_0_0_1->get_id(), 0UL), nullptr);

  EXPECT_TRUE(repository.deregister_shm_pool(OWNER_ID_0, COLLECTION_ID_1, borrower_shm_pool_0_1_0->get_id()));
  EXPECT_FALSE(repository.deregister_shm_pool(OWNER_ID_0, COLLECTION_ID_1, borrower_shm_pool_0_1_0->get_id()));
  EXPECT_EQ(repository.to_address(borrower_shm_pool_0_1_0->get_id(), 0UL), nullptr);

  EXPECT_TRUE(repository.deregister_shm_pool(OWNER_ID_1, COLLECTION_ID_0, borrower_shm_pool_1_0_0->get_id()));
  EXPECT_FALSE(repository.deregister_shm_pool(OWNER_ID_1, COLLECTION_ID_0, borrower_shm_pool_1_0_0->get_id()));
  EXPECT_EQ(repository.to_address(borrower_shm_pool_1_0_0->get_id(), 0UL), nullptr);

  // Ensure reregistration succeeds; note that the address may or may not be similar
  auto borrower_shm_pool_0_0_0_reregister = repository.register_shm_pool(OWNER_ID_0,
                                                                         COLLECTION_ID_0,
                                                                         owner_shm_pool_0_0_0->get_id(),
                                                                         owner_shm_pool_0_0_0->get_name(),
                                                                         SHM_POOL_SIZE);
  EXPECT_NE(borrower_shm_pool_0_0_0_reregister, nullptr);
  EXPECT_TRUE(repository.deregister_shm_pool(OWNER_ID_0,
                                             COLLECTION_ID_0,
                                             borrower_shm_pool_0_0_0_reregister->get_id()));

  // Deregister collections and owner
  EXPECT_TRUE(repository.deregister_collection(OWNER_ID_1, COLLECTION_ID_0));
  EXPECT_TRUE(repository.deregister_collection(OWNER_ID_0, COLLECTION_ID_1));
  EXPECT_TRUE(repository.deregister_collection(OWNER_ID_0, COLLECTION_ID_0));
  EXPECT_TRUE(repository.deregister_owner(OWNER_ID_1));
  EXPECT_TRUE(repository.deregister_owner(OWNER_ID_0));

  // Remove memory pools
  EXPECT_TRUE(owner_collection_1_0->remove_shm_pool(owner_shm_pool_1_0_0));
  EXPECT_TRUE(owner_collection_0_1->remove_shm_pool(owner_shm_pool_0_1_0));
  EXPECT_TRUE(owner_collection_0_0->remove_shm_pool(owner_shm_pool_0_0_1));
  EXPECT_TRUE(owner_collection_0_0->remove_shm_pool(owner_shm_pool_0_0_0));

  // Ensure the collections are empty
  EXPECT_TRUE(ensure_empty_collection_at_destruction(borrower_collection_0_0));
  EXPECT_TRUE(ensure_empty_collection_at_destruction(borrower_collection_0_1));
  EXPECT_TRUE(ensure_empty_collection_at_destruction(borrower_collection_1_0));
  EXPECT_TRUE(ensure_empty_collection_at_destruction(owner_collection_0_0));
  EXPECT_TRUE(ensure_empty_collection_at_destruction(owner_collection_0_1));
  EXPECT_TRUE(ensure_empty_collection_at_destruction(owner_collection_1_0));
}

} // namespace ipc::session::shm::arena_lend::test
