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
#include <iostream>
#include "ipc/shm/arena_lend/owner_shm_pool_collection.hpp"
#include "ipc/test/test_common_util.hpp"

namespace ipc::shm::arena_lend::test
{

/// Prefix of shared memory object names (without the leading slash).
extern const std::string S_SHM_OBJECT_NAME_PREFIX;
/// Location of shared memory objects.
extern const std::string S_SHM_OBJECT_DIR;

/**
 * Returns a new shared memory object name, which should be unique. The format is:
 * object name = [prefix] + "_" + [process id] + "_" + [monotonically increasing counter starting from 1]
 * prefix = S_SHM_OBJECT_NAME_PREFIX + "_" + [use case id]
 *
 * Typically the use case id is the test or application name.
 *
 * @param prefix The prefix of the name to use.
 *
 * @return See above.
 */
std::string generate_shm_object_name(const std::string& prefix);
/**
 * Creates a functor to generate shared memory object names for use in tests using a specific test prefix.
 *
 * @param use_case_id Typically the test or application name, which will be used as a trailing part of the
 *                    shared object name prefix. If this value is empty, the application must be running
 *                    in a Googletest context as the test name will be used. If it is not running in such
 *                    a context, an exception will be thrown.
 *
 * @return See above.
 *
 * @see generate_shm_object_name
 */
Owner_shm_pool_collection::Shm_object_name_generator create_shm_object_name_generator(
  const std::string& use_case_id = "");

/**
 * Removes shared memory objects directly via the filesystem.
 *
 * @param prefix The prefix of the shared memory objects to remove without leading path and slash.
 *
 * @return Whether all shared memory objects that matched prefix were successfully removed.
 */
bool remove_shm_objects_filesystem(const std::string& prefix);
/**
 * Removes shared memory objects named with a specific test prefix directly via the filesystem.
 *
 * @return Whether all shared memory objects were successfully removed.
 */
bool remove_test_shm_objects_filesystem();

/**
 * Checks that the output indicates that the collection contained only empty shared memory pools, if any.
 * In particular, there is an output expectation that all shared memory pools will be output at destruction with
 * a regular expression format of: ".*, size: SIZE,.*, remaining size: REMAINING_SIZE].*". The value SIZE is
 * compared to REMAINING_SIZE. If there are no pools, then "Empty SHM pool map" is expected.
 *
 * @param output The captured output after collection destruction.
 *
 * @return Whether the captured output indicated that the collection contained only empty shared memory pools, if any.
 */
bool check_empty_collection_in_output(const std::string& output);

/**
 * Sets the shared memory pool collection to nullptr, which should be the last handle to the shared pointer. This
 * should cause the destructor to display the remaining pools, which we verify to have a particular output indicating
 * empty. The log level of the message is at TRACE, so the logger must be at that severity level of lower.
 *
 * @tparam Owner_shm_pool_collection_pointer_type The owner shared memory pool collection pointer type.
 * @param shm_pool_collection The shared memory pool collection. The passed in parameter should be the last
 *                            reference to the shared pointer.
 * @param os The stream to check output on.
 *
 * @return Whether the collection was detected to be the last reference and empty.
 */
template <typename Owner_shm_pool_collection_pointer_type>
bool ensure_empty_collection_at_destruction(Owner_shm_pool_collection_pointer_type& shm_pool_collection,
                                            std::ostream& os = std::cout)
{
  std::string output = ipc::test::collect_output([&shm_pool_collection]() { shm_pool_collection = nullptr; }, os);
  return check_empty_collection_in_output(output);
}

} // namespace ipc::shm::arena_lend::test
