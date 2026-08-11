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
#include "ipc/util/util_fwd.hpp"
#include "ipc/util/shared_name_fwd.hpp"
#include "ipc/shm/stl/stl_fwd.hpp"
#include <vector>
#include <memory>

namespace ipc::session::shm::arena_lend
{

// Types.

// Find doc headers near the bodies of these compound types.

template<typename Shm_arena_t>
class Borrower_shm_pool_collection_repository;

/// Short-hand for util::Shared_name; used in particular for SHM pool names at least.
using Shared_name = util::Shared_name;

/// Identifier type for a shared memory pool collection.
using collection_id_t = ipc::shm::arena_lend::collection_id_t;

/// Alias for an identifier of the owner (essentially namespace of `collection_id_t`s) of shared information.
using owner_id_t = ipc::shm::arena_lend::owner_id_t;

/**
 * Canonical short-hand for a list of per-arena `stat::Borrower_pool_stats`, as produced by the borrower-side
 * process-wide stat accessor (see jemalloc::Shm_session::borrower_pool_stats_process_wide()).  The
 * `unique_ptr` wrapper allows cheap moves and lets the stat-sets -- not natively copyable due to their `atomic`
 * members -- live in a `vector`.
 */
using Borrower_pool_stats_list = std::vector<std::unique_ptr<ipc::shm::arena_lend::stat::Borrower_pool_stats>>;

} // namespace ipc::session::shm::arena_lend
