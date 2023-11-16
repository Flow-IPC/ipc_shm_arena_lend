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

#include "ipc/shm/arena_lend/shm_pool_collection.hpp"

namespace ipc::shm::arena_lend
{

/**
 * Shared memory pool collection for borrowers, which are entities (e.g., processes) that do not have access to
 * the memory manager. In other words, they cannot allocate nor deallocate memory.
 */
class Borrower_shm_pool_collection :
  public Shm_pool_collection
{
public:
  /**
   * Alias for a callback that is executed when a shared pointer is destructed.
   * The parameter is for the address of the memory that is being released.
   *
   * @see construct()
   */
  using Release_callback = std::function<void(void*)>;
  /// Short-hand for pool ID type.
  using pool_id_t = Shm_pool::pool_id_t;
  /// Short-hand for pool offset type.
  using pool_offset_t = Shm_pool::size_t;

  /**
   * Constructor.
   *
   * @param logger For logging purposes.
   * @param id Identifier for the collection.
   */
  Borrower_shm_pool_collection(flow::log::Logger* logger, Collection_id id);

  /**
   * Opens a named shared memory object for read-only and maps it in the process' address space.
   *
   * @param id Pool ID.
   * @param name Shared memory object name.
   * @param size Shared memory size when it was constructed.
   *
   * @return Upon success, a shared pointer to the shared memory pool; otherwise, an empty shared pointer.
   */
  std::shared_ptr<Shm_pool> open_shm_pool(pool_id_t id, const std::string& name, std::size_t size);
  /**
   * Unmaps and closes a shared memory pool. Note that this does not remove the shared memory object.
   *
   * @param shm_pool The shared memory pool to be released.
   *
   * @return Whether the shared memory pool was released successfully.
   */
  inline bool release_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool);

  /**
   * Converts an address to a shared pointer holding an object at that address. When the shared pointer has no more
   * references, a callback will be triggered.
   *
   * @tparam T The object type.
   * @param base_address The base address where the object is located, typically the base address of a mapped region.
   * @param offset The byte offset from the base address where the object is located.
   * @param callback The callback to be executed when the shared pointer has no more references.
   *
   * @return If the sum of the base address and offset is non-zero, a shared pointer to an object pointing to shared
   *         memory; otherwise, an empty shared pointer.
   * @todo The comment immediately preceding this appears to be inaccurate versus the behavior.
   */
  template <typename T>
  std::shared_ptr<T> construct(void* base_address, std::size_t offset, Release_callback&& callback)
  {
    uint8_t* base_address_2 = reinterpret_cast<uint8_t*>(base_address);
    uint8_t* actual_address = base_address_2 + offset;
    // Check that we didn't wrap around
    if (actual_address < base_address_2)
    {
      // @todo Is this not more of an assert()y situation? Relatedly, is it really recoverable practically speaking?
      FLOW_LOG_WARNING("Base address (" << base_address << ") + offset (" << offset <<
                       ") overflowed (" << reinterpret_cast<void*>(actual_address) << ")");
      return nullptr;
    }

    return construct(reinterpret_cast<T*>(actual_address), std::forward<Release_callback>(callback));
  }
  /**
   * Converts an object to a shared pointer holding that object. When the shared pointer has no more references,
   * a callback will be triggered.
   *
   * @tparam T The object type.
   * @param object The object that was created.
   * @param callback The callback to be executed when the shared pointer has no more references. The callback
   *
   * @return If object is non-null, a shared pointer to an object pointing to shared memory; otherwise, an empty
   *         shared pointer.
   */
  template <typename T>
  std::shared_ptr<T> construct(T* object, Release_callback&& callback) const
  {
    if (object == nullptr)
    {
      return nullptr;
    }
    return std::shared_ptr<T>(object, Object_releaser(std::move(callback)));
  }

private:
  /**
   * Handles callback from shared pointer destruction. The result should be a release of the object in the
   * current process (borrower) and notification to the lender.
   */
  class Object_releaser
  {
  public:
    /**
     * Constructor.
     *
     * @param callback The callback to be triggered when release is executed.
     */
    Object_releaser(Release_callback&& callback) :
      m_callback(std::move(callback))
    {
    }

    /**
     * Release callback executed by shared pointer destruction. The stored callback will be executed here.
     *
     * @param p The allocation held by the shared pointer.
     *
     * @todo - Does the callee need type information?
     */
    void operator()(void* p)
    {
      m_callback(p);
    }

  private:
    /// The callback to be triggered when release is executed.
    Release_callback m_callback;
  }; // class Object_releaser
}; // class Borrower_shm_pool_collection

bool Borrower_shm_pool_collection::release_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool)
{
  return (deregister_shm_pool(shm_pool) && close_shm_pool(shm_pool));
}

} // namespace ipc::shm::arena_lend
