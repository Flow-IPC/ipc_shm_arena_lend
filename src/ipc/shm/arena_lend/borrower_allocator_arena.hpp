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

#include <ipc/shm/arena_lend/shm_pool_offset_ptr.hpp>

namespace ipc::shm::arena_lend
{

/**
 * Provides functionality to be used as a stateless "allocator" from a borrower perspective. In actuality, no
 * allocations nor deallocations are allowed on the borrower, so the APIs are suppressed; however, we provide
 * access to the underlying objects that are stored as fancy pointers in shared memory here via the #Pointer.
 *
 * @tparam Repository_type The shared memory pool repository type to convert a handle to a pointer. The requirement
 *                         is that a static interface of "void* to_address(std::string_view, std::size_t)" exists.
 * @tparam Difference_type A signed integer type that can represent the arithmetic operations on the pointer.
 */
template <typename Repository_type, typename Difference_type>
class Borrower_allocator_arena
{
public:
  /**
   * Fancy pointer for stateless allocator use. A singleton repository will be used to convert to a raw pointer.
   * No allocations nor deallocations are allowed for the borrowed object. There should be no use of raw pointers
   * on the borrower side.
   *
   * @tparam Pointed_type The underlying type of the object being held in the fancy pointer.
   */
  template <typename Pointed_type>
  using Pointer = Shm_pool_offset_ptr<Pointed_type, Repository_type, Difference_type, false>;

#if 0
  // We intentionally don't implement this as we don't allow allocations by the borrower.
  void* allocate(std::size_t size);
  // We intentionally don't implement this as we don't allow deallocations by the borrower.
  void deallocate(void* address);
#endif // #if 0
}; // class Borrower_allocator_arena

} // namespace ipc::shm::arena_lend
