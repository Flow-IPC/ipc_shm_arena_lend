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
#include "ipc/shm/arena_lend/jemalloc/thread_cache.hpp"
#include "ipc/shm/arena_lend/jemalloc/detail/jemalloc.hpp"
#include "ipc/shm/arena_lend/jemalloc/test/test_jemalloc_pages.hpp"
#include "ipc/test/test_logger.hpp"
#include <flow/test/test_common_util.hpp>
#include <flow/error/error.hpp>
#include <flow/log/log.hpp>
#include <flow/util/util.hpp>
#include <flow/util/util_fwd.hpp>
#include <atomic>
#include <bitset>
#include <memory>
#include <string>
#include <vector>
#include <sys/mman.h>
#include <limits.h>

using std::size_t;
using std::set;
using flow::log::Log_context_mt;

namespace ipc::shm::arena_lend::jemalloc::test
{

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
    return did_remove_action() || m_action_flags.test(static_cast<unsigned int>(Action_flags_enum::OPTIONAL_REMOVE));
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
   */
  Test_memory_manager() :
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
  arena_id_t create_arena()
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
   */
  Default_jemalloc_memory_manager() :
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
  static constexpr arena_id_t S_DEFAULT_ARENA = 0;
  /// Command used to get/set the extent hooks.
  static constexpr char S_DEFAULT_ARENA_EXTENT_HOOKS_OP[] = "arena.0.extent_hooks";
  /// Command used to get/set the jemalloc arena used for this thread.
  static constexpr char S_DEFAULT_ARENA_OP[] = "thread.arena";
}; // class Default_jemalloc_memory_manager

/// Class interface death tests.
TEST(Memory_manager_DeathTest, Interface)
{
#ifdef NDEBUG
  GTEST_SKIP() << "Death tests rely on assert()s which are disabled in this (NDEBUG) build.";
#endif
  {
    constexpr arena_id_t ARENA = 0;
    Memory_manager memory_manager;

    // Allocation and deallocation
    EXPECT_DEATH(memory_manager.allocate(0UL, ARENA), "size > 0");
    EXPECT_DEATH(memory_manager.deallocate(nullptr, ARENA), "address");
  }
}

/**
 * Class interface tests.  Can't name it Jemalloc_memory_manager_test, as there's one named same in another namespace;
 * this makes Google test framework barf (albeit very politely, wherein it explains this is what usually makes
 * that problem occur).
 */
