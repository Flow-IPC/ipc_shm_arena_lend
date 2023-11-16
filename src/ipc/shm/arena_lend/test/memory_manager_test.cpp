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
#include "ipc/test/test_logger.hpp"
#include "ipc/shm/arena_lend/memory_manager.hpp"
#include <sys/mman.h>

using ipc::test::Test_logger;

namespace ipc::shm::arena_lend::test
{

/// Class interface death tests.
TEST(Memory_manager_DeathTest, Interface)
{
  Test_logger test_logger;
  Memory_manager memory_manager(&test_logger);

  // Allocation and deallocation
  {
    EXPECT_DEATH(memory_manager.allocate(0UL), "size > 0");
    EXPECT_DEATH(memory_manager.deallocate(nullptr), "address != nullptr");
  }
}

/// Class interface tests.
TEST(Memory_manager_test, Interface)
{
  Test_logger test_logger(flow::log::Sev::S_TRACE);
  Memory_manager memory_manager(&test_logger);

  // Allocation and deallocation
  {
    void* p1 = memory_manager.allocate(100);
    EXPECT_NE(p1, nullptr);
    void* p2 = memory_manager.allocate(1000);
    EXPECT_NE(p2, nullptr);
    memory_manager.deallocate(p2);
    memory_manager.deallocate(p1);
  }
}

} // namespace ipc::shm::arena_lend::test
