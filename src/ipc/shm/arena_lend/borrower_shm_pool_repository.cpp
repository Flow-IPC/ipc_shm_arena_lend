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

#include "ipc/shm/arena_lend/borrower_shm_pool_repository.hpp"

using std::shared_ptr;
using std::string;

namespace ipc::shm::arena_lend
{

shared_ptr<Shm_pool> Borrower_shm_pool_repository::increment_use(pool_id_t shm_pool_id)
{
  Lock lock(get_shm_pool_maps_mutex());

  auto& shm_pool_id_map = get_shm_pool_id_map();
  const auto iter = shm_pool_id_map.find(shm_pool_id);
  if (iter == shm_pool_id_map.end())
  {
    return nullptr;
  }

  auto& shm_pool_data = iter->second;
  shm_pool_data.increment_use();
  return shm_pool_data.get_shm_pool();
}

shared_ptr<Shm_pool> Borrower_shm_pool_repository::erase_or_decrement_use(pool_id_t shm_pool_id,
                                                                          unsigned int& use_count)
{
  Lock lock(get_shm_pool_maps_mutex());

  auto& shm_pool_id_map = get_shm_pool_id_map();
  auto iter = shm_pool_id_map.find(shm_pool_id);
  if (iter == shm_pool_id_map.end())
  {
    return nullptr;
  }

  auto& shm_pool_data = iter->second;
  // Save SHM pool handle before iterator is invalidated
  auto shm_pool = shm_pool_data.get_shm_pool();
  if (shm_pool_data.get_use_count() <= 1)
  {
    // Remove the entry
    use_count = 0;
    // Iterator is now invalid
    erase_locked(iter);
  }
  else
  {
    // Decrement the counter
    shm_pool_data.decrement_use();
    use_count = shm_pool_data.get_use_count();
  }

  return shm_pool;
}

} // namespace ipc::shm::arena_lend