TEST(Jemalloc_memory_manager_test, Interface)
{
  using flow::error::Runtime_error;
  using std::atomic;

  // Arena creation and destruction
  {
    Memory_manager memory_manager;

    constexpr size_t NUM_ARENAS = 5;
    set<arena_id_t> arena_ids;
    for (size_t i = 0; i < NUM_ARENAS; ++i)
    {
      EXPECT_TRUE(arena_ids.emplace(memory_manager.create_arena(nullptr)).second);
    }

    /* Let tcache mean thread cache.
     * Notes about how the below tests tcache-related [de]allocations and arena destruction:
     *
     * Historically, this test was originally written by echan.  At the time tcache support in Flow-IPC's SHM-jemalloc
     * module (where we are) was rudimentary at best; but realistically it was ineffective and unused and unusable
     * by any user (we knew this; it was not a surprise; more of a half-done to-do).  Nevertheless, to the limited
     * extent that it did exist, the present test case exercised it.  Some time later I (ygoldfel)
     * added real tcache support (centered on the then-new class Thread_cache), integrated with the rest of
     * SHM-jemalloc including Memory_manager (the testee here) and Ipc_arena and co. (though Thread_cache could also
     * be used by itself + direct jemalloc or just Memory_manager which is a very thin wrapper around jemalloc).
     * I then wrote unit and functional test(s) for all of that in various dimensions.
     *
     * However the present test case, from echan, remained of value, and there was no reason to rip out parts of it
     * that -- while incomplete if viewed as testing of tcache-aware allocation -- still were valid (if arguably of
     * less value than other parts of the present test case and surrounding ones).
     *
     * The act of destroying an arena, which echan's test case exercised, is actually intimitely intertwined with
     * that of destroying certain relevant tcache(s) (if any); e.g., not destroying a tcache but destroying an
     * arena related in a certain way to that tcache can lead to intra-jemalloc crashing.  Long story short, the above
     * notes essentially apply to arena destruction too.  That is I (ygoldfel still; hello) handled arena destruction
     * by properly relating Memory_manager code (mostly by echan but now modified by me somewhat) to Thread_cache
     * code: they cooperate.  Similarly, then, I added testing thereof elsewhere.  As of this writing it's all
     * roughly in one place.
     *
     * So: The below largely retains the original test steps (somewhat modified given my subsequent changes)
     * but, concerning the above topics, should be considered basic as opposed to an attempt at being exhaustive.
     * If adding more related testing, please consider Thread_cache_test first. */

    arena_id_t arena_id = *(arena_ids.begin());
    // Non-thread cache allocation
    {
      void* p = memory_manager.allocate(1000, arena_id);
      EXPECT_NE(p, nullptr);
      memory_manager.deallocate(p, arena_id);
    }

    // Thread cache allocation
    {
      const auto tid = Thread_cache::this_thread_cache()->id(arena_id);
      void* p = memory_manager.allocate(1000, arena_id, tid);
      EXPECT_NE(p, nullptr);
      EXPECT_EQ(tid, Thread_cache::this_thread_cache()->id(arena_id));
      memory_manager.deallocate(p, arena_id, tid);
    }
    // Thread cache allocation / non-thread-cache deallocation [added versus original test-case/see above notes]
    {
      void* p = memory_manager.allocate(1000, arena_id, Thread_cache::this_thread_cache()->id(arena_id));
      EXPECT_NE(p, nullptr);
      memory_manager.deallocate(p, arena_id);
    }

    // Clean up arenas
    /* [Original test-case (see above notes) used check_empty_collection_in_output() on the output of
     *  memory_manager.destroy_arena(); but it seems like that wasn't really checking anything real: memory_manager
     *  was not (and still is not) hooked up to any extent-hook-SHM-stuff, so it could not print anything about
     *  SHM-pools ever in the first place, hence it'd just scan whatever random generic arena-destruction-related
     *  things memory_manager.destroy_arena() used to log, find nothing SHM-pool-related, and vacuously pass.
     *  Possibly the idea was it'd a pool but with stats showing all of its space is unused at arena-destruction time?
     *  Seems that way, but again -- as noted -- it could never print anything like that.
     *  This all may or may not have been intentional; or maybe my (ygoldfel) code inspection is mistaken; but in any
     *  case it makes little sense trying to replicate that check. So, instead, we basically (1) ensure it doesn't
     *  crash (which has some value given the tcache-vs-arena-related shenanigans' capacity for mayhem); and (2)
     *  do ensure that the "time to REALLY destroy arena, as tcache(s) have been safely eliminated from all threads"
     *  (in our case just our thread, hence the synchronous execution) functor *does* get invoked synchronously.]
     *  Part (2) also has some value, even though from a black-box perspective it doesn't necessarily *prove* the
     *  arena will get really-really destroyed. Still not bad though given our overall intent of basic testing. */
    boost::shared_ptr<atomic<bool>> destroyed{new atomic<bool>};
    for (const auto& iter : arena_ids)
    {
      /* (In reality there are no background threads in the impl but just in case that changes use `atomic`.
       * Also it will work synchronously, so the shared_ptr should not be needed, but black-boxily speaking, if
       * it fails to destroy arena synchronously as is expected (due to our basic, single-threaded setup here),
       * then invalid memory access could occur without the shared_ptr. */
      *destroyed = false;
      Log_context_mt log_ctx;
      memory_manager.destroy_arena(iter, &log_ctx, [destroyed](auto, auto&& really_destroy_arena_func)
      {
        really_destroy_arena_func(); *destroyed = true;
      });
      EXPECT_TRUE(*destroyed) << "Arena [" << iter << "] should have been synchronously destroyed.";

      *destroyed = false; // Go again but "destroy" fake arena that does not exist anymore (that part should throw).
      memory_manager.destroy_arena(iter, &log_ctx,
                                   [destroyed](auto, auto&& really_destroy_arena_func)
                                     { EXPECT_THROW(really_destroy_arena_func(), Runtime_error); *destroyed = true; });
      EXPECT_TRUE(*destroyed) << "Now-fake-arena [" << iter << "] should have been synchronously fake-destroyed.";
    }

    // Similar to last thing above but use fake index.
    *destroyed = false; // Go again but destroy fake arena that does not exist anymore.
    Log_context_mt log_ctx;
    memory_manager.destroy_arena(INT_MAX, &log_ctx,
                                 [destroyed](auto, auto&& really_destroy_arena_func)
                                   { EXPECT_THROW(really_destroy_arena_func(), Runtime_error);  *destroyed = true; });
    EXPECT_TRUE(*destroyed) << "Mega-fake-arena [" << INT_MAX << "] should have been synchronously fake-destroyed.";
  }

  // Extent hooks test
  {
    const size_t ALLOC_SIZE = 1000;

    Test_memory_manager memory_manager;
    arena_id_t arena = memory_manager.create_arena();
    memory_manager.reset_action_flags();

    // Use heap mapping as we don't want to open shared memory for this test
    void* p = memory_manager.allocate(ALLOC_SIZE, arena);
    EXPECT_NE(p, nullptr);
    EXPECT_TRUE(memory_manager.did_create_action());
    memory_manager.deallocate(p, arena);
    memory_manager.reset_action_flags();

    Log_context_mt log_ctx;
    EXPECT_NO_THROW(memory_manager.destroy_arena(arena, &log_ctx));
    EXPECT_TRUE(memory_manager.did_any_remove_action());
    memory_manager.reset_action_flags();
  }
}

