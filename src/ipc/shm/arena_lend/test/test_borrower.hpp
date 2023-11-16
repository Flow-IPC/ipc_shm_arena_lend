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

#include <string>
#include "ipc/shm/arena_lend/shm_pool_collection.hpp"

namespace ipc::shm::arena_lend::test
{

using pool_id_t = Shm_pool::pool_id_t;

/**
 * Simulates a borrower process that opens a shared memory pool and runs tests.
 */
class Test_borrower
{
public:
  /**
   * Runs a process that opens a shared memory pool and makes sure the data matches expected.
   *
   * @param shm_pool_collection_id The identifier for the shared memory collection.
   * @param shm_object_name The name of the shared memory object to open.
   * @param shm_object_size The size of the shared memory.
   * @param data_offset The offset from the base address of the shared memory region to read data from.
   * @param data The expected data in the shared memory.
   *
   * @return Upon success, 0; otherwise, non-zero.
   */
  int execute_read_check(Collection_id shm_pool_collection_id,
                         pool_id_t shm_pool_id,
                         const std::string& shm_object_name,
                         std::size_t shm_object_size,
                         std::size_t data_offset,
                         const std::string& data);

  /// The program name.
  static const std::string S_PROGRAM_NAME;
  /// The relative path to the program.
  static const std::string S_PROGRAM_PATH;
  /// The shared memory pool collection id command-line parameter.
  static const std::string S_SHM_POOL_COLLECTION_ID_PARAM;
  /// The shared memory pool id command-line parameter.
  static const std::string S_SHM_POOL_ID_PARAM;
  /// The shared memory object name command-line parameter.
  static const std::string S_SHM_OBJECT_NAME_PARAM;
  /// The shared memory object size command-line parameter.
  static const std::string S_SHM_OBJECT_SIZE_PARAM;
  /// The offset from the base of the shared memory object where the data resides command-line parameter.
  static const std::string S_DATA_OFFSET_PARAM;
  /// The expected data command-line parameter.
  static const std::string S_EXPECTED_DATA_PARAM;

private:
  /**
   * Converts a parameter into its long form used in the command-line.
   *
   * @param s The short command-line parameter name.
   *
   * @return The long form of the parameter.
   */
  static std::string convert_to_long_param(const std::string& s);
}; // class Test_borrower

} // namespace ipc::shm::arena_lend::test
