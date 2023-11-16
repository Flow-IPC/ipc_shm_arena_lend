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

#include "ipc/session/standalone/shm/arena_lend/lender_collection.hpp"
#include "ipc/shm/arena_lend/shm_pool.hpp"
#include "ipc/common.hpp"

using std::shared_ptr;

namespace ipc::session::shm::arena_lend
{

Lender_collection::Lender_collection(flow::log::Logger* logger,
                                     const shared_ptr<Shm_pool_collection>& shm_pool_collection) :
  flow::log::Log_context(logger, Log_component::S_SESSION),
  m_shm_pool_collection(shm_pool_collection)
{
}

bool Lender_collection::register_shm_pool(const shared_ptr<Shm_pool>& shm_pool)
{
  const auto shm_pool_id = shm_pool->get_id();
  auto result_pair = m_shm_pool_data_map.emplace(shm_pool_id, Shm_pool_data(shm_pool));
  if (!result_pair.second)
  {
    FLOW_LOG_WARNING("Could not register duplicate SHM pool [" << shm_pool_id << "] in collection [" <<
                     get_id() << "]");
    return false;
  }

  FLOW_LOG_TRACE("Registered SHM pool [" << shm_pool_id << "] in collection [" << get_id() << "]");
  return true;
}

bool Lender_collection::deregister_shm_pool(pool_id_t shm_pool_id, bool& was_empty)
{
  const auto map_node = m_shm_pool_data_map.extract(shm_pool_id);
  if (!map_node)
  {
    FLOW_LOG_WARNING("Could not deregister unregistered SHM pool [" << shm_pool_id << "] in collection [" <<
                     get_id() << "]");
    return false;
  }

  const auto& shm_pool_data = map_node.mapped();
  was_empty = shm_pool_data.empty();
  if (!was_empty)
  {
    FLOW_LOG_WARNING("Deregistered SHM pool ID [" << shm_pool_id << "] with remaining entries [" <<
                     shm_pool_data.size() << "] in collection [" << get_id() << "]");
  }
  else
  {
    FLOW_LOG_TRACE("Deregistered SHM pool [" << shm_pool_id << "] in collection [" << get_id() << "]");
  }

  return true;
}

bool Lender_collection::register_object(const shared_ptr<void>& object,
                                        pool_id_t& shm_pool_id, pool_offset_t& pool_offset)
{
  // Determine SHM pool and offset within SHM pool for the object
  void* const object_ptr = object.get();
  shared_ptr<Shm_pool> shm_pool = m_shm_pool_collection->lookup_shm_pool(object_ptr);
  if (!shm_pool)
  {
    FLOW_LOG_WARNING("Object [" << object_ptr << "] not located within collection [" << get_id() << "]");
    return false;
  }

  if (!shm_pool->determine_offset(object_ptr, pool_offset))
  {
    FLOW_LOG_WARNING("Object [" << object_ptr << "] is not located within SHM pool [" << *shm_pool <<
                     "] in collection [" << get_id() << "], but we just looked it up successfully");
    return false;
  }

  // Register object
  shm_pool_id = shm_pool->get_id();
  if (!register_object(shm_pool_id, pool_offset, object))
  {
    return false; // Out-args set but shall be ignored
  }

  return true;
}

bool Lender_collection::register_object(pool_id_t shm_pool_id, pool_offset_t pool_offset,
                                        const shared_ptr<void>& object)
{
  void* object_ptr = object.get();

  auto iter = m_shm_pool_data_map.find(shm_pool_id);
  if (iter == m_shm_pool_data_map.end())
  {
    FLOW_LOG_WARNING("SHM pool id [" << shm_pool_id << "] is not registered in collection [" << get_id() <<
                     "] when registering object [" << object_ptr << "] at offset [" << pool_offset << "]");
    return false;
  }

  iter->second.register_object(pool_offset, object);

  FLOW_LOG_TRACE("Registered object [" << object_ptr << "] in SHM pool [" << shm_pool_id << "] at offset [" <<
                 pool_offset << "] in collection [" << get_id() << "]");
  return true;
}

shared_ptr<void> Lender_collection::deregister_object(pool_id_t shm_pool_id, pool_offset_t pool_offset)
{
  const auto iter = m_shm_pool_data_map.find(shm_pool_id);
  if (iter == m_shm_pool_data_map.end())
  {
    FLOW_LOG_WARNING("SHM pool [" << shm_pool_id << "] not found, when deregistering object at offset [" <<
                     pool_offset << "] in collection [" << get_id() << "]");
    return nullptr;
  }

  const auto object = iter->second.deregister_object(pool_offset);
  if (object == nullptr)
  {
    FLOW_LOG_WARNING("Object at offset [" << pool_offset << "] not found, when deregistering in SHM pool [" <<
                     shm_pool_id << "] in collection [" << get_id() << "]");
  }
  else
  {
    FLOW_LOG_TRACE("Deregistered object in SHM pool [" << shm_pool_id << "] at offset [" << pool_offset <<
                   "] in collection [" << get_id() << "]");
  }

  return object;
}

Lender_collection::Shm_pool_data::Shm_pool_data(const shared_ptr<Shm_pool>& shm_pool) :
  m_shm_pool(shm_pool)
{
}

void Lender_collection::Shm_pool_data::register_object(pool_offset_t offset, const shared_ptr<void>& object)
{
  // Insert or get the iterator
  const auto result_pair = m_object_data_map.emplace(offset, Object_data(object));
  if (!result_pair.second)
  {
    // Increment the usage as we have a new registration
    ++result_pair.first->second.m_use_count;
  }
}

shared_ptr<void> Lender_collection::Shm_pool_data::deregister_object(pool_offset_t offset)
{
  const auto iter = m_object_data_map.find(offset);
  if (iter == m_object_data_map.end())
  {
    return nullptr;
  }

  auto& object_data = iter->second;
  if (object_data.m_use_count == 1)
  {
    // Last use, so save object before iterator invalidation and remove entry
    shared_ptr<void> object = std::move(object_data.m_object);
    m_object_data_map.erase(iter);
    return object;
  }
  // else

  // Decrement the usage
  --object_data.m_use_count;
  return object_data.m_object;
}

Lender_collection::Shm_pool_data::Object_data::Object_data(const shared_ptr<void>& object) :
  m_object(object),
  m_use_count(1)
{
}

} // namespace ipc::session::shm::arena_lend
