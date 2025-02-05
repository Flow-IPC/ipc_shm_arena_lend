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
#include "ipc/shm/arena_lend/jemalloc/shm_pool_collection.hpp"
#include "ipc/shm/arena_lend/jemalloc/test/test_jemalloc_pages.hpp"
#include "ipc/shm/arena_lend/shm_pool.hpp"
#include "ipc/shm/arena_lend/test/test_borrower.hpp"
#include "ipc/shm/arena_lend/test/test_event_listener.hpp"
#include "ipc/shm/arena_lend/test/test_shm_object.hpp"
#include "ipc/shm/arena_lend/test/test_shm_pool_collection.hpp"
#include <flow/test/test_common_util.hpp>
#include "ipc/test/test_logger.hpp"
#include <flow/test/test_common_util.hpp>
#include <iostream>
#include <sys/mman.h>
#include <deque>
#include <optional>
#include <random>

using ipc::test::Test_logger;
using std::array;
using std::cerr;
using std::cout;
using std::deque;
using std::make_shared;
using std::make_unique;
using std::map;
using std::ostringstream;
using std::set;
using std::shared_ptr;
using std::size_t;
using std::static_pointer_cast;
using std::string;
using std::stringstream;
using std::to_string;
using std::unique_ptr;
using std::vector;

using namespace ipc::shm::arena_lend::test;
using namespace flow::test;

