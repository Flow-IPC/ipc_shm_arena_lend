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

#include "ipc/shm/arena_lend/jemalloc/shm_pool_collection.hpp"
#include "ipc/shm/arena_lend/jemalloc/jemalloc_fwd.hpp"
#include "ipc/shm/arena_lend/shm_pool_offset_ptr.hpp"
#include "ipc/shm/arena_lend/shm_pool_repository_singleton.hpp"
#include "ipc/shm/arena_lend/arena_lend_fwd.hpp"
#include "ipc/shm/stl/arena_activator.hpp"

namespace ipc::shm::arena_lend::jemalloc
{

/**
 * Wraps around a Shm_pool_collection to provide IPC functionality. Namely, the following are added:
 *   -# Managing subscriptions (listeners) - Subscribers will be informed initially of the current SHM pools as well
 *      any future changes in the SHM pools. These pools are necessary to distribute to borrowers for opening the
 *      shared memory files and creating objects that lie within those pools.
 *   -# Maintaining the current SHM pools - This is needed to properly notify SHM pools to subscribers at registration.
 *   -# Utility method to obtain the arena instance identifier from an object constructed within an arena.
 *   -# Single jemalloc arena that will be created at construction.
 *   -# Ability to be used in an ipc::shm::stl::Stateless_allocator.
 *
 * To put it into context:
 *   - Shm_pool_collection is technically sufficient to achieve no-fuss garbage-collected allocation, at least of
 *     PoD-typed objects, in shared memory. However it is not sufficient to be able to then reasonably easily let other
 *     processes/entities IPC-receive such object; or to allocate STL-compliant non-PoD-typed objects as facilitated
 *     by stl::Stateless_allocator. To get those abilities you will need Ipc_arena. As an object allocated in SHM
 *     but not possible to share with other processes is not too useful (why not just use the heap then?), generally
 *     speaking an Ipc_arena is essential for most users.
 *   - Using Ipc_arena for those capabilities is necessary but not sufficient. Namely you'll also need a
 *     ipc::session::shm::arena_lend::jemalloc::Shm_session on each side to go with the Ipc_arena on the
 *     owner (allocating) side.
 */
class Ipc_arena :
  public Shm_pool_collection
{
public:
  /**
   * Fancy pointer type used by ipc::shm::stl::Stateless_allocator.
   *
   * @tparam Pointed_type The type contained within the pointer.
   */
  template <typename Pointed_type>
  using Pointer = Shm_pool_offset_ptr<Pointed_type, Owner_shm_pool_repository_singleton>;

  /**
   * First-class handle -- a `shared_ptr` of some kind by definition -- to objects `construct<>()`ed by this arena
   * class. While as of this writing no separately documented concept for SHM arena class (implemented, e.g., by
   * this Ipc_arena; or its counterpart in shm::classic, classic::Pool_arena) exists, certain important
   * APIs do take `Shm_arena`s like us and formally require it to contain `Handle`, which must be *a* `shared_ptr`
   * type (`std::`, `bipc::`, or in theory some other impl), and be the type returned by `Shm_arena::construct<>()`
   * as well as `Shm_session::borrow()`.  A key such API is transport::struc::shm::Builder.
   *
   * ### Rationale ###
   * One benefit of requiring `Handle` is the ability for a SHM-provider (like shm::arena_lend::jemalloc) to
   * choose the particular `shared_ptr` impl desired. (This could even be a template parameter knob.) A
   * squishier alleged benefit is the stylistic attention it calls to construct() or session `borrow()` returning
   * a cross-process SHM handle, as opposed to a normal heap-deleting ref-counting pointer.
   */
  template <typename Pointed_type>
  using Handle = std::shared_ptr<Pointed_type>;

  /// Short-hand for pool ID type.
  using pool_id_t = Shm_pool::pool_id_t;

  /**
   * Creates an instance of this class with a single jemalloc arena. We require the use of a shared pointer,
   * because the construct() interfaces requires a handle for their destruction operations.
   *
   * @todo In ygoldfel's opinion Ipc_arena and certain other nearby classes do not actually require a GC-factory
   * interface. echan may have at one point agreed it is not strictly necessary, in my (ygoldfel) memory.
   * I could be wrong, but using mandatory shared ownership could be replaced by just storing raw pointers
   * (to pool collections/arenas) wherever internally needed; the only thing lost is that the big boys
   * (pool collections/arenas) be destroyed no earlier than is safe. Is that desirable? That's a philosophical
   * question; it's an added layer of GC, which could be nice, but on the other hand an arena/pool collection
   * is a heavy-weight, foundational object; and it is perfectly reasonable to expect a user to ensure it exists
   * beyond any contained object. As I see it, no one necessarily asked for this bit of GC, and as compared to
   * the rest of Flow-IPC (and Boost, STL arguably) it is somewhat inconsistent to add it. Also it does lengthen
   * and complicate the API and impl somewhat; and the added ref-counting could (theoretically at least) affect
   * performance. (Note! This is *not* about the mandatorily garbage-collected return values for construct()
   * and the like; that is absolutely a core design feature.) So to summarize consider having Ipc_arena and
   * Shm_pool_collection (etc.) be directly constructible as opposed to featuring a ref-counting factory create().
   *
   * @param logger Used for logging purposes.
   * @param memory_manager The memory allocator.
   * @param name_generator Shared object name generator.
   * @param permissions The shared memory object file permissions when one is created.
   *
   * @return Upon success, a shared pointer to an instance of this class; otherwise, an empty shared pointer.
   *
   * @see construct()
   */
  static std::shared_ptr<Ipc_arena> create(
    flow::log::Logger* logger,
    const std::shared_ptr<Memory_manager>& memory_manager,
    Shm_object_name_generator&& name_generator,
    const util::Permissions& permissions);
  /**
   * Destructor. Any shared memory pool listeners will be informed of the (imminent) deletion of the shared
   * memory pools ahead of its actual deletion.
   *
   * @todo Consider eliminating `virtual` APIs (destructor, construct(), etc.) from pool-collection hierarchy
   * for performance. Rationale: Flow-IPC elsewhere avoids run-time polymorphism, reasoning that if it is
   * necessary to the user, one could just add wrappers providing it; and meanwhile lightning-fast compile-time
   * polymorphism (if even applicable in the first place) is available. STL/Boost are generally designed similarly.
   * As far as I (ygoldfel) know, no user (internal or external) relies on run-time polymorphism in this
   * hierarchy (Ipc_arena, its parent Shm_pool_collection, etc.).
   */
  virtual ~Ipc_arena() override;

  /**
   * Returns the identifier for the arena.
   *
   * @return See above.
   */
  inline Arena_id get_arena_id() const;

  /**
   * Performs an allocation backed by shared memory. No thread cache will be used with this allocation.
   *
   * @param size The allocation size, which must be greater than zero.
   *
   * @return Upon success, a non-null pointer to the base address of the allocation; otherwise, nullptr.
   */
  virtual void* allocate(std::size_t size) override;
  /**
   * Performs an allocation backed by shared memory. No thread cache will be used with this deallocation.
   *
   * @param address The address to be deallocated, which must be non-null.
   */
  virtual void deallocate(void* address) override;

  /**
   * Performs an allocation backed by shared memory, uses the allocation to construct an object and returns a shared
   * pointer to this object. When the shared pointer has no more references, it will be destructed by this pool
   * collection instance. No thread cache will be used.
   *
   * @tparam T The object type to be created.
   * @tparam Args The parameter types that are passed to the constructor of T.
   * @param args The arguments passed to the constructor of T.
   *
   * @return A shared pointer to an object created in shared memory.
   *
   * @see construct(Arena_id, Args&&...)
   */
  template <typename T, typename... Args>
  inline Handle<T> construct(Args&&... args);
  /**
   * Performs an allocation backed by shared memory, uses the allocation to construct an object and returns a shared
   * pointer to this object. When the shared pointer has no more references, it will be destructed by this pool
   * collection instance.
   *
   * @tparam T The object type to be created.
   * @tparam Args The parameter types that are passed to the constructor of T.
   * @param use_cache Whether thread caching should be used.
   * @param args The arguments passed to the constructor of T.
   *
   * @return A shared pointer to an object created in shared memory.
   *
   * @see construct(Arena_id, Args&&...)
   */
  template <typename T, typename... Args>
  inline Handle<T> construct(bool use_cache, Args&&... args);
  /**
   * Adds a listener to get updates on shared memory pools. The pointer must remain valid until after it is removed.
   * The #ipc::shm::arena_lend::Owner_shm_pool_listener::notify_initial_shm_pools method will be called synchronously
   * within this call.
   *
   * @param listener The listener to add.
   *
   * @return Whether the listener was registered successfully, which means it wasn't registered previously.
   */
  bool add_shm_pool_listener(Owner_shm_pool_listener* listener);
  /**
   * Removes a listener from getting further updates on shared memory pools.
   *
   * @param listener The listener to remove.
   *
   * @return Whether the listener was deregistered successfully, which means it was registered previously.
   */
  bool remove_shm_pool_listener(Owner_shm_pool_listener* listener);

  /**
   * Returns the collection id that this object was constructed in. If it was not created in any collection, a
   * runtime_error will be thrown.
   *
   * @tparam T The object type.
   * @param object The non-null object to get the collection id from.
   *
   * @return See above.
   *
   * @throws std::runtime_error
   */
  template <typename T>
  static Collection_id get_collection_id(const Handle<T>& object);

protected:
  /**
   * Handles callback from shared pointer destruction without the use of a thread cache. Wraps around
   * #Object_deleter_no_cache by providing an arena activator when calling the object's destructor. This is
   * necessary when using objects containing Shm_pool_offset_ptr handles (fancy pointers), which are employed
   * with the use of Stateless_allocators (e.g., in containers supporting custom allocators).
   */
  class Ipc_object_deleter_no_cache :
    public Object_deleter_no_cache
  {
  public:
    /**
     * Constructor.
     *
     * @param pool_collection The object that allocated the underlying memory from which it should be returned
     * @param arena_id The arena in which the allocation was performed
     */
    Ipc_object_deleter_no_cache(const std::shared_ptr<Shm_pool_collection>& pool_collection,
                                Arena_id arena_id);

    /**
     * Release callback executed by shared pointer destruction. The destructor for the object will be called
     * and the underlying memory will be returned to the memory manager.
     *
     * @tparam T The object type.
     * @param p The object held by the shared pointer.
     */
    template <typename T>
    void operator()(T* p);
  }; // class Ipc_object_deleter_no_cache

  /**
   * Handles callback from shared pointer destruction with the use of a thread cache. Wraps around
   * #Object_deleter_cache by providing an arena activator when calling the object's destructor. This is
   * necessary when using objects containing Shm_pool_offset_ptr handles (fancy pointers), which are employed
   * with the use of Stateless_allocators (e.g., in containers supporting custom allocators).
   */
  class Ipc_object_deleter_cache :
    public Object_deleter_cache
  {
  public:
    /**
     * Constructor.
     *
     * @param thread_cache The thread cache to associate the allocation with.
     */
    Ipc_object_deleter_cache(const std::shared_ptr<Thread_cache>& thread_cache);

    /**
     * Release callback executed by shared pointer destruction. The destructor for the object will be called
     * and the underlying memory will be returned to the memory manager.
     *
     * @tparam T The object type
     * @param p The object held by the shared pointer
     */
    template <typename T>
    void operator()(T* p);
  }; // class Ipc_object_deleter_cache

  /**
   * Constructor.
   *
   * @param logger Used for logging purposes.
   * @param memory_manager The memory allocator.
   * @param name_generator Shared object name generator.
   * @param permissions The shared memory object file permissions when one is created.
   */
  Ipc_arena(flow::log::Logger* logger,
            const std::shared_ptr<Memory_manager>& memory_manager,
            Shm_object_name_generator&& name_generator,
            const util::Permissions& permissions);

  /**
   * Finds a shared memory pool by its identifier.
   *
   * @param shm_pool_id The identifier to located a shared memory pool by.
   *
   * @return If found, the shared memory pool; otherwise, nullptr.
   */
  std::shared_ptr<Shm_pool> lookup_shm_pool_by_id(pool_id_t shm_pool_id) const;

  /**
   * Registers the shared memory pool and notifies subscribers (listeners).
   *
   * @param shm_pool The shared memory pool that was created.
   */
  void handle_created_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool);
  /**
   * Deregisters the shared memory pool and notifies subscribers (listeners).
   *
   * @param shm_pool The shared memory pool that was removed.
   */
  void handle_removed_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool);