/**
 * Concurrency regression guard for detail::jemalloc_arena_list_mutex() (see doc header).  Hammers concurrent jemalloc
 * arena create/destroy against concurrent whole-arena-set stats dumps (`stats_dump_to_string()`, i.e. jemalloc
 * `malloc_stats_print()`).  Without that mutex being used internally, a destroy landing inside a dump's per-arena
 * walk makes jemalloc `abort()` the whole process (a race in jemalloc's stats.c).  With the mutex,
 * create/destroy/dump (if using Flow-IPC wrappers) become non-concurrent, so this must run to completion with
 * no crash and no deadlock.
 *
 * Notes:
 *   - *Probabilistic* guard, not a deterministic reproducer: the race window is only microseconds wide, so a
 *     regression would typically crash only *sometimes*.  The deterministic variant needs the window widened
 *     inside jemalloc's stats.c (a local, debug-only patch, intentionally not part of the product build).
 *   - Only meaningful against a stats-enabled jemalloc (the Flow-IPC default); with stats off the dump does no
 *     per-arena walk, and this passes vacuously.
 *   - Worker threads make no gtest calls: they record any failure into a mutex-guarded string, and every gtest
 *     check runs on the main thread after join().  That sidesteps gtest's cross-thread caveats (assertion
 *     thread-safety, and the ASSERT_* return-only-exits-the-lambda gotcha) entirely.
 *   - The main thread *polls* (`Thread_cache::this_thread_cache_or_null()`) while awaiting the workers, instead of
 *     blocking in join().  Why: if any thread holds `Thread_cache` per-thread state yet never again uses SHM-jemalloc
 *     nor exits, every arena-destroy defers to it indefinitely (see Thread_cache::destroy_arena_safely()); and in a
 *     full-suite run an earlier test leaves the main thread in exactly that condition.  Observed pre-fix, in-suite:
 *     every destroy deferred => each create leaked an arena => each dump walked/printed an ever-growing arena set =>
 *     O(n^2), ~16x slowdown.  The polling keeps deferred destroys flowing; bonus: in-suite this exercises the
 *     deferred cross-thread destroy path (isolated, it is a no-op: `_or_null` creates no state).
 */
