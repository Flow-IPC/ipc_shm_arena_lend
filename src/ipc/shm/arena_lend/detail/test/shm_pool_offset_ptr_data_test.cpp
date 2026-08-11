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
#include "ipc/shm/arena_lend/test/test_shm_pool_repository.hpp"
#include "ipc/shm/arena_lend/detail/shm_pool_offset_ptr_data.hpp"
#include <flow/test/test_common_util.hpp>
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

/// Second test pool ID.
static const Shm_pool::pool_id_t S_TEST_POOL_ID_2 = 2;
/// Second test pool name.
static const string S_TEST_POOL_NAME_2("Test_pool_2");
/// Second test pool address (well above first pool to avoid overlap).
static const auto S_TEST_POOL_ADDRESS_2 = reinterpret_cast<void*>(0x30000);
/// Second test pool size.
static constexpr size_t S_TEST_POOL_SIZE_2 = 1024 * 10;
/// Second test pool file descriptor.
static constexpr int S_TEST_POOL_FD_2 = 11;

/// Alias for the test repository type.
using Test_repository = ipc::shm::arena_lend::test::Test_shm_pool_repository;
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
  stringstream ss; // @todo Seems we write things to it but then do nothing with that. What's the idea?

  /// No parameter constructor
  Raw_supported_data data;
  EXPECT_FALSE(data.to_bool());
  EXPECT_EQ(data.get(), nullptr);
  EXPECT_FALSE(data.is_raw());
  ss << data;

  // Pointer constructor with unregistered pool
  data = S_TEST_POOL_ADDRESS;
  EXPECT_TRUE(data.to_bool());
  EXPECT_EQ(data.get(), S_TEST_POOL_ADDRESS);
  EXPECT_TRUE(data.is_raw());
  ss << data;

  // Copy constructor interface
  {
    Raw_supported_data data_2 = data;
    EXPECT_TRUE(data_2.to_bool());
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
    Test_repository::get_instance().insert(shared_ptr<Shm_pool>{TEST_POOL});

    Raw_supported_data data_2 = data;
    EXPECT_TRUE(data_2.to_bool());
    // We copy contents as we copy into the same type
    EXPECT_TRUE(data_2.is_raw());
    EXPECT_EQ(data.get(), data_2.get());
    ss << data_2;

    // Copy to a offset only type
    {
      Offset_only_data data_3 = data_2;
      // Raw-allowed, raw-containing ptr -> no-raw-allowed -> contract says save null, don't attempt backwards-lookup.
      EXPECT_FALSE(data_3.to_bool());
      EXPECT_FALSE(data_3.is_raw());
      EXPECT_EQ(data_3.get(), nullptr);
      ss << data_3;
    }

    data_2.increment(S_TEST_OFFSET_2);
    EXPECT_EQ(data_2.get(), static_cast<const uint8_t*>(S_TEST_POOL_ADDRESS) + S_TEST_OFFSET_1);
    data_2.increment(-S_TEST_OFFSET_1);
    EXPECT_EQ(data_2.get(), S_TEST_POOL_ADDRESS);

    // Deregister pool
    Test_repository::get_instance().erase(TEST_POOL->get_id());
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
  EXPECT_FALSE(data.to_bool());
  EXPECT_EQ(data.get(), nullptr);
  EXPECT_FALSE(data.is_raw());
  ss << data;

  // Pointer constructor with unregistered pool
  data = S_TEST_POOL_ADDRESS;
  if constexpr(std::is_same_v<Data, Offset_only_data>)
  {
    EXPECT_FALSE(data.to_bool());
    EXPECT_EQ(data.get(), nullptr);
    EXPECT_FALSE(data.is_raw());
    ss << data;

    // Copy construct from nullptr
    {
      auto data_2 = data;
      EXPECT_FALSE(data.to_bool());
      EXPECT_FALSE(data_2.is_raw());
      EXPECT_EQ(data.get(), data_2.get());
    }
  }
  else
  {
    static_assert(std::is_same_v<Data, Raw_supported_data>, "WTF?");
    EXPECT_TRUE(data.to_bool());
    EXPECT_NE(data.get(), nullptr);
    EXPECT_TRUE(data.is_raw());
    ss << data;

    // Copy construct
    {
      auto data_2 = data;
      EXPECT_TRUE(data_2.to_bool());
      EXPECT_TRUE(data_2.is_raw());
      EXPECT_EQ(data.get(), data_2.get());
    }
  }

  {
    // Register pool
    Test_repository::get_instance().insert(shared_ptr<Shm_pool>{TEST_POOL});

    // Pointer constructor with registered pool
    data = S_TEST_POOL_ADDRESS;
    EXPECT_TRUE(data.to_bool());
    EXPECT_EQ(data.get(), S_TEST_POOL_ADDRESS);
    EXPECT_FALSE(data.is_raw());
    ss << data;

    {
      // Copy constructor with like object
      auto data_2 = data;
      EXPECT_TRUE(data_2.to_bool());
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
      EXPECT_TRUE(data_2.to_bool());
      EXPECT_EQ(data.get(), data_2.get());
      EXPECT_FALSE(data_2.is_raw());
      ss << data_2;
    }

    // Decrement out of pool (still non-null pointer value by contract)
    data.increment(-S_TEST_OFFSET_1);
    EXPECT_NE(data.get(), nullptr);
    EXPECT_EQ(data.get(), static_cast<void*>(static_cast<uint8_t*>(S_TEST_POOL_ADDRESS) - S_TEST_OFFSET_2));
    EXPECT_TRUE(data.to_bool());

    {
      // Copy into raw supported, but the translated pointer is out of pool (still non-null pointer value by contract)
      Raw_supported_data data_2 = data;
      EXPECT_TRUE(data_2.to_bool());
      /* If Data=Raw_supported_data, then it's a straight copy ctor: out of pool or not, the target and src shall
       * be exactly the same; not just via .get() but .is_raw() would be false for both. The more interesting
       * case is Data=Offset_only_data. Then it's not the copy ctor but the type-converting copy-like ctor.
       * In that case... same, in fact. Raw_supported_data can store raw, but it doesn't need to: it shall by
       * contract act like regular copy ctor.
       *
       * This would be different if the contract were basically to translate by delegating to ctor(src.get()).
       * Then .get()s would still be required to be equal, but data_2.is_raw() would be expected true. */
      EXPECT_FALSE(data_2.is_raw());
      EXPECT_EQ(data_2.get(), data.get()); // Either way they point to the same place!
      ss << data_2;
    }

    /* Increment back into pool, showing that it's possible to travel around invalid-in-a-sense boundary conditions
     * and get back onto solid ground. (As noted elsewhere, such things enable sloppy comparisons around the start
     * or end of a pool to work, as they would with native or raw pointers.) */
    data.increment(S_TEST_OFFSET_2);
    EXPECT_TRUE(data.to_bool());
    EXPECT_EQ(data.get(), S_TEST_POOL_ADDRESS);
    ss << data;

    // Now in the other direction -- go past *end* of pool and then back.
    {
      data = nullptr;
      EXPECT_FALSE(data.to_bool());
      data = static_cast<const uint8_t*>(S_TEST_POOL_ADDRESS) + S_TEST_POOL_SIZE - S_TEST_OFFSET_2;
      auto data_2 = data;
      EXPECT_TRUE(data.to_bool()); // Sanity check.
      EXPECT_LT(data.get(), static_cast<const uint8_t*>(S_TEST_POOL_ADDRESS) + S_TEST_POOL_SIZE); // Sanity check.
      // Go past pool.
      data.increment(S_TEST_OFFSET_1);
      EXPECT_GE(data.get(), static_cast<const uint8_t*>(S_TEST_POOL_ADDRESS) + S_TEST_POOL_SIZE); // Sanity check.
      EXPECT_NE(data.get(), data_2.get()); // Sanity check.
      EXPECT_TRUE(data.to_bool()); // Still a pointer even though out of pool to which it internally refers.
      data.increment(-S_TEST_OFFSET_1); // And go back.
      EXPECT_TRUE(data.to_bool());
      EXPECT_EQ(data.get(), data_2.get());
      // Travel around the boundary a bit.
      data.increment(S_TEST_OFFSET_2 - 1); // Point to last byte of pool.
      EXPECT_EQ(data.get(), static_cast<const uint8_t*>(TEST_POOL->get_address()) + TEST_POOL->get_size() - 1);
      EXPECT_TRUE(data.to_bool());
      EXPECT_TRUE(TEST_POOL->is_subset(data.get(), 1));
      data.increment(1);
      EXPECT_EQ(data.get(), static_cast<const uint8_t*>(TEST_POOL->get_address()) + TEST_POOL->get_size());
      EXPECT_TRUE(data.to_bool());
      EXPECT_FALSE(TEST_POOL->is_subset(data.get(), 1));
      ss << data;
      data = nullptr;
      EXPECT_FALSE(data.to_bool());
    }

    // Deregister pool
    Test_repository::get_instance().erase(TEST_POOL->get_id());
  }
}

