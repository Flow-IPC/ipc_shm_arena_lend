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
#include <sys/mman.h>
#include "ipc/shm/arena_lend/borrower_allocator_arena.hpp"
#include "ipc/shm/arena_lend/borrower_shm_pool_collection.hpp"
#include "ipc/shm/arena_lend/borrower_shm_pool_listener.hpp"
#include "ipc/shm/arena_lend/borrower_shm_pool_repository.hpp"
#include "ipc/shm/arena_lend/jemalloc/ipc_arena.hpp"
#include "ipc/shm/arena_lend/owner_shm_pool_listener_for_repository.hpp"
#include "ipc/shm/stl/stateless_allocator.hpp"
#include "ipc/test/test_logger.hpp"
#include "ipc/shm/arena_lend/test/test_shm_object.hpp"
#include "ipc/shm/arena_lend/jemalloc/jemalloc_fwd.hpp"
#include "ipc/shm/arena_lend/jemalloc/test/test_util.hpp"
#include <boost/container/string.hpp>
#include <boost/container/vector.hpp>
#include <boost/container/list.hpp>
#include <flow/util/basic_blob.hpp>
#include <flow/log/log.hpp>
#include <list>

#include <signal.h>
#include <boost/stacktrace.hpp>
#include <iostream>

namespace chrono = std::chrono;

using ipc::test::Test_logger;
using std::make_shared;
using std::shared_ptr;
using std::string;
using std::vector;

