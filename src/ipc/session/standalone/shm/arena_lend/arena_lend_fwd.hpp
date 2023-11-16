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
#include "ipc/shm/stl/stl_fwd.hpp"

namespace ipc::session::shm::arena_lend
{

class Borrower_collection;
class Borrower_shm_pool_collection_repository;
class Lender_collection;
class Shm_session_data;

/// Identifier type for a shared memory pool collection.
using Collection_id = ipc::shm::arena_lend::Collection_id;

/// Alias for an identifier of the owner of shared information.
using Owner_id = pid_t;

/**
 * Alias for the singleton storing shared memory pool collection information from a borrower side. This is used
 * across sessions in support of stored memory handles in shared memory. To use, include
 * "ipc/session/shm/arena_lend/borrower_shm_pool_collection_repository.hpp" and
 * "ipc/shm/arena_lend/shm_pool_repository_singleton.hpp".
 *
 * @see ipc::shm::arena_lend::Shm_pool_offset_ptr
 */
using Borrower_shm_pool_collection_repository_singleton =
  ipc::shm::arena_lend::Shm_pool_repository_singleton<Borrower_shm_pool_collection_repository>;

/**
 * Alias for a Stateless_allocator using Borrower_allocator_arena. To use, include
 * "ipc/shm/arena_lend/borrower_allocator_arena.hpp",
 * "ipc/session/shm/arena_lend/borrower_shm_pool_collection_repository.hpp" and
 * "ipc/shm/arena_lend/shm_pool_repository_singleton.hpp".
 *
 * @tparam T The allocated object type.
 */
template<typename T>
using Borrower_arena_allocator = ipc::shm::stl::Stateless_allocator<
  T, ipc::shm::arena_lend::Borrower_allocator_arena<Borrower_shm_pool_collection_repository_singleton>>;

} // namespace ipc::session::shm::arena_lend
