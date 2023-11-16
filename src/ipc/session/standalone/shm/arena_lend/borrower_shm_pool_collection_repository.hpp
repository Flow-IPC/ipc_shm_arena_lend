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

#include "ipc/session/standalone/shm/arena_lend/arena_lend_fwd.hpp"
#include "ipc/shm/arena_lend/borrower_shm_pool_repository.hpp"
#include "ipc/shm/arena_lend/borrower_shm_pool_collection.hpp"
#include "ipc/shm/arena_lend/arena_lend_fwd.hpp"
#include "ipc/util/use_counted_object.hpp"
#include <flow/log/log.hpp>
#include <set>

namespace ipc::session::shm::arena_lend
{

/**
 * Repository for borrowed shared memory pool collection data. The primary use case for such a repository is
 * when handles are stored pointing to data residing in shared memory. Related sessions should funnel their
 * information to a repository instance, such that conversion from handles to pointers can be performed.
 *
 * The organizational hierarchy is owner -> collection -> shared memory pool. There can be multiple sessions
 * with the same owner, but within each session, there must not be the same collection borrowed nor the same
 * shared memory pool borrowed. Generally, when a session is established, the owner should be registered. As
 * collections are borrowed, those should be registered. Lastly, as shared memory pools are borrowed, those
 * should be registered. The first time a shared memory pool is registered, it will be opened (internally).
 *
 * As shared memory pools are instructed to be removed, those should be deregistered. The last time a shared
 * memory pool is deregistered, it will be closed (internally). Similarly, when collections are instructed to
 * be removed, those should be deregistered. Lastly, when a session terminates, the owner (remote side of the
 * session) should be deregistered.
 *
 * @see ipc::shm::arena_lend::Shm_pool_offset_ptr
 * @see ipc::shm::arena_lend::Shm_pool_repository_singleton
 */
class Borrower_shm_pool_collection_repository
{
protected:
  /// Alias for a borrower shared memory pool collection.
  using Borrower_shm_pool_collection = ipc::shm::arena_lend::Borrower_shm_pool_collection;

public:
  /// Short-hand for pool ID type.
  using pool_id_t = Borrower_shm_pool_collection::pool_id_t;
  /// Short-hand for pool offset type.
  using pool_offset_t = Borrower_shm_pool_collection::pool_offset_t;

  /**
   * Constructor. This must take zero parameters due to singleton support.
   *
   * @see ipc::shm::arena_lend::Shm_pool_repository_singleton
   */
  Borrower_shm_pool_collection_repository();

  /**
   * Sets the logger.
   *
   * @param logger Used for logging purposes. Any non-null logger must be valid for the lifetime of this object.
   *
   * @warning Must be set in a single-threaded context! It is not thread-safe for performance reasons.
   */
  inline void set_logger(flow::log::Logger* logger);
  /**
   * Returns the logger.
   *
   * @return See above.
   */
  inline flow::log::Logger* get_logger() const;
  /**
   * Returns the component used for categorizing log messages.
   *
   * @return See above.
   */
  inline const flow::log::Component& get_log_component() const;

  /**
   * Registers an owner in the repository. Owners may be registered multiple times and should be subsequently
   * deregistered after its contained information is deregistered.
   *
   * @param owner_id The owner to be registered.
   */
  void register_owner(Owner_id owner_id);
  /**
   * Deregisters a previously registered owner. As owners may be registered multiple times, its contents should be
   * deregistered prior.
   *
   * @param owner_id The owner to be deregistered.
   *
   * @return Whether the owner was deregistered.
   */
  bool deregister_owner(Owner_id owner_id);
  /**
   * Registers a shared memory pool collection. A collection may be registered multiple times as an owner may be
   * registered multiple times, but it should only be registered once per borrower. Thus, the number of times a
   * collection is registered should not exceed the number of times an owner is registered.
   *
   * @param owner_id The owner of the collection to be registered.
   * @param collection_id The identifier of the collection to be registered.
   *
   * @return If successful, the collection; otherwise, nullptr.
   */
  std::shared_ptr<Borrower_shm_pool_collection> register_collection(Owner_id owner_id, Collection_id collection_id);
  /**
   * Deregisters a shared memory pool collection. The information contained within the collection should be
   * deregistered prior.
   *
   * @param owner_id The owner of the collection to be deregistered.
   * @param collection_id The identifier of the collection to be deregistered.
   *
   * @return Whether the collection was deregistered successfully.
   */
  bool deregister_collection(Owner_id owner_id, Collection_id collection_id);
  /**
   * Registers a shared memory pool. A pool may be registered multiple times as an owner may be registered
   * multiple times, but it should only be registered once per borrower. Thus, the number of times a pool
   * is registered should not exceed the number of times an owner is registered. If the shared memory pool
   * was not previously registered, the shared memory pool will be opened prior to registration.
   *
   * @param owner_id The owner of the collection to be registered.
   * @param collection_id The identifier of the collection containing the shared memory pool to be registered.
   * @param shm_pool_id The identifier of the shared memory pool.
   * @param shm_pool_name The name of the shared memory object.
   * @param shm_pool_size The size of the shared memory pool.
   *
   * @return If successful, the shared memory pool; otherwise, nullptr.
   */
  std::shared_ptr<ipc::shm::arena_lend::Shm_pool> register_shm_pool(Owner_id owner_id,
                                                                    Collection_id collection_id,
                                                                    pool_id_t shm_pool_id,
                                                                    const std::string& shm_pool_name,
                                                                    std::size_t shm_pool_size);
  /**
   * Deregisters a shared memory pool. If the shared memory pool no longer has any more registrations, the
   * shared memory pool will be closed.
   *
   * @param owner_id The owner of the collection to be deregistered.
   * @param collection_id The identifier of the collection containing the shared memory pool to be deregistered.
   * @param shm_pool_id The identifier of the shared memory pool.
   *
   * @return Whether the shared memory pool was deregistered.
   */
  bool deregister_shm_pool(Owner_id owner_id, Collection_id collection_id, pool_id_t shm_pool_id);
  /**
   * Acts analogously to Shm_pool_repository method. This implements the interface requirement of
   * #ipc::shm::arena_lend::Shm_pool_repository_singleton.
   *
   * @param shm_pool_id See above.
   * @param offset See above.
   *
   * @return  See above.
   */
  inline void* to_address(pool_id_t shm_pool_id, pool_offset_t offset);
  /**
   * Acts analogously to Shm_pool_repository method. This implements the interface requirement
   * of #ipc::shm::arena_lend::Shm_pool_repository_singleton.
   *
   * @param address See above.
   * @param shm_pool See above.
   * @param offset See above.
   */
  inline void from_address(const void* address,
                           std::shared_ptr<ipc::shm::arena_lend::Shm_pool>& shm_pool,
                           pool_offset_t& offset) const;

private:
  /// The mutex type.
  using Mutex = std::mutex;
  /// Exclusive access lock.
  using Lock = std::lock_guard<Mutex>;

