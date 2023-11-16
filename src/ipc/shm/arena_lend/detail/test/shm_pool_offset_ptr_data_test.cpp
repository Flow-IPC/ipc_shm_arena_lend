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
#include "ipc/shm/arena_lend/shm_pool_repository.hpp"
#include "ipc/shm/arena_lend/shm_pool_repository_singleton.hpp"
#include "ipc/shm/arena_lend/detail/shm_pool_offset_ptr_data.hpp"
#include <sstream>

using std::make_shared;
using std::stringstream;
using std::shared_ptr;
using std::string;
using std::size_t;

namespace ipc::shm::arena_lend::detail::test
{

namespace
{

/// Test pool ID.
static const Shm_pool::pool_id_t S_TEST_POOL_ID = 1;
/// Test pool name, which won't actually be opened as a shared memory pool.
static const string S_TEST_POOL_NAME("Test_pool");
/// Test pool address.
static const auto S_TEST_POOL_ADDRESS = reinterpret_cast<void*>(0x10000);
/// Test pool size.
static constexpr size_t S_TEST_POOL_SIZE = 1024 * 10;
/// Test pool file descriptor.
static constexpr int S_TEST_POOL_FD = 10;
/// Offset to increment or decrement by.
static constexpr Shm_pool::size_t S_TEST_OFFSET_1 = 0x100;
/// Another offset to increment or decrement by.
static constexpr Shm_pool::size_t S_TEST_OFFSET_2 = 0x010;

/// Wrapper around Shm_pool_repository for testing purposes
class Test_shm_pool_repository :
  public Shm_pool_repository<Shm_pool_holder>
{
}; // class Test_shm_pool_repository

/// Alias for the repository singleton type.
using Test_repository = Shm_pool_repository_singleton<Test_shm_pool_repository>;
/// Alias for the combo offset/raw pointer data type.
using Raw_supported_data = Shm_pool_offset_ptr_data<Test_repository, true>;
/// Alsias for the offset only pointer data type.
using Offset_only_data = Shm_pool_offset_ptr_data<Test_repository, false>;

} // Anonymous namespace

/**
 * Does offset-type pointer actions -- for the specified variation of Shm_pool_offset_ptr_data.
 * I.e., Offset_only_data => do the only type of action it can do (ish); Raw_supported_data => do those same actions
 * but no more. The code paths are different(ish), so it's worthwhile to try both.
 */
template <typename Data>
static void offset_tests();

///// Shm_pool_offset_ptr_data class for raw supported.
TEST(Shm_pool_offset_ptr_data_test, Raw_supported)
{
  stringstream ss;

  /// No parameter constructor
  Raw_supported_data data;
  EXPECT_TRUE(!data);
  EXPECT_EQ(data.get(), nullptr);
  EXPECT_FALSE(data.is_raw());
  ss << data;

  // Pointer constructor with unregistered pool
  data = S_TEST_POOL_ADDRESS;
  EXPECT_TRUE(!!data);
  EXPECT_EQ(data.get(), S_TEST_POOL_ADDRESS);
  EXPECT_TRUE(data.is_raw());
  ss << data;

  // Copy constructor interface
  {
    Raw_supported_data data_2 = data;
    EXPECT_TRUE(!!data_2);
    EXPECT_TRUE(data_2.is_raw());
    EXPECT_EQ(data.get(), data_2.get());
  }

  data.increment(S_TEST_OFFSET_1);
  EXPECT_EQ(data.get(), static_cast<const uint8_t*>(S_TEST_POOL_ADDRESS) + S_TEST_OFFSET_1);
  data.increment(-S_TEST_OFFSET_2);
  EXPECT_EQ(data.get(), static_cast<const uint8_t*>(S_TEST_POOL_ADDRESS) + S_TEST_OFFSET_1 - S_TEST_OFFSET_2);

  {
    const shared_ptr<Shm_pool> TEST_POOL =
      make_shared<Shm_pool>(S_TEST_POOL_ID, S_TEST_POOL_NAME, S_TEST_POOL_ADDRESS, S_TEST_POOL_SIZE, S_TEST_POOL_FD);

    // Register pool
    Test_repository::get_instance().insert(TEST_POOL);

    Raw_supported_data data_2 = data;
    EXPECT_TRUE(!!data_2);
    // We copy contents as we copy into the same type
    EXPECT_TRUE(data_2.is_raw());
    EXPECT_EQ(data.get(), data_2.get());
    ss << data_2;

    // Copy to a offset only type
    {
      Offset_only_data data_3 = data_2;
      EXPECT_TRUE(!!data_3);
      // As we copied into a different type, we translated the pointer
      EXPECT_FALSE(data_3.is_raw());
      EXPECT_EQ(data_2.get(), data_3.get());
      ss << data_3;
    }

    data_2.increment(S_TEST_OFFSET_2);
    EXPECT_EQ(data_2.get(), static_cast<const uint8_t*>(S_TEST_POOL_ADDRESS) + S_TEST_OFFSET_1);
    data_2.increment(-S_TEST_OFFSET_1);
    EXPECT_EQ(data_2.get(), S_TEST_POOL_ADDRESS);

    // Deregister pool
    EXPECT_TRUE(Test_repository::get_instance().erase(TEST_POOL->get_id()));
  }

  offset_tests<Raw_supported_data>();
}

/// Shm_pool_offset_ptr_data class for offset only.
TEST(Shm_pool_offset_ptr_data_test, Offset_only)
{
  offset_tests<Offset_only_data>();
}

/**
 * Does offset-type pointer actions -- for the specified variation of Shm_pool_offset_ptr_data.
 * I.e., Offset_only_data => do the only type of action it can do (ish); Raw_supported_data => do those same actions
 * but no more than those. The tested code paths are different(ish), so it's worthwhile to try both.
 */
template <typename Data>
void offset_tests() // Static.
{
  stringstream ss;
  const shared_ptr<Shm_pool> TEST_POOL =
    make_shared<Shm_pool>(S_TEST_POOL_ID, S_TEST_POOL_NAME, S_TEST_POOL_ADDRESS, S_TEST_POOL_SIZE, S_TEST_POOL_FD);

  // No parameter constructor
  Data data;
  EXPECT_TRUE(!data);
  EXPECT_EQ(data.get(), nullptr);
  EXPECT_FALSE(data.is_raw());
  ss << data;

  // Pointer constructor with unregistered pool
  data = S_TEST_POOL_ADDRESS;
  if constexpr(std::is_same_v<Data, Offset_only_data>)
  {
    EXPECT_TRUE(!data);
    EXPECT_EQ(data.get(), nullptr);
    EXPECT_FALSE(data.is_raw());
    ss << data;

    // Copy construct from nullptr
    {
      auto data_2 = data;
      EXPECT_TRUE(!data_2);
      EXPECT_FALSE(data_2.is_raw());
      EXPECT_EQ(data.get(), data_2.get());
    }
  }
  else
  {
    static_assert(std::is_same_v<Data, Raw_supported_data>, "WTF?");
    EXPECT_TRUE(data);
    EXPECT_NE(data.get(), nullptr);
    EXPECT_TRUE(data.is_raw());
    ss << data;

    // Copy construct
    {
      auto data_2 = data;
      EXPECT_TRUE(data_2);
      EXPECT_TRUE(data_2.is_raw());
      EXPECT_EQ(data.get(), data_2.get());
    }
  }

  {
    // Register pool
    Test_repository::get_instance().insert(TEST_POOL);

    // Pointer constructor with registered pool
    data = S_TEST_POOL_ADDRESS;
    EXPECT_TRUE(!!data);
    EXPECT_EQ(data.get(), S_TEST_POOL_ADDRESS);
    EXPECT_FALSE(data.is_raw());
    ss << data;

    {
      // Copy constructor with like object
      auto data_2 = data;
      EXPECT_TRUE(!!data_2);
      EXPECT_FALSE(data_2.is_raw());
      EXPECT_EQ(data.get(), data_2.get());
    }

    data.increment(S_TEST_OFFSET_1);
    EXPECT_EQ(data.get(), static_cast<const uint8_t*>(S_TEST_POOL_ADDRESS) + S_TEST_OFFSET_1);
    data.increment(-S_TEST_OFFSET_2);
    EXPECT_EQ(data.get(), static_cast<const uint8_t*>(S_TEST_POOL_ADDRESS) + S_TEST_OFFSET_1 - S_TEST_OFFSET_2);

    {
      // Copy into raw supported (reminder: source is either raw-supported or offset-only, at compile-time)
      Raw_supported_data data_2 = data;
      EXPECT_TRUE(!!data_2);
      EXPECT_EQ(data.get(), data_2.get());
      EXPECT_FALSE(data_2.is_raw());
      ss << data_2;
    }

    // Decrement out of pool (still non-null pointer value by contract)
    data.increment(-S_TEST_OFFSET_1);
    EXPECT_NE(data.get(), nullptr);
    EXPECT_EQ(data.get(), static_cast<void*>(static_cast<uint8_t*>(S_TEST_POOL_ADDRESS) - S_TEST_OFFSET_2));
    EXPECT_TRUE(bool(data)); // Use bool() instead of !! for variety.

    {
      // Copy into raw supported, but the translated pointer is out of pool (still non-null pointer value by contract)
      Raw_supported_data data_2 = data;
      EXPECT_TRUE(data_2);
      /* This is subtle... we're essentially restating the tested code's logic but:
       * If data_2 + data are *both* Raw_supported_data the copy ctor is invoked; hence the out-of-pool handle (ID,
       * out-of-bounds offset) is simply copied into data_2. Therefore !is_raw().
       * If they are of different types, then data_2 is constructed by lookup from data.get(); since it's out of pool
       * it'll store it as a raw pointer. Hence is_raw(). */
      EXPECT_EQ(data_2.is_raw(), (std::is_same_v<Data, Offset_only_data>));
      EXPECT_EQ(data_2.get(), data.get()); // Either way they point to the same place!
      ss << data_2;
    }

    /* Increment back into pool, showing that it's possible to travel around invalid-in-a-sense boundary conditions
     * and get back onto solid ground. (As noted elsewhere, such things enable sloppy comparisons around the start
     * or end of a pool to work, as they would with native or raw pointers.) */
    data.increment(S_TEST_OFFSET_2);
    EXPECT_TRUE(bool(data));
    EXPECT_EQ(data.get(), S_TEST_POOL_ADDRESS);
    ss << data;

    // Now in the other direction -- go past *end* of pool and then back.
    {
      data = nullptr;
      EXPECT_FALSE(data);
      data = static_cast<const uint8_t*>(S_TEST_POOL_ADDRESS) + S_TEST_POOL_SIZE - S_TEST_OFFSET_2;
      auto data_2 = data;
      EXPECT_TRUE(data); // Sanity check.
      EXPECT_LT(data.get(), static_cast<const uint8_t*>(S_TEST_POOL_ADDRESS) + S_TEST_POOL_SIZE); // Sanity check.
      // Go past pool.
      data.increment(S_TEST_OFFSET_1);
      EXPECT_GE(data.get(), static_cast<const uint8_t*>(S_TEST_POOL_ADDRESS) + S_TEST_POOL_SIZE); // Sanity check.
      EXPECT_NE(data.get(), data_2.get()); // Sanity check.
      EXPECT_TRUE(data); // Still a pointer even though out of pool to which it internally refers.
      data.increment(-S_TEST_OFFSET_1); // And go back.
      EXPECT_TRUE(data);
      EXPECT_EQ(data.get(), data_2.get());
      // Travel around the boundary a bit.
      data.increment(S_TEST_OFFSET_2 - 1); // Point to last byte of pool.
      EXPECT_EQ(data.get(), static_cast<const uint8_t*>(TEST_POOL->get_address()) + TEST_POOL->get_size() - 1);
      EXPECT_TRUE(data);
      EXPECT_TRUE(TEST_POOL->is_subset(data.get(), 1));
      data.increment(1);
      EXPECT_EQ(data.get(), static_cast<const uint8_t*>(TEST_POOL->get_address()) + TEST_POOL->get_size());
      EXPECT_TRUE(data);
      EXPECT_FALSE(TEST_POOL->is_subset(data.get(), 1));
      ss << data;
      data = nullptr;
      EXPECT_FALSE(data);
    }

    // Deregister pool
    EXPECT_TRUE(Test_repository::get_instance().erase(TEST_POOL->get_id()));
  }
}

} // namespace ipc::shm::arena_lend::detail::test
