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
#pragma once

#include "ipc/shm/arena_lend/arena_lend_fwd.hpp"
#include "ipc/shm/arena_lend/jemalloc/jemalloc_fwd.hpp"
#include "ipc/shm/arena_lend/borrower_allocator_arena.hpp"
#include "ipc/shm/arena_lend/jemalloc/ipc_arena.hpp"
#include "ipc/shm/shm.hpp"
#include "ipc/session/standalone/shm/arena_lend/arena_lend_fwd.hpp"
#include "ipc/session/standalone/shm/arena_lend/borrower_shm_pool_collection_repository.hpp"

namespace ipc::shm
{

/// Implementation of #Arena_to_borrower_allocator_arena_t for SHM-jemalloc arenas.
template<>
struct Arena_to_borrower_allocator_arena<arena_lend::jemalloc::Ipc_arena>
{
  /**
   * Implementation of `Arena_to_borrower_allocator_arena_t`; for SHM-jemalloc the `Arena` and
   * `Borrower_allocator_arena` are different types, and in common use there is no relevant *object*
   * of the latter; only the type matters.
   */
  using Type
    = arena_lend::Borrower_allocator_arena
        <arena_lend::Shm_pool_repository_singleton
           <ipc::session::shm::arena_lend::Borrower_shm_pool_collection_repository>>;
};

} // namespace ipc::shm
