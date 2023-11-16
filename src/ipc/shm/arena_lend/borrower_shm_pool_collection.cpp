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

#include "ipc/shm/arena_lend/borrower_shm_pool_collection.hpp"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

using std::shared_ptr;
using std::string;
using std::size_t;
using std::make_shared;

namespace ipc::shm::arena_lend
{

Borrower_shm_pool_collection::Borrower_shm_pool_collection(flow::log::Logger* logger, Collection_id id) :
  Shm_pool_collection(logger, id)
{
}

shared_ptr<Shm_pool> Borrower_shm_pool_collection::open_shm_pool(pool_id_t id, const string& name, size_t size)
{
  assert((size <= size_t(std::numeric_limits<pool_offset_t>::max()))
         && "Borrowing a pool sized too large to express offsets given our pointer data structures. "
              "Did opposing-process create_shm_pool() act strangely? Was size IPC-transmitted incorrectly?");

  // Open shared memory pool for read-only
  int flags = O_RDONLY;
  int fd = shm_open(name.c_str(), flags, 0);
  if (fd == -1)
  {
    // Alert
    FLOW_LOG_WARNING("Error occurred when opening shared memory name '" << name << "': " << strerror(errno) << "(" <<
                     errno << ")");
    return nullptr;
  }

  void* addr = map_shm(size, true, false, fd);
  if (addr == nullptr)
  {
    // Handle error
    // @todo - Fill in
    return nullptr;
  }

  shared_ptr<Shm_pool> pool = make_shared<Shm_pool>(id, name, addr, size, fd);
  if (pool == nullptr)
  {
    FLOW_LOG_WARNING("Could not map pool '" << name << "', size " << size);
    return nullptr;
  }

  if (!register_shm_pool(pool))
  {
    close_shm_pool(pool);
    return nullptr;
  }

  return pool;
}

} // namespace ipc::shm::arena_lend
