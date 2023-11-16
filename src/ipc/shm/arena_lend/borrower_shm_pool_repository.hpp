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

#include "ipc/shm/arena_lend/detail/shm_pool_use_tracker.hpp"
#include "ipc/shm/arena_lend/shm_pool_repository.hpp"

namespace ipc::shm::arena_lend
{

/**
 * Container for shared memory pools on the borrower side. The shared memory pool identifiers not pointing to the
 * same shared memory object must be distinct from each other. This repository supports multiple registrations
 * via a use count. The superclass #ipc::shm::arena_lend::Shm_pool_repository<detail::Shm_pool_use_tracker>::erase()
 * interface is suppressed in favor or #erase_or_decrement_use().
 */
class Borrower_shm_pool_repository :
  protected Shm_pool_repository<detail::Shm_pool_use_tracker>
{
public:
  // Make public
  using pool_id_t = Shm_pool_repository<detail::Shm_pool_use_tracker>::pool_id_t;
  using pool_offset_t = Shm_pool_repository<detail::Shm_pool_use_tracker>::pool_offset_t;
  using Shm_pool_repository<detail::Shm_pool_use_tracker>::to_address;
  using Shm_pool_repository<detail::Shm_pool_use_tracker>::from_address;
  using Shm_pool_repository<detail::Shm_pool_use_tracker>::insert;

  /**
   * Increments the use counter of the shared memory pool, if it exists.
   *
   * @param shm_pool_id The identifier of a shared memory pool to increment the use of.
   *
   * @return If found, the shared memory pool that was incremented in use; otherwise, nullptr.
   */
  std::shared_ptr<Shm_pool> increment_use(pool_id_t shm_pool_id);
  /**
   * Removes a shared memory pool. If a shared memory pool was previously inserted multiple times, the counter
   * will only be decremented.
   *
   * @param shm_pool_id The identifier of a shared memory pool to remove.
   * @param use_count If the shared memory pool was found, the use count after decrementing or removing (0).
   *
   * @return If found, the shared memory pool that was removed or decremented in use; otherwise, nullptr.
   */
  std::shared_ptr<Shm_pool> erase_or_decrement_use(pool_id_t shm_pool_id, unsigned int& use_count);
}; // class Borrower_shm_pool_repository

} // namespace ipc::shm::arena_lend
