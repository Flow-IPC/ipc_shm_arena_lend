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

#include "ipc/shm/arena_lend/jemalloc/detail/jemalloc_fwd.hpp"
#include "ipc/shm/arena_lend/jemalloc/detail/jemalloc.hpp"
#include <jemalloc/jemalloc.h>
#include <cstddef>

namespace ipc::shm::arena_lend::jemalloc::detail::stat
{

// Template implementations.

template<typename T>
bool mallctl_read(const char* name, T* val)
{
  auto val_size = sizeof(T);
  return IPC_SHM_ARENA_LEND_JEMALLOC_API(mallctl)(name, val, &val_size, nullptr, 0) == 0;
}

template<typename T>
bool mallctl_read_mib(const size_t* mib, size_t mib_len, T* val)
{
  auto val_size = sizeof(T);
  return IPC_SHM_ARENA_LEND_JEMALLOC_API(mallctlbymib)(mib, mib_len, val, &val_size, nullptr, 0) == 0;
}

} // namespace ipc::shm::arena_lend::jemalloc::detail::stat
