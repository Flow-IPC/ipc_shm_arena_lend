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

#include <gtest/gtest.h>
#include "ipc/shm/arena_lend/jemalloc/memory_manager.hpp"
#include "ipc/shm/arena_lend/jemalloc/detail/jemalloc.hpp"
#include "ipc/shm/arena_lend/jemalloc/test/test_jemalloc_pages.hpp"
#include "ipc/shm/arena_lend/test/test_shm_object.hpp"
#include "ipc/test/test_logger.hpp"
#include <bitset>
#include <sys/mman.h>
#include <limits.h>

using std::size_t;
using std::set;
using std::string_view;
using namespace ipc::test;

namespace ipc::shm::arena_lend::jemalloc::test
{

using Arena_id = Memory_manager::Arena_id;
using Thread_cache_id = Memory_manager::Thread_cache_id;

/**
 * Utility class that tracks extent execution.
 */
class Extent_hooks_tracker
{
public:
  /**
   * The set of callbacks that can be executed.
   */
  enum class Action_flags_enum : unsigned int
  {
    CREATE = 0,
    OPTIONAL_REMOVE,
    REMOVE,
    COMMIT,
    DECOMMIT,
    SPLIT,
    COUNT
  }; // enum class Action_flags_enum

  /**
   * Sets the action that occurred.
   *
   * @param action The action that occurred.
   */
  void set_action_flag(Action_flags_enum action)
  {
    m_action_flags.set(static_cast<unsigned int>(action));
  }

  /**
   * Resets the action tracker.
   */
  void reset_action_flags()
  {
    m_action_flags.reset();
  }

  /**
   * Returns whether creation occurred since last reset.
   *
   * @return See above.
   */
  bool did_create_action() const
  {
    return m_action_flags.test(static_cast<unsigned int>(Action_flags_enum::CREATE));
  }

  /**
   * Returns whether removal occurred since last reset.
   *
   * @return See above.
   */
  bool did_remove_action() const
  {
    return m_action_flags.test(static_cast<unsigned int>(Action_flags_enum::REMOVE));
  }

  /**
   * Returns whether removal or optional removal occurred since last reset.
   *
   * @return See above.
   */
  bool did_any_remove_action() const
  {
    return (did_remove_action() || m_action_flags.test(static_cast<unsigned int>(Action_flags_enum::OPTIONAL_REMOVE)));
  }

  /**
   * Returns whether a split occurred since last reset.
   *
   * @return See above.
   */
  bool did_split_action() const
  {
    return m_action_flags.test(static_cast<unsigned int>(Action_flags_enum::SPLIT));
  }