private:
  /**
   * Implements an event listener to capture event related to creation and removal of shared memory pools.
   */
  class Event_listener_impl :
    public Owner_shm_pool_collection::Event_listener
  {
  public:
    /**
     * Constructor.
     *
     * @param owner The arena containing this event listener, which must be valid for the lifetime of this listener.
     */
    Event_listener_impl(Ipc_arena& owner);

    /**
     * Notification for created shared memory pools.
     *
     * @param shm_pool The shared memory pool that was created.
     */
    virtual void notify_created_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool) override;
    /**
     * Notification for removed shared memory pools.
     *
     * @param shm_pool The shared memory pool that was removed.
     * @param removed_shared_memory Whether the underlying shared memory was actually removed.
     */
    virtual void notify_removed_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool,
                                         bool removed_shared_memory) override;

  private:
    /// The arena containing this event listener.
    Ipc_arena& m_owner;
  }; // class Event_listener_impl

  /// Single-reader, single-writer mutex.
  using Mutex = flow::util::Mutex_non_recursive;
  /// Exclusive lock for the mutex.
  using Lock = flow::util::Lock_guard<Mutex>;

  /// The listener for superclass events.
  Event_listener_impl m_event_listener;
  /// Protects access to #m_shm_pools and #m_listeners.
  mutable Mutex m_shm_pools_and_listeners_mutex;
  /// Collection of created shared memory pools.
  std::set<std::shared_ptr<Shm_pool>> m_shm_pools;
  /// The SHM pool listeners.
  std::set<Owner_shm_pool_listener*> m_listeners;

  /// The monotonically increasing collection identifier.
  static std::atomic<Collection_id> m_collection_id_counter;
}; // class Ipc_arena

