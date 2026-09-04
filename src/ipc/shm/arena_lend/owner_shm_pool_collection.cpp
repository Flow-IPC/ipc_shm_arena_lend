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
#include "ipc/shm/arena_lend/owner_shm_pool_collection.hpp"
#include "ipc/shm/arena_lend/memory_manager.hpp"
#include "ipc/util/util_fwd.hpp"
#include "ipc/util/native_handle.hpp"
#include <sstream>
#include <cstdlib>
#include <sys/mman.h>

using std::size_t;
using std::shared_ptr;
using std::string;
using std::to_string;
using std::make_shared;
using std::stringstream;
using std::static_pointer_cast;
using flow::log::Logger;

namespace ipc::shm::arena_lend
{

Owner_shm_pool_collection::Owner_shm_pool_collection(Logger* logger,
                                                     collection_id_t id,
                                                     const shared_ptr<Memory_manager>& memory_manager,
                                                     Shared_name&& pool_name_base,
                                                     const util::Permissions& permissions) :
  Shm_pool_collection(logger, id),
  m_memory_manager(memory_manager),
  m_pool_name_base(std::move(pool_name_base)),
  m_permissions(permissions)
{
}

Owner_shm_pool_collection::~Owner_shm_pool_collection()
{
  using std::vector;

#ifndef FLOW_OS_LINUX
  static_assert(false, "The straggler-pool sweep just below (::munmap() et al) has only been designed-for/tested "
                       "in Linux as of this writing; check this area when porting.");
#endif

  /* Remove any still-registered pools; normally the stragglers described in our doc header (SHM-jemalloc:
   * the native arena's base/metadata block pools), if any.  Mechanics notes:
   *   - for_each_shm_pool() holds the lock across its callbacks, while remove_shm_pool() write-locks;
   *     hence snapshot first, remove after.
   *   - remove_shm_pool() invokes virtual on_shm_pool_removed(); executing as we are within our own destructor,
   *     that dispatches to *our* (default no-op) implementation -- subclass overrides are unreachable by C++
   *     rules, the subclass object no longer existing.  That is as desired: any subclass-level bookkeeping was
   *     handled during the subclass's own destruction steps (e.g., jemalloc::Ipc_arena bulk-deregisters its pools from
   *     the process-wide repository at the start of its destruction sequence).  Corollary: if this sweep is ever
   *     relocated outside the destructor, reconsider -- overrides *would* fire then.
   *   - Ditto any subclass-level pool-removal stats: that layer is already destroyed; these removals go
   *     uncounted.  In any case user can't access *this stats, as they can't access *this; if they could then
   *     *this would not be undergoing destruction now.  At least that is how jemalloc::Ipc_arena's ownership
   *     semantics work; probably hypothetical other arena-lending SHM-providers would use the same semantics.
   *     (As of this writing jemalloc::Ipc_arena <=> SHM-jemalloc is the only arena-lending SHM-provider.) */
  vector<shared_ptr<Shm_pool>> shm_pools;
  for_each_shm_pool([&](shared_ptr<Shm_pool>&& shm_pool) { shm_pools.emplace_back(std::move(shm_pool)); });

  for (const auto& shm_pool : shm_pools)
  {
    FLOW_LOG_INFO("Owner pool collection [" << get_id() << "]: removing straggler SHM pool [" << *shm_pool << "] "
                  "left registered through the memory manager's native teardown.");
    bool ignored;
    remove_shm_pool(shm_pool,
                    [](const shared_ptr<Shm_pool>& pool) -> bool
                      { return ::munmap(pool->get_address(), pool->get_size()) == 0; },
                    ignored);
    /* Subtlety: The little unmap functor is correct and all; jemalloc::Ipc_arena elsewhere does something that
     * looks different -- it uses Jemalloc_pages utilities -- but in actual fact, at least in Linux, that
     * reduces to the same thing we do here.  For maintainability w/r/t future porting, though, we added
     * the static_assert() above.  @todo Arguably a totally proper impl would do some kind of polymorphic thing
     * so that the same unmap-functor is used in both/all places. */
  }
}

void Owner_shm_pool_collection::deallocate(void* object)
{
  m_memory_manager->deallocate(object);
}

void Owner_shm_pool_collection::on_shm_pool_created(const shared_ptr<Shm_pool>&)
{
  // Default no-op.
}

void Owner_shm_pool_collection::on_shm_pool_removed(const shared_ptr<Shm_pool>&, bool)
{
  // Default no-op.
}

shared_ptr<Shm_pool> Owner_shm_pool_collection::create_shm_pool(pool_id_t id,
                                                                const string& name,
                                                                size_t size,
                                                                void* address,
                                                                const Memory_map_functor& memory_map_functor)
{
  /* @todo Tighten up the various error scenarios in Owner_shm_pool_collection (also spiritually related
   * code on the borrower side): Be careful to classify errors as recoverable -versus- not really/indicates our
   * bug somewhere -versus- not really/indicates catastrophic environment failure; then act accordingly and
   * be consistent about it.  Maintenance-historic note: Generally we've already executed such refactor-contained
   * error-scenario tightening in most places in SHM-jemalloc, such as Shm_session; but the low-level SHM-pool areas
   * have not undergone refactoring yet, as mostly on functionality they haven't needed much change. */

  int fd = create_shm_object(name, size);
  if (fd == -1)
  {
    return nullptr;
  }

  void* actual_address = memory_map_functor(fd, size, address);
  if (!actual_address)
  {
    FLOW_LOG_WARNING("Could not map shared memory object [" << name << "], size [" << size << "]");
    ::close(fd);
    remove_shm_object(name);
    return nullptr;
  }

  // @todo Make fd list cache?
  shared_ptr<Shm_pool> shm_pool
    = make_shared<Lockable_shm_pool>(id, name, actual_address, size, fd);
  if (!register_shm_pool_and_notify(shm_pool))
  {
    // We somehow allocated at an existing location
    FLOW_LOG_FATAL("Could not map shared memory pool [" << *shm_pool << "]");
    assert(false && "Duplicate SHM pool address");
    std::abort();
  }

  return shm_pool;
}

int Owner_shm_pool_collection::create_shm_object(const string& name, size_t size)
{
  assert(!name.empty());
  assert(size > 0);

  assert((size <= size_t(std::numeric_limits<pool_offset_t>::max()))
         && "Creating a pool sized too large to express offsets given our pointer data structures; "
            "did memory allocator algorithm (e.g., jemalloc) demand a shockingly gigantic pool? "
            "See Shm_pool_offset_ptr_data_base::pool_offset_t docs.");

  /* shm_open() requires a leading '/' per POSIX; Shared_name uses '_' as separator, so we prepend it here.
   * On Linux (glibc) it works without, but POSIX portability demands it. */
  const string shm_name = '/' + name;

  // Create shared memory pool
  int fd = ::shm_open(shm_name.c_str(), (O_RDWR | O_CREAT | O_EXCL), m_permissions.get_permissions());
  if (fd == -1)
  {
    if (errno == EEXIST)
    {
      // Shared memory object name already exists
      FLOW_LOG_WARNING("Shared object name '" << name << "' already exists");
    }
    else
    {
      FLOW_LOG_WARNING("Error occurred when opening shm name '" << name << "': " << strerror(errno) << "(" <<
                       errno << ")");
    }
    return -1;
  }

  // Set proper permissions on the file handle due to potential conflict with umask when opening
  Error_code ec;
  util::set_resource_permissions(get_logger(), util::Native_handle{fd}, m_permissions, &ec);
  if (ec)
  {
    FLOW_LOG_WARNING("Could not change permissions to [" << std::oct << m_permissions.get_permissions() <<
                     "] for object name [" << name << "], error [" << ec << "]");
    ::close(fd);
    ::shm_unlink(shm_name.c_str());
    return -1;
  }

  // Set size
  int result = ftruncate(fd, size);
  if (result == -1)
  {
    // Handle error
    FLOW_LOG_WARNING("Error occurred when setting size for shm name '" << name << "': " << strerror(errno) << "(" <<
                     errno << ")");
    ::close(fd);
    ::shm_unlink(shm_name.c_str());
    return -1;
  }

  return fd;
}

bool Owner_shm_pool_collection::remove_range_and_pool_if_empty(const void* address,
                                                               size_t size,
                                                               const Memory_decommit_functor* decommit_functor,
                                                               bool& removed_range,
                                                               const Memory_unmap_functor& unmap_functor,
                                                               bool& unmapped_pool)
{
  removed_range = false;
  unmapped_pool = false;

  if (size <= 0)
  {
    FLOW_LOG_WARNING("Removal size is zero");
    return false;
  }

  shared_ptr<Lockable_shm_pool> shm_pool = static_pointer_cast<Lockable_shm_pool>(lookup_shm_pool(address));
  if (!shm_pool)
  {
    FLOW_LOG_WARNING("Specified address " << address << " is not within a pool");
    return false;
  }

  // Sanity check that range is wholly within pool
  Shm_pool::size_t offset;
  if (!shm_pool->is_subset(address, size, &offset))
  {
    FLOW_LOG_WARNING("Specified range of address " << address << ", size " << size <<
                     " is not within the pool's range; potential bug or memory corruption");
    return false;
  }

  if ((decommit_functor != nullptr) && !(*decommit_functor)(shm_pool, offset, size))
  {
    FLOW_LOG_WARNING("Failed in decommitting range with address " << address << ", size " << size);
    return false;
  }

  {
    Lockable_shm_pool::Lock lock{shm_pool->get_mutex()};
    size_t remaining_size = shm_pool->get_remaining_size();
    if (remaining_size < size)
    {
      if (remaining_size == 0)
      {
        FLOW_LOG_WARNING("Request to remove size [" << size << "] from a zero remaining size shared memory pool [" <<
                         *shm_pool << "]");
        return false;
      }
      FLOW_LOG_WARNING("Request to remove size " << size << " that is larger than remaining " << remaining_size <<
                       " starting at address " << address << "; will remove remainder");
    }

    if (shm_pool->remove_size(size))
    {
      FLOW_LOG_TRACE("Successfully removed address " << address << ", size " << size << " from shm_pool " <<
                     *shm_pool);
    }
    removed_range = true;

    if (shm_pool->get_remaining_size() > 0)
    {
      // There are additional regions in use, so we won't remove the shared memory pool
      return true;
    }
  }

  return remove_shm_pool(shm_pool, unmap_functor, unmapped_pool);
}

bool Owner_shm_pool_collection::remove_shm_pool(const shared_ptr<Shm_pool>& shm_pool,
                                                const Memory_unmap_functor& unmap_functor,
                                                bool& unmapped_pool)
{
  FLOW_LOG_TRACE("Removing shared memory pool [" << *shm_pool << "]");

  if (!deregister_shm_pool(shm_pool))
  {
    unmapped_pool = false;
    return false;
  }

  const bool shm_object_removed = remove_shm_object(shm_pool->get_name());

  /* Notify -- which notably erases the pool from Owner_shm_pool_repository (see jemalloc::Ipc_arena
   * override) -- strictly *before* the unmap below.  Invariant at stake: the pool's vaddr range must not
   * become OS-reusable (a future `mmap()` can land on any free range) while repositories still list this
   * pool at that range; otherwise a concurrently-created pool -- from any arena in the process -- could land
   * there and clash with the stale entry.  (Arena-teardown pool-removal maintains the same order: there the
   * repository erasure happens in bulk, before arena destruction triggers the unmaps; see
   * jemalloc::Ipc_arena teardown-sequence comments.  Also: the name-unlink above is fine pre-unmap; POSIX
   * destroys the object only once all unmaps/closes have occurred.) */
  on_shm_pool_removed(shm_pool, shm_object_removed);

  unmapped_pool = unmap_functor(shm_pool);

  // Close pool
  if (::close(shm_pool->get_fd()) != 0)
  {
    // Error occurred - possibly nonexistent
    FLOW_LOG_WARNING("Error occurred when closing shared memory name [" << shm_pool->get_name() << "], error [" <<
                     strerror(errno) << " (" << errno << ")]");
  }

  return true;
}

bool Owner_shm_pool_collection::remove_shm_object(const string& name)
{
  assert(!name.empty());

  const string shm_name = '/' + name;
  if (::shm_unlink(shm_name.c_str()) != 0)
  {
    // Error occurred - possibly nonexistent
    FLOW_LOG_WARNING("Error occurred when removing shared memory name '" << name << "': " << strerror(errno) << "(" <<
                     errno << ")");
    // @todo Handle
    return false;
  }

  return true;
}

bool Owner_shm_pool_collection::register_shm_pool_and_notify(const shared_ptr<Shm_pool>& shm_pool)
{
  if (!register_shm_pool(shm_pool))
  {
    return false;
  }

  on_shm_pool_created(shm_pool);

  return true;
}

const Shared_name& Owner_shm_pool_collection::get_pool_name_base() const
{
  return m_pool_name_base;
}

const util::Permissions& Owner_shm_pool_collection::get_permissions() const
{
  return m_permissions;
}

} // namespace ipc::shm::arena_lend