/* Verify all 6 comparison relationships between `a` and `b`.  Exactly one of the expect_* flags must be true.
 * Also checks negation consistency (!=, >=, <=) and symmetry/antisymmetry (b vs a). */
template <typename Data>
static void check_cmp(Data a, Data b, bool expect_eq, bool expect_lt, bool expect_gt)
{
  ASSERT_EQ(int(expect_eq) + int(expect_lt) + int(expect_gt), 1) << "Exactly one relationship expected.";

  // Direct checks.
  EXPECT_EQ(a.equals(b), expect_eq)         << "a.equals(b)";
  EXPECT_EQ(a.less_than(b), expect_lt)      << "a.less_than(b)";
  EXPECT_EQ(a.greater_than(b), expect_gt)   << "a.greater_than(b)";

  // Symmetry/antisymmetry: == is symmetric; a<b iff b>a; a>b iff b<a.
  EXPECT_EQ(b.equals(a), expect_eq)         << "b.equals(a) (symmetry)";
  EXPECT_EQ(b.less_than(a), expect_gt)      << "b.less_than(a) (should == a.greater_than(b))";
  EXPECT_EQ(b.greater_than(a), expect_lt)   << "b.greater_than(a) (should == a.less_than(b))";
}

/* Exhaustive comparison tests for equals(), less_than(), greater_than() — for the specified Data variation.
 * Both Raw_supported_data and Offset_only_data are exercised (different code paths in each). */
