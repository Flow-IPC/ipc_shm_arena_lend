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

/// @file
#pragma once

#include "ipc/shm/arena_lend/shm_pool_collection.hpp"
#include "ipc/shm/arena_lend/divisible_shm_pool.hpp"
#include "ipc/shm/arena_lend/arena_lend_fwd.hpp"
#include "ipc/shm/arena_lend/detail/arena_lend_fwd.hpp"
#include "ipc/util/shared_name.hpp"
#include "ipc/util/util_fwd.hpp"
#include <flow/util/util.hpp>

namespace ipc::shm::arena_lend
{

/**
 * Abstract class representing a shared memory pool collection for owners, which are entities (e.g., processes)
 * that have access to a memory manager. In other words, they can allocate and deallocate memory.  Subclasses
 * may override on_shm_pool_created() and on_shm_pool_removed() to be notified of changes in the SHM pool set.
 *
 * @todo ipc::shm::arena_lend::Owner_shm_pool_collection (among others) should be
 * officially classified an internal API (at least, per coding guide, placed in `detail/` header; optionally in `detail`
 * sub-namespace).  Once `ipc::shm::arena_lend` (the SHM-arena-lend module) becomes officially extensible to
 * handle other memory-managers beyond jemalloc, thus allowing for user's own arena-lending SHM-provider impls
 * (not just SHM-jemalloc), then move this back out of `detail`, as sub-classing it is expected in that case.
 */
class Owner_shm_pool_collection :
  public Shm_pool_collection
{
public:
  /// Short-hand for pool ID type.
  using pool_id_t = Shm_pool::pool_id_t;
  /// Short-hand for pool offset type.
  using pool_offset_t = Shm_pool::size_t;

  /**
   * Destructor.  Removes (unmaps, closes, unlinks) any SHM-pools still registered -- see below for why any
   * would be.
   *
   * The subclass typically causes pool removal organically, via its memory manager's native teardown, before
   * this runs: e.g., jemalloc::Ipc_arena's native-arena destruction invokes the mandatory-removal (extent
   * *destroy*) hook per extent.  However that mechanism can leave stragglers -- see inside
   * jemalloc::Ipc_arena::optional_remove_shm_pool() -- and we will here eliminate them too.
   *
   * Formally(ish) we can justify it as follows:
   *   - Can we do it here?  Yes: By the time we run, the whole collection is defunct -- the native arena is
   *     gone, and no one is permitted to rely on any of its pools (in this process or any borrower).
   *   - Should we do it here?  Yes: Whether every pool is removed organically during native-arena destruction
   *     that *must* precede us, or whether some -- or even all -- are left around is a memory-manager (e.g.: jemalloc)
   *     detail, and in actual fact at least some (e.g.: yes, still jemalloc) memory-managers will leave 1+
   *     pool around.  Not removing them here is a leak on some level; probably (no guarantees though) not of physical
   *     RAM but at least of pool-names in the file-system (which can count against OS limits).
   */
  ~Owner_shm_pool_collection() override;

  /**
   * Constructor.
   *
   * @param logger For logging purposes.
   * @param id Identifier for the collection.
   * @param memory_manager The memory allocator.
   * @param pool_name_base Pool-name prefix; each pool's SHM object name is derived from this plus its unique ID.
   * @param permissions The shared memory object file permissions when one is created.
   */
  Owner_shm_pool_collection(flow::log::Logger* logger,
                            collection_id_t id,
                            const std::shared_ptr<Memory_manager>& memory_manager,
                            Shared_name&& pool_name_base,
                            const util::Permissions& permissions);

  /**
   * Allocates memory from the memory manager.
   *
   * @param size The amount of memory to allocate.
   *
   * @return The resulting allocation upon success, or nullptr, upon failure.
   */
  virtual void* allocate(std::size_t size) = 0;
  /**
   * Deallocates memory back to the memory manager.
   *
   * @param address The address to be deallocated.
   */
  virtual void deallocate(void* address);

  /**
   * Returns SHM object file-system permissions we were given via constructor.
   * @return See above.
   */
  const util::Permissions& get_permissions() const;

protected:
  /**
   * Hook invoked after a SHM pool has been registered. Default implementation does nothing.
   *
   * @param shm_pool The shared memory pool that was created.
   */
  virtual void on_shm_pool_created(const std::shared_ptr<Shm_pool>& shm_pool);
  /**
   * Hook invoked after a SHM pool has been deregistered. Default implementation does nothing.
   *
   * @param shm_pool The shared memory pool that was deregistered.
   * @param removed_shared_memory Whether the underlying shared memory was actually removed.
   */
  virtual void on_shm_pool_removed(const std::shared_ptr<Shm_pool>& shm_pool, bool removed_shared_memory);

  /**
   * Function to perform memory mapping.
   *
   * The input parameters are:
   * 1. file descriptor - The file descriptor of the opened shared memory object.
   * 2. size - The size of the shared memory object.
   * 3. address - The address to use for mapping or nullptr, to allow the system to select.
   *
   * The output parameter is the address of the mapped pool when successful; otherwise, nullptr.
   */
  using Memory_map_functor = std::function<void*(int, std::size_t, void*)>;
  /**
   * Function to perform memory unmapping.
   *
   * The input parameter is:
   * 1. shm_pool - The memory pool.
   *
   * The output parameter is whether unmap was successful.
   */
  using Memory_unmap_functor = std::function<bool(const std::shared_ptr<Shm_pool>&)>;
  /**
   * Function to decommit memory, which means purging from physical memory and marking as read only.
   *
   * The input parameters are:
   * 1. shm_pool - The memory pool.
   * 2. offset - The offset from the start of the pool to purge.
   * 3. size - The size to purge.
   *
   * The output parameter is whether unmap was successful.
   */
  using Memory_decommit_functor = std::function<bool(const std::shared_ptr<Shm_pool>&, std::size_t, std::size_t)>;

  /**
   * A shared memory pool that also contains a mutex for serializing access.
   * @todo Making Divisible_shm_pool::m_remaining_size `atomic` -- perhaps optionally based on a tparam --
   * would speed things up (and likely reduce lines-of-code).  `fetch_sub(&R, dec, relaxed) - dec` would
   * be quite quick and would return 0 -- the only condition we care about actually checking -- exactly once.
   * Possibly the `relaxed` might have to be changed to `acq_rel` or something, but in any case it's a well
   * known pattern and tailor-made for this simple algorithm.  Better than a mutex.
   */
  class Lockable_shm_pool :
    public Divisible_shm_pool,
    private boost::noncopyable
  {
  public:
    /// Single-reader, single-writer mutex.
    using Mutex = std::mutex;
    /// Exclusive lock for the mutex.
    using Lock = std::lock_guard<Mutex>;

    /// Constructor.
    using Divisible_shm_pool::Divisible_shm_pool;

    /**
     * Returns the mutex to synchronize access to the pool.
     *
     * @return See description.
     */
    inline Mutex& get_mutex();

  private:
    /// Mutex to synchronize access.
    Mutex m_mutex;
  }; // class Lockable_shm_pool

  /**
   * Returns the memory manager.
   *
   * @return See above.
   */
  inline std::shared_ptr<Memory_manager> get_memory_manager() const;

  /**
   * Creates a shared memory object and maps it in the process' address space.
   *
   * @param id The ultra-unique ID (presumably recently) generated for the pool.
   * @param name The name of the shared memory object to create (perhaps encoding `id` among other things).
   * @param size The desired size of the shared memory object.
   * @param address The address to map the shared memory; a nullptr indicates that the system should select.
   * @param memory_map_functor The mapping function.
   *
   * @return Upon success, the created shared memory pool; otherwise, an empty shared pointer.
   */
  std::shared_ptr<Shm_pool> create_shm_pool(pool_id_t id,
                                            const std::string& name,
                                            std::size_t size,
                                            void* address,
                                            const Memory_map_functor& memory_map_functor);

  /**
   * Logically removes a memory range from a shared memory pool and if the pool is now accounted as empty,
   * removes the shared memory pool.
   *
   * @see remove_shm_pool
   *
   * @param address The starting address of the range.
   * @param size The size of the range.
   * @param decommit_functor A functor to decommit the range from physical memory or nullptr if the range is
   *                         already decommitted.
   * @param removed_range Whether the range was successfully removed and decommitted, if required.
   * @param unmap_functor A functor to unmap the pool.
   * @param unmapped_pool Whether the memory pool was successfully unmapped.
   *
   * @return If there are remaining pages after removal, whether range removal was successful;
   *         otherwise, whether the pool was deregistered.
   */
  bool remove_range_and_pool_if_empty(const void* address,
                                      std::size_t size,
                                      const Memory_decommit_functor* decommit_functor,
                                      bool& removed_range,
                                      const Memory_unmap_functor& unmap_functor,
                                      bool& unmapped_pool);
  /**
   * Deregisters a shared memory pool, unmaps the shared memory pool, removes the underlying shared memory object and
   * notifies listeners. If the deregistration fails, no other actions are performed.
   *
   * @param shm_pool The shared memory pool to remove.
   * @param unmap_functor The unmapping function.
   * @param unmapped_pool Whether the memory pool was successfully unmapped.
   *
   * @return Whether the shared memory pool was deregistered.
   */
  bool remove_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool,
                       const Memory_unmap_functor& unmap_functor,
                       bool& unmapped_pool);

