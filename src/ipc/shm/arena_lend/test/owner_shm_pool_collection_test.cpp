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
#include "ipc/shm/arena_lend/owner_shm_pool_collection.hpp"
#include "ipc/shm/arena_lend/memory_manager.hpp"
#include "ipc/shm/arena_lend/shm_pool.hpp"
#include "ipc/shm/arena_lend/jemalloc/jemalloc_pages.hpp"
#include "ipc/shm/arena_lend/test/test_event_listener.hpp"
#include "ipc/shm/arena_lend/test/test_owner_shm_pool_collection.hpp"
#include "ipc/shm/arena_lend/test/test_shm_object.hpp"
#include "ipc/shm/arena_lend/test/test_shm_pool_collection.hpp"
#include "ipc/test/test_logger.hpp"
#include "ipc/test/test_common_util.hpp"
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <iostream>
#include <unordered_set>
#include <random>

using std::cerr;
using std::make_shared;
using std::make_unique;
using std::shared_ptr;
using std::size_t;
using std::string;
using std::unique_ptr;
using std::vector;
using ipc::util::Permissions;

using namespace ipc::test;

namespace ipc::shm::arena_lend::test
{

namespace
{

/**
 * Retrieve the file type and mode for the file handle.
 *
 * @param fd The file handle.
 * @param mode Upon success, this will be filled in with the file type and mode bits. (See man page for stat.)
 *
 * @return Whether the operation was successful.
 */
bool get_mode(int fd, mode_t& mode)
{
  struct stat stats;
  int result = fstat(fd, &stats);
  if (result != 0)
  {
    return false;
  }

  mode = stats.st_mode;
  return true;
}

/**
 * Checks whether the file type and mode are set properly for underlying file handle of the shared memory pool.
 *
 * @param shm_pool The shared memory pool.
 * @param expected_permissions The file mode that should match that of the shared memory pool.
 *
 * @return Whether the file type and mode of the shared memory pool match expected.
 */
bool check_permissions(const shared_ptr<Shm_pool>& shm_pool, const Permissions& expected_permissions)
{
  const auto FILE_MODE_BITMASK = 0777;

  mode_t mode;
  if (!get_mode(shm_pool->get_fd(), mode))
  {
    ADD_FAILURE() << "Could not get mode for newly created pool [" << shm_pool << "]";
    return false;
  }

  if (!S_ISREG(mode))
  {
    ADD_FAILURE() << "Shm pool [" << shm_pool->get_name() << "] is not a regular file, mode [" << mode << "]";
    return false;
  }

  auto actual_native_permissions = mode & FILE_MODE_BITMASK;
  auto expected_native_permissions = expected_permissions.get_permissions();
  if (actual_native_permissions != expected_native_permissions)
  {
    ADD_FAILURE() << "Shm pool [" << shm_pool->get_name() << "] permissions [" << std::oct <<
      actual_native_permissions << "] do not match expected [" << expected_native_permissions << "]";
    return false;
  }

  return true;
}

/**
 * Implementation of Owner_shm_pool_collection class to test internal functionality.
 */
class Test_owner_shm_pool_collection_2 :
  public Test_owner_shm_pool_collection
{
public:
  using Owner_shm_pool_collection::Memory_decommit_functor;
  using Owner_shm_pool_collection::Memory_unmap_functor;
  using Mutex = Owner_shm_pool_collection::Lockable_shm_pool::Mutex;
  using Lock = Owner_shm_pool_collection::Lockable_shm_pool::Lock;

  // Make methods public
  using Owner_shm_pool_collection::remove_range_and_pool_if_empty;
  using Test_owner_shm_pool_collection::get_memory_unmap_functor;
  using Shm_pool_collection::register_shm_pool;

  /**
   * Constructor.
   *
   * @param logger Used for logging purposes.
   * @param memory_manager The memory allocator.
   * @param id Identifier for the collection.
   */
  Test_owner_shm_pool_collection_2(flow::log::Logger* logger,
                                   std::shared_ptr<Memory_manager> memory_manager,
                                   Collection_id id = Test_shm_pool_collection::S_DEFAULT_COLLECTION_ID) :
    Test_owner_shm_pool_collection(logger, id, memory_manager)
  {
  }

  /**
   * Allocates memory from the memory manager.
   *
   * @param size The amount of memory to allocate.
   *
   * @return The resulting allocation upon success, or nullptr, upon failure.
   */
  virtual void* allocate(size_t size) override
  {
    m_last_address_allocated = get_memory_manager()->allocate(size);
    return m_last_address_allocated;
  }

  /**
   * Deallocates memory from the memory manager.
   *
   * @param address The address to deallocate.
   */
  virtual void deallocate(void* address) override
  {
    Owner_shm_pool_collection::deallocate(address);
    m_last_address_deallocated = address;
  }

  /**
   * Returns the last address allocated or nullptr, if there was none.
   *
   * @return See above.
   */
  void* get_last_address_allocated() const
  {
    return m_last_address_allocated;
  }

  /**
   * Returns the last address deallocated or nullptr, if there was none.
   *
   * @return See above.
   */
  void* get_last_address_deallocated() const
  {
    return m_last_address_deallocated;
  }

  /**
   * Creates a shared memory object, maps it in the process' address space, and pre-registers it before the
   * actual registration occurs, which causes an address conflict.
   *
   * @param size The desired size of the shared memory object.
   *
   * @return Upon success, a shared pointer to the created shared memory pool; otherwise, an empty shared pointer.
   */
  shared_ptr<Shm_pool> simulate_create_shm_pool_address_collision(size_t size)
  {
    string name = generate_shm_object_name(0 /* ignored by us anyway */);
    shared_ptr<Shm_pool> actual_shm_pool;
    const auto id = detail::Shm_pool_offset_ptr_data_base::generate_pool_id();
    shared_ptr<Shm_pool> result =
      Owner_shm_pool_collection::create_shm_pool(detail::Shm_pool_offset_ptr_data_base::generate_pool_id(),
                                                 name,
                                                 size,
                                                 nullptr,
                                                 [&](int fd, size_t size, void*) -> void*
                                                 {
                                                   void* address = map_shm(size, true, true, fd);
                                                   // Register the SHM pool to get the collision
                                                   actual_shm_pool = make_shared<Shm_pool>(id, name, address, size, fd);
                                                   EXPECT_TRUE(register_shm_pool(actual_shm_pool));
                                                   return address;
                                                 });
    EXPECT_TRUE(deregister_shm_pool(actual_shm_pool));
    return result;
  }

  /**
   * Creates a shared memory object, but returns a nullptr when mapping.
   *
   * @param name The name of the shared memory object to create.
   * @param size The desired size of the shared memory object.
   *
   * @return Upon success, a shared pointer to the created shared memory pool; otherwise, an empty shared pointer.
   */
  shared_ptr<Shm_pool> simulate_create_shm_pool_map_failure(const string& name, size_t size)
  {
    return Owner_shm_pool_collection::create_shm_pool(detail::Shm_pool_offset_ptr_data_base::generate_pool_id(),
                                                      name,
                                                      size,
                                                      nullptr,
                                                      [&](int, size_t, void*) -> void*
                                                      {
                                                        return nullptr;
                                                      });
  }

  /**
   * Ignores the unmapping of a shared memory pool and attempts to remove the underlying shared memory object.
   * This is specifically for a test to check the result of closing an invalid fd.
   *
   * @param shm_pool The shared memory pool to remove.
   * @param unmapped_memory_pool Whether the memory pool was unmapped, which may be true even though the result
   *                             is false.
   *
   * @return Whether the shared memory pool was removed; it's possible that partial execution was successfully
   *         performed like unmapping, but not shared memory removal, which would be false result.
   */
  bool remove_shm_pool_fake_unmap(const shared_ptr<Shm_pool>& shm_pool, bool& unmapped_memory_pool)
  {
    return Owner_shm_pool_collection::remove_shm_pool(
      shm_pool,
      []([[maybe_unused]] const std::shared_ptr<Shm_pool>& shm_pool) -> bool
      {
        return true;
      },
      unmapped_memory_pool);
  }


  /**
   * Performs an allocation backed by shared memory, uses the allocation to construct an object and returns a shared
   * pointer to this object. When the shared pointer has no more references, it will be destructed by this pool
   * collection instance. The start() method must be executed prior to any calls to this.
   *
   * @tparam T The object type to be created.
   * @tparam Args The parameter types that are passed to the constructor of T.
   * @param args The arguments passed to the constructor of T.
   *
   * @return A shared pointer to an object created in shared memory.
   */
  template <typename T, typename... Args>
  std::shared_ptr<T> construct(Args&&... args)
  {
    return construct_helper<T>(allocate(sizeof(T)), Object_deleter(this), std::forward<Args>(args)...);
  }

private:
  /**
   * Handles callback from shared pointer destruction. The result should be a destruction of the object and return
   * of the underlying memory back to the memory manager.
   */
  class Object_deleter 
  {
  public:
    /**
     * Constructor.
     *
     * @param pool_collection The object that allocated the underlying memory from which it should be returned.
     *
     * @todo Use shared pointer for pool_collection.
     * @todo Pass in arena id.
     */
    Object_deleter(Test_owner_shm_pool_collection_2* pool_collection) :
      m_pool_collection(pool_collection)
    {
      static size_t s_count = 0;
      ++s_count;
    }

    /**
     * Release callback executed by shared pointer destruction. The destructor for the object will be called
     * and the underlying memory will be returned to the memory manager.
     *
     * @tparam T The pointer type.
     * @param p The pointer held by the shared pointer.
     */
    template <typename T>
    void operator()(T* p)
    {
      // Call destructor
      p->~T();
      m_pool_collection->deallocate(p);
    }

  private:
    /// The object that allocated the underlying memory from which it should be returned.
    Test_owner_shm_pool_collection_2* m_pool_collection;
  }; // class Object_deleter

  /// The last address allocated or nullptr, if there was none.
  void* m_last_address_allocated = nullptr;
  /// The last address deallocated or nullptr, if there was none.
  void* m_last_address_deallocated = nullptr;
}; // class Test_owner_shm_pool_collection_2

/// Google test fixture
class Owner_shm_pool_collection_test :
  public ::testing::Test
{
public:
  /// Constructor
  Owner_shm_pool_collection_test() :
    m_test_logger(flow::log::Sev::S_TRACE),
    m_memory_manager(make_shared<Memory_manager>(&m_test_logger)),
    m_collection(&m_test_logger, m_memory_manager)
  {
  }

  /**
   * Returns the logger.
   *
   * @return See above.
   */
  inline flow::log::Logger* get_logger()
  {
    return &m_test_logger;
  }
  /**
   * Returns the memory manager.
   *
   * @return See above.
   */
  inline const shared_ptr<Memory_manager>& get_memory_manager() const
  {
    return m_memory_manager;
  }
  /**
   * Returns the owner collection.
   *
   * @return See above.
   */
  inline Test_owner_shm_pool_collection_2& get_collection()
  {
    return m_collection;
  }

  /**
   * Returns the memory pool size to use for tests.
   *
   * @return See above.
   */
  static inline size_t get_pool_size()
  {
    return (jemalloc::Jemalloc_pages::get_page_size() * 4);
  }

private:
  /// For logging purposes.
  Test_logger m_test_logger;
  /// The memory manager.
  const shared_ptr<Memory_manager> m_memory_manager;
  /// The owner collection.
  Test_owner_shm_pool_collection_2 m_collection;
}; // class Owner_shm_pool_collection_test

} // Anonymous namespace

/// Death tests - suffixed with DeathTest per Googletest conventions, which allows aliasing.
using Owner_shm_pool_collection_DeathTest = Owner_shm_pool_collection_test;
TEST_F(Owner_shm_pool_collection_DeathTest, Interface_death)
{
  auto& collection = get_collection();

  // Bad size
  EXPECT_DEATH(collection.create_shm_pool(0), "size > 0");
  // Bad name
  EXPECT_DEATH(collection.create_shm_pool("", get_pool_size()), "!name.empty()");

  // Duplicate address
  EXPECT_DEATH(collection.simulate_create_shm_pool_address_collision(get_pool_size()),
               "Duplicate SHM pool address");
}

/// Tests involving the class interface, both public and protected.
TEST_F(Owner_shm_pool_collection_test, Interface)
{
  auto& collection = get_collection();

  EXPECT_EQ(collection.get_id(), Test_shm_pool_collection::S_DEFAULT_COLLECTION_ID);
  
  // Basic allocation (memory manager is really doing the work)
  {
    void* p = collection.allocate(100);
    EXPECT_NE(p, nullptr);
    collection.deallocate(p);
  }

  // Shared pointer allocation
  {
    shared_ptr<size_t> constructor_counter = make_shared<size_t>(0);
    shared_ptr<size_t> destructor_counter = make_shared<size_t>(0);

    class Foo
    {
    public:
      Foo(shared_ptr<size_t>& constructor_counter, shared_ptr<size_t>& destructor_counter) :
        m_destructor_counter(destructor_counter)
      {
        ++(*constructor_counter);
      }

      ~Foo()
      {
        ++(*m_destructor_counter);
      }

    private:
      shared_ptr<size_t> m_destructor_counter;
    }; // class Foo

    void* last_address_deallocated = collection.get_last_address_deallocated();
    shared_ptr<Foo> foo_ptr = collection.construct<Foo>(constructor_counter, destructor_counter);
    Foo* foo = foo_ptr.get();
    // Ensure constructor is called exactly once
    EXPECT_EQ(collection.get_last_address_allocated(), foo);
    EXPECT_EQ(*constructor_counter, 1UL);
    // Ensure destructor was not called
    EXPECT_EQ(collection.get_last_address_deallocated(), last_address_deallocated);
    EXPECT_EQ(*destructor_counter, 0UL);

    // Trigger shared pointer and thus, object destruction
    foo_ptr = nullptr;
    // Ensure constructor was not called
    EXPECT_EQ(collection.get_last_address_allocated(), foo);
    EXPECT_EQ(*constructor_counter, 1UL);
    EXPECT_EQ(collection.get_last_address_deallocated(), foo);
    // Ensure destructor is only called once
    EXPECT_EQ(*destructor_counter, 1UL);
  }

  // Shm pools
  {
    shared_ptr<Shm_pool> pool = collection.create_shm_pool(get_pool_size());
    EXPECT_NE(pool, nullptr);
    // Conflicting name
    EXPECT_EQ(collection.create_shm_pool(pool->get_name(), get_pool_size()), nullptr);

    // Check permissions
    {
      // Group level permissions
      EXPECT_TRUE(check_permissions(pool, util::shared_resource_permissions(util::Permissions_level::S_GROUP_ACCESS)));

      // User level permissions
      Test_owner_shm_pool_collection collection_2(get_logger(),
                                                  Test_shm_pool_collection::S_DEFAULT_COLLECTION_ID,
                                                  get_memory_manager(),
                                                  create_shm_object_name_generator(),
                                                  util::Permissions_level::S_USER_ACCESS);
      auto pool_2 = collection_2.create_shm_pool(get_pool_size());
      EXPECT_NE(pool_2, nullptr);
      EXPECT_TRUE(
        check_permissions(pool_2, util::shared_resource_permissions(util::Permissions_level::S_USER_ACCESS)));

      // Clean up
      EXPECT_TRUE(collection_2.remove_shm_pool(pool_2));
    }

    // Map failure
    {
      const string name = collection.generate_shm_object_name(0 /* ignored by us anyway */);
      shared_ptr<Shm_pool> pool = collection.simulate_create_shm_pool_map_failure(name, get_pool_size());
      EXPECT_EQ(pool, nullptr);
      // Make sure we didn't have residual object
      EXPECT_NE(shm_unlink(name.c_str()), 0);
    }

    // Successful unmap
    bool unmapped_memory_pool = false;
    EXPECT_TRUE(collection.remove_shm_pool(pool, unmapped_memory_pool));
    EXPECT_TRUE(unmapped_memory_pool);

    // Unregistered pool
    EXPECT_FALSE(collection.remove_shm_pool_fake_unmap(pool, unmapped_memory_pool));
    EXPECT_FALSE(unmapped_memory_pool);

    // Registered pool, but bad fd, non-existent shared memory object
    EXPECT_TRUE(collection.register_shm_pool(pool));
    bool result = false;
    EXPECT_TRUE(check_output([&]()
                             {
                               result = collection.remove_shm_pool_fake_unmap(pool, unmapped_memory_pool);
                             },
                             cerr,
                             strerror(EBADF)));
    EXPECT_TRUE(result);
    EXPECT_TRUE(unmapped_memory_pool);

    // Successful fake unmap
    EXPECT_TRUE(collection.register_shm_pool(pool));
    EXPECT_TRUE(collection.remove_shm_pool_fake_unmap(pool, unmapped_memory_pool));
    EXPECT_TRUE(unmapped_memory_pool);
  }

  // Notifications
  {
    Test_event_listener event_listener;

    // Add/remove tests
    EXPECT_FALSE(collection.remove_event_listener(&event_listener));
    EXPECT_TRUE(collection.add_event_listener(&event_listener));
    EXPECT_FALSE(collection.add_event_listener(&event_listener));

    // Create/delete tests
    // Normal creation should yield a notification
    shared_ptr<Shm_pool> pool = collection.create_shm_pool(get_pool_size());
    EXPECT_NE(pool, nullptr);
    shared_ptr<Test_event_listener::Create_notification> create_notification =
      event_listener.get_create_notification();
    EXPECT_NE(create_notification, nullptr);
    if (create_notification != nullptr)
    {
      EXPECT_EQ(create_notification->get_shm_pool(), pool);
    }
    EXPECT_EQ(event_listener.get_remove_notification(), nullptr);
    event_listener.reset_notifications();

    // Failed creation should not yield a notification
    EXPECT_EQ(collection.create_shm_pool(pool->get_name(), get_pool_size()), nullptr);
    EXPECT_EQ(event_listener.get_create_notification(), nullptr);
    EXPECT_EQ(event_listener.get_remove_notification(), nullptr);
    event_listener.reset_notifications();

    // Normal unmapping of a shared memory pool should yield a notification
    bool unmapped_memory_pool;
    EXPECT_TRUE(collection.remove_shm_pool(pool, unmapped_memory_pool));
    EXPECT_EQ(event_listener.get_create_notification(), nullptr);
    shared_ptr<Test_event_listener::Remove_notification> remove_notification =
      event_listener.get_remove_notification();
    if (remove_notification != nullptr)
    {
      EXPECT_EQ(remove_notification->get_shm_pool(), pool);
      EXPECT_TRUE(remove_notification->get_removed_shared_memory());
    }
    else
    {
      // Failed
      EXPECT_NE(remove_notification, nullptr);
    }
    event_listener.reset_notifications();

    // Unregistered memory pool should not yield a notification
    EXPECT_FALSE(collection.remove_shm_pool(pool, unmapped_memory_pool));
    EXPECT_FALSE(unmapped_memory_pool);
    EXPECT_EQ(event_listener.get_create_notification(), nullptr);
    EXPECT_EQ(event_listener.get_remove_notification(), nullptr);
    event_listener.reset_notifications();

    // Unregistered memory pool should not yield a notification despite unmapping that would succeed
    EXPECT_FALSE(collection.remove_shm_pool_fake_unmap(pool, unmapped_memory_pool));
    EXPECT_FALSE(unmapped_memory_pool);
    EXPECT_EQ(event_listener.get_create_notification(), nullptr);
    EXPECT_EQ(event_listener.get_remove_notification(), nullptr);
    event_listener.reset_notifications();

    // Registered memory pool, successful unmap, but non-existent shared memory should still yield a notification
    EXPECT_TRUE(collection.register_shm_pool(pool));
    EXPECT_TRUE(collection.remove_shm_pool_fake_unmap(pool, unmapped_memory_pool));
    EXPECT_TRUE(unmapped_memory_pool);
    EXPECT_EQ(event_listener.get_create_notification(), nullptr);
    remove_notification = event_listener.get_remove_notification();
    if (remove_notification != nullptr)
    {
      EXPECT_EQ(remove_notification->get_shm_pool(), pool);
      EXPECT_FALSE(remove_notification->get_removed_shared_memory());
    }
    else
    {
      // Failure
      EXPECT_NE(remove_notification, nullptr);
    }
    event_listener.reset_notifications();

    EXPECT_TRUE(collection.remove_event_listener(&event_listener));
    EXPECT_FALSE(collection.remove_event_listener(&event_listener));
  }
}

/// Exercises the remove_range_and_pool_if_empty() interface.
TEST_F(Owner_shm_pool_collection_test, Range_remove)
{
  auto& collection = get_collection();

  // Range removal
  {
    // The positive/negative offset from a shared memory pool the tests will use
    const int OFFSET = 1;
    // The size of the shared memory pools we will create
    const size_t POOL_SIZE = 4 * get_pool_size();

    /**
     * Creates a shared memory pool of established size.
     *
     * @return The shared memory pool.
     */
    auto create_shm_pool_functor =
      [&]() -> shared_ptr<Shm_pool>
      {
        shared_ptr<Shm_pool> pool = collection.create_shm_pool(POOL_SIZE);
        EXPECT_NE(pool, nullptr);
        return pool;
      };

    /**
     * Removes a shared memory pool by deregistering and unmapping it.
     *
     * @param The shared memory pool to remove.
     *
     * @return Whether the shared memory pool was deregistered and unmapped successfully.
     */
    auto remove_shm_pool_functor =
      [&](const shared_ptr<Shm_pool>& shm_pool) -> bool
      {
        bool unmapped_pool = false;
        bool result;
        EXPECT_TRUE(result = (collection.remove_shm_pool(shm_pool, unmapped_pool) && unmapped_pool));
        return result;
      };

    /**
     * Executes a test that attempts to remove a memory region.
     *
     * @param shm_pool The shared memory pool to remove from.
     * @param start_offset The offset from the shared memory pool the start of the region resides at.
     * @param size The size of the region to be removed.
     * @param expected_result The expected result of the attempt to remove the region.
     * @param force_decommit_fail If non-null, whether the functor to decommit the region should fail.
     * @param force_unmap_fail If non-null, whether the functor to unmap the region should fail.
     * @param unmapped_pool Whether the memory pool was unmapped.
     *
     * @return Whether the test succeeded.
     */
    auto remove_range_test_functor =
      [&](const shared_ptr<Shm_pool>& shm_pool,
          int start_offset,
          size_t size,
          bool expected_result,
          const bool* force_decommit_fail,
          const bool* force_unmap_fail,
          bool& unmapped_pool) -> bool
      {
        unmapped_pool = false;

        bool decommit_attempted = false;
        /**
         * Functor to decommit a memory region.
         *
         * @return Whether force_decommit_fail is non-null or it is not set to fail.
         */
        Test_owner_shm_pool_collection_2::Memory_decommit_functor decommit_functor =
        [&](const shared_ptr<Shm_pool>&, size_t, size_t) -> bool
        {
          decommit_attempted = true;
          return (force_decommit_fail != nullptr) && !(*force_decommit_fail);
        };

        bool unmap_attempted = false;
        /**
         * Functor to unmap a shared memory pool.
         *
         * @param The shared memory pool to unmap.
         *
         * @return Whether force_unmap_fail is non-null or it is not set to fail.
         */
        Test_owner_shm_pool_collection_2::Memory_unmap_functor unmap_functor =
        [&](const shared_ptr<Shm_pool>& shm_pool) -> bool
        {
          unmap_attempted = true;
          if ((force_unmap_fail != nullptr) && *force_unmap_fail)
          {
            return false;
          }

          return collection.get_memory_unmap_functor()(shm_pool);
        };

        bool removed_range;

        bool result = true;
        if (expected_result !=
            collection.remove_range_and_pool_if_empty((static_cast<char*>(shm_pool->get_address()) + start_offset),
                                                      size,
                                                      &decommit_functor,
                                                      removed_range,
                                                      unmap_functor,
                                                      unmapped_pool))
        {
          ADD_FAILURE() << "Result of range removal [" << !expected_result << "] did not match expected";
          result = false;
        }

        if ((force_decommit_fail != nullptr) != decommit_attempted)
        {
          ADD_FAILURE() << "Decommit attempted [" << decommit_attempted << "] did not match expected";
          result = false;
        }

        if ((force_unmap_fail != nullptr) != unmap_attempted)
        {
          ADD_FAILURE() << "Unmap attempted [" << unmap_attempted << "] did not match expected";
          result = false;
        }

        return result;
      };

    /**
     * Executes a test that creates a shared memory and attempts to remove a memory region from it.
     *
     * @param start_offset The offset from the shared memory pool the start of the region resides at.
     * @param size The size of the region to be removed.
     * @param expected_result The expected result of the attempt to remove the region.
     * @param expected_unmap_result Whether unmapping should have been attempted and should have succeeded.
     * @param force_decommit_fail If non-null, whether the functor to decommit the region should fail.
     * @param force_unmap_fail If non-null, whether the functor to unmap the region should fail.
     *
     * @return Whether the tests succeeded.
     */
    auto remove_range_test_functor_2 =
      [&](int start_offset,
          size_t size,
          bool expected_result,
          bool expected_unmap_result,
          bool* force_decommit_fail,
          bool* force_unmap_fail) -> bool
      {
        shared_ptr<Shm_pool> pool = create_shm_pool_functor();
        if (pool == nullptr)
        {
          return false;
        }
        bool unmapped_pool = false;
        bool result = remove_range_test_functor(pool,
                                                start_offset,
                                                size,
                                                expected_result,
                                                force_decommit_fail,
                                                force_unmap_fail,
                                                unmapped_pool);
        EXPECT_EQ(expected_unmap_result, unmapped_pool);
        // Clean up the shared memory pool if we didn't expect the pool to be deregistered already
        if (!expected_unmap_result && ((force_unmap_fail == nullptr) || !*force_unmap_fail))
        {
          result = remove_shm_pool_functor(pool);
        }

        return result;
      };

    bool force_decommit_fail = false;
    bool force_unmap_fail = false;

    // Start address before pool
    {
      // Start address before pool and end at same location (zero size)
      EXPECT_TRUE(remove_range_test_functor_2((-OFFSET - 1), 0, false, false, nullptr, nullptr));
      // Start address before pool and end before pool
      EXPECT_TRUE(remove_range_test_functor_2((-OFFSET - 1), 1, false, false,  nullptr, nullptr));
      // Start address before pool and end at start of pool
      EXPECT_TRUE(remove_range_test_functor_2(-OFFSET, OFFSET, false, false, nullptr, nullptr));
      // Start address before pool and end in pool
      EXPECT_TRUE(remove_range_test_functor_2(-OFFSET, (OFFSET + 1), false, false, nullptr, nullptr));
      // Start address before pool and end at end of pool
      EXPECT_TRUE(remove_range_test_functor_2(-OFFSET, (OFFSET + POOL_SIZE), false, false, nullptr, nullptr));
      // Start address before pool and end past end of pool
      EXPECT_TRUE(remove_range_test_functor_2(-OFFSET, (OFFSET + POOL_SIZE + 1), false, false, nullptr, nullptr));
    }

    // Start address at start of pool
    {
      // Start address at start of pool and end at same location (zero size)
      EXPECT_TRUE(remove_range_test_functor_2(0, 0, false, false, nullptr, nullptr));
      // Start address at start of pool and end in pool
      EXPECT_TRUE(remove_range_test_functor_2(0, OFFSET, true, false, &force_decommit_fail, nullptr));
      // Start address at start of pool and end at end of pool
      EXPECT_TRUE(remove_range_test_functor_2(0, POOL_SIZE, true, true, &force_decommit_fail, &force_unmap_fail));
      // Start address at start of pool and end at end of pool with failed unmap
      EXPECT_TRUE(remove_range_test_functor_2(0, POOL_SIZE, true, true, &force_decommit_fail, &force_unmap_fail));
      // Start address at start of pool and end after end of pool
      EXPECT_TRUE(remove_range_test_functor_2(0, (POOL_SIZE + 1), false, false, nullptr, nullptr));
    }

    // Start address in pool
    {
      // Start address in pool and end at same location (zero size)
      EXPECT_TRUE(remove_range_test_functor_2(OFFSET, 0, false, false, nullptr, nullptr));
      // Start address in pool and end in pool
      EXPECT_TRUE(remove_range_test_functor_2(OFFSET,
                                              (POOL_SIZE - OFFSET - 1),
                                              true,
                                              false,
                                              &force_decommit_fail,
                                              nullptr));
      // Start address in pool and end at end of pool
      EXPECT_TRUE(remove_range_test_functor_2(OFFSET,
                                              (POOL_SIZE - OFFSET),
                                              true,
                                              false,
                                              &force_decommit_fail,
                                              nullptr));
      // Start address in pool and end after end of pool
      EXPECT_TRUE(remove_range_test_functor_2(OFFSET,
                                              (POOL_SIZE - OFFSET + 1),
                                              false,
                                              false,
                                              nullptr,
                                              nullptr));
    }

    // Start address at end of pool
    {
      // Start address at end of pool and end at same location (zero size)
      EXPECT_TRUE(remove_range_test_functor_2(POOL_SIZE, 0, false, false, nullptr, nullptr));
      // Start address at end of pool and end after end of pool
      EXPECT_TRUE(remove_range_test_functor_2(POOL_SIZE, OFFSET, false, false, nullptr, nullptr));
    }

    // Start address after end of pool
    {
      // Start address after end of pool and end at same location (zero size)
      EXPECT_TRUE(remove_range_test_functor_2((POOL_SIZE + 1), 0, false, false, nullptr, nullptr));
      // Start address after end of pool and end after end of pool
      EXPECT_TRUE(remove_range_test_functor_2((POOL_SIZE + 1), OFFSET, false, false, nullptr, nullptr));
    }

    // Multi-block removal
    {
      force_unmap_fail = false;
      shared_ptr<Shm_pool> shm_pool = create_shm_pool_functor();
      EXPECT_NE(shm_pool, nullptr);
      bool unmapped_pool = false;

      // Start address at start of pool and end in pool
      EXPECT_TRUE(remove_range_test_functor(shm_pool, 0, OFFSET, true, &force_decommit_fail, nullptr, unmapped_pool));
      EXPECT_FALSE(unmapped_pool);

      // Start address at start of last removal and end in pool
      EXPECT_LT(static_cast<size_t>(OFFSET), (POOL_SIZE - 1));
      EXPECT_TRUE(remove_range_test_functor(shm_pool, OFFSET, 1, true, &force_decommit_fail, nullptr, unmapped_pool));
      EXPECT_FALSE(unmapped_pool);

      // Start address at start of last removal and end at end of pool
      EXPECT_TRUE(remove_range_test_functor(shm_pool,
                                            OFFSET,
                                            (POOL_SIZE - OFFSET - 1),
                                            true,
                                            &force_decommit_fail,
                                            &force_unmap_fail,
                                            unmapped_pool));
      if (!unmapped_pool)
      {
        ADD_FAILURE() << "Expected pool to be unmapped, but it was not";
        EXPECT_TRUE(remove_shm_pool_functor(shm_pool));
      }

      // Attempt to remove again
      EXPECT_TRUE(remove_range_test_functor(shm_pool, 0, OFFSET, false, nullptr, nullptr, unmapped_pool));
      EXPECT_FALSE(unmapped_pool);
    }

    // Failed functors
    {
      force_decommit_fail = true;
      force_unmap_fail = true;
      // Start address at start of pool and end in pool
      EXPECT_TRUE(remove_range_test_functor_2(0, OFFSET, false, false, &force_decommit_fail, nullptr));
      // Start address at start of pool and end at the end of pool
      force_decommit_fail = false;
      EXPECT_TRUE(remove_range_test_functor_2(0, POOL_SIZE, true, false, &force_decommit_fail, &force_unmap_fail));
      force_unmap_fail = false;
    }
  }
}

/**
 * Tests involving multithreaded operations on creation and removal. There are two sets of tests:
 * 1. Concurrent allocation/deallocation of shared memory pools.
 * 2. Concurrent deallocation of regions within shared memory pools.
 */
TEST_F(Owner_shm_pool_collection_test, Multithread_remove)
{
  /**
   * Tracks allocated shared memory pools for removal.
   */
  class Shm_pool_tracker
  {
  public:
    /**
     * Constructor.
     *
     * @param shm_pool The shared memory pool to track.
     */
    Shm_pool_tracker(shared_ptr<Shm_pool>&& shm_pool) :
      m_shm_pool(std::move(shm_pool)),
      m_remaining_size(m_shm_pool->get_size())
    {
    }

    /**
     * Returns the shared memory pool being tracked.
     *
     * @return See above.
     */
    shared_ptr<Shm_pool> get_shm_pool() const
    {
      return m_shm_pool;
    }

    /**
     * Performs accounting for a range removal from the shared memory pool.
     *
     * @param size The size to remove.
     * @param size_removed The actual size removed. This may be different than size, if the remaining size was smaller.
     *
     * @return Whether the remaining size after removal is zero.
     */
    bool remove_size(size_t size, size_t& size_removed)
    {
      if (size >= m_remaining_size)
      {
        size_removed = m_remaining_size;
        m_remaining_size = 0;
        return true;
      }
      else
      {
        size_removed = size;
        m_remaining_size -= size;
        return false;
      }
    }

  private:
    /// The shared memory pool.
    shared_ptr<Shm_pool> m_shm_pool;
    /// The remaining size of the shared memory pool as accounted for in the test (not in the shared memory pool
    /// itself).
    size_t m_remaining_size;
  }; // struct Shm_pool_tracker

  // The quantity of initial memory pools to allocate
  const size_t INITIAL_POOLS = 500;
  // The quantity of creation/removal operations to perform per thread
  const size_t NUM_OPS = 1000;
  // The number of threads to perform operations
  const size_t NUM_THREADS = 10;
  // Mutex to protect the shared memory pool list
  Test_owner_shm_pool_collection_2::Mutex shm_pools_mutex;
  // The shared memory pool list to add pools to and remove pools from
  std::deque<Shm_pool_tracker> shm_pools;
  // Mutex to protect the removal pending pool list
  Test_owner_shm_pool_collection_2::Mutex removal_pending_shm_pools_mutex;
  // The list of shared memory pools that are designated to be removed
  std::set<shared_ptr<Shm_pool>> removal_pending_shm_pools;

  // Decrease severity to get more output as necessary for debugging
  Test_logger logger(flow::log::Sev::S_INFO);
  Test_owner_shm_pool_collection_2 collection(&logger, make_shared<Memory_manager>(&logger));

  /**
   * Creates a shared memory pool and puts it into the pool list.
   */
  const auto create_shm_pool_functor =
    [&]()
    {
      shared_ptr<Shm_pool> shm_pool = collection.create_shm_pool();
      EXPECT_NE(shm_pool, nullptr);

      {
        Test_owner_shm_pool_collection_2::Lock lock(shm_pools_mutex);
        shm_pools.emplace_back(Shm_pool_tracker(std::move(shm_pool)));
      }
    };

  /**
   * Pops the first or last element from the pool list and removes the shared memory pool or a range from the
   * pool. If this results in a removal of the pool, the pool is tracked to make sure it is removed.
   *
   * @param random_generator If non-null, a random generator to determine whether to remove the first or last
   *                         element from the pool.
   * @param size If non-zero, the size of the range to remove; otherwise, the entire pool will be removed.
   */
  const auto remove_shm_pool_functor =
    [&](std::default_random_engine* random_generator, size_t size)
    {
      shared_ptr<Shm_pool> shm_pool;
      size_t size_to_remove;
      {
        Test_owner_shm_pool_collection_2::Lock lock(shm_pools_mutex);
        if (shm_pools.empty())
        {
          // Nothing to remove
          return;
        }

        size_t coin_result;
        if (random_generator != nullptr)
        {
          // Get the first or last element depending on the coin
          std::uniform_int_distribution<size_t> coin(0, 1);
          coin_result = coin(*random_generator);
        }
        else
        {
          // Use the first element
          coin_result = 0;
        }
        Shm_pool_tracker& tracker = ((coin_result != 0) ? shm_pools.front() : shm_pools.back());
        shm_pool = tracker.get_shm_pool();

        bool remove;
        if (size == 0)
        {
          size_to_remove = size;
          remove = true;
        }
        else
        {
          remove = tracker.remove_size(size, size_to_remove);
        }

        if (remove)
        {
          // Track that this pool should be removed (by some thread).
          {
            Test_owner_shm_pool_collection_2::Lock lock(removal_pending_shm_pools_mutex);
            EXPECT_TRUE(removal_pending_shm_pools.emplace(shm_pool).second);
          }
          if (coin_result != 0)
          {
            shm_pools.pop_front();
          }
          else
          {
            shm_pools.pop_back();
          }
        }
      }

      bool unmapped_pool;
      if (size == 0)
      {
        EXPECT_TRUE(collection.remove_shm_pool(shm_pool, unmapped_pool));
        EXPECT_TRUE(unmapped_pool);
      }
      else
      {
        // Operations here may occur concurrently, so there's no guarantee which range removal will
        // ultimately remove the pool
        bool removed_range;
        EXPECT_TRUE(collection.remove_range_and_pool_if_empty(shm_pool->get_address(),
                                                              size_to_remove,
                                                              nullptr,
                                                              removed_range,
                                                              collection.get_memory_unmap_functor(),
                                                              unmapped_pool));
        EXPECT_TRUE(removed_range);
      }

      if (unmapped_pool)
      {
        // We removed the pool, so check it off
        Test_owner_shm_pool_collection_2::Lock lock(removal_pending_shm_pools_mutex);
        EXPECT_EQ(removal_pending_shm_pools.erase(shm_pool), 1UL);
      }
    };

  for (size_t test_index = 0; test_index < 2; ++test_index)
  {
    // Populate list with initial pools
    for (size_t i = 0; i < INITIAL_POOLS; ++i)
    {
      create_shm_pool_functor();
    }

    vector<unique_ptr<std::thread>> threads;
    for (size_t i = 0; i < NUM_THREADS; ++i)
    {
      threads.emplace_back(
        make_unique<std::thread>(
          [&]()
          {
            std::default_random_engine random_generator;
            std::uniform_int_distribution<size_t> coin(0, 1);
            std::uniform_int_distribution<size_t> eight_sided_die(1, 8);

            for (size_t i = 0; i < NUM_OPS; ++i)
            {
              if (test_index == 0)
              {
                // Randomly decide whether to allocate or deallocate a shared memory pool
                if (coin(random_generator) != 0)
                {
                  // Allocate
                  create_shm_pool_functor();
                }
                else
                {
                  // Deallocate
                  remove_shm_pool_functor(&random_generator, 0);
                }
              }
              else
              {
                // Randomly decide the size to remove from the pool
                size_t size = get_pool_size() / eight_sided_die(random_generator);
                // Deallocate
                remove_shm_pool_functor(&random_generator, size);
              }
            }
          }));
    }

    // Wait for threads to complete
    for (const auto& cur_thread : threads)
    {
      cur_thread->join();
    }

    // Ensure any pending removals are completed
    EXPECT_TRUE(removal_pending_shm_pools.empty());

    // Clean up remaining pools
    while (!shm_pools.empty())
    {
      remove_shm_pool_functor(nullptr, get_pool_size());
    }

    // Ensure any pending removals are completed
    EXPECT_TRUE(removal_pending_shm_pools.empty());
    // Reset state
    removal_pending_shm_pools.clear();
  }
}

} // namespace ipc::shm::arena_lend::test