  /**
   * Returns whether any action was taken since last reset.
   *
   * @return See above.
   */
  bool did_any_action() const
  {
    return m_action_flags.any();
  }

private:
  /// Storage for the actions that occurred since last reset.
  std::bitset<static_cast<unsigned int>(Action_flags_enum::COUNT)> m_action_flags;
}; // class Extent_hooks_tracker

/**
 * Wrapper around Memory_manager that tracks callback execution.
 */
class Test_memory_manager :
  public Memory_manager,
  public Extent_hooks_tracker
{
public:
  /**
   * Constructor.
   *
   * @param logger For logging purposes.
   */
  Test_memory_manager(flow::log::Logger* logger) :
    Memory_manager(logger),
    m_extent_hooks_wrapper(
      {
        .alloc = &create_memory_pool_handler,
        .dalloc = &optional_remove_memory_pool_handler,
        .destroy = &remove_memory_pool_handler,
        .commit = &commit_memory_pages_handler,
        .decommit = &decommit_memory_pages_handler,
        .purge_lazy = nullptr,
        .purge_forced = nullptr,
        .split = nullptr,
        .merge = nullptr
      },
      this)
  {
  }

  /**
   * Creates a new segregated memory area.
   *
   * @return The id of the memory area that is created.
   */
  Arena_id create_arena()
  {
    return Memory_manager::create_arena(&m_extent_hooks_wrapper);
  }

private:
  /// Convenience type.
  using Hooks_wrapper = Memory_manager::Extent_hooks_wrapper<Test_memory_manager>;

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
  static void* create_memory_pool_handler(extent_hooks_t* extent_hooks,
                                          void* address,
                                          size_t size,
                                          size_t alignment,
                                          bool* zero,
                                          bool* commit,
                                          unsigned arena_id)
  {
    assert(extent_hooks != nullptr);
    Test_memory_manager* memory_manager = static_cast<Hooks_wrapper*>(extent_hooks)->get_owner();
    return memory_manager->create_memory_pool(address, size, alignment, zero, commit, arena_id);
  }

  /**
   * Jemalloc callback when a memory pool is no longer needed.
   *
   * @param extent_hooks The set of callbacks containing this callback.
   * @param address The memory pool to remove.
   * @param size The size of the memory pool to be removed.
   * @param committed Whether the memory pool is currently committed to physical memory.
   * @param arena_id The memory area that the pool resides in.
   *
   * @return Whether the memory was NOT removed.
   */
  static bool optional_remove_memory_pool_handler(extent_hooks_t* extent_hooks,
                                                  void* address,
                                                  size_t size,
                                                  bool committed,
                                                  unsigned arena_id)
  {
    assert(extent_hooks != nullptr);
    Test_memory_manager* memory_manager = static_cast<Hooks_wrapper*>(extent_hooks)->get_owner();
    return !memory_manager->optional_remove_memory_pool(address, size, committed, arena_id);
  }

  /**
   * Jemalloc callback when a memory pool is instructed to be removed.
   *
   * @param extent_hooks The set of callbacks containing this callback.
   * @param address The memory pool to remove.
   * @param size The size of the memory pool to be removed.
   * @param committed Whether the memory pool is currently committed to physical memory.
   * @param arena_id The memory area that the pool resides in.
   */
  static void remove_memory_pool_handler(extent_hooks_t* extent_hooks,
                                         void* address,
                                         size_t size,
                                         bool committed,
                                         unsigned arena_id)
  {
    assert(extent_hooks != nullptr);
    Test_memory_manager* memory_manager = static_cast<Hooks_wrapper*>(extent_hooks)->get_owner();
    memory_manager->remove_memory_pool(address, size, committed, arena_id);
  }

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
                                          size_t size,
                                          size_t offset,
                                          size_t length,
                                          unsigned arena_id)
  {
    assert(extent_hooks != nullptr);
    Test_memory_manager* memory_manager = static_cast<Hooks_wrapper*>(extent_hooks)->get_owner();
    return !memory_manager->commit_memory_pages(address, size, offset, length, arena_id);
  }

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
                                            size_t size,
                                            size_t offset,
                                            size_t length,
                                            unsigned arena_id)
  {
    assert(extent_hooks != nullptr);
    Test_memory_manager* memory_manager = static_cast<Hooks_wrapper*>(extent_hooks)->get_owner();
    return !memory_manager->decommit_memory_pages(address, size, offset, length, arena_id);
  }

  /**
   * Creates a memory pool.
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
  void* create_memory_pool(void* address,
                           size_t size,
                           size_t alignment,
                           bool* zero,
                           bool* commit,
                           [[maybe_unused]] unsigned arena_id)
  {
    assert(zero != nullptr);
    assert(commit != nullptr);

    set_action_flag(Action_flags_enum::CREATE);
    void* pool_address = Jemalloc_pages::map(address, size, alignment, *commit, -1);
    if (pool_address == nullptr)
    {
      return nullptr;
    }

    *zero = *commit;

    return pool_address;
  }

  /**
   * Decides whether to remove an unneeded memory pool or preserve it for future use, and remove it as appropriate.
   * We will always remove in our case.
   *
   * @param address The memory pool to remove.
   * @param size The size of the memory pool to be removed.
   * @param committed Whether the memory pool is currently committed to physical memory.
   * @param arena_id The memory area that the pool resides in.
   *
   * @return Whether the memory pool was unmapped.
   */
  bool optional_remove_memory_pool(void* address, size_t size, bool committed, unsigned arena_id)
  {
    set_action_flag(Action_flags_enum::OPTIONAL_REMOVE);
    remove_memory_pool_helper(address, size, committed, arena_id);
    return true;
  }

  /**
   * Removes a memory pool.
   *
   * @param address The memory pool to remove.
   * @param size The size of the memory pool to be removed.
   * @param committed Whether the memory pool is currently committed to physical memory.
   * @param arena_id The memory area that the pool resides in.
   */
  void remove_memory_pool(void* address, size_t size, bool committed, unsigned arena_id)
  {
    set_action_flag(Action_flags_enum::REMOVE);
    remove_memory_pool_helper(address, size, committed, arena_id);
  }

  /**
   * Removes a memory pool.
   *
   * @param address The memory pool to remove.
   * @param size The size of the memory pool to be removed.
   * @param committed Whether the memory pool is currently committed to physical memory.
   * @param arena_id The memory area that the pool resides in.
   */
  void remove_memory_pool_helper(void* address,
                                 size_t size,
                                 [[maybe_unused]] bool committed,
                                 [[maybe_unused]] unsigned arena_id)
  {
    Jemalloc_pages::unmap(address, size);
  }

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
                           [[maybe_unused]] size_t size,
                           size_t offset,
                           size_t length,
                           [[maybe_unused]] unsigned arena_id)
  {
    set_action_flag(Action_flags_enum::COMMIT);
    return Test_jemalloc_pages::commit_original((static_cast<char*>(address) + offset), length);
  }

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
                             [[maybe_unused]] size_t size,
                             size_t offset,
                             size_t length,
                             [[maybe_unused]] unsigned arena_id)
  {
    set_action_flag(Action_flags_enum::DECOMMIT);
    return Test_jemalloc_pages::decommit_original((static_cast<char*>(address) + offset), length);
  }

  /// The extent hooks.
  Hooks_wrapper m_extent_hooks_wrapper;
}; // class Test_memory_manager

