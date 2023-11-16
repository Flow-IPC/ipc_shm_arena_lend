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
#include "ipc/shm/arena_lend/divisible_shm_pool.hpp"
#include "ipc/util/util_fwd.hpp"
#include <flow/util/util.hpp>

namespace ipc::shm::arena_lend
{

/**
 * Abstract class representing a shared memory pool collection for owners, which are entities (e.g., processes)
 * that have access to a memory manager. In other words, they can allocate and deallocate memory. An event listener
 * can be registered to obtain notifications of key events in the collection, which are currently changes in the
 * shared memory pool set.
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
   * Alias for the functor that creates a new shared memory object name. This functor must be thread-safe.
   * To conform with shared object naming convention, the name must:
   * 1. Start with a slash ('/') character.
   * 2. Have [1, NAME_MAX - 2] non-slash characters, following the initial slash character, NAME_MAX = 255.
   *
   * The final character string used will be null-terminated.
   *
   * Most importantly the pool name must be unique across all time between boots.
   *
   * The argument is an unsigned integer ID that is guaranteed unique across *all* pools across *all* time between
   * boots. It should, though formally is not required to, be encoded into the returned name.
   *   - It provides excellent uniqueness which one might as well take advantage of instead of having to
   *     come up with one's own source of unique pool names.
   *   - It can help debugging/clarity, as glancing at the name identifies the pool within the
   *     shm::arena_lend system as well.
   */
  using Shm_object_name_generator = std::function<std::string(pool_id_t)>;

  /**
   * Abstract class representing listeners for events in the collection.
   */
  class Event_listener
  {
  public:
    /// Destructor.
    virtual ~Event_listener() = 0;

    /**
     * Notification for created shared memory pools.
     *
     * @param shm_pool The shared memory pool that was created.
     */
    virtual void notify_created_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool) = 0;
    /**
     * Notification for removed shared memory pools.
     *
     * @param shm_pool The shared memory pool that was deregistered.
     * @param removed_shared_memory Whether the underlying shared memory was actually removed.
     */
    virtual void notify_removed_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool, bool removed_shared_memory) = 0;
  }; // class Event_listener

  /**
   * Constructor.
   *
   * @param logger For logging purposes.
   * @param id Identifier for the collection.
   * @param memory_manager The memory allocator.
   * @param name_generator The shared object name generator.
   * @param permissions The shared memory object file permissions when one is created.
   */
  Owner_shm_pool_collection(flow::log::Logger* logger,
                            Collection_id id,
                            const std::shared_ptr<Memory_manager>& memory_manager,
                            Shm_object_name_generator&& name_generator,
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
   * Adds a listener to be notified for collection events.
   *
   * @param listener A borrowed handle to the listener.
   *
   * @return Whether the listener was added successfully, which would generally only fail if the listener was
   *         already previously added.
   */
  bool add_event_listener(Event_listener* listener);
  /**
   * Removes a listener from being notified for collection events.
   *
   * @param listener A borrowed handle to the listener.
   *
   * @return Whether the listener was removed successfully, which would generally only fail if the listener was
   *         not found.
   */
  bool remove_event_listener(Event_listener* listener);

protected:
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

  /// A shared memory pool that also contains a mutex for serializing access.
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
   * Returns a new shared memory object name, which should be unique.
   *
   * @param shm_pool_id Recently generated ultra-unique ID.
   * @return See above.
   */
  inline std::string generate_shm_object_name(pool_id_t shm_pool_id) const;

  /**
   * Performs an allocation backed by shared memory, uses the allocation to construct an object and returns a shared
   * pointer to this object. When the shared pointer has no more references, it will be destructed by this pool
   * collection instance. The start() method must be executed prior to any calls to this.
   *
   * @tparam T The object type to be created.
   * @tparam Deleter A copy-constructible class containing operator() that performs destruction of the memory.
   * @tparam Args The parameter types that are passed to the constructor of T.
   * @param allocation Allocated shared memory.
   * @param deleter A deleter instance that will delete T (typically constructed on the stack as it will be copied).
   * @param args The arguments passed to the constructor of T.
   *
   * @return A shared pointer to an object created in shared memory.
   */
  template <typename T, typename Deleter, typename... Args>
  std::shared_ptr<T> construct_helper(void* allocation, Deleter&& deleter, Args&&... args)
  {
    if (allocation == nullptr)
    {
      return nullptr;
    }

    T* obj = new(allocation) T(std::forward<Args>(args)...);
    return std::shared_ptr<T>(obj, std::forward<Deleter>(deleter));
  }

private:
  /// Multi-reader, single-writer mutex.
  using Mutex = flow::util::Mutex_shared_non_recursive;
  /// Single-writer lock for the mutex.
  using Write_lock = flow::util::Lock_guard<Mutex>;
  /// Multi-reader lock for the mutex.
  using Read_lock = flow::util::Shared_lock_guard<Mutex>;

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
  /// Shared object name generator.
  Shm_object_name_generator m_name_generator;
  /// The shared memory object file permissions when one is created.
  const util::Permissions m_permissions;
  /// Mutex for listener map.
  mutable Mutex m_event_listeners_mutex;
  /// Event_listener container for notifications.
  std::set<Event_listener*> m_event_listeners;
}; // class Owner_shm_pool_collection

std::shared_ptr<Memory_manager> Owner_shm_pool_collection::get_memory_manager() const
{
  return m_memory_manager;
}

std::string Owner_shm_pool_collection::generate_shm_object_name(pool_id_t shm_pool_id) const
{
  return m_name_generator(shm_pool_id);
}

Owner_shm_pool_collection::Lockable_shm_pool::Mutex& Owner_shm_pool_collection::Lockable_shm_pool::get_mutex()
{
  return m_mutex;
}

} // namespace ipc::shm::arena_lend