template <typename Data>
static void comparison_tests();

TEST(Shm_pool_offset_ptr_data_test, Comparison_raw_supported)
{
  comparison_tests<Raw_supported_data>();
}

TEST(Shm_pool_offset_ptr_data_test, Comparison_offset_only)
{
  comparison_tests<Offset_only_data>();
}

template <typename Data>
void comparison_tests() // Static.
{
  const auto pool_1 =
    make_shared<Shm_pool>(S_TEST_POOL_ID, S_TEST_POOL_NAME, S_TEST_POOL_ADDRESS, S_TEST_POOL_SIZE, S_TEST_POOL_FD);
  const auto pool_2 =
    make_shared<Shm_pool>(S_TEST_POOL_ID_2, S_TEST_POOL_NAME_2,
                           S_TEST_POOL_ADDRESS_2, S_TEST_POOL_SIZE_2, S_TEST_POOL_FD_2);

  Test_repository::get_instance().insert(shared_ptr<Shm_pool>{pool_1});
  Test_repository::get_instance().insert(shared_ptr<Shm_pool>{pool_2});

  // -- Null vs null. --
  {
    Data a;
    Data b;
    FLOW_TEST_TRACE(); check_cmp(a, b, true, false, false); // null == null.
  }

  // -- Null vs non-null offset pointer. --
  {
    Data null;
    Data offset(static_cast<uint8_t*>(S_TEST_POOL_ADDRESS) + S_TEST_OFFSET_1);
    FLOW_TEST_TRACE(); check_cmp(null, offset, false, true, false); // null < non-null.
  }

  // -- Same pool, same offset (bitwise equal — the fast path in equals()). --
  {
    auto* addr = static_cast<uint8_t*>(S_TEST_POOL_ADDRESS) + S_TEST_OFFSET_1;
    Data a(addr);
    Data b(addr);
    FLOW_TEST_TRACE(); check_cmp(a, b, true, false, false);
  }

  // -- Same pool, different offset (the main perf-win path in less_than/greater_than). --
  {
    Data lo(static_cast<uint8_t*>(S_TEST_POOL_ADDRESS) + S_TEST_OFFSET_2); // +0x10.
    Data hi(static_cast<uint8_t*>(S_TEST_POOL_ADDRESS) + S_TEST_OFFSET_1); // +0x100.
    FLOW_TEST_TRACE(); check_cmp(lo, hi, false, true, false);
  }

  // -- Different pools: pool 1 (0x10000) < pool 2 (0x30000).  Falls through to to_address() in < and >. --
  {
    Data in_p1(static_cast<uint8_t*>(S_TEST_POOL_ADDRESS) + S_TEST_OFFSET_1);
    Data in_p2(static_cast<uint8_t*>(S_TEST_POOL_ADDRESS_2) + S_TEST_OFFSET_1);
    FLOW_TEST_TRACE(); check_cmp(in_p1, in_p2, false, true, false);
  }

  // -- Different pools, reversed. --
  {
    Data in_p1(static_cast<uint8_t*>(S_TEST_POOL_ADDRESS) + S_TEST_OFFSET_1);
    Data in_p2(static_cast<uint8_t*>(S_TEST_POOL_ADDRESS_2) + S_TEST_OFFSET_1);
    FLOW_TEST_TRACE(); check_cmp(in_p2, in_p1, false, false, true);
  }

  // -- Different pools, same offset — tests that equals() correctly returns false (NO_RAW path or CAN_RAW path). --
  {
    Data in_p1(static_cast<uint8_t*>(S_TEST_POOL_ADDRESS) + S_TEST_OFFSET_2);
    Data in_p2(static_cast<uint8_t*>(S_TEST_POOL_ADDRESS_2) + S_TEST_OFFSET_2);
    FLOW_TEST_TRACE(); check_cmp(in_p1, in_p2, false, true, false); // Same offsets, different pools => NE.
  }

  /* Documented pathological case in equals(): two offset pointers in different pools, one or both with out-of-bounds
   * offset, such that get() yields the same vaddr — yet equals() returns false (because pool IDs differ and we
   * skip the expensive to_address() lookup).  Trichotomy breaks here: none of ==, <, > is true.  This is the
   * accepted trade-off for avoiding map lookups in the common case.  Can't use check_cmp() for this one. */
  {
    FLOW_TEST_TRACE();
    // Pointer in pool 1 at base (offset 0).
    Data in_p1(S_TEST_POOL_ADDRESS);
    // Increment way past pool 1, landing on pool 2 + 0x100: get() = 0x10000 + 0x20100 = 0x30100.
    const auto distance = static_cast<const uint8_t*>(S_TEST_POOL_ADDRESS_2)
                          - static_cast<const uint8_t*>(S_TEST_POOL_ADDRESS)
                          + S_TEST_OFFSET_1;
    in_p1.increment(Shm_pool_offset_ptr_data_base::diff_t(distance));
    // Pointer in pool 2 at offset 0x100: get() = 0x30000 + 0x100 = 0x30100.
    Data in_p2(static_cast<uint8_t*>(S_TEST_POOL_ADDRESS_2) + S_TEST_OFFSET_1);
    // Same vaddr...
    EXPECT_EQ(in_p1.get(), in_p2.get());
    // ...but equals() says no (the documented pathological exception).
    EXPECT_FALSE(in_p1.equals(in_p2));
    // And trichotomy is broken: neither < nor > either, since both to_address() calls yield the same result.
    EXPECT_FALSE(in_p1.less_than(in_p2));
    EXPECT_FALSE(in_p1.greater_than(in_p2));
  }

  if constexpr(Data::S_CAN_STORE_RAW_PTR)
  {
    // Raw-pointer-specific tests.  Use addresses outside both pools for raw pointers.
    const auto RAW_LO = reinterpret_cast<void*>(0x5000);  // Below pool 1 (0x10000).
    const auto RAW_HI = reinterpret_cast<void*>(0x50000); // Above pool 2 (0x30000).

    // -- Both raw, same address (bitwise equal). --
    {
      Data a(RAW_LO);
      Data b(RAW_LO);
      ASSERT_TRUE(a.is_raw());
      FLOW_TEST_TRACE(); check_cmp(a, b, true, false, false);
    }

    // -- Both raw, different addresses. --
    {
      Data lo(RAW_LO);
      Data hi(RAW_HI);
      ASSERT_TRUE(lo.is_raw());
      ASSERT_TRUE(hi.is_raw());
      FLOW_TEST_TRACE(); check_cmp(lo, hi, false, true, false);
    }

    // -- Null vs raw. --
    {
      Data null;
      Data raw(RAW_LO);
      ASSERT_TRUE(raw.is_raw());
      FLOW_TEST_TRACE(); check_cmp(null, raw, false, true, false); // null < raw.
    }

    /* Mixed raw vs offset — this is the code path where the original less_than()/greater_than() bug lived
     * (swapped operands when `!is_raw`).  Exercise both orderings. */

    // -- Raw < offset: raw at 0x5000, offset at pool 1 + 0x100 = 0x10100. --
    {
      Data raw(RAW_LO);
      Data offset(static_cast<uint8_t*>(S_TEST_POOL_ADDRESS) + S_TEST_OFFSET_1);
      ASSERT_TRUE(raw.is_raw());
      ASSERT_FALSE(offset.is_raw());
      FLOW_TEST_TRACE(); check_cmp(raw, offset, false, true, false); // 0x5000 < 0x10100.
    }

    // -- Raw > offset: raw at 0x50000, offset at pool 1 + 0x100 = 0x10100. --
    {
      Data raw(RAW_HI);
      Data offset(static_cast<uint8_t*>(S_TEST_POOL_ADDRESS) + S_TEST_OFFSET_1);
      ASSERT_TRUE(raw.is_raw());
      ASSERT_FALSE(offset.is_raw());
      FLOW_TEST_TRACE(); check_cmp(raw, offset, false, false, true); // 0x50000 > 0x10100.
    }

    // -- Raw vs offset, raw address between the two pools.  Exercises different orderings. --
    {
      const auto RAW_MID = reinterpret_cast<void*>(0x20000); // Between pool 1 and pool 2.
      Data raw(RAW_MID);
      Data in_p1(static_cast<uint8_t*>(S_TEST_POOL_ADDRESS) + S_TEST_OFFSET_1);
      Data in_p2(static_cast<uint8_t*>(S_TEST_POOL_ADDRESS_2) + S_TEST_OFFSET_1);
      ASSERT_TRUE(raw.is_raw());
      FLOW_TEST_TRACE(); check_cmp(raw, in_p1, false, false, true);  // 0x20000 > 0x10100.
      FLOW_TEST_TRACE(); check_cmp(raw, in_p2, false, true, false);  // 0x20000 < 0x30100.
    }
  } // if constexpr(S_CAN_STORE_RAW_PTR)

  Test_repository::get_instance().erase(pool_1->get_id());
  Test_repository::get_instance().erase(pool_2->get_id());
} // comparison_tests()

} // namespace ipc::shm::arena_lend::detail::test
