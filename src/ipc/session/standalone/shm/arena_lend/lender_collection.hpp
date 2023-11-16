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
#include "ipc/shm/arena_lend/shm_pool_collection.hpp"

namespace ipc::session::shm::arena_lend
{

/**
 * Collection of shared memory pools and objects lent to a borrower. The collection is organized by
 * shared memory pool collections containing shared memory pool identifiers containing object offsets.
 * The shared memory pool id uniquely identifies a shared memory pool within the collection. An object
 * offset within a shared memory pool may be reused once the object is removed.
 */
class Lender_collection :
  public flow::log::Log_context
{
public:
  /// Alias for a shared memory pool.
  using Shm_pool = ipc::shm::arena_lend::Shm_pool;
  /// Alias for a shared memory pool collection.
  using Shm_pool_collection = ipc::shm::arena_lend::Shm_pool_collection;
  /// Short-hand for pool ID type.
  using pool_id_t = Shm_pool::pool_id_t;
  /// Short-hand for pool offset type.
  using pool_offset_t = Shm_pool::size_t;

  /**
   * Constructor.
   *
   * @param logger Used for logging purposes.
   * @param shm_pool_collection The shared memory pool collection where the memory pools and objects reside.
   *                            Note that this is intentionally not a Owner_shm_pool_collection for future
   *                            potential proxy support where the lender is not the owner.
   */
  Lender_collection(flow::log::Logger* logger,
                    const std::shared_ptr<Shm_pool_collection>& shm_pool_collection);

  /**
   * Registers that the shared memory pool belonging to the shared memory pool collection is or will be shared
   * with the borrower.
   *
   * @param shm_pool The shared memory pool to be registered.
   *
   * @return Whether the pool was registered successfully, which means that it was not previously registered.
   */
  bool register_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool);
  /**
   * Deregisters the shared memory pool id as being shared with the borrower.
   *
   * @param shm_pool_id The id of the shared memory pool to be deregistered.
   * @param was_empty If successful, this will contain whether the pool was empty at deregistration.
   *
   * @return Whether the pool was deregistered successfully, which means that the pool corresponding to the id
   *         was previously registered.
   */
  bool deregister_shm_pool(pool_id_t shm_pool_id, bool& was_empty);
  /**
   * Registers that an object is or will be shared with the borrower. An object will be determined to be within
   * a particular shared memory pool by using the pointer address. An object can be registered multiple times.
   *
   * @param object The object to be registered.
   * @param shm_pool_id When the result is true, this will contain the id of the shared memory pool where the
   *                    object resides; otherwise, it is to be ignored.
   * @param pool_offset When the result is true, this will contain the offset within the shared memory pool where
   *                    the base of the object resides; otherwise, it is to be ignored.
   *
   * @return Whether the object was registered successfully, which means it is within a registered pool.
   */
  bool register_object(const std::shared_ptr<void>& object, pool_id_t& shm_pool_id, pool_offset_t& pool_offset);
  /**
   * Deregisters an object as being shared with the borrower. Note that an object that is registered multiple times
   * must be deregistered the same amount of times to be removed.
   *
   * @param shm_pool_id The id of the shared memory pool where the object resides.
   * @param pool_offset The base offset of the object within the shared memory pool.
   *
   * @return If the object was deregistered successfully, the object; otherwise, nullptr.
   */
  std::shared_ptr<void> deregister_object(pool_id_t shm_pool_id, pool_offset_t pool_offset);

private:
  /// Data corresponding to a registered shared memory pool.
  class Shm_pool_data
  {
  public:
    /**
     * Constructor.
     *
     * @param shm_pool The shared memory pool.
     */
    Shm_pool_data(const std::shared_ptr<Shm_pool>& shm_pool);

    /**
     * Returns the number of unique objects registered.
     *
     * @return See above.
     */
    inline std::size_t size() const;
    /**
     * Returns whether there are any objects registered.
     *
     * @return See above.
     */
    inline bool empty() const;
    /**
     * Registers that an object is or will be shared with the borrower. If it is already being shared, then the
     * use count will be incremented.
     *
     * @param offset The offset within the shared memory pool.
     * @param object The object to register.
     */
    void register_object(pool_offset_t offset, const std::shared_ptr<void>& object);
    /**
     * Deregisters an object as being shared with the borrower. If it is used more than once, the use count will
     * be decremented; otherwise, it will be removed.
     *
     * @param offset The offset of the object within the shared memory pool.
     *
     * @return If the object was deregistered successfully, the object; otherwise, nullptr.
     */
    std::shared_ptr<void> deregister_object(pool_offset_t offset);

  private:
    /**
     * Container holding the object and the number of times it has been lent to a particular borrower.
     */
    struct Object_data
    {
      /**
       * Constructor.
       *
       * @param object The object that is being lent.
       */
      Object_data(const std::shared_ptr<void>& object);

      /// The object that is being lent.
      std::shared_ptr<void> m_object;
      /// The number of times the object is being lent to the borrower.
      unsigned int m_use_count;
    }; // struct Object_data

    /// The shared memory pool where the objects reside.
    const std::shared_ptr<Shm_pool> m_shm_pool;
    /// Map of pool offsets to object data.
    std::unordered_map<pool_offset_t, Object_data> m_object_data_map;
  }; // class Shm_pool_data

  /**
   * Registers that an object is or will be shared with the borrower. An object will be determined to be within
   * a particular shared memory pool by using the pointer address.
   *
   * @param shm_pool_id The id of the shared memory pool where the object resides.
   * @param pool_offset The base offset of the object within the shared memory pool.
   * @param object The object to be registered.
   *
   * @return Whether the object was registered successfully.
   */
  bool register_object(pool_id_t shm_pool_id, pool_offset_t pool_offset, const std::shared_ptr<void>& object);
  /**
   * Returns the id of the shared memory pool collection.
   *
   * @return See above.
   */
  inline Collection_id get_id() const;

  /// The shared memory pool collection where the objects reside.
  const std::shared_ptr<Shm_pool_collection> m_shm_pool_collection;
  /// Maps shared memory pool id to shared memory pool data.
  std::unordered_map<pool_id_t, Shm_pool_data> m_shm_pool_data_map;
}; // class Lender_collection

Collection_id Lender_collection::get_id() const
{
  return m_shm_pool_collection->get_id();
}

std::size_t Lender_collection::Shm_pool_data::size() const
{
  return m_object_data_map.size();
}

bool Lender_collection::Shm_pool_data::empty() const
{
  return size() == 0;
}

} // namespace ipc::session::shm::arena_lend
