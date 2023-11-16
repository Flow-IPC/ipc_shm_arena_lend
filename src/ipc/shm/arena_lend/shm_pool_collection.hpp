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

#include "ipc/shm/arena_lend/arena_lend_fwd.hpp"
#include <memory>
#include <map>
#include <set>
#include <flow/log/log.hpp>

namespace ipc::shm::arena_lend
{

/**
 * A set of shared memory pools related in their application usage.
 */
class Shm_pool_collection :
  public flow::log::Log_context
{
public:
  /**
   * Constructor.
   *
   * @param logger For logging purposes.
   * @param id Identifier for the collection.
   */
  Shm_pool_collection(flow::log::Logger* logger, Collection_id id);
  /**
   * Destructor.
   */
  virtual ~Shm_pool_collection();

  /**
   * Returns the identifier for the collection.
   *
   * @return See above.
   */
  inline Collection_id get_id() const;

  /**
   * Returns the shared memory pool that the address resides in.
   *
   * @param address The address to lookup.
   *
   * @return If the address is within the collection, a shared pointer to the shared memory pool containing that
   *         address; otherwise, a null shared pointer.
   */
  std::shared_ptr<Shm_pool> lookup_shm_pool(const void* address) const;
  /**
   * Returns the shared memory pool that corresponds to the address.
   *
   * @param address The address to lookup.
   *
   * @return A shared pointer to the shared memory pool, which base address matches the address; otherwise, a null
   *         shared pointer, if there is no match.
   */
  std::shared_ptr<Shm_pool> lookup_shm_pool_exact(const void* address) const;

  /**
   * Prints the shared memory pools.
   */
  void print_shm_pool_map() const;

protected:
  /**
   * Maps an opened shared memory object.
   *
   * @param size Size of the shared memory; this should be a multiple of page size.
   * @param read_enabled Whether the mapping should be read enabled.
   * @param write_enabled Whether the mapping should be write enabled.
   * @param fd The file descriptor of the opened shared memory.
   *
   * @return Upon success, the address to the shared memory pool; otherwise, nullptr.
   */
  void* map_shm(std::size_t size, bool read_enabled, bool write_enabled, int fd);
  /**
   * Unmaps a shared memory pool and closes the open file handle to the shared memory.
   * Note that this does not remove the shared memory.
   *
   * @param shm_pool The shared memory pool to close.
   *
   * @return Whether the shared memory pool was unmapped and closed properly.
   */
  bool close_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool);
  /**
   * Registers a shared memory pool as part of the collection.
   *
   * @param shm_pool The shared memory pool to register
   *
   * @return Whether the registration was successfully performed, which generally would only fail if the shared memory
   *         address is currently registered.
   */
  bool register_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool);
  /**
   * Deregisters a shared memory pool from the collection.
   *
   * @param shm_pool The shared memory pool to deregister.
   *
   * @return Whether the deregistration was successfully performed, which generally would only fail if the shared
   *         memory address is not registered.
   */
  bool deregister_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool);

private:
  /// Multi-reader, single-writer mutex.
  using Mutex = flow::util::Mutex_shared_non_recursive;
  /// Single-writer lock for the mutex.
  using Write_lock = flow::util::Lock_guard<Mutex>;
  /// Multi-reader lock for the mutex.
  using Read_lock = flow::util::Shared_lock_guard<Mutex>;

  /**
   * Wrapper around mmap().
   *
   * @param address The address to map memory or nullptr to allow system to select the address.
   * @param size The size of the memory region to be mapped.
   * @param memory_protection_flags The desired accessibility of the mapped region.
   * @param flags Mapping options.
   * @param fd The file descriptor of the opened shared memory or -1 for local heap.
   *
   * @return Upon success, the mapped region's address; otherwise, nullptr.
   */
  void* memory_map(void* address, std::size_t size, int memory_protection_flags, int flags, int fd);
  /**
   * Wrapper around munmap().
   *
   * @param address The address to unmap memory.
   * @param size The size of the memory region to be unmapped.
   *
   * @return Whether unmapping of the memory region was successful.
   */
  bool memory_unmap(void* address, std::size_t size);
  /**
   * Deregisters an address to shared memory from the collection.
   *
   * @param address The address to a shared memory pool to deregister.
   *
   * @return Whether the deregistration was successfully performed. It generally would only fail if the shared memory
   *         address is not registered.
   */
  inline bool deregister_shm_pool_internal(void* address);

  /// Collection identifier.
  const Collection_id m_id;
  /// Mutex for pool map.
  mutable Mutex m_shm_pool_map_mutex;
  /// Address -> Shm_pool ptr map.
  std::map<const void*, std::shared_ptr<Shm_pool>> m_shm_pool_map;
}; // class Shm_pool_collection

Collection_id Shm_pool_collection::get_id() const
{
  return m_id;
}

bool Shm_pool_collection::deregister_shm_pool_internal(void* address)
{
  Write_lock write_lock(m_shm_pool_map_mutex);
  return (m_shm_pool_map.erase(address) > 0);
}

} // namespace ipc::shm::arena_lend