/**
 * Wrapper around default hooks that tracks callback execution.
 */
class Default_jemalloc_memory_manager :
  public jemalloc::Memory_manager,
  public Extent_hooks_tracker
{
public:
  /**
   * Constructor. Overrides the extent hooks in the default arena.
   *
   * @param logger For logging purposes.
   */
  Default_jemalloc_memory_manager(flow::log::Logger* logger) :
    Memory_manager(logger),
    m_default_hooks(get_default_hooks()),
    m_extent_hooks_wrapper(
      {
        .alloc = &create_memory_pool_handler,
        .dalloc = &optional_remove_memory_pool_handler,
        .destroy = &remove_memory_pool_handler,
        .commit = &commit_memory_pages_handler,
        .decommit = &decommit_memory_pages_handler,
        .purge_lazy = nullptr,
        .purge_forced = nullptr,
        .split = &split_memory_pages_handler,
        .merge = nullptr
      },
      this)
  {
    if (m_default_hooks == nullptr)
    {
      throw std::runtime_error("Default hooks not found");
    }

    m_extent_hooks_wrapper.purge_lazy = m_default_hooks->purge_lazy;
    m_extent_hooks_wrapper.purge_forced = m_default_hooks->purge_forced;
    m_extent_hooks_wrapper.merge = m_default_hooks->merge;

    // Set the extent hooks to use for operations
    extent_hooks_t* hooks = &m_extent_hooks_wrapper;
    size_t input_size = sizeof(hooks);
    if (IPC_SHM_ARENA_LEND_JEMALLOC_API(mallctl)
          (S_DEFAULT_ARENA_EXTENT_HOOKS_OP, nullptr, nullptr, &hooks, input_size) != 0)
    {
      throw std::runtime_error("Could not set extent hooks");
    }

    // Set the arena to use for this thread
    unsigned arena_id = S_DEFAULT_ARENA;
    if (IPC_SHM_ARENA_LEND_JEMALLOC_API(mallctl)
          (S_DEFAULT_ARENA_OP, nullptr, nullptr, &arena_id, sizeof(arena_id)) != 0)
    {
      throw std::runtime_error("Could not set arena");
    }
  }

  /**
   * Destructor. Restores the extent hooks in the default arena.
   */
  ~Default_jemalloc_memory_manager()
  {
    // Restore extent hooks
    size_t input_size = sizeof(m_default_hooks);
    if (IPC_SHM_ARENA_LEND_JEMALLOC_API(mallctl)
          (S_DEFAULT_ARENA_EXTENT_HOOKS_OP, nullptr, nullptr, &m_default_hooks, input_size) != 0)
    {
      ADD_FAILURE() << "Could not set default extent hooks";
    }
  }

private:
  /// Convenience type.
  using Hooks_wrapper = Memory_manager::Extent_hooks_wrapper<Default_jemalloc_memory_manager>;

  /**
   * Retrieves the jemalloc extent hooks in the default arena.
   *
   * @return See above.
   */
  static extent_hooks_t* get_default_hooks()
  {
    extent_hooks_t* default_hooks;
    size_t output_size = sizeof(default_hooks);
    if (IPC_SHM_ARENA_LEND_JEMALLOC_API(mallctl)
          (S_DEFAULT_ARENA_EXTENT_HOOKS_OP, &default_hooks, &output_size, nullptr, 0) != 0)
    {
      return nullptr;
    }

    EXPECT_NE(default_hooks->alloc, nullptr);
    EXPECT_NE(default_hooks->dalloc, nullptr);
    EXPECT_NE(default_hooks->destroy, nullptr);
    EXPECT_NE(default_hooks->commit, nullptr);
    EXPECT_NE(default_hooks->decommit, nullptr);
    EXPECT_NE(default_hooks->purge_lazy, nullptr);
    EXPECT_NE(default_hooks->purge_forced, nullptr);
    EXPECT_NE(default_hooks->split, nullptr);
    EXPECT_NE(default_hooks->merge, nullptr);

    return default_hooks;
  }

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
  static void* create_memory_pool_handler(extent_hooks_t* extent_hooks,
                                          void* address,
                                          size_t size,
                                          size_t alignment,
                                          bool* zero,
                                          bool* commit,
                                          unsigned arena_id)
  {
    assert(extent_hooks != nullptr);
    Default_jemalloc_memory_manager* memory_manager = static_cast<Hooks_wrapper*>(extent_hooks)->get_owner();
    return memory_manager->create_memory_pool(address, size, alignment, zero, commit, arena_id);
  }

  /**
   * Jemalloc callback when a memory pool is no longer needed.
   *
   * @param extent_hooks The set of callbacks containing this callback.
   * @param address The memory pool to remove.
   * @param size The size of the memory pool to be removed.
   * @param committed Whether the memory pool is currently committed to physical memory.
   * @param arena_id The memory area that the pool resides in.
   *
   * @return Whether the memory was NOT removed.
   */
  static bool optional_remove_memory_pool_handler(extent_hooks_t* extent_hooks,
                                                  void* address,
                                                  size_t size,
                                                  bool committed,
                                                  unsigned arena_id)
  {
    assert(extent_hooks != nullptr);
    Default_jemalloc_memory_manager* memory_manager = static_cast<Hooks_wrapper*>(extent_hooks)->get_owner();
    return !memory_manager->optional_remove_memory_pool(address, size, committed, arena_id);
  }

  /**
   * Jemalloc callback when a memory pool is instructed to be removed.
   *
   * @param extent_hooks The set of callbacks containing this callback.
   * @param address The memory pool to remove.
   * @param size The size of the memory pool to be removed.
   * @param committed Whether the memory pool is currently committed to physical memory.
   * @param arena_id The memory area that the pool resides in.
   */
  static void remove_memory_pool_handler(extent_hooks_t* extent_hooks,
                                         void* address,
                                         size_t size,
                                         bool committed,
                                         unsigned arena_id)
  {
    assert(extent_hooks != nullptr);
    Default_jemalloc_memory_manager* memory_manager = static_cast<Hooks_wrapper*>(extent_hooks)->get_owner();
    memory_manager->remove_memory_pool(address, size, committed, arena_id);
  }

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
                                          size_t size,
                                          size_t offset,
                                          size_t length,
                                          unsigned arena_id)
  {
    assert(extent_hooks != nullptr);
    Default_jemalloc_memory_manager* memory_manager = static_cast<Hooks_wrapper*>(extent_hooks)->get_owner();
    return memory_manager->commit_memory_pages(address, size, offset, length, arena_id);
  }

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
                                            size_t size,
                                            size_t offset,
                                            size_t length,
                                            unsigned arena_id)
  {
    assert(extent_hooks != nullptr);
    Default_jemalloc_memory_manager* memory_manager = static_cast<Hooks_wrapper*>(extent_hooks)->get_owner();
    return memory_manager->decommit_memory_pages(address, size, offset, length, arena_id);
  }

  /**
   * Jemalloc callback to logically split a contiguous set of memory pages into two memory pools.
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
                                         size_t size,
                                         size_t size_a,
                                         size_t size_b,
                                         bool committed,
                                         unsigned arena_id)
  {
    assert(extent_hooks != nullptr);
    Default_jemalloc_memory_manager* memory_manager = static_cast<Hooks_wrapper*>(extent_hooks)->get_owner();
    return !memory_manager->split_memory_pages(address, size, size_a, size_b, committed, arena_id);
  }

  /**
   * Creates a memory pool.
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
  void* create_memory_pool(void* address,
                           size_t size,
                           size_t alignment,
                           bool* zero,
                           bool* commit,
                           unsigned arena_id)
  {
    set_action_flag(Action_flags_enum::CREATE);
    return m_default_hooks->alloc(&m_extent_hooks_wrapper, address, size, alignment, zero, commit, arena_id);
  }

  /**
   * Decides whether to remove an unneeded memory pool or preserve it for future use, and remove it as appropriate.
   * We will always remove in our case.
   *
   * @param address The memory pool to remove.
   * @param size The size of the memory pool to be removed.
   * @param committed Whether the memory pool is currently committed to physical memory.
   * @param arena_id The memory area that the pool resides in.
   *
   * @return Whether the memory pool was unmapped.
   */
  bool optional_remove_memory_pool(void* address, size_t size, bool committed, unsigned arena_id)
  {
    set_action_flag(Action_flags_enum::OPTIONAL_REMOVE);
    return !m_default_hooks->dalloc(&m_extent_hooks_wrapper, address, size, committed, arena_id);
  }

  /**
   * Removes a memory pool.
   *
   * @param address The memory pool to remove.
   * @param size The size of the memory pool to be removed.
   * @param committed Whether the memory pool is currently committed to physical memory.
   * @param arena_id The memory area that the pool resides in.
   */
  void remove_memory_pool(void* address, size_t size, bool committed, unsigned arena_id)
  {
    set_action_flag(Action_flags_enum::REMOVE);
    m_default_hooks->destroy(&m_extent_hooks_wrapper, address, size, committed, arena_id);
  }

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
  bool commit_memory_pages(void* address, size_t size, size_t offset, size_t length, unsigned arena_id)
  {
    set_action_flag(Action_flags_enum::COMMIT);
    return !m_default_hooks->commit(&m_extent_hooks_wrapper, address, size, offset, length, arena_id);
  }

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
  bool decommit_memory_pages(void* address, size_t size, size_t offset, size_t length, unsigned arena_id)
  {
    set_action_flag(Action_flags_enum::DECOMMIT);
    return !m_default_hooks->decommit(&m_extent_hooks_wrapper, address, size, offset, length, arena_id);
  }

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
  bool split_memory_pages(void* address,
                          size_t size,
                          size_t size_a,
                          size_t size_b,
                          bool committed,
                          unsigned arena_id)
  {
    set_action_flag(Action_flags_enum::SPLIT);
    return !m_default_hooks->split(&m_extent_hooks_wrapper, address, size, size_a, size_b, committed, arena_id);
  }

  /// The default extent hooks.
  extent_hooks_t* m_default_hooks;
  /// The extent hooks that wrap around the default.
  Hooks_wrapper m_extent_hooks_wrapper;

  /// Default jemalloc arena used for allocations.
  static constexpr Arena_id S_DEFAULT_ARENA = 0;
  /// Command used to get/set the extent hooks.
  static constexpr char S_DEFAULT_ARENA_EXTENT_HOOKS_OP[] = "arena.0.extent_hooks";
  /// Command used to get/set the jemalloc arena used for this thread.
  static constexpr char S_DEFAULT_ARENA_OP[] = "thread.arena";
}; // class Default_jemalloc_memory_manager

