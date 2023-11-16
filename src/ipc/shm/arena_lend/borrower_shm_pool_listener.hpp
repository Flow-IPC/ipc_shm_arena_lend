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

namespace ipc::shm::arena_lend
{

/**
 * Interface to listen for shared memory pool events that occur on the borrower side.
 */
class Borrower_shm_pool_listener
{
public:
  /**
   * This method will be called after opening a shared memory pool.
   *
   * @param shm_pool The shared memory pool that was opened.
   */
  virtual void notify_opened_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool) = 0;
  /**
   * This method will be called after a shared memory pool was closed.
   *
   * @param shm_pool_id The identifier of the shared memory pool that was closed.
   */
  virtual void notify_closed_shm_pool(Shm_pool::pool_id_t shm_pool_id) = 0;
}; // class Borrower_shm_pool_listener

} // namespace ipc::shm::arena_lend
