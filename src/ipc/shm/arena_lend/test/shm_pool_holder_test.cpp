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

#include <gtest/gtest.h>
#include "ipc/shm/arena_lend/shm_pool_holder.hpp"
#include "ipc/shm/arena_lend/shm_pool.hpp"

using std::make_shared;
using std::size_t;
using std::string;

namespace ipc::shm::arena_lend::test
{

/// Class interface tests.
TEST(Shm_pool_holder_test, Interface)
{
  const Shm_pool::pool_id_t SHM_POOL_ID(33);;
  const string SHM_POOL_NAME("Borrower_shm_pool_repository_test_pool");
  void* const SHM_POOL_ADDRESS = reinterpret_cast<void*>(0x10);
  constexpr size_t SHM_POOL_SIZE = 4096;
  constexpr int SHM_POOL_FD = 1;

  auto shm_pool = make_shared<Shm_pool>(SHM_POOL_ID, SHM_POOL_NAME, SHM_POOL_ADDRESS, SHM_POOL_SIZE, SHM_POOL_FD);
  Shm_pool_holder shm_pool_holder(shm_pool);
  auto stored_shm_pool = shm_pool_holder.get_shm_pool();
  if (stored_shm_pool == nullptr)
  {
    ADD_FAILURE() << "Shared memory pool is nullptr";
    return;
  }

  EXPECT_EQ(*stored_shm_pool, *shm_pool);
}

} // namespace ipc::shm::arena_lend::test
