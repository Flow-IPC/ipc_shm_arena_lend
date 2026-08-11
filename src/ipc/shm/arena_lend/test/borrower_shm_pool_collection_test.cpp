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
#include "ipc/shm/arena_lend/borrower_shm_pool_collection.hpp"
#include "ipc/shm/arena_lend/shm_pool.hpp"
#include "ipc/shm/arena_lend/test/test_borrower.hpp"
#include "ipc/shm/arena_lend/jemalloc/test/test_jemalloc_pages.hpp"
#include "ipc/shm/arena_lend/test/test_shm_object.hpp"
#include "ipc/shm/arena_lend/test/test_shm_pool_collection.hpp"
#include "ipc/test/test_common_util.hpp"
#include "ipc/test/test_logger.hpp"
#include <flow/test/test_common_util.hpp>

using std::make_shared;
using std::shared_ptr;
using std::string;
using std::size_t;

using ipc::test::Test_logger;
using flow::test::get_test_suite_name;

namespace ipc::shm::arena_lend::test
{

namespace
{

/// Google test fixture.
class Borrower_shm_pool_collection_test :
  public ::testing::Test
{
public:
  /**
   * Returns the memory pool size to be used in tests.
   *
   * @return See above.
   */
  static inline size_t get_pool_size()
  {
    return jemalloc::Jemalloc_pages::get_page_size();
  }

  /// Constructor.
  Borrower_shm_pool_collection_test() :
    m_test_logger(flow::log::Sev::S_TRACE),
    m_owner_collection(&m_test_logger),
    /* The borrower must share the owner's pool-name base: open_shm_pool() recomputes a pool's SHM-object
     * name as base + separator + pool-ID, and the owner names its (namelessly-created) pools the same way. */
    m_borrower_collection(&m_test_logger, Test_shm_pool_collection::S_DEFAULT_COLLECTION_ID,
                          Shared_name(m_owner_collection.get_pool_name_base()))
  {
  }

  /**
   * Returns the owner shared memory pool collection.
   *
   * @return See above.
   */
  inline Test_shm_pool_collection& get_owner_collection()
  {
    return m_owner_collection;
  }

  /**
   * Returns the borrower shared memory pool collection.
   *
   * @return See above.
   */
  inline Borrower_shm_pool_collection& get_borrower_collection()
  {
    return m_borrower_collection;
  }

  /**
   * The data to be stored in the memory pool.
   *
   * @return See above.
   */
  string get_arbitrary_data() const
  {
    return get_test_suite_name();
  }

private:
  /// The logger.
  Test_logger m_test_logger;
  /// The owner shared memory pool collection.
  Test_shm_pool_collection m_owner_collection;
  /// The borrower shared memory pool collection.
  Borrower_shm_pool_collection m_borrower_collection;
}; // class Borrower_shm_pool_collection_test

} // Anonymous namespace

/// Death tests - suffixed with DeathTest per Googletest conventions, aliased to fixture.
using Borrower_shm_pool_collection_DeathTest = Borrower_shm_pool_collection_test;
TEST_F(Borrower_shm_pool_collection_DeathTest, Interface)
{
  Test_shm_pool_collection& owner_collection = get_owner_collection();
  Borrower_shm_pool_collection& borrower_collection = get_borrower_collection();

  /* Create namelessly: the owner collection then names the SHM object base + separator + pool-ID -- the
   * same name open_shm_pool() below shall recompute. */
  shared_ptr<Shm_pool> pool = owner_collection.create_shm_pool(get_pool_size());
  EXPECT_NE(pool, nullptr);
  // Read only pool
  Error_code err_code;
  shared_ptr<Shm_pool> read_pool
    = borrower_collection.open_shm_pool(pool->get_id(), pool->get_size(), &err_code);
  EXPECT_FALSE(err_code);
  ASSERT_NE(read_pool, nullptr);
  // Ensure that we cannot write
  EXPECT_DEATH((*static_cast<char*>(read_pool->get_address()) = 'c'), ".*");

  EXPECT_TRUE(remove_test_shm_objects_filesystem());
}

/// Class interface tests.
TEST_F(Borrower_shm_pool_collection_test, Interface)
{
  Test_shm_pool_collection& owner_collection = get_owner_collection();

  EXPECT_EQ(get_borrower_collection().get_id(), Test_shm_pool_collection::S_DEFAULT_COLLECTION_ID);

  // Access tests
  {
    shared_ptr<Shm_pool> pool = owner_collection.create_shm_pool(get_pool_size());
    EXPECT_NE(pool, nullptr);
    memcpy(pool->get_address(), get_arbitrary_data().c_str(), get_arbitrary_data().size());

    {
      Test_logger test_logger(flow::log::Sev::S_TRACE);
      auto borrower_collection =
        make_shared<Borrower_shm_pool_collection>(&test_logger, Test_shm_pool_collection::S_DEFAULT_COLLECTION_ID,
                                                  Shared_name(owner_collection.get_pool_name_base()));

      // Read only pool
      Error_code err_code;
      shared_ptr<Shm_pool> read_pool
        = borrower_collection->open_shm_pool(pool->get_id(), pool->get_size(), &err_code);
      EXPECT_FALSE(err_code);
      ASSERT_NE(read_pool, nullptr);
      EXPECT_EQ(memcmp(read_pool->get_address(), get_arbitrary_data().c_str(), get_arbitrary_data().size()), 0);
      EXPECT_EQ(memcmp(pool->get_address(), read_pool->get_address(), get_pool_size()), 0);
      EXPECT_TRUE(borrower_collection->release_shm_pool(read_pool));
      EXPECT_FALSE(borrower_collection->release_shm_pool(read_pool));

      // Make sure there are no SHM pools at destruction
      EXPECT_TRUE(ensure_empty_collection_at_destruction(borrower_collection));
    }

    EXPECT_TRUE(owner_collection.remove_shm_pool(pool));
  }
}

/// Ensure that a different process can read the data.
TEST_F(Borrower_shm_pool_collection_test, Multiprocess)
{
  Test_shm_pool_collection& owner_collection = get_owner_collection();

  {
    shared_ptr<Shm_pool> pool = owner_collection.create_shm_pool(get_pool_size());
    EXPECT_NE(pool, nullptr);
    memcpy(pool->get_address(), get_arbitrary_data().c_str(), get_arbitrary_data().size());

    // In new process, open and read the data and ensure it matches the contents
    Test_borrower borrower;
    EXPECT_EQ(0, borrower.execute_read_check(owner_collection.get_id(),
                                             pool->get_id(),
                                             owner_collection.get_pool_name_base().str(),
                                             get_pool_size(),
                                             0,
                                             get_arbitrary_data()));

    EXPECT_TRUE(owner_collection.remove_shm_pool(pool));
  }
}

} // namespace ipc::shm::arena_lend::test
