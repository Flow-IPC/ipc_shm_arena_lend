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

#include "ipc/session/standalone/shm/arena_lend/test/test_shm_session_util.hpp"
#include "ipc/session/standalone/shm/arena_lend/shm_session_data.hpp"
#include <gtest/gtest.h>

using std::shared_ptr;

using ipc::shm::arena_lend::Borrower_shm_pool_collection;
using ipc::shm::arena_lend::Shm_pool;

namespace ipc::session::shm::arena_lend::test
{

Borrower_shm_pool_collection::Release_callback create_object_deleter(void* expected_address,
                                                                     bool& callback_executed)
{
  return ([&callback_executed, expected_address](void* address)
          {
            EXPECT_EQ(address, expected_address);
            callback_executed = true;
          });
}

static bool construct_and_register_borrower_object_helper(shared_ptr<int>&& object,
                                                          Collection_id collection_id,
                                                          const shared_ptr<Shm_pool>& shm_pool,
                                                          Shm_pool::size_t pool_offset,
                                                          bool& callback_executed,
                                                          bool expect_success)
{
  if (!expect_success)
  {
    if (object != nullptr)
    {
      ADD_FAILURE() << "Object creation succeeded, when it is expected to fail for collection [" << collection_id <<
        "], SHM pool [" << *shm_pool << "], offset [" << pool_offset << "]";
      return false;
    }

    if (callback_executed)
    {
      ADD_FAILURE() << "Callback executed, when it shouldn't be for collection [" << collection_id <<
        "], SHM pool [" << *shm_pool << "], offset [" << pool_offset << "]";
      return false;
    }

    return true;
  }

  if (object == nullptr)
  {
    ADD_FAILURE() << "Object creation failed in collection [" << collection_id <<
        "], SHM pool [" << *shm_pool << "], offset [" << pool_offset << "]";
    return false;
  }

  object = nullptr;
  if (!callback_executed)
  {
    ADD_FAILURE() << "Deleter not executed for object created in collection [" << collection_id <<
      "], SHM pool [" << *shm_pool << "], offset [" << pool_offset << "]";
    return false;
  }

  return true;
}

bool construct_and_register_borrower_object(Shm_session_data& session_data,
                                            Collection_id collection_id,
                                            const shared_ptr<Shm_pool>& shm_pool,
                                            Shm_pool::size_t pool_offset,
                                            bool expect_success)
{
  bool callback_executed = false;
  void* expected_address = static_cast<uint8_t*>(shm_pool->get_address()) + pool_offset;
  auto object = session_data.construct_and_register_borrower_object<int>(
    collection_id, shm_pool->get_id(), pool_offset, create_object_deleter(expected_address, callback_executed));
  return construct_and_register_borrower_object_helper(std::move(object),
                                                       collection_id,
                                                       shm_pool,
                                                       pool_offset,
                                                       callback_executed,
                                                       expect_success);
}

} // namespace ipc::session::shm::arena_lend::test
