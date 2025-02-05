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
    m_borrower_collection(&m_test_logger, Test_shm_pool_collection::S_DEFAULT_COLLECTION_ID)
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

  Owner_shm_pool_collection::Shm_object_name_generator name_generator = create_shm_object_name_generator();
  shared_ptr<Shm_pool> pool = owner_collection.create_shm_pool(name_generator(0 /* ignored by us */), get_pool_size());
  EXPECT_NE(pool, nullptr);
  // Read only pool
  shared_ptr<Shm_pool> read_pool
    = borrower_collection.open_shm_pool(pool->get_id(), pool->get_name(), pool->get_size());
  // Ensure that we cannot write
  EXPECT_DEATH((*static_cast<char*>(read_pool->get_address()) = 'c'), ".*");

  EXPECT_TRUE(remove_test_shm_objects_filesystem());
}

/// Class interface tests.
TEST_F(Borrower_shm_pool_collection_test, Interface)
{
  Test_shm_pool_collection& owner_collection = get_owner_collection();

  // Access tests
  {
    shared_ptr<Shm_pool> pool = owner_collection.create_shm_pool(get_pool_size());
    EXPECT_NE(pool, nullptr);
    memcpy(pool->get_address(), get_arbitrary_data().c_str(), get_arbitrary_data().size());

    {
      Test_logger test_logger(flow::log::Sev::S_TRACE);
      auto borrower_collection =
        make_shared<Borrower_shm_pool_collection>(&test_logger, Test_shm_pool_collection::S_DEFAULT_COLLECTION_ID);

      // Read only pool
      shared_ptr<Shm_pool> read_pool
        = borrower_collection->open_shm_pool(pool->get_id(), pool->get_name(), pool->get_size());
      EXPECT_EQ(memcmp(read_pool->get_address(), get_arbitrary_data().c_str(), get_arbitrary_data().size()), 0);
      EXPECT_EQ(memcmp(pool->get_address(), read_pool->get_address(), get_pool_size()), 0);
      EXPECT_TRUE(borrower_collection->release_shm_pool(read_pool));
      EXPECT_FALSE(borrower_collection->release_shm_pool(read_pool));

      // Make sure there are no SHM pools at destruction
      EXPECT_TRUE(ensure_empty_collection_at_destruction(borrower_collection));
    }

    EXPECT_TRUE(owner_collection.remove_shm_pool(pool));
  }

  // Create shared object tests
  {
    Borrower_shm_pool_collection& borrower_collection = get_borrower_collection();

    EXPECT_EQ(borrower_collection.get_id(), Test_shm_pool_collection::S_DEFAULT_COLLECTION_ID);

    // Ensure that the specified callback is executed properly and that the underlying object is not deleted
    shared_ptr<bool> destructor_executed = make_shared<bool>(false);

    // Test class
    class Foo
    {
     public:
      Foo(shared_ptr<bool>& destructor_executed) :
        m_destructor_executed(destructor_executed)
      {
        *m_destructor_executed = false;
      }

      ~Foo()
      {
        *m_destructor_executed = true;
      }

     private:
      shared_ptr<bool> m_destructor_executed;
    }; // class Foo
    Foo* foo = new Foo(destructor_executed);

    bool callback_executed = false;
    {
      // Create object
      shared_ptr<Foo> foo_ptr = borrower_collection.construct<Foo>(foo,
                                                                   [&](void* p)
                                                                   {
                                                                     callback_executed = true;
                                                                     EXPECT_EQ(p, foo);
                                                                   });
      EXPECT_NE(foo_ptr, nullptr);
      EXPECT_FALSE(callback_executed);
      // Ensure deletion callback is executed, but object should not be destroyed
      foo_ptr = nullptr;
      EXPECT_TRUE(callback_executed);
      EXPECT_FALSE(*destructor_executed);
    }

    // Reset variables
    callback_executed = false;
    *destructor_executed = false;
    {
      // Create object
      shared_ptr<Foo> foo_ptr = borrower_collection.construct<Foo>(foo,
                                                                   0UL,
                                                                   [&](void* p)
                                                                   {
                                                                     callback_executed = true;
                                                                     EXPECT_EQ(p, foo);
                                                                   });
      EXPECT_NE(foo_ptr, nullptr);
      EXPECT_FALSE(callback_executed);
      // Ensure deletion callback is executed, but object should not be destroyed
      foo_ptr = nullptr;
      EXPECT_TRUE(callback_executed);
      EXPECT_FALSE(*destructor_executed);
    }

    // Actual object deletion occurs here
    *destructor_executed = false;
    delete foo;
    EXPECT_TRUE(*destructor_executed);
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
                                             pool->get_name(),
                                             get_pool_size(),
                                             0,
                                             get_arbitrary_data()));

    EXPECT_TRUE(owner_collection.remove_shm_pool(pool));
  }
}

} // namespace ipc::shm::arena_lend::test
