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

#include "ipc/shm/arena_lend/shm_pool_collection.hpp"
#include "ipc/shm/arena_lend/shm_pool.hpp"
#include "ipc/common.hpp"
#include <sys/mman.h>

using std::shared_ptr;
using std::make_pair;
using std::string;
using std::map;

namespace ipc::shm::arena_lend
{

Shm_pool_collection::Shm_pool_collection(flow::log::Logger* logger, Collection_id id) :
  Log_context(logger, Log_component::S_SHM),
  m_id(id)
{
}

Shm_pool_collection::~Shm_pool_collection()
{
  FLOW_LOG_TRACE("Remaining pools:");
  print_shm_pool_map();
}

void* Shm_pool_collection::map_shm(size_t size, bool read_enabled, bool write_enabled, int fd)
{
  assert(size > 0);
  assert(fd > 0);

  int memory_protection_flags = 0;
  if (write_enabled)
  {
    memory_protection_flags |= PROT_WRITE;
    if (!read_enabled)
    {
      FLOW_LOG_WARNING("Write with no read for fd " << fd);
    }
    else
    {
      memory_protection_flags |= PROT_READ;
    }
  }
  else if (read_enabled)
  {
    memory_protection_flags |= PROT_READ;
  }
  else
  {
    memory_protection_flags |= PROT_NONE;
  }

  return memory_map(nullptr,
                    size,
                    memory_protection_flags,
                    MAP_SHARED,
                    fd);
}

void* Shm_pool_collection::memory_map(void* address, size_t size, int memory_protection_flags, int flags, int fd)
{
  void* actual_address = ::mmap(address, size, memory_protection_flags, flags, fd, 0);
  if (actual_address == MAP_FAILED)
  {
    FLOW_LOG_WARNING("Error occurred when memory mapping size " << size << ": " <<
                     strerror(errno) << "(" << errno << ")");
    // @todo - Fill in, alert
    return nullptr;
  }

  return actual_address;
}

bool Shm_pool_collection::memory_unmap(void* address, size_t size)
{
  if (::munmap(address, size) != 0)
  {
    FLOW_LOG_WARNING("Error occurred when memory unmapping address " << address << ", size " << size << ": " <<
                     strerror(errno) << "(" << errno << ")");
    // @todo - Fill in, alert
    return false;
  }

  return true;
}

bool Shm_pool_collection::close_shm_pool(const shared_ptr<Shm_pool>& shm_pool)
{
  if (!memory_unmap(shm_pool->get_address(), shm_pool->get_size()))
  {
    return false;
  }

  // Close pool
  if (close(shm_pool->get_fd()) != 0)
  {
    // Error occurred - possibly nonexistent
    FLOW_LOG_WARNING("Error occurred when closing shared memory name '" << shm_pool->get_name() << "': " <<
                     strerror(errno) << "(" << errno << ")");
    // @todo - Handle
    return false;
  }

  return true;
}

void Shm_pool_collection::print_shm_pool_map() const
{
  Read_lock read_lock(m_shm_pool_map_mutex);

  if (m_shm_pool_map.empty())
  {
    FLOW_LOG_TRACE("Empty SHM pool map");
    return;
  }

  size_t index = 0;
  for (const auto& iter : m_shm_pool_map)
  {
    FLOW_LOG_TRACE("[" << index << "] [" << *iter.second << "]");
    ++index;
  }
}

shared_ptr<Shm_pool> Shm_pool_collection::lookup_shm_pool(const void* address) const
{
  Read_lock read_lock(m_shm_pool_map_mutex);

  shared_ptr<Shm_pool> shm_pool;
  auto iter = m_shm_pool_map.lower_bound(address);
  if (iter == m_shm_pool_map.end())
  {
    // Address > all entries, so last entry may be holding the address
    auto riter = m_shm_pool_map.rbegin();
    if (riter == m_shm_pool_map.rend())
    {
      // Empty map
      return nullptr;
    }

    shm_pool = riter->second;
  }
  else
  {
    shm_pool = iter->second;
    if (shm_pool->get_address() > address)
    {
      if (iter == m_shm_pool_map.begin())
      {
        // Address < all entries, so pool containing address does not exist
        return nullptr;
      }
      // Get preceding entry as that must be lower
      --iter;
      assert(iter->first < address);
      shm_pool = iter->second;
    }
    // else address == entry found
  }

  // Check range
  Shm_pool::size_t ignored;
  if (!shm_pool->determine_offset(address, ignored))
  {
    // Object out of range
    return nullptr;
  }

  return shm_pool;
}

shared_ptr<Shm_pool> Shm_pool_collection::lookup_shm_pool_exact(const void* address) const
{
  Read_lock read_lock(m_shm_pool_map_mutex);

  const auto iter = m_shm_pool_map.find(address);
  if (iter == m_shm_pool_map.end())
  {
    // No pool matches
    return nullptr;
  }

  return iter->second;
}

bool Shm_pool_collection::register_shm_pool(const shared_ptr<Shm_pool>& shm_pool)
{
  {
    Write_lock write_lock(m_shm_pool_map_mutex);

    auto iter = m_shm_pool_map.emplace(make_pair(shm_pool->get_address(), shm_pool));
    if (!iter.second)
    {
      // Alert
      FLOW_LOG_WARNING("Could not register pool [" << *shm_pool << "]");
      return false;
    }
  }
  FLOW_LOG_TRACE("Registered pool [" << *shm_pool << "]");
  return true;
}

bool Shm_pool_collection::deregister_shm_pool(const shared_ptr<Shm_pool>& shm_pool)
{
  if (!deregister_shm_pool_internal(shm_pool->get_address()))
  {
    // Alert
    FLOW_LOG_WARNING("Could not deregister pool [" << *shm_pool << "]");
    return false;
  }
  FLOW_LOG_TRACE("Deregistered pool [" << *shm_pool << "]");
  return true;
}

} // namespace ipc::shm::arena_lend
