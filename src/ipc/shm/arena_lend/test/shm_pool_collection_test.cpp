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
#include "ipc/shm/arena_lend/jemalloc/jemalloc_pages.hpp"
#include "ipc/shm/arena_lend/shm_pool.hpp"
#include "ipc/shm/arena_lend/test/test_borrower.hpp"
#include "ipc/shm/arena_lend/test/test_shm_pool_collection.hpp"
#include "ipc/test/test_logger.hpp"
#include <flow/test/test_common_util.hpp>
#include <sstream>

using std::make_shared;
using std::shared_ptr;
using std::string;
using std::size_t;
using std::cout;
using ipc::test::Test_logger;
using flow::test::check_output;

namespace ipc::shm::arena_lend::test
{

namespace
{

/// Google test fixture.
class Shm_pool_collection_test :
  public ::testing::Test
{
public:
  /// Sample data used in tests.
  static const string S_ARBITRARY_DATA;
  /// Fixed shm object name for use in death tests.
  static const string S_DEATH_TEST_SHM_OBJECT_NAME;

  /// Constructor.
  Shm_pool_collection_test() :
    m_test_logger(flow::log::Sev::S_TRACE),
    m_collection(&m_test_logger)
  {
  }

  /**
   * Returns the shared memory pool collection.
   *
   * @return See above.
   */
  inline Test_shm_pool_collection& get_shm_pool_collection()
  {
    return m_collection;
  }

  /**
   * Prints the shared memory pool map and validates expected output.
   *
   * @param regex_match Regular expression match for the expected output.
   *
   * @return Whether the check passed.
   */
  bool print_shm_pool_map_test(const string& regex_match)
  {
    return check_output([&]()
                        {
                          m_collection.print_shm_pool_map();
                        }, cout, regex_match);
  }

  /**
   * Returns the memory pool size to be used in tests.
   *
   * @return See above.
   */
  static inline size_t get_pool_size()
  {
    return jemalloc::Jemalloc_pages::get_page_size();
  }

private:
  /// Used for logging purposes.
  Test_logger m_test_logger;
  /// The collection.
  Test_shm_pool_collection m_collection;
}; // class Shm_pool_collection_test

// Static constants
const string Shm_pool_collection_test::S_ARBITRARY_DATA("Shm_pool_collection_test");
const string Shm_pool_collection_test::S_DEATH_TEST_SHM_OBJECT_NAME("/" + S_ARBITRARY_DATA + "_death");

} // Anonymous namespace

/// Death tests - suffixed with DeathTest per Googletest conventions, aliased to fixture.
using Shm_pool_collection_DeathTest = Shm_pool_collection_test;
TEST_F(Shm_pool_collection_DeathTest, Interface)
{
  Test_shm_pool_collection& collection = get_shm_pool_collection();

  shared_ptr<Shm_pool> pool = collection.create_shm_pool(S_DEATH_TEST_SHM_OBJECT_NAME, get_pool_size());
  shared_ptr<Shm_pool> read_pool = collection.open_shm_pool(pool->get_name(), pool->get_size(), false);

  EXPECT_DEATH((*static_cast<char*>(read_pool->get_address()) = 'c'), ".*");

  EXPECT_TRUE(collection.remove_shm_object(S_DEATH_TEST_SHM_OBJECT_NAME));
}

/// Tests for the public and protected interfaces.
TEST_F(Shm_pool_collection_test, Interface)
{
  Test_shm_pool_collection& collection = get_shm_pool_collection();

  char* lower_pool_address = reinterpret_cast<char*>(2000);
  char* higher_pool_address = lower_pool_address + get_pool_size() + 1;
  shared_ptr<Shm_pool> lower_pool = make_shared<Shm_pool>(1, "lower", lower_pool_address, get_pool_size(), 1);
  shared_ptr<Shm_pool> higher_pool = make_shared<Shm_pool>(2, "higher", higher_pool_address, get_pool_size(), 1);

  EXPECT_EQ(collection.get_id(), Test_shm_pool_collection::S_DEFAULT_COLLECTION_ID);

  // Lookup tests
  {
    EXPECT_TRUE(collection.register_shm_pool(lower_pool));
    EXPECT_FALSE(collection.register_shm_pool(lower_pool));
    EXPECT_TRUE(collection.deregister_shm_pool(lower_pool));
    EXPECT_FALSE(collection.deregister_shm_pool(lower_pool));
    EXPECT_TRUE(collection.register_shm_pool(lower_pool));
    EXPECT_TRUE(collection.register_shm_pool(higher_pool));

    // Cover the gamut for lookup_shm_pool()
    EXPECT_EQ(collection.lookup_shm_pool(lower_pool_address - 1), nullptr);
    EXPECT_EQ(collection.lookup_shm_pool(lower_pool_address), lower_pool);
    EXPECT_EQ(collection.lookup_shm_pool(lower_pool_address + 1), lower_pool);
    EXPECT_EQ(collection.lookup_shm_pool(lower_pool_address + get_pool_size() - 1), lower_pool);
    EXPECT_EQ(collection.lookup_shm_pool(lower_pool_address + get_pool_size()), nullptr);
    EXPECT_EQ(collection.lookup_shm_pool(higher_pool_address - 1), nullptr);
    EXPECT_EQ(collection.lookup_shm_pool(higher_pool_address), higher_pool);
    EXPECT_EQ(collection.lookup_shm_pool(higher_pool_address + 1), higher_pool);
    EXPECT_EQ(collection.lookup_shm_pool(higher_pool_address + get_pool_size() - 1), higher_pool);
    EXPECT_EQ(collection.lookup_shm_pool(higher_pool_address + get_pool_size()), nullptr);

    // Cover the gamut for lookup_shm_pool_exact()
    EXPECT_EQ(collection.lookup_shm_pool_exact(lower_pool_address - 1), nullptr);
    EXPECT_EQ(collection.lookup_shm_pool_exact(lower_pool_address), lower_pool);
    EXPECT_EQ(collection.lookup_shm_pool_exact(lower_pool_address + 1), nullptr);
    EXPECT_EQ(collection.lookup_shm_pool_exact(higher_pool_address - 1), nullptr);
    EXPECT_EQ(collection.lookup_shm_pool_exact(higher_pool_address), higher_pool);
    EXPECT_EQ(collection.lookup_shm_pool_exact(higher_pool_address + 1), nullptr);

    EXPECT_TRUE(collection.deregister_shm_pool(lower_pool));
    EXPECT_EQ(collection.lookup_shm_pool(lower_pool_address), nullptr);
    EXPECT_EQ(collection.lookup_shm_pool(lower_pool_address + 1), nullptr);
    EXPECT_EQ(collection.lookup_shm_pool_exact(lower_pool_address), nullptr);

    EXPECT_TRUE(collection.deregister_shm_pool(higher_pool));
    EXPECT_EQ(collection.lookup_shm_pool(higher_pool_address), nullptr);
    EXPECT_EQ(collection.lookup_shm_pool(higher_pool_address + 1), nullptr);
    EXPECT_EQ(collection.lookup_shm_pool_exact(higher_pool_address), nullptr);
  }

  // Map tests (internal)
  {
    shared_ptr<Shm_pool> pool1 = collection.create_shm_pool(get_pool_size());
    EXPECT_NE(pool1, nullptr);
    shared_ptr<Shm_pool> pool2 = collection.create_shm_pool(get_pool_size());
    EXPECT_NE(pool2->get_address(), nullptr);
    EXPECT_NE(pool1, pool2);

    EXPECT_TRUE(collection.remove_shm_pool(pool1));
    EXPECT_TRUE(collection.remove_shm_pool(pool2));
  }

  // Access tests
  {
    shared_ptr<Shm_pool> pool = collection.create_shm_pool(get_pool_size());
    EXPECT_NE(pool, nullptr);
    strncpy(static_cast<char*>(pool->get_address()), S_ARBITRARY_DATA.c_str(), get_pool_size());

    {
      // Read only pool
      const auto read_pool = collection.open_shm_pool(pool->get_name(), pool->get_size(), false);
      EXPECT_EQ(memcmp(read_pool->get_address(), S_ARBITRARY_DATA.c_str(), S_ARBITRARY_DATA.size()), 0);
      EXPECT_EQ(memcmp(pool->get_address(), read_pool->get_address(), get_pool_size()), 0);
      EXPECT_TRUE(collection.close_shm_pool(read_pool));
    }

    {
      // Write enabled pool
      const auto write_pool = collection.open_shm_pool(pool->get_name(), pool->get_size(), true);
      EXPECT_EQ(memcmp(write_pool->get_address(), S_ARBITRARY_DATA.c_str(), S_ARBITRARY_DATA.size()), 0);
      EXPECT_EQ(memcmp(pool->get_address(), write_pool->get_address(), get_pool_size()), 0);
      EXPECT_NO_THROW(*static_cast<char*>(write_pool->get_address()) = 'c');
      EXPECT_TRUE(collection.close_shm_pool(write_pool));
    }

    EXPECT_TRUE(collection.remove_shm_pool(pool));
  }

  // Print map tests
  {
    EXPECT_TRUE(print_shm_pool_map_test("Empty SHM pool map"));
    EXPECT_TRUE(collection.register_shm_pool(higher_pool));
    EXPECT_TRUE(print_shm_pool_map_test("\\[0\\] \\[Id: " + std::to_string(higher_pool->get_id())));
    EXPECT_TRUE(collection.register_shm_pool(lower_pool));
    EXPECT_TRUE(print_shm_pool_map_test("\\[0\\] \\[Id: " + std::to_string(lower_pool->get_id()) + ".*\n"
                                        ".*\\[1\\] \\[Id: " + std::to_string(higher_pool->get_id())));
    EXPECT_TRUE(collection.deregister_shm_pool(higher_pool));
    EXPECT_TRUE(print_shm_pool_map_test("\\[0\\] \\[Id: " + std::to_string(lower_pool->get_id())));
    EXPECT_TRUE(collection.deregister_shm_pool(lower_pool));
    EXPECT_TRUE(print_shm_pool_map_test("Empty SHM pool map"));
  }
}

/// Tests to ensure that another process can read the data from the shared memory pool.
TEST_F(Shm_pool_collection_test, Multiprocess)
{
  Test_shm_pool_collection& collection = get_shm_pool_collection();

  {
    shared_ptr<Shm_pool> pool = collection.create_shm_pool(get_pool_size());
    EXPECT_NE(pool, nullptr);
    memcpy(pool->get_address(), S_ARBITRARY_DATA.c_str(), S_ARBITRARY_DATA.size());

    // In new process, open and read the data and ensure it matches the contents
    Test_borrower borrower;
    EXPECT_EQ(0, borrower.execute_read_check(collection.get_id(),
                                             pool->get_id(),
                                             pool->get_name(),
                                             get_pool_size(),
                                             0,
                                             S_ARBITRARY_DATA));

    EXPECT_TRUE(collection.remove_shm_pool(pool));
  }
}

} // namespace ipc::shm::arena_lend::test
