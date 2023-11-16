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

#include "ipc/shm/arena_lend/test/test_owner_shm_pool_collection.hpp"
#include "ipc/shm/arena_lend/memory_manager.hpp"
#include <sys/mman.h>

using std::shared_ptr;
using std::size_t;

namespace ipc::shm::arena_lend::test
{

Test_owner_shm_pool_collection::Test_owner_shm_pool_collection(flow::log::Logger* logger,
                                                               Collection_id id,
                                                               const shared_ptr<Memory_manager>& memory_manager,
                                                               Shm_object_name_generator&& name_generator,
                                                               util::Permissions_level permissions_level) :
  Owner_shm_pool_collection(logger,
                            id,
                            memory_manager,
                            std::move(name_generator),
                            util::shared_resource_permissions(permissions_level)),
  m_memory_map_functor([this](int fd, size_t size, [[maybe_unused]] void* new_address) -> void*
                       {
                         return map_shm(size, true, true, fd);
                       }),
  m_memory_unmap_functor([](const std::shared_ptr<Shm_pool>& shm_pool) -> bool
                         {
                           return (::munmap(shm_pool->get_address(), shm_pool->get_size()) == 0);
                         })
{
  FLOW_LOG_TRACE("Constructed with id [" << id << "]");
}

Test_owner_shm_pool_collection::~Test_owner_shm_pool_collection()
{
  FLOW_LOG_TRACE("Destructed with id [" << get_id() << "]");
}

void* Test_owner_shm_pool_collection::allocate(size_t size)
{
  return get_memory_manager()->allocate(size);
}

} // namespace ipc::shm::arena_lend::test
