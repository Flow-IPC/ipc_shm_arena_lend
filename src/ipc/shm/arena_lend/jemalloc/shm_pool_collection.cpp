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

#include "ipc/shm/arena_lend/jemalloc/shm_pool_collection.hpp"
#include "ipc/shm/arena_lend/jemalloc/memory_manager.hpp"
#include "ipc/shm/arena_lend/jemalloc/jemalloc_pages.hpp"
#include "ipc/shm/arena_lend/shm_pool.hpp"
#include "ipc/common.hpp"

using std::shared_ptr;
using std::string;
using std::vector;
using std::size_t;
using std::make_shared;
using flow::log::Logger;

namespace ipc::shm::arena_lend::jemalloc
{

using Pages_wrapper = Memory_manager::Extent_hooks_wrapper<Shm_pool_collection>;

// Thread local data for caching allocations
thread_local Shm_pool_collection::Thread_local_data Shm_pool_collection::s_thread_local_data;

// Static method
shared_ptr<Shm_pool_collection> Shm_pool_collection::create(
  Logger* logger,
  Collection_id id,
  const shared_ptr<Memory_manager>& memory_manager,
  Shm_object_name_generator&& name_generator,
  const util::Permissions& permissions)
{
  return shared_ptr<Shm_pool_collection>(
    new Shm_pool_collection(logger, id, memory_manager, std::move(name_generator), permissions));
}

Shm_pool_collection::Shm_pool_collection(Logger* logger,
                                         Collection_id id,
                                         const shared_ptr<Memory_manager>& memory_manager,
                                         Shm_object_name_generator&& name_generator,
                                         const util::Permissions& permissions) :
  Owner_shm_pool_collection(logger, id, memory_manager, std::move(name_generator), permissions),
  m_started(false),
  m_extent_hooks_wrapper(
    {
      .alloc = &create_shm_pool_handler,
      .dalloc = &optional_remove_shm_pool_handler,
      .destroy = &remove_shm_pool_handler,
      .commit = &commit_memory_pages_handler,
      .decommit = &decommit_memory_pages_handler,
      .purge_lazy = nullptr,
      .purge_forced = &purge_forced_memory_pages_handler,
      .split = &split_memory_pages_handler,
      .merge = &merge_memory_pages_handler
    },
    this)
{
}

bool Shm_pool_collection::start(unsigned int arenas)
{
  if (arenas == 0)
  {
    FLOW_LOG_WARNING("Invalid arenas specified " << arenas);
    return false;
  }

  if (m_started)
  {
    FLOW_LOG_WARNING("Already started");
    return true;
  }
  m_started = true;

  assert(m_arenas.empty());
  for (unsigned int i = 0; i < arenas; ++i)
  {
    Memory_manager::Arena_id arena =
      get_jemalloc_memory_manager()->create_arena(&m_extent_hooks_wrapper);
    if (!m_arenas.emplace(arena).second)
    {
      FLOW_LOG_WARNING("Duplicate arena index (" << arena << ") when starting");
      assert(false);
      return false;
    }
  }

  return true;
}

Shm_pool_collection::~Shm_pool_collection()
{
  for (const auto& i : m_arenas)
  {
    get_jemalloc_memory_manager()->destroy_arena(i);
  }
}

// Static method
void* Shm_pool_collection::create_shm_pool_handler(extent_hooks_t* extent_hooks,
                                                   void* address,
                                                   size_t size,
                                                   size_t alignment,
                                                   bool* zero,
                                                   bool* commit,
                                                   unsigned arena_id)
{
  assert(extent_hooks != nullptr);
  Shm_pool_collection* collection = static_cast<Pages_wrapper*>(extent_hooks)->get_owner();
  return collection->create_shm_pool(address, size, alignment, zero, commit, arena_id);
}

// Static method
bool Shm_pool_collection::optional_remove_shm_pool_handler(extent_hooks_t* extent_hooks,
                                                           void* address,
                                                           size_t size,
                                                           bool committed,
                                                           unsigned arena_id)
{
  assert(extent_hooks != nullptr);
  Shm_pool_collection* collection = static_cast<Pages_wrapper*>(extent_hooks)->get_owner();
  return !collection->optional_remove_shm_pool(address, size, committed, arena_id);
}

// Static method
void Shm_pool_collection::remove_shm_pool_handler(extent_hooks_t* extent_hooks,
                                                  void* address,
                                                  size_t size,
                                                  bool committed,
                                                  unsigned arena_id)
{
  assert(extent_hooks != nullptr);
  Shm_pool_collection* collection = static_cast<Pages_wrapper*>(extent_hooks)->get_owner();
  collection->remove_shm_pool(address, size, committed, arena_id);
}

// Static method
bool Shm_pool_collection::commit_memory_pages_handler(extent_hooks_t* extent_hooks,
                                                      void* address,
                                                      size_t size,
                                                      size_t offset,
                                                      size_t length,
                                                      unsigned arena_id)
{
  assert(extent_hooks != nullptr);
  Shm_pool_collection* collection = static_cast<Pages_wrapper*>(extent_hooks)->get_owner();
  return !collection->commit_memory_pages(address, size, offset, length, arena_id);
}

// Static method
bool Shm_pool_collection::decommit_memory_pages_handler(extent_hooks_t* extent_hooks,
                                                        void* address,
                                                        size_t size,
                                                        size_t offset,
                                                        size_t length,
                                                        unsigned arena_id)
{
  assert(extent_hooks != nullptr);
  Shm_pool_collection* collection = static_cast<Pages_wrapper*>(extent_hooks)->get_owner();
  return !collection->decommit_memory_pages(address, size, offset, length, arena_id);
}

// Static method
bool Shm_pool_collection::purge_forced_memory_pages_handler(extent_hooks_t* extent_hooks,
                                                            void* address,
                                                            size_t size,
                                                            size_t offset,
                                                            size_t length,
                                                            unsigned arena_id)
{
  assert(extent_hooks != nullptr);
  Shm_pool_collection* collection = static_cast<Pages_wrapper*>(extent_hooks)->get_owner();
  return !collection->purge_forced_memory_pages(address, size, offset, length, arena_id);
}

// Static method
bool Shm_pool_collection::split_memory_pages_handler(extent_hooks_t* extent_hooks,
                                                     void* address,
                                                     size_t size,
                                                     size_t size_a,
                                                     size_t size_b,
                                                     bool committed,
                                                     unsigned arena_id)
{
  assert(extent_hooks != nullptr);

  Shm_pool_collection* collection = static_cast<Pages_wrapper*>(extent_hooks)->get_owner();
  return !collection->split_memory_pages(address, size, size_a, size_b, committed, arena_id);
}

// Static method
bool Shm_pool_collection::merge_memory_pages_handler(extent_hooks_t* extent_hooks,
                                                     void* address_a,
                                                     size_t size_a,
                                                     void* address_b,
                                                     size_t size_b,
                                                     bool committed,
                                                     unsigned arena_id)
{
  assert(extent_hooks != nullptr);
  Shm_pool_collection* collection = static_cast<Pages_wrapper*>(extent_hooks)->get_owner();
  return !collection->merge_memory_pages(address_a, size_a, address_b, size_b, committed, arena_id);
}

void* Shm_pool_collection::allocate(size_t size)
{
  assert(m_started);

  // Use the first arena
  auto iter = m_arenas.begin();
  assert(iter != m_arenas.end());
  return get_jemalloc_memory_manager()->allocate(size, *iter);
}

void* Shm_pool_collection::allocate(size_t size, Arena_id arena_id)
{
  assert(m_started);
  assert(m_arenas.find(arena_id) != m_arenas.end());

  return get_jemalloc_memory_manager()->allocate(size, arena_id);
}

void* Shm_pool_collection::allocate(size_t size, Arena_id arena_id, Thread_cache_id thread_cache_id)
{
  assert(m_started);
  assert(m_arenas.find(arena_id) != m_arenas.end());

  return get_jemalloc_memory_manager()->allocate(size, arena_id, thread_cache_id);
}

void Shm_pool_collection::deallocate(void* address)
{
  assert(m_started);

  // Use the first arena
  auto iter = m_arenas.begin();
  assert(iter != m_arenas.end());
  get_jemalloc_memory_manager()->deallocate(address, *iter);
}

void Shm_pool_collection::deallocate(void* address, Arena_id arena_id)
{
  assert(m_started);
  assert(m_arenas.find(arena_id) != m_arenas.end());

  get_jemalloc_memory_manager()->deallocate(address, arena_id);
}

void Shm_pool_collection::deallocate(void* address, Arena_id arena_id, Thread_cache_id thread_cache_id)
{
  assert(m_started);
  assert(m_arenas.find(arena_id) != m_arenas.end());

  get_jemalloc_memory_manager()->deallocate(address, arena_id, thread_cache_id);
}

void* Shm_pool_collection::create_shm_pool(void* address,
                                           size_t size,
                                           size_t alignment,
                                           bool* zero,
                                           bool* commit,
                                           Arena_id arena_id)
{
  assert(m_started);
  assert(zero != nullptr);
  assert(commit != nullptr);

  /* Ultra-unique ID generated here -- and only here. It's used as a key in various maps including
   * borrower-side global maps. */
  const auto id = detail::Shm_pool_offset_ptr_data_base::generate_pool_id();
  // It may also be encoded in the name, to provide uniqueness and for convenience in debugging and such.
  string name = generate_shm_object_name(id);

  shared_ptr<Shm_pool> pool =
    Owner_shm_pool_collection::create_shm_pool(
      id,
      name,
      size,
      address,
      [&](int fd, size_t size, void* address) -> void*
      {
        void* actual_address = Jemalloc_pages::map(address, size, alignment, *commit, fd);
        if (actual_address == nullptr)
        {
          // Handle error
          // @todo - Fill in
          return nullptr;
        }

        // Extracted (somewhat) from extent_mmap.c
        *zero = *commit;

        FLOW_LOG_TRACE("Mapped SHM pool at address " << actual_address);
        return actual_address;
      });

  void* pool_address;
  if (pool)
  {
    pool_address = pool->get_address();
    FLOW_LOG_TRACE("Created SHM pool at address " << pool_address << ", name '" << name <<
                   "', size " << size << ", arena " << arena_id);
  }
  else
  {
    // Failed to create pool
    pool_address = nullptr;
    FLOW_LOG_WARNING("Failed to create SHM pool of size " << size);
  }

  flow::log::Logger* logger = get_logger();
  if ((logger != nullptr) && logger->should_log(flow::log::Sev::S_TRACE, get_log_component()))
  {
    print_shm_pool_map();
  }

  return pool_address;
}

bool Shm_pool_collection::optional_remove_shm_pool([[maybe_unused]] void* address,
                                                   [[maybe_unused]] size_t size,
                                                   [[maybe_unused]] bool committed,
                                                   [[maybe_unused]] Arena_id arena_id)
{
  assert(m_started);
  // @todo - MGCOGS-385 - Create decision algorithm
  // Always retain for now
  return false;
  // return remove_shm_pool(address, size, committed, arena_id);
}

bool Shm_pool_collection::remove_shm_pool(void* address,
                                          size_t size,
                                          bool committed,
                                          Arena_id arena_id)
{
  assert(m_started);

  Memory_decommit_functor decommit_functor =
    [](const shared_ptr<Shm_pool>& shm_pool, size_t offset, size_t length) -> bool
    {
      void* page_address = add_offset(shm_pool->get_address(), offset);
      return Jemalloc_pages::decommit(page_address, shm_pool->get_fd(), offset, length, true);
    };

  bool removed_range;
  bool unmapped_pool;
  bool result =
    remove_range_and_pool_if_empty(address,
                                   size,
                                   (committed ? &decommit_functor : nullptr),
                                   removed_range,
                                   [](const shared_ptr<Shm_pool>& shm_pool) -> bool
                                   {
                                     Jemalloc_pages::unmap(shm_pool->get_address(), shm_pool->get_size());
                                     return true;
                                   },
                                   unmapped_pool);
  if (!result)
  {
    FLOW_LOG_WARNING("Failure when performing range removal at address " << address << ", size " << size <<
                     ", arena " << arena_id << ", removed range " << removed_range << ", unmapped pool " <<
                     unmapped_pool);
    return removed_range;
  }

  if (unmapped_pool)
  {
    FLOW_LOG_TRACE("Unmapped SHM pool when removing range at address " << address << ", size " << size <<
                   ", arena " << arena_id);
    print_shm_pool_map();
  }

  // If we removed the range, no physical memory would persist, although virtual memory leak may occur
  return removed_range;
}

bool Shm_pool_collection::commit_memory_pages(void* address,
                                              size_t size,
                                              size_t offset,
                                              size_t length,
                                              [[maybe_unused]] Arena_id arena_id)
{
  shared_ptr<Shm_pool> pool;
  // The offset from the start of the originally created pool (i.e., file offset)
  size_t pool_offset;
  if (!compute_pool_and_offset(address,
                               size,
                               offset,
                               length,
                               "committing",
                               pool,
                               pool_offset))
  {
    return false;
  }

  void* page_address = reinterpret_cast<void*>(reinterpret_cast<uint8_t*>(address) + offset);
  bool result = Jemalloc_pages::commit(page_address, length);

  FLOW_LOG_TRACE("Committing with success " << result << ", page address " << page_address << ", length " <<
                 length << " in pool " << *pool);

  return result;
}

bool Shm_pool_collection::decommit_memory_pages(void* address,
                                                size_t size,
                                                size_t offset,
                                                size_t length,
                                                [[maybe_unused]] Arena_id arena_id)
{
  shared_ptr<Shm_pool> pool;
  // The offset from the start of the originally created pool (i.e., file offset)
  size_t pool_offset;
  if (!compute_pool_and_offset(address,
                               size,
                               offset,
                               length,
                               "decommitting",
                               pool,
                               pool_offset))
  {
    return false;
  }

  void* page_address = reinterpret_cast<void*>(reinterpret_cast<uint8_t*>(address) + offset);
  bool result = Jemalloc_pages::decommit(page_address, pool->get_fd(), pool_offset, length);

  FLOW_LOG_TRACE("Decommitting with success " << result << ", page address " << page_address << ", pool offset " <<
                 pool_offset << ", length " << length << " in pool " << *pool);

  return result;
}

bool Shm_pool_collection::purge_forced_memory_pages(void* address,
                                                    size_t size,
                                                    size_t offset,
                                                    size_t length,
                                                    [[maybe_unused]] Arena_id arena_id)
{
  shared_ptr<Shm_pool> pool;
  // The offset from the start of the originally created pool (i.e., file offset)
  size_t pool_offset;
  if (!compute_pool_and_offset(address,
                               size,
                               offset,
                               length,
                               "force purging",
                               pool,
                               pool_offset))
  {
    return false;
  }

  bool result = Jemalloc_pages::purge_forced(pool->get_fd(), pool_offset, length);

  FLOW_LOG_TRACE("Force purge with success " << result << ", pool offset " << pool_offset << ", length " <<
                 length << " in pool " << *pool);

  return result;
}

bool Shm_pool_collection::split_memory_pages(const void* address,
                                             size_t size,
                                             size_t size_a,
                                             size_t size_b,
                                             [[maybe_unused]] bool committed,
                                             Arena_id arena_id)
{
  // There should not be an existing region that spans across segment boundaries, so it should always be okay to split
  FLOW_LOG_TRACE("Allowed split of memory pages at [" << address << "], size [" << size << "] to sizes [" << size_a <<
                 ", " << size_b << "], arena [" << arena_id << "]");

  return true;
}

bool Shm_pool_collection::merge_memory_pages(const void* address_a,
                                             size_t size_a,
                                             const void* address_b,
                                             size_t size_b,
                                             [[maybe_unused]] bool committed,
                                             Arena_id arena_id)
{
  shared_ptr<Shm_pool> pool_a = lookup_shm_pool(address_a);
  if (pool_a == nullptr)
  {
    FLOW_LOG_WARNING("Could not find shared memory pool for address [" << address_a << "] in merge request");
    return false;
  }

  shared_ptr<Shm_pool> pool_b = lookup_shm_pool(address_b);
  if (pool_b == nullptr)
  {
    FLOW_LOG_WARNING("Could not find shared memory pool for address [" << address_b << "] in merge request");
    return false;
  }

  if (pool_a != pool_b)
  {
    // Pools don't match, so we cannot merge. This situation can arise if the pools were originally created
    // adjacent to each other.
    FLOW_LOG_TRACE("Could not merge distinct pools from address A [" << address_a << "] and B [" << address_b << "]");
    return false;
  }

  if (!Shm_pool::is_adjacent(address_a, size_a, address_b, size_b))
  {
    // Address is not adjacent, which should not occur
    FLOW_LOG_WARNING("Merge request for non-adjacent regions (address, size) [" << address_a << ", " << size_a <<
                     "] and [" << address_b << ", " << size_b << "], arena [" << arena_id << "]");
    return false;
  }

  // Ensure both ranges are within the pool
  if (!pool_a->is_subset(address_a, size_a) || !pool_a->is_subset(address_b, size_b))
  {
    // One of the ranges is not a subset, which should not occur
    FLOW_LOG_WARNING("Merge request for a region (address, size) [" << address_a << ", " << size_a << "] or [" <<
                     address_b << ", " << size_b << "] that is not wholly within the pool [" << *pool_a << "]");
    return false;
  }

  FLOW_LOG_TRACE("Allowed merge of memory pages at [" << address_a << "], size [" << size_a << "] with [" <<
                 address_b << "], size [" << size_b << "], arena [" << arena_id << "]");

  return true;
}

bool Shm_pool_collection::compute_pool_and_offset(void* address,
                                                  size_t size,
                                                  size_t offset,
                                                  size_t length,
                                                  const string& use_case,
                                                  shared_ptr<Shm_pool>& pool,
                                                  size_t& pool_offset) const
{
  assert(m_started);
  assert(length > 0);

  pool = lookup_shm_pool(address);
  if (pool == nullptr)
  {
    // Failed
    FLOW_LOG_WARNING("When " << use_case << " pages, could not find pool with address " << address);
    return false;
  }

  Shm_pool::size_t address_offset;
  if (!pool->is_subset(address, size, &address_offset))
  {
    // Alert - specified range is not complete resident in pool
    FLOW_LOG_WARNING("Requested pool " << address << ", size " << size << " does not completely reside in pool");
    return false;
  }

  // Avoid overflow in checks
  if ((offset > size) || (length > size) || (offset + length) > size)
  {
    FLOW_LOG_WARNING("Requested offset " << offset << " and length " << length << " is > size " << size);
    return false;
  }

  pool_offset = address_offset + offset;
  return true;
}

shared_ptr<Shm_pool_collection::Thread_cache>
Shm_pool_collection::get_or_create_thread_cache(Arena_id arena_id)
{
  assert(m_started);
  assert(m_arenas.find(arena_id) != m_arenas.end());

  // Specific for current thread, so no lock needed
  shared_ptr<Thread_cache> thread_cache = s_thread_local_data.get_cache(arena_id);
  if (thread_cache != nullptr)
  {
    return thread_cache;
  }

  // No existing thread cache, so create one
  thread_cache = make_shared<Thread_cache>(shared_from_this(), arena_id);

  // Place in local storage
#ifndef NDEBUG
  bool result =
#endif
  s_thread_local_data.insert_cache(thread_cache);
  // We previously checked that the entry was not there, so we should not fail here
  assert(result);

  return thread_cache;
}

Shm_pool_collection::Object_deleter_no_cache::Object_deleter_no_cache(
  shared_ptr<Shm_pool_collection>&& pool_collection, Arena_id arena_id) :
  m_pool_collection(std::move(pool_collection)),
  m_arena_id(arena_id)
{
}

Shm_pool_collection::Object_deleter_cache::Object_deleter_cache(
  shared_ptr<Thread_cache>&& thread_cache) :
  m_thread_cache(std::move(thread_cache))
{
  assert(m_thread_cache != nullptr);
}

bool Shm_pool_collection::Thread_local_data::insert_cache(const shared_ptr<Thread_cache>& thread_cache)
{
  assert(thread_cache != nullptr);

  shared_ptr<Shm_pool_collection> owner = thread_cache->get_owner();
  FLOW_LOG_SET_CONTEXT(owner->get_logger(), owner->get_log_component());

  Arena_id arena_id = thread_cache->get_arena_id();
  shared_ptr<Thread_cache> existing_cache = get_cache(arena_id);
  if (existing_cache != nullptr)
  {
    if (existing_cache == thread_cache)
    {
      FLOW_LOG_WARNING("During insertion, unexpected existing similar cache for arena id " << arena_id <<
                       ", thread cache id " << thread_cache->get_thread_cache_id());
    }
    else
    {
      FLOW_LOG_WARNING("During insertion, unexpected existing different cache for arena id " << arena_id <<
                       ", existing thread cache id " << existing_cache->get_thread_cache_id() <<
                       ", attempted thread cache id " << thread_cache->get_thread_cache_id());
      return false;
    }
  }
  else
  {
    auto result_pair = m_thread_cache_map.emplace(arena_id, thread_cache);
    if (!result_pair.second)
    {
      // This should never happen as we already checked previously
      FLOW_LOG_WARNING("Unexpected insertion failure of pool collection");
      return false;
    }

    FLOW_LOG_TRACE("Inserted thread cache for arena id " << arena_id << ", thread cache id " <<
                   thread_cache->get_thread_cache_id());
  }

  return true;
}

bool Shm_pool_collection::Thread_local_data::remove_caches_by_owner(
  const shared_ptr<Shm_pool_collection>& owner)
{
  bool result = false;

  FLOW_LOG_SET_CONTEXT(owner->get_logger(), owner->get_log_component());

  // Find thread caches that match owner and remove them
  auto cur_iter = m_thread_cache_map.begin();
  const auto& end_iter = m_thread_cache_map.end();
  while (cur_iter != end_iter)
  {
    const shared_ptr<Thread_cache>& cur_thread_cache = cur_iter->second;
    if (owner == cur_thread_cache->get_owner())
    {
      FLOW_LOG_TRACE("Removing thread cache id " << cur_thread_cache->get_thread_cache_id() << ", arena id " <<
                     cur_thread_cache->get_arena_id());

      // Erasing iterator only invalidates the iterator erased, so save it, advance and then erase it
      auto save_iter = cur_iter;
      ++cur_iter;
      m_thread_cache_map.erase(save_iter);
      result = true;
    }
    else
    {
      ++cur_iter;
    }
  }

  return result;
}

void Shm_pool_collection::Thread_local_data::flush_all_caches()
{
  for (const auto& cur_iter : m_thread_cache_map)
  {
    const shared_ptr<Thread_cache>& cur_thread_cache = cur_iter.second;
    shared_ptr<Shm_pool_collection> owner = cur_thread_cache->get_owner();
    owner->get_jemalloc_memory_manager()->flush_thread_cache(cur_thread_cache->get_thread_cache_id());
  }
}

Shm_pool_collection::Thread_cache::Thread_cache(const shared_ptr<Shm_pool_collection>& owner,
                                                Arena_id arena_id) :
  m_owner(owner),
  m_arena_id(arena_id)
{
  assert(owner != nullptr);

  m_thread_cache_id = owner->create_thread_cache();
}

Shm_pool_collection::Thread_cache::~Thread_cache()
{
  m_owner->destroy_thread_cache(m_thread_cache_id);
}

} // namespace ipc::shm::arena_lend::jemalloc
