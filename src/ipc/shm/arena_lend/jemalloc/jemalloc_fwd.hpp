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

#include "ipc/shm/shm_fwd.hpp"
#include "ipc/shm/stl/stl_fwd.hpp"
#include <ostream>

/// ipc::shm sub-module with the SHM-jemalloc SHM-provider.  See ipc::shm doc header for introduction.
namespace ipc::shm::arena_lend::jemalloc
{

// Types.

// Find doc headers near the bodies of these compound types.

class Ipc_arena;
class Memory_manager;
class Jemalloc_pages;
class Shm_pool_collection;

/**
 * Convenience alias for a shm::stl::Stateless_allocator> w/r/t Ipc_arena;
 * use with #Ipc_arena_activator.
 *
 * @tparam T
 *         Pointed-to type for the allocator.  See standard C++ `Allocator` concept.
 */
template<typename T>
using Ipc_arena_allocator = stl::Stateless_allocator<T, Ipc_arena>;

/// Alias for an Arena_activator using Ipc_arena.
using Ipc_arena_activator = stl::Arena_activator<Ipc_arena>;

// Free functions.

/**
 * Prints string representation of the given `Ipc_arena` to the given `ostream`.
 *
 * @relatesalso Ipc_arena
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Ipc_arena& val);

} // namespace ipc::shm::arena_lend::jemalloc
