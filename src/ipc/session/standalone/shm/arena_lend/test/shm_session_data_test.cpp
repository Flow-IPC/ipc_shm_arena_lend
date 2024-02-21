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
#include "ipc/session/standalone/shm/arena_lend/shm_session_data.hpp"
#include "ipc/session/standalone/shm/arena_lend/test/test_shm_session_util.hpp"
#include "ipc/shm/arena_lend/test/test_shm_object.hpp"
#include "ipc/shm/arena_lend/test/test_shm_pool_collection.hpp"
#include "ipc/test/test_fake_object.hpp"
#include "ipc/test/test_logger.hpp"
#include <atomic>
#include <list>
#include <optional>
#include <random>
#include <unordered_set>
#include <unordered_map>

using namespace ipc::test;
using namespace ipc::shm::arena_lend::test;
using std::atomic;
using std::list;
using std::max;
using std::optional;
using std::shared_ptr;
using std::size_t;
using std::set;
using std::unique_ptr;
using std::unordered_map;
using std::unordered_set;
using std::vector;
using std::string;

using ipc::shm::arena_lend::Borrower_shm_pool_collection;
using ipc::shm::arena_lend::Shm_pool;
using ipc::shm::arena_lend::test::ensure_empty_collection_at_destruction;
using ipc::shm::arena_lend::test::remove_test_shm_objects_filesystem;
using ipc::shm::arena_lend::test::Test_shm_pool_collection;

namespace ipc::session::shm::arena_lend::test
{

namespace
{

using pool_id_t = Shm_pool::pool_id_t;
using pool_offset_t = Shm_pool::size_t;
using Mutex = std::mutex;
using Lock = std::lock_guard<Mutex>;

// The size of the shared memory pools.
static const size_t S_SHM_POOL_SIZE = 4096;

/* This is quick-and-somewhat-dirty; really we should also handle other APIs; probably discard() and possibly seed();
 * but this is sufficient for our needs at this time; and this is test code. */
class Random_engine_mt : public std::default_random_engine
{
public:
  using Base = std::default_random_engine;
  using Base::Base;

  result_type operator()()
  {
    Lock lock(m_mutex);
    return Base::operator()();
  }

private:
  Mutex m_mutex;
};

/**
 * Creates, tracks and removes objects within a collection.
 *
 * @tparam T The shared memory collection type.
 */
template <typename T>
class Collection :
  public flow::log::Log_context
{
public:
  /**
   * Constructor.
   *
   * @param logger Used for logging purposes.
   * @param shm_pool_collection The shared memory pool collection where the objects reside.
   * @param random_generator Generates random numbers for deciding which objects and pools to remove and select.
   */
  Collection(flow::log::Logger* logger,
             const shared_ptr<T>& shm_pool_collection,
             Random_engine_mt& random_generator) :
    flow::log::Log_context(logger, Log_component::S_SESSION),
    m_shm_pool_collection(shm_pool_collection),
    m_random_generator(random_generator)
  {
  }

  /**
   * Returns the shared memory pool collection.
   *
   * @return See above.
   */
  const shared_ptr<T>& get_shm_pool_collection() const
  {
    return m_shm_pool_collection;
  }

  /**
   * Returns a random registered shared memory pool, if one exists.
   *
   * @return If a shared memory pool is registered, a random shared memory pool; otherwise, an empty shared pointer.
   */
  shared_ptr<Shm_pool> get_random_shm_pool()
  {
    Lock lock(m_mutex);
    return get_random_shm_pool_lock();
  }

  /**
   * Generates a fake object in a random registered shared memory pool, if one exists.
   *
   * @param shm_pool If the result is not an empty shared pointer, the shared memory pool where the object is
   *                 registered.
   *
   * @return If a shared memory pool is registered and the number of objects created does not surpass the size
   *         of the pool, the generated object; otherwise, an empty shared pointer.
   */
  shared_ptr<void> create_object_in_random_pool(shared_ptr<Shm_pool>& shm_pool)
  {
    Lock lock(m_mutex);

    shm_pool = get_random_shm_pool_lock();
    if (shm_pool == nullptr)
    {
      FLOW_LOG_TRACE("No SHM pool to create object in");
      return nullptr;
    }

    return create_object_lock(shm_pool);
  }

  /**
   * Generates a fake object in a shared memory pool.
   *
   * @param shm_pool The shared memory pool to create the object in.
   *
   * @return If a shared memory pool is registered and the number of objects created does not surpass the size
   *         of the pool, the generated object; otherwise, an empty shared pointer.
   */
  shared_ptr<void> create_object(const shared_ptr<Shm_pool>& shm_pool)
  {
    Lock lock(m_mutex);
    return create_object_lock(shm_pool);
  }

  /**
   * Registers an object into a shared memory pool. It may fail for the following reasons:
   * 1. The object does not reside in the specified shared memory pool.
   * 2. The shared memory pool was not previously registered and the offset of the object is not at the base, which
   *    is expected due to the offset algorithm we use.
   * 3. The object was already previously registered.
   *
   * The object should be in the unregistered object list for the shared memory pool unless this is for a
   * newly created shared memory pool.
   *
   * @param shm_pool The shared memory pool the object will be registered in. If this is a previously unregistered
   *                 shared memory pool, the shared memory pool will also be registered.
   * @param object The object to register.
   *
   * @return Whether the object was registered.
   */
  bool register_object(const shared_ptr<Shm_pool>& shm_pool, const shared_ptr<void>& object)
  {
    Lock lock(m_mutex);

    pool_offset_t offset;
    if (!shm_pool->determine_offset(object.get(), offset))
    {
      ADD_FAILURE() << "Object [" << object.get() << "] is not within SHM pool [" << *shm_pool << "]";
      return false;
    }

    auto iter = m_object_map.find(shm_pool);
    if (iter == m_object_map.end())
    {
      if (offset != 0)
      {
        ADD_FAILURE() << "SHM pool id [" << shm_pool->get_id() << "] was removed after object [" <<
          object.get() << "] creation as object offset is [" << offset << "]";
        return false;
      }

      // Insert SHM pool with an empty list
      auto result_pair = m_object_map.emplace(shm_pool, Object_lists());
      if (!result_pair.second)
      {
        ADD_FAILURE() << "Could not emplace object into SHM pool; testing error";
        return false;
      }

      iter = result_pair.first;
      // Initialize counter
      iter->second.m_counter = 1;
    }
    else
    {
      // Remove from unregistered object list
      EXPECT_GT(iter->second.m_unregistered_objects.erase(object), 0UL);
    }

    // Register object
    auto result_pair = iter->second.m_registered_objects.emplace(object);
    if (!result_pair.second)
    {
      ADD_FAILURE() << "The object [" << object.get() << "] was already registered in SHM pool id [" <<
        shm_pool->get_id() << "]";
      return false;
    }
    else
    {
      FLOW_LOG_TRACE("Registered object [" << object.get() << "] in SHM pool [" << shm_pool->get_id() << ", " <<
                     shm_pool->get_name() << "]");
    }

    return true;
  }

  /**
   * Deregisters a random registered object from a random registered pool. If this leaves the pool empty, the pool
   * will also be deregistered.
   *
   * @param shm_pool If the result is not an empty shared pointer, the shared memory pool the object resides in.
   * @param removed_shm_pool If the result is not an empty shared pointer, whether the shared memory pool was
   *                         deregistered as well.
   *
   * @return If successful, the deregistered object; otherwise, an empty shared pointer.
   */
  virtual shared_ptr<void> deregister_random_object_in_random_pool(shared_ptr<Shm_pool>& shm_pool,
                                                                   bool& removed_shm_pool)
  {
    Lock lock(m_mutex);

    shm_pool = get_random_shm_pool_lock();
    if (shm_pool == nullptr)
    {
      FLOW_LOG_TRACE("No SHM pool to remove object from");
      return nullptr;
    }

    return deregister_random_object_helper(shm_pool, removed_shm_pool);
  }

  /**
   * Deregisters a random registered object from a specified registered pool. If this leaves the pool empty, the pool
   * will also be deregistered.
   *
   * @param shm_pool The shared memory pool the object resides in.
   * @param removed_shm_pool If the result is not an empty shared pointer, whether the shared memory pool was
   *                         deregistered as well.
   *
   * @return If successful, the deregistered object; otherwise, an empty shared pointer.
   */
  virtual shared_ptr<void> deregister_random_object(const shared_ptr<Shm_pool>& shm_pool, bool& removed_shm_pool)
  {
    Lock lock(m_mutex);
    return deregister_random_object_helper(shm_pool, removed_shm_pool);
  }

  virtual ~Collection() = default;

private:
  /**
   * Object tracking container per shared memory pool.
   */
  struct Object_lists
  {
    /// Registered objects within the pool.
    set<shared_ptr<void>> m_registered_objects;
    /// Created but yet to be registered objects within the pool.
    set<shared_ptr<void>> m_unregistered_objects;
    /// Object counter, which is also used as the offset from the base of the pool for newly created objects.
    size_t m_counter = 0;
  }; // struct Object_lists

  /**
   * Returns a random shared memory pool.
   *
   * @return If there is a registered shared memory pool, a shared memory pool; otherwise, an empty shared pointer.
   */
  shared_ptr<Shm_pool> get_random_shm_pool_lock()
  {
    if (m_object_map.empty())
    {
      return nullptr;
    }

    // Generate random number to use as an index
    std::uniform_int_distribution<size_t> object_map_dist(0, (m_object_map.size() - 1));
    size_t object_map_index = object_map_dist(m_random_generator);

    // Locate index by linear traversal
    for (const auto& object_map_pair : m_object_map)
    {
      if (object_map_index == 0)
      {
        return object_map_pair.first;
      }
      --object_map_index;
    }

    ADD_FAILURE() << "Shouldn't have gotten here";
    return nullptr;
  }

  /**
   * Generates a fake object in a shared memory pool. A lock on the mutex must be obtained before calling this.
   *
   * @param shm_pool The shared memory pool to create the object in.
   *
   * @return If a shared memory pool is registered and the number of objects created does not surpass the size
   *         of the pool, the generated object; otherwise, an empty shared pointer.
   */
  shared_ptr<void> create_object_lock(const shared_ptr<Shm_pool>& shm_pool)
  {
    pool_offset_t pool_offset;
    auto iter = m_object_map.find(shm_pool);
    if (iter != m_object_map.end())
    {
      pool_offset = iter->second.m_counter++;
    }
    else
    {
      pool_offset = 0;
    }

    if (pool_offset >= shm_pool->get_size())
    {
      ADD_FAILURE() << "Ran out of object offsets as determined offset is [" << pool_offset <<
        "], SHM pool size [" << shm_pool->get_size() << "]";
      return nullptr;
    }

    // Create fake object
    shared_ptr<uint8_t> object(
      create_fake_shared_object(static_cast<uint8_t*>(shm_pool->get_address()) + pool_offset));
    if (iter != m_object_map.end())
    {
      // Track object in unregistered list to avoid potential premature SHM pool removal
      auto result_pair = iter->second.m_unregistered_objects.emplace(object);
      if (!result_pair.second)
      {
        ADD_FAILURE() << "Could not add object [" << shared_ptr<void>(object) << "] into unregistered list";
        return nullptr;
      }
      FLOW_LOG_TRACE("Added unregistered object [" << shared_ptr<void>(object) << "] to SHM pool [" <<
                     shm_pool->get_id() << ", " << shm_pool->get_name() << "]");
    }

    return object;
  }

  /**
   * Deregisters a random object from a specified shared memory pool. If the removal of this object results in
   * an empty shared memory pool, the shared memory pool will be deregistered as well. This may fail for the
   * following reasons:
   * 1. The shared memory pool is not registered.
   * 2. There are no objects in the shared memory pool.
   *
   * @param shm_pool The shared memory pool to deregister the object from.
   * @param removed_shm_pool Whether the shared memory pool was deregistered due to it being empty.
   *
   * @return If successful, the deregistered object; otherwise, an empty shared pointer.
   */
  shared_ptr<void> deregister_random_object_helper(const shared_ptr<Shm_pool>& shm_pool, bool& removed_shm_pool)
  {
    removed_shm_pool = false;

    auto object_map_iter = m_object_map.find(shm_pool);
    if (object_map_iter == m_object_map.end())
    {
      ADD_FAILURE() << "SHM pool not registered when removing random object";
      return nullptr;
    }

    // Randomly select object in SHM pool
    Object_lists& object_lists = object_map_iter->second;
    auto& object_list = object_lists.m_registered_objects;
    if (object_list.empty())
    {
      // Empty registered object list means we just newly created this with an unregistered object
      EXPECT_FALSE(object_lists.m_unregistered_objects.empty());
      FLOW_LOG_TRACE("Registered object list is empty, so there's no object to remove");
      return nullptr;
    }

    std::uniform_int_distribution<size_t> object_list_dist(0, (object_list.size() - 1));
    size_t object_list_index = object_list_dist(m_random_generator);

    for (auto object_list_iter = object_list.begin(); object_list_iter != object_list.end(); ++object_list_iter)
    {
      if (object_list_index == 0)
      {
        shared_ptr<void> object = *object_list_iter;
        object_list.erase(object_list_iter);

        // When there are no more objects in a SHM pool, we'll remove it
        if (object_list.empty() && object_lists.m_unregistered_objects.empty())
        {
          FLOW_LOG_TRACE("Removed object [" << object << "]" << " and now empty SHM pool [" <<
                         shm_pool->get_id() << ", " << shm_pool->get_name() << "]");
          m_object_map.erase(object_map_iter);
          removed_shm_pool = true;
        }
        else
        {
          FLOW_LOG_TRACE("Removed object [" << object << "] from SHM pool [" << shm_pool->get_id() << ", " <<
                         shm_pool->get_name() << "], objects remaining [" << object_list.size() << "]");
        }

        return object;
      }
      --object_list_index;
    }

    ADD_FAILURE() << "Shouldn't get here";
    return nullptr;
  }

  /// The shared memory pool collection where the shared memory pools and objects within them reside.
  const shared_ptr<T> m_shm_pool_collection;
  /// Synchronizes access to m_object_map and m_random_generator.
  mutable Mutex m_mutex;
  /// Generates random numbers for deciding which objects and pools to remove and select.
  Random_engine_mt& m_random_generator;
  /// Maps shared memory pool to an Object_list structure.
  std::unordered_map<shared_ptr<Shm_pool>, Object_lists> m_object_map;
}; // class Collection

/**
 * Tracks shared memory pool collections.
 *
 * @tparam Shm_pool_collection_type The shared memory pool collection type, which is a derivative of the
 *                                  #ipc::shm::arena_lend::Shm_pool_collection class.
 * @tparam Collection_type The collection type, which is a derivative of the #Collection class.
 */
template <typename Shm_pool_collection_type, typename Collection_type>
class Collection_tracker
{
public:
  /**
   * Constructor.
   *
   * @param logger Used for logging purposes.
   * @param num_collections The quantity of shared memory pool collections to be created and used. This cannot
   *                        be modified during the tracker lifetime.
   * @param random_generator Generates random numbers for deciding which objects, pools and collections to
   *                         remove and select.
   */
  Collection_tracker(flow::log::Logger* logger,
                     size_t num_collections,
                     Random_engine_mt& random_generator) :
    m_random_generator(random_generator)
  {
    assert(num_collections > 0);

    for (size_t i = 0; i < num_collections; ++i)
    {
      auto cur_collection = std::make_shared<Shm_pool_collection_type>(logger, i);
      EXPECT_TRUE(m_collection_map.emplace(cur_collection->get_id(),
                                           std::make_unique<Collection_type>(logger,
                                                                             cur_collection,
                                                                             m_random_generator)).second);
    }
  }

