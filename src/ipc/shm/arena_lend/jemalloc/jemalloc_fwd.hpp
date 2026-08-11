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

#include <ostream>

/// ipc::shm sub-module with the SHM-jemalloc SHM-provider.  See ipc::shm doc header for introduction.
namespace ipc::shm::arena_lend::jemalloc
{

// Types.

// Find doc headers near the bodies of these compound types.

class Ipc_arena;
class Memory_manager;
class Jemalloc_pages;
class Thread_cache;

/// Thread cache ID type as dictated by jemalloc.
using tcache_id_t = unsigned int;

/// Arena ID as dictated by jemalloc.
using arena_id_t = unsigned int;

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

/**
 * Prints string representation of the given `Thread_cache` to the given `ostream`.
 *
 * @relatesalso Thread_cache
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Thread_cache& val);

} // namespace ipc::shm::arena_lend::jemalloc

/**
 * Stats-related sub-namespace for SHM-jemalloc, for general organization (and ADL segregation if needed).
 *
 * @see If you're looking for the copious stat-set `struct`s producible by SHM-jemalloc
 *      (ipc::shm::arena_lend::jemalloc), that is in ipc::shm::arena_lend::stat.  As of this writing all
 *      (and we suspect it'll stay that way, or close to that way, indefinitely) the stat-sets are generalized
 *      for all SHM-providers of SHM-arena-lend type: meaning the same for SHM-jemalloc versus
 *      SHM-some-other-memory-manager.
 * @see Arena_info_dump, Shm_session_info_dump: the reason arena_lend::jemalloc::stat sub-namespace was created.
 *      Neither is itself a stat-set (in the `flow::util::stat` `Stat_set` sense) -- e.g., it cannot be
 *      `stats_sum()`med -- but rather a printable convenience-bundling of stats/info + knobs affecting the
 *      printing format.  They correspond to a jemalloc::Ipc_arena (but may store global info not pertaining
 *      to that `Ipc_arena` specifically) and session::shm::arena_lend::jemalloc::Shm_session (ditto) respectively.
 */
namespace ipc::shm::arena_lend::jemalloc::stat
{

// Types.

// Find doc headers near the bodies of these compound types.

struct Arena_info_dump;
struct Shm_session_info_dump;

// Free functions.

/**
 * Prints string representation of the given `Arena_info_dump` to the given `ostream`.
 *
 * @note `val.m_fmt` (util::stat::Info_dump_format) may contain knobs to affect the shape of the printed output.
 *
 * @relatesalso Arena_info_dump
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Arena_info_dump& val);

/**
 * Prints string representation of the given `Shm_session_info_dump` to the given `ostream`.
 *
 * @note `val.m_fmt` (util::stat::Info_dump_format) may contain knobs to affect the shape of the printed output.
 *
 * @relatesalso Shm_session_info_dump
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Shm_session_info_dump& val);

} // namespace ipc::shm::arena_lend::jemalloc::stat
