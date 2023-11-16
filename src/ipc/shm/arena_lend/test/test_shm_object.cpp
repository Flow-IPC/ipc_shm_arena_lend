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

#include "ipc/shm/arena_lend/test/test_shm_object.hpp"
#include "ipc/test/test_logger.hpp"
#include <sstream>
#include <boost/filesystem.hpp>
#include <flow/log/log.hpp>

using std::string;
using std::size_t;
using std::stringstream;
using std::to_string;
using ipc::test::Test_logger;

namespace bfs = boost::filesystem;

namespace ipc::shm::arena_lend::test
{

// Static constants
const string S_SHM_OBJECT_NAME_PREFIX("shm_test_");
const string S_SHM_OBJECT_DIR("/dev/shm");

// Static method
string generate_shm_object_name(const string& prefix)
{
  static std::atomic<size_t> object_index = 0;
  static pid_t pid = getpid();
  stringstream ss;
  ss << '/' << prefix << '_' << to_string(pid) << '_' << to_string(++object_index);
  return ss.str();
}

Owner_shm_pool_collection::Shm_object_name_generator create_shm_object_name_generator(const string& use_case_id)
{
  string actual_use_case_id = (use_case_id.empty() ? ipc::test::get_test_suite_name() : use_case_id);
  return [actual_use_case_id = std::move(actual_use_case_id)](auto&&)
         {
           return generate_shm_object_name(S_SHM_OBJECT_NAME_PREFIX + actual_use_case_id);
         };
}

bool remove_shm_objects_filesystem(const string& prefix)
{
  Test_logger test_logger;

  FLOW_LOG_SET_CONTEXT(&test_logger, Log_component::S_TEST);

  boost::system::error_code ec;
  bfs::directory_iterator dir_iter(S_SHM_OBJECT_DIR, ec);
  if (ec)
  {
    FLOW_LOG_WARNING("Error when iterating over directory '" << S_SHM_OBJECT_DIR << "': " << ec);
    return false;
  }

  bool result = true;
  for (bfs::directory_entry& entry : dir_iter)
  {
    bfs::path cur_path = entry.path();
    string cur_file_name = cur_path.filename().string();
    if (cur_file_name.rfind(prefix, 0) == 0)
    {
      FLOW_LOG_INFO("Removing file '" << cur_path << "'");
      if (!bfs::remove(cur_path, ec))
      {
        if (ec)
        {
          FLOW_LOG_WARNING("Could not remove file '" << cur_path << "', error: " << ec);
          result = false;
        }
        else
        {
          FLOW_LOG_WARNING("File '" << cur_path << "' somehow didn't exist during removal");
        }
      }
    }
  }

  return result;
}

bool remove_test_shm_objects_filesystem()
{
  string prefix = S_SHM_OBJECT_NAME_PREFIX;
  prefix += ipc::test::get_test_suite_name();
  prefix += "_";

  return remove_shm_objects_filesystem(prefix);
}

} // namespace ipc::shm::arena_lend::test
