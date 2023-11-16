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

#include "ipc/shm/arena_lend/test/test_shm_pool_collection.hpp"
#include "ipc/shm/arena_lend/test/test_shm_object.hpp"
#include <ipc/shm/arena_lend/shm_pool.hpp>
#include <gtest/gtest.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

using std::string;
using std::size_t;
using std::make_shared;
using std::shared_ptr;

namespace ipc::shm::arena_lend::test
{

Test_shm_pool_collection::Test_shm_pool_collection(flow::log::Logger* logger, Collection_id id) :
  Shm_pool_collection(logger, id),
  m_name_generator(create_shm_object_name_generator())
{
}

shared_ptr<Shm_pool> Test_shm_pool_collection::create_shm_pool(size_t size)
{
  return create_shm_pool(m_name_generator(0 /* our name generator ignores it anyway */), size);
}

shared_ptr<Shm_pool> Test_shm_pool_collection::create_shm_pool(const string& name, size_t size)
{
  int fd = create_shm_object(name, size);
  if (fd == -1)
  {
    return nullptr;
  }

  void* address = map_shm(size, true, true, fd);
  if (address == nullptr)
  {
    remove_shm_object(name);
    return nullptr;
  }

  shared_ptr<Shm_pool> pool = make_shared<Shm_pool>(detail::Shm_pool_offset_ptr_data_base::generate_pool_id(),
                                                    name, address, size, fd);
  EXPECT_TRUE(register_shm_pool(pool));

  return pool;
}

shared_ptr<Shm_pool> Test_shm_pool_collection::open_shm_pool(const string& name, size_t size, bool write_enabled)
{
  // Open for write as we are testing shmMap command to ensure that it is providing the proper restriction
  int fd = shm_open(name.c_str(), O_RDWR, 0);
  if (fd == -1)
  {
    FLOW_LOG_WARNING("Could not open shared memory name '" << name << "': " << strerror(errno));
    return nullptr;
  }

  void* address = map_shm(size, true, write_enabled, fd);
  if (address == nullptr)
  {
    close(fd);
    return nullptr;
  }

  return make_shared<Shm_pool>(detail::Shm_pool_offset_ptr_data_base::generate_pool_id(), name, address, size, fd);
}

bool Test_shm_pool_collection::remove_shm_pool(const shared_ptr<Shm_pool>& shm_pool)
{
  return deregister_shm_pool(shm_pool) &&
         close_shm_pool(shm_pool) &&
         remove_shm_object(shm_pool->get_name());
}

int Test_shm_pool_collection::create_shm_object(const string& name, size_t size) const
{
  int fd = shm_open(name.c_str(), (O_RDWR | O_CREAT | O_EXCL), 0644);
  if (fd == -1)
  {
    FLOW_LOG_WARNING("Could not create SHM object '" << name << "': " << strerror(errno));
    return -1;
  }

  int result = ftruncate(fd, size);
  if (result == -1)
  {
    FLOW_LOG_WARNING("Could not set size for SHM object '" << name << "': " << strerror(errno));
    close(fd);
    remove_shm_object(name);
    return -1;
  }

  FLOW_LOG_TRACE("Created shm object with name '" << name << "', size " << size);

  return fd;
}

bool Test_shm_pool_collection::remove_shm_object(const string& name) const
{
  if (shm_unlink(name.c_str()) != 0)
  {
    FLOW_LOG_WARNING("Error occurred when removing shared memory name '" << name << "': " << strerror(errno));
    return false;
  }

  FLOW_LOG_TRACE("Removed shm object with name '" << name << "'");

  return true;
}

} // namespace ipc::shm::arena_lend::test
