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

#pragma once

#include "ipc/shm/arena_lend/shm_pool.hpp"
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <mutex>

namespace ipc::shm::arena_lend::test
{

/**
 * Minimal fake SHM-pool repository for use in tests that need a Repository_type template argument
 * (e.g., for Shm_pool_offset_ptr_data or Shm_pool_offset_ptr) but don't need the real production
 * repositories (Owner_shm_pool_repository, Borrower_shm_pool_collection_repository).
 *
 * Singleton with simple mutex-guarded fwd/rev maps. Not optimized — for tests only.
 * Satisfies the `Repository_type` contract of detail::Shm_pool_offset_ptr_data:
 *   - `static void* to_address(pool_id_t, pool_offset_t)`
 *   - `static void from_address(const void*, pool_id_t&, pool_offset_t&)`
 *
 * Also provides `get_instance().insert()` / `get_instance().erase()` for test setup/teardown.
 */
class Test_shm_pool_repository
{
public:
  using pool_id_t = Shm_pool::pool_id_t;
  using pool_offset_t = Shm_pool::size_t;

  static Test_shm_pool_repository& get_instance()
  {
    static Test_shm_pool_repository s_instance;
    return s_instance;
  }

  void insert(std::shared_ptr<Shm_pool>&& shm_pool)
  {
    std::lock_guard lock(m_mutex);
    auto id = shm_pool->get_id();
    auto* base = shm_pool->get_address();
    auto [it, ok] = m_fwd.emplace(id, std::move(shm_pool));
    ASSERT_TRUE(ok) << "Double-insert of pool ID [" << id << "].";
    m_rev[base] = id;
  }

  void erase(pool_id_t pool_id)
  {
    std::lock_guard lock{m_mutex};
    auto it = m_fwd.find(pool_id);
    ASSERT_NE(it, m_fwd.end()) << "Erase of unknown pool ID [" << pool_id << "].";
    m_rev.erase(it->second->get_address());
    m_fwd.erase(it);
  }

  static void* to_address(pool_id_t pool_id, pool_offset_t pool_offset)
  {
    auto& self = get_instance();
    std::lock_guard lock{self.m_mutex};
    auto it = self.m_fwd.find(pool_id);
    if (it == self.m_fwd.end())
    {
      return nullptr;
    }
    return static_cast<uint8_t*>(it->second->get_address()) + pool_offset;
  }

  static void from_address(const void* address, pool_id_t& pool_id, pool_offset_t& pool_offset)
  {
    auto& self = get_instance();
    std::lock_guard lock{self.m_mutex};

    /* Reverse lookup: find the pool whose base address is <= address.
     * m_rev is sorted by base address (std::map). upper_bound gives first entry > address;
     * back up one to get the candidate. */
    auto it = self.m_rev.upper_bound(const_cast<void*>(address));
    if (it != self.m_rev.begin())
    {
      --it;
      auto* base = it->first;
      pool_id_t candidate_id = it->second;
      auto fwd_it = self.m_fwd.find(candidate_id);
      assert(fwd_it != self.m_fwd.end());
      const auto& pool = fwd_it->second;
      auto* pool_end = static_cast<const uint8_t*>(base) + pool->get_size();
      if (static_cast<const uint8_t*>(address) < pool_end)
      {
        pool_id = candidate_id;
        pool_offset = static_cast<pool_offset_t>(static_cast<const uint8_t*>(address)
                                                 - static_cast<const uint8_t*>(base));
        return;
      }
    }

    // Not found in any pool.
    pool_id = 0;
    pool_offset = 0;
  }

private:
  Test_shm_pool_repository() = default;

  mutable std::mutex m_mutex;
  // Fwd: pool_id -> Shm_pool.
  std::map<pool_id_t, std::shared_ptr<Shm_pool>> m_fwd;
  // Rev: base_address -> pool_id.  Sorted by address for upper_bound reverse lookup.
  std::map<void*, pool_id_t> m_rev;
}; // class Test_shm_pool_repository

} // namespace ipc::shm::arena_lend::test
