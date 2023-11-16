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

#include "ipc/shm/arena_lend/detail/shm_pool_offset_ptr_data.hpp"
#include <string>
#include <ostream>

namespace ipc::shm::arena_lend
{

/**
 * Mapped contiguous memory backed by a named shared memory object.
 */
class Shm_pool
{
public:
  /// Short-hand for pool ID type.
  using pool_id_t = detail::Shm_pool_offset_ptr_data_base::pool_id_t;
  /// Short-hand for pool offset type a/k/a size type in this context.
  using size_t = detail::Shm_pool_offset_ptr_data_base::pool_offset_t;

  /**
   * Constructor.
   *
   * @param id Pool ID.
   * @param name The shared object name.
   * @param address The base address of the mapped region.
   * @param size The size of the region.
   * @param fd The file descriptor of the opened shared memory.
   */
  Shm_pool(pool_id_t id, const std::string& name, void* address, size_t size, int fd);
  /// Default destructor
  virtual ~Shm_pool() = default;

  /**
   * Returns the pool ID.
   *
   * @return See above.
   */
  inline pool_id_t get_id() const;
  /**
   * Returns the shared object name.
   *
   * @return See above.
   */
  inline const std::string& get_name() const;
  /**
   * Returns the base address of the mapped region.
   *
   * @return See above.
   */
  inline void* get_address() const;
  /**
   * Returns the size of the mapped region.
   *
   * @return See above.
   */
  inline size_t get_size() const;
  /**
   * Returns the file descriptor of the opened shared memory.
   *
   * @return See above.
   */
  inline int get_fd() const;

  /**
   * Computes the offset of an address from the base address of the pool.
   *
   * @param address The address.
   * @param offset If the address is within the pool, the offset of the address from the pool's base address will be
   *               stored here.
   *
   * @return Whether the address is located within the pool.
   */
  bool determine_offset(const void* address, size_t& offset) const;
  /**
   * Returns whether a range lies completely within the pool.
   *
   * @param address The starting address of the range.
   * @param size The size of the range.
   * @param offset If the result is true and the value is non-null, the offset of the address from the pool's base
   *               address will be stored here; otherwise, undetermined.
   *
   * @return See above.
   */
  inline bool is_subset(const void* address, std::size_t size, size_t* offset = nullptr) const;
  /**
   * Converts an offset to a pointer. Note that `offset` can be `>= get_size()` or negative in which case an
   * out-of-bounds address will still (intentionally) be returned. See explanation in
   * Shm_pool_repository_singleton::to_address() doc header.
   *
   * @param offset The byte offset from the base of the pool.
   *
   * @return See above.
   */
  void* to_address(size_t offset) const;
  /**
   * Compares against another SHM pool for equality.
   *
   * @param other The other SHM pool to compare against.
   *
   * @return Whether the other SHM pool is equivalent to this one.
   */
  bool operator==(const Shm_pool& other) const;
  /**
   * Compares against another SHM pool for inequality.
   *
   * @param other The other SHM pool to compare against.
   *
   * @return Whether the other SHM pool is not equivalent to this one.
   */
  inline bool operator!=(const Shm_pool& other) const;
  /**
   * Prints the pool description.
   *
   * @param os The stream to output the contents to.
   */
  virtual void print(std::ostream& os) const;

  /**
   * Returns whether a range borders another range. Note that overlapping regions are not considered adjacent.
   *
   * @param address_a Starting address of range A.
   * @param size_a Size of range A.
   * @param address_b Starting address of range B.
   * @param size_b Size of range B.
   *
   * @return Whether the two ranges border each other.
   */
  static inline bool is_adjacent(const void* address_a, size_t size_a, const void* address_b, size_t size_b);

private:
  /// The pool ID.
  const pool_id_t m_id;
  /// The shared object name.
  const std::string m_name;
  /// The base address of the mapped region.
  uint8_t* const m_address;
  /// The size of the region.
  const size_t m_size;
  /// The file descriptor of the opened shared memory.
  const int m_fd;
}; // class Shm_pool

Shm_pool::pool_id_t Shm_pool::get_id() const
{
  return m_id;
}

const std::string& Shm_pool::get_name() const
{
  return m_name;
}

void* Shm_pool::get_address() const
{
  return m_address;
}

Shm_pool::size_t Shm_pool::get_size() const
{
  return m_size;
}

int Shm_pool::get_fd() const
{
  return m_fd;
}

bool Shm_pool::is_subset(const void* address, std::size_t size, size_t* offset) const
{
  size_t temp_offset;
  auto& offset_ref = offset ? *offset : temp_offset;

  return determine_offset(address, offset_ref) && (static_cast<decltype(size)>(m_size - offset_ref) >= size);
}

bool Shm_pool::operator!=(const Shm_pool& other) const
{
  return !operator==(other);
}

// Static method
bool Shm_pool::is_adjacent(const void* address_a, size_t size_a, const void* address_b, size_t size_b)
{
  return (((static_cast<const uint8_t*>(address_a) + size_a) == address_b) ||
          ((static_cast<const uint8_t*>(address_b) + size_b) == address_a));
}

inline std::ostream& operator<<(std::ostream& os, const Shm_pool& shm_pool)
{
  shm_pool.print(os);
  return os;
}

} // namespace ipc::shm::arena_lend
