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
#include <memory>

namespace ipc::shm::arena_lend
{

/**
 * Contains a shared memory pool.
 */
class Shm_pool_holder
{
public:
  /**
   * Constructor.
   *
   * @param shm_pool The shared memory pool to store.
   */
  Shm_pool_holder(const std::shared_ptr<Shm_pool>& shm_pool) :
    m_shm_pool(shm_pool)
  {
  }

  /**
   * Returns the stored shared memory pool.
   *
   * @return See above.
   */
  inline const std::shared_ptr<Shm_pool>& get_shm_pool() const
  {
    return m_shm_pool;
  }

private:
  /// The stored shared memory pool.
  const std::shared_ptr<Shm_pool> m_shm_pool;
}; // class Shm_pool_holder

} // namespace ipc::shm::arena_lend
