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
#include <flow/util/stat/stat_fwd.hpp>
#include <flow/util/util_fwd.hpp>
#include <cstddef>
#include <cstdint>

/// SHM-jemalloc internal (non-public) implementation details.  See also stat-oriented sub-namespace: detail::stat.
namespace ipc::shm::arena_lend::jemalloc::detail
{

// Free functions.

/**
 * Returns the process-global mutex serializing any mutation of jemalloc's arena set (arena create/destroy)
 * against a whole-arena-set statistics dump (jemalloc `malloc_stats_print()`), so that the two never overlap.
 *
 * ### Why this exists ###
 * `malloc_stats_print()` walks every arena unguarded: it first caches which arenas are `initialized`, then reads
 * each arena's `stats.*`; if an arena is *destroyed* (from any thread) in that window, the per-arena read fails
 * and jemalloc *aborts the process* (its internal `xmallctlbymib()` is an abort-on-error wrapper).  This is not a
 * jemalloc bug -- the full dump assumes a quiescent arena set.  Holding this mutex around (1) each arena-destroy,
 * (2) each arena-create, and (3) each such dump makes that assumption hold.
 *
 * @note There might be other global-arena-list-touching ops out there in universe; if so they should probably
 *       be bracketed with this lock too.
 *
 * An unguarded stats-dump happening at a bad time, sans this mutex, tends to sporadically abort with
 * a message like `<jemalloc>: Failure in xmallctlbymib()` and variations (always with `<jemalloc>: ` prefix
 * at least).
 *
 * The single-arena, return-checked stat-queries in Ipc_arena::memory_manager_stats() do *not* need this (they touch
 * only their own live arena and check errors instead of aborting); this mutex concerns exclusively the whole-set dump
 * versus arena-set mutation.
 *
 * ### Lifetime ###
 * The returned mutex is intentionally immortal (never destroyed): it is locked at "touchy" times -- past `main()`
 * and during thread exit, where the arena-destroy path might run -- at which point a destructible function-local
 * `static` could already have been destroyed.
 *
 * ### Locking discipline ###
 * Treat it as a leaf: acquire it only immediately around the single jemalloc call (destroy/create/dump) and hold
 * no other lock while it is held.
 *
 * @return Reference to the immortal process-global mutex.
 */
flow::util::Mutex_non_recursive& jemalloc_arena_list_mutex();

} // namespace ipc::shm::arena_lend::jemalloc::detail

/**
 * Stats-related sub-namespace within SHM-jemalloc's `detail`, for general organization (and ADL segregation if needed).
 * It contains the low-level *substrate* over jemalloc's `mallctl()` statistics surface: thin, stateless helpers
 * (epoch refresh, typed `mallctl`/`mallctlbymib` reads, MIB resolution) on which the higher-level bespoke
 * SHM-jemalloc stat snapshots and the `malloc_stats_print()` dump are built.
 *
 * @note It is stateless... pretty much.  Certain things do feature some initialize-once local `static`s.
 *       In all cases these represent, essentially, constants -- just not ones available at compile-time.
 */