Shm_pool_collection::Arena_id Ipc_arena::get_arena_id() const
{
  const auto& arena_ids = get_arena_ids();
  assert(arena_ids.size() == 1);
  auto iter = arena_ids.begin();
  return *iter;
}

template <typename T, typename... Args>
Ipc_arena::Handle<T> Ipc_arena::construct(Args&&... args)
{
  return construct<T>(false, std::forward<Args>(args)...);
}

template <typename T, typename... Args>
Ipc_arena::Handle<T> Ipc_arena::construct(bool use_cache, Args&&... args)
{
  // Use the first (and only) arena
  auto arena_id = get_arena_id();

  Ipc_arena_activator ctx(this);
  if (use_cache)
  {
    std::shared_ptr<Thread_cache> thread_cache = get_or_create_thread_cache(arena_id);
    return construct_helper<T>(
      Shm_pool_collection::allocate(sizeof(T), arena_id, thread_cache->get_thread_cache_id()),
      Ipc_object_deleter_cache(thread_cache),
      std::forward<Args>(args)...);
  }
  else
  {
    return construct_helper<T>(
      Shm_pool_collection::allocate(sizeof(T), arena_id),
      Ipc_object_deleter_no_cache(shared_from_this(), arena_id),
      std::forward<Args>(args)...);
  }
}

