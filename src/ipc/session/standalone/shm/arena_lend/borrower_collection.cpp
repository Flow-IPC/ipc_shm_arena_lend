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

#include "ipc/session/standalone/shm/arena_lend/borrower_collection.hpp"
#include "ipc/shm/arena_lend/shm_pool.hpp"
#include "ipc/common.hpp"

using std::set;
using std::shared_ptr;
using ipc::shm::arena_lend::Shm_pool;

namespace ipc::session::shm::arena_lend
{

// Static constants
const shared_ptr<Shm_pool> Borrower_collection::S_EMPTY_SHM_POOL;

Borrower_collection::Borrower_collection(
  flow::log::Logger* logger,
  const shared_ptr<Borrower_shm_pool_collection>& borrower_shm_pool_collection) :
  flow::log::Log_context(logger, Log_component::S_SESSION),
  m_borrower_shm_pool_collection(borrower_shm_pool_collection)
{
}

bool Borrower_collection::register_shm_pool(const shared_ptr<Shm_pool>& shm_pool)
{
  const auto shm_pool_id = shm_pool->get_id();
  auto result_pair = m_shm_pool_data_map.emplace(shm_pool_id, Shm_pool_data(shm_pool));
  if (!result_pair.second)
  {
    FLOW_LOG_WARNING("Could not register duplicate SHM pool [" << shm_pool_id << "] in collection [" << get_id() <<
                     "]");
    return false;
  }

  FLOW_LOG_TRACE("Registered SHM pool [" << shm_pool_id << "] in collection [" << get_id() << "]");
  return true;
}

shared_ptr<Shm_pool> Borrower_collection::deregister_shm_pool(pool_id_t shm_pool_id)
{
  auto iter = m_shm_pool_data_map.find(shm_pool_id);
  if (iter == m_shm_pool_data_map.end())
  {
    FLOW_LOG_WARNING("Could not deregister non-existent SHM pool [" << shm_pool_id << "] in collection [" <<
                     get_id() << "]");
    return nullptr;
  }

  const auto& shm_pool_data = iter->second;
  const auto& offset_map = shm_pool_data.m_offset_map;
  if (!offset_map.empty())
  {
    FLOW_LOG_WARNING("Could not deregister non-empty SHM pool [" << shm_pool_id << "] with remaining entries [" <<
                     offset_map.size() << "] in collection [" << get_id() << "]");
    return nullptr;
  }

  auto shm_pool = std::move(shm_pool_data.m_shm_pool); // Save before erasure of *&shm_pool_data from map.
  m_shm_pool_data_map.erase(iter);
  FLOW_LOG_TRACE("Deregistered SHM pool [" << shm_pool_id << "] in collection [" << get_id() << "]");

  return shm_pool;
}

const shared_ptr<Shm_pool>& Borrower_collection::find_shm_pool(pool_id_t shm_pool_id) const
{
  auto iter = m_shm_pool_data_map.find(shm_pool_id);
  if (iter == m_shm_pool_data_map.end())
  {
    return S_EMPTY_SHM_POOL;
  }

  return iter->second.m_shm_pool;
}

bool Borrower_collection::register_object(pool_id_t shm_pool_id, pool_offset_t pool_offset)
{
  auto iter = m_shm_pool_data_map.find(shm_pool_id);
  if (iter == m_shm_pool_data_map.end())
  {
    FLOW_LOG_WARNING("SHM pool [" << shm_pool_id << "] not registered, when registering object at offset [" <<
                     pool_offset << "], collection [" << get_id() << "]");
    return false;
  }

  auto& offset_map = iter->second.m_offset_map;
  auto result_pair = offset_map.emplace(pool_offset, 1);
  if (!result_pair.second)
  {
    // Offset already exists, so increment the use count
    auto& use_count = result_pair.first->second;
    ++use_count;

    FLOW_LOG_TRACE("Registered existing object in SHM pool [" << shm_pool_id << "] at offset [" << pool_offset <<
                   "], collection [" << get_id() << "], use count [" << use_count << "]");
  }
  else
  {
    FLOW_LOG_TRACE("Registered new object in SHM pool [" << shm_pool_id << "] at offset [" << pool_offset <<
                   "], collection [" << get_id() << "]");
  }

  return true;
}

bool Borrower_collection::deregister_object(pool_id_t shm_pool_id, pool_offset_t pool_offset)
{
  auto shm_pool_data_map_iter = m_shm_pool_data_map.find(shm_pool_id);
  if (shm_pool_data_map_iter == m_shm_pool_data_map.end())
  {
    FLOW_LOG_WARNING("SHM pool [" << shm_pool_id << "] not registered, when deregistering object at offset [" <<
                     pool_offset << "], collection [" << get_id() << "]");
    return false;
  }

  auto& offset_map = shm_pool_data_map_iter->second.m_offset_map;
  auto offset_map_iter = offset_map.find(pool_offset);
  if (offset_map_iter == offset_map.end())
  {
    FLOW_LOG_WARNING("Offset [" << pool_offset << "] not found, when deregistering from SHM pool [" <<
                     shm_pool_id << "], collection [" << get_id() << "]");
    return false;
  }

  unsigned int& use_count = offset_map_iter->second;
  if (use_count > 1)
  {
    // Decrement the use count
    --use_count;

    FLOW_LOG_TRACE("Deregistered object in SHM pool [" << shm_pool_id << "] at offset [" << pool_offset <<
                   "], collection [" << get_id() << "], use count [" << use_count << "]");
  }
  else
  {
    // Remove entry
    if (offset_map_iter->second == 0)
    {
      FLOW_LOG_WARNING("Object use count is zero in SHM pool [" << shm_pool_id << "] at offset [" << pool_offset <<
                       "], collection [" << get_id() << "]");
    }
    offset_map.erase(offset_map_iter);

    FLOW_LOG_TRACE("Deregistered and removed object in SHM pool [" << shm_pool_id << "] at offset [" << pool_offset <<
                   "], collection [" << get_id() << "]");
  }

  return true;
}

set<Borrower_collection::pool_id_t> Borrower_collection::get_shm_pool_ids() const
{
  set<pool_id_t> shm_pool_ids;
  for (const auto& cur_map_pair : m_shm_pool_data_map)
  {
    shm_pool_ids.insert(cur_map_pair.first);
  }

  return shm_pool_ids;
}

Borrower_collection::Shm_pool_data::Shm_pool_data(const shared_ptr<Shm_pool>& shm_pool) :
  m_shm_pool(shm_pool)
{
}

} // namespace ipc::session::shm::arena_lend
