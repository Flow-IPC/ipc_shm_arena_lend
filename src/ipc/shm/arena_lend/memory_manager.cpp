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

#include "ipc/shm/arena_lend/memory_manager.hpp"
#include "ipc/common.hpp"
#include <sys/mman.h>
#include <string.h>
#include <errno.h>

using std::size_t;
using flow::log::Logger;

namespace ipc::shm::arena_lend
{

Memory_manager::Memory_manager(Logger* logger) :
  Log_context(logger, Log_component::S_SHM)
{
}

void* Memory_manager::allocate(size_t size)
{
  assert(size > 0);
  void* address = ::malloc(size);
  FLOW_LOG_DATA("Allocated size " << size << ", address " << address);
  return address;
}

void Memory_manager::deallocate(void* address)
{
  assert(address != nullptr);
  FLOW_LOG_DATA("Deallocating address at " << address);
  ::free(address);
}

} // namespace ipc::shm::arena_lend
