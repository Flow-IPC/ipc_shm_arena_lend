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

#include <boost/core/noncopyable.hpp>

namespace ipc::shm::arena_lend
{

/**
 * Generates a singleton container for shared memory pools.
 *
 * @tparam Repository_type The repository this singleton will be unique for.
 */
template <typename Repository_type>
class Shm_pool_repository_singleton :
  public boost::noncopyable
{
public:
  /// Short-hand for pool ID type.
  using pool_id_t = typename Repository_type::pool_id_t;
  /// Short-hand for pool offset type.
  using pool_offset_t = typename Repository_type::pool_offset_t;

  /**
   * Returns the singleton instance.
   *
   * @return See above.
   */
  static inline Repository_type& get_instance()
  {
    static Repository_type s_repository;
    return s_repository;
  }

  /**
   * Converts a set of data identifying an object to a raw pointer. The primary use case for this is converting
   * a fancy pointer with contents stored in shared memory to an object of type known by the caller.
   *
   * As of this writing note that this computation is permissive. Assuming `shm_pool_id` specifies a valid pool
   * this shall yield its base address plus `pool_offset` even in the following situations.
   *   - If `pool_offset > 0` and equals or exceeds the pool size, this will return a vaddr past the pool.
   *   - If `pool_offset < 0`, this will return a vaddr preceding the pool.
   *
   * This is intentional to support the widest variety of potential algorithms. In short, while it is not
   * safe to dereference such a pointer, it can be safe and in some cases legitimate to compare against one.
   *
   * @param shm_pool_id The id of the shared memory pool where the object resides in.
   * @param pool_offset The offset within the shared memory pool that the object is located.
   *
   * @return If convertible, the pointer to the object; otherwise, nullptr.
   *
   * @internal
   * @see detail::Shm_pool_offset_ptr_data_base::pool_offset_t doc header for explanation as to the benefits
   *      of allowing out-of-bounds vaddrs returned by this function.
   */
  static inline void* to_address(pool_id_t shm_pool_id, pool_offset_t pool_offset)
  {
    return get_instance().to_address(shm_pool_id, pool_offset);
  }

  /**
   * Converts a raw pointer to a set of data identifying an object. The primary use case for this is conversion
   * to a fancy pointer with contents stored in shared memory.
   *
   * @param address The raw pointer to look up.
   * @param shm_pool_id If successful, the ID of shared memory pool containing the raw pointer will be stored here;
   *                    else special value 0 will.
   * @param offset If successful, the offset from the base of the shared memory pool where the raw pointer resides
   *               will be stored here.
   */
  static inline void from_address(const void* address, pool_id_t& shm_pool_id, pool_offset_t& offset)
  {
    std::shared_ptr<Shm_pool> shm_pool;
    get_instance().from_address(address, shm_pool, offset);
    shm_pool_id = shm_pool ? shm_pool->get_id() : 0;
  }

private:
  /// Hidden constructor to prevent instantiation.
  Shm_pool_repository_singleton() = default;
}; // class Shm_pool_repository_singleton

} // namespace ipc::shm::arena_lend
