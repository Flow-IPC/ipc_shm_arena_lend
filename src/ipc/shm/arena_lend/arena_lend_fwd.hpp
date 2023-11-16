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

#include "ipc/shm/arena_lend/shm_pool_holder.hpp"
#include "ipc/shm/arena_lend/shm_pool_repository.hpp"
#include <sys/types.h>
#include <cstddef>

namespace ipc::shm::arena_lend
{

template <typename Repository_type>
class Shm_pool_repository_singleton;

template <typename Repository_type,
          typename Difference_type = int>
class Borrower_allocator_arena;

template <typename Pointed_type,
          typename Repository_type,
          typename Difference_type = int,
          bool CAN_STORE_RAW_PTR = true>
class Shm_pool_offset_ptr;

template <typename Shm_pool_holder_param>
class Shm_pool_repository;

class Borrower_shm_pool_collection;
class Borrower_shm_pool_listener;
class Borrower_shm_pool_repository;
class Divisible_shm_pool;
class Memory_manager;
class Owner_shm_pool_collection;
class Owner_shm_pool_listener;
class Owner_shm_pool_listener_for_repository;
class Shm_pool;
class Shm_pool_collection;
class Shm_pool_holder;

/**
 * Identifier type for a shared memory pool collection.
 * @todo Determine proper size of identifier.
 */
using Collection_id = uint32_t;

/// Alias for the owner SHM pool repository.
using Owner_shm_pool_repository = Shm_pool_repository<Shm_pool_holder>;

/**
 * Alias for the owner SHM pool repository singleton object used for converting shared memory handles to
 * pointers on the owner side.
 */
using Owner_shm_pool_repository_singleton = Shm_pool_repository_singleton<Owner_shm_pool_repository>;

} // namespace ipc::shm::arena_lend










