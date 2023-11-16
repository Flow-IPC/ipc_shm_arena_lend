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

#include "ipc/shm/arena_lend/test/test_shm_object.hpp"
#include "ipc/util/util_fwd.hpp"

namespace ipc::shm::arena_lend::jemalloc
{

class Ipc_arena;
class Memory_manager;

namespace test
{

/**
 * Creates a Ipc_arena instance.
 *
 * @param logger Used for logging purposes.
 * @param memory_manager The memory allocator.
 * @param name_generator Shared object name generator.
 * @param permissions The shared memory object file permissions when one is created.
 *
 * @return Upon success, a shared pointer to a Ipc_arena; otherwise, an empty shared pointer.
 */
std::shared_ptr<Ipc_arena> create_arena(
  flow::log::Logger* logger,
  const std::shared_ptr<Memory_manager>& memory_manager,
  Owner_shm_pool_collection::Shm_object_name_generator&& name_generator =
  ipc::shm::arena_lend::test::create_shm_object_name_generator(),
  const util::Permissions& permissions = util::shared_resource_permissions(util::Permissions_level::S_GROUP_ACCESS));

} // namespace test
} // namespace ipc::shm::arena_lend::jemalloc
