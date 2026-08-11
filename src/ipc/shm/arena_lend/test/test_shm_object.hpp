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
#include "ipc/shm/arena_lend/arena_lend_fwd.hpp"
#include "ipc/util/shared_name.hpp"
#include <flow/test/test_common_util.hpp>

namespace ipc::shm::arena_lend::test
{

/// Prefix of shared memory object names (without the leading slash).
extern const std::string S_SHM_OBJECT_NAME_PREFIX;
/// Location of shared memory objects.
extern const std::string S_SHM_OBJECT_DIR;

Shared_name shm_object_generate_name(const Shared_name& pool_name_base);
/**
 * Creates a SHM pool name base (prefix) for use in tests.  The resulting `Shared_name` encodes
 * a test-specific prefix, the GTest suite name (or the supplied `use_case_id`), and the PID.
 *
 * @param use_case_id Typically the test or application name, which will be used as a trailing part of the
 *                    shared object name prefix. If this value is empty, the application must be running
 *                    in a Googletest context as the test name will be used. If it is not running in such
 *                    a context, an exception will be thrown.
 *
 * @return See above.
 */
Shared_name create_test_pool_name_base(const std::string& use_case_id = "");

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
  std::string output = flow::test::collect_output([&shm_pool_collection]() { shm_pool_collection = nullptr; }, os);
  return check_empty_collection_in_output(output);
}

} // namespace ipc::shm::arena_lend::test
