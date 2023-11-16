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

#include "ipc/shm/arena_lend/test/test_borrower.hpp"
#include "ipc/test/test_logger.hpp"
#include <flow/log/log.hpp>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
#pragma GCC diagnostic ignored "-Wnarrowing"
#include <boost/process/system.hpp>
#pragma GCC diagnostic pop

using std::string;
using std::size_t;
using std::to_string;
using ipc::test::Test_logger;

namespace ipc::shm::arena_lend::test
{

// Static constants
const string Test_borrower::S_PROGRAM_NAME("libipc_test_borrower.exec");
const string Test_borrower::S_PROGRAM_PATH("./" + S_PROGRAM_NAME);
const string Test_borrower::S_SHM_POOL_COLLECTION_ID_PARAM("shm-pool-collection-id");
const string Test_borrower::S_SHM_POOL_ID_PARAM("shm-object-id");
const string Test_borrower::S_SHM_OBJECT_NAME_PARAM("shm-object-name");
const string Test_borrower::S_SHM_OBJECT_SIZE_PARAM("shm-object-size");
const string Test_borrower::S_DATA_OFFSET_PARAM("data-offset");
const string Test_borrower::S_EXPECTED_DATA_PARAM("expected-data");

// Static method
string Test_borrower::convert_to_long_param(const string& s)
{
  assert(!s.empty());
  return "--" + s;
}

int Test_borrower::execute_read_check(Collection_id shm_pool_collection_id,
                                      pool_id_t shm_pool_id,
                                      const string& shm_object_name,
                                      size_t shm_object_size,
                                      size_t data_offset,
                                      const string& data)
{
  namespace bp = boost::process;

  Test_logger test_logger;

  FLOW_LOG_SET_CONTEXT(&test_logger, Log_component::S_TEST);

  FLOW_LOG_INFO("Executing: " << S_PROGRAM_PATH << " " <<
                convert_to_long_param(S_SHM_POOL_COLLECTION_ID_PARAM) << " " << shm_pool_collection_id << " " <<
                convert_to_long_param(S_SHM_POOL_ID_PARAM) << " " << shm_pool_id << " " <<
                convert_to_long_param(S_SHM_OBJECT_NAME_PARAM) << " " << shm_object_name << " " <<
                convert_to_long_param(S_SHM_OBJECT_SIZE_PARAM) << " " << to_string(shm_object_size) << " " <<
                convert_to_long_param(S_DATA_OFFSET_PARAM) << " " << to_string(data_offset) << " " <<
                convert_to_long_param(S_EXPECTED_DATA_PARAM) << " " << data);

  return bp::system(S_PROGRAM_PATH,
                    convert_to_long_param(S_SHM_POOL_COLLECTION_ID_PARAM), to_string(shm_pool_collection_id),
                    convert_to_long_param(S_SHM_POOL_ID_PARAM), to_string(shm_pool_id),
                    convert_to_long_param(S_SHM_OBJECT_NAME_PARAM), shm_object_name,
                    convert_to_long_param(S_SHM_OBJECT_SIZE_PARAM), to_string(shm_object_size),
                    convert_to_long_param(S_DATA_OFFSET_PARAM), to_string(data_offset),
                    convert_to_long_param(S_EXPECTED_DATA_PARAM), data);
}

} // namespace ipc::shm::arena_lend::test
