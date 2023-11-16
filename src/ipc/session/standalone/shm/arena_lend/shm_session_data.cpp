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

#include "ipc/session/standalone/shm/arena_lend/shm_session_data.hpp"
#include "ipc/common.hpp"

using std::make_unique;
using std::shared_ptr;
using std::size_t;
using std::string;

namespace ipc::session::shm::arena_lend
{

Shm_session_data::Shm_session_data(flow::log::Logger* logger) :
  flow::log::Log_context(logger, Log_component::S_SESSION)
{
}

bool Shm_session_data::register_lender_collection(const shared_ptr<Shm_pool_collection>& shm_pool_collection)
{
  Collection_id collection_id = shm_pool_collection->get_id();

  {
    Lock lock(m_lender_mutex);

    auto result_pair =
      m_lender_collection_map.emplace(collection_id,
                                      make_unique<Lender_collection>(get_logger(), shm_pool_collection));
    if (!result_pair.second)
    {
      FLOW_LOG_WARNING("Could not register duplicate collection [" << collection_id << "], original pointer [" <<
                       result_pair.first->second.get() << "], new pointer [" << shm_pool_collection.get() << "]");
      return false;
    }
  }

  return true;
}

bool Shm_session_data::deregister_lender_collection(Collection_id collection_id)
{
  Lock lock(m_lender_mutex);

  if (m_lender_collection_map.erase(collection_id) == 0)
  {
    FLOW_LOG_WARNING("Could not deregister unregistered collection [" << collection_id << "]");
    return false;
  }

  return true;
}

bool Shm_session_data::register_lender_shm_pool(Collection_id collection_id, const shared_ptr<Shm_pool>& shm_pool)
{
  Lock lock(m_lender_mutex);

  auto iter = m_lender_collection_map.find(collection_id);
  if (iter == m_lender_collection_map.end())
  {
    FLOW_LOG_WARNING("Could not find lender collection [" << collection_id <<
                     "] when registering lender SHM pool [" << *shm_pool << "]");
    return false;
  }

  return iter->second->register_shm_pool(shm_pool);
}

bool Shm_session_data::deregister_lender_shm_pool(Collection_id collection_id,
                                                  const shared_ptr<Shm_pool>& shm_pool,
                                                  bool& was_empty)
{
  Lock lock(m_lender_mutex);

  auto iter = m_lender_collection_map.find(collection_id);
  if (iter == m_lender_collection_map.end())
  {
    FLOW_LOG_WARNING("Could not find lender collection [" << collection_id <<
                     "] when deregistering lender SHM pool [" << *shm_pool << "]");
    return false;
  }

  return iter->second->deregister_shm_pool(shm_pool->get_id(), was_empty);
}

bool Shm_session_data::register_lender_object(Collection_id collection_id,
                                              const shared_ptr<void>& object,
                                              pool_id_t& shm_pool_id,
                                              pool_offset_t& pool_offset)
{
  Lock lock(m_lender_mutex);

  auto iter = m_lender_collection_map.find(collection_id);
  if (iter == m_lender_collection_map.end())
  {
    FLOW_LOG_WARNING("Could not find lender collection [" << collection_id <<
                     "] when registering lender object [" << object.get() << "]");
    return false;
  }

  return iter->second->register_object(object, shm_pool_id, pool_offset);
}

shared_ptr<void> Shm_session_data::deregister_lender_object(Collection_id collection_id,
                                                            pool_id_t shm_pool_id,
                                                            pool_offset_t pool_offset)
{
  Lock lock(m_lender_mutex);

  auto iter = m_lender_collection_map.find(collection_id);
  if (iter == m_lender_collection_map.end())
  {
    FLOW_LOG_WARNING("Could not find lender collection [" << collection_id <<
                     "] when deregistering lender object with SHM pool id [" <<
                     shm_pool_id << "] at offset [" << pool_offset << "]");
    return nullptr;
  }

  return iter->second->deregister_object(shm_pool_id, pool_offset);
}

bool Shm_session_data::register_borrower_collection(
  const shared_ptr<Borrower_shm_pool_collection>& shm_pool_collection)
{
  Collection_id collection_id = shm_pool_collection->get_id();

  {
    Lock lock(m_borrower_mutex);

    auto result_pair =
      m_borrower_collection_map.emplace(collection_id,
                                        make_unique<Borrower_collection>(get_logger(), shm_pool_collection));
    if (!result_pair.second)
    {
      FLOW_LOG_WARNING("Could not register duplicate collection [" << collection_id << "], original pointer [" <<
                       result_pair.first->second.get() << "], new pointer [" << shm_pool_collection.get() << "]");
      return false;
    }
  }

  return true;
}

bool Shm_session_data::register_borrower_shm_pool(Collection_id collection_id, const shared_ptr<Shm_pool>& shm_pool)
{
  Lock lock(m_borrower_mutex);

  auto iter = m_borrower_collection_map.find(collection_id);
  if (iter == m_borrower_collection_map.end())
  {
    FLOW_LOG_WARNING("Could not find borrower collection [" << collection_id <<
                     "] when registering borrower SHM pool [" << shm_pool->get_id() << "]");
    return false;
  }

  auto& borrower_collection = iter->second;
  return borrower_collection->register_shm_pool(shm_pool);
}

bool Shm_session_data::deregister_borrower_shm_pool(Collection_id collection_id, pool_id_t shm_pool_id)
{
  Lock lock(m_borrower_mutex);

  auto iter = m_borrower_collection_map.find(collection_id);
  if (iter == m_borrower_collection_map.end())
  {
    FLOW_LOG_WARNING("Could not find borrower collection [" << collection_id <<
                     "] when deregistering borrower SHM pool [" << shm_pool_id << "]");
    return false;
  }

  auto& borrower_collection = iter->second;
  return bool(borrower_collection->deregister_shm_pool(shm_pool_id));
}

bool Shm_session_data::deregister_borrower_object(Collection_id collection_id,
                                                  pool_id_t shm_pool_id,
                                                  pool_offset_t pool_offset)
{
  Lock lock(m_borrower_mutex);

  const auto iter = m_borrower_collection_map.find(collection_id);
  if (iter == m_borrower_collection_map.end())
  {
    FLOW_LOG_WARNING("Could not find lender collection [" << collection_id <<
                     "] when deregistering borrower object with SHM pool id [" <<
                     shm_pool_id << "] at offset [" << pool_offset << "]");
    return false;
  }

  return iter->second->deregister_object(shm_pool_id, pool_offset);
}

} // namespace ipc::session::shm::arena_lend
