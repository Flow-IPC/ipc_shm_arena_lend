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
#include <unordered_map>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <cassert>

// #define SHM_POOL_REPOSITORY_DEBUG
#ifdef SHM_POOL_REPOSITORY_DEBUG
#include <iostream>
#endif // #ifdef SHM_POOL_REPOSITORY_DEBUG

namespace ipc::shm::arena_lend
{

/**
 * Container for shared memory pools. The shared memory pool identifiers not pointing to the same shared memory
 * object must be distinct from each other.
 *
 * @tparam Shm_pool_holder_param A class that contains a shared memory pool. It must provide the following interface:
 *                               const std::shared_ptr<Shm_pool>& get_shm_pool().
 */
template <typename Shm_pool_holder_param>
class Shm_pool_repository
{
public:
  /// Short-hand for pool ID type.
  using pool_id_t = Shm_pool::pool_id_t;
  /// Short-hand for pool offset type.
  using pool_offset_t = Shm_pool::size_t;

  virtual ~Shm_pool_repository() = default;

  /**
   * Looks up a shared memory pool and converts the information given to a raw pointer. Note that `offset` can
   * exceed the pool's size (assuming it is found in the first place) or be negative (assuming ditto),
   * in which case an out-of-bounds address will still (intentionally) be returned. See explanation in
   * Shm_pool_repository_singleton::to_address() doc header.
   *
   * @param shm_pool_id Identifier for a shared memory pool.
   * @param offset Offset from the base of the shared memory pool where the object resides.
   *
   * @return If the shared memory pool is found, a raw pointer; otherwise, nullptr.
   */
  void* to_address(pool_id_t shm_pool_id, pool_offset_t offset) const;
  /**
   * Looks up a raw pointer and returns the shared memory pool and offset; or if not found sets an out-arg
   * to a special value.
   *
   * @param address The raw pointer to look up.
   * @param shm_pool If successful, the shared memory pool containing the raw pointer will be stored here; else
   *                 null will.
   * @param offset If successful, the offset from the base of the shared memory pool where the pointer resides will
   *               be stored here; else its post-value is meaningless.
   */
  void from_address(const void* address, std::shared_ptr<Shm_pool>& shm_pool, pool_offset_t& offset) const;
  /**
   * Inserts a shared memory pool.
   *
   * @param shm_pool The shared memory pool to insert. This must be non-null.
   *
   * @return Whether the shared memory pool was inserted, meaning that it didn't previously exist.
   */
  bool insert(const std::shared_ptr<Shm_pool>& shm_pool);
  /**
   * Removes a shared memory pool if it is found.
   *
   * @param shm_pool_id The identifier of a shared memory pool to remove.
   *
   * @return Whether a shared memory pool was removed.
   */
  bool erase(pool_id_t shm_pool_id);

protected:
  /// The mutex type.
  using Mutex = std::mutex;
  /// Exclusive access lock.
  using Lock = std::lock_guard<Mutex>;
  /// Map from shared memory pool id -> Shm_pool_holder_param.
  using Shm_pool_id_map = std::unordered_map<pool_id_t, Shm_pool_holder_param>;

  /**
   * Returns the mutex used for the shared memory pool maps.
   *
   * @return See above.
   */
  inline Mutex& get_shm_pool_maps_mutex() const;
  /**
   * Returns the shared memory pool id -> Shm_pool_holder_param map.
   *
   * @return See above.
   */
  inline Shm_pool_id_map& get_shm_pool_id_map();

  /**
   * Removes a shared memory pool. A lock must already have been obtained.
   *
   * @param map_iterator An iterator to a map entry of #m_shm_pool_id_map.
   */
  void erase_locked(typename Shm_pool_id_map::iterator& map_iterator);

private:
  /// Serializes access to #m_shm_pool_id_map and #m_shm_pool_address_map.
  mutable Mutex m_shm_pool_maps_mutex;
  /// Map of shared memory pool id to shared memory pool data.
  Shm_pool_id_map m_shm_pool_id_map;
  /// Map of shared memory pool address to shared memory pool. It is ordered due to inexact address lookup.
  std::map<const void*, std::shared_ptr<Shm_pool>> m_shm_pool_address_map;

