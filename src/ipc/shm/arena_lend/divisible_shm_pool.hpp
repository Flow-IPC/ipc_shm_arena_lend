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

namespace ipc::shm::arena_lend
{

/**
 * Mapped contiguous memory backed by a named shared memory object that can be logically divisible. It includes
 * a loose tracking mechanism to aid in determining when the entirety of the pool has been removed, which is done
 * by checking the remaining size of the memory pool and not actual addresses. The reasoning behind this is that
 * the memory manager will be managing memory pools and to avoid double-management and performance degradation.
 */
class Divisible_shm_pool :
  public Shm_pool
{
public:
  /**
   * Constructor.
   *
   * @param id See super-class.
   * @param name See super-class.
   * @param address See super-class.
   * @param size See super-class.
   * @param fd See super-class.
   */
  Divisible_shm_pool(pool_id_t id, const std::string& name, void* address, size_t size, int fd);

  /**
   * Returns the remaining size in the pool that hasn't yet been removed.
   *
   * @return See above.
   */
  inline size_t get_remaining_size() const;
  /**
   * Returns whether the pool has been completely removed.
   *
   * @return See above.
   */
  inline bool is_completely_removed() const;
  /**
   * Accounts for removal of a memory region from the pool.
   *
   * @param size The size to remove.
   *
   * @return Whether the entire size could be removed; in other words, whether the remaining size was greater than
   *         or equal to the size requested to be removed.
   */
  inline bool remove_size(size_t size);

  /**
   * Prints the pool description.
   *
   * @param os The stream to output the contents to.
   */
  virtual void print(std::ostream& os) const override;

private:
  /// The remaining size in the pool that hasn't yet been removed.
  size_t m_remaining_size;
}; // class Divisible_shm_pool

Divisible_shm_pool::size_t Divisible_shm_pool::get_remaining_size() const
{
  return m_remaining_size;
}

bool Divisible_shm_pool::is_completely_removed() const
{
  return (m_remaining_size == 0);
}

bool Divisible_shm_pool::remove_size(size_t size)
{
  if (size > m_remaining_size)
  {
    m_remaining_size = 0;
    return false;
  }

  m_remaining_size -= size;
  return true;
}

} // namespace ipc::shm::arena_lend