TEST(Jemalloc_memory_manager_test, Arena_list_mutex_concurrency)
{
  using ipc::test::Test_logger;
  using flow::log::Log_context_mt;
  using flow::log::Logger;
  using flow::log::Sev;
  using flow::util::Mutex_non_recursive;
  using flow::util::Lock_guard;
  using flow::util::ostream_op_string;
  using flow::util::this_thread::sleep_for;
  using boost::chrono::milliseconds;
  using Thread = flow::util::Thread;
  using std::vector;
  using std::string;
  using uint = unsigned int;

  // Shared across threads: every Memory_manager method used here is `const` and a thin, stateless jemalloc wrapper.
  Memory_manager memory_manager;

  constexpr uint N_DESTROYER_THREADS = 4; // Each churns empty arenas: create=>destroy repeatedly.
  constexpr uint N_DUMPER_THREADS = 4; // Each repeatedly runs the full malloc_stats_print() dump.
  constexpr uint N_ITERATIONS = 100; // Per-thread loop count; tune up for a wider (slower) net.

  /* The worker threads touch only these standard primitives; every gtest check runs on the main thread after
   * join().  So there is zero reliance on gtest's cross-thread behavior (thread-safety, or the ASSERT_* `return`
   * subtlety).  `first_error` non-empty <=> some worker hit a problem (a throw or an empty dump); first to report
   * wins.  No separate pass/fail flag needed. */
  Mutex_non_recursive error_mutex;
  string first_error;
  const auto report_error = [&](const string& msg)
  {
    Lock_guard<Mutex_non_recursive> lock{error_mutex};
    if (first_error.empty())
    {
      first_error = msg;
    }
  };

  /* Always-on console progress logger (Test_logger serializes output => no interleaving across the worker
   * threads).  Kept separate from `log_ctx` below, which stays silent so create/destroy do not flood us. */
  Test_logger progress_logger_obj{Sev::S_INFO};
  FLOW_LOG_SET_CONTEXT(&progress_logger_obj, Log_component::S_TEST);

  /* Progress: count completed dumps (the slow, time-dominating side) and log at ~5% steps.  ++ hands each
   * count to exactly one thread, so each step logs exactly once. */
  std::atomic<unsigned long> dumps_done{0};
  const unsigned long total_dumps = static_cast<unsigned long>(N_DUMPER_THREADS) * N_ITERATIONS;
  const unsigned long dumps_per_step = (total_dumps >= 20) ? (total_dumps / 20) : 1;
  const auto note_dump = [&](size_t dump_sz)
  {
    const auto done = ++dumps_done;
    if ((done % dumps_per_step) == 0)
    {
      /* The 2 extra data points diagnose dump slowness: the dump's own text size; and `arenas.narenas` = the arena
       * slot count the dump must walk (jemalloc arena-index high-water: auto-arenas + custom-arena slots; it is
       * exactly the bound of the dump's per-arena pre-scan).  If the latter climbs during the run, our destroys are
       * not completing -- most likely deferred behind a cache-bearing thread that is not polling (the exact
       * condition the main thread's poll-wait loop, described in our doc header, exists to prevent). */
      unsigned int narenas = 0;
      size_t narenas_sz = sizeof(narenas);
      IPC_SHM_ARENA_LEND_JEMALLOC_API(mallctl)("arenas.narenas", &narenas, &narenas_sz, nullptr, 0);
      FLOW_LOG_INFO("Arena_list_mutex_concurrency: ~[" << (done * 100 / total_dumps) << "% "
                    "(" << done << "/" << total_dumps << " dumps)]; last dump size = [" << dump_sz << "]; "
                    "arenas.narenas = [" << narenas << "].");
    }
  };

  Log_context_mt log_ctx; // Null logger (silent).

  /* Each worker bumps this as its very last act; the main thread poll-waits on it (instead of blocking in join())
   * for the reason explained in our doc header. */
  std::atomic<uint> n_workers_done{0};

  vector<Thread> threads;

  for (uint t = 0; t != N_DESTROYER_THREADS; ++t)
  {
    threads.emplace_back([&]()
    {
      try
      {
        for (uint idx = 0; idx != N_ITERATIONS; ++idx)
        {
          const arena_id_t arena_id = memory_manager.create_arena(nullptr);
          /* The destroy = the real on-done-func -> jemalloc_arena_list_mutex() -> arena.destroy path.  It completes
           * synchronously if no thread holds Thread_cache state (isolated run); otherwise it is deferred until each
           * such thread polls -- hence the main thread's poll-wait loop below (see doc header). */
          memory_manager.destroy_arena(arena_id, &log_ctx);
        }
      }
      catch (const std::exception& exc)
      {
        report_error(ostream_op_string("Destroyer thread threw: [", exc.what(), "]."));
      }
      ++n_workers_done;
    });
  } // for (destroyer threads)

  for (uint t = 0; t != N_DUMPER_THREADS; ++t)
  {
    threads.emplace_back([&]()
    {
      try
      {
        for (uint idx = 0; idx != N_ITERATIONS; ++idx)
        {
          const auto dump = memory_manager.stats_dump_to_string(); // Default = full text dump (the risky walk).
          if (dump.empty())
          {
            report_error("jemalloc stats dump unexpectedly empty.");
            break;
          }
          note_dump(dump.size());
        }
      }
      catch (const std::exception& exc)
      {
        report_error(ostream_op_string("Dumper thread threw: [", exc.what(), "]."));
      }
      ++n_workers_done;
    });
  } // for (dumper threads)

  // Poll-wait (rationale in doc header; `_or_null` polls if this thread holds Thread_cache state, else no-op).
  constexpr uint N_WORKERS = N_DESTROYER_THREADS + N_DUMPER_THREADS;
  while (n_workers_done != N_WORKERS)
  {
    Thread_cache::this_thread_cache_or_null();
    sleep_for(milliseconds(10));
  }
  for (auto& t : threads)
  {
    t.join();
  }
  Thread_cache::this_thread_cache_or_null(); // Drain any destroys deferred since the loop's last poll.

  {
    // The final arena-count data point: if all destroys completed, this should be back near its pre-test value.
    unsigned int narenas = 0;
    size_t narenas_sz = sizeof(narenas);
    IPC_SHM_ARENA_LEND_JEMALLOC_API(mallctl)("arenas.narenas", &narenas, &narenas_sz, nullptr, 0);
    FLOW_LOG_INFO("Arena_list_mutex_concurrency: done; final arenas.narenas = [" << narenas << "].");
  }

  /* Reaching here at all is the core result: no jemalloc abort() from a destroy racing a dump, and no deadlock.
   * The recorded-error check then catches the softer failures (a worker throw or an empty dump). */
  EXPECT_TRUE(first_error.empty()) << "A worker thread reported: [" << first_error << "].";
} // TEST(Jemalloc_memory_manager_test, Arena_list_mutex_concurrency)

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
  Default_jemalloc_memory_manager memory_manager;

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