  /**
   * Returns the set of registered shared memory pool collections.
   *
   * @return See above.
   */
  set<shared_ptr<Shm_pool_collection_type>> get_collections() const
  {
    set<shared_ptr<Shm_pool_collection_type>> collections;
    for (const auto& cur_collection_map_pair : m_collection_map)
    {
      EXPECT_TRUE(collections.emplace(cur_collection_map_pair.second->get_shm_pool_collection()).second);
    }

    return collections;
  }

  /**
   * Returns a collection selected randomly.
   *
   * @return See above.
   */
  const shared_ptr<Shm_pool_collection_type>& get_random_collection() const
  {
    assert(!m_collection_map.empty());

    std::uniform_int_distribution<size_t> random_dist(0, (m_collection_map.size() - 1));
    size_t index = random_dist(m_random_generator);

    for (const auto& cur_collection_map_pair : m_collection_map)
    {
      if (index == 0)
      {
        return cur_collection_map_pair.second->get_shm_pool_collection();
      }
      --index;
    }

    assert(false && "Shouldn't get here");
    return m_collection_map.begin()->second->get_shm_pool_collection();
  }

  /**
   * Returns a random registered shared memory pool from a collection, if one exists.
   *
   * @param collection The shared memory pool collection to retrieve a registered shared memory pool from.
   *
   * @return If there is a shared memory pool registered within the specified collection, a shared memory pool;
   *         otherwise, an empty shared pointer.
   */
  shared_ptr<Shm_pool> get_random_shm_pool(const shared_ptr<Shm_pool_collection_type>& collection) const
  {
    auto iter = m_collection_map.find(collection->get_id());
    if (iter == m_collection_map.end())
    {
      ADD_FAILURE() << "Could not find collection [" << collection->get_id() << "]";
      return nullptr;
    }

    return iter->second->get_random_shm_pool();
  }

  /**
   * Creates an object within a registered random shared memory pool in the specified collection.
   * See Collection::create_object_in_random_pool() for description of failure scenarios.
   *
   * @param collection The shared memory pool collection to create an object in.
   * @param shm_pool If the result is a valid object, the shared memory pool containing the object.
   *
   * @return If successful, the created object; otherwise, an empty shared pointer.
   */
  shared_ptr<void> create_object_in_random_pool(const shared_ptr<Shm_pool_collection_type>& collection,
                                                shared_ptr<Shm_pool>& shm_pool)
  {
    auto iter = m_collection_map.find(collection->get_id());
    if (iter == m_collection_map.end())
    {
      ADD_FAILURE() << "Could not find collection [" << collection->get_id() << "]";
      return nullptr;
    }

    return iter->second->create_object_in_random_pool(shm_pool);
  }

