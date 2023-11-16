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

#include "ipc/shm/arena_lend/borrower_shm_pool_collection.hpp"
#include "ipc/session/standalone/shm/arena_lend/arena_lend_fwd.hpp"

namespace ipc::session::shm::arena_lend
{

/**
 * Collection of shared memory pools and objects borrowed from a lender. The collection is organized
 * by shared memory pool ids mapped to a shared memory pool and object offsets. The shared memory pool id
 * uniquely identifies a shared memory pool within the collection. An object offset within a shared memory
 * pool may be reused once the object is removed.
 *
 * The class API is not thread-safe.
 */
class Borrower_collection :
  public flow::log::Log_context
{
public:
  /// Alias for a borrower shared memory pool collection.
  using Borrower_shm_pool_collection = ipc::shm::arena_lend::Borrower_shm_pool_collection;
  /// Alias for a shared memory pool.
  using Shm_pool = ipc::shm::arena_lend::Shm_pool;
  /// Short-hand for pool ID type.
  using pool_id_t = Shm_pool::pool_id_t;
  /// Short-hand for pool offset type.
  using pool_offset_t = Shm_pool::size_t;

  /**
   * Constructor.
   *
   * @param logger Used for logging purposes.
   * @param borrower_shm_pool_collection The shared memory pool collection where the memory pools and objects reside.
   */
  Borrower_collection(flow::log::Logger* logger,
                      const std::shared_ptr<Borrower_shm_pool_collection>& borrower_shm_pool_collection);

  /**
   * Returns the shared memory pool collection where the memory pools and objects reside.
   *
   * @return See above.
   */
  inline const std::shared_ptr<Borrower_shm_pool_collection>& get_borrower_shm_pool_collection() const;
  /**
   * Registers that the shared memory pool belonging to this shared memory pool collection is shared by the
   * lender.
   *
   * @param shm_pool The shared memory pool to be registered.
   *
   * @return Whether the shared memory pool was registered successfully, which means that it was not previously
   *         registered.
   */
  bool register_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool);
  /**
   * Deregisters the shared memory pool id as being shared by the lender.
   *
   * @param shm_pool_id The id of the shared memory pool to be deregistered.
   *
   * @return If successful, the shared memory pool that was deregistered; otherwise, an empty shared memory pool.
   */
  std::shared_ptr<Shm_pool> deregister_shm_pool(pool_id_t shm_pool_id);
  /**
   * Returns the registered shared memory pool located by id.
   *
   * @param shm_pool_id The shared memory pool id to locate.
   *
   * @return If found, the shared memory pool with the id; otherwise, S_EMPTY_SHM_POOL.
   */
  const std::shared_ptr<Shm_pool>& find_shm_pool(pool_id_t shm_pool_id) const;
  /**
   * Registers that an object is shared by the lender. An object can be shared multiple times by the lender.
   *
   * @param shm_pool_id The id of the shared memory pool where the object resides.
   * @param pool_offset The offset within the shared memory pool where the base of the object resides.
   *
   * @return Whether the object was registered successfully, which means that the pool was previously registered.
   */
  bool register_object(pool_id_t shm_pool_id, pool_offset_t pool_offset);
  /**
   * Deregisters that an object is shared by the lender.
   *
   * @param shm_pool_id The id of the shared memory pool where the object resides.
   * @param pool_offset The offset within the shared memory pool where the base of the object resides.
   *
   * @return Whether the object was deregistered successfully, which means that it was previously registered.
   */
  bool deregister_object(pool_id_t shm_pool_id, pool_offset_t pool_offset);
  /**
   * Returns the shared memory pool ids that are currently registered. The use case for this interface is global
   * shared memory pool repository cleanup.
   *
   * @return See above.
   */
  std::set<pool_id_t> get_shm_pool_ids() const;

  /// An empty shared memory pool used for return values when a shared memory pool is not found.
  static const std::shared_ptr<Shm_pool> S_EMPTY_SHM_POOL;

private:
  /// The container for data pertaining to a shared memory pool.
  struct Shm_pool_data
  {
    /**
     * Constructor.
     *
     * @param shm_pool The shared memory pool this container pertains to.
     */
    Shm_pool_data(const std::shared_ptr<Shm_pool>& shm_pool);

    /// The shared memory pool this container pertains to.
    const std::shared_ptr<Shm_pool> m_shm_pool;
    /**
     * A map of objects represented by offsets from the base of the shared memory pool to the number of times it
     * is currently registered for borrowing.
     */
    std::unordered_map<pool_offset_t, unsigned int> m_offset_map;
  }; // struct Shm_pool_data

  /**
   * Returns the id of the shared memory pool collection.
   *
   * @return See above.
   */
  inline Collection_id get_id() const;

  /// The shared memory pool collection where the objects reside.
  const std::shared_ptr<Borrower_shm_pool_collection> m_borrower_shm_pool_collection;
  /// Maps a shared memory pool id to correlated data.
  std::unordered_map<pool_id_t, Shm_pool_data> m_shm_pool_data_map;
}; // class Borrower_collection

const std::shared_ptr<ipc::shm::arena_lend::Borrower_shm_pool_collection>&
Borrower_collection::get_borrower_shm_pool_collection() const
{
  return m_borrower_shm_pool_collection;
}

Collection_id Borrower_collection::get_id() const
{
  return m_borrower_shm_pool_collection->get_id();
}

} // namespace ipc::session::shm::arena_lend