namespace ipc::shm::arena_lend::jemalloc::test
{

namespace
{
/// Alias for the arena identifier.
using Arena_id = Shm_pool_collection::Arena_id;

/**
 * Overrides Shm_pool_collection to gain access to protected members for testing.
 */
class Test_shm_pool_collection :
  public Shm_pool_collection
{
public:
  /// Alias for the thread cache identifier.
  using Thread_cache_id = Shm_pool_collection::Thread_cache_id;
  // Make public
  using Shm_pool_collection::Thread_cache;
  using Shm_pool_collection::Thread_local_data;
  using Shm_pool_collection::allocate;
  using Shm_pool_collection::deallocate;
  using Shm_pool_collection::get_jemalloc_memory_manager;

  /// Container for Thread_cache internal class.
  class Thread_cache_holder
  {
  public:
    /**
     * Comparison for sorting purposes.
     */
    struct Less_than
    {
      /**
       * Compares whether the first value is less than the second value.
       *
       * @param lhs The left value.
       * @param rhs The right value.
       *
       * @return See above.
       */
      bool operator()(const shared_ptr<Thread_cache_holder>& lhs, const shared_ptr<Thread_cache_holder>& rhs) const
      {
        return (*lhs < *rhs);
      }
    }; // struct Less_than

    /**
     * Constructor.
     *
     * @param thread_cache The contained object.
     */
    Thread_cache_holder(const shared_ptr<Thread_cache>& thread_cache)
      : m_thread_cache(thread_cache)
    {
    }

    /**
     * Returns whether the thread caches are equal.
     *
     * @param other Comparison object.
     *
     * @returns See above.
     */
    bool operator==(const Thread_cache_holder& other) const
    {
      return (m_thread_cache == other.m_thread_cache);
    }

    /**
     * Returns whether the thread caches are not equal.
     *
     * @param other Comparison object.
     *
     * @returns See above.
     */
    bool operator!=(const Thread_cache_holder& other) const
    {
      return !(*this == other);
    }

    /**
     * Returns whether another object's thread cache pointer is greater or equal to this thread cache's pointer.
     *
     * @param other The other Thread_cache_holder object.
     *
     * @return See above.
     */
    bool operator<(const Thread_cache_holder& other) const
    {
      return (m_thread_cache < other.m_thread_cache);
    }

    /**
     * Returns the underlying thread cache object.
     *
     * @return See above.
     */
    shared_ptr<Thread_cache> get_thread_cache() const
    {
      return m_thread_cache;
    }

    /**
     * Returns the thread cache id.
     *
     * @return See above.
     */
    Thread_cache_id get_thread_cache_id() const
    {
      return m_thread_cache->get_thread_cache_id();
    }

  private:
    /// The thread cache.
    shared_ptr<Thread_cache> m_thread_cache;
  }; // class Thread_cache_holder

  /**
   * Creates an instance of this class along with its arenas. We require this, because the construct() interface
   * requires the use of shared pointers.
   *
   * @param logger Used for logging purposes.
   * @param memory_manager The memory allocator.
   *
   * @return Upon success, a shared pointer to an instance of this class; otherwise, an empty shared pointer.
   *
   * @see Shm_pool_collection::construct()
   */
  static shared_ptr<Test_shm_pool_collection> create(
    flow::log::Logger* logger,
    const shared_ptr<Memory_manager>& memory_manager)
  {
    return shared_ptr<Test_shm_pool_collection>(
      new Test_shm_pool_collection(logger, memory_manager));
  }

  /// Destructor.
  virtual ~Test_shm_pool_collection() override
  {
    FLOW_LOG_TRACE("~Test_shm_pool_collection()");
  }

  /**
   * Returns a holder containing the thread cache.
   *
   * @param arena_id The arena where the thread cache resides.
   *
   * @return See above.
   *
   * @see Shm_pool_collection::get_or_create_thread_cache_external
   */
  shared_ptr<Thread_cache_holder> get_or_create_thread_cache_external(Arena_id arena_id)
  {
    return make_shared<Thread_cache_holder>(get_or_create_thread_cache(arena_id));
  }

  /**
   * Returns the arena id commonly used for the tests.
   *
   * @param arena_id Output parameter to be filled in when the result is true; otherwise, unmodified.
   *
   * @return Whether there are any arenas.
   */
  bool get_default_arena_id(Arena_id& arena_id) const
  {
    const set<Arena_id>& arenas = get_arena_ids();
    if (arenas.empty())
    {
      EXPECT_FALSE(arenas.empty());
      return false;
    }

    arena_id = *arenas.begin();
    return true;
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
    Arena_id arena_id;
    if (!get_default_arena_id(arena_id))
    {
      return nullptr;
    }

    return create_shm_pool(nullptr,
                           size,
                           Jemalloc_pages::get_page_size(),
                           zero,
                           commit,
                           arena_id);
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
    const auto id = detail::Shm_pool_offset_ptr_data_base::generate_pool_id();
    const string name = generate_shm_object_name(id);

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
    Arena_id arena_id;
    if (!get_default_arena_id(arena_id))
    {
      return false;
    }

    return remove_shm_pool(address, size, committed, arena_id);
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
    Arena_id arena_id;
    if (!get_default_arena_id(arena_id))
    {
      return false;
    }

    return commit_memory_pages(address, size, 0, size, arena_id);
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
    Arena_id arena_id;
    if (!get_default_arena_id(arena_id))
    {
      return false;
    }

    return decommit_memory_pages(address, size, 0, size, arena_id);
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
    Arena_id arena_id;
    if (!get_default_arena_id(arena_id))
    {
      return false;
    }

    return purge_forced_memory_pages(address, size, 0, size, arena_id);
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
    Arena_id arena_id;
    if (!get_default_arena_id(arena_id))
    {
      return false;
    }

    return merge_memory_pages(address_a, size_a, address_b, size_b, false, arena_id);
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

  /**
   * Create a thread cache object.
   *
   * @param arena_id The arena which the thread cache will be associated with.
   *
   * @return The created thread cache object.
   */
  shared_ptr<Thread_cache> create_thread_cache_object(Arena_id arena_id)
  {
    shared_ptr<Thread_cache> thread_cache;
    EXPECT_TRUE(check_output([&]()
                             {
                               thread_cache = make_shared<Thread_cache>(
                                 static_pointer_cast<Shm_pool_collection>(shared_from_this()),
                                 arena_id);
                             },
                             cout,
                             "Created thread cache id "));
    return thread_cache;
  }

  /**
   * Retrieves the creator collection information of an object. This assumes that the object was created with
   * a deleter for the shared pointer of base type Object_deleter. If it was not, a crash will likely occur.
   *
   * @tparam T The object type.
   * @param object The object.
   * @param collection If the result is true, the pool collection that created the object, which should be an
   *                   instance of this class.
   * @param arena_id If the result is true, the arena id within the pool collection of this class.
   * @param thread_cache_id If the result is true, a thread cache id holder within the pool collection of this class,
   *                        which may be empty if no thread cache was used.
   *
   * @return Whether the creator collection information was found.
   */
  template <typename T>
  static bool determine_object_collection_info(const std::shared_ptr<T>& object,
                                               std::shared_ptr<Shm_pool_collection>& collection,
                                               Arena_id& arena_id,
                                               std::optional<Thread_cache_id>& thread_cache_id)
  {
    Object_deleter_cache* deleter_cache = std::get_deleter<Object_deleter_cache>(object);
    if (deleter_cache != nullptr)
    {
      std::shared_ptr<Thread_cache> thread_cache = deleter_cache->get_thread_cache();
      collection = thread_cache->get_owner();
      arena_id = thread_cache->get_arena_id();
      thread_cache_id = thread_cache->get_thread_cache_id();
      return true;
    }

    Object_deleter_no_cache* deleter_no_cache = std::get_deleter<Object_deleter_no_cache>(object);
    if (deleter_no_cache != nullptr)
    {
      collection = deleter_no_cache->get_pool_collection();
      arena_id = deleter_no_cache->get_arena_id();
      thread_cache_id = {};
      return true;
    }

    return false;
  }

private:
  /**
   * Constructor.
   *
   * @see Shm_pool_collection::Shm_pool_collection()
   */
  Test_shm_pool_collection(flow::log::Logger* logger,
                                    const shared_ptr<Memory_manager>& memory_manager) :
    Shm_pool_collection(logger,
                        arena_lend::test::Test_shm_pool_collection::S_DEFAULT_COLLECTION_ID,
                        memory_manager,
                        create_shm_object_name_generator(),
                        util::shared_resource_permissions(util::Permissions_level::S_GROUP_ACCESS))
  {
  }
}; // class Test_shm_pool_collection

/**
 * Google test fixture.  Can't name it Shm_pool_collection_test, as there's one named same in another namespace;
 * this makes Google test framework barf (albeit very politely, wherein it explains this is what usually makes
 * that problem occur).
 */
class Jemalloc_shm_pool_collection_test :
  public ::testing::Test
{
public:
  /// Number of arena to use in the tests.
  static constexpr size_t S_ARENAS = 2;
  /// Default allocation size.
  static constexpr size_t S_ALLOCATION_SIZE = 100;

  /**
   * Returns the size of memory pools used in the tests.
   *
   * @return See above.
   */
  static inline size_t get_pool_size()
  {
    return (Jemalloc_pages::get_page_size() * 4);
  }

  /// @see form_arena_destruction_output_checks(const shared_ptr<Shm_pool_collection>&)
  static vector<string> form_arena_destruction_output_checks(
    const shared_ptr<Test_shm_pool_collection>& collection)
  {
    return form_arena_destruction_output_checks(static_pointer_cast<Shm_pool_collection>(collection));
  }
  /**
   * Forms a list of strings related to arena destruction to check the output of when a collection is destroyed.
   *
   * @param collection The collection that will be destroyed.
   *
   * @return See above.
   */
  static vector<string> form_arena_destruction_output_checks(
    const shared_ptr<Shm_pool_collection>& collection)
  {
    vector<string> output_checks;

    const set<Arena_id>& arenas = collection->get_arena_ids();
    for (const auto& cur_arena_id : arenas)
    {
      // Add to checks for later when the arena is destroyed
      output_checks.emplace_back("Destroyed arena " + to_string(cur_arena_id));
    }

    return output_checks;
  }

  /**
   * Destroys a thread cache object. The number of references to the thread cache must be only the passed in
   * handle.
   *
   * @param thread_cache The thread cache object to destroy.
   */
  static void destroy_thread_cache_object(
    shared_ptr<Test_shm_pool_collection::Thread_cache>& thread_cache)
  {
    EXPECT_EQ(thread_cache.use_count(), 1);

    stringstream ss;
    ss << "Destroyed thread cache id " << thread_cache->get_thread_cache_id();

    EXPECT_TRUE(check_output([&]()
                             {
                               thread_cache = nullptr;
                             },
                             cout,
                             ss.str()));
  }

  /// Constructor
  Jemalloc_shm_pool_collection_test() :
    // We need data level here for certain tests
    m_test_logger(flow::log::Sev::S_DATA),
    m_memory_manager(make_shared<Memory_manager>(&m_test_logger))
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
  shared_ptr<Test_shm_pool_collection> create_collection()
  {
    return Test_shm_pool_collection::create(&m_test_logger, m_memory_manager);
  }

private:
  /// Used for logging purposes.
  Test_logger m_test_logger;
  /// Memory allocator.
  shared_ptr<Memory_manager> m_memory_manager;
  /// Callbacks for collection events.
  Test_event_listener m_event_listener;
}; // class Jemalloc_shm_pool_collection_test

} // Anonymous namespace

/// Death tests - suffixed with DeathTest per Googletest conventions, aliased to fixture.
using Jemalloc_shm_pool_collection_DeathTest = Jemalloc_shm_pool_collection_test;
#ifdef NDEBUG // These "deaths" occur only if assert()s enabled; else these are guaranteed failures.
TEST_F(Jemalloc_shm_pool_collection_DeathTest, DISABLED_Interface)
#else
TEST_F(Jemalloc_shm_pool_collection_DeathTest, Interface)
#endif
{
  auto collection = create_collection();

  // Public interface
  {
    // Not started yet
    EXPECT_DEATH(collection->allocate(100), "m_started");
    EXPECT_DEATH(collection->allocate(100, 0UL), "m_started");
    EXPECT_DEATH(collection->construct<int>(false, 100), "m_started");
    EXPECT_DEATH(collection->construct_in_arena<int>(0UL, false, 100), "m_started");
    EXPECT_DEATH(collection->construct_in_arena<int>(0UL, true, 100), "m_started");
    EXPECT_DEATH(collection->deallocate(reinterpret_cast<void*>(0x1)), "m_started");
    EXPECT_DEATH(collection->deallocate(reinterpret_cast<void*>(0x1), 0UL), "m_started");
    EXPECT_DEATH(collection->get_arena_ids(), "m_started");
  }
  EXPECT_TRUE(collection->start());

  // Bad arena
  EXPECT_DEATH(collection->allocate(100, 0UL), "m_arenas.find\\(arena_id\\) != m_arenas.end\\(\\)");
  EXPECT_DEATH(collection->deallocate(reinterpret_cast<void*>(0x1), 0UL),
               "m_arenas.find\\(arena_id\\) != m_arenas.end\\(\\)");
  EXPECT_DEATH(collection->construct_in_arena<int>(0UL, false, 100)
               , "m_arenas.find\\(arena_id\\) != m_arenas.end\\(\\)");

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
TEST_F(Jemalloc_shm_pool_collection_test, Interface)
{
  using Thread_cache = Test_shm_pool_collection::Thread_cache;
  using Thread_cache_id = Test_shm_pool_collection::Thread_cache_id;
  using Thread_cache_holder = Test_shm_pool_collection::Thread_cache_holder;

  FLOW_LOG_SET_CONTEXT(&get_test_logger(), Log_component::S_TEST);

  // Public interface
  {
    // Ensure no parameter start is one arena
    shared_ptr<Test_shm_pool_collection> collection = create_collection();
    EXPECT_TRUE(collection->start());
    EXPECT_EQ(collection->get_arena_ids().size(), 1UL);
  }

  {
    shared_ptr<Test_shm_pool_collection> collection = create_collection();
    EXPECT_EQ(collection->get_id(), arena_lend::test::Test_shm_pool_collection::S_DEFAULT_COLLECTION_ID);

    // Register callback
    Test_event_listener event_listener;
    collection->add_event_listener(&event_listener);
    // Invalid number of arenas
    EXPECT_FALSE(collection->start(0));
    EXPECT_TRUE(collection->start(S_ARENAS));
    // Already started
    EXPECT_TRUE(check_output([&]()
                             {
                               EXPECT_TRUE(collection->start());
                             },
                             cerr,
                             "Already started"));
    const set<Arena_id>& arenas = collection->get_arena_ids();
    EXPECT_EQ(arenas.size(), S_ARENAS);

    // Perform allocation check on each arena
    for (const auto& cur_arena_id : arenas)
    {
      bool use_default = (cur_arena_id == *(collection->get_arena_ids().begin()));
      vector<string> output_checks;

      stringstream ss;
      ss << "Allocated size " << S_ALLOCATION_SIZE << ", arena " << cur_arena_id;
      void* p;
      EXPECT_TRUE(check_output([&]()
                               {
                                 event_listener.reset_notifications();
                                 // This must be the first allocation for the arena to ensure pool creation
                                 if (use_default)
                                 {
                                   p = collection->allocate(S_ALLOCATION_SIZE);
                                 }
                                 else
                                 {
                                   p = collection->allocate(S_ALLOCATION_SIZE, cur_arena_id);
                                 }
                                 EXPECT_NE(event_listener.get_create_notification(), nullptr);
                               },
                               cout,
                               {
                                 ("Created SHM pool at .* arena " + to_string(cur_arena_id)),
                                 ss.str()
                               }));
      // Reset stream
      ss.str("");

      ss << "Deallocated address " << p << ", arena " << cur_arena_id;
      EXPECT_TRUE(check_output([&]()
                               {
                                 if (use_default)
                                 {
                                   collection->deallocate(p);
                                 }
                                 else
                                 {
                                   collection->deallocate(p, cur_arena_id);
                                 }
                               },
                               cout,
                               ss.str()));
      // Reset stream
      ss.str("");

      // Use shared object creation interface; first, no thread cache and then thread cache
      // Ensure that construction and destruction is executed properly and that the underlying object's destructor
      // is called
      bool use_cache = false;
      do
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
        ss << "Allocating size " << sizeof(Foo) << ", arena " << cur_arena_id << ", " << (use_cache ? "" : "no ") <<
          "thread cache";

        shared_ptr<Foo> foo;
        EXPECT_TRUE(check_output([&]()
                                 {
                                   if (use_default)
                                   {
                                     foo = collection->construct<Foo>(use_cache,
                                                                      constructor_counter,
                                                                      destructor_counter);
                                   }
                                   else
                                   {
                                     foo = collection->construct_in_arena<Foo>(cur_arena_id,
                                                                               use_cache,
                                                                               constructor_counter,
                                                                               destructor_counter);
                                   }
                                 },
                                 cout,
                                 ss.str()));
        // Reset stream
        ss.str("");
        EXPECT_EQ(*constructor_counter, 1UL);
        EXPECT_EQ(*destructor_counter, 0UL);

        if (foo != nullptr)
        {
          ss << "Deallocating address " << static_cast<void*>(foo.get()) << ", arena " << cur_arena_id <<
            ", " << (use_cache ? "" : "no ") << "thread cache";

          // Ensure that the destructor for the object is called and the memory is deallocated
          EXPECT_TRUE(check_output([&]()
                                   {
                                     foo = nullptr;
                                   },
                                   cout,
                                   ss.str()));
          // Reset stream
          ss.str("");
          EXPECT_EQ(*constructor_counter, 1UL);
          EXPECT_EQ(*destructor_counter, 1UL);
        }
        else
        {
          EXPECT_NE(foo, nullptr);
        }

        // We must destroy all the caches before we can clean up
        if (use_cache)
        {
          collection->flush_current_thread_cache_all();
          collection->remove_current_thread_cache();
        }

        use_cache = !use_cache;
      } while (use_cache);
    }

    // Destroy arenas
    auto checks = form_arena_destruction_output_checks(collection);
    auto output = collect_output([&]() { collection = nullptr; });
    EXPECT_TRUE(check_output(output, checks));
    EXPECT_TRUE(check_empty_collection_in_output(output));
  }

  {
    // Ensure collection is not destroyed until all constructed objects are released. In addition,
    // ensure that we can retrieve the object's arena, collection and thread cache from the object.
    // We will allocate objects in arenas, remove all but one, release handle to collection and then
    // finally, release the final object.
    const size_t ARENAS = 3;
    const int ARBITRARY_VALUE = 5;

    auto collection = create_collection();
    EXPECT_TRUE(collection->start(ARENAS));
    EXPECT_EQ(collection.use_count(), 1);

    // Construct objects in each arena
    deque<shared_ptr<int>> objects;
    unsigned int num_expected_thread_caches = 0;
    const set<Arena_id>& arena_ids = collection->get_arena_ids();
    for (const auto& cur_arena_id : arena_ids)
    {
      // Create an object without thread cache and then with thread cache
      bool use_cache = false;
      do
      {
        shared_ptr<int> cur_object;
        if (objects.size() == 0)
        {
          // Construct using first arena
          cur_object = collection->construct<int>(use_cache, ARBITRARY_VALUE);
        }
        else
        {
          cur_object = collection->construct_in_arena<int>(cur_arena_id, use_cache, ARBITRARY_VALUE);
        }
        EXPECT_NE(cur_object, nullptr);
        objects.emplace_back(cur_object);
        if (use_cache)
        {
          ++num_expected_thread_caches;
        }

        // 1) Each object has a collection handle in its destructor
        // 2) Each thread cache has a collection handle
        // 3) This thread is holding a collection handle
        EXPECT_EQ(static_cast<size_t>(collection.use_count()), (objects.size() + num_expected_thread_caches + 1));

        // Check that the creator information can be obtained
        shared_ptr<Shm_pool_collection> object_collection;
        Arena_id object_arena_id = 0;
        std::optional<Test_shm_pool_collection::Thread_cache_id> thread_cache_id;
        EXPECT_TRUE(Test_shm_pool_collection::determine_object_collection_info(cur_object,
                                                                               object_collection,
                                                                               object_arena_id,
                                                                               thread_cache_id));
        EXPECT_EQ(object_collection, collection);
        EXPECT_EQ(object_arena_id, cur_arena_id);
        EXPECT_EQ(thread_cache_id.has_value(), use_cache);

        use_cache = !use_cache;
      } while (!use_cache);
    }

    // Get rid of object handles except one, which should not destroy anything
    while (objects.size() > 1)
    {
      objects.pop_front();
      EXPECT_EQ(static_cast<size_t>(collection.use_count()), (objects.size() + num_expected_thread_caches + 1));
    }

    // Get rid of thread caches
    collection->remove_current_thread_cache();
    EXPECT_EQ(static_cast<size_t>(collection.use_count()), (objects.size() + 1));

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
    collection->add_event_listener(&event_listener);
    // Start collection
    EXPECT_TRUE(collection->start(S_ARENAS));

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
        EXPECT_EQ(event_listener.get_num_create_notifications(), 1UL);
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
        EXPECT_EQ(event_listener.get_num_remove_notifications(), 1UL);
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

            return (remove_shm_pool_functor(shm_pool) && result);
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

  // Non-public Thread_cache creation
  {
    array<shared_ptr<Test_shm_pool_collection>, S_ARENAS> owners;
    for (auto& cur_owner : owners)
    {
      cur_owner = create_collection();
      EXPECT_TRUE(cur_owner->start(S_ARENAS));
    }

    set<shared_ptr<Thread_cache_holder>, Thread_cache_holder::Less_than> thread_caches;
    {
      const set<Arena_id>& arena_ids = owners[0]->get_arena_ids();
      EXPECT_EQ(arena_ids.size(), S_ARENAS);

      auto iter = arena_ids.begin();
      shared_ptr<Thread_cache_holder> thread_cache = owners[0]->get_or_create_thread_cache_external(*iter);
      EXPECT_TRUE(thread_caches.emplace(thread_cache).second);
      // The same thread cache should be returned from a subsequent request
      shared_ptr<Thread_cache_holder> thread_cache_dup = owners[0]->get_or_create_thread_cache_external(*iter);
      EXPECT_EQ(*thread_cache_dup, *thread_cache);
      EXPECT_FALSE(thread_caches.emplace(thread_cache_dup).second);
      // A different arena yields a different cache
      ++iter;
      EXPECT_TRUE(thread_caches.emplace(owners[0]->get_or_create_thread_cache_external(*iter)).second);
    }
    // A different collection yields a different cache
    {
      const set<Arena_id>& arena_ids = owners[1]->get_arena_ids();
      EXPECT_EQ(arena_ids.size(), S_ARENAS);

      const auto& iter = arena_ids.begin();
      shared_ptr<Thread_cache_holder> cur_thread_cache = owners[1]->get_or_create_thread_cache_external(*iter);
      EXPECT_TRUE(thread_caches.emplace(cur_thread_cache).second);
    }

    // Clean up
    for (auto& cur_owner : owners)
    {
      cur_owner->remove_current_thread_cache();
    }
  }

  // Non-public Thread_cache class
  {
    const size_t NUM_THREAD_CACHES = 5;
    shared_ptr<Test_shm_pool_collection> owner = create_collection();
    // These ids do not need to be actual arena ids
    array<Arena_id, NUM_THREAD_CACHES> arena_ids = {1, 5, 5, 11, 20};
    map<Thread_cache_id, shared_ptr<Thread_cache>> thread_caches;

    for (const auto& cur_arena_id : arena_ids)
    {
      shared_ptr<Thread_cache> cur_thread_cache = owner->create_thread_cache_object(cur_arena_id);
      EXPECT_EQ(cur_thread_cache->get_owner(), owner);
      EXPECT_EQ(cur_thread_cache->get_arena_id(), cur_arena_id);
      auto result = thread_caches.emplace(cur_thread_cache->get_thread_cache_id(), cur_thread_cache);
      EXPECT_TRUE(result.second);
    }
  }

  // Non-public Thread_local_data class
  {
    // The number of collections we will be using
    const size_t NUM_OWNERS = 2;
    // The number of caches we will be using
    const size_t NUM_CACHES = 3;

    // Populate collections
    array<shared_ptr<Test_shm_pool_collection>, NUM_OWNERS> owners;
    for (auto& iter : owners)
    {
      iter = create_collection();
    }

    Test_shm_pool_collection::Thread_local_data thread_local_data;
    // Populate arena ids, which do not need to be actual arena ids
    array<Arena_id, NUM_CACHES> arena_ids = {10, 11, 12};
    array<shared_ptr<Thread_cache>, NUM_CACHES> caches;

    // Nothing in cache
    EXPECT_EQ(thread_local_data.get_cache(arena_ids[0]), nullptr);
    EXPECT_FALSE(thread_local_data.remove_caches_by_owner(owners[0]));
    // Add entry in cache
    caches[0] = owners[0]->create_thread_cache_object(arena_ids[0]);
    EXPECT_TRUE(thread_local_data.insert_cache(caches[0]));
    EXPECT_EQ(thread_local_data.get_cache(arena_ids[0]), caches[0]);
    // Attempt to add duplicate entry in cache
    EXPECT_TRUE(thread_local_data.insert_cache(caches[0]));
    EXPECT_EQ(thread_local_data.get_cache(arena_ids[0]), caches[0]);
    // Add new entry in cache with same owner
    caches[1] = owners[0]->create_thread_cache_object(arena_ids[1]);
    // Each arena has its own unique thread cache ids, so there should be no collision
    EXPECT_NE(caches[0]->get_thread_cache_id(), caches[1]->get_thread_cache_id());
    EXPECT_TRUE(thread_local_data.insert_cache(caches[1]));
    EXPECT_EQ(thread_local_data.get_cache(arena_ids[0]), caches[0]);
    EXPECT_EQ(thread_local_data.get_cache(arena_ids[1]), caches[1]);
    // Add new entry in cache with different owner
    caches[2] = owners[1]->create_thread_cache_object(arena_ids[2]);
    EXPECT_TRUE(thread_local_data.insert_cache(caches[2]));
    EXPECT_EQ(thread_local_data.get_cache(arena_ids[0]), caches[0]);
    EXPECT_EQ(thread_local_data.get_cache(arena_ids[1]), caches[1]);
    EXPECT_EQ(thread_local_data.get_cache(arena_ids[2]), caches[2]);
    // Remove cache with multiple owners
    EXPECT_TRUE(thread_local_data.remove_caches_by_owner(owners[0]));
    EXPECT_FALSE(thread_local_data.remove_caches_by_owner(owners[0]));    
    EXPECT_EQ(thread_local_data.get_cache(arena_ids[0]), nullptr);
    EXPECT_EQ(thread_local_data.get_cache(arena_ids[1]), nullptr);
    EXPECT_EQ(thread_local_data.get_cache(arena_ids[2]), caches[2]);
    // Remove cache with single owner
    EXPECT_TRUE(thread_local_data.remove_caches_by_owner(owners[1]));
    EXPECT_FALSE(thread_local_data.remove_caches_by_owner(owners[1]));
    EXPECT_EQ(thread_local_data.get_cache(arena_ids[0]), nullptr);
    EXPECT_EQ(thread_local_data.get_cache(arena_ids[1]), nullptr);
    EXPECT_EQ(thread_local_data.get_cache(arena_ids[2]), nullptr);

    // Destroy caches
    destroy_thread_cache_object(caches[0]);
    destroy_thread_cache_object(caches[1]);
    destroy_thread_cache_object(caches[2]);
  }
}

/// Ensure that a different thread has different thread cache information.
TEST_F(Jemalloc_shm_pool_collection_test, Multithread)
{
  using Thread_cache_holder = Test_shm_pool_collection::Thread_cache_holder;

  shared_ptr<Test_shm_pool_collection> collection = create_collection();
  EXPECT_TRUE(collection->start(S_ARENAS));
  Arena_id arena_id = *collection->get_arena_ids().begin();
  // Exhibit that we have the same thread cache when requesting twice
  shared_ptr<Thread_cache_holder> thread_cache = collection->get_or_create_thread_cache_external(arena_id);
  EXPECT_EQ(*collection->get_or_create_thread_cache_external(arena_id), *thread_cache);

  // Ensure that any thread cache is destroyed automatically when a thread dies
  EXPECT_TRUE(check_output([&]()
                           {
                             // Exhibit that we create a new thread cache when requesting from a different thread
                             std::thread t([&]()
                                           {
                                             shared_ptr<Thread_cache_holder> thread_cache_2 = 
                                               collection->get_or_create_thread_cache_external(arena_id);
                                             EXPECT_NE(*thread_cache_2, *thread_cache);
                                             EXPECT_EQ(*collection->get_or_create_thread_cache_external(arena_id),
                                                       *thread_cache_2);
                                           });
                             t.join();
                           },
                           cout,
                           "Destroyed thread cache id "));

  // Clean up
  collection->remove_current_thread_cache();
}

/// Ensure that a different process can read the data.
TEST_F(Jemalloc_shm_pool_collection_test, Multiprocess)
{
  assert(S_ALLOCATION_SIZE >= get_arbitrary_data().size());
  shared_ptr<Test_shm_pool_collection> collection = create_collection();
  EXPECT_TRUE(collection->start(S_ARENAS));
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
                                               shm_pool1->get_name(),
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
                                                      shm_pool2->get_name(),
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
TEST_F(Jemalloc_shm_pool_collection_test, Multithread_load)
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
  // Size of the large memory objects
  const size_t LARGE_OBJECT_SIZE = Jemalloc_pages::get_page_size() ^ 2;
  // Number of (randomized) large allocation/deallocation operations to perform
  constexpr size_t NUM_LARGE_OBJECT_OPERATIONS = 100;

  Test_logger logger(flow::log::Sev::S_INFO);
  shared_ptr<Memory_manager> memory_manager(make_shared<Memory_manager>(&logger));
  auto collection = Test_shm_pool_collection::create(&logger, memory_manager);
  EXPECT_TRUE(collection->start());

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

  using Thread_cache_id = Test_shm_pool_collection::Thread_cache_id;

  /**
   * Creates an object in a particular arena.
   *
   * @param object_size The size of the object.
   * @param arena_id The arena where the allocation will take place.
   * @param thread_cache_id If non-null, the thread cache id to use; otherwise, no thread cache will be used.
   *
   * @return The allocated object.
   */
  auto allocate_functor =
    [&](size_t object_size, Arena_id arena_id, Thread_cache_id* thread_cache_id) -> void*
    {
      void* p;
      if (thread_cache_id != nullptr)
      {
        p = collection->allocate(object_size, arena_id, *thread_cache_id);
      }
      else
      {
        p = collection->allocate(object_size, arena_id);
      }

      return p;
    };

  /**
   * Deallocates an object that was created in the specified arena.
   *
   * @param object The object to deallocated.
   * @param arena_id The arena where the allocation took place and will be deallocated from.
   * @param thread_cache_id If non-null, the thread cache id that was used; otherwise, no thread cache will be used.
   */
  auto deallocate_functor =
    [&](void* object, Arena_id arena_id, Thread_cache_id* thread_cache_id)
    {
      if (thread_cache_id != nullptr)
      {
        collection->deallocate(object, arena_id, *thread_cache_id);
      }
      else
      {
        collection->deallocate(object, arena_id);
      }
    };

  /**
   * Executes a test by performing the following:
   * 1. Allocating an initial pool of objects
   * 2. Randomly allocating or deallocating objects
   * 3. Cleaning up all objects
   * 4. Repeating above steps using a thread cache
   *
   * @param num_initial_objects The number of objects to be allocated initially.
   * @param object_size The size of the objects to allocated. If 0, a size will randomly be chosen in the range
   *                    of [1, LARGE_OBJECT_SIZE].
   * @param arena_id The arena where allocation/deallocations will take place.
   * @param thread_cache_id The thread cache to use when the test uses the thread cache.
   * @param num_operations The number of random allocation/deallocations to perform.
   */
  auto test_functor =
    [&](size_t num_initial_objects,
        size_t object_size,
        Arena_id arena_id,
        Thread_cache_id thread_cache_id,
        size_t num_operations,
        std::default_random_engine& random_generator)
    {
      Allocation_tracker tracker;
      // Random size allocation
      std::uniform_int_distribution<size_t> size_distribution(1, LARGE_OBJECT_SIZE);

      // First, use no thread cache and then use a thread cache
      for (size_t use_cache = 0; use_cache < 2; ++use_cache)
      {
        Thread_cache_id* tcache_id = ((use_cache > 0) ? &thread_cache_id : nullptr);

        // Allocate the initial pool of objects and track them
        for (size_t i = 0; i < num_initial_objects; ++i)
        {
          size_t size = ((object_size == 0) ? size_distribution(random_generator) : object_size);
          tracker.append(allocate_functor(size, arena_id, tcache_id));
        }

        // Randomly decide whether to allocate or deallocate an object
        std::uniform_int_distribution<size_t> coin(0, 1);
        for (size_t i = 0; i < num_operations; ++i)
        {
          if (coin(random_generator) != 0)
          {
            // Allocate
            size_t size = ((object_size == 0) ? size_distribution(random_generator) : object_size);
            tracker.append(allocate_functor(size, arena_id, tcache_id));
          }
          else
          {
            // Deallocate an object from the list randomly
            void* cur_object = tracker.pop_random(random_generator);
            if (cur_object != nullptr)
            {
              deallocate_functor(cur_object, arena_id, tcache_id);
            }
          }
        }

        // Deallocate all the objects from the list in a random order
        void* cur_object;
        while ((cur_object = tracker.pop_random(random_generator)) != nullptr)
        {
          deallocate_functor(cur_object, arena_id, tcache_id);
        }
      }
    };

  Arena_id arena_id;
  if (collection->get_default_arena_id(arena_id))
  {
    vector<unique_ptr<std::thread>> threads;
    for (size_t i = 0; i < NUM_THREADS; ++i)
    {
      threads.emplace_back(
        make_unique<std::thread>(
          [&]()
          {
            std::default_random_engine random_generator;
            using Thread_cache_holder = Test_shm_pool_collection::Thread_cache_holder;
            shared_ptr<Thread_cache_holder> thread_cache = collection->get_or_create_thread_cache_external(arena_id);
            Thread_cache_id tcache_id = thread_cache->get_thread_cache_id();

            // Run test on small object sizes with and without a thread cache
            test_functor(NUM_INITIAL_OBJECTS, OBJECT_SIZE, arena_id, tcache_id, NUM_OPERATIONS, random_generator);
            // Run test of large object sizes with and without a thread cache
            test_functor(NUM_INITIAL_LARGE_OBJECTS,
                         LARGE_OBJECT_SIZE,
                         arena_id,
                         tcache_id,
                         NUM_LARGE_OBJECT_OPERATIONS,
                         random_generator);
            // Run test on random object sizes with and without a thread cache
            test_functor(NUM_INITIAL_OBJECTS, 0, arena_id, tcache_id, NUM_OPERATIONS, random_generator);
          }));
    }

    for (const auto& cur_thread : threads)
    {
      cur_thread->join();
    }
  }
  else
  {
    ADD_FAILURE() << "No default arena id";
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
TEST_F(Jemalloc_shm_pool_collection_test, Owner_shm_pool_removal)
{
  Test_logger logger;
  auto memory_manager(make_shared<Memory_manager>(&logger));
  auto owner_collection = Test_shm_pool_collection::create(&logger, memory_manager);
  EXPECT_TRUE(owner_collection->start());
  auto borrower_collection =
    make_shared<Borrower_shm_pool_collection>(&logger,
                                              arena_lend::test::Test_shm_pool_collection::S_DEFAULT_COLLECTION_ID);

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
      auto borrower_pool
        = borrower_collection->open_shm_pool(owner_pool->get_id(), owner_pool->get_name(), owner_pool->get_size());
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
