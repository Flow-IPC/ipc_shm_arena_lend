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

#include "ipc/shm/arena_lend/owner_shm_pool_listener_for_repository.hpp"
#include "ipc/shm/arena_lend/shm_pool.hpp"
#include "ipc/shm/arena_lend/shm_pool_repository_singleton.hpp"
#include "ipc/common.hpp"

using std::set;
using std::shared_ptr;
using std::string;

namespace ipc::shm::arena_lend
{

Owner_shm_pool_listener_for_repository::Owner_shm_pool_listener_for_repository(flow::log::Logger* logger) :
  flow::log::Log_context(logger, Log_component::S_SHM)
{
}

void Owner_shm_pool_listener_for_repository::notify_initial_shm_pools(const set<shared_ptr<Shm_pool>>& shm_pools)
{
  if (!shm_pools.empty())
  {
    for (const auto& cur_shm_pool : shm_pools)
    {
      // Only allowing this call in a non-performant manner as the general use case is that it is empty.
      notify_created_shm_pool(cur_shm_pool);
    }
  }
}

void Owner_shm_pool_listener_for_repository::notify_created_shm_pool(const shared_ptr<Shm_pool>& shm_pool)
{
  if (!Owner_shm_pool_repository_singleton::get_instance().insert(shm_pool))
  {
    FLOW_LOG_WARNING("Could not insert shm pool [" << shm_pool->get_id() << "]");
  }
}

void Owner_shm_pool_listener_for_repository::notify_removed_shm_pool(const shared_ptr<Shm_pool>& shm_pool)
{
  const auto shm_pool_id = shm_pool->get_id();
  if (!Owner_shm_pool_repository_singleton::get_instance().erase(shm_pool_id))
  {
    FLOW_LOG_WARNING("Could not remove shm pool [" << shm_pool_id << "]");
  }
}

} // namespace ipc::shm::arena_lend