namespace ipc::shm::arena_lend::test
{

using Ipc_arena = jemalloc::Ipc_arena;
template <typename T>
using Ipc_arena_allocator = jemalloc::Ipc_arena_allocator<T>;
using Ipc_arena_activator = jemalloc::Ipc_arena_activator;
using Memory_manager = jemalloc::Memory_manager;
using pool_offset_t = Shm_pool::size_t;

class Ipc_arena_wrapper :
  public Ipc_arena
{
public:
  /**
   * Offset pointer type used by #ipc::shm::stl::Stateless_allocator.
   *
   * @tparam Pointed_type The type contained within the pointer.
   */
  template <typename Pointed_type>
  using Pointer = Shm_pool_offset_ptr<Pointed_type,
                                      Owner_shm_pool_repository_singleton,
                                      int,
                                      false>;
}; // class Ipc_arena_wrapper

/// Google test fixture.
class Shm_pool_offset_ptr_test :
  public ::testing::Test
{
public:
  static constexpr size_t S_INITIAL_STRING_SIZE = 1;
  static constexpr size_t S_ADDITIONAL_STRING_SIZE = 25;
  static constexpr size_t S_TOTAL_STRING_SIZE = S_INITIAL_STRING_SIZE + S_ADDITIONAL_STRING_SIZE;
  static constexpr char S_STARTING_STRING_CHAR = 'a';

  static constexpr size_t S_INITIAL_LIST_SIZE = 5;
  static constexpr size_t S_ADDITIONAL_LIST_SIZE = 100;
  static constexpr size_t S_TOTAL_LIST_SIZE = S_INITIAL_LIST_SIZE + S_ADDITIONAL_LIST_SIZE;

  static constexpr size_t S_DEFAULT_VALUE = 100;
  static constexpr size_t S_BUFFER_SIZE = 100;

  /// Test structure.
  struct My_class
  {
    size_t m_value = S_DEFAULT_VALUE;
  }; // struct My_class

  /// Test subclass.
  struct My_subclass :
    public My_class
  {
  }; // struct My_subclass

  using Pointer = Ipc_arena::Pointer<My_class>;
  using Subpointer = Ipc_arena::Pointer<My_subclass>;
  using Offset_pointer = Ipc_arena_wrapper::Pointer<My_class>;

  // Construct arena and register a listener that puts the shared memory pools in a repository for offset pointer use
  Shm_pool_offset_ptr_test() :
    m_logger(flow::log::Sev::S_INFO),
    m_log_component(Log_component::S_TEST),
    m_owner_shm_pool_listener(&m_logger),
    m_memory_manager(make_shared<Memory_manager>(&m_logger)),
    m_arena(jemalloc::test::create_arena(&m_logger, m_memory_manager))
  {
    EXPECT_TRUE(m_arena->add_shm_pool_listener(&m_owner_shm_pool_listener));
  }

  flow::log::Logger* get_logger()
  {
    return &m_logger;
  }

  const flow::log::Component& get_log_component() const
  {
    return m_log_component;
  }

  shared_ptr<Ipc_arena>& get_arena()
  {
    return m_arena;
  }

private:
  Test_logger m_logger;
  flow::log::Component m_log_component;
  Owner_shm_pool_listener_for_repository m_owner_shm_pool_listener;
  shared_ptr<Memory_manager> m_memory_manager;
  shared_ptr<Ipc_arena> m_arena;
}; // class Shm_pool_offset_ptr_test

namespace
{

using Borrower_shm_pool_repository_singleton = Shm_pool_repository_singleton<Borrower_shm_pool_repository>;
/**
 * Alias for a Stateless_allocator using Borrower_allocator_arena.
 *
 * @tparam T The allocated object type.
 */
template<typename T>
using Borrower_arena_allocator =
  ipc::shm::stl::Stateless_allocator<T, Borrower_allocator_arena<Borrower_shm_pool_repository_singleton>>;

/**
 * Listener for shared memory pool events that occur on the borrower side that maintain a repository for
 * use with shared memory smart pointers. Using this type of listener comes with an expectation that the
 * shared memory pool names are unique and not opened by multiple sessions concurrently.
 *
 * @see Shm_pool_offset_ptr
 */
class Borrower_shm_pool_listener_for_repository :
  public flow::log::Log_context,
  public Borrower_shm_pool_listener
{
public:
  /**
   * Constructor.
   *
   * @param logger Used for logging purposes.
   */
  Borrower_shm_pool_listener_for_repository(flow::log::Logger* logger) :
    flow::log::Log_context(logger, Log_component::S_SHM)
  {
  }

  /**
   * Registers a shared memory pool in the repository.
   *
   * @param shm_pool The shared memory pool that was opened.
   */
  virtual void notify_opened_shm_pool(const shared_ptr<Shm_pool>& shm_pool) override
  {
    if (!Borrower_shm_pool_repository_singleton::get_instance().insert(shm_pool))
    {
      FLOW_LOG_WARNING("Could not insert SHM pool [" << shm_pool->get_name() << "]");
    }
  }

  /**
   * Deregisters a shared memory pool from the repository.
   *
   * @param shm_pool The shared memory pool that was opened.
   */
  virtual void notify_closed_shm_pool(Shm_pool::pool_id_t shm_pool_id) override
  {
    unsigned int use_count;
    auto shm_pool =
      Borrower_shm_pool_repository_singleton::get_instance().erase_or_decrement_use(shm_pool_id, use_count);
    EXPECT_NE(shm_pool, nullptr);
    EXPECT_EQ(use_count, 0UL);
  }
}; // class Borrower_shm_pool_repository_listener

/**
 * Checks that a container matches expected.
 *
 * @tparam Container_type A container type, which is different for borrower and owner.
 * @tparam Value_type The type of the values in the container.
 * @param container The container to check.
 * @param expected_size The expected size of the container.
 * @param starting_value The starting value in the container.
 *
 * @return Whether the container matched expected.
 */
template <typename Container_type, typename Value_type>
bool check_container_ref(Container_type& container, size_t expected_size, Value_type starting_value)
{
  auto size = container.size();
  if (size != expected_size)
  {
    ADD_FAILURE() << "Size [" << size << "] does not match expected [" << expected_size << "]";
    return false;
  }

  size_t index = 0;
  Value_type expected_value = starting_value;
  bool result = true;
  for (const auto& cur_value : container)
  {
    if (cur_value != expected_value)
    {
      ADD_FAILURE() << "Index [" << index << "] value [" << cur_value << "] != expected value [" <<
        expected_value << "]";
      result = false;
    }
    ++index;
    ++expected_value;
  }

  return result;
}

/**
 * Checks that a container matches expected.
 *
 * @tparam Container_type A container type, which is different for borrower and owner.
 * @tparam Value_type The type of the values in the container.
 * @param container The container to check.
 * @param expected_size The expected size of the container.
 * @param starting_value The starting value in the container.
 *
 * @return Whether the container matched expected.
 */
template <typename Container_type, typename Value_type>
bool check_container(const shared_ptr<Container_type>& container, size_t expected_size, Value_type starting_value)
{
  return check_container_ref(*container, expected_size, starting_value);
}

/**
 * Returns whether the shared memory pool is registered in the global shared memory pool repository.
 *
 * @tparam is_owner Whether the repository in question is the owner repository; otherwise, it is the borrower.
 * @param shm_pool The shared memory pool to check.
 *
 * @return See above.
 */
template <bool is_owner>
bool check_shm_pool_registration(const shared_ptr<Shm_pool>& shm_pool)
{
  using Repository_type = std::conditional_t<is_owner, Owner_shm_pool_repository, Borrower_shm_pool_repository>;

  return (Shm_pool_repository_singleton<Repository_type>::to_address(shm_pool->get_id(), 0) != nullptr);
}

} // Anonymous namespace

/// Class interface tests using a offset pointer.
TEST_F(Shm_pool_offset_ptr_test, Offset_pointer_interface)
{
  auto& arena = get_arena();

  void* buffer = arena->allocate(S_BUFFER_SIZE);

  Pointer offset_ptr = Pointer(buffer);
  EXPECT_NE(offset_ptr.get(), nullptr);
  EXPECT_TRUE(offset_ptr.is_offset());
  EXPECT_FALSE(offset_ptr.is_raw());

  Pointer next_ptr = offset_ptr + 1;
  EXPECT_EQ(reinterpret_cast<size_t>(next_ptr.get()) - reinterpret_cast<size_t>(offset_ptr.get()), sizeof(My_class));
  EXPECT_TRUE(next_ptr.is_offset());

  // Convert to pointer and back and make sure it is equivalent
  {
    My_class* pointer = offset_ptr.get();
    Pointer other_offset_ptr = Pointer::pointer_to(*pointer);
    EXPECT_TRUE(other_offset_ptr.is_offset());
    EXPECT_EQ(other_offset_ptr, offset_ptr);
  }
  EXPECT_EQ(static_cast<void*>(&offset_ptr->m_value), offset_ptr.get());
  EXPECT_EQ(&(*offset_ptr), offset_ptr);

  {
    Pointer p = next_ptr;
    p -= 1;
    EXPECT_EQ(p, offset_ptr);
  }
  {
    Pointer p1 = next_ptr;
    Pointer p2 = --p1;
    EXPECT_EQ(p1, offset_ptr);
    EXPECT_EQ(p2, offset_ptr);
  }
  {
    Pointer p1 = next_ptr;
    Pointer p2 = p1--;
    EXPECT_EQ(p1, offset_ptr);
    EXPECT_EQ(p2, next_ptr);
  }
  {
    Pointer p = next_ptr - 1;
    EXPECT_EQ(p, offset_ptr);
  }
  EXPECT_EQ(next_ptr - offset_ptr, 1);
  {
    Pointer p = offset_ptr;
    p += 1;
    EXPECT_EQ(p, next_ptr);
  }
  {
    Pointer p1 = offset_ptr;
    Pointer p2 = ++p1;
    EXPECT_EQ(p1, next_ptr);
    EXPECT_EQ(p2, next_ptr);
  }
  {
    Pointer p1 = offset_ptr;
    Pointer p2 = p1++;
    EXPECT_EQ(p1, next_ptr);
    EXPECT_EQ(p2, offset_ptr);
  }
  {
    Pointer p1 = &offset_ptr[0];
    Pointer p2 = &p1[1];
    EXPECT_EQ(p1, offset_ptr);
    EXPECT_EQ(p2, next_ptr);
  }

  EXPECT_EQ(offset_ptr + 1, next_ptr);
  EXPECT_EQ(1 + offset_ptr, next_ptr);
  EXPECT_TRUE(offset_ptr);
  EXPECT_FALSE(!offset_ptr);
  EXPECT_TRUE(offset_ptr == offset_ptr);
  EXPECT_TRUE(offset_ptr != next_ptr);
  EXPECT_TRUE(next_ptr != offset_ptr);
  EXPECT_TRUE(offset_ptr < next_ptr);
  EXPECT_TRUE(offset_ptr <= next_ptr);
  EXPECT_TRUE(offset_ptr <= offset_ptr);
  EXPECT_TRUE(next_ptr > offset_ptr);
  EXPECT_TRUE(next_ptr >= offset_ptr);
  EXPECT_TRUE(next_ptr >= next_ptr);

  // Clean up
  arena->deallocate(buffer);
}

/// Class interface tests using a raw pointer.
TEST_F(Shm_pool_offset_ptr_test, Raw_pointer_interface)
{
  uint8_t buffer[S_BUFFER_SIZE] = {};

  Pointer offset_ptr = Pointer(buffer);
  EXPECT_NE(offset_ptr.get(), nullptr);
  EXPECT_TRUE(offset_ptr.is_raw());
  EXPECT_FALSE(offset_ptr.is_offset());

  Pointer next_ptr = offset_ptr + 1;
  EXPECT_EQ(reinterpret_cast<size_t>(next_ptr.get()) - reinterpret_cast<size_t>(offset_ptr.get()), sizeof(My_class));
  EXPECT_TRUE(next_ptr.is_raw());

  // Convert to pointer and back and make sure it is equivalent
  {
    My_class* pointer = offset_ptr.get();
    Pointer other_offset_ptr = Pointer::pointer_to(*pointer);
    EXPECT_TRUE(other_offset_ptr.is_raw());
    EXPECT_EQ(other_offset_ptr, offset_ptr);
  }
  EXPECT_EQ(static_cast<void*>(&offset_ptr->m_value), offset_ptr.get());
  EXPECT_EQ(&(*offset_ptr), offset_ptr);

  {
    Pointer p = next_ptr;
    p -= 1;
    EXPECT_EQ(p, offset_ptr);
  }
  {
    Pointer p1 = next_ptr;
    Pointer p2 = --p1;
    EXPECT_EQ(p1, offset_ptr);
    EXPECT_EQ(p2, offset_ptr);
  }
  {
    Pointer p1 = next_ptr;
    Pointer p2 = p1--;
    EXPECT_EQ(p1, offset_ptr);
    EXPECT_EQ(p2, next_ptr);
  }
  {
    Pointer p = next_ptr - 1;
    EXPECT_EQ(p, offset_ptr);
  }
  EXPECT_EQ((next_ptr - offset_ptr), 1);
  {
    Pointer p = offset_ptr;
    p += 1;
    EXPECT_EQ(p, next_ptr);
  }
  {
    Pointer p1 = offset_ptr;
    Pointer p2 = ++p1;
    EXPECT_EQ(p1, next_ptr);
    EXPECT_EQ(p2, next_ptr);
  }
  {
    Pointer p1 = offset_ptr;
    Pointer p2 = p1++;
    EXPECT_EQ(p1, next_ptr);
    EXPECT_EQ(p2, offset_ptr);
  }
  {
    Pointer p1 = &offset_ptr[0];
    Pointer p2 = &p1[1];
    EXPECT_EQ(p1, offset_ptr);
    EXPECT_EQ(p2, next_ptr);
  }

  EXPECT_EQ(offset_ptr + 1, next_ptr);
  EXPECT_EQ(1 + offset_ptr, next_ptr);
  EXPECT_TRUE(offset_ptr);
  EXPECT_FALSE(Pointer());
  EXPECT_TRUE(!Pointer());
  EXPECT_FALSE(Pointer().is_raw());
  EXPECT_FALSE(Pointer(nullptr));
  EXPECT_TRUE(!Pointer(nullptr));
  EXPECT_FALSE(Pointer(nullptr).is_raw());
  EXPECT_FALSE(!offset_ptr);
  EXPECT_TRUE(offset_ptr == offset_ptr);
  EXPECT_TRUE(offset_ptr != next_ptr);
  EXPECT_TRUE(next_ptr != offset_ptr);
  EXPECT_TRUE(offset_ptr < next_ptr);
  EXPECT_TRUE(offset_ptr <= next_ptr);
  EXPECT_TRUE(offset_ptr <= offset_ptr);
  EXPECT_TRUE(next_ptr > offset_ptr);
  EXPECT_TRUE(next_ptr >= offset_ptr);
  EXPECT_TRUE(next_ptr >= next_ptr);
}

/// Tests the usage of the offset pointer only interface.
TEST_F(Shm_pool_offset_ptr_test, Offset_pointer_only_interface)
{
  auto& arena = get_arena();

  void* buffer = arena->allocate(S_BUFFER_SIZE);

  Offset_pointer offset_ptr = Offset_pointer(buffer);
  EXPECT_NE(offset_ptr.get(), nullptr);
  EXPECT_TRUE(offset_ptr.is_offset());

  Offset_pointer next_ptr = offset_ptr + 1;
  EXPECT_EQ(reinterpret_cast<size_t>(next_ptr.get()) - reinterpret_cast<size_t>(offset_ptr.get()), sizeof(My_class));
  EXPECT_TRUE(next_ptr.is_offset());

  // Convert to pointer and back and make sure it is equivalent
  {
    My_class* pointer = offset_ptr.get();
    Offset_pointer other_offset_ptr = Offset_pointer::pointer_to(*pointer);
    EXPECT_TRUE(other_offset_ptr.is_offset());
    EXPECT_EQ(other_offset_ptr, offset_ptr);
  }
  EXPECT_EQ(static_cast<void*>(&offset_ptr->m_value), offset_ptr.get());
  EXPECT_EQ(&(*offset_ptr), offset_ptr);

  {
    Offset_pointer p = next_ptr;
    p -= 1;
    EXPECT_EQ(p, offset_ptr);
  }
  {
    Offset_pointer p1 = next_ptr;
    Offset_pointer p2 = --p1;
    EXPECT_EQ(p1, offset_ptr);
    EXPECT_EQ(p2, offset_ptr);
  }
  {
    Offset_pointer p1 = next_ptr;
    Offset_pointer p2 = p1--;
    EXPECT_EQ(p1, offset_ptr);
    EXPECT_EQ(p2, next_ptr);
  }
  {
    Offset_pointer p = next_ptr - 1;
    EXPECT_EQ(p, offset_ptr);
  }
  EXPECT_EQ(next_ptr - offset_ptr, 1);
  {
    Offset_pointer p = offset_ptr;
    p += 1;
    EXPECT_EQ(p, next_ptr);
  }
  {
    Offset_pointer p1 = offset_ptr;
    Offset_pointer p2 = ++p1;
    EXPECT_EQ(p1, next_ptr);
    EXPECT_EQ(p2, next_ptr);
  }
  {
    Offset_pointer p1 = offset_ptr;
    Offset_pointer p2 = p1++;
    EXPECT_EQ(p1, next_ptr);
    EXPECT_EQ(p2, offset_ptr);
  }
  {
    Offset_pointer p1 = &offset_ptr[0];
    Offset_pointer p2 = &p1[1];
    EXPECT_EQ(p1, offset_ptr);
    EXPECT_EQ(p2, next_ptr);
  }

  EXPECT_EQ((offset_ptr + 1), next_ptr);
  EXPECT_EQ((1 + offset_ptr), next_ptr);
  EXPECT_TRUE(offset_ptr);
  EXPECT_FALSE(Offset_pointer());
  EXPECT_TRUE(!Offset_pointer());
  EXPECT_FALSE(Offset_pointer().is_offset());
  EXPECT_FALSE(Offset_pointer(nullptr));
  EXPECT_TRUE(!Offset_pointer(nullptr));
  EXPECT_FALSE(Offset_pointer(nullptr).is_offset());
  EXPECT_FALSE(!offset_ptr);
  EXPECT_TRUE(offset_ptr == offset_ptr);
  EXPECT_TRUE(offset_ptr != next_ptr);
  EXPECT_TRUE(next_ptr != offset_ptr);
  EXPECT_TRUE(offset_ptr < next_ptr);
  EXPECT_TRUE(offset_ptr <= next_ptr);
  EXPECT_TRUE(offset_ptr <= offset_ptr);
  EXPECT_TRUE(next_ptr > offset_ptr);
  EXPECT_TRUE(next_ptr >= offset_ptr);
  EXPECT_TRUE(next_ptr >= next_ptr);
}

/// Tests copy interface.
TEST_F(Shm_pool_offset_ptr_test, Copy_interface)
{
  using Const_pointer = Ipc_arena::Pointer<const My_class>;
  using Volatile_pointer = Ipc_arena::Pointer<volatile My_class>;
  using Const_volatile_pointer = Ipc_arena::Pointer<const volatile My_class>;

  auto& arena = get_arena();
  void* buffer = arena->allocate(sizeof(My_subclass));
  Pointer object(buffer);
  EXPECT_NE(object, nullptr);
  EXPECT_TRUE(object.is_offset());
  Subpointer subclass_object(buffer);
  EXPECT_NE(subclass_object, nullptr);
  EXPECT_TRUE(subclass_object.is_offset());

  uint8_t raw_buffer[S_BUFFER_SIZE] = {};
  Pointer raw_object(raw_buffer);

  // Copy to another similar object
  {
    Pointer other_object(object);
    EXPECT_EQ(object, other_object);
    other_object = object;
    EXPECT_EQ(object, other_object);
  }
  // Copy to offset pointer only
  {
    Offset_pointer offset_object(object);
    EXPECT_EQ(object.get(), offset_object.get());
    EXPECT_TRUE(offset_object.is_offset());
    offset_object = object;
    EXPECT_EQ(object.get(), offset_object.get());
    EXPECT_TRUE(offset_object.is_offset());
    // Copy back to another object
    Pointer other_object(offset_object);
    EXPECT_EQ(other_object.get(), offset_object.get());
    EXPECT_TRUE(other_object.is_offset());
    other_object = offset_object;
    EXPECT_EQ(other_object.get(), offset_object.get());
    EXPECT_TRUE(other_object.is_offset());
  }
  // Copy to const
  {
    Const_pointer const_object(object);
    EXPECT_EQ(object, const_object);
    const_object = object;
    EXPECT_EQ(object, const_object);
    // Copy from const to const
    Const_pointer other_const_object(const_object);
    EXPECT_EQ(const_object, other_const_object);
    other_const_object = const_object;
    EXPECT_EQ(const_object, other_const_object);
    // Copy to const volatile
    Const_volatile_pointer const_volatile_object(const_object);
    EXPECT_EQ(const_object, const_volatile_object);
    const_volatile_object = const_object;
    EXPECT_EQ(const_object, const_volatile_object);
    // Copy from const volatile to const volatile
    Const_volatile_pointer other_const_volatile_object(const_volatile_object);
    EXPECT_EQ(const_volatile_object, other_const_volatile_object);
    other_const_volatile_object = const_volatile_object;
    EXPECT_EQ(const_volatile_object, other_const_volatile_object);
  }
  // Copy to volatile
  {
    Volatile_pointer volatile_object(object);
    EXPECT_EQ(object, volatile_object);
    volatile_object = object;
    EXPECT_EQ(object, volatile_object);
    // Copy to const volatile
    Const_volatile_pointer const_volatile_object(volatile_object);
    EXPECT_EQ(volatile_object, const_volatile_object);
    const_volatile_object = volatile_object;
    EXPECT_EQ(volatile_object, const_volatile_object);
  }
  // Copy from subclass to superclass
  {
    Pointer other_object(subclass_object);
    EXPECT_EQ(subclass_object.get(), other_object.get());
    other_object = subclass_object;
    EXPECT_EQ(subclass_object.get(), other_object.get());
  }
  // Copy from offset pointer to raw pointer
  {
    Pointer other_raw_object;
    EXPECT_FALSE(other_raw_object.is_raw());
    other_raw_object = object;
    EXPECT_TRUE(other_raw_object.is_offset());
    EXPECT_EQ(object, other_raw_object);
  }
  // Copy from raw pointer to offset pointer
  {
    Pointer other_offset_object(object);
    other_offset_object = raw_object;
    EXPECT_TRUE(other_offset_object.is_raw());
    EXPECT_EQ(raw_object, other_offset_object);
  }

  // Clean up
  arena->deallocate(buffer);
}

/**
 * Tests the following:
 * 1. A vector can be constructed and modified from the owner side.
 * 2. The vector can be constructed and read from the borrower side.
 * 3. The borrower shared memory pools are deregistered from the borrower repository when the listener triggers
 *    the removal (manually triggered in this scenario).
 * 4. The owner shared memory pools are deregistered from the owner repository when the arena is destructed.
 */
TEST_F(Shm_pool_offset_ptr_test, Vector)
{
  using Shm_vector = vector<size_t, Ipc_arena_allocator<size_t>>;

  auto& arena = get_arena();

  // Create the vector
  auto owner_vec = arena->construct<Shm_vector>(S_INITIAL_LIST_SIZE);
  if (owner_vec == nullptr)
  {
    ADD_FAILURE() << "Could not create vector";
    return;
  }

  // Fill in initial values
  for (size_t i = 0; i < S_INITIAL_LIST_SIZE; ++i)
  {
    (*owner_vec)[i] = i;
  }

  {
    Ipc_arena_activator ctx(arena.get());
    // Add more values
    for (size_t i = S_INITIAL_LIST_SIZE; i < S_TOTAL_LIST_SIZE; ++i)
    {
      // This needs to be performed within an activator context as it may allocate memory
      owner_vec->push_back(i);
    }
  }
  // Intentionally check vector outside of activator
  EXPECT_TRUE(check_container(owner_vec, S_TOTAL_LIST_SIZE, 0UL));

  auto owner_shm_pool = arena->lookup_shm_pool(owner_vec.get());
  if (owner_shm_pool == nullptr)
  {
    ADD_FAILURE() << "Did not locate shared memory pool for object";
    return;
  }

  // "Borrow" the shared memory pool
  using Borrower_shm_vector = vector<size_t, Borrower_arena_allocator<size_t>>;
  Borrower_shm_pool_collection borrower_collection(get_logger(), arena->get_id());
  auto borrower_shm_pool = borrower_collection.open_shm_pool(owner_shm_pool->get_id(), owner_shm_pool->get_name(),
                                                             owner_shm_pool->get_size());

  // Register the shared memory pool in the SHM pool singleton repository necessary for the offset pointer
  Borrower_shm_pool_listener_for_repository borrower_shm_pool_listener(get_logger());
  borrower_shm_pool_listener.notify_opened_shm_pool(borrower_shm_pool);

  // "Borrow" the object and make sure it is as expected
  pool_offset_t offset;
  EXPECT_TRUE(owner_shm_pool->determine_offset(owner_vec.get(), offset));

  bool object_released = false;
  auto borrower_vec = borrower_collection.construct<Borrower_shm_vector>(borrower_shm_pool->get_address(),
                                                                         offset,
                                                                         [&](void*)
                                                                         {
                                                                           object_released = true;
                                                                         });
  EXPECT_TRUE(check_container(borrower_vec, owner_vec->size(), 0UL));

  // Release the object
  borrower_vec.reset();
  EXPECT_TRUE(object_released);

  // Check that the borrower shared memory pool is registered in the borrower repository
  EXPECT_TRUE(check_shm_pool_registration<false>(borrower_shm_pool));

  // Deregister the shared memory pool
  EXPECT_TRUE(borrower_collection.release_shm_pool(borrower_shm_pool));
  borrower_shm_pool_listener.notify_closed_shm_pool(borrower_shm_pool->get_id());

  // Check that the borrower shared memory pool is no longer registered in the borrower repository
  EXPECT_FALSE(check_shm_pool_registration<false>(borrower_shm_pool));

  // Delete outside of an activator
  owner_vec.reset();

  // Check that the owner shared memory pool is registered in the owner repository
  EXPECT_TRUE(check_shm_pool_registration<true>(owner_shm_pool));

  // Release handle to arena to trigger removal of shared memory pools from the owner repository
  EXPECT_EQ(arena.use_count(), 1);
  arena.reset();

  // Check that the owner shared memory pool is no longer registered in the owner repository
  EXPECT_FALSE(check_shm_pool_registration<true>(owner_shm_pool));
}

/**
 * Tests the usage of boost container, which exercises a wider amount of the API.
 */
TEST_F(Shm_pool_offset_ptr_test, Boost_string)
{
  using Shm_string =
    boost::container::basic_string<char, std::char_traits<char>, Ipc_arena_allocator<char>>;

  auto& arena = get_arena();

  // Create the string
  auto owner_string = arena->construct<Shm_string>(S_INITIAL_STRING_SIZE, S_STARTING_STRING_CHAR);
  if (owner_string == nullptr)
  {
    ADD_FAILURE() << "Could not create string";
    return;
  }

  // Add more characters
  {
    Ipc_arena_activator ctx(arena.get());
    // Add more values
    for (size_t i = S_INITIAL_STRING_SIZE; i < S_TOTAL_STRING_SIZE; ++i)
    {
      // This needs to be performed within an activator context as it may allocate memory
      *owner_string += (S_STARTING_STRING_CHAR + i);
    }
  }
  // Intentionally check string outside of activator
  EXPECT_TRUE(check_container(owner_string, S_TOTAL_STRING_SIZE, 'a'));

  auto owner_shm_pool = arena->lookup_shm_pool(owner_string.get());
  if (owner_shm_pool == nullptr)
  {
    ADD_FAILURE() << "Did not locate shared memory pool for object";
    return;
  }

  // "Borrow" the shared memory pool
  using Borrower_shm_string =
    boost::container::basic_string<char, std::char_traits<char>, Borrower_arena_allocator<char>>;
  Borrower_shm_pool_collection borrower_collection(get_logger(), arena->get_id());
  auto borrower_shm_pool = borrower_collection.open_shm_pool(owner_shm_pool->get_id(), owner_shm_pool->get_name(),
                                                             owner_shm_pool->get_size());

  // Register the shared memory pool in the SHM pool singleton repository necessary for the offset pointer
  Borrower_shm_pool_listener_for_repository borrower_shm_pool_listener(get_logger());
  borrower_shm_pool_listener.notify_opened_shm_pool(borrower_shm_pool);

  // "Borrow" the object and make sure it is as expected
  pool_offset_t offset;
  EXPECT_TRUE(owner_shm_pool->determine_offset(owner_string.get(), offset));

  bool object_released = false;
  auto borrower_string = borrower_collection.construct<Borrower_shm_string>(borrower_shm_pool->get_address(),
                                                                            offset,
                                                                            [&](void*)
                                                                            {
                                                                              object_released = true;
                                                                            });
  EXPECT_TRUE(check_container(borrower_string, S_TOTAL_STRING_SIZE, 'a'));

  // Release the object
  borrower_string.reset();
  EXPECT_TRUE(object_released);

  // Check that the borrower shared memory pool is registered in the borrower repository
  EXPECT_TRUE(check_shm_pool_registration<false>(borrower_shm_pool));

  // Deregister the shared memory pool
  EXPECT_TRUE(borrower_collection.release_shm_pool(borrower_shm_pool));
  borrower_shm_pool_listener.notify_closed_shm_pool(borrower_shm_pool->get_id());

  // Check that the borrower shared memory pool is no longer registered in the borrower repository
  EXPECT_FALSE(check_shm_pool_registration<false>(borrower_shm_pool));

  // Release object outside of an activator
  owner_string.reset();

  // Check that the owner shared memory pool is registered in the owner repository
  EXPECT_TRUE(check_shm_pool_registration<true>(owner_shm_pool));

  // Release handle to arena to trigger removal of shared memory pools from the owner repository
  EXPECT_EQ(arena.use_count(), 1);
  arena.reset();

  // Check that the owner shared memory pool is no longer registered in the owner repository
  EXPECT_FALSE(check_shm_pool_registration<true>(owner_shm_pool));
}

/**
 * Tests the usage of boost container on the stack with contents placed in shared memory. The test performs the
 * following:
 * 1. Allocate a string in shared memory.
 * 2. Use a string on the stack with a shared memory allocator.
 * 3. Insert some contents into the string on the stack.
 * 4. Copy the string on the stack to the string in shared memory.
 * 5. Destroy the string on the stack (implicitly).
 * 6. Insert more contents into the string in shared memory.
 * 7. Share string in shared memory with a borrower.
 * 8. Ensure borrower string matches owner string.
 */
TEST_F(Shm_pool_offset_ptr_test, Stack_string)
{
  using Shm_string =
    boost::container::basic_string<char, std::char_traits<char>, Ipc_arena_allocator<char>>;

  auto& arena = get_arena();

  // Create the string
  auto owner_string = arena->construct<Shm_string>();
  if (owner_string == nullptr)
  {
    ADD_FAILURE() << "Could not create string";
    return;
  }

  // Partially build the string on the stack
  {
    Ipc_arena_activator ctx(arena.get());
    // Create the stack string
    Shm_string stack_string(S_INITIAL_STRING_SIZE, S_STARTING_STRING_CHAR);

    // Add more values
    for (size_t i = S_INITIAL_STRING_SIZE; i < (S_TOTAL_STRING_SIZE / 2); ++i)
    {
      // This needs to be performed within an activator context as it may allocate memory
      stack_string += (S_STARTING_STRING_CHAR + i);
    }
    EXPECT_TRUE(check_container_ref(stack_string, (S_TOTAL_STRING_SIZE / 2), 'a'));

    // Copy the stack string contents
    *owner_string = stack_string;
  }

  // Build the rest using shared memory
  {
    Ipc_arena_activator ctx(arena.get());

    for (size_t i = (S_TOTAL_STRING_SIZE / 2); i < S_TOTAL_STRING_SIZE; ++i)
    {
      // This needs to be performed within an activator context as it may allocate memory
      *owner_string += (S_STARTING_STRING_CHAR + i);
    }
  }
  EXPECT_TRUE(check_container(owner_string, S_TOTAL_STRING_SIZE, 'a'));

  auto owner_shm_pool = arena->lookup_shm_pool(owner_string.get());
  if (owner_shm_pool == nullptr)
  {
    ADD_FAILURE() << "Did not locate shared memory pool for object";
    return;
  }

  // "Borrow" the shared memory pool
  using Borrower_shm_string =
    boost::container::basic_string<char, std::char_traits<char>, Borrower_arena_allocator<char>>;
  Borrower_shm_pool_collection borrower_collection(get_logger(), arena->get_id());
  auto borrower_shm_pool = borrower_collection.open_shm_pool(owner_shm_pool->get_id(), owner_shm_pool->get_name(),
                                                             owner_shm_pool->get_size());

  // Register the shared memory pool in the SHM pool singleton repository necessary for the offset pointer
  Borrower_shm_pool_listener_for_repository borrower_shm_pool_listener(get_logger());
  borrower_shm_pool_listener.notify_opened_shm_pool(borrower_shm_pool);

  // "Borrow" the object and make sure it is as expected
  pool_offset_t offset;
  EXPECT_TRUE(owner_shm_pool->determine_offset(owner_string.get(), offset));

  bool object_released = false;
  auto borrower_string = borrower_collection.construct<Borrower_shm_string>(borrower_shm_pool->get_address(),
                                                                            offset,
                                                                            [&](void*)
                                                                            {
                                                                              object_released = true;
                                                                            });
  EXPECT_TRUE(check_container(borrower_string, S_TOTAL_STRING_SIZE, 'a'));

  // Release the object
  borrower_string.reset();
  EXPECT_TRUE(object_released);

  // Check that the borrower shared memory pool is registered in the borrower repository
  EXPECT_TRUE(check_shm_pool_registration<false>(borrower_shm_pool));

  // Deregister the shared memory pool
  EXPECT_TRUE(borrower_collection.release_shm_pool(borrower_shm_pool));
  borrower_shm_pool_listener.notify_closed_shm_pool(borrower_shm_pool->get_id());

  // Check that the borrower shared memory pool is no longer registered in the borrower repository
  EXPECT_FALSE(check_shm_pool_registration<false>(borrower_shm_pool));

  // Release object outside of an activator
  owner_string.reset();

  // Check that the owner shared memory pool is registered in the owner repository
  EXPECT_TRUE(check_shm_pool_registration<true>(owner_shm_pool));

  // Release handle to arena to trigger removal of shared memory pools from the owner repository
  EXPECT_EQ(arena.use_count(), 1);
  arena.reset();

  // Check that the owner shared memory pool is no longer registered in the owner repository
  EXPECT_FALSE(check_shm_pool_registration<true>(owner_shm_pool));
}

/// Used to measure clock time for container operations, which is currently appending. See ECOGS-527.
TEST_F(Shm_pool_offset_ptr_test, Container_list_insertion)
{
  static constexpr size_t S_NUM_OBJECTS = 100000;
  auto& arena = get_arena();
  Ipc_arena_activator ctx(arena.get());
  {
    FLOW_LOG_INFO("Starting Boost integer list, size [" << S_NUM_OBJECTS << "]");
    using Object = int;
    using Shm_list = boost::container::list<Object, Ipc_arena_allocator<Object>>;

    Object stack_object = 10;
    auto shm_list = arena->construct<Shm_list>();
    for (size_t i = 0; i < S_NUM_OBJECTS; ++i)
    {
      shm_list->emplace_back(stack_object);
    }
    FLOW_LOG_INFO("Finished Boost integer list");
  }
  {
    FLOW_LOG_INFO("Starting integer vector, size [" << S_NUM_OBJECTS << "]");
    using Object = int;
    using Shm_list = std::vector<Object, Ipc_arena_allocator<Object>>;

    Object stack_object = 10;
    auto shm_list = arena->construct<Shm_list>();
    for (size_t i = 0; i < S_NUM_OBJECTS; ++i)
    {
      shm_list->emplace_back(stack_object);
    }
    FLOW_LOG_INFO("Finished integer vector");
  }
  {
    FLOW_LOG_INFO("Starting Boost Blob list, size [" << S_NUM_OBJECTS << "]");
    static constexpr size_t S_OBJECT_SIZE = 32;
    using Object = flow::util::Basic_blob<Ipc_arena_allocator<uint8_t>>;
    using Shm_list = boost::container::list<Object, Ipc_arena_allocator<Object>>;

    Object stack_object(S_OBJECT_SIZE);
    auto shm_list = arena->construct<Shm_list>();
    for (size_t i = 0; i < S_NUM_OBJECTS; ++i)
    {
      shm_list->emplace_back(stack_object);
    }
    FLOW_LOG_INFO("Finished Boost Blob list");
  }
}

} // namespace ipc::shm::arena_lend::test