  /**
   * Tracks utilization of a shared memory pool collection.
   */
  class Collection_data :
    public util::Use_counted_object
  {
  public:
    /**
     * Constructor.
     *
     * @param collection The collection to track utilization of.
     */
    Collection_data(const std::shared_ptr<Borrower_shm_pool_collection>& collection);

    /**
     * Returns the associated shared memory pool collection.
     *
     * @return See above.
     */
    inline const std::shared_ptr<Borrower_shm_pool_collection>& get_collection() const
    {
      return m_collection;
    }

    /**
     * Returns whether the shared memory pool id is registered.
     *
     * @param shm_pool_id The shared memory pool id to check.
     *
     * @return See above.
     */
    inline bool find_shm_pool_id(pool_id_t shm_pool_id) const
    {
      return m_shm_pool_ids.find(shm_pool_id) != m_shm_pool_ids.end();
    }

    /**
     * Registers a shared memory pool id.
     *
     * @param shm_pool_id The shared memory pool id to remove.
     *
     * @return Whether the shared memory pool id was registered, meaning it was not already registered.
     */
    inline bool insert_shm_pool_id(pool_id_t shm_pool_id)
    {
      return m_shm_pool_ids.insert(shm_pool_id).second;
    }

    /**
     * Deregisters a shared memory pool id.
     *
     * @param shm_pool_id The shared memory pool id to remove.
     *
     * @return Whether the shared memory pool id was removed, meaning it was previously registered.
     */
    inline bool remove_shm_pool_id(pool_id_t shm_pool_id)
    {
      return m_shm_pool_ids.erase(shm_pool_id) != 0;
    }

  private:
    /// The associated shared memory pool collection.
    std::shared_ptr<Borrower_shm_pool_collection> m_collection;
    /// The associated shared memory pools.
    std::set<pool_id_t> m_shm_pool_ids;
  }; // class Collection_data

  /**
   * Tracks utilization of an owner and its borrowed data.
   */
  class Owner_data :
    public util::Use_counted_object
  {
  public:
    /// Alias for a map of collection id -> collection data.
    using Collection_data_map = std::unordered_map<Collection_id, Collection_data>;
    /**
     * Returns the shared memory collection utilization information.
     *
     * @return See above.
     */
    inline Collection_data_map& get_collection_data_map()
    {
      return m_collection_data_map;
    }

  private:
    /// The shared memory collection utilization information.
    Collection_data_map m_collection_data_map;
  }; // class Owner_data

  /// Used for logging purposes.
  flow::log::Logger* m_logger;
  /// Serializes access to #m_owner_data_map and modifications to #m_shm_pool_repository.
  mutable Mutex m_owner_data_map_mutex;
  /// The shared memory pool repository.
  ipc::shm::arena_lend::Borrower_shm_pool_repository m_shm_pool_repository;
  /// Maps owner id -> owner utilization data.
  std::unordered_map<Owner_id, Owner_data> m_owner_data_map;

  /// The categorization of log messages for this class.
  static flow::log::Component S_LOG_COMPONENT;
}; // class Borrower_shm_pool_collection_repository

void* Borrower_shm_pool_collection_repository::to_address(pool_id_t shm_pool_id, pool_offset_t offset)
{
  return m_shm_pool_repository.to_address(shm_pool_id, offset);
}

void Borrower_shm_pool_collection_repository::from_address(const void* address,
                                                           std::shared_ptr<ipc::shm::arena_lend::Shm_pool>& shm_pool,
                                                           pool_offset_t& offset) const
{
  m_shm_pool_repository.from_address(address, shm_pool, offset);
}

void Borrower_shm_pool_collection_repository::set_logger(flow::log::Logger* logger)
{
  m_logger = logger;
}

flow::log::Logger* Borrower_shm_pool_collection_repository::get_logger() const
{
  return m_logger;
}

const flow::log::Component& Borrower_shm_pool_collection_repository::get_log_component() const
{
  return S_LOG_COMPONENT;
}

} // namespace ipc::session::shm::arena_lend
