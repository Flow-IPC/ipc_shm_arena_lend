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
#include "ipc/shm/arena_lend/jemalloc/test/test_jemalloc_pages.hpp"
#include "ipc/shm/arena_lend/test/test_shm_pool_collection.hpp"
#include "ipc/shm/arena_lend/test/test_shm_object.hpp"
#include "ipc/test/test_logger.hpp"
#include <flow/test/test_common_util.hpp>
#include <sys/time.h>
#include <sys/resource.h>

using ipc::test::Test_logger;
using std::size_t;
using std::string;

using namespace flow::test;
using namespace ipc::shm::arena_lend::test;

namespace ipc::shm::arena_lend::jemalloc::test
{

/// Google test fixture
class Jemalloc_pages_test :
  public ::testing::Test
{
public:
  /// Expected page size.
  static constexpr size_t S_PAGE_SIZE = 4096;
  /// Huge page size.
  static constexpr size_t S_HUGE_PAGE_SIZE = 2 * 1024 * 1024;
  /// Identifier stating that no file descriptor was created or is being used.
  static constexpr int S_NO_FD = -1;

  /// Constructor.
  Jemalloc_pages_test() :
    m_test_logger(flow::log::Sev::S_TRACE),
    m_collection(&m_test_logger)
  {
  }

  /**
   * Returns the collection created for this test.
   *
   * @return Ditto.
   */
  Test_shm_pool_collection& get_collection()
  {
    return m_collection;
  }

  /**
   * Executes a test that does the following:
   * 1. Creates a memory mapped region
   * 2. Fills the region with bytes
   * 3. Removes the memory mapped region
   *
   * @param address The address to create the region; a nullptr indicates letting the system choose.
   * @param size The size of the region to map; this value must be page aligned.
   * @param alignment The alignment that the address of the region should have.
   * @param fd The file descriptor to map the region to.
   *
   * @return The address of the pool that was created and subsequently removed or nullptr, if a pool was not
   *         successfully created.
   */
  void* map_test(void* address, size_t size, size_t alignment, int fd)
  {
    bool commit = true;
    void* pool = Jemalloc_pages::map(address, size, alignment, commit, fd);
    if (pool == nullptr)
    {
      EXPECT_NE(pool, nullptr);
      return nullptr;
    }

    EXPECT_EQ((reinterpret_cast<size_t>(pool) & (alignment - 1)), 0UL);

    if (commit)
    {
      write_test(pool, size);
    }
    else
    {
      // Fail
      EXPECT_TRUE(commit);
    }

    Jemalloc_pages::unmap(pool, size);

    return pool;
  }

  /**
   * Fills in a region with bytes.
   *
   * @param start_address The starting address of the region.
   * @param size The number of bytes to fill the region with.
   */
  void write_test(void* start_address, size_t size)
  {
    // Ensure the entirety of the map is writable
    string s(S_PAGE_SIZE, 'a');
    char* p = static_cast<char*>(start_address);
    size_t remaining_size = size;
    while (remaining_size > 0)
    {
      size_t cur_write_size = (remaining_size >= S_PAGE_SIZE) ? S_PAGE_SIZE : remaining_size;
      remaining_size -= cur_write_size;
      memcpy(p, s.c_str(), cur_write_size);
      p += cur_write_size;
    }
  }

  /**
   * Checks that a memory region is filled with zeroes.
   *
   * @param start_address The starting address of the region.
   * @param size The region size.
   *
   * @return Whether the region is filled with zeroes.
   */
  bool check_zeroes(void* start_address, size_t size)
  {
    // Ensure the entirety of the region is zeroed
    string s(S_PAGE_SIZE, '\0');
    char* p = static_cast<char*>(start_address);
    size_t remaining_size = size;
    while (remaining_size > 0)
    {
      size_t cur_read_size = (remaining_size >= S_PAGE_SIZE) ? S_PAGE_SIZE : remaining_size;
      remaining_size -= cur_read_size;
      if (memcmp(p, s.c_str(), cur_read_size) != 0)
      {
        return false;
      }
      p += cur_read_size;
    }

    return true;
  }

private:
  /// For logging purposes.
  Test_logger m_test_logger;
  /// The collection to use in certain tests.
  Test_shm_pool_collection m_collection;
}; // class Jemalloc_pages_test

/// Death tests - suffixed with DeathTest per Googletest conventions, aliased to fixture.
using Jemalloc_pages_DeathTest = Jemalloc_pages_test;
#ifdef NDEBUG // These "deaths" occur only if assert()s enabled; else these are guaranteed failures.
TEST_F(Jemalloc_pages_DeathTest, DISABLED_Interface)
#else
TEST_F(Jemalloc_pages_DeathTest, Interface)
#endif
{
  bool commit;

  Owner_shm_pool_collection::Shm_object_name_generator name_generator = create_shm_object_name_generator();
  string shm_object_name = name_generator(0 /* ignored anyway by our generator */);
  Test_shm_pool_collection& collection = get_collection();
  int fd = collection.create_shm_object(shm_object_name, S_PAGE_SIZE);
  EXPECT_NE(fd, S_NO_FD);

  // Map death tests
  {
    // Bad address for alignment
    EXPECT_DEATH(Jemalloc_pages::map(reinterpret_cast<void*>(S_PAGE_SIZE + 1),
                                     S_PAGE_SIZE,
                                     S_PAGE_SIZE,
                                     commit,
                                     fd),
                 "ALIGNMENT_ADDR2BASE\\(addr, alignment\\) == addr");
    // Size is not a multiple of page size
    EXPECT_DEATH(Jemalloc_pages::map(nullptr,
                                     S_PAGE_SIZE - 1,
                                     S_PAGE_SIZE,
                                     commit,
                                     fd),
                 "ALIGNMENT_CEILING\\(size, os_page\\) == size");
    // Alignment < page size
    EXPECT_DEATH(Jemalloc_pages::map(nullptr,
                                     S_PAGE_SIZE,
                                     S_PAGE_SIZE - 1,
                                     commit,
                                     fd),
                 "alignment >= PAGE");
    // Alignment < page size (second case with fixed address)
    EXPECT_DEATH(Jemalloc_pages::map(reinterpret_cast<void*>(S_PAGE_SIZE),
                                     S_PAGE_SIZE,
                                     S_PAGE_SIZE - 1,
                                     commit,
                                     fd),
                 "alignment >= PAGE");
  }

  // Commit death tests
  {
    // Unaligned address (should be on page size boundary)
    EXPECT_DEATH(Jemalloc_pages::commit(reinterpret_cast<void*>(0x1), S_PAGE_SIZE),
                 "PAGE_ADDR2BASE\\(address\\) == address");
    // Size is not a multiple of page size
    EXPECT_DEATH(Jemalloc_pages::commit(reinterpret_cast<void*>(S_PAGE_SIZE), (S_PAGE_SIZE - 1)),
                 "PAGE_CEILING\\(size\\) == size");
  }

  // Decommit death tests
  {
    // Unaligned address (should be on page size boundary)
    EXPECT_DEATH(Jemalloc_pages::decommit(reinterpret_cast<void*>(0x1), fd, 0, S_PAGE_SIZE),
                 "PAGE_ADDR2BASE\\(address\\) == address");
    // Invalid file descriptor
    EXPECT_DEATH(Jemalloc_pages::decommit(reinterpret_cast<void*>(S_PAGE_SIZE), -1, 0, S_PAGE_SIZE),
                 "fd != -1");
    // Offset is not a multiple of page size
    EXPECT_DEATH(Jemalloc_pages::decommit(reinterpret_cast<void*>(S_PAGE_SIZE), fd, 1, S_PAGE_SIZE),
                 "PAGE_CEILING\\(file_offset\\) == file_offset");
    // Size is not a multiple of page size
    EXPECT_DEATH(Jemalloc_pages::decommit(reinterpret_cast<void*>(S_PAGE_SIZE), fd, 0, (S_PAGE_SIZE - 1)),
                 "PAGE_CEILING\\(size\\) == size");
  }

  // Commit and decommit
  {
    // Get flag
    bool overcommit = Test_jemalloc_pages::get_os_overcommit_memory();

    {
      // Commit should follow overcommit flag if commit is false
      commit = false;
      Test_jemalloc_pages::set_os_overcommit_memory(false);
      void* p = Jemalloc_pages::map(nullptr, S_PAGE_SIZE, S_PAGE_SIZE, commit, fd);
      EXPECT_NE(p, nullptr);
      EXPECT_FALSE(commit);
      // Memory not committed, so no read/write should be possible
      EXPECT_DEATH(write_test(p, 1), "");
      EXPECT_TRUE(Jemalloc_pages::commit(p, S_PAGE_SIZE));
      // Should be able to write
      EXPECT_NO_THROW(write_test(p, S_PAGE_SIZE));
      EXPECT_TRUE(Jemalloc_pages::decommit(p, fd, 0, S_PAGE_SIZE));
      // Should not be able to write
      EXPECT_DEATH(write_test(p, 1), "");
      EXPECT_TRUE(Jemalloc_pages::commit(p, S_PAGE_SIZE));
      // Should be able to write
      EXPECT_NO_THROW(write_test(p, S_PAGE_SIZE));

      Jemalloc_pages::unmap(p, S_PAGE_SIZE);
    }

    {
      commit = false;
      Test_jemalloc_pages::set_os_overcommit_memory(true);
      void* p = Jemalloc_pages::map(nullptr, S_PAGE_SIZE, S_PAGE_SIZE, commit, fd);
      EXPECT_NE(p, nullptr);
      EXPECT_TRUE(commit);
      // Memory committed, so read/write should be possible
      EXPECT_NO_THROW(write_test(p, S_PAGE_SIZE));
      // Overcommit without force is a no-op
      EXPECT_FALSE(Jemalloc_pages::decommit(p, fd, 0, S_PAGE_SIZE, false));
      EXPECT_NO_THROW(write_test(p, S_PAGE_SIZE));
      // Force flag will decommit the page
      EXPECT_TRUE(Jemalloc_pages::decommit(p, fd, 0, S_PAGE_SIZE, true));
      // Should not be able to write
      EXPECT_DEATH(write_test(p, 1), "");
      // Recommit is not possible due to overcommit
      EXPECT_FALSE(Jemalloc_pages::commit(p, S_PAGE_SIZE));
      // Should not be able to write
      EXPECT_DEATH(write_test(p, 1), "");

      Jemalloc_pages::unmap(p, S_PAGE_SIZE);
    }

    // Restore flag
    Test_jemalloc_pages::set_os_overcommit_memory(overcommit);
  }

  // Purge forced death tests
  {
    // Unaligned offset (should be on page size boundary)
    EXPECT_DEATH(Jemalloc_pages::purge_forced(fd, 1, S_PAGE_SIZE),
                 "PAGE_CEILING\\(file_offset\\) == file_offset");
    // Size is not a multiple of page size
    EXPECT_DEATH(Jemalloc_pages::purge_forced(fd, 0, (S_PAGE_SIZE - 1)),
                 "PAGE_CEILING\\(size\\) == size");
  }

  // Unmap death tests
  {
    // Invalid address
    EXPECT_DEATH(Jemalloc_pages::unmap(reinterpret_cast<void*>(-S_PAGE_SIZE), S_PAGE_SIZE),
                 "Could not unmap");

    void* p = Jemalloc_pages::map(nullptr, S_PAGE_SIZE, S_PAGE_SIZE, commit, fd);
    // Unaligned address (should be on page size boundary)
    EXPECT_DEATH(Jemalloc_pages::unmap((static_cast<char*>(p) + 1), S_PAGE_SIZE),
                 "PAGE_ADDR2BASE\\(addr\\) == addr");
    // Size is not a multiple of page size
    EXPECT_DEATH(Jemalloc_pages::unmap(p, (S_PAGE_SIZE - 1)),
                 "PAGE_CEILING\\(size\\) == size");
    Jemalloc_pages::unmap(p, S_PAGE_SIZE);
  }

  EXPECT_EQ(close(fd), 0);
  EXPECT_TRUE(collection.remove_shm_object(shm_object_name));

  // Remove shared memory pools that were created
  EXPECT_TRUE(remove_test_shm_objects_filesystem());
}

/// Tests involving the class interface, both public and protected.
TEST_F(Jemalloc_pages_test, Interface)
{
  void* INVALID_ADDRESS = reinterpret_cast<void*>(S_PAGE_SIZE);

  bool commit;

  EXPECT_EQ(Jemalloc_pages::get_page_size(), S_PAGE_SIZE);
  EXPECT_EQ(Jemalloc_pages::get_page_mask(), (S_PAGE_SIZE - 1));

  Test_shm_pool_collection& collection = get_collection();
  Owner_shm_pool_collection::Shm_object_name_generator name_generator = create_shm_object_name_generator();
  string shm_object_name = name_generator(0 /* ignored anyway by our generator */);
  int fd = collection.create_shm_object(shm_object_name, S_HUGE_PAGE_SIZE);
  EXPECT_NE(fd, S_NO_FD);

  // Invalid address to map to
  EXPECT_EQ(Jemalloc_pages::map(INVALID_ADDRESS, S_PAGE_SIZE, S_PAGE_SIZE, commit, fd), nullptr);
  // System chosen address
  void* fixed_address;
  void* huge_page_fixed_address;
  {
    SCOPED_TRACE("System chosen address, page size with page size alignment");
    fixed_address = map_test(nullptr, S_PAGE_SIZE, S_PAGE_SIZE, fd);
  }
  {
    SCOPED_TRACE("System chosen address, huge page size with page size alignment");
    map_test(nullptr, S_HUGE_PAGE_SIZE, S_PAGE_SIZE, fd);
  }
  {
    SCOPED_TRACE("System chosen address, huge page size with huge page size alignment");
    huge_page_fixed_address = map_test(nullptr, S_HUGE_PAGE_SIZE, S_HUGE_PAGE_SIZE, fd);
  }

  if (fixed_address != nullptr)
  {
    SCOPED_TRACE("Fixed address, page size with page size alignment");
    map_test(fixed_address, S_PAGE_SIZE, S_PAGE_SIZE, fd);
  }
  else
  {
    ADD_FAILURE() << "Skipped test due to fixed_address being nullptr";
  }
  if (huge_page_fixed_address != nullptr)
  {
    SCOPED_TRACE("Fixed huge page address, page size with huge page size alignment");
    map_test(huge_page_fixed_address, S_PAGE_SIZE, S_HUGE_PAGE_SIZE, fd);
  }
  else
  {
    ADD_FAILURE() << "Skipped test due to huge_page_fixed_address being nullptr";
  }

  // Check commit flag after mapping
  {
    // Get flag
    bool overcommit = Test_jemalloc_pages::get_os_overcommit_memory();

    // When commit is true, resulting commit is true
    commit = true;
    Test_jemalloc_pages::set_os_overcommit_memory(false);
    void* p = Jemalloc_pages::map(nullptr, S_PAGE_SIZE, S_PAGE_SIZE, commit, fd);
    EXPECT_NE(p, nullptr);
    EXPECT_TRUE(commit);
    Jemalloc_pages::unmap(p, S_PAGE_SIZE);

    commit = true;
    Test_jemalloc_pages::set_os_overcommit_memory(true);
    p = Jemalloc_pages::map(nullptr, S_PAGE_SIZE, S_PAGE_SIZE, commit, fd);
    EXPECT_NE(p, nullptr);
    EXPECT_TRUE(commit);
    Jemalloc_pages::unmap(p, S_PAGE_SIZE);

    // When commit is false, resulting commit should follow overcommit flag
    commit = false;
    Test_jemalloc_pages::set_os_overcommit_memory(false);
    p = Jemalloc_pages::map(nullptr, S_PAGE_SIZE, S_PAGE_SIZE, commit, fd);
    EXPECT_NE(p, nullptr);
    EXPECT_FALSE(commit);
    Jemalloc_pages::unmap(p, S_PAGE_SIZE);

    commit = false;
    Test_jemalloc_pages::set_os_overcommit_memory(true);
    p = Jemalloc_pages::map(nullptr, S_PAGE_SIZE, S_PAGE_SIZE, commit, fd);
    EXPECT_NE(p, nullptr);
    EXPECT_TRUE(commit);
    Jemalloc_pages::unmap(p, S_PAGE_SIZE);

    // Restore flag
    Test_jemalloc_pages::set_os_overcommit_memory(overcommit);
  }

  {
    // Get flag
    bool overcommit = Test_jemalloc_pages::get_os_overcommit_memory();

    // When commit is true, resulting commit is true
    commit = true;
    Test_jemalloc_pages::set_os_overcommit_memory(false);
    void* p = Jemalloc_pages::map(nullptr, S_HUGE_PAGE_SIZE, S_PAGE_SIZE, commit, fd);
    EXPECT_NE(p, nullptr);
    EXPECT_TRUE(commit);
    write_test(p, S_HUGE_PAGE_SIZE);

    size_t rss_before = get_rss();
    EXPECT_TRUE(Jemalloc_pages::purge_forced(fd, 0, S_HUGE_PAGE_SIZE));
    size_t rss_after = get_rss();
    // Ensure that purging the memory reduced RSS by a significant amount of the memory previously paged in
    // This is approximate, and hopefully, does not lead to false positives
    EXPECT_GT((rss_after - rss_before), (S_HUGE_PAGE_SIZE * 3 / 4));

    // Check data is zeroed
    EXPECT_TRUE(check_zeroes(p, S_HUGE_PAGE_SIZE));

    Jemalloc_pages::unmap(p, S_PAGE_SIZE);

    // Restore flag
    Test_jemalloc_pages::set_os_overcommit_memory(overcommit);
  }

  EXPECT_EQ(close(fd), 0);
  EXPECT_TRUE(collection.remove_shm_object(shm_object_name));
}

} // namespace ipc::shm::arena_lend::jemalloc::test
