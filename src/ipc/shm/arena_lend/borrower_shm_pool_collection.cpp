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

/// @file
#include "ipc/shm/arena_lend/borrower_shm_pool_collection.hpp"
#include <flow/error/error.hpp>
#include <boost/system/system_category.hpp>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

using std::shared_ptr;
using std::string;
using std::size_t;
using std::make_shared;

namespace ipc::shm::arena_lend
{

Borrower_shm_pool_collection::Borrower_shm_pool_collection(flow::log::Logger* logger, collection_id_t id,
                                                           Shared_name&& pool_name_base) :
  Shm_pool_collection(logger, id),
  m_pool_name_base(std::move(pool_name_base))
{
}

const Shared_name& Borrower_shm_pool_collection::get_pool_name_base() const
{
  return m_pool_name_base;
}

Shared_name Borrower_shm_pool_collection::recompute_pool_name(pool_id_t id) const
{
  return m_pool_name_base / Shared_name::ct_from_int(id);
}

shared_ptr<Shm_pool> Borrower_shm_pool_collection::open_shm_pool(pool_id_t id, size_t size, Error_code* err_code)
{
  using boost::system::system_category;

  assert((size <= size_t(std::numeric_limits<pool_offset_t>::max()))
         && "Borrowing a pool sized too large to express offsets given our pointer data structures. "
              "Did opposing-process create_shm_pool() act strangely? Was size IPC-transmitted incorrectly?");
  assert(err_code && "This internal-use API doesn't use full Flow-style error emission with optionally throwing; "
                       "intentionally only via setting Error_code out-arg on error; so `err_code` cannot be null.");

  const auto name = recompute_pool_name(id);

  // Open shared memory pool for read-only; leading '/' required by POSIX for shm_open().
  int flags = O_RDONLY;
  const auto shm_name = '/' + name.str();
  int fd = shm_open(shm_name.c_str(), flags, 0);
  if (fd == -1)
  {
    const Error_code sys_err_code{errno, system_category()};
    FLOW_LOG_WARNING("Tried to open SHM-object [" << name << "]; got the following error.");
    FLOW_ERROR_SYS_ERROR_LOG_WARNING();

    *err_code = sys_err_code;
    return nullptr;
  }

  void* addr = map_shm(size, true, false, fd);
  if (!addr)
  {
    /* @todo map_shm() should probably issue a civilized Error_code itself. Or at least it should document
     * on failure one should check errno.
     *
     * Anyway in practice the latter is the case as of this writing, so we can be a bit more civilized ourselves
     * at least. */

    const Error_code sys_err_code{errno, system_category()};
    FLOW_LOG_WARNING("Tried to memory-map just-opened SHM-object (named [" << name << "]); got the following error.");
    FLOW_ERROR_SYS_ERROR_LOG_WARNING();

    *err_code = sys_err_code;

    ::close(fd); // Don't leak it.
    return nullptr;
  }

  const auto pool = make_shared<Shm_pool>(id, name.str(), addr, size, fd);
  assert(pool);

#ifndef NDEBUG
  const bool ok =
#endif
  register_shm_pool(pool);
  assert(ok && "We just created the s_p<Shm_pool>, but one at the same addr was already registered in this "
                 "collection? It could happen if the name were the same as the existing pool's which implies "
                 "the system is borked totally/bug.");

  return pool; // Currently advertising that *err_code is irrelevant on success.
}

} // namespace ipc::shm::arena_lend
