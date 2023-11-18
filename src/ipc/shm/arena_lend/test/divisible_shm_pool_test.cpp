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
#include "ipc/shm/arena_lend/divisible_shm_pool.hpp"

using std::string;
using std::size_t;

namespace ipc::shm::arena_lend::test
{

namespace
{

using pool_id_t = Shm_pool::pool_id_t;

/// Test shared memory pool ID.
static const pool_id_t S_ID(1212);
/// Test shared memory pool name.
static const string S_NAME("test_pool");
/// Test address.
static char* const S_ADDRESS = reinterpret_cast<char*>(0x1000);
/// Test shared memory pool size.
static constexpr size_t S_SIZE = 0x2000;
/// Test file descriptor.
static constexpr int S_FD = 12;

} // Anonymous namespace

/// Class interface tests.
TEST(Divisible_shm_pool_test, Interface)
{
  {
    Divisible_shm_pool pool(S_ID, S_NAME, S_ADDRESS, S_SIZE, S_FD);
    EXPECT_EQ(pool.get_id(), S_ID);
    EXPECT_EQ(pool.get_name(), S_NAME);
    EXPECT_EQ(pool.get_address(), S_ADDRESS);
    EXPECT_EQ(size_t(pool.get_size()), S_SIZE);
    EXPECT_EQ(pool.get_fd(), S_FD);
    EXPECT_EQ(size_t(pool.get_remaining_size()), S_SIZE);
    EXPECT_FALSE(pool.is_completely_removed());

    // Sanity check to make sure printing doesn't crash
    std::ostringstream os;
    pool.print(os);

    // Remove half of the size
    const size_t SIZE_1 = S_SIZE / 2;
    const size_t EXPECTED_REMAINING_SIZE = S_SIZE - SIZE_1;
    EXPECT_TRUE(pool.remove_size(SIZE_1));
    EXPECT_EQ(pool.get_remaining_size(), EXPECTED_REMAINING_SIZE);
    EXPECT_FALSE(pool.is_completely_removed());

    // Remove remaining size
    EXPECT_TRUE(pool.remove_size(EXPECTED_REMAINING_SIZE));
    EXPECT_EQ(pool.get_remaining_size(), 0UL);
    EXPECT_TRUE(pool.is_completely_removed());
  }

  {
    Divisible_shm_pool pool(S_ID, S_NAME, S_ADDRESS, S_SIZE, S_FD);

    // Remove excessive size
    EXPECT_FALSE(pool.remove_size(S_SIZE + 1));
    EXPECT_EQ(pool.get_remaining_size(), 0UL);
    EXPECT_TRUE(pool.is_completely_removed());

    // Remove after empty
    EXPECT_FALSE(pool.remove_size(S_SIZE + 1));
    EXPECT_EQ(pool.get_remaining_size(), 0UL);
    EXPECT_TRUE(pool.is_completely_removed());
  }
}

} // namespace shm::ipc::arena_lend::test