namespace ipc::shm::arena_lend::jemalloc::detail::stat
{

// Free functions.

/**
 * Returns whether the linked jemalloc was built with statistics support (its `config.stats` is `true`).  The
 * value is read once -- it is a compile-time constant within a given jemalloc build -- and cached.
 *
 * If this returns `false`, every `stats.*` `mallctl` query yields `ENOENT`; accordingly the bespoke SHM-jemalloc
 * stat snapshots shall, as a matter of policy, produce no real data (return defaults/sentinels) rather
 * than partial results.
 *
 * @return See above.
 */
bool config_stats_enabled();

/**
 * Returns the jemalloc page size in bytes (`arenas.page`), read once and cached (constant for the process).
 * Several jemalloc gauges (e.g. `stats.arenas.(i).pdirty`) are denominated in pages; multiply by this to obtain
 * bytes.
 *
 * @return See above.
 */
size_t page_size();

/**
 * Advances jemalloc's internal statistics epoch (writes `mallctl("epoch")`), causing subsequently-read `stats.*`
 * values to reflect a fresh, mutually-consistent snapshot.  Call once immediately before a batch of `stats.*`
 * reads; a single call suffices regardless of how many arenas are read in that batch.
 *
 * ### Background: jemalloc's stats `epoch` ###
 * jemalloc does not recompute the `stats.*` values on each read; it keeps a *cached snapshot* of all dynamic
 * statistics and serves reads from it.  That snapshot is (re)computed only when you *write* to the `epoch`
 * `mallctl` -- writing any value refreshes the reported data and bumps a monotonic epoch counter; merely
 * *reading* `epoch` refreshes nothing.  Man-page wording (the `epoch` entry): "If a value is passed in, refresh
 * the data from which the `mallctl*()` functions report values, and increment the epoch."
 *
 * Recipe (what we do): write `epoch` once, then read every `stats.*` value of interest.  A single refresh yields
 * one mutually-consistent moment across all arenas/fields read afterward.
 *
 * Stakes: with no refresh, `stats.*` reads are stale -- frozen at the last snapshot, or all-zero if none was
 * ever taken -- so they won't reflect reality.  Refreshing again *mid-batch* is worse than not at all: you would
 * mix values from different moments (a "torn" set), which can corrupt derived quantities (e.g. live-count =
 * `nmalloc - ndalloc` could come out wrong or even negative).  Hence: exactly one refresh per snapshot batch,
 * up front.
 */
void epoch_refresh();

/**
 * Resolves a `mallctl` name -- which may contain integer components (e.g. the `0`s in
 * `stats.arenas.0.bins.0.nmalloc`) -- to its Management Information Base (MIB), enabling repeated reads via
 * mallctl_read_mib() without repeated name lookups.  Per the jemalloc MIB convention the integer components
 * become fixed MIB slots that the caller may mutate per iteration (e.g. set the arena-index slot to each JAID).
 *
 * @param name
 *        The (NUL-terminated) `mallctl` name to resolve.
 * @param mib
 *        Buffer receiving the resolved MIB; must have capacity for at least `*mib_len` components.
 * @param mib_len
 *        In: the capacity of `mib` (the name's component count, e.g. 6 for `stats.arenas.0.bins.0.nmalloc`).
 *        Out: the actual resolved MIB length.
 * @return `true` on success; `false` on any `mallctl` error.
 */
bool mallctl_name_to_mib(const char* name, size_t* mib, size_t* mib_len);

/**
 * Reads a single scalar `mallctl` value, by name, into `*val` (using `sizeof(T)` as the value size).  Intended
 * for one-off reads (init-time constants, scalars); for repeated reads of an indexed path prefer
 * mallctl_name_to_mib() + mallctl_read_mib().
 *
 * @tparam T
 *         The scalar type of the value as dictated by jemalloc for `name` (e.g. `size_t` for byte gauges,
 *         `uint64_t` for cumulative counters, `bool` for `config.*`).  A wrong-sized type yields failure
 *         (jemalloc requires an exact size match).
 * @param name
 *        The (NUL-terminated) `mallctl` name to read.
 * @param val
 *        On success, set to the read value; untouched on failure.
 * @return `true` on success; `false` on any `mallctl` error (e.g. `ENOENT` if `name` is unavailable in this build).
 */
template<typename T>
bool mallctl_read(const char* name, T* val);

/**
 * Like mallctl_read() but addresses the value by a previously-resolved MIB (see mallctl_name_to_mib()) rather
 * than by name; this is the efficient path for repeated/indexed reads.
 *
 * @tparam T
 *         See mallctl_read().
 * @param mib
 *        The resolved MIB; its integer slots may have been mutated by the caller to select an arena/bucket.
 * @param mib_len
 *        The MIB length (as produced by mallctl_name_to_mib()).
 * @param val
 *        On success, set to the read value; untouched on failure.
 * @return `true` on success; `false` on any `mallctl` error.
 */
template<typename T>
bool mallctl_read_mib(const size_t* mib, size_t mib_len, T* val);

/**
 * Computes, for the given jemalloc-arena, the cumulative bytes-ever-allocated and bytes-ever-deallocated (by
 * summing each per-size-class cumulative op-count times that size-class's byte-size), and populates a
 * by-size-class allocation-count histogram spanning all classes (jemalloc's small `bins` then its large
 * `lextents`, concatenated) -- all in one walk over the per-size-class counters.  jemalloc has no direct
 * cumulative-bytes counter: hence the aforementioned derivation.
 *
 * The histogram, with its request-range bucket bounds (see the by-size histogram member docs in
 * arena_lend_stats.hpp), is (re)constructed fresh, then its buckets are overwritten from `nmalloc`
 * (region allocations).  So it is an ACCUMULATOR (with N per-bucket counts) in the
 * `flow::util::stat::Stat_type` sense.
 *
 * Pre-conditions: jemalloc-stats enabled within jemalloc; jemalloc-stats epoch refreshed.
 *
 * @param arena_id
 *        The jemalloc-arena to read.
 * @param alloc_sz
 *        Out: cumulative bytes allocated.  Must not be null.
 * @param dealloc_sz
 *        Out: cumulative bytes deallocated.  Must not be null.
 * @param alloc_count_by_size
 *        Out: by-size-class cumulative alloc-count histogram (from `nmalloc`; overwritten).  Must not be null.
 */
void cumulative_alloc_dealloc_sz(arena_id_t arena_id, uint64_t* alloc_sz, uint64_t* dealloc_sz,
                                 flow::util::stat::Histogram_counter* alloc_count_by_size);

} // namespace ipc::shm::arena_lend::jemalloc::detail::stat
