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

#include "ipc/shm/arena_lend/jemalloc/jemalloc_fwd.hpp"
#include "ipc/transport/struc/shm/shm_fwd.hpp"

/// See parent ipc::transport::struc::shm and at least sub-namespace ipc::transport::struc::shm::arena_lend::jemalloc.
namespace ipc::transport::struc::shm::arena_lend { }
// @todo Perhaps move into a ../arena_lend_fwd.hpp; may be more of a thing once/if more arena_lend SHM-providers exist.

/**
 * As of this writing certain convenience aliases supplied for the SHM-jemalloc SHM-provider as pertains to
 * zero-copy structured message passing with that SHM-provider.
 *
 * @see ipc::transport::struc::shm doc header.
 */
namespace ipc::transport::struc::shm::arena_lend::jemalloc
{

// Types.

/// Convenience alias: `transport::struc::shm::Builder` that works with SHM-jemalloc arenas.
using Builder = Builder<ipc::shm::arena_lend::jemalloc::Ipc_arena>;

/// Convenience alias: `transport::struc::shm::Reader` that works with SHM-jemalloc arenas.
using Reader = Reader<ipc::shm::arena_lend::jemalloc::Ipc_arena>;

} // namespace ipc::transport::struc::shm::arena_lend::jemalloc
