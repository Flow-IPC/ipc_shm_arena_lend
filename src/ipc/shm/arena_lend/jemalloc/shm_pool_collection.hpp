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
#include "ipc/shm/arena_lend/jemalloc/memory_manager.hpp"
#include <unordered_map>

namespace ipc::shm::arena_lend::jemalloc
{

/**
 * A collection of shared memory pools for owners using jemalloc as the memory manager. It creates and manages
 * the arenas that trigger the generation of the shared memory pools. In addition, it provides the following
 * features:
 * 1. Object construction
 *    Instead of just raw pointers, a shared pointer object can be created to allow for auto-deletion to the
 *    pool collection. Note that custom deleters of the object are not supported here as it is used by the
 *    facility itself.
 * 2. Thread cache
 *    Memory can be managed in a thread cache to potentially improve performance due to lock avoidance.
 * 3. Start is required before utilization
 *    This is necessary as an initialization aspect.
 */
class Shm_pool_collection :
  public Owner_shm_pool_collection,
  public std::enable_shared_from_this<Shm_pool_collection>
{
public:
  /// Alias for the arena identifier.
  using Arena_id = Memory_manager::Arena_id;

  /**
   * Creates an instance of this class. We require the use of a shared pointer, because the construct() interfaces
   * require a handle for their destruction operations.
   *
   * @param logger Used for logging purposes.
   * @param id Identifier for the collection.
   * @param memory_manager The memory allocator.
   * @param name_generator Shared object name generator.
   * @param permissions The shared memory object file permissions when one is created.
   *
   * @return Upon success, a shared pointer to an instance of this class; otherwise, an empty shared pointer.
   *
   * @see construct()
   */
  static std::shared_ptr<Shm_pool_collection> create(
    flow::log::Logger* logger,
    Collection_id id,
    const std::shared_ptr<Memory_manager>& memory_manager,
    Shm_object_name_generator&& name_generator,
    const util::Permissions& permissions);
  /**
   * Destructor. This destroys any previously allocated arenas along with their shared memory pools.
   */
  virtual ~Shm_pool_collection() override;
  /**
   * Creates segregated memory areas. This must be executed before any allocations take place.
   *
   * @param arenas The number of arenas to create.
   *
   * @return Whether we did not previously start and the arenas were successfully created.
   *
   * @warning Not thread-safe.
   */
  bool start(unsigned int arenas = 1);

  /**
   * Returns the set of arenas ids, which is constant after start(). The start() method must be executed prior to
   * any calls to this.
   *
   * @return See above.
   */
  inline const std::set<Arena_id>& get_arena_ids() const;

  /**
   * Performs an allocation backed by shared memory, uses the allocation to construct an object and returns a shared
   * pointer to this object. When the shared pointer has no more references, it will be destructed by this pool
   * collection instance. The start() method must be executed prior to any calls to this.
   *
   * @tparam T The object type to be created.
   * @tparam Args The parameter types that are passed to the constructor of T.
   * @param arena_id The arena to perform the allocation.
   * @param use_cache Whether thread caching should be used.
   * @param args The arguments passed to the constructor of T.
   *
   * @return A shared pointer to an object created in shared memory.
   */
  template <typename T, typename... Args>
  std::shared_ptr<T> construct_in_arena(Arena_id arena_id, bool use_cache, Args&&... args)
  {
    if (use_cache)
    {
      auto thread_cache = get_or_create_thread_cache(arena_id);
      const auto thread_cache_id = thread_cache->get_thread_cache_id();
      return construct_helper<T>(allocate(sizeof(T), arena_id, thread_cache_id),
                                 Object_deleter_cache(std::move(thread_cache)),
                                 std::forward<Args>(args)...);
    }
    // else
    return construct_helper<T>(allocate(sizeof(T), arena_id),
                               Object_deleter_no_cache(shared_from_this(), arena_id),
                               std::forward<Args>(args)...);
  }

  /**
   * Performs an allocation backed by shared memory, uses the allocation to construct an object and returns a shared
   * pointer to this object. When the shared pointer has no more references, it will be destructed by this pool
   * collection instance. The start() method must be executed prior to any calls to this. If there are multiple
   * arenas, the lowest identifier will be chosen.
   *
   * @tparam T The object type to be created.
   * @tparam Args The parameter types that are passed to the constructor of T.
   * @param use_cache Whether thread caching should be used.
   * @param args The arguments passed to the constructor of T.
   *
   * @return A shared pointer to an object created in shared memory.
   *
   * @see construct_in_arena(Arena_id, Args&&...)
   */
  template <typename T, typename... Args>
  std::shared_ptr<T> construct_maybe_thread_cached(bool use_cache, Args&&... args)
  {
    assert(m_started);

    // Use the first arena
    auto iter = m_arenas.begin();
    assert(iter != m_arenas.end());
    return construct_in_arena<T>(*iter, use_cache, std::forward<Args>(args)...);
  }

  /**
   * Performs an allocation backed by shared memory. The start() method must be executed prior to any calls to
   * this. If there are multiple arenas, the lowest identifier will be chosen. No thread cache will be used with
   * this allocation.
   *
   * @param size The allocation size, which must be greater than zero.
   *
   * @return Upon success, a non-null pointer to the base address of the allocation; otherwise, nullptr.
   */
  virtual void* allocate(std::size_t size) override;
  /**
   * Performs an allocation backed by shared memory. The start() method must be executed prior to any calls to
   * this. No thread cache will be used with this allocation.
   *
   * @param size The allocation size, which must be greater than zero.
   * @param arena_id The arena in which to perform the allocation.
   *
   * @return Upon success, a non-null pointer to the base address of the allocation; otherwise, nullptr.
   */
  void* allocate(std::size_t size, Arena_id arena_id);
  /**
   * Deallocates memory back to the memory manager. If there are multiple arenas, the lowest identifier will be
   * chosen. No thread cache must have been used.
   *
   * @param address The address to be deallocated, which must be non-null.
   */
  virtual void deallocate(void* address) override;
  /**
   * Deallocates memory back to the memory manager. No thread cache must have been used.
   *
   * @param address The address to be deallocated, which must be non-null.
   * @param arena_id The arena in which to perform the deallocation, which must match the arena in which the
   *                 allocation was originally performed.
   */
  void deallocate(void* address, Arena_id arena_id);

  /**
   * Removes the collection from the current thread's thread cache.
   */
  inline void remove_current_thread_cache();
  /**
   * Flushes all the thread caches for the current thread.
   */
  static inline void flush_current_thread_cache_all();

protected:
  /// Alias for the thread cache identifier.
  using Thread_cache_id = Memory_manager::Thread_cache_id;

  /**
   * Holder of thread cache information used to allocate and deallocate objects. A thread cache must be
   * emptied of any objects belonging to an arena prior to an arena being destroyed. We associate a thread
   * cache to a combination of collection and arena.
   *
   * We prevent copying of this class as destruction has removal of the thread cache id, which cannot be
   * performed safely twice.
   */
  class Thread_cache :
    private boost::noncopyable
  {
  public:
    /**
     * Constructor.
     *
     * @param owner The collection that this was created in.
     * @param arena_id The jemalloc arena identifier that the thread cache will be used for.
     */
    Thread_cache(const std::shared_ptr<Shm_pool_collection>& owner, Arena_id arena_id);
    /**
     * Destructor. Returns the thread cache back to the memory manager.
     */
    ~Thread_cache();

    /**
     * Returns the thread cache identifier.
     *
     * @return See above.
     */
    inline Thread_cache_id get_thread_cache_id() const;
    /**
     * Returns the collection that this cache was created in.
     *
     * @return See above.
     */
    inline std::shared_ptr<Shm_pool_collection> get_owner() const;
    /**
     * Returns the arena that this cache will be used for.
     *
     * @return See above.
     */
    inline Arena_id get_arena_id() const;

  private:
    /// The thread cache identifier.
    Thread_cache_id m_thread_cache_id;
    /// The collection that this cache was created in.
    std::shared_ptr<Shm_pool_collection> m_owner;
    /// The arena that this cache will be used for.
    Arena_id m_arena_id;
  }; // class Thread_cache

  /**
   * Store for thread local data, which contains thread memory cache information. The thread memory cache
   * entries consists of prior allocations that were deallocated. Keeping this memory in a cache allows
   * allocations without locking, so it is used as a performance enhancement.
   */
  class Thread_local_data
  {
  public:
    /**
     * Retrieves a thread cache by the collection and the arena it belongs to.
     *
     * @param arena_id The arena in which the allocations reside.
     *
     * @return If found, the thread cache; otherwise, empty shared pointer.
     */
    inline std::shared_ptr<Thread_cache> get_cache(Arena_id arena_id) const;
    /**
     * Inserts a thread cache uniquely indexed by the arena in which it is serving.
     *
     * @param thread_cache The thread cache to insert.
     *
     * @return Whether the thread cache was successfully inserted or it was previously inserted.
     */
    bool insert_cache(const std::shared_ptr<Thread_cache>& thread_cache);
    /**
     * Removes any cache belonging to the given collection.
     *
     * @param owner The collection in which to remove the caches of.
     *
     * @return Whether there were any caches removed.
     */
    bool remove_caches_by_owner(const std::shared_ptr<Shm_pool_collection>& owner);
    /**
     * Flushes all the caches.
     */
    void flush_all_caches();

  private:
    /// The cache mappings of arena id -> Thread cache shared pointer.
    std::unordered_map<Arena_id, std::shared_ptr<Thread_cache>> m_thread_cache_map;
  }; // class Thread_local_data

  /**
   * Constructor.
   *
   * @param logger Used for logging purposes.
   * @param id Identifier for the collection.
   * @param memory_manager The memory allocator.
   * @param name_generator Shared object name generator.
   * @param permissions The shared memory object file permissions when one is created.
   */
  Shm_pool_collection(flow::log::Logger* logger,
                      Collection_id id,
                      const std::shared_ptr<Memory_manager>& memory_manager,
                      Shm_object_name_generator&& name_generator,
                      const util::Permissions& permissions);

  /**
   * Returns the memory manager.
   *
   * @return See above.
   */
  inline std::shared_ptr<Memory_manager> get_jemalloc_memory_manager() const;

  /*
   * Performs an allocation backed by shared memory, uses the allocation to construct an object and returns a shared
   * pointer to this object. When the shared pointer has no more references, it will be destructed by this pool
   * collection instance. The start() method must be executed prior to any calls to this.
   *
   * @tparam T The object type to be created.
   * @param Args The parameter types that are passed to the constructor of T.
   * @param arena_id The arena to perform the allocation.
   * @param thread_cache_id The thread cache id to associate the allocation with.
   * @param args The arguments passed to the constructor of T.
   *
   * @return A shared pointer to an object created in shared memory.
   */
  /*
  template <typename T, typename... Args>
  std::shared_ptr<T> construct_using_cache(const std::shared_ptr<Thread_cache>& thread_cache, Args&&... args)
  {
    return construct_helper<T>(allocate(sizeof(T), thread_cache->get_arena_id(), thread_cache->get_thread_cache_id()),
                               Object_deleter(thread_cache),
                               std::forward<Args>(args)...);
  }
  */

  /**
   * Retrieves or creates a thread cache for allocation purposes.
   *
   * @param arena_id The arena id to operate on.
   *
   * @return A thread cache pertaining to the specified arena id.
   */
  std::shared_ptr<Thread_cache> get_or_create_thread_cache(Arena_id arena_id);
  /**
   * Creates a thread cache in the memory manager.
   *
   * @return The identifier for the created thread cache.
   */
  inline Thread_cache_id create_thread_cache() const;
  /**
   * Destroys a thread cache in the memory manager.
   *
   * @param thread_cache_id The identifier for the thread cache to be destroyed.
   */
  inline void destroy_thread_cache(Thread_cache_id thread_cache_id) const;

  /**
   * Performs an allocation backed by shared memory. The start() method must be executed prior to any calls to
   * this. Note that this is not currenly made public due to it being error prone in usage, as deallocation must
   * be paired appropriately with the proper thread cache id.
   *
   * @param size The allocation size, which must be greater than zero.
   * @param arena_id The arena in which to perform the allocation.
   * @param thread_cache_id The thread_cache in which to perform the allocation.
   *
   * @return Upon success, a non-null pointer to the base address of the allocation; otherwise, nullptr.
   */
  void* allocate(std::size_t size, Arena_id arena_id, Thread_cache_id thread_cache_id);
  /**
   * Deallocates memory back to the memory manager. Note that this is not currenly made public due to it being
   * error prone in usage as allocation must be paired appropriately with the proper thread cache id.
   *
   * @param address The address to be deallocated, which must be non-null.
   * @param arena_id The arena in which to perform the deallocation, which must match the arena in which the
   *                 allocation was originally performed.
   * @param thread_cache_id The thread_cache in which to perform the deallocation, which must match the thread cache
   *                        in which the allocation was originally performed.
   */
  void deallocate(void* address, Arena_id arena_id, Thread_cache_id thread_cache_id);

  /**
   * Creates a shared memory pool.
   *
   * @param address The desired location to map this memory pool, which can be null for system specification.
   * @param size The size of the memory pool to be created.
   * @param alignment The value to align the resulting address on, which is generally a multiple of page size.
   * @param zero Output parameter indicating whether the contents have been zeroed.
   * @param commit Whether the system should designate the pages to be readable and writable (marked active and
   *               can be put into physical memory). If they system is set to overcommit memory, commit is always
   *               enabled. The value will be updated as an output parameter to indicate whether the memory was
   *               committed.
   * @param arena_id The memory area that the pool will be placed into.
   *
   * @return Upon success, the created memory pool; otherwise, nullptr.
   */
  void* create_shm_pool(void* address,
                        std::size_t size,
                        std::size_t alignment,
                        bool* zero,
                        bool* commit,
                        Arena_id arena_id);
  /**
   * Decide whether to remove an unneeded memory pool or preserve it for future use.
   *
   * @param address The memory pool to remove.
   * @param size The size of the memory pool to be removed.
   * @param committed Whether the memory pool is currently committed to physical memory.
   * @param arena_id The memory area that the pool resides in.
   *
   * @return Whether the pages in the pool were successfully decommitted from physical memory.
   */
  bool optional_remove_shm_pool(void* address,
                                std::size_t size,
                                bool committed,
                                Arena_id arena_id);
  /**
   * Removes a memory pool.
   *
   * @param address The memory pool to remove.
   * @param size The size of the memory pool to be removed.
   * @param committed Whether the memory pool is currently committed to physical memory.
   * @param arena_id The memory area that the pool resides in.
   *
   * @return Whether the pages in the pool were successfully decommitted from physical memory.
   */
  bool remove_shm_pool(void* address,
                       std::size_t size,
                       bool committed,
                       Arena_id arena_id);
  /**
   * Mark memory pages as readable and writable.
   *
   * @param address The memory pool where the pages reside in.
   * @param size The size of the memory pool.
   * @param offset The offset from the address to commit.
   * @param length The length of the pages to commit.
   * @param arena_id The memory area that the pages reside in.
   *
   * @return Whether the pages were successfully committed.
   */
  bool commit_memory_pages(void* address,
                           std::size_t size,
                           std::size_t offset,
                           std::size_t length,
                           Arena_id arena_id);
  /**
   * Mark memory pages as inaccessible (non-writable and non-readable).
   *
   * @param address The memory pool where the pages reside in.
   * @param size The size of the memory pool.
   * @param offset The offset from the address to decommit.
   * @param length The length of the pages to decommit.
   * @param arena_id The memory area that the pages reside in.
   *
   * @return Whether the pages were successfully decommitted.
   */
  bool decommit_memory_pages(void* address,
                             std::size_t size,
                             std::size_t offset,
                             std::size_t length,
                             Arena_id arena_id);
  /**
   * Purges memory pages. If the application accesses a page purged after this call, a page fault occurs.
   *
   * @param address The memory pool where the pages reside in.
   * @param size The size of the memory pool.
   * @param offset The offset from the address to purge.
   * @param length The length of the pages to purge.
   * @param arena_id The memory area that the pages reside in.
   *
   * @return Whether the pages were successfully purged.
   */
  bool purge_forced_memory_pages(void* address,
                                 std::size_t size,
                                 std::size_t offset,
                                 std::size_t length,
                                 Arena_id arena_id);
  /**
   * Returns whether memory pages can be logically split.
   *
   * @param address The memory location.
   * @param size The size of the region.
   * @param size_a The proposed size of the lower addressed region.
   * @param size_b The proposed size of the higher addressed region.
   * @param committed Whether the memory pool is currently committed to physical memory.
   * @param arena_id The memory area that the pages reside in.
   *
   * @return Whether the pages are allowed to split.
   */
  bool split_memory_pages(const void* address,
                          size_t size,
                          size_t size_a,
                          size_t size_b,
                          bool committed,
                          Arena_id arena_id);
  /**
   * Returns whether memory pages can be logically merged.
   *
   * @param address_a The first memory pool location.
   * @param size_a The size of the first memory pool.
   * @param address_b The second memory pool location.
   * @param size_b The size of the second memory pool.
   * @param committed Whether the memory pool is currently committed to physical memory.
   * @param arena_id The memory area that the pages reside in.
   *
   * @return Whether the pages are allowed to merge.
   */
  bool merge_memory_pages(const void* address_a,
                          std::size_t size_a,
                          const void* address_b,
                          std::size_t size_b,
                          bool committed,
                          Arena_id arena_id);

  /**
   * Returns the result of adding an offset to an address.
   *
   * @param address The original address.
   * @param offset The offset to add.
   *
   * @return See above.
   */
  static inline void* add_offset(void* address, std::size_t offset);

  /**
   * Handles callback from shared pointer destruction. The result should be a destruction of the object and return
   * of the underlying memory back to the allocating memory manager without the use of a thread cache.
   */
  class Object_deleter_no_cache
  {
  public:
    /**
     * Constructor. No thread cache is used here.
     *
     * @param pool_collection The object that allocated the underlying memory from which it should be returned.
     * @param arena_id The arena in which the allocation was performed.
     */
    Object_deleter_no_cache(std::shared_ptr<Shm_pool_collection>&& pool_collection, Arena_id arena_id);

    /**
     * Release callback executed by shared pointer destruction. The destructor for the object will be called
     * and the underlying memory will be returned to the memory manager.
     *
     * @tparam T The object type.
     * @param p The object held by the shared pointer.
     */
    template <typename T>
    void operator()(T* p)
    {
      /* Call object's destructor (works for primitives).
       * Key context: an allocator-equipped T (usually STL-compliant container) here will (in T::~T())
       * call-through to m_pool_collection->deallocate() for each buffer that T had allocated for itself
       * (e.g., for vector<> internal buffer; for list<> the various nodes). In the process more destructors
       * may be invoked which would quite possibly do more dtor calling and thus deallocate()ing. And so on.
       * Hence the present operator()() is called at just the outer layer, then the inner deallocations (if any)
       * happen, and then we call deallocate() on the outer object's memory, last-thing, just below. */
      p->~T();
      m_pool_collection->deallocate(p, m_arena_id);
    }

    /**
     * Returns the creator pool collection.
     *
     * @return See above.
     */
    inline const std::shared_ptr<Shm_pool_collection>& get_pool_collection() const;
    /**
     * Returns the creator arena id.
     *
     * @return See above.
     */
    inline Arena_id get_arena_id() const;

  private:
    /// The object that allocated the underlying memory from which it should be returned.
    const std::shared_ptr<Shm_pool_collection> m_pool_collection;
    /// The arena in which the allocation was performed.
    Arena_id m_arena_id;
  }; // class Object_deleter_no_cache

  /**
   * Handles callback from shared pointer destruction. The result should be a destruction of the object and return
   * of the underlying memory back to the allocating memory manager to a thread cache.
   */
  class Object_deleter_cache
  {
  public:
    /**
     * Constructor.
     *
     * @param thread_cache The thread cache to associate the allocation with.
     */
    Object_deleter_cache(std::shared_ptr<Thread_cache>&& thread_cache);

    /**
     * Release callback executed by shared pointer destruction. The destructor for the object will be called
     * and the underlying memory will be returned to the memory manager.
     *
     * @tparam T The object type.
     * @param p The object held by the shared pointer.
     */
    template <typename T>
    void operator()(T* p)
    {
      // Call object's destructor (works for primitives). See more key notes in Object_deleter_no_cache comment.
      p->~T();
      m_thread_cache->get_owner()->deallocate(p,
                                              m_thread_cache->get_arena_id(),
                                              m_thread_cache->get_thread_cache_id());
    }

    /**
     * Returns the thread cache id holder, which may not contain any thread cache id.
     *
     * @return See above.
     */
    inline const std::shared_ptr<Thread_cache>& get_thread_cache() const;

  private:
    /// The thread cache id in which the allocation was performed or no value if no cache was used.
    const std::shared_ptr<Thread_cache> m_thread_cache;
  }; // class Object_deleter_cache

private:
  /**
   * Jemalloc callback when a memory pool is requested to be created.
   *
   * @param extent_hooks The set of callbacks containing this callback.
   * @param address The desired location to map this memory pool, which can be null for system specification.
   * @param size The size of the memory pool to be created.
   * @param alignment The value to align the resulting address on, which is generally a multiple of page size.
   * @param zero Output parameter indicating whether the contents have been zeroed.
   * @param commit Whether the system should designate the pages to be readable and writable (marked active and
   *               can be put into physical memory). If they system is set to overcommit memory, commit is always
   *               enabled. The value will be updated as an output parameter to indicate whether the memory was
   *               committed.
   * @param arena_id The memory area that the pool will be placed into.
   *
   * @return Upon success, the created memory pool; otherwise, nullptr.
   */
  static void* create_shm_pool_handler(extent_hooks_t* extent_hooks,
                                       void* address,
                                       std::size_t size,
                                       std::size_t alignment,
                                       bool* zero,
                                       bool* commit,
                                       unsigned arena_id);
  /**
   * Jemalloc callback when a memory pool is no longer needed.
   *
   * @param extent_hooks The set of callbacks containing this callback.
   * @param address The memory pool to remove.
   * @param size The size of the memory pool to be removed.
   * @param committed Whether the memory pool is currently committed to physical memory.
   * @param arena_id The memory area that the pool resides in.
   *
   * @return Whether the memory was NOT unmapped.
   */
  static bool optional_remove_shm_pool_handler(extent_hooks_t* extent_hooks,
                                               void* address,
                                               std::size_t size,
                                               bool committed,
                                               unsigned arena_id);
  /**
   * Jemalloc callback when a memory pool is instructed to be removed.
   *
   * @param extent_hooks The set of callbacks containing this callback.
   * @param address The memory pool to remove.
   * @param size The size of the memory pool to be removed.
   * @param committed Whether the memory pool is currently committed to physical memory.
   * @param arena_id The memory area that the pool resides in.
   */
  static void remove_shm_pool_handler(extent_hooks_t* extent_hooks,
                                      void* address,
                                      std::size_t size,
                                      bool committed,
                                      unsigned arena_id);
  /**
   * Jemalloc callback when a contiguous set of memory pages are instructed to be marked as readable and writable.
   *
   * @param extent_hooks The set of callbacks containing this callback.
   * @param address The memory pool where the pages reside in.
   * @param size The size of the memory pool.
   * @param offset The offset from the address to commit.
   * @param length The length of the pages to commit.
   * @param arena_id The memory area that the pages reside in.
   *
   * @return Whether the pages were NOT successfully committed.
   */
  static bool commit_memory_pages_handler(extent_hooks_t* extent_hooks,
                                          void* address,
                                          std::size_t size,
                                          std::size_t offset,
                                          std::size_t length,
                                          unsigned arena_id);
  /**
   * Jemalloc callback when a contiguous set of memory pages are instructed to be marked as inaccessible
   * (non-writable and non-readable).
   *
   * @param extent_hooks The set of callbacks containing this callback.
   * @param address The memory pool where the pages reside in.
   * @param size The size of the memory pool.
   * @param offset The offset from the address to decommit.
   * @param length The length of the pages to decommit.
   * @param arena_id The memory area that the pages reside in.
   *
   * @return Whether the pages were NOT successfully decommitted.
   */
  static bool decommit_memory_pages_handler(extent_hooks_t* extent_hooks,
                                            void* address,
                                            std::size_t size,
                                            std::size_t offset,
                                            std::size_t length,
                                            unsigned arena_id);
  /**
   * Jemalloc callback to purge a contiguous set of memory pages from physical memory, which allows the system
   * to reclaim the pages for use (when needed).
   *
   * @param extent_hooks The set of callbacks containing this callback.
   * @param address The memory pool where the pages reside in.
   * @param size The size of the memory pool.
   * @param offset The offset from the address to purge.
   * @param length The length of the pages to purge.
   * @param arena_id The memory area that the pages reside in.
   *
   * @return Whether the pages were NOT successfully purged.
   */
  static bool purge_forced_memory_pages_handler(extent_hooks_t* extent_hooks,
                                                void* address,
                                                std::size_t size,
                                                std::size_t offset,
                                                std::size_t length,
                                                unsigned arena_id);
  /**
   * Jemalloc callback to logically split a contiguous set of memory pages into two memory pools (only as
   * perceived by jemalloc). This will currently always return false (i.e., allowed) as the pool must be
   * within a single originally created pool.
   *
   * @param extent_hooks The set of callbacks containing this callback.
   * @param address The memory pool where the pages reside in.
   * @param size The size of the memory pool.
   * @param size_a The size of the first memory pool.
   * @param size_b The size of the second memory pool.
   * @param committed Whether the memory pool is currently committed to physical memory.
   * @param arena_id The memory area that the pages reside in.
   *
   * @return Whether the pages are NOT allowed to split.
   */
  static bool split_memory_pages_handler(extent_hooks_t* extent_hooks,
                                         void* address,
                                         std::size_t size,
                                         std::size_t size_a,
                                         std::size_t size_b,
                                         bool committed,
                                         unsigned arena_id);
  /**
   * Jemalloc callback to logically merge two memory pools into one (as perceived by jemalloc).
   *
   * @param extent_hooks The set of callbacks containing this callback.
   * @param address_a The first memory pool location.
   * @param size_a The size of the first memory pool.
   * @param address_b The second memory pool location.
   * @param size_b The size of the second memory pool.
   * @param committed Whether the memory pool is currently committed to physical memory.
   * @param arena_id The memory area that the pages reside in.
   *
   * @return Whether the pages are NOT allowed to merge.
   */
  static bool merge_memory_pages_handler(extent_hooks_t* extent_hooks,
                                         void* address_a,
                                         std::size_t size_a,
                                         void* address_b,
                                         std::size_t size_b,
                                         bool committed,
                                         unsigned arena_id);

  /**
   * Helper function to compute the pool and offset within the pool to perform a subsequent operation. Executes
   * checks to ensure that the input is valid.
   *
   * @param address The memory pool where the pages reside in.
   * @param size The size of the memory pool.
   * @param offset The offset from the address, which must be page aligned.
   * @param length The length of the pages, which must be page aligned.
   * @param use_case Description for logging.
   * @param pool If the result is true, the resulting shared memory pool.
   * @param pool_offset If the result is true, the difference between address + offset with the pool base address.
   *
   * @return Whether the input was valid and a pool matching the address was found.
   */
  bool compute_pool_and_offset(void* address,
                               std::size_t size,
                               std::size_t offset,
                               std::size_t length,
                               const std::string& use_case,
                               std::shared_ptr<Shm_pool>& pool,
                               std::size_t& pool_offset) const;

  /**
   * Whether the collection has been started. Primarily used as a safety-check measure.
   * @note During arena creation performed in start(), jemalloc callback handlers may be called, so using arena
   *       count as a (replacement) safety check may lead to an assertion.
   */
  bool m_started;
  /// The segregated memory areas.
  std::set<Memory_manager::Arena_id> m_arenas;
  /// The callback functions for jemalloc.
  Memory_manager::Extent_hooks_wrapper<Shm_pool_collection> m_extent_hooks_wrapper;

  /**
   * Thread local data - Each thread will create one at construction and destroy it at destruction.
   * @see class Thread_local_data
   */
  static thread_local Thread_local_data s_thread_local_data;
}; // class Shm_pool_collection

std::shared_ptr<Memory_manager> Shm_pool_collection::get_jemalloc_memory_manager() const
{
  return std::static_pointer_cast<Memory_manager>(get_memory_manager());
}

const std::set<Shm_pool_collection::Arena_id>& Shm_pool_collection::get_arena_ids() const
{
  assert(m_started);
  return m_arenas;
}

Shm_pool_collection::Thread_cache_id Shm_pool_collection::create_thread_cache() const
{
  return get_jemalloc_memory_manager()->create_thread_cache();
}

void Shm_pool_collection::destroy_thread_cache(Thread_cache_id thread_cache_id) const
{
  get_jemalloc_memory_manager()->destroy_thread_cache(thread_cache_id);
}

void Shm_pool_collection::remove_current_thread_cache()
{
  s_thread_local_data.remove_caches_by_owner(shared_from_this());
}

void Shm_pool_collection::flush_current_thread_cache_all()
{
  s_thread_local_data.flush_all_caches();
}

// Static method
void* Shm_pool_collection::add_offset(void* address, std::size_t offset)
{
  return reinterpret_cast<void*>(reinterpret_cast<uint8_t*>(address) + offset);
}

const std::shared_ptr<Shm_pool_collection>&
Shm_pool_collection::Object_deleter_no_cache::get_pool_collection() const
{
  return m_pool_collection;
}

Shm_pool_collection::Arena_id Shm_pool_collection::Object_deleter_no_cache::get_arena_id() const
{
  return m_arena_id;
}

const std::shared_ptr<Shm_pool_collection::Thread_cache>&
Shm_pool_collection::Object_deleter_cache::get_thread_cache() const
{
  return m_thread_cache;
}

Shm_pool_collection::Thread_cache_id Shm_pool_collection::Thread_cache::get_thread_cache_id() const
{
  return m_thread_cache_id;
}

std::shared_ptr<Shm_pool_collection> Shm_pool_collection::Thread_cache::get_owner() const
{
  return m_owner;
}

Shm_pool_collection::Arena_id Shm_pool_collection::Thread_cache::get_arena_id() const
{
  return m_arena_id;
}

std::shared_ptr<Shm_pool_collection::Thread_cache>
Shm_pool_collection::Thread_local_data::get_cache(Arena_id arena_id) const
{
  const auto& iter = m_thread_cache_map.find(arena_id);
  if (iter == m_thread_cache_map.end())
  {
    return nullptr;
  }

  return iter->second;
}

} // namespace ipc::shm::arena_lend::jemalloc