  /**
   * Creates an object within the specified shared memory pool in the specified collection.
   * See Collection::create_object() for description of failure scenarios.
   *
   * @param collection The shared memory pool collection to create an object in.
   * @param shm_pool The shared memory pool to create an object in.
   *
   * @return If successful, the created object; otherwise, an empty shared pointer.
   */
  shared_ptr<void> create_object(const shared_ptr<Shm_pool_collection_type>& collection,
                                 const shared_ptr<Shm_pool>& shm_pool)
  {
    auto iter = m_collection_map.find(collection->get_id());
    if (iter == m_collection_map.end())
    {
      ADD_FAILURE() << "Could not find collection [" << collection->get_id() << "]";
      return nullptr;
    }

    return iter->second->create_object(shm_pool);
  }

  /**
   * Registers an object for tracking purposes.
   *
   * @param collection The collection containing the object.
   * @param shm_pool The shared memory pool containing the object.
   * @param object The object to register.
   *
   * @return Whether the object was successfully registered.
   */
  bool register_object(const shared_ptr<Shm_pool_collection_type>& collection,
                       const shared_ptr<Shm_pool>& shm_pool,
                       const shared_ptr<void>& object)
  {
    auto iter = m_collection_map.find(collection->get_id());
    if (iter == m_collection_map.end())
    {
      ADD_FAILURE() << "Could not find collection [" << collection->get_id() << "]";
      return false;
    }

    return iter->second->register_object(shm_pool, object);
  }

  /**
   * Deregisters a random registered object from a random registered pool in the specified shared memory pool
   * collection. If this leaves the pool empty, the pool will also be deregistered.
   *
   * @param collection The collection containing the object.
   * @param shm_pool If successful, this will be filled in with the shared memory pool containing the object.
   * @param removed_shm_pool If successful, this will be filled in with whether the shared memory pool was also
   *                         deregistered.
   *
   * @return If successful, the object that was deregistered; otherwise, an empty shared pointer.
   */
  shared_ptr<void> deregister_random_object_in_random_pool(const shared_ptr<Shm_pool_collection_type>& collection,
                                                           shared_ptr<Shm_pool>& shm_pool,
                                                           bool& removed_shm_pool)
  {
    auto iter = m_collection_map.find(collection->get_id());
    if (iter == m_collection_map.end())
    {
      ADD_FAILURE() << "Could not find collection [" << collection->get_id() << "]";
      return nullptr;
    }

    return iter->second->deregister_random_object_in_random_pool(shm_pool, removed_shm_pool);
  }

