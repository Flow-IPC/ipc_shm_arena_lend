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

#include <ipc/shm/arena_lend/shm_pool_offset_ptr.hpp>

namespace ipc::shm::arena_lend
{

/**
 * (Arena-lending SHM providers only) When borrowing STL-compliant in-SHM data
 * structures, use this type as the `Arena` template parameter to
 * shm::stl::Stateless_allocator (or equivalent).  It would be unusual for most users to specify this
 * directly (e.g., session::shm::arena_lend::jemalloc::Shm_session::Borrower_arena_allocator or more generically
 * session::shm::arena_lend::jemalloc::Session::Borrower_allocator can be used directly instead).
 *
 * ### Context ###
 * Owner-side, a SHM-aware STL-allocator (notably our `Stateless_allocator`) requires an `Arena` type, and this
 * is intuitively uncontroversial: It needs to allocate/deallocate things, and in SHM-provider-land a SHM-arena
 * is what does that.  `Arena::Pointer` is also an important thing: it is the fancy-pointer type for the
 * allocator -- and by extension STL-compliant container -- code to use instead of the typical raw `T*` pointer.
 * So for SHM-jemalloc, a principal arena-lending SHM-provider, `Arena` is jemalloc::Ipc_arena, while
 * `Arena::Pointer` is `Shm_pool_offset_ptr<T, Owner_shm_pool_repository<Ipc_arena>, int, true>`.  With these
 * plugged into the SHM-aware STL-allocator (compile-time), allocation/deallocation and pointer work in
 * STL-compliant containers work nicely.
 *
 * Borrower-side, however -- given the arena-lending design -- on the one hand one obviously needs to be able
 * to access in-SHM STL containers; but on the other hand the borrower cannot [de]allocate at all (on behalf
 * of the borrowed, owner-side-allocated data structure).  Nevertheless an `Allocator` arg (to STL types)
 * is still required borrower-side.  The reason is simple: it can't allocate/deallocate anyway, but
 * the proper, compatible borrower-side pointer type is still required.
 *
 * Thus Borrower_allocator_arena is what one shall pass as the `Arena` parameter to `Stateless_allocator`; and
 * it exclusively provides #Pointer.  As not [de]allocation within borrowed containers will ever be
 * attempted, or even compiled, there is no issue: STL code will only use Stateless_allocator::Pointer which
 * is simply our #Pointer here.
 *
 * No allocate() or deallocate() methods are provided, as that would be nonsensical.
 *
 * @tparam Repository_type
 *         See `Repository_type` parameter to Shm_pool_offset_ptr.
 *         Generally in production use: `Borrower_shm_pool_collection_repository<Ipc_arena>`.
 *         As of this writing this is configurable for testing purposes.
 */
template<typename Repository_type>
class Borrower_allocator_arena
{
public:
  // Types.

  /**
   * Fancy pointer for stateless allocator use.
   *
   * @internal
   * The target will usually be `Shm_pool_offset_ptr<T, Borrower_shm_pool_collection_repository<Ipc_arena>, ..., false>`
   * of which these are of interest:
   *   - `Borrower_shm_pool_collection_repository<Ipc_arena>` is the singleton/central repository of SHM-pool
   *     information so that one can performantly convert from the inner bits of data stored inside a
   *     `Shm_pool_offset_ptr` to a raw `void*` -- `to_address()`; and vice versa (`from_address()`).  Thus
   *     STL code can get an actual process-local vaddr from an in-SHM fancy pointer; and vice versa.
   *   - `false` means `bool CAN_STORE_RAW_PTR = false`.  Borrower-side, all pointers within an STL data structure
   *     must point into SHM.  Owner-side this would be `true`, as it is possible to intermix (say) a stack-stored
   *     container but with the SHM-enabled allocator parameter; so buffers allocated on the container's behalf
   *     do live in SHM, but other parts (at least temporarily) might not and hence must be represented by
   *     raw pointers after all.  `false` here forbids the same borrower-side.
   * @endinternal
   *
   * @tparam T
   *         Pointee type.
   */
  template <typename T>
  using Pointer = Shm_pool_offset_ptr<T, Repository_type, int, false>;
}; // class Borrower_allocator_arena

} // namespace ipc::shm::arena_lend
