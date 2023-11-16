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

#include "ipc/shm/arena_lend/memory_manager.hpp"
#include <jemalloc/jemalloc.h>

namespace ipc::shm::arena_lend::jemalloc
{

/**
 * Wrapper around jemalloc, an implementation of a memory allocator. This is a thin wrapper around the jemalloc
 * API.
 */
class Memory_manager :
  public arena_lend::Memory_manager
{
public:
  /// Arena id as dictated by jemalloc.
  using Arena_id = unsigned int;
  /// Thread cache id as dictated by jemalloc.
  using Thread_cache_id = unsigned int;

  /**
   * Wrapper around extent hooks, which are callbacks that can be instituted in jemalloc per arena. The prescribed
   * usage is to implement desired callbacks that execute the implementation within the creating object. The
   * extent_hooks callback parameter can be casted to this type to retrieve the owner of the callback for
   * execution in that class' context.
   */
  template <typename T>
  class Extent_hooks_wrapper :
    public extent_hooks_t
  {
  public:
    /**
     * Constructor.
     *
     * @param extent_hooks The set of callback functions.
     * @param owner The owner of this instance.
     */
    Extent_hooks_wrapper(extent_hooks_t&& extent_hooks, T* owner) :
      extent_hooks_t(std::move(extent_hooks)),
      m_owner(owner)
    {
    }

    /**
     * Returns the owner of this class.
     *
     * @return See above.
     */
    inline T* get_owner() const { return m_owner; }

  private:
    /// The holder of this object.
    T* m_owner;
  }; // class Extent_hooks_wrapper

  /**
   * Constructor.
   *
   * @param logger For logging purposes.
   */
  Memory_manager(flow::log::Logger* logger);

  /**
   * Allocates uninitialized memory designated for the default memory areas, which are also known as arenas,
   * without the use of a thread cache.
   *
   * @param size The allocation size, which must be greater than zero.
   *
   * @return Upon success, a non-null pointer to the base address of the allocation; otherwise, nullptr.
   */
  virtual void* allocate(std::size_t size) override;
  /**
   * Allocates uninitialized memory designated in a segregated memory area, which is also known as an arena,
   * without the use of a thread cache.
   *
   * @param size The allocation size, which must be greater than zero.
   * @param arena_id The id of the memory area.
   *
   * @return Upon success, a non-null pointer to the base address of the allocation; otherwise, nullptr.
   */
  void* allocate(std::size_t size, Arena_id arena_id);
  /**
   * Allocates uninitialized memory designated in a segregated memory area, which is also known as an arena.
   *
   * @param size The allocation size, which must be greater than zero.
   * @param arena_id The id of the memory area.
   * @param thread_cache_id The thread cache id to associate the allocation with.
   *
   * @return Upon success, a non-null pointer to the base address of the allocation; otherwise, nullptr.
   */
  void* allocate(std::size_t size, Arena_id arena_id, Thread_cache_id thread_cache_id);

  /**
   * Deallocates memory from the default arenas without using a thread cache.
   *
   * @param address The address to be deallocated, which must be non-null.
   */
  virtual void deallocate(void* address) override;
  /**
   * Deallocates memory from a specific arena without using a thread cache.
   *
   * @param address The address to be deallocated, which must be non-null.
   * @param arena_id The id of the memory area that initially allocated the memory.
   */
  void deallocate(void* address, Arena_id arena_id);
  /**
   * Deallocates memory from a specific arena.
   *
   * @param address The address to be deallocated, which must be non-null.
   * @param arena_id The id of the memory area that initially allocated the memory.
   * @param thread_cache_id The thread cache id that was originally associated with the allocation.
   */
  void deallocate(void* address, Arena_id arena_id, Thread_cache_id thread_cache_id);

  /**
   * Creates a new segregated memory area.
   *
   * @param extent_hooks A borrowed handle to a set of callbacks, which may be null to use the default set.
   *                     Note that any non-null value must be kept valid for the arena's lifetime.
   *
   * @return The id of the memory area that is created.
   *
   * @throws system_error jemalloc may return an error, which results in a system error being thrown.
   */
  Arena_id create_arena(extent_hooks_t* extent_hooks);
  /**
   * Destroys a memory area. Note that any thread that allocated memory in the specified arena must have their
   * thread cache flushed prior to the destruction of the arena.
   *
   * @param arena_id The id of the memory area to destroy.
   *
   * @throws system_error jemalloc may return an error, which results in a system error being thrown.
   */
  void destroy_arena(Arena_id arena_id);

  /**
   * Creates a thread cache.
   *
   * @return The id of the created thread cache.
   *
   * @throws system_error jemalloc may return an error, which results in a system error being thrown.
   */
  Thread_cache_id create_thread_cache();
  /**
   * Destroys a thread cache.
   *
   * @param thread_cache_id The id of the thread cache to destroy.
   *
   * @throws system_error jemalloc may return an error, which results in a system error being thrown.
   */
  void destroy_thread_cache(Thread_cache_id thread_cache_id);
  /**
   * Flushes a thread cache.
   *
   * @param thread_cache_id The id of the thread cache to flush.
   *
   * @throws system_error jemalloc may return an error, which results in a system error being thrown.
   */
  void flush_thread_cache(Thread_cache_id thread_cache_id);
  /**
   * Flushes the current thread's thread cache.
   *
   * @throws system_error jemalloc may return an error, which results in a system error being thrown.
   */
  // void flush_current_thread_cache();

  /**
   * Prints statistics to stdout.
   */
  void print_stats() const;

private:
  /**
   * Allocates memory associated designated in a segregated memory area, which is also known as an arena.
   *
   * @param size The allocation size, which must be greater than zero.
   * @param arena_id The id of the memory area.
   * @param thread_cache_flags The jemalloc flags specifying a thread cache.
   *
   * @return Upon success, a non-null pointer to the base address of the allocation; otherwise, nullptr.
   */
  void* allocate_helper(std::size_t size, Arena_id arena_id, int thread_cache_flags);
  /**
   * Deallocates memory from a specific arena.
   *
   * @param address The address to be deallocated, which must be non-null.
   * @param arena_id The id of the memory area that initially allocated the memory.
   * @param thread_cache_flags The jemalloc flags specifying a thread cache.
   */
  void deallocate_helper(void* address, Arena_id arena_id, int thread_cache_flags);
}; // class Memory_manager

} // namespace ipc::shm::arena_lend::jemalloc
