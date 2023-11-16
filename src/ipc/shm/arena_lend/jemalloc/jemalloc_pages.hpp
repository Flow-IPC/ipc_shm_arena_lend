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

#include <boost/core/noncopyable.hpp>

namespace ipc::shm::arena_lend::jemalloc
{

/**
 * This class contains an API to access functionality that is internal to jemalloc. It is derived from jemalloc's
 * pages.c source file in version 5.2.1.x.
 */
class Jemalloc_pages :
  private boost::noncopyable
{
public:
  /// Disallow construction.
  Jemalloc_pages() = delete;

  /**
   * Returns the system page size.
   *
   * @return See above.
   */
  static std::size_t get_page_size();
  /**
   * Returns the system page mask.
   *
   * @return See above.
   */
  static std::size_t get_page_mask();

  /**
   * Maps a memory region into the address space.
   *
   * @param address Specific address to map to or nullptr to allow system to select.
   * @param size Size of memory to map; must be a multiple of system page size.
   * @param alignment Offset from the specified address.
   * @param commit Specifies whether to commit the pages to memory; it will be overwritten based on whether the
   *               memory was committed or not. In addition, the input may be disregarded if the system overcommits
   *               memory, which results in always setting to commit.
   * @param fd If mapping from a file, the file descriptor to map; otherwise, -1.
   *
   * @return Upon success, the address to the mapped region; otherwise, nullptr. When specifying a non-null address
   *         parameter (addr), the return value will be the same address upon success.
   */
  static void* map(void* address, std::size_t size, std::size_t alignment, bool& commit, int fd);
  /**
   * Unmaps a memory region from the address space.
   *
   * @param address The address of the memory region to unmap.
   * @param size The size of the memory region.
   */
  static void unmap(void* address, std::size_t size);
  /**
   * Commits a contiguous set of virtual memory pages into physical memory.
   *
   * @param address The base address of the pages.
   * @param size The size of the region.
   *
   * @return Whether the pages were successfully committed.
   */
  static bool commit(void* address, std::size_t size);
  /**
   * Decommits a contiguous set of virtual memory pages from physical memory that are mapped to a file.
   *
   * @param address The base address of the pages.
   * @param fd A valid file descriptor to a memory mapped file.
   * @param file_offset The offset from the beginning of the file.
   * @param size The size of the region.
   * @param force Whether to force the decommit and not base it on configuration.
   *
   * @return Whether the pages were successfully decommitted.
   */
  static bool decommit(void* address, int fd, std::size_t file_offset, std::size_t size, bool force = false);
  /**
   * Purges memory pages mapped to a file. The purging will zero out the pages and will keep the size of the file
   * the same. If the application subsequently accesses a page purged as part of this call, a page fault occurs.
   *
   * @param fd A valid file descriptor to a memory mapped file.
   * @param file_offset The offset from the beginning of the file.
   * @param size The size of the region.
   *
   * @return Whether the pages were successfully purged.
   */
  static bool purge_forced(int fd, std::size_t file_offset, std::size_t size);

protected:
  /**
   * Sets whether to simulate the system to overcommit memory. This is not thread-safe and should not be modified
   * once memory is allocated. This is primarily here for testing purposes.
   *
   * @param overcommit Whether to simulate the system to overcommit memory.
   */
  static void set_os_overcommit_memory(bool overcommit);
  /**
   * Returns whether the system overcommits memory, simulated or in reality.
   *
   * @return See above.
   */
  static bool get_os_overcommit_memory();
  /**
   * Commits a contiguous set of virtual memory pages into physical memory using the original jemalloc approach of
   * using mmap(). This is primarily here for testing purposes.
   *
   * @param address The base address of the pages.
   * @param size The size of the region.
   *
   * @return Whether the pages were successfully committed.
   */
  static bool commit_original(void* address, size_t size);
  /**
   * Decommits a contiguous set of virtual memory pages from physical memory using the original jemalloc approach of
   * using mmap(). This is primarily here for testing purposes.
   *
   * @param address The base address of the pages.
   * @param size The size of the region.
   *
   * @return Whether the pages were successfully decommitted.
   */
  static bool decommit_original(void* address, size_t size);
}; // class Jemalloc_pages

} // namespace ipc::shm::arena_lend::jemalloc
