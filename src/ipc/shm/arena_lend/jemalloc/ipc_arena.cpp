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

#include "ipc/shm/arena_lend/jemalloc/ipc_arena.hpp"
#include "ipc/shm/arena_lend/owner_shm_pool_listener.hpp"

using std::shared_ptr;
using std::size_t;
using std::string;
using std::string_view;
using std::weak_ptr;

namespace ipc::shm::arena_lend::jemalloc
{

// Static variables
std::atomic<Collection_id> Ipc_arena::m_collection_id_counter(0);

// Static method
shared_ptr<Ipc_arena> Ipc_arena::create(
  flow::log::Logger* logger,
  const shared_ptr<Memory_manager>& memory_manager,
  Shm_object_name_generator&& name_generator,
  const util::Permissions& permissions)
{
  auto arena = shared_ptr<Ipc_arena>(
    new Ipc_arena(logger, memory_manager, std::move(name_generator), permissions));
  if (!arena->start())
  {
    return nullptr;
  }

  return arena;
}

Ipc_arena::Ipc_arena(flow::log::Logger* logger,
                     const shared_ptr<Memory_manager>& memory_manager,
                     Shm_object_name_generator&& name_generator,
                     const util::Permissions& permissions) :
  Shm_pool_collection(logger,
                      ++m_collection_id_counter,
                      memory_manager,
                      std::move(name_generator),
                      permissions),
  m_event_listener(*this)
{
  // If we wrap around, abort
  Collection_id id = get_id();
  assert(id != 0);

  if (!add_event_listener(&m_event_listener))
  {
    FLOW_LOG_WARNING("Could not add self as event listener for collection [" << id << "]");
  }
}

Ipc_arena::~Ipc_arena()
{
  if (!remove_event_listener(&m_event_listener))
  {
    FLOW_LOG_WARNING("Could not remove self as event listener for arena id [" << get_id() << "]");
  }

  // Inform listeners of the shared memory pools that the pools will be removed now. In essence, they will be removed
  // shortly, but this class will be destroyed first and thus, any registered listeners cannot be notified. There
  // should be no handles remaining within the pools, because we should not be destroying this class if there was.
  {
    Lock lock(m_shm_pools_and_listeners_mutex);

    FLOW_LOG_TRACE("Notifying listeners of [" << m_shm_pools.size() << "] shared memory pool removals");
    for (const auto& cur_shm_pool : m_shm_pools)
    {
      // Notify listeners
      for (auto cur_listener : m_listeners)
      {
        cur_listener->notify_removed_shm_pool(cur_shm_pool);
      }
    }
  }
}

void* Ipc_arena::allocate(size_t size)
{
  return Shm_pool_collection::allocate(size);
}

void Ipc_arena::deallocate(void* address)
{
  return Shm_pool_collection::deallocate(address);
}

bool Ipc_arena::add_shm_pool_listener(Owner_shm_pool_listener* listener)
{
  {
    Lock lock(m_shm_pools_and_listeners_mutex);

    auto result_pair = m_listeners.emplace(listener);
    if (!result_pair.second)
    {
      FLOW_LOG_WARNING("Could not add already existing SHM pool listener [" << listener << "] to collection [" <<
                       get_id() << "]");
      return false;
    }

    listener->notify_initial_shm_pools(m_shm_pools);
  }

  FLOW_LOG_TRACE("Successfully added SHM pool listener [" << listener << "] to collection [" << get_id() << "]");

  return true;
}

bool Ipc_arena::remove_shm_pool_listener(Owner_shm_pool_listener* listener)
{
  {
    Lock lock(m_shm_pools_and_listeners_mutex);

    if (m_listeners.erase(listener) == 0)
    {
      FLOW_LOG_WARNING("Could not remove non-existent SHM pool listener [" << listener << "] from collection [" <<
                       get_id() << "]");
      return false;
    }
  }

  FLOW_LOG_TRACE("Successfully removed SHM pool listener [" << listener << "] from collection [" << get_id() << "]");

  return true;
}

void Ipc_arena::handle_created_shm_pool(const shared_ptr<Shm_pool>& shm_pool)
{
  const auto shm_pool_id = shm_pool->get_id();

  {
    // Register SHM pool
    Lock lock(m_shm_pools_and_listeners_mutex);

    auto result_pair = m_shm_pools.emplace(shm_pool);
    if (!result_pair.second)
    {
      // This will eventually lead to inconsistency and issues, so abort
      FLOW_LOG_FATAL("Could not insert SHM pool [" << *shm_pool << "], existing registration [" <<
                     *(result_pair.first) << "] in collection [" << get_id() << "]");
      std::abort();
      return;
    }
    FLOW_LOG_TRACE("Registered SHM pool [" << shm_pool_id << "] in collection [" << get_id() << "]");

    // Notify listeners
    for (auto cur_listener : m_listeners)
    {
      cur_listener->notify_created_shm_pool(shm_pool);
    }
  }

  FLOW_LOG_TRACE("Successfully handled SHM pool creation notification of pool [" << shm_pool_id <<
                 "] in collection [" << get_id() << "]");
}

void Ipc_arena::handle_removed_shm_pool(const shared_ptr<Shm_pool>& shm_pool)
{
  const auto shm_pool_id = shm_pool->get_id();

  {
    // Remove SHM pool
    Lock lock(m_shm_pools_and_listeners_mutex);

    if (m_shm_pools.erase(shm_pool) == 0)
    {
      FLOW_LOG_WARNING("SHM pool [" << shm_pool_id << "] not found in collection [" << get_id() << "]");
      return;
    }

    FLOW_LOG_TRACE("Deregistered SHM pool [" << shm_pool_id << "] from collection [" << get_id() << "]");

    // Notify listeners
    for (auto cur_listener : m_listeners)
    {
      cur_listener->notify_removed_shm_pool(shm_pool);
    }
  }

  FLOW_LOG_TRACE("Successfully handled SHM pool removal notification of pool [" << shm_pool_id <<
                 "] in collection [" << get_id() << "]");
}

shared_ptr<Shm_pool> Ipc_arena::lookup_shm_pool_by_id(pool_id_t shm_pool_id) const
{
  // @todo - Use map
  for (const auto& cur_shm_pool : m_shm_pools)
  {
    if (cur_shm_pool->get_id() == shm_pool_id)
    {
      FLOW_LOG_TRACE("Found shm pool with id [" << shm_pool_id << "], SHM pool [" << *cur_shm_pool << "]");
      return cur_shm_pool;
    }
  }

  FLOW_LOG_WARNING("Could not locate shm pool with id [" << shm_pool_id << "]");

  return nullptr;
}

Ipc_arena::Event_listener_impl::Event_listener_impl(Ipc_arena& owner) :
  m_owner(owner)
{
}

void Ipc_arena::Event_listener_impl::notify_created_shm_pool(const shared_ptr<Shm_pool>& shm_pool)
{
  m_owner.handle_created_shm_pool(shm_pool);
}

void Ipc_arena::Event_listener_impl::notify_removed_shm_pool(const shared_ptr<Shm_pool>& shm_pool,
                                                             bool)
{
  m_owner.handle_removed_shm_pool(shm_pool);
}

Ipc_arena::Ipc_object_deleter_no_cache::Ipc_object_deleter_no_cache(
  shared_ptr<Shm_pool_collection>&& pool_collection, Arena_id arena_id) :
  Object_deleter_no_cache(std::move(pool_collection), arena_id)
{
}

Ipc_arena::Ipc_object_deleter_cache::Ipc_object_deleter_cache(
  shared_ptr<Thread_cache>&& thread_cache) :
  Object_deleter_cache(std::move(thread_cache))
{
}

std::ostream& operator<<(std::ostream& os, const Ipc_arena& val)
{
  // @todo Something more useful than just this?
  return os << '@' << &val;
}

} // namespace ipc::shm::arena_lend::jemalloc
