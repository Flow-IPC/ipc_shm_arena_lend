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

#include "ipc/shm/arena_lend/jemalloc/memory_manager.hpp"
#include "ipc/shm/arena_lend/jemalloc/detail/jemalloc.hpp"
#include <errno.h>
#include <sstream>

using std::size_t;
using std::string;
using std::to_string;
using std::errc;
using std::system_error;
using std::stringstream;

namespace ipc::shm::arena_lend::jemalloc
{

Memory_manager::Memory_manager(flow::log::Logger* logger) :
  arena_lend::Memory_manager(logger)
{
}

void* Memory_manager::allocate(size_t size)
{
  FLOW_LOG_DATA("Allocating size " << size << ", default arena, no thread cache");
  return IPC_SHM_ARENA_LEND_JEMALLOC_API(malloc)(size);
}

void* Memory_manager::allocate(size_t size, Arena_id arena_id)
{
  FLOW_LOG_DATA("Allocating size " << size << ", arena " << arena_id << ", no thread cache");
  return allocate_helper(size, arena_id, MALLOCX_TCACHE_NONE);
}

void* Memory_manager::allocate(size_t size, Arena_id arena_id, Thread_cache_id thread_cache_id)
{
  FLOW_LOG_DATA("Allocating size " << size << ", arena " << arena_id << ", thread cache " << thread_cache_id);
  return allocate_helper(size, arena_id, MALLOCX_TCACHE(thread_cache_id));
}

void* Memory_manager::allocate_helper(size_t size, Arena_id arena_id, int thread_cache_flags)
{
  assert(size > 0);

  void* address = IPC_SHM_ARENA_LEND_JEMALLOC_API(mallocx)(size, (MALLOCX_ARENA(arena_id) | thread_cache_flags));
  FLOW_LOG_DATA("Allocated size " << size << ", arena " << arena_id << " resulting in " << address);
  return address;
}

void Memory_manager::deallocate(void* address)
{
  FLOW_LOG_DATA("Deallocating address " << address << ", default arena");
  IPC_SHM_ARENA_LEND_JEMALLOC_API(free)(address);
}

void Memory_manager::deallocate(void* address, Arena_id arena_id)
{
  FLOW_LOG_DATA("Deallocating address " << address << ", arena " << arena_id << ", no thread cache");
  deallocate_helper(address, arena_id, MALLOCX_TCACHE_NONE);
}

void Memory_manager::deallocate(void* address, Arena_id arena_id, Thread_cache_id thread_cache_id)
{
  FLOW_LOG_DATA("Deallocating address " << address << ", arena " << arena_id << ", thread cache " << thread_cache_id);
  deallocate_helper(address, arena_id, MALLOCX_TCACHE(thread_cache_id));
}

void Memory_manager::deallocate_helper(void* address, Arena_id arena_id, int thread_cache_flags)
{
  assert(address != nullptr);

  IPC_SHM_ARENA_LEND_JEMALLOC_API(dallocx)(address, (MALLOCX_ARENA(arena_id) | thread_cache_flags));
  FLOW_LOG_DATA("Deallocated address " << address << ", arena " << arena_id);
}

Memory_manager::Arena_id Memory_manager::create_arena(extent_hooks_t* extent_hooks)
{
  FLOW_LOG_INFO("Creating arena");

  Arena_id arena_id;
  size_t output_size = sizeof(arena_id);

  extent_hooks_t** input_param;
  size_t input_size;
  if (extent_hooks == nullptr)
  {
    input_param = nullptr;
    input_size = 0;
  }
  else
  {
    input_param = &extent_hooks;
    input_size = sizeof(extent_hooks);
  }

  int ec = IPC_SHM_ARENA_LEND_JEMALLOC_API(mallctl)("arenas.create", &arena_id, &output_size, input_param, input_size);
  if (ec != 0)
  {
    FLOW_LOG_WARNING("je_mallctl() - Could not create arena");
    throw system_error(make_error_code(static_cast<errc>(ec)), "je_mallctl() error on creating arena");
  }

  FLOW_LOG_INFO("Created arena " << arena_id);

  return arena_id;
}

void Memory_manager::destroy_arena(Arena_id arena_id)
{
  int ec;
  size_t input_size = 0;

  FLOW_LOG_INFO("Destroying arena " << arena_id);

  string command("arena.");
  command += to_string(arena_id);
  command += ".destroy";

  if ((ec = IPC_SHM_ARENA_LEND_JEMALLOC_API(mallctl)(command.c_str(), nullptr, nullptr, nullptr, input_size)) != 0)
  {
    FLOW_LOG_WARNING("je_mallctl(" << command << ") - Could not destroy arena " << arena_id);
    throw system_error(make_error_code(static_cast<errc>(ec)), "je_mallctl() error on destroying arena");
  }

  FLOW_LOG_INFO("Destroyed arena " << arena_id);
}

Memory_manager::Thread_cache_id Memory_manager::create_thread_cache()
{
  FLOW_LOG_INFO("Creating thread cache");

  Thread_cache_id id;
  size_t output_size = sizeof(Thread_cache_id);
  int ec = IPC_SHM_ARENA_LEND_JEMALLOC_API(mallctl)("tcache.create", &id, &output_size, nullptr, 0);
  if (ec != 0)
  {
    FLOW_LOG_WARNING("je_mallctl() - Could not create thread cache");
    throw system_error(make_error_code(static_cast<errc>(ec)), "je_mallctl() error on creating a thread cache");
  }

  FLOW_LOG_INFO("Created thread cache id " << id);

  return id;
}

void Memory_manager::destroy_thread_cache(Thread_cache_id id)
{
  FLOW_LOG_INFO("Destroying thread cache id " << id);

  int ec = IPC_SHM_ARENA_LEND_JEMALLOC_API(mallctl)("tcache.destroy", nullptr, nullptr, &id, sizeof(Thread_cache_id));
  if (ec != 0)
  {
    FLOW_LOG_WARNING("je_mallctl() - Could not destroy cache id " << id);
    throw system_error(make_error_code(static_cast<errc>(ec)), "je_mallctl() error on destroying cache");
  }

  FLOW_LOG_INFO("Destroyed thread cache id " << id);
}

void Memory_manager::flush_thread_cache(Thread_cache_id id)
{
  FLOW_LOG_INFO("Flushing thread cache id " << id);

  int ec = IPC_SHM_ARENA_LEND_JEMALLOC_API(mallctl)("tcache.flush", nullptr, nullptr, &id, sizeof(Thread_cache_id));
  if (ec != 0)
  {
    FLOW_LOG_WARNING("je_mallctl() - Could not flush cache id " << id);
    throw system_error(make_error_code(static_cast<errc>(ec)), "je_mallctl() error on flushing thread cache by id");
  }

  FLOW_LOG_INFO("Flushed thread cache id " << id);
}

/*
void Memory_manager::flush_current_thread_cache()
{
  FLOW_LOG_INFO("Flushing current thread cache");

  int ec = IPC_SHM_ARENA_LEND_JEMALLOC_API(mallctl)("thread.tcache.flush", nullptr, nullptr, nullptr, 0);
  if (ec != 0)
  {
    FLOW_LOG_WARNING("je_mallctl() - Could not flush current thread cache");
    throw system_error(make_error_code(static_cast<errc>(ec)), "je_mallctl() error on flushing current thread cache");
  }

  FLOW_LOG_INFO("Flushed current thread cache");
}
*/

void Memory_manager::print_stats() const
{
  IPC_SHM_ARENA_LEND_JEMALLOC_API(malloc_stats_print)(NULL, NULL, NULL);
}

} // namespace ipc::shm::arena_lend::jemalloc
