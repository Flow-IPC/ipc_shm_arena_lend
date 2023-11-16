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

#include "ipc/shm/arena_lend/arena_lend_fwd.hpp"
#include <memory>
#include <set>

namespace ipc::shm::arena_lend
{

/**
 * Listener interface to obtain owner shared memory pool notification events.
 */
class Owner_shm_pool_listener
{
public:
  /**
   * This method will be called upon initial registration in a synchronous manner and thus, this will only
   * be called once.
   *
   * @param shm_pools The current set of active SHM pools, which may be empty.
   */
  virtual void notify_initial_shm_pools(const std::set<std::shared_ptr<Shm_pool>>& shm_pools) = 0;
  /**
   * This method will be called upon creation of a shared memory pool.
   *
   * @param shm_pool The shared memory pool that was created.
   */
  virtual void notify_created_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool) = 0;
  /**
   * This method will be called upon removal of a shared memory pool. No objects should be resident within this
   * pool at the time of removal.
   *
   * @param shm_pool The shared memory pool that is being removed.
   */
  virtual void notify_removed_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool) = 0;
}; // class Owner_shm_pool_listener

} // namespace ipc::shm::arena_lend