  /// Allow insertion operator to access internals.
  template <typename Shm_pool_holder_param_2>
  friend std::ostream& operator<<(std::ostream& os, const Shm_pool_repository<Shm_pool_holder_param_2>& repository);
}; // class Shm_pool_repository

template <typename Shm_pool_holder_param>
typename Shm_pool_repository<Shm_pool_holder_param>::Mutex&
Shm_pool_repository<Shm_pool_holder_param>::get_shm_pool_maps_mutex() const
{
  return m_shm_pool_maps_mutex;
}

template <typename Shm_pool_holder_param>
typename Shm_pool_repository<Shm_pool_holder_param>::Shm_pool_id_map&
Shm_pool_repository<Shm_pool_holder_param>::get_shm_pool_id_map()
{
  return m_shm_pool_id_map;
}

template <typename Shm_pool_holder_param>
void* Shm_pool_repository<Shm_pool_holder_param>::to_address(pool_id_t shm_pool_id, pool_offset_t offset) const
{
  Lock lock(m_shm_pool_maps_mutex);

  auto iter = m_shm_pool_id_map.find(shm_pool_id);
  if (iter == m_shm_pool_id_map.end())
  {
    return nullptr;
  }

  return iter->second.get_shm_pool()->to_address(offset);
}

template <typename Shm_pool_holder_param>
void Shm_pool_repository<Shm_pool_holder_param>::from_address(const void* address,
                                                              std::shared_ptr<Shm_pool>& shm_pool,
                                                              pool_offset_t& offset) const
{
  Lock lock(m_shm_pool_maps_mutex);

  auto iter = m_shm_pool_address_map.lower_bound(address);
  if (iter == m_shm_pool_address_map.end())
  {
    // Address > all entries, so last entry may be holding the address
    auto riter = m_shm_pool_address_map.rbegin();
    if (riter == m_shm_pool_address_map.rend())
    {
      // Empty map
      shm_pool.reset();
      return;
    }

    shm_pool = riter->second;
  }
  else
  {
    shm_pool = iter->second;
    if (shm_pool->get_address() > address)
    {
      if (iter == m_shm_pool_address_map.begin())
      {
        // Address < all entries, so pool containing address does not exist
        shm_pool.reset();
        return;
      }
      // Get preceding entry as that must be lower
      --iter;
      assert(iter->first < address);
      shm_pool = iter->second;
    }
    // else address == entry found
  }

  // Check range
  if (!shm_pool->determine_offset(address, offset))
  {
    // Object out of range (offset was touched, but by contract they should ignore it because we set shm_pool = null)
    shm_pool.reset();
  }
  // else { All set }
}

template <typename Shm_pool_holder_param>
bool Shm_pool_repository<Shm_pool_holder_param>::insert(const std::shared_ptr<Shm_pool>& shm_pool)
{
  Lock lock(m_shm_pool_maps_mutex);

  std::cout << "XXX0: (type holder/tracker [" << typeid(Shm_pool_holder_param).name() << "])\n";

  auto id_map_result = m_shm_pool_id_map.emplace(shm_pool->get_id(), Shm_pool_holder_param(shm_pool));
  if (!id_map_result.second)
  {
  // XXX
#ifdef SHM_POOL_REPOSITORY_DEBUG
    std::cout << "XXX1 - Could not insert [" << shm_pool->get_address() << ", " << shm_pool->get_id() << "]\n";
#endif // #ifdef SHM_POOL_REPOSITORY_DEBUG
    return false;
  }
  // XXX
#ifdef SHM_POOL_REPOSITORY_DEBUG
  std::cout << "XXX1 - Inserted [" << shm_pool->get_address() << ", " << shm_pool->get_id() << "]\n";
#endif // #ifdef SHM_POOL_REPOSITORY_DEBUG

  auto address_map_result = m_shm_pool_address_map.emplace(shm_pool->get_address(), shm_pool);
  if (!address_map_result.second)
  {
    // Somehow failed, so revert prior action
#ifdef SHM_POOL_REPOSITORY_DEBUG
    std::cout << "Could not insert [" << shm_pool->get_address() << ", " << shm_pool->get_id() << "]\n";
    std::cout << *this;
    assert(false);
#endif // #ifdef SHM_POOL_REPOSITORY_DEBUG
    m_shm_pool_id_map.erase(id_map_result.first);
    return false;
  }

#ifdef SHM_POOL_REPOSITORY_DEBUG
  std::cout << "Inserted [" << shm_pool->get_address() << ", " << shm_pool->get_id() << "]\n";
#endif // #ifdef SHM_POOL_REPOSITORY_DEBUG

  return true;
}

template <typename Shm_pool_holder_param>
bool Shm_pool_repository<Shm_pool_holder_param>::erase(pool_id_t shm_pool_id)
{
  Lock lock(m_shm_pool_maps_mutex);

  auto iter = m_shm_pool_id_map.find(shm_pool_id);
  if (iter == m_shm_pool_id_map.end())
  {
    return false;
  }

  erase_locked(iter);
  return true;
}

template <typename Shm_pool_holder_param>
void Shm_pool_repository<Shm_pool_holder_param>::erase_locked(typename Shm_pool_id_map::iterator& map_iterator)
{
  // Save SHM pool handle before iterator is invalidated
  std::shared_ptr<Shm_pool> shm_pool = map_iterator->second.get_shm_pool();
  m_shm_pool_id_map.erase(map_iterator);

  // Remove from other lookup map
#ifndef NDEBUG
  auto result =
#endif
  m_shm_pool_address_map.erase(shm_pool->get_address());
  assert(result > 0);

#ifdef SHM_POOL_REPOSITORY_DEBUG
  std::cout << "Erased [" << shm_pool->get_address() << ", " << shm_pool->get_id() << "]\n";
#endif // #ifdef SHM_POOL_REPOSITORY_DEBUG
}

/**
 * Prints the maps to an output stream.
 *
 * @tparam Shm_pool_holder_param A class that contains a shared memory pool.
 * @param os The output stream to use.
 * @param repository The Shm_pool_repository to print.
 *
 * @return The output stream.
 */
template <typename Shm_pool_holder_param>
std::ostream& operator<<(std::ostream& os, const Shm_pool_repository<Shm_pool_holder_param>& repository)
{
  typename Shm_pool_repository<Shm_pool_holder_param>::Lock lock(repository.m_shm_pool_maps_mutex);

  {
    size_t i = 0;
    for (const auto& cur_pair : repository.m_shm_pool_id_map)
    {
      os << "Id map [" << i++ << "]: [" << cur_pair.first << ", " << cur_pair.second.get_shm_pool()->get_id() <<
        "]\n";
    }
  }
  {
    size_t i = 0;
    for (const auto& cur_pair : repository.m_shm_pool_address_map)
    {
      os << "Address map [" << i++ << "]: [" << cur_pair.first << ", " << cur_pair.second->get_id() << "]\n";
    }
  }

  return os;
}

} // namespace ipc::shm::arena_lend
