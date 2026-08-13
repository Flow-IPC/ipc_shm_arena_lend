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
#include "ipc/shm/arena_lend/detail/owner_spc_impl.hpp"
#include "ipc/shm/arena_lend/jemalloc/ipc_arena.hpp"
#include "ipc/shm/arena_lend/jemalloc/test/test_jemalloc_pages.hpp"
#include "ipc/shm/arena_lend/shm_pool.hpp"
#include "ipc/shm/arena_lend/test/test_borrower.hpp"
#include "ipc/shm/arena_lend/test/test_event_listener.hpp"
#include "ipc/shm/arena_lend/test/test_shm_object.hpp"
#include "ipc/shm/arena_lend/test/test_shm_pool_collection.hpp"
#include <flow/test/test_common_util.hpp>
#include <flow/async/single_thread_task_loop.hpp>
#include "ipc/test/test_logger.hpp"
#include <iostream>
#include <sys/mman.h>
#include <deque>
#include <random>

using ipc::test::Test_logger;
using flow::util::ostream_op_string;
using std::array;
using std::cout;
using std::deque;
using std::make_shared;
using std::make_unique;
using std::set;
using std::shared_ptr;
using std::size_t;
using std::static_pointer_cast;
using std::string;
using std::unique_ptr;
using std::vector;

using namespace ipc::shm::arena_lend::test;
using namespace flow::test;

