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
#include "ipc/util/shared_name.hpp"
#include "ipc/shm/arena_lend/arena_lend_fwd.hpp"
#include "ipc/shm/arena_lend/shm_pool.hpp"
#include "ipc/shm/arena_lend/shm_pool_collection.hpp"
#include "ipc/common.hpp"

namespace ipc::shm::arena_lend
{

/**
 * Shared memory pool collection for borrowers, which are entities (e.g., processes) that do not have access to
 * the memory manager. In other words, they cannot allocate nor deallocate memory.
 *
 * For a given `*this`, all pools belong to the same originating owner arena (opposing-process
 * Owner_shm_pool_collection) and therefore have the same (implied) owner ID (as of this writing: owner's PID)
 * and (explicitly specified in ctor) collection ID (as of this writing per-owner-ID ordinal 1, 2, ...).
 * (The two combined form a detail::Uniq_collection_id which is globally unique across processes and time, up
 * to reboot.)
 *
 * @todo There are at least some items (such as a per-object mutex) within Borrower_shm_pool_collection that
 * are extraneous, as of this writing, in actual use. That use is exclusively in
 * Borrower_shm_pool_collection_repository, which is the process-wide borrowed-pool amalgamation. In that role
 * essentially it stores the given arena's pool-name base, so that it can compute a pool's name, when opening
 * said pool at borrow time, from only a pool ID. The rest of what it provides is really just per-pool operations
 * (open pool, release pool) to which being part of a collection isn't relevant. It contains a mutex around
 * insert/erase pool ops -- but Borrower_shm_pool_collection_repository already locks a process-wide mutex -- so
 * it's a waste (a ~small one, as there's no new contention introduced). This to-do is probably less about just
 * this class and more about a holistic look at the arena_lend::Shm_pool_collection hierarchy and perhaps
 * arena_lend::Shm_pool (and that hierarchy) as well. Nothing criminal is going on (that we know of); it could just
 * stand to be refactored for design cleanliness and likely a bit of perf too.
 *
 * @todo The SHM-arena-lend/SHM-jemalloc (ipc::shm::arena_lend) module might gain from using boost.interprocess
 * primitives, particularly around SHM-pools/memory-mapping, instead of native calls to `shm_open()`, `mmap()`,
 * and the like, and storing raw pool handles. For some ops owner-side, when interacting with jemalloc especially,
 * native calls might be needed at times; but boost.interprocess is already a helpfully thin wrapper around
 * native resources and provides native handle access when desired. (E.g., in SHM-classic we use boost.interprocess
 * SHM primitives in shm::classic::Pool_arena; in some parts of SHM-arena-lend/SHM-jemalloc, like
 * detail::Lend_tracker_pool, we do as well. It's nice.)
 *
 * @todo ipc::shm::arena_lend::Borrower_shm_pool_collection (among others) should be
 * officially classified an internal API (at least, per coding guide, placed in `detail/` header; optionally in `detail`
 * sub-namespace).  Once `ipc::shm::arena_lend` (the SHM-arena-lend module) becomes officially extensible
 * to handle other memory-managers beyond jemalloc, thus allowing for user's own arena-lending SHM-provider impls
 * (not just SHM-jemalloc), then *maybe* move this back out of `detail`, if
 * indeed referencing it is expected in that case.  (As of this writing we estimate it should not be.)
 */
class Borrower_shm_pool_collection :
  public Shm_pool_collection
{
public:
  /// Short-hand for pool ID type.
  using pool_id_t = Shm_pool::pool_id_t;
  /// Short-hand for pool offset type.
  using pool_offset_t = Shm_pool::size_t;

  /**
   * Constructor.
   *
   * @param logger For logging purposes.
   * @param id Identifier for the collection.
   * @param pool_name_base Used in recompute_pool_name() and therefore open_shm_pool().
   */
  Borrower_shm_pool_collection(flow::log::Logger* logger, collection_id_t id, Shared_name&& pool_name_base);

  /**
   * Pool name prefix used in recompute_pool_name() and therefore open_shm_pool().
   * @return See above.
   */
  const Shared_name& get_pool_name_base() const;

  /**
   * Pool name that would be used by open_shm_pool() given the same `id`. As of this writing that is
   * get_pool_name_base() plus separator plus `id` (FYI when debugging and/or generally grokking how things work).
   *
   * @param id The globally unique pool ID.
   * @return See above.
   */
  Shared_name recompute_pool_name(pool_id_t id) const;

  /**
   * Opens a named shared memory object for read-only and maps it in the process' address space.
   *
   * (The error emission semantics here are a bit scruffy and not totally consistent with Flow-IPC best practices,
   * but it's fine until we go over this and nearby classes and straighten everything out stylistically.
   * Until then we at least *do* emit the reason for failure, so more "straightened out" calling code can
   * act nicely.)
   *
   * @param id Pool ID.
   * @param size Shared memory size when it was constructed.
   * @param err_code If null returned, reason shall be placed here. Otherwise (on success) please ignore `*err_code`.
   *
   * @return Upon success, a shared pointer to the shared memory pool; otherwise, an empty shared pointer.
   *         In the latter case consult `*err_code`.
   */
  std::shared_ptr<Shm_pool> open_shm_pool(pool_id_t id, std::size_t size, Error_code* err_code);

  /**
   * Unmaps and closes a shared memory pool. Note that this does not remove the shared memory object.
   *
   * @param shm_pool The shared memory pool to be released.
   *
   * @return Whether the shared memory pool was released successfully.
   */
  inline bool release_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool);

private:
  /// See get_pool_name_base().
  const Shared_name m_pool_name_base;
}; // class Borrower_shm_pool_collection

bool Borrower_shm_pool_collection::release_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool)
{
  return deregister_shm_pool(shm_pool) && close_shm_pool(shm_pool);
}

} // namespace ipc::shm::arena_lend
