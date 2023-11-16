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
#include "ipc/shm/arena_lend/test/test_shm_object.hpp"
#include "ipc/util/util_fwd.hpp"

namespace ipc::shm::arena_lend
{

class Memory_manager;

namespace test
{

/**
 * Overrides Owner_shm_pool_collection to gain access to protected members for testing.
 */
class Test_owner_shm_pool_collection :
  public Owner_shm_pool_collection
{
public:
  using Owner_shm_pool_collection::generate_shm_object_name;

  /// Default shared memory pool size.
  static constexpr std::size_t S_DEFAULT_SHM_POOL_SIZE = 4096;

  /**
   * Constructor.
   *
   * @param logger Used for logging purposes.
   * @param id Identifier for the collection.
   * @param memory_manager The memory allocator.
   * @param name_generator Shared object name generator.
   * @param permissions_level The file permissions for the shared memory pool that are created.
   *
   * @see Owner_shm_pool_collection::Owner_shm_pool_collection()
   */
  Test_owner_shm_pool_collection(flow::log::Logger* logger,
                                 Collection_id id,
                                 const std::shared_ptr<Memory_manager>& memory_manager,
                                 Shm_object_name_generator&& name_generator = create_shm_object_name_generator(),
                                 util::Permissions_level permissions_level = util::Permissions_level::S_GROUP_ACCESS);
  /// Destructor.
  virtual ~Test_owner_shm_pool_collection() override;

  /**
   * Allocates memory from the memory manager.
   *
   * @param size The amount of memory to allocate.
   *
   * @return The resulting allocation upon success, or nullptr, upon failure.
   */
  virtual void* allocate(std::size_t size) override;
  /**
   * Creates a shared memory object and maps it in the process' address space.
   *
   * @param size The desired size of the shared memory object.
   *
   * @return Upon success, a shared pointer to the created shared memory pool; otherwise, an empty shared pointer.
   */
  inline std::shared_ptr<Shm_pool> create_shm_pool(std::size_t size = S_DEFAULT_SHM_POOL_SIZE);
  /**
   * Creates a shared memory object and maps it in the process' address space.
   *
   * @param name The name of the shared memory object to create.
   * @param size The desired size of the shared memory object.
   *
   * @return Upon success, a shared pointer to the created shared memory pool; otherwise, an empty shared pointer.
   */
  inline std::shared_ptr<Shm_pool> create_shm_pool(const std::string& name, std::size_t size);
  /**
   * Unmaps a shared memory pool and removes the underlying shared memory object.
   *
   * @param shm_pool The shared memory pool to remove.
   *
   * @return Whether the shared memory pool was removed; it's possible that partial execution was successfully
   *         performed like unmapping, but not shared memory removal, which would be false result.
   */
  inline bool remove_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool);
  /**
   * Unmaps a shared memory pool and removes the underlying shared memory object.
   *
   * @param shm_pool The shared memory pool to remove.
   * @param unmapped_memory_pool Whether the memory pool was unmapped, which may be true even though the result
   *                             is false.
   *
   * @return Whether the shared memory pool was removed; it's possible that partial execution was successfully
   *         performed like unmapping, but not shared memory removal, which would be false result.
   */
  inline bool remove_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool, bool& unmapped_memory_pool);

protected:
  /**
   * Returns the memory map functor.
   *
   * @return See above.
   */
  inline const Memory_map_functor& get_memory_map_functor() const;
  /**
   * Returns the memory unmap functor.
   *
   * @return See above.
   */
  inline const Memory_unmap_functor& get_memory_unmap_functor() const;

private:
  /// Functor that maps a shared memory pool.
  const Memory_map_functor m_memory_map_functor;
  /// Functor that unmaps a shared memory pool.
  const Memory_unmap_functor m_memory_unmap_functor;
}; // class Test_owner_shm_pool_collection

const Owner_shm_pool_collection::Memory_map_functor& Test_owner_shm_pool_collection::get_memory_map_functor() const
{
  return m_memory_map_functor;
}

const Owner_shm_pool_collection::Memory_unmap_functor& Test_owner_shm_pool_collection::get_memory_unmap_functor() const
{
  return m_memory_unmap_functor;
}

std::shared_ptr<Shm_pool> Test_owner_shm_pool_collection::create_shm_pool(std::size_t size)
{
  return create_shm_pool(generate_shm_object_name(0 /* our name-generator ignores it anyway */), size);
}

std::shared_ptr<Shm_pool> Test_owner_shm_pool_collection::create_shm_pool(const std::string& name,
                                                                          std::size_t size)
{
  return Owner_shm_pool_collection::create_shm_pool(detail::Shm_pool_offset_ptr_data_base::generate_pool_id(),
                                                    name, size, nullptr, m_memory_map_functor);
}

bool Test_owner_shm_pool_collection::remove_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool)
{
  bool unmapped_memory_pool;
  return Owner_shm_pool_collection::remove_shm_pool(shm_pool, m_memory_unmap_functor, unmapped_memory_pool);
}

bool Test_owner_shm_pool_collection::remove_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool,
                                                     bool& unmapped_memory_pool)
{
  return Owner_shm_pool_collection::remove_shm_pool(shm_pool, m_memory_unmap_functor, unmapped_memory_pool);
}

} // namespace test
} // namespace ipc::shm::arena_lend
