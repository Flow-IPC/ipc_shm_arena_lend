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

#include "ipc/shm/arena_lend/owner_shm_pool_collection.hpp"

namespace ipc::shm::arena_lend::test
{

/**
 * An implementation of a Shm_pool_collection for testing purposes to avoid using a subclass and expose
 * protected methods.
 */
class Test_shm_pool_collection :
  public Shm_pool_collection
{
public:
  /// Default collection identifier.
  static constexpr Collection_id S_DEFAULT_COLLECTION_ID = 10;
  // Make public
  using Shm_pool_collection::close_shm_pool;
  using Shm_pool_collection::register_shm_pool;
  using Shm_pool_collection::deregister_shm_pool;

  /**
   * Constructor.
   *
   * @param logger For logging purposes.
   * @param id The identifier for the collection.
   */
  Test_shm_pool_collection(flow::log::Logger* logger, Collection_id id = S_DEFAULT_COLLECTION_ID);

  /**
   * Creates a shared memory object with a generated name and maps it in the process' address space.
   *
   * @param size The desired size of the shared memory object.
   *
   * @return Upon success, a shared pointer to the created shared memory pool; otherwise, an empty shared pointer.
   */
  std::shared_ptr<Shm_pool> create_shm_pool(std::size_t size);
  /**
   * Creates a shared memory object and maps it in the process' address space.
   *
   * @param name The desired name for the shared memory object.
   * @param size The desired size of the shared memory object.
   *
   * @return Upon success, a shared pointer to the created shared memory pool; otherwise, an empty shared pointer.
   */
  std::shared_ptr<Shm_pool> create_shm_pool(const std::string& name, std::size_t size);
  /**
   * Opens a named shared memory object for read-only and maps it in the process' address space.
   *
   * @param name The shared memory object name.
   * @param size The shared memory size when it was constructed.
   * @param write_enabled Whether write is enabled.
   *
   * @return Upon success, a shared pointer to the shared memory pool; otherwise, an empty shared pointer.
   */
  std::shared_ptr<Shm_pool> open_shm_pool(const std::string& name, std::size_t size, bool write_enabled);
  /**
   * Unmaps a shared memory pool and removes the underlying shared memory object.
   *
   * @param shm_pool The shared memory pool to remove.
   *
   * @return Whether the shared memory pool was removed.
   */
  bool remove_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool);

  /**
   * Creates a shared memory object.
   *
   * @param name The shared memory object name.
   * @param size The size of the shared memory object.
   *
   * @return If successful, the file descriptor of the newly created (and opened) shared memory object; otherwise, -1.
   */
  int create_shm_object(const std::string& name, std::size_t size) const;
  /**
   * Removes a shared memory object.
   *
   * @param name The shared memory object name.
   *
   * @return Whether the shared memory object was removed successfully.
   */
  bool remove_shm_object(const std::string& name) const;

private:
  /// Generates shared memory object names.
  Owner_shm_pool_collection::Shm_object_name_generator m_name_generator;
}; // class Test_shm_pool_collection

} // namespace ipc::shm::arena_lend::test