  /**
   * Deregisters a random registered object in a specified registered pool within the specified shared memory pool
   * collection. If this leaves the pool empty, the pool will also be deregistered.
   *
   * @param collection The collection containing the object.
   * @param shm_pool The shared memory pool containing the object.
   * @param removed_shm_pool If successful, this will be filled in with whether the shared memory pool was also
   *                         deregistered.
   *
   * @return If successful, the object deregistered; otherwise, an empty shared pointer.
   */
  shared_ptr<void> deregister_random_object(const shared_ptr<Shm_pool_collection_type>& collection,
                                            const shared_ptr<Shm_pool>& shm_pool,
                                            bool& removed_shm_pool)
  {
    auto iter = m_collection_map.find(collection->get_id());
    if (iter == m_collection_map.end())
    {
      ADD_FAILURE() << "Could not find collection [" << collection->get_id() << "]";
      return nullptr;
    }

    return iter->second->deregister_random_object(shm_pool, removed_shm_pool);
  }

protected:
  /// Generates random numbers for deciding which collections, objects and pools to remove and select.
  Random_engine_mt& m_random_generator;
  /// Maps collection id to a Collection.
  unordered_map<size_t, std::unique_ptr<Collection_type>> m_collection_map;
}; // class Collection_tracker

// Constants
/// The identifier for a shared memory pool that will not be registered.
const pool_id_t UNREGISTERED_SHM_POOL_ID = 5000;
/// The identifier for a shared memory pool that will not be registered.
const string UNREGISTERED_SHM_POOL_NAME = "name_unregistered";
/// The identifier for a shared memory pool collection that will not be registered.
const Collection_id UNREGISTERED_COLLECTION_ID = 80;
/// An offset for an object that will not be registered.
const pool_offset_t UNREGISTERED_OBJECT_OFFSET = 100;
/// A shared memory pool that will not be registered.
const shared_ptr<Shm_pool> UNREGISTERED_SHM_POOL =
  std::make_shared<Shm_pool>(UNREGISTERED_SHM_POOL_ID, UNREGISTERED_SHM_POOL_NAME, reinterpret_cast<void*>(0x4), 8, 1);

} // Anonymous namespace

/**
 * Object database related tests.
 * We perform the following:
 * 1. Register fake shared memory pools
 * 2. Register fake objects
 * 3. Deregister the fake objects
 * 4. Deregister the fake shared memory pools
 */
TEST(Shm_session_data_test, Borrower_object_database)
{
  /**
   * Container for the shared memory collection and configuration for the shared memory pools and objects.
   */
  class Borrower_collection_config
  {
  public:
    /**
     * Constructor.
     *
     * @param logger Used for logging purposes.
     * @param collection_id Identifier for the shared memory pool collection.
     * @param object_offsets Shared memory pool offsets for the objects to be registered.
     */
    Borrower_collection_config(flow::log::Logger* logger,
                               Collection_id collection_id,
                               vector<pool_offset_t>&& object_offsets) :
      m_collection(std::make_shared<Borrower_shm_pool_collection>(logger, collection_id)),
      m_owner_shm_pool_collection(logger),
      m_object_offsets(std::move(object_offsets))
    {
    }

    /**
     * Returns the borrower shared memory pool collection.
     *
     * @return See above.
     */
    const shared_ptr<Borrower_shm_pool_collection>& get_collection() const
    {
      return m_collection;
    }
    /**
     * Returns the borrower shared memory pool collection.
     *
     * @return See above.
     */
    shared_ptr<Borrower_shm_pool_collection>& get_collection()
    {
      return m_collection;
    }
    /**
     * Returns the generated shared memory pools.
     *
     * @return See above.
     */
    const vector<shared_ptr<Shm_pool>>& get_shm_pools() const
    {
      return m_shm_pools;
    }
    /**
     * Returns the offsets of the objects to be registered.
     *
     * @return See above.
     */
    const vector<pool_offset_t>& get_object_offsets() const
    {
      return m_object_offsets;
    }
    /**
     * Generates shared memory pools to be registered.
     *
     * @param quantity The number of shared memory pools to generate.
     *
     * @return Whether the requested shared memory pools were successfully generated.
     */
    bool generate_shm_pools(size_t quantity)
    {
      for (size_t i = 0; i < quantity; ++i)
      {
        shared_ptr<Shm_pool> shm_pool = m_owner_shm_pool_collection.create_shm_pool(S_SHM_POOL_SIZE);
        if (shm_pool == nullptr)
        {
          return false;
        }

        m_shm_pools.push_back(shm_pool);
      }

      return true;
    }
    /**
     * Removes the generated shared memory pools.
     *
     * @return Whether the shared memory pools were successfully removed.
     */
    bool remove_shm_pools()
    {
      for (auto iter = m_shm_pools.begin(); iter != m_shm_pools.end(); ++iter)
      {
        if (!m_owner_shm_pool_collection.remove_shm_pool(*iter))
        {
          m_shm_pools.erase(m_shm_pools.begin(), iter);
          return false;
        }
      }

      m_shm_pools.clear();
      return true;
    }

  private:
    /// The shared memory pool collection.
    shared_ptr<Borrower_shm_pool_collection> m_collection;
    /// The generator of shared memory objects, which basically simulates an owner.
    Test_shm_pool_collection m_owner_shm_pool_collection;
    /// The shared memory pools that are generated and not yet removed.
    vector<shared_ptr<Shm_pool>> m_shm_pools;
    /// Shared memory pool offsets for the objects involved.
    const vector<pool_offset_t> m_object_offsets;
  }; // struct Borrower_collection_config

  // The number of shared memory pools to create and register.
  const size_t NUM_SHM_POOLS = 3;

  Test_logger logger;
  Borrower_collection_config borrower_collection_configs[] =
    { Borrower_collection_config(&logger, 1, { 0, 16, 24, 40 }),
      Borrower_collection_config(&logger, 85, { 32, 40, 48 }) };

  // Create session in a different scope to get rid of references to collection during cleanup phase
  {
    Shm_session_data session_data(&logger);

    // Register borrower SHM pool collections
    for (auto& cur_config : borrower_collection_configs)
    {
      const shared_ptr<Borrower_shm_pool_collection>& cur_borrower_shm_pool_collection = cur_config.get_collection();
      Collection_id cur_collection_id = cur_borrower_shm_pool_collection->get_id();

      // Create shared memory objects
      if (!cur_config.generate_shm_pools(NUM_SHM_POOLS))
      {
        ADD_FAILURE() << "Could not generate [" << NUM_SHM_POOLS << "] SHM pools";
        remove_test_shm_objects_filesystem();
        return;
      }

      // Register collection
      EXPECT_TRUE(session_data.register_borrower_collection(cur_borrower_shm_pool_collection));

      // Register SHM pools
      for (const auto& cur_shm_pool : cur_config.get_shm_pools())
      {
        EXPECT_TRUE(session_data.register_borrower_shm_pool(cur_collection_id, cur_shm_pool));
        // The shared memory pool with the same id was just registered above
        EXPECT_FALSE(session_data.register_borrower_shm_pool(cur_collection_id, cur_shm_pool));

        // Register offsets
        for (auto cur_object_offset : cur_config.get_object_offsets())
        {
          EXPECT_TRUE(construct_and_register_borrower_object(
                        session_data, cur_collection_id, cur_shm_pool, cur_object_offset));
          // We are registering the same object as above
          EXPECT_TRUE(construct_and_register_borrower_object(
                        session_data, cur_collection_id, cur_shm_pool, cur_object_offset));
        }
      }

      // Register object at an unregistered SHM pool
      EXPECT_TRUE(
        construct_and_register_borrower_object(session_data, cur_collection_id, UNREGISTERED_SHM_POOL, 0, false));
    }

    {
      const auto& config = borrower_collection_configs[0];
      const auto& shm_pools = config.get_shm_pools();
      if (shm_pools.size() > 0)
      {
        const auto& shm_pool = shm_pools[0];
        const auto shm_pool_id = shm_pool->get_id();
        auto object_offset = config.get_object_offsets()[0];

        // Register SHM pool in an unregistered collection
        EXPECT_FALSE(session_data.register_borrower_shm_pool(UNREGISTERED_COLLECTION_ID, shm_pool));
        // Register object in an unregistered collection
        EXPECT_TRUE(construct_and_register_borrower_object(
                      session_data, UNREGISTERED_COLLECTION_ID, shm_pool, object_offset, false));
        // Deregister SHM pool from unregistered collection
        EXPECT_FALSE(session_data.deregister_borrower_shm_pool(UNREGISTERED_COLLECTION_ID, shm_pool_id));
        // Deregister object from unregistered collection
        EXPECT_FALSE(session_data.deregister_borrower_object(UNREGISTERED_COLLECTION_ID,
                                                             shm_pool_id,
                                                             object_offset));
      }
      else
      {
        ADD_FAILURE() << "No SHM pools created";
      }
    }

    // Deregister borrower SHM pool collections
    for (const auto& cur_config : borrower_collection_configs)
    {
      Collection_id cur_collection_id = cur_config.get_collection()->get_id();
      const vector<pool_offset_t>& object_offsets = cur_config.get_object_offsets();

      // Deregister a SHM pool that was never registered
      EXPECT_FALSE(session_data.deregister_borrower_shm_pool(cur_collection_id, UNREGISTERED_SHM_POOL_ID));
      // Deregister an object for a SHM pool that was never registered
      EXPECT_FALSE(
        session_data.deregister_borrower_object(cur_collection_id, UNREGISTERED_SHM_POOL_ID, object_offsets[0]));

      for (const auto& cur_shm_pool : cur_config.get_shm_pools())
      {
        const auto cur_shm_pool_id = cur_shm_pool->get_id();

        // Deregister an object that was never registered
        EXPECT_FALSE(session_data.deregister_borrower_object(cur_collection_id,
                                                             cur_shm_pool_id,
                                                             UNREGISTERED_OBJECT_OFFSET));

        // Deregister SHM pool before deregistering objects, which is not allowed
        EXPECT_FALSE(session_data.deregister_borrower_shm_pool(cur_collection_id, cur_shm_pool_id));

        // Deregister offsets
        for (const auto& cur_object_offset : cur_config.get_object_offsets())
        {
          EXPECT_TRUE(session_data.deregister_borrower_object(cur_collection_id, cur_shm_pool_id, cur_object_offset));
          // We registered twice, so we can deregister twice
          EXPECT_TRUE(session_data.deregister_borrower_object(cur_collection_id, cur_shm_pool_id, cur_object_offset));
          EXPECT_FALSE(session_data.deregister_borrower_object(cur_collection_id, cur_shm_pool_id, cur_object_offset));
        }

        // Deregister SHM pool after deregistering offsets
        EXPECT_TRUE(session_data.deregister_borrower_shm_pool(cur_collection_id, cur_shm_pool_id));
        EXPECT_FALSE(session_data.deregister_borrower_shm_pool(cur_collection_id, cur_shm_pool_id));
      }
    }
  }

  for (auto& cur_config : borrower_collection_configs)
  {
    // Remove SHM pools
    EXPECT_TRUE(cur_config.remove_shm_pools());
    EXPECT_TRUE(ensure_empty_collection_at_destruction(cur_config.get_collection()));
  }
}

/**
 * Object database related tests.
 * We perform the following:
 * 1. Register fake shared memory pools
 * 2. Register fake objects
 * 3. Deregister the fake objects
 * 4. Deregister the fake shared memory pools
 */
TEST(Shm_session_data_test, Lender_object_database)
{
  // Alias for the list of objects.
  using Object_list = unordered_set<shared_ptr<void>>;

  // The number of shared memory pools to be created.
  const size_t NUM_SHM_POOLS = 3;
  // An unregistered shared memory pool for negative testing.
  const shared_ptr<Shm_pool> UNREGISTERED_SHM_POOL =
    std::make_shared<Shm_pool>(UNREGISTERED_SHM_POOL_ID, UNREGISTERED_SHM_POOL_NAME,
                               reinterpret_cast<void*>(0x4), 1, 1);
  // An unregistered object not residing in any shared memory pool for negative testing.
  shared_ptr<uint8_t> DUMMY_OBJECT(create_fake_shared_object(static_cast<uint8_t*>(nullptr)));

  /**
   * Container for the shared memory collection, object tracking and configuration for the objects.
   */
  struct Lender_collection_config
  {
    /**
     * Constructor.
     *
     * @param logger Used for logging purposes.
     * @param collection_id Identifier for the shared memory pool collection.
     * @param object_offsets Shared memory pool offsets for the objects involved.
     */
    Lender_collection_config(flow::log::Logger* logger, Collection_id collection_id,
                             vector<pool_offset_t>&& object_offsets) :
      m_collection(std::make_shared<Test_shm_pool_collection>(logger, collection_id)),
      m_object_offsets(std::move(object_offsets))
    {
    }

    /**
     * The shared memory pool collection. It will be nullified to destroy it the end of the test, which is why
     * it isn't constant.
     */
    shared_ptr<Test_shm_pool_collection> m_collection;
    /// Shared memory pool offset for the objects involved.
    const vector<pool_offset_t> m_object_offsets;
    /// Maps shared memory pool to a list of objects.
    std::unordered_map<shared_ptr<Shm_pool>, Object_list> m_object_map;
  }; // struct Lender_collection_config

  Test_logger logger;
  Lender_collection_config lender_collection_configs[] = { Lender_collection_config(&logger, 1, { 0, 10, 20, 30 }),
                                                           Lender_collection_config(&logger, 85, { 30, 40, 50 }) };

  // Create session in different scope to get rid of references to collection during cleanup phase
  {
    Shm_session_data session_data(&logger);

    // Register lender SHM pool collections
    for (auto& cur_config : lender_collection_configs)
    {
      shared_ptr<Test_shm_pool_collection>& cur_shm_pool_collection = cur_config.m_collection;
      EXPECT_TRUE(session_data.register_lender_collection(cur_shm_pool_collection));

      Collection_id cur_collection_id = cur_shm_pool_collection->get_id();

      // Create SHM pools
      auto& cur_object_map = cur_config.m_object_map;
      for (size_t shm_pool_counter = 0; shm_pool_counter < NUM_SHM_POOLS; ++shm_pool_counter)
      {
        shared_ptr<Shm_pool> cur_shm_pool = cur_shm_pool_collection->create_shm_pool(S_SHM_POOL_SIZE);
        if (cur_shm_pool == nullptr)
        {
          ADD_FAILURE() << "Could not create SHM pool, so aborting test";
          remove_test_shm_objects_filesystem();
          return;
        }
        auto cur_result_pair = cur_object_map.emplace(cur_shm_pool, Object_list());
        EXPECT_TRUE(cur_result_pair.second);
        auto& cur_object_list = cur_result_pair.first->second;

        // Create fake objects at specified offsets in the SHM pool
        for (auto cur_object_offset : cur_config.m_object_offsets)
        {
          auto result = cur_object_list.emplace(
            create_fake_shared_object(static_cast<uint8_t*>(cur_shm_pool->get_address()) + cur_object_offset));
          EXPECT_TRUE(result.second);
        }
      }

      // Register SHM pools and objects
      for (const auto& cur_object_map_pair : cur_object_map)
      {
        const shared_ptr<Shm_pool>& cur_shm_pool = cur_object_map_pair.first;
        auto cur_shm_pool_id = cur_shm_pool->get_id();
        const Object_list& cur_object_list = cur_object_map_pair.second;

        EXPECT_TRUE(session_data.register_lender_shm_pool(cur_collection_id, cur_shm_pool));
        EXPECT_FALSE(session_data.register_lender_shm_pool(cur_collection_id, cur_shm_pool));

        // Register objects
        {
          for (const auto& cur_object : cur_object_list)
          {
            // Compute object offset from the base of the pool
            pool_offset_t cur_object_offset = 0UL;
            EXPECT_TRUE(cur_shm_pool->determine_offset(cur_object.get(), cur_object_offset));

            decltype(cur_shm_pool_id) cur_registered_shm_pool_id;
            pool_offset_t cur_registered_pool_offset;
            EXPECT_TRUE(session_data.register_lender_object(cur_collection_id,
                                                            cur_object,
                                                            cur_registered_shm_pool_id,
                                                            cur_registered_pool_offset));
            // Check that the shared memory pool id matches expected
            EXPECT_EQ(cur_shm_pool_id, cur_registered_shm_pool_id);
            // Check that the object offset matches expected
            EXPECT_EQ(cur_object_offset, cur_registered_pool_offset);

            // Register the same object twice
            EXPECT_TRUE(session_data.register_lender_object(cur_collection_id,
                                                            cur_object,
                                                            cur_registered_shm_pool_id,
                                                            cur_registered_pool_offset));
            // Check that the shared memory pool id matches expected
            EXPECT_EQ(cur_shm_pool_id, cur_registered_shm_pool_id);
            // Check that the object offset matches expected
            EXPECT_EQ(cur_object_offset, cur_registered_pool_offset);
          }
        }

        // Register an object residing at a location not in a SHM pool
        {
          pool_id_t cur_registered_shm_pool_id;
          pool_offset_t cur_registered_pool_offset;
          shared_ptr<uint8_t> fake_object(create_fake_shared_object(static_cast<uint8_t*>(nullptr)));
          EXPECT_FALSE(session_data.register_lender_object(cur_collection_id,
                                                           fake_object,
                                                           cur_registered_shm_pool_id,
                                                           cur_registered_pool_offset));
        }
      }
    }

    {
      const Lender_collection_config& config = lender_collection_configs[0];
      const shared_ptr<Shm_pool>& shm_pool = config.m_object_map.begin()->first;

      // Register SHM pool in an unregistered collection
      EXPECT_FALSE(session_data.register_lender_shm_pool(UNREGISTERED_COLLECTION_ID, shm_pool));
      // Register object in an unregistered collection
      pool_id_t cur_registered_shm_pool_id;
      pool_offset_t cur_registered_pool_offset;
      EXPECT_FALSE(session_data.register_lender_object(UNREGISTERED_COLLECTION_ID,
                                                       DUMMY_OBJECT,
                                                       cur_registered_shm_pool_id,
                                                       cur_registered_pool_offset));
      // Deregister SHM pool from unregistered collection
      bool was_empty;
      EXPECT_FALSE(session_data.deregister_lender_shm_pool(UNREGISTERED_COLLECTION_ID, shm_pool, was_empty));
      // Deregister object from unregistered collection
      EXPECT_EQ(session_data.deregister_lender_object(UNREGISTERED_COLLECTION_ID,
                                                      shm_pool->get_id(),
                                                      config.m_object_offsets[0]), nullptr);
    }

    // Deregister lender SHM pool collections
    for (auto& cur_config : lender_collection_configs)
    {
      Collection_id cur_collection_id = cur_config.m_collection->get_id();

      // Deregister a SHM pool that was never registered
      bool was_empty;
      EXPECT_FALSE(session_data.deregister_lender_shm_pool(cur_collection_id, UNREGISTERED_SHM_POOL, was_empty));
      // Deregister an object for a SHM pool that was never registered
      EXPECT_EQ(session_data.deregister_lender_object(cur_collection_id,
                                                      UNREGISTERED_SHM_POOL_ID,
                                                      cur_config.m_object_offsets[0]), nullptr);

      // Deregister SHM pool ids
      bool is_first_shm_pool = true;
      for (auto& cur_object_map_pair : cur_config.m_object_map)
      {
        const shared_ptr<Shm_pool>& cur_shm_pool = cur_object_map_pair.first;
        Object_list& cur_object_list = cur_object_map_pair.second;
        const auto cur_shm_pool_id = cur_shm_pool->get_id();

        // Deregister an object that was never registered
        EXPECT_EQ(
          session_data.deregister_lender_object(cur_collection_id, cur_shm_pool_id, UNREGISTERED_OBJECT_OFFSET),
          nullptr);

        if (is_first_shm_pool)
        {
          // Deregister SHM pool before deregistering offsets
          bool was_empty = true;
          EXPECT_TRUE(session_data.deregister_lender_shm_pool(cur_collection_id, cur_shm_pool, was_empty));
          EXPECT_FALSE(was_empty);
          EXPECT_FALSE(session_data.deregister_lender_shm_pool(cur_collection_id, cur_shm_pool, was_empty));
        }

        // Deregister objects
        for (auto cur_object_offset : cur_config.m_object_offsets)
        {
          // If it is the first SHM pool, we deregistered the SHM pool already
          if (!is_first_shm_pool)
          {
            auto cur_object =
              session_data.deregister_lender_object(cur_collection_id, cur_shm_pool_id, cur_object_offset);
            EXPECT_NE(cur_object, nullptr);
            // As we registered the object twice, we can deregister the object twice
            auto cur_object_2 =
              session_data.deregister_lender_object(cur_collection_id, cur_shm_pool_id, cur_object_offset);
            EXPECT_EQ(cur_object, cur_object_2);
            EXPECT_NE(cur_object_list.erase(cur_object), 0UL);
          }
          EXPECT_EQ(session_data.deregister_lender_object(cur_collection_id, cur_shm_pool_id, cur_object_offset),
                    nullptr);
        }

        if (!is_first_shm_pool)
        {
          // The list should be empty now
          EXPECT_EQ(cur_object_list.size(), 0UL);
          // Deregister SHM pool after deregistering objects
          bool was_empty = false;
          EXPECT_TRUE(session_data.deregister_lender_shm_pool(cur_collection_id, cur_shm_pool, was_empty));
          EXPECT_TRUE(was_empty);
          EXPECT_FALSE(session_data.deregister_lender_shm_pool(cur_collection_id, cur_shm_pool, was_empty));
        }
        else
        {
          is_first_shm_pool = false;
        }
      }
    }
  }

  // Remove all the SHM pools and nullify collections ensuring that they are empty
  for (auto& cur_config : lender_collection_configs)
  {
    shared_ptr<Test_shm_pool_collection>& cur_shm_pool_collection = cur_config.m_collection;
    const auto& cur_object_map = cur_config.m_object_map;
    for (const auto& cur_object_map_pair : cur_object_map)
    {
      EXPECT_TRUE(cur_shm_pool_collection->remove_shm_pool(cur_object_map_pair.first));
    }

    EXPECT_TRUE(ensure_empty_collection_at_destruction(cur_shm_pool_collection));
  }
}

/**
 * Uses the Shm_session_data class pertaining to the object database in a multithreaded manner to help ensure that
 * there isn't an issue with multithreaded execution. The test is performed in three phases:
 * 1. Create initial collections, shared memory pools and objects.
 *    This is to allow removal of objects at the start rather than only creation.
 * 2. Parallel execution of the following as either lender or borrower:
 * 2.1. Creation of shared memory pools in a random shared memory pool collection.
 * 2.2. Creation of objects in a random shared memory pool within a random shared memory pool collection.
 * 2.3. Removal of objects in a random shared memory pool within a random shared memory pool collection.
 * 3. Cleanup objects, shared memory pool and collections.
 *
 * Note that if too many shared memory pools are created, there may be a chance that the file descriptor
 * limit is hit, so the settings are capped to mitigate the probability of it occurring, while still having
 * sufficient entropy.
 */
TEST(Shm_session_data_test, Multithread_object_database)
{
  const size_t NUM_COLLECTIONS = 3;
  const size_t INITIAL_SHM_POOLS = 5;
  const size_t INITIAL_OBJECTS = 5;
  const size_t NUM_THREADS = 5;
  // The number of random actions each thread executes
  const size_t ITERATIONS = 100;

  using Mutex = std::mutex;
  using Lock = std::lock_guard<Mutex>;

  // We need to synchronize object removals as they may trigger a SHM pool removal that can't be out of order as
  // all objects are required to be removed beforehand.
  Mutex lender_object_deregistration_mutex;
  Mutex borrower_object_deregistration_mutex;

  /**
   * Creates, tracks and removes objects within a lender collection.
   */
  class Lender_collection final :
    public Collection<Test_shm_pool_collection>
  {
  public:
    /**
     * Constructor.
     *
     * @see Collection()
     */
    Lender_collection(flow::log::Logger* logger,
                      const shared_ptr<Test_shm_pool_collection>& shm_pool_collection,
                      Random_engine_mt& random_generator) :
      Collection<Test_shm_pool_collection>(logger, shm_pool_collection, random_generator)
    {
    }

    /**
     * Deregisters a random registered object in a specified registered pool within the specified shared memory pool
     * collection. If this leaves the pool empty, the pool will also be deregistered from tracking as well as
     * from the shared memory pool collection.
     *
     * @see Collection::deregister_random_object()
     */
    shared_ptr<void> deregister_random_object(const shared_ptr<Shm_pool>& shm_pool,
                                              bool& removed_shm_pool) override
    {
      shared_ptr<void> object = Collection::deregister_random_object(shm_pool, removed_shm_pool);
      deregister_random_object_helper(shm_pool, object, removed_shm_pool);
      return object;
    }
    /**
     * Deregisters a random registered object from a random registered pool. If this leaves the pool empty, the pool
     * will also be deregistered from this tracking as well as from the shared memory pool collection.
     *
     * @see Collection::deregister_random_object_in_random_pool()
     */
    shared_ptr<void> deregister_random_object_in_random_pool(shared_ptr<Shm_pool>& shm_pool,
                                                             bool& removed_shm_pool) override
    {
      shared_ptr<void> object = Collection::deregister_random_object_in_random_pool(shm_pool, removed_shm_pool);
      deregister_random_object_helper(shm_pool, object, removed_shm_pool);
      return object;
    }

  private:
    /**
     * This is executed after an attempt to deregister a random object from a shared memory pool. If the
     * execution was successful and it resulted in the deregistration of the shared memory pool containing the
     * object, then we will remove the shared memory pool from the shared memory pool collection as well.
     *
     * @param shm_pool The shared memory pool where the object resided or an empty shared pointer.
     * @param object If deregistration was previously successful, the object; otherwise, an empty shared pointer.
     * @param removed_shm_pool Whether deregistration resulted in the deregistration of the shared memory pool.
     */
    void deregister_random_object_helper(const shared_ptr<Shm_pool>& shm_pool,
                                         const shared_ptr<void>& object,
                                         bool removed_shm_pool)
    {
      if ((object != nullptr) && removed_shm_pool)
      {
        EXPECT_TRUE(get_shm_pool_collection()->remove_shm_pool(shm_pool));
      }
    }
  }; // class Lender_collection

  /**
   * Tracks lender collections and its shared memory pool and objects.
   */
  class Lender_collection_tracker :
    public Collection_tracker<Test_shm_pool_collection, Lender_collection>
  {
  public:
    /**
     * Constructor.
     *
     * @see Collection_tracker::Collection_tracker()
     */
    Lender_collection_tracker(flow::log::Logger* logger,
                              size_t num_collections,
                              Random_engine_mt& random_generator) :
      Collection_tracker<Test_shm_pool_collection, Lender_collection>(logger, num_collections, random_generator)
    {
    }
  }; // class Lender_collection_tracker

  /**
   * Tracks borrower collections and its shared memory pool and objects.
   */
  class Borrower_collection_tracker :
    public Collection_tracker<Borrower_shm_pool_collection, Collection<Borrower_shm_pool_collection>>
  {
  public:
    /**
     * Constructor.
     *
     * @see Collection_tracker::Collection_tracker()
     */
    Borrower_collection_tracker(flow::log::Logger* logger,
                                size_t num_collections,
                                Random_engine_mt& random_generator) :
      Collection_tracker<Borrower_shm_pool_collection, Collection<Borrower_shm_pool_collection>>(
        logger, num_collections, random_generator),
      m_shm_pool_collection(logger)
    {
    }

    /**
     * Generates a shared memory pool.
     *
     * @return If successful, the shared memory pool; otherwise, nullptr.
     */
    shared_ptr<Shm_pool> generate_shm_pool()
    {
      shared_ptr<Shm_pool> shm_pool = m_shm_pool_collection.create_shm_pool(S_SHM_POOL_SIZE);
      if (shm_pool == nullptr)
      {
        return nullptr;
      }

      {
        Lock lock(m_shm_pool_map_mutex);

        auto result_pair = m_shm_pool_map.emplace(shm_pool->get_id(), shm_pool);
        if (!result_pair.second)
        {
          ADD_FAILURE() << "Could not insert newly created SHM pool [" << *shm_pool << "] into tracking list";
          EXPECT_TRUE(m_shm_pool_collection.remove_shm_pool(shm_pool));
          return nullptr;
        }

        return shm_pool;
      }
    }

    /**
     * Removes a shared memory pool.
     *
     * @param shm_pool_id The identifier for the shared memory pool to remove.
     *
     * @return Whether the shared memory pool was removed successfully.
     */
    bool remove_shm_pool(Shm_pool::pool_id_t shm_pool_id)
    {
      shared_ptr<Shm_pool> shm_pool;

      {
        Lock lock(m_shm_pool_map_mutex);
        auto map_node = m_shm_pool_map.extract(shm_pool_id);
        if (map_node.empty())
        {
          return false;
        }

        shm_pool = std::move(map_node.mapped());
      }

      return m_shm_pool_collection.remove_shm_pool(shm_pool);
    }

    /**
     * Returns whether there are no more tracked shared memory pools.
     *
     * @return See above.
     */
    bool is_shm_pools_map_empty() const
    {
      return m_shm_pool_map.empty();
    }

  private:
    using Mutex = std::mutex;
    using Lock = std::lock_guard<Mutex>;

    /// The shared memory pool collection that generates the shared memory pools. This can be likened to an owner.
    Test_shm_pool_collection m_shm_pool_collection;
    /// Protects access to #m_shm_pool_map.
    Mutex m_shm_pool_map_mutex;
    /// Tracked shared memory pools. Maps shared memory pool id to the shared memory pool.
    unordered_map<pool_id_t, shared_ptr<Shm_pool>> m_shm_pool_map;
  }; // class Borrower_collection_tracker

  ////////// Initialization
  // In case of issues, change this log level to S_TRACE from S_INFO
  Test_logger logger(flow::log::Sev::S_INFO);
  Random_engine_mt random_generator;
  auto lender_tracker = std::make_unique<Lender_collection_tracker>(&logger, NUM_COLLECTIONS, random_generator);
  Borrower_collection_tracker borrower_tracker(&logger, NUM_COLLECTIONS, random_generator);
  auto session_data = std::make_unique<Shm_session_data>(&logger);

  // Initialize lender tracker
  std::set<shared_ptr<Test_shm_pool_collection>> lender_collections = lender_tracker->get_collections();
  for (const auto& cur_collection : lender_collections)
  {
    EXPECT_TRUE(session_data->register_lender_collection(cur_collection));
    Collection_id cur_collection_id = cur_collection->get_id();

    for (size_t shm_pool_counter = 0; shm_pool_counter < INITIAL_SHM_POOLS; ++shm_pool_counter)
    {
      shared_ptr<Shm_pool> cur_shm_pool = cur_collection->create_shm_pool(S_SHM_POOL_SIZE);

      EXPECT_TRUE(session_data->register_lender_shm_pool(cur_collection_id, cur_shm_pool));

      for (size_t object_counter = 0; object_counter < INITIAL_OBJECTS; ++object_counter)
      {
        shared_ptr<void> cur_object = lender_tracker->create_object(cur_collection, cur_shm_pool);
        EXPECT_TRUE(lender_tracker->register_object(cur_collection, cur_shm_pool, cur_object));

        pool_id_t cur_registered_shm_pool_id;
        pool_offset_t cur_registered_pool_offset;
        EXPECT_TRUE(session_data->register_lender_object(cur_collection_id,
                                                         cur_object,
                                                         cur_registered_shm_pool_id,
                                                         cur_registered_pool_offset));
        EXPECT_EQ(cur_registered_shm_pool_id, cur_shm_pool->get_id());
      }
    }
  }

  // Initialize borrower tracker
  std::set<shared_ptr<Borrower_shm_pool_collection>> borrower_collections = borrower_tracker.get_collections();
  for (const auto& cur_collection : borrower_collections)
  {
    EXPECT_TRUE(session_data->register_borrower_collection(cur_collection));
    Collection_id cur_collection_id = cur_collection->get_id();

    for (size_t shm_pool_counter = 0; shm_pool_counter < INITIAL_SHM_POOLS; ++shm_pool_counter)
    {
      shared_ptr<Shm_pool> cur_owner_shm_pool = borrower_tracker.generate_shm_pool();
      if (session_data->register_borrower_shm_pool(cur_collection_id, cur_owner_shm_pool))
      {
        for (size_t object_counter = 0; object_counter < INITIAL_OBJECTS; ++object_counter)
        {
          shared_ptr<void> cur_object = borrower_tracker.create_object(cur_collection, cur_owner_shm_pool);
          pool_offset_t cur_offset = 0;
          EXPECT_TRUE(cur_owner_shm_pool->determine_offset(cur_object.get(), cur_offset));
          EXPECT_TRUE(borrower_tracker.register_object(cur_collection, cur_owner_shm_pool, cur_object));
          EXPECT_TRUE(construct_and_register_borrower_object(
                        *session_data, cur_collection_id, cur_owner_shm_pool, cur_offset));
        }
      }
      else
      {
        ADD_FAILURE() << "Could not register SHM pool";
      }
    }
  }

  ////////// Execution
  vector<unique_ptr<std::thread>> threads;
  enum class Action
  {
    S_ACTION_CREATE_LENDER_SHM_POOL = 0,
    S_ACTION_CREATE_LENDER_OBJECT,
    S_ACTION_REMOVE_LENDER_OBJECT,
    S_ACTION_CREATE_BORROWER_SHM_POOL,
    S_ACTION_CREATE_BORROWER_OBJECT,
    S_ACTION_REMOVE_BORROWER_OBJECT,
    S_NUM_ACTIONS
  }; // enum class Action

  FLOW_LOG_SET_CONTEXT(&logger, Log_component::S_TEST);

  /**
   * Registers an object and tracks that we did so. We register the object in the session data first,
   * because our random actions operate on the tracker. If the tracker holds the object, another thread may operate
   * on the object, which may cause a race condition as it may not be registered in the session data yet.
   *
   * @param collection The collection where the object will be registered.
   * @param shm_pool The shared memory pool where the object is located.
   * @param object The object to register.
   */
  auto create_lender_object_functor_helper = [&](const shared_ptr<Test_shm_pool_collection>& collection,
                                                 const shared_ptr<Shm_pool>& shm_pool,
                                                 const shared_ptr<void>& object)
    {
      if (object == nullptr)
      {
        return;
      }

      pool_id_t registered_shm_pool_id;
      pool_offset_t registered_pool_offset;
      // Order matters here due to potential race condition
      EXPECT_TRUE(session_data->register_lender_object(collection->get_id(),
                                                       object,
                                                       registered_shm_pool_id,
                                                       registered_pool_offset));
      EXPECT_EQ(registered_shm_pool_id, shm_pool->get_id());
      EXPECT_TRUE(lender_tracker->register_object(collection, shm_pool, object));
    };

  /**
   * Creates an object in a random registered shared memory pool and registers it. The order of operations is
   * the following:
   * 1) Create object within a random SHM pool
   * 2) Register the object within the session data
   * 3) Track object in the object tracker
   *
   * @param collection The collection where the object will be created and registered.
   *
   * @see create_lender_object_functor_helper()
   */
  auto create_lender_object_in_random_pool_functor = [&](const shared_ptr<Test_shm_pool_collection>& collection)
    {
      shared_ptr<Shm_pool> shm_pool;
      shared_ptr<void> object = lender_tracker->create_object_in_random_pool(collection, shm_pool);
      create_lender_object_functor_helper(collection, shm_pool, object);
    };

  /**
   * Creates an object and registers it. The order of operations is the following:
   * 1) Create object within the SHM pool
   * 2) Register the object within the session data
   * 3) Track object in the object tracker
   *
   * @param collection The collection where the object will be created and registered.
   * @param shm_pool The shared memory pool where the object will be created and registered.
   *
   * @see create_lender_object_functor_helper()
   */
  auto create_lender_object_functor = [&](const shared_ptr<Test_shm_pool_collection>& collection,
                                          const shared_ptr<Shm_pool>& shm_pool)
    {
      shared_ptr<void> object = lender_tracker->create_object(collection, shm_pool);
      create_lender_object_functor_helper(collection, shm_pool, object);
    };

  /**
   * Deregisters an object residing within a specified registered shared memory pool in a specified collection.
   * If the object is the last one in the shared memory pool that we've tracked, deregister the shared memory
   * pool as well.
   *
   * @param collection The collection where the object will be deregistered from.
   * @param shm_pool The shared memory pool where the object resides.
   * @param object The object to deregister.
   * @param removed_shm_pool Whether the shared memory pool was also deregistered.
   *
   * @return Whether the object was successfully deregistered.
   */
  auto remove_lender_object_functor_helper = [&](const shared_ptr<Test_shm_pool_collection>& collection,
                                                 const shared_ptr<Shm_pool>& shm_pool,
                                                 const shared_ptr<void>& object,
                                                 bool removed_shm_pool) -> bool
    {
      Collection_id collection_id = collection->get_id();

      if (object == nullptr)
      {
        FLOW_LOG_TRACE("No object when attempting to remove from SHM pool");
        return false;
      }

      pool_offset_t object_offset;
      if (!shm_pool->determine_offset(object.get(), object_offset))
      {
        ADD_FAILURE() << "Object [" << object.get() << "] not found in SHM pool [" << *shm_pool << "]";
        return false;
      }

      FLOW_LOG_TRACE("Removing object [" << object.get() << "] offset [" << object_offset << "] from collection [" <<
                     collection_id << "], SHM pool id [" << shm_pool->get_id() << "]");
      auto removed_object = session_data->deregister_lender_object(collection_id, shm_pool->get_id(), object_offset);
      EXPECT_EQ(object, removed_object);

      if (removed_shm_pool)
      {
        FLOW_LOG_TRACE("Deregistering empty SHM pool [" << *shm_pool << "] in collection [" <<
                       collection_id << "]");
        bool was_empty = false;
        EXPECT_TRUE(session_data->deregister_lender_shm_pool(collection_id, shm_pool, was_empty));
        EXPECT_TRUE(was_empty);
      }

      return true;
    };

  /**
   * Deregisters a random registered object residing within a random registered shared memory pool in a specified
   * collection. If the object is the last one in the shared memory pool that we've tracked, deregister the shared
   * memory pool as well.
   *
   * @param collection The collection where the object will be deregistered from.
   *
   * @return Whether an object was successfully deregistered.
   *
   * @see remove_lender_object_functor_helper()
   */
  auto remove_lender_object_in_random_pool_functor =
    [&](const shared_ptr<Test_shm_pool_collection>& collection) -> bool
    {
      Lock lock(lender_object_deregistration_mutex);

      shared_ptr<Shm_pool> shm_pool;
      bool removed_shm_pool;
      shared_ptr<void> object = lender_tracker->deregister_random_object_in_random_pool(collection,
                                                                                        shm_pool,
                                                                                        removed_shm_pool);
      return remove_lender_object_functor_helper(collection, shm_pool, object, removed_shm_pool);
    };

  /**
   * Deregisters a random registered object residing within a specified registered shared memory pool in a specified
   * collection. If the object is the last one in the shared memory pool that we've tracked, deregister the shared
   * memory pool as well.
   *
   * @param collection The collection where the object will be deregistered from.
   * @param shm_pool The shared memory pool where the object is registered in.
   *
   * @return Whether an object was successfully deregistered.
   *
   * @see remove_lender_object_functor_helper()
   */
  auto remove_lender_object_functor = [&](const shared_ptr<Test_shm_pool_collection>& collection,
                                          const shared_ptr<Shm_pool>& shm_pool) -> bool
    {
      Lock lock(lender_object_deregistration_mutex);

      bool removed_shm_pool;
      shared_ptr<void> object = lender_tracker->deregister_random_object(collection, shm_pool, removed_shm_pool);
      return remove_lender_object_functor_helper(collection, shm_pool, object, removed_shm_pool);
    };

  /**
   * Registers an object and tracks that we did so. We register the object in the session data first,
   * because our random actions operate on the tracker. If the tracker holds the object, another thread may operate
   * on the object, which may cause a race condition as it may not be registered in the session data yet.
   *
   * @param collection The collection where the object will be registered.
   * @param shm_pool The shared memory pool where the object is located.
   * @param object The object to register.
   */
  auto create_borrower_object_functor_helper = [&](const shared_ptr<Borrower_shm_pool_collection>& collection,
                                                   const shared_ptr<Shm_pool>& shm_pool,
                                                   const shared_ptr<void>& object)
    {
      if (object == nullptr)
      {
        return;
      }

      pool_offset_t offset;
      if (!shm_pool->determine_offset(object.get(), offset))
      {
        ADD_FAILURE() << "Newly created object [" << object.get() << "] not within SHM pool [" << *shm_pool << "]";
      }

      // Order matters here due to potential race condition
      EXPECT_TRUE(construct_and_register_borrower_object(*session_data, collection->get_id(), shm_pool, offset));
      EXPECT_TRUE(borrower_tracker.register_object(collection, shm_pool, object));
    };

  /**
   * Creates an object in a random registered shared memory pool and registers it. The order of operations is
   * the following:
   * 1. Create object within a random SHM pool
   * 2. Register the object within the session data
   * 3. Track object in the object tracker
   *
   * @param collection The collection where the object will be created and registered.
   *
   * @see create_borrower_object_functor_helper()
   */
  auto create_borrower_object_in_random_pool_functor = [&](const shared_ptr<Borrower_shm_pool_collection>& collection)
    {
      shared_ptr<Shm_pool> shm_pool;
      shared_ptr<void> object = borrower_tracker.create_object_in_random_pool(collection, shm_pool);
      create_borrower_object_functor_helper(collection, shm_pool, object);
    };

  /**
   * Creates an object and registers it. The order of operations is the following:
   * 1. Create object within the SHM pool
   * 2. Register the object within the session data
   * 3. Track object in the object tracker
   *
   * @param collection The collection where the object will be created and registered.
   * @param shm_pool The shared memory pool where the object will be created and registered.
   *
   * @see create_borrower_object_functor_helper()
   */
  auto create_borrower_object_functor = [&](const shared_ptr<Borrower_shm_pool_collection>& collection,
                                            const shared_ptr<Shm_pool>& shm_pool)
    {
      shared_ptr<void> object = borrower_tracker.create_object(collection, shm_pool);
      create_borrower_object_functor_helper(collection, shm_pool, object);
    };

  /**
   * Deregisters an object residing within a specified registered shared memory pool in a specified collection.
   * If the object is the last one in the shared memory pool that we've tracked, deregister the shared memory
   * pool as well.
   *
   * @param collection The collection where the object will be deregistered from.
   * @param shm_pool The shared memory pool where the object resides.
   * @param object The object to deregister.
   * @param removed_shm_pool Whether deregistration resulted in the deregistration of the shared memory pool.
   *
   * @return Whether the object was successfully deregistered.
   */
  auto remove_borrower_object_functor_helper = [&](const shared_ptr<Borrower_shm_pool_collection>& collection,
                                                   const shared_ptr<Shm_pool>& shm_pool,
                                                   const shared_ptr<void>& object,
                                                   bool removed_shm_pool) -> bool
    {
      if (shm_pool == nullptr)
      {
        return false;
      }

      Collection_id collection_id = collection->get_id();

      pool_offset_t object_offset;
      if (!shm_pool->determine_offset(object.get(), object_offset))
      {
        ADD_FAILURE() << "Object [" << object.get() << "] not found in SHM pool [" << *shm_pool << "]";
        return false;
      }

      const auto shm_pool_id = shm_pool->get_id();
      FLOW_LOG_TRACE("Removing object [" << object.get() << "] offset [" << object_offset << "] from collection [" <<
                     collection_id << "], SHM pool id [" << shm_pool_id << "]");
      EXPECT_TRUE(session_data->deregister_borrower_object(collection_id, shm_pool_id, object_offset));

      if (removed_shm_pool)
      {
        FLOW_LOG_TRACE("Removing SHM pool [" << *shm_pool << "] created from borrower in collection [" <<
                       collection_id << "]");
        EXPECT_TRUE(session_data->deregister_borrower_shm_pool(collection_id, shm_pool_id));
        EXPECT_TRUE(borrower_tracker.remove_shm_pool(shm_pool_id));
      }

      return true;
    };

  /**
   * Deregisters a random registered object residing within a random registered shared memory pool in a specified
   * collection. If the object is the last one in the shared memory pool that we've tracked, deregister the shared
   * memory pool as well.
   *
   * @param collection The collection where the object will be deregistered from.
   *
   * @return Whether an object was successfully deregistered.
   *
   * @see remove_borrower_object_functor_helper()
   */
  auto remove_borrower_object_in_random_pool_functor =
    [&](const shared_ptr<Borrower_shm_pool_collection>& collection) -> bool
    {
      Lock lock(borrower_object_deregistration_mutex);

      shared_ptr<Shm_pool> shm_pool;
      bool removed_shm_pool;
      shared_ptr<void> object = borrower_tracker.deregister_random_object_in_random_pool(collection,
                                                                                         shm_pool,
                                                                                         removed_shm_pool);
      if (object == nullptr)
      {
        return false;
      }

      return remove_borrower_object_functor_helper(collection, shm_pool, object, removed_shm_pool);
    };

  /**
   * Deregisters a random registered object residing within a specified registered shared memory pool in a specified
   * collection. If the object is the last one in the shared memory pool that we've tracked, deregister the shared
   * memory pool as well.
   *
   * @param collection The collection where the object will be deregistered from.
   * @param shm_pool The shared memory pool where the object is registered in.
   *
   * @return Whether an object was successfully deregistered.
   *
   * @see remove_borrower_object_functor_helper()
   */
  auto remove_borrower_object_functor = [&](const shared_ptr<Borrower_shm_pool_collection>& collection,
                                            const shared_ptr<Shm_pool>& shm_pool) -> bool
    {
      Lock lock(borrower_object_deregistration_mutex);

      bool removed_shm_pool;
      shared_ptr<void> object = borrower_tracker.deregister_random_object(collection, shm_pool, removed_shm_pool);
      if (object == nullptr)
      {
        ADD_FAILURE() << "No object when attempting to remove from SHM pool id [" << shm_pool->get_id() << "]";
        return false;
      }

      return remove_borrower_object_functor_helper(collection, shm_pool, object, removed_shm_pool);
    };

  for (size_t i = 0; i < NUM_THREADS; ++i)
  {
    threads.emplace_back(
      std::make_unique<std::thread>(
        [&]()
        {
          std::default_random_engine random_generator;
          std::uniform_int_distribution<size_t> action_die(0, (static_cast<size_t>(Action::S_NUM_ACTIONS) - 1));
          for (size_t action_counter = 0; action_counter < ITERATIONS; ++action_counter)
          {
            Action cur_action = static_cast<Action>(action_die(random_generator));

            switch (cur_action)
            {
              case Action::S_ACTION_CREATE_LENDER_SHM_POOL:
              {
                FLOW_LOG_TRACE("Thread [" << i << "], Action [" << action_counter <<
                               "]: Creating lender SHM pool with object");

                const shared_ptr<Test_shm_pool_collection>& cur_collection = lender_tracker->get_random_collection();
                auto cur_collection_id = cur_collection->get_id();
                shared_ptr<Shm_pool> cur_shm_pool = cur_collection->create_shm_pool(S_SHM_POOL_SIZE);
                if (cur_shm_pool == nullptr)
                {
                  ADD_FAILURE() << "Could not create SHM pool in collection [" << cur_collection_id <<
                    "] due to potentially too many open files";
                  break;
                }

                EXPECT_TRUE(session_data->register_lender_shm_pool(cur_collection_id, cur_shm_pool));
                create_lender_object_functor(cur_collection, cur_shm_pool);
              }
              break;

              case Action::S_ACTION_CREATE_LENDER_OBJECT:
              {
                const shared_ptr<Test_shm_pool_collection>& cur_collection = lender_tracker->get_random_collection();
                FLOW_LOG_TRACE("Thread [" << i << "], Action [" << action_counter << "]: Creating lender object");
                create_lender_object_in_random_pool_functor(cur_collection);
              }
              break;

              case Action::S_ACTION_REMOVE_LENDER_OBJECT:
              {
                const shared_ptr<Test_shm_pool_collection>& cur_collection = lender_tracker->get_random_collection();
                if (!remove_lender_object_in_random_pool_functor(cur_collection))
                {
                  FLOW_LOG_TRACE("Thread [" << i << "], Action [" << action_counter << "]: Failed to remove "
                                 "lender object in collection [" << cur_collection->get_id() << "]");
                  break;
                }

                FLOW_LOG_TRACE("Thread [" << i << "], Action [" << action_counter << "]: Removed lender object");
              }
              break;

              case Action::S_ACTION_CREATE_BORROWER_SHM_POOL:
              {
                FLOW_LOG_TRACE("Thread [" << i << "], Action [" << action_counter <<
                               "]: Creating borrower SHM pool with object");

                // Create SHM pool
                const shared_ptr<Borrower_shm_pool_collection>& cur_collection =
                  borrower_tracker.get_random_collection();
                auto cur_collection_id = cur_collection->get_id();
                shared_ptr<Shm_pool> cur_owner_shm_pool = borrower_tracker.generate_shm_pool();
                if (cur_owner_shm_pool == nullptr)
                {
                  ADD_FAILURE() << "Could not create SHM pool in collection [" << cur_collection_id <<
                    "] due to potentially too many open files";
                  break;
                }

                // Register SHM pool
                const auto cur_shm_pool_id = cur_owner_shm_pool->get_id();
                if (!session_data->register_borrower_shm_pool(cur_collection_id, cur_owner_shm_pool))
                {
                  // This may be an error due to fd limit, but we can't distinguish right now
                  ADD_FAILURE() << "Failed to create borrower SHM pool in collection [" << cur_collection_id << "]";
                  EXPECT_TRUE(borrower_tracker.remove_shm_pool(cur_shm_pool_id));
                  break;
                }

                // Create and register object
                create_borrower_object_functor(cur_collection, cur_owner_shm_pool);
              }
              break;

              case Action::S_ACTION_CREATE_BORROWER_OBJECT:
              {
                const shared_ptr<Borrower_shm_pool_collection>& cur_collection =
                  borrower_tracker.get_random_collection();
                FLOW_LOG_TRACE("Thread [" << i << "], Action [" << action_counter << "]: Creating borrower object");
                create_borrower_object_in_random_pool_functor(cur_collection);
              }
              break;

              case Action::S_ACTION_REMOVE_BORROWER_OBJECT:
              {
                const shared_ptr<Borrower_shm_pool_collection>& cur_collection =
                  borrower_tracker.get_random_collection();
                if (!remove_borrower_object_in_random_pool_functor(cur_collection))
                {
                  FLOW_LOG_TRACE("Thread [" << i << "], Action [" << action_counter <<
                                 "]: Failed to remove borrower object in collection [" << cur_collection << "]");
                  break;
                }

                FLOW_LOG_TRACE("Thread [" << i << "], Action [" << action_counter << "]: Removed borrower object");
              }
              break;

              case Action::S_NUM_ACTIONS:
                assert(false);
                break;
            }
          }
        }));
  }

  // Wait for threads to complete
  for (const auto& cur_thread : threads)
  {
    cur_thread->join();
  }

  ////////// Clean up
  // Remove all the SHM pools
  for (const auto& cur_collection : lender_collections)
  {
    shared_ptr<Shm_pool> cur_shm_pool;
    while ((cur_shm_pool = lender_tracker->get_random_shm_pool(cur_collection)) != nullptr)
    {
      EXPECT_TRUE(remove_lender_object_functor(cur_collection, cur_shm_pool));
    }
  }

  for (const auto& cur_collection : borrower_collections)
  {
    shared_ptr<Shm_pool> cur_shm_pool;
    while ((cur_shm_pool = borrower_tracker.get_random_shm_pool(cur_collection)) != nullptr)
    {
      EXPECT_TRUE(remove_borrower_object_functor(cur_collection, cur_shm_pool));
    }
  }

  // Nullify lender collections ensuring that they are empty
  // Set logging level to TRACE to get appropriate log message when cleaning up collections
  logger.get_config().configure_default_verbosity(flow::log::Sev::S_TRACE, true);
  lender_tracker = nullptr;
  session_data = nullptr;
  while (!lender_collections.empty())
  {
    shared_ptr<Test_shm_pool_collection> cur_collection;
    {
      auto iter = lender_collections.begin();
      cur_collection = *iter;
      lender_collections.erase(iter);
    }
    EXPECT_TRUE(ensure_empty_collection_at_destruction(cur_collection));
  }

  // SHM pools created for the borrower should be all gone
  EXPECT_TRUE(borrower_tracker.is_shm_pools_map_empty());
}

} // namespace ipc::session::shm::arena_lend::test
