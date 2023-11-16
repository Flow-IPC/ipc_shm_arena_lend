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

#include <cstddef>
#include <flow/log/log.hpp>

namespace ipc::shm::arena_lend
{

/**
 * Manages memory allocations and deallocations.
 */
class Memory_manager :
  public flow::log::Log_context
{
public:
  /**
   * Constructor.
   *
   * @param logger For logging purposes.
   */
  Memory_manager(flow::log::Logger* logger);
  /// Destructor.
  virtual ~Memory_manager() = default;

  /**
   * Allocates uninitialized memory using the system allocator.
   *
   * @param size The allocation size, which must be greater than zero.
   *
   * @return Upon success, a non-null pointer to the base address of the allocation.
   */
  virtual void* allocate(std::size_t size);
  /**
   * Deallocates previously allocated memory using the system deallocator.
   *
   * @param address The address to be deallocated, which must be non-null.
   */
  virtual void deallocate(void* address);
}; // class Memory_manager

} // namespace ipc::shm::arena_lend
