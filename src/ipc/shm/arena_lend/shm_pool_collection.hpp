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

#include "ipc/shm/arena_lend/arena_lend_fwd.hpp"
#include <flow/log/log.hpp>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/algorithm/sort.hpp>
#include <memory>
#include <map>
#include <set>
#include <vector>

namespace ipc::shm::arena_lend
{

/**
 * A set of shared memory pools related in their application usage.
 */
class Shm_pool_collection :
  /* `protected`, not `public`: end-users have no business with our logging accessors (get_logger()/etc.); and
   * `Log_context`, not `Log_context_mt`: `*this` is not concurrently-set_logger()-able (see ctor/dtor and
   * skip_fast_path_verbose_logging()), so the thread-safe variant is unnecessary entropy.  Sub-classes
   * (Owner_shm_pool_collection, jemalloc::Ipc_arena) still log freely via the (protected) inherited accessors. */
  protected flow::log::Log_context
{
public:
  /**
   * Constructor.
   *
   * @param logger For logging purposes.
   * @param id Identifier for the collection.
   */
  Shm_pool_collection(flow::log::Logger* logger, collection_id_t id);
  /**
   * Destructor.
   */
  virtual ~Shm_pool_collection();

  /**
   * Returns the identifier for the collection.
   *
   * @return See above.
   */
  inline collection_id_t get_id() const;

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
   * Returns the cached flag indicating whether hot-path TRACE/DATA logging should be skipped.
   * The flag is snapshotted once at construction (from `Logger::should_log()` at that time), letting hot paths
   * skip even the `should_log()` check on every allocation/deallocation.  As `*this` is not
   * user-`set_logger()`-able, the logger -- hence this flag -- is fixed for `*this` lifetime; a TRACE-verbosity
   * config change after construction will not take effect here (acceptable trade-off for the hot-path savings).
   *
   * @return `true` if hot-path verbose logging should be skipped.
   */
  inline bool skip_fast_path_verbose_logging() const;
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

  /**
   * Utility: walks through each shared memory pool in the collection, in pool-ID order (therefore, in chronological
   * order by creation time), and executes `func(P)`, where `P` is `shared_ptr<const Shm_pool>&&`.
   *
   * The entire method executes under lock; this includes calling `func()` N times.
   *
   * ### Rationale ###
   * The precipitating use-case was Ipc_arena::shm_pool_live_info() which is logging/monitoring/etc.-oriented.
   * However it might be useful for some other purpose; but be mindful of perf impact of the lock (though,
   * as of this writing, it is a read-lock of a 2-level mutex).
   *
   * @tparam Func See above; functor.
   * @param func See above.
   */
  template<typename Func>
  void for_each_shm_pool(const Func& func) const;

  /**
   * Identical to the `const` for_each_shm_pool() overload but feeds `shared_ptr<Shm_pool>&&` (pointer to
   * mutable pool) to `func()`.  All notes from that doc header apply; in particular the entire method executes
   * under (read-)lock, so `func()` must not add/remove pools (write-lock ops) -- snapshot and act afterward
   * instead.
   *
   * @tparam Func See above; functor.
   * @param func See above.
   */
  template<typename Func>
  void for_each_shm_pool(const Func& func);

private:
  /// Multi-reader, single-writer mutex.
  using Mutex = flow::util::Mutex_shared_non_recursive;
  /// Single-writer lock for the mutex.
  using Write_lock = flow::util::Lock_guard<Mutex>;
  /// Multi-reader lock for the mutex.
  using Read_lock = flow::util::Shared_lock_guard<Mutex>;

  /**
   * Implements both for_each_shm_pool() overloads (which differ only in the constness of the pool
   * pointers fed to `func()`).  `const` despite potentially feeding pointers-to-mutable: the interface-level
   * constness statement is made by the public overloads.
   *
   * @tparam Shm_pool_t Either `Shm_pool` or `const Shm_pool`.
   * @tparam Func See for_each_shm_pool().
   * @param func See for_each_shm_pool().
   */
  template<typename Shm_pool_t, typename Func>
  void for_each_shm_pool_impl(const Func& func) const;

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

  /**
   * Cached flag: `true` if TRACE/DATA logging was disabled (per `Logger::should_log()`) at construction,
   * letting hot paths skip even the `should_log()` check.  `const`: snapshotted once at construction; see
   * skip_fast_path_verbose_logging().
   */
  const bool m_skip_fast_path_verbose_logging;
  /// Collection identifier.
  const collection_id_t m_id;
  /// Mutex for pool map.
  mutable Mutex m_shm_pool_map_mutex;
  /// Address -> Shm_pool ptr map.
  std::map<void*, std::shared_ptr<Shm_pool>> m_shm_pool_map;
}; // class Shm_pool_collection

collection_id_t Shm_pool_collection::get_id() const
{
  return m_id;
}

bool Shm_pool_collection::skip_fast_path_verbose_logging() const
{
  return m_skip_fast_path_verbose_logging;
}

bool Shm_pool_collection::deregister_shm_pool_internal(void* address)
{
  Write_lock write_lock(m_shm_pool_map_mutex);
  return m_shm_pool_map.erase(address) != 0;
}

template<typename Func>
void Shm_pool_collection::for_each_shm_pool(const Func& func) const
{
  for_each_shm_pool_impl<const Shm_pool>(func);
}

template<typename Func>
void Shm_pool_collection::for_each_shm_pool(const Func& func)
{
  for_each_shm_pool_impl<Shm_pool>(func);
}

template<typename Shm_pool_t, typename Func>
void Shm_pool_collection::for_each_shm_pool_impl(const Func& func) const
{
  using boost::adaptors::map_values;
  using boost::adaptors::transformed;
  using boost::range::sort;
  using std::vector;
  using std::static_pointer_cast;
  using std::shared_ptr;

  // As advertised: lock throughout.
  Read_lock read_lock{m_shm_pool_map_mutex};

  const auto shm_pools_lame_order_rng = m_shm_pool_map | map_values | transformed([](const auto& shm_pool_ptr) -> auto
  {
    return static_pointer_cast<Shm_pool_t>(shm_pool_ptr); // shared_ptr<S_p> => shared_ptr<[const] S_p>.
  });
  vector<shared_ptr<Shm_pool_t>> shm_pools{shm_pools_lame_order_rng.begin(), shm_pools_lame_order_rng.end()};

  sort(shm_pools, [](const auto& shm_pool_ptr_a, const auto& shm_pool_ptr_b) -> bool
                    { return shm_pool_ptr_a->get_id() < shm_pool_ptr_b->get_id(); });
  for (auto& shm_pool_ptr : shm_pools)
  {
    func(std::move(shm_pool_ptr));
  }
} // Shm_pool_collection::for_each_shm_pool_impl()

} // namespace ipc::shm::arena_lend
