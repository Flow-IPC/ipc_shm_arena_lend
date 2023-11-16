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

#pragma once

#include "ipc/shm/arena_lend/borrower_shm_pool_collection.hpp"
#include "ipc/shm/arena_lend/arena_lend_fwd.hpp"
#include "ipc/session/standalone/shm/arena_lend/arena_lend_fwd.hpp"

namespace ipc::session::shm::arena_lend
{

namespace test
{

/**
 * Creates an object used as the deleter for a shared pointer.
 *
 * @param expected_address The object address that should be deleted.
 * @param callback_executed Sets a value indicating that this deleter was executed.
 *
 * @return The deleter object.
 */
ipc::shm::arena_lend::Borrower_shm_pool_collection::Release_callback
create_object_deleter(void* expected_address, bool& callback_executed);
/**
 * Constructs and registers a borrower object within a session database. For all practical purposes, we are just
 * registering an offset from the base of the shared memory pool.
 *
 * @param session_data The session database to perform the registration in.
 * @param collection_id The identifier for the shared memory pool collection that the object resides in.
 * @param shm_pool The shared memory pool that the object resides in.
 * @param pool_offset The offset from the base of the shared memory pool that the object resides at.
 * @param expect_success Whether we expect the operation to succeed.
 *
 * @return Whether expectations matched reality. For example, if we expected the operation to fail and it failed,
 *         this would be a success (true).
 */
bool construct_and_register_borrower_object(Shm_session_data& session_data,
                                            Collection_id collection_id,
                                            const std::shared_ptr<ipc::shm::arena_lend::Shm_pool>& shm_pool,
                                            ipc::shm::arena_lend::Shm_pool::size_t pool_offset,
                                            bool expect_success = true);

} // namespace test
} // namespace ipc::session::shm::arena_lend