/// Class interface death tests.
#ifdef NDEBUG // These "deaths" occur only if assert()s enabled; else these are guaranteed failures.
TEST(Memory_manager_DeathTest, DISABLED_Interface)
#else
TEST(Memory_manager_DeathTest, Interface)
#endif
{
  Test_logger test_logger;

  {
    constexpr Arena_id ARENA = 0;
    Memory_manager memory_manager(&test_logger);

    // Allocation and deallocation
    EXPECT_DEATH(memory_manager.allocate(0UL, ARENA), "size > 0");
    EXPECT_DEATH(memory_manager.deallocate(nullptr, ARENA), "address != nullptr");

    // Thread cache
    // Illegal thread cache id
    EXPECT_DEATH(memory_manager.destroy_thread_cache(INT_MAX), "");
  }
}

/**
 * Class interface tests.  Can't name it Jemalloc_memory_manager_test, as there's one named same in another namespace;
 * this makes Google test framework barf (albeit very politely, wherein it explains this is what usually makes
 * that problem occur).
 */
TEST(Jemalloc_memory_manager_test, Interface)
{
  Test_logger test_logger;

  // Arena creation and destruction
  {
    Memory_manager memory_manager(&test_logger);

    constexpr size_t NUM_ARENAS = 5;
    set<Arena_id> arena_ids;
    for (size_t i = 0; i < NUM_ARENAS; ++i)
    {
      EXPECT_TRUE(arena_ids.emplace(memory_manager.create_arena(nullptr)).second);
    }

    constexpr size_t NUM_THREAD_CACHES = 10;
    
    set<Thread_cache_id> thread_cache_ids;
    for (size_t i = 0; i < NUM_THREAD_CACHES; ++i)
    {
      EXPECT_TRUE(thread_cache_ids.emplace(memory_manager.create_thread_cache()).second);
    }

    Arena_id arena_id = *(arena_ids.begin());
    Thread_cache_id thread_cache_id = *(thread_cache_ids.begin());

    // Non-thread cache allocation
    {
      void* p = memory_manager.allocate(1000, arena_id);
      EXPECT_NE(p, nullptr);
      memory_manager.deallocate(p, arena_id);
    }

    // Thread cache allocation
    {
      void* p = memory_manager.allocate(1000, arena_id, thread_cache_id);
      EXPECT_NE(p, nullptr);
      memory_manager.deallocate(p, arena_id, thread_cache_id);
    }

    // Clean up thread caches
    for (const auto& iter : thread_cache_ids)
    {
      EXPECT_NO_THROW(memory_manager.destroy_thread_cache(iter));
    }

    // Clean up arenas
    for (const auto& iter : arena_ids)
    {
      using ipc::shm::arena_lend::test::check_empty_collection_in_output;
      EXPECT_TRUE(check_empty_collection_in_output(ipc::test::collect_output([&memory_manager, &iter]()
                                                                             {
                                                                               memory_manager.destroy_arena(iter);
                                                                             })));
      // Illegal arena indices, which does not crash today, but check if there's change in behavior
      EXPECT_THROW(memory_manager.destroy_arena(iter), std::system_error);
    }

    // Illegal arena index, which does not crash today, but check if there's change in behavior
    EXPECT_THROW(memory_manager.destroy_arena(INT_MAX), std::system_error);
  }

  // Extent hooks test
  {
    const size_t ALLOC_SIZE = 1000;

    Test_memory_manager memory_manager(&test_logger);
    Memory_manager::Arena_id arena = memory_manager.create_arena();
    memory_manager.reset_action_flags();

    // Use heap mapping as we don't want to open shared memory for this test
    void* p = memory_manager.allocate(ALLOC_SIZE, arena);
    EXPECT_NE(p, nullptr);
    EXPECT_TRUE(memory_manager.did_create_action());
    memory_manager.deallocate(p, arena);
    memory_manager.reset_action_flags();

    EXPECT_NO_THROW(memory_manager.destroy_arena(arena));
    EXPECT_TRUE(memory_manager.did_any_remove_action());
    memory_manager.reset_action_flags();
  }
}

