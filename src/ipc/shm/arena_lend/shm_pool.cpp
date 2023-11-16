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

#include "ipc/shm/arena_lend/shm_pool.hpp"
#include <cassert>

using std::string;
using std::ostream;

namespace ipc::shm::arena_lend
{

Shm_pool::Shm_pool(pool_id_t id, const string& name, void* address, size_t size, int fd) :
  m_id(id),
  m_name(name),
  m_address(static_cast<uint8_t*>(address)),
  m_size(size),
  m_fd(fd)
{
  assert(id != 0);
  assert(!name.empty());
  assert(address != nullptr);
  assert(size > 0);
  assert(fd > 0);
}

bool Shm_pool::determine_offset(const void* address, size_t& offset) const
{
  const uint8_t* byte_address = static_cast<const uint8_t*>(address);
  if ((byte_address < m_address) || (byte_address >= (m_address + m_size)))
  {
    return false;
  }
  offset = byte_address - m_address;
  return true;
}

void* Shm_pool::to_address(size_t offset) const
{
  // Intentional/as promised: no check against get_size() nor 0.  And yes, as of this writing, our size_t is signed.
  return m_address + offset;
}

bool Shm_pool::operator==(const Shm_pool& other) const
{
  return (m_name == other.m_name) &&
         (m_address == other.m_address) &&
         (m_size == other.m_size) &&
         (m_fd == other.m_fd);
}

void Shm_pool::print(ostream& os) const
{
  os <<
    "Id: " << m_id <<
    ", name: " << m_name <<
    // Cast to void*
    ", address: " << get_address() <<
    ", size: " << m_size <<
    ", fd: " << m_fd;
}

} // namespace ipc::shm::arena_lend
