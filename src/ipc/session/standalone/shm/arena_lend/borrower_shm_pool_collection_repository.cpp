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

#include "ipc/session/standalone/shm/arena_lend/borrower_shm_pool_collection_repository.hpp"
#include "ipc/shm/arena_lend/borrower_shm_pool_collection.hpp"
#include "ipc/shm/arena_lend/shm_pool.hpp"
#include "ipc/common.hpp"

using std::make_shared;
using std::shared_ptr;
using std::size_t;
using std::string;

namespace ipc::session::shm::arena_lend
{

using ipc::shm::arena_lend::Shm_pool;

// Static variables
flow::log::Component Borrower_shm_pool_collection_repository::S_LOG_COMPONENT(Log_component::S_SESSION);

Borrower_shm_pool_collection_repository::Borrower_shm_pool_collection_repository() :
  m_logger(nullptr)
{
}

void Borrower_shm_pool_collection_repository::register_owner(Owner_id owner_id)
{
  Lock lock(m_owner_data_map_mutex);

  auto owner_result = m_owner_data_map.emplace(owner_id, Owner_data());
  if (owner_result.second)
  {
    FLOW_LOG_TRACE("Registered new owner [" << owner_id << "]");
  }
  else
  {
    auto& owner_data = owner_result.first->second;
    owner_data.increment_use();
    FLOW_LOG_TRACE("Incremented use of owner [" << owner_id << "] to [" << owner_data.get_use_count() << "]");
  }
}

bool Borrower_shm_pool_collection_repository::deregister_owner(Owner_id owner_id)
{
  Lock lock(m_owner_data_map_mutex);

  auto iter = m_owner_data_map.find(owner_id);
  if (iter == m_owner_data_map.end())
  {
    FLOW_LOG_WARNING("Could not find owner [" << owner_id << "]");
    return false;
  }

  auto& owner_data = iter->second;
  if (owner_data.get_use_count() <= 1)
  {
    m_owner_data_map.erase(iter);
    FLOW_LOG_TRACE("Removed owner [" << owner_id << "]");
  }
  else
  {
    owner_data.decrement_use();
    FLOW_LOG_TRACE("Decremented use of owner [" << owner_id << "] to [" << owner_data.get_use_count() << "]");
  }

  return true;
}

shared_ptr<Borrower_shm_pool_collection_repository::Borrower_shm_pool_collection>
  Borrower_shm_pool_collection_repository::register_collection(Owner_id owner_id, Collection_id collection_id)
{
  Lock lock(m_owner_data_map_mutex);

  auto owner_iter = m_owner_data_map.find(owner_id);
  if (owner_iter == m_owner_data_map.end())
  {
    FLOW_LOG_WARNING("Could not find owner [" << owner_id << "] when registering collection [" << collection_id <<
                     "]");
    return nullptr;
  }

  auto& owner_data = owner_iter->second;
  auto& collection_data_map = owner_data.get_collection_data_map();
  auto collection_iter = collection_data_map.find(collection_id);
  if (collection_iter != collection_data_map.end())
  {
    auto& collection_data = collection_iter->second;
    collection_data.increment_use();
    FLOW_LOG_TRACE("Incremented use of collection [" << collection_id << "], owner [" << owner_id << "] to [" <<
                   collection_data.get_use_count() << "]");
    return collection_data.get_collection();
  }

  // Create collection
  auto borrower_collection = make_shared<Borrower_shm_pool_collection>(get_logger(), collection_id);
  auto collection_result = collection_data_map.emplace(collection_id, Collection_data(borrower_collection));
  if (!collection_result.second)
  {
    FLOW_LOG_WARNING("Could not insert new collection [" << collection_id << "], owner [" << owner_id <<
                     "] after it was not found");
    return nullptr;
  }

  return borrower_collection;
}

bool Borrower_shm_pool_collection_repository::deregister_collection(Owner_id owner_id, Collection_id collection_id)
{
  Lock lock(m_owner_data_map_mutex);

  auto owner_iter = m_owner_data_map.find(owner_id);
  if (owner_iter == m_owner_data_map.end())
  {
    FLOW_LOG_WARNING("Could not find owner [" << owner_id << "] when deregistering collection [" << collection_id <<
                     "]");
    return false;
  }

  auto& collection_data_map = owner_iter->second.get_collection_data_map();
  auto collection_data_iter = collection_data_map.find(collection_id);
  if (collection_data_iter == collection_data_map.end())
  {
    FLOW_LOG_TRACE("Could not find collection [" << collection_id << "], owner [" << owner_id << "]");
    return false;
  }

  auto& collection_data = collection_data_iter->second;
  if (collection_data.get_use_count() <= 1)
  {
    // Erase collection
    collection_data_map.erase(collection_data_iter);
    FLOW_LOG_TRACE("Removed collection [" << collection_id << "], owner [" << owner_id << "]");
  }
  else
  {
    // Decrement count
    collection_data.decrement_use();
    FLOW_LOG_TRACE("Decremented counter of owner [" << owner_id << "], collection [" << collection_id << "] to [" <<
                   collection_data.get_use_count() << "]");
  }

  return true;
}

shared_ptr<Shm_pool> Borrower_shm_pool_collection_repository::register_shm_pool(Owner_id owner_id,
                                                                                Collection_id collection_id,
                                                                                pool_id_t shm_pool_id,
                                                                                const string& shm_pool_name,
                                                                                size_t shm_pool_size)
{
  Lock lock(m_owner_data_map_mutex);

  auto owner_iter = m_owner_data_map.find(owner_id);
  if (owner_iter == m_owner_data_map.end())
  {
    FLOW_LOG_WARNING("Could not find owner [" << owner_id << "] when registering SHM pool [" << shm_pool_id <<
                     "] in collection [" << collection_id << "]");
    return nullptr;
  }

  auto& collection_data_map = owner_iter->second.get_collection_data_map();
  auto collection_data_iter = collection_data_map.find(collection_id);
  if (collection_data_iter == collection_data_map.end())
  {
    FLOW_LOG_TRACE("Could not find collection [" << collection_id << "], owner [" << owner_id <<
                   "] when registering SHM pool [" << shm_pool_id << "]");
    return nullptr;
  }

  shared_ptr<Shm_pool> shm_pool;
  auto& collection_data = collection_data_iter->second;
  if (collection_data.find_shm_pool_id(shm_pool_id))
  {
    shm_pool = m_shm_pool_repository.increment_use(shm_pool_id);
    if (shm_pool == nullptr)
    {
      /* @todo Consider revisiting (much) code like this that could possibly be considered more of an assert()
       * situation. That would simplify many APIs and code flows. Note I am not stating for a fact that that's
       * appropriate. It is a feeling that it's something potentially to look into that could pay major
       * dividends. Point being, how much of this would be the result of clear bugs/misuse of (often internal
       * in practice?) APIs, and how often are such errors really recoverable-from in practice? -ygoldfel */

      // We had it recorded as registered, so there's an accounting issue
      FLOW_LOG_WARNING("SHM pool ID [" << shm_pool_id << "], name [" << shm_pool_name << "] registered, "
                       "but it's not found in the repository");
      return nullptr;
    }

    assert(size_t(shm_pool->get_size()) == shm_pool_size);
    FLOW_LOG_TRACE("Incremented use of SHM pool ID [" << shm_pool_id << "], name [" << shm_pool_name << "], "
                   "owner [" << owner_id << "], collection [" << collection_id << "]");
  }
  else
  {
    // SHM pool doesn't exist, so open and insert
    auto& collection = collection_data.get_collection();
    shm_pool = collection->open_shm_pool(shm_pool_id, shm_pool_name, shm_pool_size);
    if (shm_pool == nullptr)
    {
      FLOW_LOG_WARNING("Could not open SHM pool ID [" << shm_pool_id << "], name [" << shm_pool_name << "], "
                       "size [" << shm_pool_size << "], owner [" << owner_id << "]");
      return nullptr;
    }

    if (!collection_data.insert_shm_pool_id(shm_pool_id))
    {
      FLOW_LOG_WARNING("Could not insert SHM pool ID [" << shm_pool_id << "], name [" << shm_pool_name << "], "
                       "size [" << shm_pool_size << "], owner [" << owner_id << "]");
      // Undo previous action
      collection->release_shm_pool(shm_pool);
      return nullptr;
    }

    if (!m_shm_pool_repository.insert(shm_pool))
    {
      FLOW_LOG_WARNING("Could not insert newly opened SHM pool ID [" << shm_pool_id << "] name "
                       "[" << shm_pool_name << "], size [" << shm_pool_size <<
                       "], owner [" << owner_id << "], collection [" << collection_id << "] into repository");
      // Undo previous actions
      collection_data.remove_shm_pool_id(shm_pool_id);
      collection->release_shm_pool(shm_pool);
      return nullptr;
    }

    FLOW_LOG_TRACE("Inserted SHM pool ID [" << shm_pool_id << "], name [" << shm_pool_name << "], "
                   "size [" << shm_pool_size << "], owner [" << owner_id <<
                   "], collection [" << collection_id << "]");
  }

  return shm_pool;
}

bool Borrower_shm_pool_collection_repository::deregister_shm_pool(Owner_id owner_id,
                                                                  Collection_id collection_id,
                                                                  pool_id_t shm_pool_id)
{
  Lock lock(m_owner_data_map_mutex);

  const auto owner_iter = m_owner_data_map.find(owner_id);
  if (owner_iter == m_owner_data_map.end())
  {
    FLOW_LOG_WARNING("Could not find owner [" << owner_id << "] when deregistering SHM pool [" << shm_pool_id <<
                     "] in collection [" << collection_id << "]");
    return false;
  }

  auto& collection_data_map = owner_iter->second.get_collection_data_map();
  const auto collection_data_iter = collection_data_map.find(collection_id);
  if (collection_data_iter == collection_data_map.end())
  {
    FLOW_LOG_TRACE("Could not find collection [" << collection_id << "], owner [" << owner_id <<
                   "] when deregistering SHM pool [" << shm_pool_id << "]");
    return false;
  }

  auto& collection_data = collection_data_iter->second;
  if (!collection_data.find_shm_pool_id(shm_pool_id))
  {
    FLOW_LOG_WARNING("SHM pool [" << shm_pool_id << "] not registered for owner [" << owner_id << "] collection [" <<
                     collection_id << "]");
    return false;
  }

  unsigned int use_count;
  const auto shm_pool = m_shm_pool_repository.erase_or_decrement_use(shm_pool_id, use_count);
  if (shm_pool == nullptr)
  {
    FLOW_LOG_WARNING("Could not find SHM pool [" << shm_pool_id << "], owner [" << owner_id << "] collection [" <<
                     collection_id << "]");
    return false;
  }

  if (use_count == 0)
  {
    // No more references, so close the SHM pool
    auto& collection = collection_data.get_collection();
    if (!collection->release_shm_pool(shm_pool))
    {
      // Couldn't release for some reason
      FLOW_LOG_WARNING("Could not close SHM pool [" << shm_pool_id << "] in collection [" << collection_id <<
                       "], owner [" << owner_id << "]");
    }
    else
    {
      FLOW_LOG_TRACE("Successfully closed SHM pool [" << shm_pool_id << "] in collection [" << collection_id <<
                     "], owner [" << owner_id << "]");
    }

    // Deregister from collection data
    if (!collection_data.remove_shm_pool_id(shm_pool_id))
    {
      FLOW_LOG_WARNING("Could not deregister SHM pool [" << shm_pool_id << "] in collection [" << collection_id <<
                       "], owner [" << owner_id << "]");
    }

    FLOW_LOG_TRACE("Deregistered SHM pool [" << shm_pool_id << "] in collection [" << collection_id <<
                   "], owner [" << owner_id << "]");
  }
  else
  {
    FLOW_LOG_TRACE("Decremented counter to [" << use_count << "] for SHM pool [" << shm_pool_id <<
                   "] in collection [" << collection_id << "], owner [" << owner_id << "]");
  }

  return true;
}

Borrower_shm_pool_collection_repository::Collection_data::Collection_data(
  const shared_ptr<Borrower_shm_pool_collection>& collection) :
  m_collection(collection)
{
}

} // namespace ipc::session::shm::arena_lend
