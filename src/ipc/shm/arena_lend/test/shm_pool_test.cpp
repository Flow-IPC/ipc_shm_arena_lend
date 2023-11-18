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
#include "ipc/shm/arena_lend/shm_pool.hpp"

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

/// Death tests (@todo - does it work with NDEBUG as with default CMake Release build-type? -ygoldfel)
TEST(Shm_pool_DeathTest, Interface)
{
  EXPECT_DEATH(Shm_pool(0, S_NAME, S_ADDRESS, S_SIZE, S_FD), "id != 0");
  EXPECT_DEATH(Shm_pool(S_ID, "", S_ADDRESS, S_SIZE, S_FD), "!name.empty\\(\\)");
  EXPECT_DEATH(Shm_pool(S_ID, S_NAME, nullptr, S_SIZE, S_FD), "address != nullptr");
  EXPECT_DEATH(Shm_pool(S_ID, S_NAME, S_ADDRESS, 0, S_FD), "size > 0");
  EXPECT_DEATH(Shm_pool(S_ID, S_NAME, S_ADDRESS, S_SIZE, 0), "fd > 0");
  EXPECT_DEATH(Shm_pool(S_ID, S_NAME, S_ADDRESS, S_SIZE, -1), "fd > 0");
}

/// Class interface tests
TEST(Shm_pool_test, Interface)
{
  Shm_pool shm_pool(S_ID, S_NAME, S_ADDRESS, S_SIZE, S_FD);
  EXPECT_EQ(shm_pool.get_id(), S_ID);
  EXPECT_EQ(shm_pool.get_name(), S_NAME);
  EXPECT_EQ(shm_pool.get_address(), S_ADDRESS);
  EXPECT_EQ(size_t(shm_pool.get_size()), S_SIZE);
  EXPECT_EQ(shm_pool.get_fd(), S_FD);
  // Sanity check to make sure it doesn't crash
  std::ostringstream os;
  shm_pool.print(os);

  const Shm_pool::size_t OFFSET_1 = 0x8;
  const Shm_pool::size_t OFFSET_2 = 0x24;
  Shm_pool::size_t offset;

  {
    EXPECT_TRUE(shm_pool.determine_offset(S_ADDRESS, offset));
    EXPECT_EQ(offset, static_cast<Shm_pool::size_t>(0));
    EXPECT_TRUE(shm_pool.determine_offset(S_ADDRESS + OFFSET_1, offset));
    EXPECT_EQ(offset, OFFSET_1);
    EXPECT_TRUE(shm_pool.determine_offset(S_ADDRESS + OFFSET_2, offset));
    EXPECT_EQ(offset, OFFSET_2);
    EXPECT_TRUE(shm_pool.determine_offset(S_ADDRESS + (S_SIZE - 1), offset));
    EXPECT_EQ(offset, static_cast<Shm_pool::size_t>(S_SIZE - 1));
    // Address prior to region
    EXPECT_FALSE(shm_pool.determine_offset(S_ADDRESS - 1, offset));
    // Address after region
    EXPECT_FALSE(shm_pool.determine_offset(S_ADDRESS + S_SIZE, offset));
  }

  {
    // Start address prior to pool
    {
      char* address = S_ADDRESS - OFFSET_1;

      // Start address prior to pool, end address prior to pool
      EXPECT_FALSE(shm_pool.is_subset(address, OFFSET_1 - 1, nullptr));
      // Start address prior to pool, end address at start of pool
      EXPECT_FALSE(shm_pool.is_subset(address, OFFSET_1, nullptr));
      // Start address prior to pool, end address in pool
      EXPECT_FALSE(shm_pool.is_subset(address, OFFSET_1 + 1, nullptr));
      // Start address prior to pool, end address at end of pool
      EXPECT_FALSE(shm_pool.is_subset(address, OFFSET_1 + S_SIZE, nullptr));
      // Start address prior to pool, end address past end of pool
      EXPECT_FALSE(shm_pool.is_subset(address, OFFSET_1 + S_SIZE + 1, nullptr));
    }
    // Start address at start of pool
    {
      // Start address at start of pool, end address in pool
      EXPECT_TRUE(shm_pool.is_subset(S_ADDRESS, S_SIZE - 1, &offset));
      EXPECT_EQ(offset, static_cast<Shm_pool::size_t>(0));
      // Start address at start of pool, end address at end of pool
      EXPECT_TRUE(shm_pool.is_subset(S_ADDRESS, S_SIZE, &offset));
      EXPECT_EQ(offset, static_cast<Shm_pool::size_t>(0));
      // Start address at start of pool, end address past end of pool
      EXPECT_FALSE(shm_pool.is_subset(S_ADDRESS, S_SIZE + 1, &offset));
    }
    // Start address in pool
    {
      char* address = S_ADDRESS + OFFSET_1;

      // Start address in pool, end address in pool
      EXPECT_TRUE(shm_pool.is_subset(address, S_SIZE - OFFSET_1 - 1, &offset));
      EXPECT_EQ(offset, OFFSET_1);
      // Start address in pool, end address at end of pool
      EXPECT_TRUE(shm_pool.is_subset(address, S_SIZE - OFFSET_1, &offset));
      EXPECT_EQ(offset, OFFSET_1);
      // Start address in pool, end address past end of pool
      EXPECT_FALSE(shm_pool.is_subset(address, S_SIZE - OFFSET_1 + 1, &offset));
    }
    // Start address at end of pool, end address past end of pool
    EXPECT_FALSE(shm_pool.is_subset(S_ADDRESS + S_SIZE, 1, &offset));
    // Start address past end of pool, end address past end of pool
    EXPECT_FALSE(shm_pool.is_subset(S_ADDRESS + S_SIZE + 1, 1, &offset));
  }

  {
    auto is_adjacent_functor =
      [&](void* address, size_t size) -> bool
      {
        return Shm_pool::is_adjacent(S_ADDRESS, S_SIZE, address, size);
      };

    // Start address prior to pool
    {
      char* address = S_ADDRESS - OFFSET_1;

      // Start address prior to pool, end address prior to pool
      EXPECT_FALSE(is_adjacent_functor(address, OFFSET_1 - 1));
      // Start address prior to pool, end address at start of pool
      EXPECT_TRUE(is_adjacent_functor(address, OFFSET_1));
      // Start address prior to pool, end address in pool
      EXPECT_FALSE(is_adjacent_functor(address, OFFSET_1 + 1));
      // Start address prior to pool, end address at end of pool
      EXPECT_FALSE(is_adjacent_functor(address, OFFSET_1 + S_SIZE));
      // Start address prior to pool, end address past end of pool
      EXPECT_FALSE(is_adjacent_functor(address, OFFSET_1 + S_SIZE + 1));

    }
    // Start address at start of pool
    {
      // Start address at start of pool, end address in pool
      EXPECT_FALSE(is_adjacent_functor(S_ADDRESS, S_SIZE - 1));
      // Start address at start of pool, end address at end of pool
      EXPECT_FALSE(is_adjacent_functor(S_ADDRESS, S_SIZE));
      // Start address at start of pool, end address past end of pool
      EXPECT_FALSE(is_adjacent_functor(S_ADDRESS, S_SIZE + 1));
    }
    // Start address in pool
    {
      char* address = S_ADDRESS + OFFSET_1;

      // Start address in pool, end address in pool
      EXPECT_FALSE(is_adjacent_functor(address, S_SIZE - OFFSET_1 - 1));
      // Start address in pool, end address at end of pool
      EXPECT_FALSE(is_adjacent_functor(address, S_SIZE - OFFSET_1));
      // Start address in pool, end address past end of pool
      EXPECT_FALSE(is_adjacent_functor(address, S_SIZE - OFFSET_1 + 1));
    }
    // Start address at end of pool, end address past end of pool
    EXPECT_TRUE(is_adjacent_functor(S_ADDRESS + S_SIZE, 1));
    // Start address past end of pool, end address past end of pool
    EXPECT_FALSE(is_adjacent_functor(S_ADDRESS + S_SIZE + 1, 1));
  }

  {
    // Within range
    EXPECT_EQ(shm_pool.to_address(0), S_ADDRESS);
    EXPECT_EQ(shm_pool.to_address(OFFSET_1), S_ADDRESS + OFFSET_1);
    EXPECT_EQ(shm_pool.to_address(S_SIZE - 1), S_ADDRESS + (S_SIZE - 1));
    // Beyond range: still allowed (obv deref would fail, but this isn't that)
    EXPECT_EQ(shm_pool.to_address(S_SIZE), S_ADDRESS + S_SIZE);
    EXPECT_EQ(shm_pool.to_address(S_SIZE + 1), S_ADDRESS + S_SIZE + 1);
  }

  {
    EXPECT_EQ(shm_pool, shm_pool);
    {
      const Shm_pool EQUAL_SHM_POOL(S_ID, S_NAME, S_ADDRESS, S_SIZE, S_FD);
      EXPECT_EQ(shm_pool, EQUAL_SHM_POOL);
      EXPECT_EQ(EQUAL_SHM_POOL, shm_pool);
    }
    {
      // Different name
      const Shm_pool OTHER_SHM_POOL(S_ID, S_NAME + "_1", S_ADDRESS, S_SIZE, S_FD);
      EXPECT_NE(shm_pool, OTHER_SHM_POOL);
      EXPECT_NE(OTHER_SHM_POOL, shm_pool);
    }
    {
      // Different address
      const Shm_pool OTHER_SHM_POOL(S_ID, S_NAME, S_ADDRESS + 0x1, S_SIZE, S_FD);
      EXPECT_NE(shm_pool, OTHER_SHM_POOL);
      EXPECT_NE(OTHER_SHM_POOL, shm_pool);
    }
    {
      // Different size
      const Shm_pool OTHER_SHM_POOL(S_ID, S_NAME, S_ADDRESS, S_SIZE + 1, S_FD);
      EXPECT_NE(shm_pool, OTHER_SHM_POOL);
      EXPECT_NE(OTHER_SHM_POOL, shm_pool);
    }
    {
      // Different file descriptor
      const Shm_pool OTHER_SHM_POOL(S_ID, S_NAME, S_ADDRESS, S_SIZE, S_FD + 1);
      EXPECT_NE(shm_pool, OTHER_SHM_POOL);
      EXPECT_NE(OTHER_SHM_POOL, shm_pool);
    }
  }
}

} // namespace ipc::shm::arena_lend::test