namespace ipc::shm::arena_lend::jemalloc::test
{

namespace
{
/**
 * Overrides Ipc_arena to gain access to protected members for testing.
 */
class Test_ipc_arena :
  public Ipc_arena
{
public:
  // Make public
  using Ipc_arena::allocate;
  using Ipc_arena::deallocate;
  using Ipc_arena::get_jemalloc_memory_manager;
  using Ipc_arena::start;

  /**
   * Creates an instance of this class along with its arenas. We require this, because the
   * construct() interface requires the use of shared pointers.
   *
   * @param logger Used for logging purposes.
   * @param memory_manager The memory allocator.
   *
   * @return Upon success, a shared pointer to an instance of this class; otherwise, an empty shared pointer.
   */
  static shared_ptr<Test_ipc_arena> create(
    flow::log::Logger* logger,
    const shared_ptr<Memory_manager>& memory_manager)
  {
    return shared_ptr<Test_ipc_arena>
             (new Test_ipc_arena(logger, memory_manager),
              [](auto* coll)
    {
      coll->destroy();
    });
  }

  /// Destructor.
  virtual ~Test_ipc_arena() override
  {
    FLOW_LOG_TRACE("~Test_ipc_arena()");
  }

  /**
   * Creates a shared memory pool.
   *
   * @param size The size of the memory pool to be created.
   * @param zero Output parameter indicating whether the contents have been zeroed.
   * @param commit Whether the system should designate the pages to be readable and writable (marked active and
   *               can be put into physical memory). If they system is set to overcommit memory, commit is always
   *               enabled. The value will be updated as an output parameter to indicate whether the memory was
   *               committed.
   *
   * @return Upon success, the created memory pool's starting address; otherwise, nullptr.
   */
  void* create_shm_pool_external(size_t size, bool* zero, bool* commit)
  {
    return create_shm_pool(nullptr,
                           size,
                           Jemalloc_pages::get_page_size(),
                           zero,
                           commit,
                           get_jemalloc_arena_id());
  }

  /**
   * Creates a shared memory pool, but with no real memory backing.
   *
   * @param address The address of the memory pool to be created.
   * @param size The size of the memory pool to be created.
   *
   * @return Upon success, the created memory pool; otherwise, nullptr.
   */
  shared_ptr<Shm_pool> create_fake_shm_pool(void* address, size_t size)
  {
    const auto id = arena_lend::detail::Shm_pool_offset_ptr_data_base::generate_pool_id();
    const string name = generate_shm_object_name(id).str();

    return Owner_shm_pool_collection::create_shm_pool(
      id,
      name,
      size,
      address,
      [&](int, size_t, void*) -> void*
      {
        FLOW_LOG_TRACE("Mapped SHM pool at address " << address);
        return address;
      });
  }
  /**
   * Removes a memory pool.
   *
   * @param address The memory pool to remove.
   * @param size The size of the memory pool to be removed.
   * @param committed Whether the range was previously committed.
   *
   * @return Whether the memory pool was removed successfully.
   */
  bool remove_shm_pool_external(void* address, size_t size, bool committed = false)
  {
    return remove_shm_pool(address, size, committed, get_jemalloc_arena_id());
  }
  /**
   * Removes a shared memory pool created by create_fake_shm_pool that has no real memory backing.
   *
   * @param address The address of the memory pool to be created.
   * @param size The size of the memory pool to be created.
   *
   * @return Whether the memory pool was removed successfully.
   */
  bool remove_fake_shm_pool(const shared_ptr<Shm_pool>& shm_pool)
  {
    bool removed_range;
    bool unmapped_pool;
    if (!remove_range_and_pool_if_empty(shm_pool->get_address(),
                                        shm_pool->get_size(),
                                        nullptr,
                                        removed_range,
                                        []([[maybe_unused]] const shared_ptr<Shm_pool>& shm_pool) -> bool
                                        {
                                          return true;
                                        },
                                        unmapped_pool) ||
        !removed_range ||
        !unmapped_pool)
    {
      FLOW_LOG_WARNING("Failure when performing range removal of shm_pool [" << shm_pool << "], removed range [" <<
                       removed_range << "], unmapped_pool [" << unmapped_pool << "]");
      return false;
    }

    return true;
  }

  /**
   * Mark memory pages as readable and writable.
   *
   * @param address The memory pool where the pages reside in.
   * @param size The size of the memory pool.
   *
   * @return Whether the pages were successfully committed.
   */
  bool commit_memory_pages_external(void* address, size_t size)
  {
    return commit_memory_pages(address, size, 0, size, get_jemalloc_arena_id());
  }
  /**
   * Mark memory pages as inaccessible (non-writable and non-readable).
   *
   * @param address The memory pool where the pages reside in.
   * @param size The size of the memory pool.
   *
   * @return Whether the pages were successfully decommitted.
   */
  bool decommit_memory_pages_external(void* address, size_t size)
  {
    return decommit_memory_pages(address, size, 0, size, get_jemalloc_arena_id());
  }
  /**
   * Mark memory pages as inaccessible (non-writable and non-readable).
   *
   * @param address The memory pool where the pages reside in.
   * @param size The size of the memory pool.
   *
   * @return Whether the pages were successfully decommitted.
   */
  bool purge_forced_memory_pages_external(void* address, std::size_t size)
  {
    return purge_forced_memory_pages(address, size, 0, size, get_jemalloc_arena_id());
  }
  /**
   * Returns whether memory pages can be merged.
   *
   * @param address_a The first memory pool location.
   * @param size_a The size of the first memory pool.
   * @param address_b The second memory pool location.
   * @param size_b The size of the second memory pool.
   *
   * @return Whether the pages are allowed to merge.
   */
  bool merge_memory_pages_external(const void* address_a, size_t size_a, const void* address_b, size_t size_b)
  {
    return merge_memory_pages(address_a, size_a, address_b, size_b, false, get_jemalloc_arena_id());
  }
  /**
   * Returns whether memory pages can be merged.
   *
   * @param pool_a The first memory pool location.
   * @param offset_a The offset from the first memory pool base address.
   * @param size_a The size of the first memory pool.
   * @param pool_b The second memory pool location.
   * @param offset_b The offset from the second memory pool base address.
   * @param size_b The size of the second memory pool.
   *
   * @return Whether the pages are allowed to merge.
   */
  bool merge_memory_pages_external(const void* pool_a,
                                   int offset_a,
                                   size_t size_a,
                                   const void* pool_b,
                                   int offset_b,
                                   size_t size_b)
  {
    const char* address_a = static_cast<const char*>(pool_a) + offset_a;
    const char* address_b = static_cast<const char*>(pool_b) + offset_b;

    return merge_memory_pages_external(address_a, size_a, address_b, size_b);
  }

  void set_test_event_listener(Test_event_listener* listener) { m_test_event_listener = listener; }

protected:
  void on_shm_pool_created(const std::shared_ptr<Shm_pool>& shm_pool) override
  {
    if (m_test_event_listener) { m_test_event_listener->notify_created_shm_pool(shm_pool); }
  }
  void on_shm_pool_removed(const std::shared_ptr<Shm_pool>& shm_pool, bool removed_shared_memory) override
  {
    if (m_test_event_listener) { m_test_event_listener->notify_removed_shm_pool(shm_pool, removed_shared_memory); }
  }

private:
  Test_event_listener* m_test_event_listener = nullptr;

  /**
   * Constructor.
   *
   * @see Ipc_arena::Ipc_arena()
   */
  Test_ipc_arena(flow::log::Logger* logger,
                           const shared_ptr<Memory_manager>& memory_manager) :
    Ipc_arena(logger,
              memory_manager,
              create_test_pool_name_base(),
              util::shared_resource_permissions(util::Permissions_level::S_GROUP_ACCESS))
  {
  }
}; // class Test_ipc_arena

/// Google test fixture.
class Ipc_arena_test :
  public ::testing::Test
{
public:
  /// Default allocation size.
  static constexpr size_t S_ALLOCATION_SIZE = 100;

  /**
   * Returns the size of memory pools used in the tests.
   *
   * @return See above.
   */
  static inline size_t get_pool_size()
  {
    return Jemalloc_pages::get_page_size() * 4;
  }

  /**
   * Forms a list of strings related to arena destruction to check the output of when a collection is destroyed.
   *
   * @param collection The collection that will be destroyed.
   *
   * @return See above.
   */
  static vector<string> form_arena_destruction_output_checks(
    const shared_ptr<Test_ipc_arena>& collection)
  {
    return { ostream_op_string("Destroyed arena \\[", collection->get_jemalloc_arena_id(), "\\]") };
  }

  /// Constructor
  Ipc_arena_test() :
    // We need data level here for certain tests
    m_test_logger(flow::log::Sev::S_DATA),
    m_memory_manager(make_shared<Memory_manager>())
  {
  }

  /**
   * Returns the logger used for logging purposes.
   *
   * @return See above.
   */
  inline Test_logger& get_test_logger()
  {
    return m_test_logger;
  }

  /**
   * Returns the memory allocator.
   *
   * @return See above.
   */
  inline shared_ptr<Memory_manager> get_memory_manager() const
  {
    return m_memory_manager;
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

  /**
   * Creates a Shm_pool_collection object using default test parameters.
   *
   * @return A created Shm_pool_collection object.
   */
  shared_ptr<Test_ipc_arena> create_collection()
  {
    return Test_ipc_arena::create(&m_test_logger, m_memory_manager);
  }

private:
  /// Used for logging purposes.
  Test_logger m_test_logger;
  /// Memory allocator.
  shared_ptr<Memory_manager> m_memory_manager;
  /// Callbacks for collection events.
  Test_event_listener m_event_listener;
}; // class Ipc_arena_test

} // Anonymous namespace

/// Death tests - suffixed with DeathTest per Googletest conventions, aliased to fixture.
using Ipc_arena_DeathTest = Ipc_arena_test;
TEST_F(Ipc_arena_DeathTest, Interface)
{
#ifdef NDEBUG
  GTEST_SKIP() << "Death tests rely on assert()s which are disabled in this (NDEBUG) build.";
#endif
  auto collection = create_collection();

  // Public interface
  {
    // Not started yet
    EXPECT_DEATH(collection->allocate(100), "m_arenas");
    EXPECT_DEATH(collection->construct<int>(100), "m_arenas");
    EXPECT_DEATH(collection->deallocate(reinterpret_cast<void*>(0x1)), "m_arenas");
  }
  collection->start();

  bool zero = true;
  bool commit = true;
  void* pool = collection->create_shm_pool_external(get_pool_size(), &zero, &commit);

  bool os_overcommits = Test_jemalloc_pages::get_os_overcommit_memory();
  if (!os_overcommits)
  {
    // Ensure no write capability after creation
    EXPECT_DEATH(memcpy(pool, get_arbitrary_data().c_str(), get_arbitrary_data().size()), ".*");
  }

  // Zero size decommit
  EXPECT_DEATH(collection->decommit_memory_pages_external(pool, 0), "length > 0");

  size_t page_size = Jemalloc_pages::get_page_size();
  EXPECT_GT(get_pool_size(), page_size);
  // Enable decommit
  Test_jemalloc_pages::set_os_overcommit_memory(false);
  // Commit a page
  EXPECT_TRUE(collection->commit_memory_pages_external(pool, page_size));
  // Ensure write capability at the page
  EXPECT_NO_THROW(memcpy(pool, get_arbitrary_data().c_str(), get_arbitrary_data().size()));
  // Decommit the page
  EXPECT_TRUE(collection->decommit_memory_pages_external(pool, page_size));
  // Ensure no write capability at the page
  EXPECT_DEATH(memcpy(pool, get_arbitrary_data().c_str(), get_arbitrary_data().size()), ".*");

  // Commit the page
  EXPECT_TRUE(collection->commit_memory_pages_external(pool, page_size));
  // Ensure write capability at the page
  EXPECT_NO_THROW(memcpy(pool, get_arbitrary_data().c_str(), get_arbitrary_data().size()));
  // Remove the page
  EXPECT_TRUE(collection->remove_shm_pool_external(pool, page_size, true));
  // Ensure no write capability at the page
  EXPECT_DEATH(memcpy(pool, get_arbitrary_data().c_str(), get_arbitrary_data().size()), ".*");
  // Restore overcommit
  Test_jemalloc_pages::set_os_overcommit_memory(os_overcommits);

  // Zero size commit
  EXPECT_DEATH(collection->commit_memory_pages_external(pool, 0), "length > 0");

  // Zero size purge
  EXPECT_DEATH(collection->purge_forced_memory_pages_external(pool, 0), "length > 0");

  // Remove shared memory pools that were created
  EXPECT_TRUE(remove_test_shm_objects_filesystem());
}

/// Tests involving the class interface, both public and protected.
TEST_F(Ipc_arena_test, Interface)
{
  FLOW_LOG_SET_CONTEXT(&get_test_logger(), Log_component::S_TEST);

  // Public interface
  {
    // Ensure start works
    shared_ptr<Test_ipc_arena> collection = create_collection();
    collection->start();
    EXPECT_NE(collection->get_jemalloc_arena_id(), 0u);
  }

  {
    shared_ptr<Test_ipc_arena> collection = create_collection();
    EXPECT_NE(collection->get_id(), 0u);

    // Register callback
    Test_event_listener event_listener;
    collection->set_test_event_listener(&event_listener);
    collection->start();

    const auto arena_id = collection->get_jemalloc_arena_id();

    // Perform allocation check
    void* p;
    EXPECT_TRUE(check_output([&]()
                             {
                               event_listener.reset_notifications();
                               // This must be the first allocation for the arena to ensure pool creation
                               p = collection->allocate(S_ALLOCATION_SIZE);
                               EXPECT_NE(event_listener.get_create_notification(), nullptr);
                             },
                             cout,
                             {
                               ostream_op_string("Created SHM pool at .* arena \\[", arena_id, "\\]"),
                               ostream_op_string("Allocated size \\[", S_ALLOCATION_SIZE,
                                                 "\\], arena \\[", arena_id, "\\]")
                             }));
    // (deallocate() is intentionally log-free -- see its impl comment -- so there is no output to check.)
    collection->deallocate(p);

    // Use shared object creation interface
    // Ensure that construction and destruction is executed properly and that the underlying object's destructor
    // is called
    {
      shared_ptr<size_t> constructor_counter = make_shared<size_t>(0);
      shared_ptr<size_t> destructor_counter = make_shared<size_t>(0);

      // Object to track construction and destruction
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

      // Ensure that the memory is allocated and the constructor of the object is called
      shared_ptr<Foo> foo;
      EXPECT_TRUE(check_output([&]()
                               {
                                 foo = collection->construct<Foo>(constructor_counter,
                                                                  destructor_counter);
                               },
                               cout,
                               ostream_op_string("Allocating size \\[", sizeof(Foo),
                                                 "\\], arena \\[", arena_id, "\\]")));
      EXPECT_EQ(*constructor_counter, 1u);
      EXPECT_EQ(*destructor_counter, 0u);

      if (foo != nullptr)
      {
        /* Ensure that the destructor for the object is called and the memory is deallocated
         * (via the counters: deallocate() is intentionally log-free -- see its impl comment -- so no
         * output check here). */
        foo = nullptr;
        EXPECT_EQ(*constructor_counter, 1u);
        EXPECT_EQ(*destructor_counter, 1u);
      }
      else
      {
        EXPECT_NE(foo, nullptr);
      }
    }

    // Destroy arena
    auto checks = form_arena_destruction_output_checks(collection);
    auto output = collect_output([&]() { collection = nullptr; });
    EXPECT_TRUE(check_output(output, checks));
    EXPECT_TRUE(check_empty_collection_in_output(output));
  }

  {
    // Ensure collection is not destroyed until all constructed objects are released.
    // We will allocate objects, remove all but one, release handle to collection and then
    // finally, release the final object.
    constexpr size_t N_OBJECTS = 3;
    const int ARBITRARY_VALUE = 5;

    auto collection = create_collection();
    collection->start();
    EXPECT_EQ(collection.use_count(), 1);

    // Construct objects
    deque<shared_ptr<int>> objects;
    for (size_t i = 0; i != N_OBJECTS; ++i)
    {
      auto cur_object = collection->construct<int>(ARBITRARY_VALUE);
      EXPECT_NE(cur_object, nullptr);
      objects.emplace_back(cur_object);

      // 1) Each object has a collection handle in its destructor
      // 2) This thread is holding a collection handle
      EXPECT_EQ(static_cast<size_t>(collection.use_count()), (objects.size() + 1));
    }

    flow::async::Single_thread_task_loop thread{&get_test_logger(), "toDealloc"};
    thread.start([&]()
    {
      // Get rid of object handles except one, which should not destroy anything
      while (objects.size() > 1)
      {
        objects.pop_front();
        EXPECT_EQ(static_cast<size_t>(collection.use_count()),
                  objects.size() + 1);
      }
    });
    thread.stop();

    // Cache output relating to arena destruction when collection is destroyed
    vector<string> output_checks = form_arena_destruction_output_checks(collection);
    // Get rid of collection handle, which should not destroy anything
    collection = nullptr;

    // Destroy arena
    EXPECT_TRUE(check_output([&]()
                             {
                               objects.pop_front();
                             },
                             cout,
                             output_checks));
  }

  // Non-public interface
  {
    Test_event_listener event_listener;
    auto collection = create_collection();
    // Register callback
    collection->set_test_event_listener(&event_listener);
    // Start collection
    collection->start();

    // Store original setting as we'll be manipulating it
    bool os_overcommits = Test_jemalloc_pages::get_os_overcommit_memory();
    bool commit;
    bool zero;

    // Jemalloc hooks
    {
      // Create pool
      // Commit will automatically be set to true if OS overcommits memory
      zero = !os_overcommits;
      commit = false;
      event_listener.reset_notifications();
      void* pool = collection->create_shm_pool_external(get_pool_size(), &zero, &commit);
      EXPECT_NE(pool, nullptr);
      EXPECT_EQ(commit, os_overcommits);
      EXPECT_EQ(zero, commit);
      shared_ptr<Test_event_listener::Create_notification> create_notification =
        event_listener.get_create_notification();
      if (create_notification != nullptr)
      {
        EXPECT_EQ(create_notification->get_shm_pool()->get_address(), pool);
        EXPECT_EQ(event_listener.get_num_create_notifications(), 1u);
      }
      else
      {
        EXPECT_NE(create_notification, nullptr);
      }
      EXPECT_EQ(event_listener.get_remove_notification(), nullptr);

      // Make sure behavior fits commit or decommit
      if (os_overcommits)
      {
        // Ensure write capability after creation
        EXPECT_NO_THROW(memcpy(pool, get_arbitrary_data().c_str(), get_arbitrary_data().size()));
      }
      // else this is checked in death tests

      // Disable decommit
      Test_jemalloc_pages::set_os_overcommit_memory(true);
      // Decommit will be ignored
      EXPECT_FALSE(collection->decommit_memory_pages_external(pool, get_pool_size()));
      // Ensure write capability
      EXPECT_NO_THROW(memcpy(pool, get_arbitrary_data().c_str(), get_arbitrary_data().size()));
      // Commit will be ignored
      EXPECT_FALSE(collection->commit_memory_pages_external(pool, get_pool_size()));
      // Ensure write capability
      EXPECT_NO_THROW(memcpy(pool, get_arbitrary_data().c_str(), get_arbitrary_data().size()));

      // Bogus pool
      void* bogus_pool = reinterpret_cast<void*>(0x1);

      // Enable decommit
      Test_jemalloc_pages::set_os_overcommit_memory(false);
      // Non-existent pool
      EXPECT_FALSE(collection->decommit_memory_pages_external(bogus_pool, get_pool_size()));
      // Decommit
      EXPECT_TRUE(collection->decommit_memory_pages_external(pool, get_pool_size()));
      // Bad range
      EXPECT_FALSE(collection->decommit_memory_pages_external(pool, (get_pool_size() + 1)));

      // Non-existent pool
      EXPECT_FALSE(collection->commit_memory_pages_external(bogus_pool, get_pool_size()));
      // Commit
      EXPECT_TRUE(collection->commit_memory_pages_external(pool, get_pool_size()));
      // Ensure write capability
      EXPECT_NO_THROW(memcpy(pool, get_arbitrary_data().c_str(), get_arbitrary_data().size()));
      // Bad range
      EXPECT_FALSE(collection->commit_memory_pages_external(pool, (get_pool_size() + 1)));

      // Non-existent pool
      EXPECT_FALSE(collection->purge_forced_memory_pages_external(bogus_pool, get_pool_size()));
      // Force purge
      EXPECT_TRUE(collection->purge_forced_memory_pages_external(pool, get_pool_size()));
      // Ensure write capability
      EXPECT_NO_THROW(memcpy(pool, get_arbitrary_data().c_str(), get_arbitrary_data().size()));
      // Bad range
      EXPECT_FALSE(collection->purge_forced_memory_pages_external(pool, (get_pool_size() + 1)));

      size_t page_size = Jemalloc_pages::get_page_size();
      // Tests below require this
      EXPECT_GT(get_pool_size(), (2 * page_size));
      EXPECT_LE(get_arbitrary_data().size(), page_size);
      // Remove pool
      event_listener.reset_notifications();
      // Remove partial pool
      EXPECT_TRUE(collection->remove_shm_pool_external(pool, page_size));
      EXPECT_EQ(event_listener.get_create_notification(), nullptr);
      EXPECT_EQ(event_listener.get_remove_notification(), nullptr);
      // Ensure write capability on remaining pages
      EXPECT_NO_THROW(memcpy((static_cast<char*>(pool) + page_size),
                             get_arbitrary_data().c_str(),
                             get_arbitrary_data().size()));
      // Remove partial pool
      EXPECT_TRUE(collection->remove_shm_pool_external((static_cast<char*>(pool) + get_pool_size() - page_size),
                                                       page_size));
      EXPECT_EQ(event_listener.get_create_notification(), nullptr);
      EXPECT_EQ(event_listener.get_remove_notification(), nullptr);
      // Ensure write capability on remaining pages
      EXPECT_NO_THROW(memcpy((static_cast<char*>(pool) + page_size),
                             get_arbitrary_data().c_str(),
                             get_arbitrary_data().size()));
      // Remove remainder of pool
      EXPECT_TRUE(collection->remove_shm_pool_external((static_cast<char*>(pool) + page_size),
                                                       (get_pool_size() - (2 * page_size))));
      EXPECT_EQ(event_listener.get_create_notification(), nullptr);
      shared_ptr<Test_event_listener::Remove_notification> remove_notification =
        event_listener.get_remove_notification();
      if (remove_notification == nullptr)
      {
        EXPECT_EQ(remove_notification->get_shm_pool()->get_address(), pool);
        EXPECT_TRUE(remove_notification->get_removed_shared_memory());
        EXPECT_EQ(event_listener.get_num_remove_notifications(), 1u);
      }
      else
      {
        // Failed
        EXPECT_NE(remove_notification, nullptr);
      }

      // Merge pool tests
      {
        const int OFFSET_1 = 100;
        const int OFFSET_2 = 50;
        const int OFFSET_DIFF = (OFFSET_1 - OFFSET_2);
        // Need at least a two space gap between offsets
        static_assert(OFFSET_DIFF > 1);
        const size_t POOL_ADDRESS_VALUE = 0x1000000;
        const size_t POOL_SIZE = 4 * get_pool_size();
        // Need at least a gap of at least POOL_SIZE as we may create a pool prior to this one
        if (POOL_ADDRESS_VALUE <= POOL_SIZE)
        {
          ADD_FAILURE() << "Pool address [" << POOL_ADDRESS_VALUE << "] is too low <= pool size [" << POOL_SIZE << "]";
        }
        // Need to encompass both offsets as we may create a pool after this one
        if (POOL_SIZE <= (OFFSET_1 + OFFSET_2))
        {
          ADD_FAILURE() << "Pool size [" << POOL_SIZE << "] is too small <= (offset size 1 + offset size 2) [" <<
            (OFFSET_1 + OFFSET_2) << "]";
        }

        /**
         * Creates a shared memory pool without any real memory backing.
         *
         * @param pool_address The address to create the pool.
         *
         * @return The shared memory pool.
         */
        auto create_shm_pool_functor =
          [&](void* pool_address) -> shared_ptr<Shm_pool>
          {
            shared_ptr<Shm_pool> shm_pool = collection->create_fake_shm_pool(pool_address, POOL_SIZE);
            EXPECT_NE(shm_pool, nullptr);
            return shm_pool;
          };

        /**
         * Removes a shared memory pool previously created by create_shm_pool_functor, which has no real memory
         * backing.
         *
         * @param shm_pool The shared memory pool to remove.
         *
         * @return Whether removal was successful.
         */
        auto remove_shm_pool_functor =
          [&](const shared_ptr<Shm_pool>& shm_pool) -> bool
          {
            bool result = collection->remove_fake_shm_pool(shm_pool);
            EXPECT_TRUE(result);
            return result;
          };

        /**
         * Executes a test that performs the following:
         * 1. Creates a shared memory pool at a particular address (with no real memory backing)
         * 2. Attempt to merge region at offset A from the pool, size A with region at offset B from the pool, size B
         * 3. Perform step 2 but with the regions specified in reverse order
         * 4. Perform above steps, but create surrounding pools from the memory pool created
         *
         * @param offset_a The first region offset from the allocated memory pool.
         * @param size_a The first region size.
         * @param offset_a The second region offset from the allocated memory pool.
         * @param size_a The second region size.
         * @param expected_result The expected result from merging the first and second regions together.
         * @param expected_result_extended_pools The expected result from merging the first and second regions
         *                                       together when we create pools surrounding the original pool.
         *
         * @return Whether the test passed.
         */
        auto run_merge_test_functor =
          [&](int offset_a,
              size_t size_a,
              int offset_b,
              size_t size_b,
              bool expected_result,
              bool expected_result_extended_pools) -> bool
          {
            // Range A must be <= range B
            if ((offset_a + static_cast<int>(size_a)) > offset_b)
            {
              ADD_FAILURE() << "Offset A + size A [" << (static_cast<int>(offset_a) + size_a) <<
                "] must be <= offset B [" << offset_b << "]";
              return false;
            }

            shared_ptr<Shm_pool> shm_pool = create_shm_pool_functor(reinterpret_cast<char*>(POOL_ADDRESS_VALUE));
            if (!shm_pool)
            {
              return false;
            }

            bool result = true;
            void* pool = shm_pool->get_address();
            if ((pool <= reinterpret_cast<void*>(abs(offset_a))) || (pool <= reinterpret_cast<void*>(abs(offset_b))))
            {
              ADD_FAILURE() << "Address of pool [" << pool << "] is unexpectedly <= abs(offset_a) [" <<
              abs(offset_a) << "] or <= abs(offset_b) [" << abs(offset_b) << "]";
              result = false;
            }
            else
            {
              for (int i = 0; i < 2; ++i)
              {
                // Shared memory pool adjacent to shm_pool at the beginning
                shared_ptr<Shm_pool> pre_shm_pool;
                // Shared memory pool adjacent to shm_pool at the end
                shared_ptr<Shm_pool> post_shm_pool;

                bool cur_expected_result;

                if (i == 0)
                {
                  cur_expected_result = expected_result;
                }
                else
                {
                  cur_expected_result = expected_result_extended_pools;

                  // Create surrounding pools as necessary
                  if (offset_a < 0)
                  {
                    void* pre_pool = reinterpret_cast<void*>(POOL_ADDRESS_VALUE - POOL_SIZE);
                    pre_shm_pool = create_shm_pool_functor(pre_pool);
                    if (!pre_shm_pool)
                    {
                      ADD_FAILURE() << "Could not create pool before target pool at address [" << pre_pool << "]";
                      result = false;
                    }
                  }

                  if ((offset_b + static_cast<int>(size_b)) > static_cast<int>(POOL_SIZE))
                  {
                    void* post_pool = reinterpret_cast<void*>(POOL_ADDRESS_VALUE + POOL_SIZE);
                    post_shm_pool = create_shm_pool_functor(post_pool);
                    if (!post_shm_pool)
                    {
                      ADD_FAILURE() << "Could not create pool after target pool at address [" << post_pool << "]";
                      result = false;
                    }
                  }

                  if (!pre_shm_pool && !post_shm_pool)
                  {
                    // No need to run redundant tests as the pools didn't change
                    continue;
                  }
                }

                if (cur_expected_result != collection->merge_memory_pages_external(pool,
                                                                               offset_a,
                                                                               size_a,
                                                                               pool,
                                                                               offset_b,
                                                                               size_b))
                {
                  ADD_FAILURE() << "Result of merge A-B [" << !cur_expected_result << "] did not match expected, "
                    "additional pools [" << (i != 0) << "]";
                  result = false;
                }

                // Perform vice versa merge, which should be identical in result
                if (cur_expected_result != collection->merge_memory_pages_external(pool,
                                                                                   offset_b,
                                                                                   size_b,
                                                                                   pool,
                                                                                   offset_a,
                                                                                   size_a))
                {
                  ADD_FAILURE() << "Result of merge B-A [" << !cur_expected_result << "] did not match expected, "
                    "additional pools [" << (i != 0) << "]";
                  result = false;
                }

                // Remove surrounding pools
                if (pre_shm_pool)
                {
                  result = (remove_shm_pool_functor(pre_shm_pool) && result);
                }
                if (post_shm_pool)
                {
                  result = (remove_shm_pool_functor(post_shm_pool) && result);
                }
              }
            }

            return remove_shm_pool_functor(shm_pool) && result;
          };

        // First region starting prior to pool, ending prior to pool, second region starting prior to pool
        {
          // Second region ending prior to pool, non-adjacent
          EXPECT_TRUE(run_merge_test_functor(-OFFSET_1, (OFFSET_DIFF - 1), -OFFSET_2, 1, false, false));
          // Second region ending prior to pool, adjacent
          EXPECT_TRUE(run_merge_test_functor(-OFFSET_1, OFFSET_DIFF, -OFFSET_2, 1, false, true));
          // Second region ending at pool start, non-adjacent
          EXPECT_TRUE(run_merge_test_functor(-OFFSET_1, (OFFSET_DIFF - 1), -OFFSET_2, OFFSET_2, false, false));
          // Second region ending at pool start, adjacent
          EXPECT_TRUE(run_merge_test_functor(-OFFSET_1, OFFSET_DIFF, -OFFSET_2, OFFSET_2, false, true));
          // Second region ending in pool, non-adjacent
          EXPECT_TRUE(run_merge_test_functor(-OFFSET_1, (OFFSET_DIFF - 1), -OFFSET_2, (OFFSET_2 + 1), false, false));
          // Second region ending in pool, adjacent
          EXPECT_TRUE(run_merge_test_functor(-OFFSET_1, OFFSET_DIFF, -OFFSET_2, (OFFSET_2 + 1), false, false));
          // Second region ending at pool end, non-adjacent
          EXPECT_TRUE(run_merge_test_functor(-OFFSET_1,
                                             (OFFSET_DIFF - 1),
                                             -OFFSET_2,
                                             (OFFSET_2 + POOL_SIZE),
                                             false,
                                             false));
          // Second region ending at pool end, adjacent
          EXPECT_TRUE(run_merge_test_functor(-OFFSET_1, OFFSET_DIFF, -OFFSET_2, (OFFSET_2 + POOL_SIZE), false, false));
          // Second region ending past pool end, non-adjacent
          EXPECT_TRUE(run_merge_test_functor(-OFFSET_1,
                                             (OFFSET_DIFF - 1),
                                             -OFFSET_2,
                                             (OFFSET_2 + POOL_SIZE + 1),
                                             false,
                                             false));
          // Second region ending past pool end, adjacent
          EXPECT_TRUE(run_merge_test_functor(-OFFSET_1,
                                             OFFSET_DIFF,
                                             -OFFSET_2,
                                             (OFFSET_2 + POOL_SIZE + 1),
                                             false,
                                             false));
        }

        // First region starting prior to pool, ending at pool start, second region starting at pool start
        {
          // Second region ending in pool, adjacent
          EXPECT_TRUE(run_merge_test_functor(-OFFSET_1, OFFSET_1, 0, 1, false, false));
          // Second region ending at pool end, adjacent
          EXPECT_TRUE(run_merge_test_functor(-OFFSET_1, OFFSET_1, 0, POOL_SIZE, false, false));
          // Second region ending past pool end, adjacent
          EXPECT_TRUE(run_merge_test_functor(-OFFSET_1, OFFSET_1, 0, (POOL_SIZE + 1), false, false));
        }

        // First region starting prior to pool, ending in pool, second region starting in pool
        {
          const size_t ADJACENT_SIZE = OFFSET_1 + OFFSET_2;

          // Second region ending in pool, non-adjacent
          EXPECT_TRUE(run_merge_test_functor(-OFFSET_1, (ADJACENT_SIZE - 1), OFFSET_2, 1, false, false));
          // Second region ending in pool, adjacent
          EXPECT_TRUE(run_merge_test_functor(-OFFSET_1, ADJACENT_SIZE, OFFSET_2, 1, false, false));
          // Second region ending at pool end, non-adjacent
          EXPECT_TRUE(run_merge_test_functor(-OFFSET_1,
                                             (ADJACENT_SIZE - 1),
                                             OFFSET_2,
                                             (POOL_SIZE - OFFSET_2),
                                             false,
                                             false));
          // Second region ending at pool end, adjacent
          EXPECT_TRUE(run_merge_test_functor(-OFFSET_1,
                                             ADJACENT_SIZE,
                                             OFFSET_2,
                                             (POOL_SIZE - OFFSET_2),
                                             false,
                                             false));
          // Second region ending past pool end, non-adjacent
          EXPECT_TRUE(run_merge_test_functor(-OFFSET_1,
                                             (ADJACENT_SIZE - 1),
                                             OFFSET_2,
                                             (POOL_SIZE - OFFSET_2 + 1),
                                             false,
                                             false));
          // Second region ending past pool end, adjacent
          EXPECT_TRUE(run_merge_test_functor(-OFFSET_1,
                                             ADJACENT_SIZE,
                                             OFFSET_2,
                                             (POOL_SIZE - OFFSET_2 + 1),
                                             false,
                                             false));
        }

        // First region starting prior to pool, ending at pool end, second region starting at pool end
        {
          // Second region ending past pool end, adjacent
          EXPECT_TRUE(run_merge_test_functor(-OFFSET_1, (OFFSET_1 + POOL_SIZE), POOL_SIZE, 1, false, false));
        }

        // First region starting prior to pool, ending past pool end, second region starting past pool end
        {
          // Second region ending past pool end, non-adjacent
          EXPECT_TRUE(run_merge_test_functor(-OFFSET_1,
                                             (OFFSET_1 + POOL_SIZE + OFFSET_2 - 1),
                                             (POOL_SIZE + OFFSET_2),
                                             1,
                                             false,
                                             false));
          // Second region ending past pool end, adjacent
          EXPECT_TRUE(run_merge_test_functor(-OFFSET_1,
                                             (OFFSET_1 + POOL_SIZE + OFFSET_2),
                                             (POOL_SIZE + OFFSET_2),
                                             1,
                                             false,
                                             false));
        }

        // First region starting at pool start, ending in pool, second region starting in pool
        {
          // Second region ending in pool, non-adjacent
          EXPECT_TRUE(run_merge_test_functor(0, (OFFSET_1 - 1), OFFSET_1, 1, false, false));
          // Second region ending in pool, adjacent
          EXPECT_TRUE(run_merge_test_functor(0, OFFSET_1, OFFSET_1, 1, true, true));
          // Second region ending at the end of the pool, non-adjacent
          EXPECT_TRUE(run_merge_test_functor(0, (OFFSET_1 - 1), OFFSET_1, (POOL_SIZE - OFFSET_1), false, false));
          // Second region ending at the end of the pool, adjacent
          EXPECT_TRUE(run_merge_test_functor(0, OFFSET_1, OFFSET_1, (POOL_SIZE - OFFSET_1), true, true));
          // Second region ending past the end of the pool, non-adjacent
          EXPECT_TRUE(run_merge_test_functor(0, (OFFSET_1 - 1), OFFSET_1, (POOL_SIZE - OFFSET_1 + 1), false, false));
          // Second region ending past the end of the pool, adjacent
          EXPECT_TRUE(run_merge_test_functor(0, OFFSET_1, OFFSET_1, (POOL_SIZE - OFFSET_1 + 1), false, false));
        }

        // First region starting at pool start, ending at pool end, second region starting at pool end
        {
          // Second region ending past pool end, adjacent
          EXPECT_TRUE(run_merge_test_functor(0, POOL_SIZE, POOL_SIZE, 1, false, false));
        }

        // First region starting at pool start, ending past pool end, second region starting past pool end
        {
          // Second region ending past pool end, non-adjacent
          EXPECT_TRUE(run_merge_test_functor(0, (POOL_SIZE + OFFSET_1 - 1), (POOL_SIZE + OFFSET_1), 1, false, false));
          // Second region ending past pool end, adjacent
          EXPECT_TRUE(run_merge_test_functor(0, (POOL_SIZE + OFFSET_1), (POOL_SIZE + OFFSET_1), 1, false, false));
        }

        // First region starting in pool, ending in pool, second region starting in pool
        {
          // Second region ending in pool, non-adjacent
          EXPECT_TRUE(run_merge_test_functor(OFFSET_2, (OFFSET_DIFF - 1), OFFSET_1, 1, false, false));
          // Second region ending in pool, adjacent
          EXPECT_TRUE(run_merge_test_functor(OFFSET_2, OFFSET_DIFF, OFFSET_1, 1, true, true));
          // Second region ending at the end of the pool, non-adjacent
          EXPECT_TRUE(run_merge_test_functor(OFFSET_2,
                                             (OFFSET_DIFF - 1),
                                             OFFSET_1,
                                             (POOL_SIZE - OFFSET_1),
                                             false,
                                             false));
          // Second region ending at the end of the pool, adjacent
          EXPECT_TRUE(run_merge_test_functor(OFFSET_2, OFFSET_DIFF, OFFSET_1, (POOL_SIZE - OFFSET_1), true, true));
          // Second region ending past the end of the pool, non-adjacent
          EXPECT_TRUE(run_merge_test_functor(OFFSET_2,
                                             (OFFSET_DIFF - 1),
                                             OFFSET_1,
                                             (POOL_SIZE - OFFSET_1 + 1),
                                             false,
                                             false));
          // Second region ending past the end of the pool, adjacent
          EXPECT_TRUE(run_merge_test_functor(OFFSET_2,
                                             OFFSET_DIFF,
                                             OFFSET_1,
                                             (POOL_SIZE - OFFSET_1 + 1),
                                             false,
                                             false));
        }

        // First region starting in pool, ending at pool end, second region starting at pool end
        {
          // Second region ending past pool end, adjacent
          EXPECT_TRUE(run_merge_test_functor(OFFSET_2, (POOL_SIZE - OFFSET_2), POOL_SIZE, 1, false, false));
        }

        // First region starting in pool, ending past pool end, second region starting past pool end
        {
          // Second region ending past pool end, non-adjacent
          EXPECT_TRUE(run_merge_test_functor(OFFSET_2,
                                             (POOL_SIZE - OFFSET_DIFF - 1),
                                             (POOL_SIZE + OFFSET_1),
                                             1,
                                             false,
                                             false));
          // Second region ending past pool end, adjacent
          EXPECT_TRUE(run_merge_test_functor(OFFSET_2,
                                             (POOL_SIZE - OFFSET_DIFF),
                                             (POOL_SIZE + OFFSET_1),
                                             1,
                                             false,
                                             false));
        }

        // First region starting at pool end, ending past pool end, second region starting past pool end
        {
          // Second region ending past pool end, non-adjacent
          EXPECT_TRUE(run_merge_test_functor(POOL_SIZE, (OFFSET_1 - 1), (POOL_SIZE + OFFSET_1), 1, false, false));
          // Second region ending past pool end, adjacent
          EXPECT_TRUE(run_merge_test_functor(POOL_SIZE, OFFSET_1, (POOL_SIZE + OFFSET_1), 1, false, true));
        }

        // First region starting past pool end, ending past pool end, second region starting past pool end
        {
          // Second region ending past pool end, non-adjacent
          EXPECT_TRUE(run_merge_test_functor((POOL_SIZE + OFFSET_1),
                                             (OFFSET_2 - 1),
                                             (POOL_SIZE + OFFSET_1 + OFFSET_2),
                                             1,
                                             false,
                                             false));
          // Second region ending past pool end, adjacent
          EXPECT_TRUE(run_merge_test_functor((POOL_SIZE + OFFSET_1),
                                             OFFSET_2,
                                             (POOL_SIZE + OFFSET_1 + OFFSET_2),
                                             1,
                                             false,
                                             true));
        }
      } // End Merge pool tests
    }

    // Restore original setting
    Test_jemalloc_pages::set_os_overcommit_memory(os_overcommits);
  }
}

/// Ensure that a different process can read the data.
TEST_F(Ipc_arena_test, Multiprocess)
{
  assert(S_ALLOCATION_SIZE >= get_arbitrary_data().size());
  shared_ptr<Test_ipc_arena> collection = create_collection();
  collection->start();
  FLOW_LOG_SET_CONTEXT(&get_test_logger(), Log_component::S_TEST);

  // Expect zero offset
  void* p1 = collection->allocate(S_ALLOCATION_SIZE);
  EXPECT_NE(p1, nullptr);
  memcpy(p1, get_arbitrary_data().c_str(), get_arbitrary_data().size());
  shared_ptr<Shm_pool> shm_pool1 = collection->lookup_shm_pool(p1);
  if (shm_pool1 != nullptr)
  {
    Shm_pool::size_t offset;
    if (shm_pool1->determine_offset(p1, offset))
    {
      EXPECT_EQ(offset, static_cast<Shm_pool::size_t>(0));
      // In new process, open and read the data and ensure it matches the contents
      Test_borrower borrower;
      EXPECT_EQ(0, borrower.execute_read_check(collection->get_id(),
                                               shm_pool1->get_id(),
                                               arena_lend::detail::Owner_spc_impl<Test_ipc_arena>{*collection}
                                                 .get_pool_name_base().str(),
                                               shm_pool1->get_size(),
                                               offset,
                                               get_arbitrary_data()));
    }
    else
    {
      ADD_FAILURE() << "Created object not within determined pool";
    }
  }
  else
  {
    FLOW_LOG_WARNING("Could not locate pool with address " << p1);
    EXPECT_NE(shm_pool1, nullptr);
  }

  // Expect non-zero offset
  void* p2 = collection->allocate(S_ALLOCATION_SIZE);
  EXPECT_NE(p2, nullptr);
  memcpy(p2, get_arbitrary_data().c_str(), get_arbitrary_data().size());
  shared_ptr<Shm_pool> shm_pool2 = collection->lookup_shm_pool(p2);
  if (shm_pool2 != nullptr)
  {
    Shm_pool::size_t offset;
    if (shm_pool2->determine_offset(p2, offset))
    {
      EXPECT_NE(offset, static_cast<Shm_pool::size_t>(0));
      // In new process, open and read the data and ensure it matches the contents
      EXPECT_EQ(0, Test_borrower().execute_read_check(collection->get_id(),
                                                      shm_pool2->get_id(),
                                                      arena_lend::detail::Owner_spc_impl<Test_ipc_arena>{*collection}
                                                        .get_pool_name_base().str(),
                                                      shm_pool2->get_size(),
                                                      offset,
                                                      get_arbitrary_data()));
    }
    else
    {
      ADD_FAILURE() << "Created object not within determined pool";
    }
  }
  else
  {
    FLOW_LOG_WARNING("Could not locate pool with address " << p2);
    EXPECT_NE(shm_pool2, nullptr);
  }

  collection->deallocate(p1);
  collection->deallocate(p2);
}

/// Empirically test that there isn't a race condition with memory management.
TEST_F(Ipc_arena_test, Multithread_load)
{
  // Number of memory objects to create initially
  constexpr size_t NUM_INITIAL_OBJECTS = 10000;
  // Size of the memory objects
  constexpr size_t OBJECT_SIZE = 40;
  // Number of (randomized) allocation/deallocation operations to perform
  constexpr size_t NUM_OPERATIONS = 10000;
  // Number of threads to perform allocation/deallocation operations
  constexpr size_t NUM_THREADS = 10;

  // Number of large memory objects to create initially
  constexpr size_t NUM_INITIAL_LARGE_OBJECTS = 100;
  /* Size of the large memory objects: several pages, comfortably above the max *small* size class
   * (~14KiB with 4KiB pages), so these allocations exercise jemalloc's large-allocation path. */
  const size_t LARGE_OBJECT_SIZE = Jemalloc_pages::get_page_size() * 8;
  // Number of (randomized) large allocation/deallocation operations to perform
  constexpr size_t NUM_LARGE_OBJECT_OPERATIONS = 100;

  Test_logger logger(flow::log::Sev::S_INFO);

  shared_ptr<Memory_manager> memory_manager(make_shared<Memory_manager>());
  auto collection = Test_ipc_arena::create(&logger, memory_manager);
  collection->start();

  /// Tracker for allocations that have not yet been deallocated
  class Allocation_tracker
  {
  public:
    /**
     * Adds an allocation to the list.
     *
     * @param p The allocation to be tracked.
     */
    void append(void* p)
    {
      m_object_list.emplace_back(p);
    }

    /**
     * Pops a random allocation from the list.
     *
     * @return If the list is not empty, the allocation that was removed; otherwise, nullptr.
     */
    void* pop_random(std::default_random_engine& random_generator)
    {
      if (m_object_list.size() == 0)
      {
        return nullptr;
      }

      std::uniform_int_distribution<size_t> distribution(0, (m_object_list.size() - 1));
      size_t remove_index = distribution(random_generator);
      auto cur_iter = m_object_list.begin();
      for (size_t cur_index = 0; cur_index < remove_index; ++cur_index)
      {
        ++cur_iter;
      }

      void* p = *cur_iter;
      m_object_list.erase(cur_iter);
      return p;
    }

  private:
    /// The list of allocations that are tracked.
    std::deque<void*> m_object_list;
  }; // class Allocation_tracker

  /**
   * Executes a test by performing the following:
   * 1. Allocating an initial pool of objects
   * 2. Randomly allocating or deallocating objects
   * 3. Cleaning up all objects
   *
   * @param num_initial_objects The number of objects to be allocated initially.
   * @param object_size The size of the objects to allocated. If 0, a size will randomly be chosen in the range
   *                    of [1, LARGE_OBJECT_SIZE].
   * @param num_operations The number of random allocation/deallocations to perform.
   */
  auto test_functor =
    [&](size_t num_initial_objects,
        size_t object_size,
        size_t num_operations,
        std::default_random_engine& random_generator)
    {
      Allocation_tracker tracker;
      // Random size allocation
      std::uniform_int_distribution<size_t> size_distribution(1, LARGE_OBJECT_SIZE);

      {
        // Allocate the initial pool of objects and track them
        for (size_t i = 0; i < num_initial_objects; ++i)
        {
          size_t size = ((object_size == 0) ? size_distribution(random_generator) : object_size);
          tracker.append(collection->allocate(size));
        }

        // Randomly decide whether to allocate or deallocate an object
        std::uniform_int_distribution<size_t> coin(0, 1);
        for (size_t i = 0; i < num_operations; ++i)
        {
          if (coin(random_generator) != 0)
          {
            // Allocate
            size_t size = ((object_size == 0) ? size_distribution(random_generator) : object_size);
            tracker.append(collection->allocate(size));
          }
          else
          {
            // Deallocate an object from the list randomly
            void* cur_object = tracker.pop_random(random_generator);
            if (cur_object != nullptr)
            {
              collection->deallocate(cur_object);
            }
          }
        }

        // Deallocate all the objects from the list in a random order
        void* cur_object;
        while ((cur_object = tracker.pop_random(random_generator)) != nullptr)
        {
          collection->deallocate(cur_object);
        }
      }
    };

  {
    vector<unique_ptr<std::thread>> threads;
    for (size_t i = 0; i < NUM_THREADS; ++i)
    {
      threads.emplace_back(
        make_unique<std::thread>(
          [&, i]() // i by value: the loop iterates (and ends) while the new thread is still starting up.
          {
            flow::log::Logger::this_thread_set_logged_nickname(ostream_op_string("testThread", i), &logger);

            std::default_random_engine random_generator;

            // Run test on small object sizes
            test_functor(NUM_INITIAL_OBJECTS, OBJECT_SIZE, NUM_OPERATIONS, random_generator);
            // Run test of large object sizes
            test_functor(NUM_INITIAL_LARGE_OBJECTS,
                         LARGE_OBJECT_SIZE,
                         NUM_LARGE_OBJECT_OPERATIONS,
                         random_generator);
            // Run test on random object sizes
            test_functor(NUM_INITIAL_OBJECTS, 0, NUM_OPERATIONS, random_generator);
          }));
    }

    for (const auto& cur_thread : threads)
    {
      cur_thread->join();
    }
  }

  // Ensure the shared memory pools are empty when they get destroyed
  logger.get_config().configure_default_verbosity(flow::log::Sev::S_TRACE, false);
  EXPECT_FALSE(check_output([&]() { collection = nullptr; },
                            cout,
                            "deregister_shm_pool.*remaining size: [1-9]",
                            false));
}

/**
 * This test has the owner removing the pool where a borrowed object lies by not decommitting and decommitting
 * (i.e., not purging and purging) the underlying shared memory. Decommitting causes the shared memory to be
 * zeroed in the file.
 */
TEST_F(Ipc_arena_test, Owner_shm_pool_removal)
{
  Test_logger logger;
  auto memory_manager(make_shared<Memory_manager>());
  auto owner_collection = Test_ipc_arena::create(&logger, memory_manager);
  owner_collection->start();
  auto borrower_collection =
    make_shared<Borrower_shm_pool_collection>(&logger,
                                              arena_lend::test::Test_shm_pool_collection::S_DEFAULT_COLLECTION_ID,
                                              Shared_name(arena_lend::detail::Owner_spc_impl<Test_ipc_arena>
                                                            {*owner_collection}.get_pool_name_base()));

  auto test_func = [&](bool decommit_pool)
    {
      // Create pool
      bool zero;
      bool commit;
      auto owner_pool_address = owner_collection->create_shm_pool_external(get_pool_size(), &zero, &commit);
      EXPECT_NE(owner_pool_address, nullptr);
      if (owner_pool_address == nullptr)
      {
        ADD_FAILURE() << "Owner pool address is nullptr";
        return;
      }

      // Copy data into the address
      size_t data_size = get_arbitrary_data().size();
      memcpy(owner_pool_address, get_arbitrary_data().c_str(), data_size);

      // Lookup pool
      auto owner_pool = owner_collection->lookup_shm_pool(owner_pool_address);
      if (owner_pool == nullptr)
      {
        ADD_FAILURE() << "Owner pool is nullptr";
        owner_collection->remove_shm_pool_external(owner_pool_address, get_pool_size(), commit);
        return;
      }

      // Create borrower pool
      Error_code ec;
      auto borrower_pool
        = borrower_collection->open_shm_pool(owner_pool->get_id(), owner_pool->get_size(), &ec);
      EXPECT_TRUE(borrower_pool) << "Error opening/mapping pool: [" << ec << "] [" << ec.message() << "].";
      EXPECT_EQ(memcmp(borrower_pool->get_address(), owner_pool_address, data_size), 0);

      // Remove owner pool and check borrower pool
      EXPECT_TRUE(owner_collection->remove_shm_pool_external(owner_pool_address,
                                                             owner_pool->get_size(),
                                                             decommit_pool));
      if (decommit_pool)
      {
        // Pool was zeroed
        EXPECT_EQ(memcmp(borrower_pool->get_address(), string(data_size, '\0').c_str(), data_size), 0);
      }
      else
      {
        // Pool was not zeroed
        EXPECT_EQ(memcmp(borrower_pool->get_address(), get_arbitrary_data().c_str(), data_size), 0);
      }

      borrower_collection->release_shm_pool(borrower_pool);
    };

  test_func(true);
  test_func(false);
}

} // namespace ipc::shm::arena_lend::jemalloc::test
