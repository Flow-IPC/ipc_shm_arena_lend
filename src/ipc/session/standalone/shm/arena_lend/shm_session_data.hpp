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

#pragma once

#include "ipc/session/standalone/shm/arena_lend/borrower_collection.hpp"
#include "ipc/session/standalone/shm/arena_lend/lender_collection.hpp"
#include "ipc/session/standalone/shm/arena_lend/arena_lend_fwd.hpp"
#include "ipc/shm/arena_lend/borrower_shm_pool_collection.hpp"
#include "ipc/shm/arena_lend/shm_pool.hpp"
#include <mutex>

namespace ipc::session::shm::arena_lend
{

/**
 * Container for shared memory information shared with another entity. This includes:
 * 1. Shared memory pool collections that are lent and borrowed
 * 2. Shared memory pools that are lent and borrowed
 * 3. Shared memory objects that are lent and borrowed
 *
 * It provides additional functionality of opening and closing borrowed shared memory pools when they are
 * registered.
 */
class Shm_session_data :
  public flow::log::Log_context
{
public:
  /// Convenience alias for a borrower shared memory pool collection.
  using Borrower_shm_pool_collection = ipc::shm::arena_lend::Borrower_shm_pool_collection;
  /// Convenience alias for a shared memory pool.
  using Shm_pool = ipc::shm::arena_lend::Shm_pool;
  /// Convenience alias for a shared memory pool collection.
  using Shm_pool_collection = ipc::shm::arena_lend::Shm_pool_collection;
  /// Short-hand for pool ID type.
  using pool_id_t = Shm_pool::pool_id_t;
  /// Short-hand for pool offset type.
  using pool_offset_t = Shm_pool::size_t;

  /**
   * Constructor.
   *
   * @param logger Used for logging purposes.
   */
  Shm_session_data(flow::log::Logger* logger);

  /**
   * Registers that a shared memory pool collection is lent to another entity.
   *
   * @param shm_pool_collection The shared memory pool collection to register.
   *
   * @return Whether the shared memory pool collection was registered successfully.
   */
  bool register_lender_collection(const std::shared_ptr<Shm_pool_collection>& shm_pool_collection);
  /**
   * Deregisters that a shared memory pool collection is lent to another entity.
   *
   * @param collection_id The identifier for the shared memory pool collection to deregister.
   *
   * @return Whether a shared memory pool collection was deregistered successfully.
   */
  bool deregister_lender_collection(Collection_id collection_id);
  /**
   * Registers that a shared memory pool is lent to another entity.
   *
   * @param collection_id The identifier for the shared memory pool collection that the shared memory pool resides in.
   * @param shm_pool The shared memory pool to register.
   *
   * @return Whether the shared memory pool was registered successfully.
   */
  bool register_lender_shm_pool(Collection_id collection_id, const std::shared_ptr<Shm_pool>& shm_pool);
  /**
   * Deregisters that a shared memory pool is lent to another entity.
   *
   * @param collection_id The identifier for the shared memory pool collection that the shared memory pool resides in.
   * @param shm_pool The shared memory pool to deregister.
   * @param was_empty If successful, this will contain whether the pool was empty at deregistration.
   *
   * @return Whether the shared memory pool was deregistered successfully.
   */
  bool deregister_lender_shm_pool(Collection_id collection_id,
                                  const std::shared_ptr<Shm_pool>& shm_pool,
                                  bool& was_empty);
  /**
   * Registers that an object is lent to another entity. An object can be registered multiple times.
   *
   * @param collection_id The identifier for the shared memory pool collection that the object resides in.
   * @param object The object to register.
   * @param shm_pool_id If successful, this will contain the shared memory pool id that the object resides in.
   * @param pool_offset If successful, this will contain the offset from the base of the shared memory pool that the
   *                    object resides at.
   *
   * @return Whether the object was registered successfully.
   */
  bool register_lender_object(Collection_id collection_id,
                              const std::shared_ptr<void>& object,
                              pool_id_t& shm_pool_id,
                              pool_offset_t& pool_offset);
  /**
   * Deregisters that an object is lent to another process. Note than an object can be registered multiple times
   * and thus deregistered multiple times depending on the number of times registered.
   *
   * @param collection_id The identifier for the shared memory pool collection that the object resides in.
   * @param shm_pool_id The identifier for the shared memory pool that the object resides in.
   * @param pool_offset The offset from the base of the shared memory pool that the object resides at.
   *
   * @return If the object was deregistered successfully, the object; otherwise, nullptr.
   */
  std::shared_ptr<void> deregister_lender_object(Collection_id collection_id,
                                                 pool_id_t shm_pool_id,
                                                 pool_offset_t pool_offset);

  /**
   * Registers that a shared memory pool collection is borrowed from another entity.
   *
   * @param shm_pool_collection The shared memory pool collection to register.
   *
   * @return Whether the shared memory pool collection was registered successfully.
   */
  bool register_borrower_collection(const std::shared_ptr<Borrower_shm_pool_collection>& shm_pool_collection);
  /**
   * Registers a shared memory pool borrowed from another entity.
   *
   * @param collection_id The identifier for the shared memory pool collection that the shared memory pool resides in.
   * @param shm_pool The shared memory pool to register.
   *
   * @return Whether the shared memory pool was registered successfully.
   */
  bool register_borrower_shm_pool(Collection_id collection_id, const std::shared_ptr<Shm_pool>& shm_pool);
  /**
   * Deregisters a shared memory pool borrowed from another entity.
   *
   * @param collection_id The identifier for the shared memory pool collection that the shared memory pool resides in.
   * @param shm_pool_id The identifier for the shared memory pool to deregister.
   *
   * @return Whether the shared memory pool was deregistered successfully.
   */
  bool deregister_borrower_shm_pool(Collection_id collection_id, pool_id_t shm_pool_id);
  /**
   * Constructs and registers an object borrowed from another entity. An object can be registered multiple times
   * and each registration will be a construction of a different shared pointer to the same object in shared
   * memory.
   *
   * @tparam T The type of object to construct.
   * @param collection_id The identifier for the shared memory pool collection that the object resides in.
   * @param shm_pool_id The identifier for the shared memory pool that the object resides in.
   * @param pool_offset The offset from the base of the shared memory pool that the object resides at.
   * @param release_callback The functor to execute when the object is destroyed.
   *
   * @return If successful, the registered object; otherwise, nullptr.
   */
  template <typename T>
  std::shared_ptr<T> construct_and_register_borrower_object(
    Collection_id collection_id,
    pool_id_t shm_pool_id,
    pool_offset_t pool_offset,
    Borrower_shm_pool_collection::Release_callback&& release_callback);
  /**
   * Deregisters that an object is borrowed from another entity.  Note than an object can be registered multiple
   * times and thus can be deregistered multiple times depending on the number of times registered.
   *
   * @param collection_id The identifier for the shared memory pool collection that the object resides in.
   * @param shm_pool_id The identifier for the shared memory pool that the object resides in.
   * @param pool_offset The offset from the base of the shared memory pool that the object resides at.
   *
   * @return Whether the object was deregistered successfully.
   */
  bool deregister_borrower_object(Collection_id collection_id,
                                  pool_id_t shm_pool_id,
                                  pool_offset_t pool_offset);

  /**
   * Returns the borrower collection map, which is not thread-safe in its usage. The use case for this interface is
   * for cleaning up globally registered shared memory pools on destruction of a container holding this entity.
   *
   * @return See above.
   */
  inline const std::unordered_map<Collection_id, std::unique_ptr<Borrower_collection>>&
  get_borrower_collection_map() const;

private:
  /// The mutex type.
  using Mutex = std::mutex;
  /// Exclusive lock for the mutex.
  using Lock = std::lock_guard<Mutex>;

  /// Protects access to #m_lender_collection_map.
  mutable Mutex m_lender_mutex;
  /// Shared memory pool collections, shared memory pools and objects lent to the other process.
  std::unordered_map<Collection_id, std::unique_ptr<Lender_collection>> m_lender_collection_map;

  /// Protects access to #m_borrower_collection_map.
  mutable Mutex m_borrower_mutex;
  /// Shared memory pool collections, shared memory pools and objects borrowed from the other process.
  std::unordered_map<Collection_id, std::unique_ptr<Borrower_collection>> m_borrower_collection_map;
}; // class Shm_session_data

template <typename T>
std::shared_ptr<T> Shm_session_data::construct_and_register_borrower_object(
  Collection_id collection_id,
  pool_id_t shm_pool_id,
  pool_offset_t pool_offset,
  Borrower_shm_pool_collection::Release_callback&& release_callback)
{
  Lock lock(m_borrower_mutex);

  auto iter = m_borrower_collection_map.find(collection_id);
  if (iter == m_borrower_collection_map.end())
  {
    FLOW_LOG_WARNING("Could not find borrower collection [" << collection_id <<
                     "] when registering borrower object in SHM pool id [" << shm_pool_id <<
                     "], offset [" << pool_offset << "]");
    return nullptr;
  }

  auto& borrower_collection = iter->second;
  auto& shm_pool = borrower_collection->find_shm_pool(shm_pool_id);
  if (shm_pool == nullptr)
  {
    FLOW_LOG_WARNING("Could not find SHM pool [" << shm_pool_id <<
                     "] when registering borrower object in collection [" << collection_id <<
                     "], offset [" << pool_offset << "]");
    return nullptr;
  }

  if (!borrower_collection->register_object(shm_pool_id, pool_offset))
  {
    return nullptr;
  }

  auto& borrower_shm_pool_collection = borrower_collection->get_borrower_shm_pool_collection();
  // We do this last as it is harder to back out as the deleter functor will be called once properly constructed
  auto object = borrower_shm_pool_collection->construct<T>(shm_pool->get_address(),
                                                           pool_offset,
                                                           std::move(release_callback));
  if (object == nullptr)
  {
    if (!borrower_collection->deregister_object(shm_pool_id, pool_offset))
    {
      FLOW_LOG_WARNING("Could not deregister object at SHM pool [" << shm_pool_id << "], offset [" << pool_offset <<
                       "] that we just registered properly in collection [" << collection_id << "]");
    }
    return nullptr;
  }

  return object;
}

const std::unordered_map<Collection_id, std::unique_ptr<Borrower_collection>>&
Shm_session_data::get_borrower_collection_map() const
{
  return m_borrower_collection_map;
}

} // namespace ipc::session::shm::arena_lend
