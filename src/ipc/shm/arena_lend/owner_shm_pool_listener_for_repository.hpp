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

#include "ipc/shm/arena_lend/owner_shm_pool_listener.hpp"
#include <flow/log/log.hpp>

namespace ipc::shm::arena_lend
{

/**
 * Implementation of a listener that records the shared memory pools in a singleton repository. The use case for
 * this is supporting containers holding handles to shared memory objects. This should be registered only once
 * per arena and only removed after the arena is destroyed; otherwise, the repository may incur inconsistent state.
 *
 * @see Shm_pool_offset_ptr
 */
class Owner_shm_pool_listener_for_repository :
  public flow::log::Log_context,
  public Owner_shm_pool_listener
{
public:
  /**
   * Constructor.
   *
   * @param logger Used for logging purposes.
   */
  Owner_shm_pool_listener_for_repository(flow::log::Logger* logger);

  /**
   * Registers the shared memory pools with the repository.
   *
   * @param shm_pools The current set of active SHM pools, which may be empty.
   */
  virtual void notify_initial_shm_pools(const std::set<std::shared_ptr<Shm_pool>>& shm_pools) override;
  /**
   * Registers the shared memory pool with the repository.
   *
   * @param shm_pool The shared memory pool that was created.
   */
  virtual void notify_created_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool) override;
  /**
   * Deregisters the shared memory pool from the repository.
   *
   * @param shm_pool The shared memory pool that was removed.
   */
  virtual void notify_removed_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool) override;
}; // class Owner_shm_pool_listener_for_repository

} // namespace ipc::shm::arena_lend
