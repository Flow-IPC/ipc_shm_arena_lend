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

#include "ipc/shm/arena_lend/owner_shm_pool_collection.hpp"
#include "ipc/shm/arena_lend/memory_manager.hpp"
#include "ipc/util/util_fwd.hpp"
#include "ipc/util/native_handle.hpp"
#include <sstream>
#include <sys/mman.h>

using std::size_t;
using std::shared_ptr;
using std::string;
using std::to_string;
using std::make_shared;
using std::stringstream;
using std::static_pointer_cast;
using flow::log::Logger;

namespace ipc::shm::arena_lend
{

Owner_shm_pool_collection::Owner_shm_pool_collection(Logger* logger,
                                                     Collection_id id,
                                                     const shared_ptr<Memory_manager>& memory_manager,
                                                     Shm_object_name_generator&& name_generator,
                                                     const util::Permissions& permissions) :
  Shm_pool_collection(logger, id),
  m_memory_manager(memory_manager),
  m_name_generator(std::move(name_generator)),
  m_permissions(permissions)
{
}

void Owner_shm_pool_collection::deallocate(void* object)
{
  m_memory_manager->deallocate(object);
}

bool Owner_shm_pool_collection::add_event_listener(Event_listener* listener)
{
  Write_lock write_lock(m_event_listeners_mutex);  

  if (!m_event_listeners.emplace(listener).second)
  {
    // Duplicate listener
    FLOW_LOG_WARNING("Failed to add duplicate listener [" << listener << "]");
    return false;
  }

  return true;
}

bool Owner_shm_pool_collection::remove_event_listener(Event_listener* listener)
{
  Write_lock write_lock(m_event_listeners_mutex);  

  size_t num_removed = m_event_listeners.erase(listener);
  if (num_removed <= 0)
  {
    // Event_listener not found
    FLOW_LOG_WARNING("Failed to remove unknown listener [" << listener << "]");
    return false;
  }
  return true;
}

shared_ptr<Shm_pool> Owner_shm_pool_collection::create_shm_pool(pool_id_t id,
                                                                const string& name,
                                                                size_t size,
                                                                void* address,
                                                                const Memory_map_functor& memory_map_functor)
{
  int fd = create_shm_object(name, size);
  if (fd == -1)
  {
    return nullptr;
  }

  void* actual_address = memory_map_functor(fd, size, address);
  if (actual_address == nullptr)
  {
    FLOW_LOG_WARNING("Could not map shared memory object [" << name << "], size [" << size << "]");
    ::close(fd);
    remove_shm_object(name);
    return nullptr;
  }

  // @todo - Make fd list cache?
  shared_ptr<Shm_pool> shm_pool
    = make_shared<Lockable_shm_pool>(id, name, actual_address, size, fd);
  if (!register_shm_pool_and_notify(shm_pool))
  {
    // We somehow allocated at an existing location
    FLOW_LOG_WARNING("Could not map shared memory pool [" << *shm_pool << "]");
    // Can't unmap as there may be custom function
    ::close(fd);
    remove_shm_object(name);
    assert(false && "Duplicate SHM pool address");
  }

  return shm_pool;
}

int Owner_shm_pool_collection::create_shm_object(const string& name, size_t size)
{
  assert(!name.empty());
  assert(size > 0);

  assert((size <= size_t(std::numeric_limits<pool_offset_t>::max()))
         && "Creating a pool sized too large to express offsets given our pointer data structures; "
            "did memory allocator algorithm (e.g., jemalloc) demand a shockingly gigantic pool? "
            "See Shm_pool_offset_ptr_data_base::pool_offset_t docs.");

  // Create shared memory pool
  int fd = ::shm_open(name.c_str(), (O_RDWR | O_CREAT | O_EXCL), m_permissions.get_permissions());
  if (fd == -1)
  {
    if (errno == EEXIST)
    {
      // Shared memory object name already exists
      FLOW_LOG_WARNING("Shared object name '" << name << "' already exists");
    }
    else
    {
      FLOW_LOG_WARNING("Error occurred when opening shm name '" << name << "': " << strerror(errno) << "(" <<
                       errno << ")");
    }
    return -1;
  }

  // Set proper permissions on the file handle due to potential conflict with umask when opening
  Error_code ec;
  util::set_resource_permissions(get_logger(), util::Native_handle(fd), m_permissions, &ec);
  if (ec)
  {
    FLOW_LOG_WARNING("Could not change permissions to [" << std::oct << m_permissions.get_permissions() << 
                     "] for object name [" << name << "], error [" << ec << "]");
    ::close(fd);
    ::shm_unlink(name.c_str());
    return -1;
  }

  // Set size
  int result = ftruncate(fd, size);
  if (result == -1)
  {
    // Handle error
    FLOW_LOG_WARNING("Error occurred when setting size for shm name '" << name << "': " << strerror(errno) << "(" <<
                     errno << ")");
    ::close(fd);
    ::shm_unlink(name.c_str());
    return -1;
  }

  return fd;
}

bool Owner_shm_pool_collection::remove_range_and_pool_if_empty(const void* address,
                                                               size_t size,
                                                               const Memory_decommit_functor* decommit_functor,
                                                               bool& removed_range,
                                                               const Memory_unmap_functor& unmap_functor,
                                                               bool& unmapped_pool)
{
  removed_range = false;
  unmapped_pool = false;

  if (size <= 0)
  {
    FLOW_LOG_WARNING("Removal size is zero");
    return false;
  }

  shared_ptr<Lockable_shm_pool> shm_pool = static_pointer_cast<Lockable_shm_pool>(lookup_shm_pool(address));
  if (shm_pool == nullptr)
  {
    FLOW_LOG_WARNING("Specified address " << address << " is not within a pool");
    return false;
  }

  // Sanity check that range is wholly within pool
  Shm_pool::size_t offset;
  if (!shm_pool->is_subset(address, size, &offset))
  {
    FLOW_LOG_WARNING("Specified range of address " << address << ", size " << size <<
                     " is not within the pool's range; potential bug or memory corruption");
    return false;
  }

  if ((decommit_functor != nullptr) && !(*decommit_functor)(shm_pool, offset, size))
  {
    FLOW_LOG_WARNING("Failed in decommitting range with address " << address << ", size " << size);
    return false;
  }

  {
    Lockable_shm_pool::Lock lock(shm_pool->get_mutex());
    size_t remaining_size = shm_pool->get_remaining_size();
    if (remaining_size < size)
    {
      if (remaining_size == 0)
      {
        FLOW_LOG_WARNING("Request to remove size [" << size << "] from a zero remaining size shared memory pool [" <<
                         *shm_pool << "]");
        return false;
      }
      FLOW_LOG_WARNING("Request to remove size " << size << " that is larger than remaining " << remaining_size <<
                       " starting at address " << address << "; will remove remainder");
    }

    if (shm_pool->remove_size(size))
    {
      FLOW_LOG_TRACE("Successfully removed address " << address << ", size " << size << " from shm_pool " <<
                     *shm_pool);
    }
    removed_range = true;

    if (shm_pool->get_remaining_size() > 0)
    {
      // There are additional regions in use, so we won't remove the shared memory pool
      return true;
    }
  }

  return remove_shm_pool(shm_pool, unmap_functor, unmapped_pool);
}

bool Owner_shm_pool_collection::remove_shm_pool(const shared_ptr<Shm_pool>& shm_pool,
                                                const Memory_unmap_functor& unmap_functor,
                                                bool& unmapped_pool)
{
  FLOW_LOG_TRACE("Removing shared memory pool [" << *shm_pool << "]");

  if (!deregister_shm_pool(shm_pool))
  {
    unmapped_pool = false;
    return false;
  }

  unmapped_pool = unmap_functor(shm_pool);

  // Close pool
  if (::close(shm_pool->get_fd()) != 0)
  {
    // Error occurred - possibly nonexistent
    FLOW_LOG_WARNING("Error occurred when closing shared memory name [" << shm_pool->get_name() << "], error [" <<
                     strerror(errno) << " (" << errno << ")]");
  }

  bool shm_object_removed = remove_shm_object(shm_pool->get_name());

  {
    Read_lock read_lock(m_event_listeners_mutex);

    // Send notification
    for (auto& cur_listener : m_event_listeners)
    {
      cur_listener->notify_removed_shm_pool(shm_pool, shm_object_removed);
    }
  }

  return true;
}

bool Owner_shm_pool_collection::remove_shm_object(const string& name)
{
  assert(!name.empty());

  if (::shm_unlink(name.c_str()) != 0)
  {
    // Error occurred - possibly nonexistent
    FLOW_LOG_WARNING("Error occurred when removing shared memory name '" << name << "': " << strerror(errno) << "(" <<
                     errno << ")");
    // @todo - Handle
    return false;
  }

  return true;
}

bool Owner_shm_pool_collection::register_shm_pool_and_notify(const shared_ptr<Shm_pool>& shm_pool)
{
  if (!register_shm_pool(shm_pool))
  {
    return false;
  }

  {
    Read_lock read_lock(m_event_listeners_mutex);

    // Send notification
    for (auto& cur_listener : m_event_listeners)
    {
      cur_listener->notify_created_shm_pool(shm_pool);
    }
  }

  return true;
}

Owner_shm_pool_collection::Event_listener::~Event_listener() = default;

} // namespace ipc::shm::arena_lend