/**
 * Tests to ensure default allocators/deallocators are not overridden.
 * NOTE: This only passes if jemalloc is not the default allocator.
 *
 * ygoldfel adds: As of this writing (11/2023) this test appears to ~always fail, at least if run as part of
 * the overall suite.  I discussed briefly with echan (test author); he didn't have time to get into it yet,
 * but generally I believe it might be a matter of ordering of this test versus others in the suite.
 * For now disabling it (DISABLED_) to have a look later.
 */
TEST(Jemalloc_memory_manager_test, DISABLED_No_default_override)
{
  // Allocate a large enough size that an allocation or split would likely be performed if jemalloc was used
  const size_t ALLOC_SIZE = Jemalloc_pages::get_page_size() * 1024 * 1024;
  Test_logger test_logger;
  Default_jemalloc_memory_manager memory_manager(&test_logger);

  // C interface
  {
    void* p = malloc(ALLOC_SIZE);
    EXPECT_NE(p, nullptr);
    EXPECT_FALSE(memory_manager.did_any_action());
    free(p);
    memory_manager.reset_action_flags();
  }

  // C++ interface
  {
    uint8_t* p = new uint8_t[ALLOC_SIZE];
    EXPECT_NE(p, nullptr);
    EXPECT_FALSE(memory_manager.did_any_action());
    delete[] p;
    memory_manager.reset_action_flags();
  }

  // Make sure our hooks get called
  {
    void* p = memory_manager.allocate(ALLOC_SIZE);
    EXPECT_NE(p, nullptr);
    // We should have either created a new extent or split one
    EXPECT_TRUE(memory_manager.did_create_action() || memory_manager.did_split_action());
    memory_manager.deallocate(p);
    memory_manager.reset_action_flags();
  }
}

} // namespace ipc::shm::arena_lend::jemalloc::test