// Static method
template <typename T>
Collection_id Ipc_arena::get_collection_id(const std::shared_ptr<T>& object)
{
  {
    const auto* deleter = std::get_deleter<Ipc_object_deleter_no_cache>(object);
    if (deleter != nullptr)
    {
      auto pool_collection = deleter->get_pool_collection();
      auto* logger = pool_collection->get_logger();
      if ((logger != nullptr) && logger->should_log(flow::log::Sev::S_INFO, Log_component::S_SESSION))
      {
        FLOW_LOG_SET_CONTEXT(logger, Log_component::S_SESSION);
        FLOW_LOG_INFO_WITHOUT_CHECKING("Pool collection " << pool_collection << ", count " <<
                                       pool_collection.use_count());
      }
      return pool_collection->get_id();
    }
  }

  {
    const auto* deleter = std::get_deleter<Ipc_object_deleter_cache>(object);
    if (deleter != nullptr)
    {
      return deleter->get_thread_cache()->get_owner()->get_id();
    }
  }

  // Improper execution
  throw std::runtime_error("Object is not of proper construction to obtain collection id");
}

template <typename T>
void Ipc_arena::Ipc_object_deleter_no_cache::operator()(T* p)
{
  Ipc_arena_activator ctx(
    std::static_pointer_cast<Ipc_arena>(get_pool_collection()).get());
  Object_deleter_no_cache::operator()(p);
}

template <typename T>
void Ipc_arena::Ipc_object_deleter_cache::operator()(T* p)
{
  Ipc_arena_activator ctx(std::static_pointer_cast<Ipc_arena>(get_thread_cache()->get_owner()).get());
  Object_deleter_cache::operator()(p);
}

} // namespace ipc::shm::arena_lend::jemalloc