  /**
   * Returns a unique SHM object name for the given pool ID, by combining the pool-name base with the ID.
   *
   * @param shm_pool_id Recently generated ultra-unique ID.
   * @return See above.
   */
  inline Shared_name generate_shm_object_name(pool_id_t shm_pool_id) const;

private:
  /// Friend facade providing privileged access for internal Flow-IPC components.
  template<typename Base_t>
  friend struct detail::Owner_spc_impl;

  /**
   * Returns fragment we were told via constructor to be used as the base for SHM object name.
   * See generate_shm_object_name().
   * @return See above.
   */
  const Shared_name& get_pool_name_base() const;

  /**
   * Creates a shared memory object.
   *
   * @param name The shared memory object name.
   * @param size The size of the shared memory object.
   *
   * @return If successful, the file descriptor of the newly created (and opened) shared memory object; otherwise, -1.
   */
  int create_shm_object(const std::string& name, std::size_t size);
  /**
   * Removes a shared memory object.
   *
   * @param name The shared memory object name.
   *
   * @return Whether the shared memory object was removed successfully.
   */
  bool remove_shm_object(const std::string& name);
  /**
   * Registers a shared memory pool and if successful, sends a notification to listeners.
   *
   * @param shm_pool The shared memory pool to register.
   *
   * @return Whether registration was successful.
   *
   * @see Shm_pool_collection::register_shm_pool
   */
  bool register_shm_pool_and_notify(const std::shared_ptr<Shm_pool>& shm_pool);

  /// Memory allocator.
  std::shared_ptr<Memory_manager> m_memory_manager;
  /// Pool-name prefix; individual pool SHM object names are derived from this.
  const Shared_name m_pool_name_base;
  /// The shared memory object file permissions when one is created.
  const util::Permissions m_permissions;
}; // class Owner_shm_pool_collection

std::shared_ptr<Memory_manager> Owner_shm_pool_collection::get_memory_manager() const
{
  return m_memory_manager;
}

Shared_name Owner_shm_pool_collection::generate_shm_object_name(pool_id_t shm_pool_id) const
{
  return m_pool_name_base / Shared_name::ct_from_int(shm_pool_id);
}

Owner_shm_pool_collection::Lockable_shm_pool::Mutex& Owner_shm_pool_collection::Lockable_shm_pool::get_mutex()
{
  return m_mutex;
}

} // namespace ipc::shm::arena_lend
