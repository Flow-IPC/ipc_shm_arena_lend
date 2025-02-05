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
#include <flow/test/test_common_util.hpp>
#include <flow/log/log.hpp>

using std::string;
using std::string_view;
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
  string actual_use_case_id = (use_case_id.empty() ? flow::test::get_test_suite_name() : use_case_id);
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
  prefix += flow::test::get_test_suite_name();
  prefix += "_";

  return remove_shm_objects_filesystem(prefix);
}

bool check_empty_collection_in_output(const string& output)
{
  string_view output_view(output);

  // Parse each shared memory pool's size and remaining size
  constexpr string_view SHM_POOL_LOG_TOKEN = ":print_shm_pool_map";
  constexpr string_view EMPTY_MAP_TOKEN = "Empty SHM pool map";
  constexpr string_view SIZE_START_TOKEN = ", size: ";
  constexpr string_view SIZE_END_TOKEN = ",";
  constexpr string_view REMAINING_START_TOKEN = ", remaining size: ";
  constexpr string_view REMAINING_END_TOKEN = "]";

  size_t search_index = 0;
  while ((search_index = output.find(SHM_POOL_LOG_TOKEN, search_index)) != output.npos)
  {
    search_index += SHM_POOL_LOG_TOKEN.size();

    size_t cur_line_end = output.find("\n", search_index);
    if (cur_line_end == output.npos)
    {
      // Did not parse successfully, so assume fail
      // std::cerr << "Did not find new line token\n";
      return false;
    }
    auto cur_line_size = cur_line_end - search_index;
    auto cur_line = output_view.substr(search_index, cur_line_size);

    // If we find this token we're done; otherwise, match size of pool with remaining size
    if (cur_line.find(EMPTY_MAP_TOKEN, 0) == cur_line.npos)
    {
      auto cur_size_start = cur_line.find(SIZE_START_TOKEN, 0);
      if (cur_size_start == cur_line.npos)
      {
        // Did not parse successfully, so assume fail
        // std::cerr << "Did not find token: [" << SIZE_START_TOKEN << "]\n";
        return false;
      }
      // Advance to the end of the token
      cur_size_start += SIZE_START_TOKEN.size();

      auto cur_size_end = cur_line.find(SIZE_END_TOKEN, cur_size_start);
      if (cur_size_end == cur_line.npos)
      {
        // Did not parse successfully, so assume fail
        // std::cerr << "Did not find token: [" << SIZE_END_TOKEN << "]\n";
        return false;
      }

      auto cur_remaining_start = cur_line.find(REMAINING_START_TOKEN, (cur_size_end + SIZE_END_TOKEN.size()));
      if (cur_remaining_start == cur_line.npos)
      {
        // Did not parse successfully, so assume fail
        // std::cerr << "Did not find token: [" << REMAINING_START_TOKEN << "]\n";
        return false;
      }
      // Advance to the end of the token
      cur_remaining_start += REMAINING_START_TOKEN.size();

      auto cur_remaining_end = cur_line.find(REMAINING_END_TOKEN, cur_remaining_start);
      if (cur_remaining_end == cur_line.npos)
      {
        // Did not parse successfully, so assume fail
        // std::cerr << "Did not find token: [" << REMAINING_END_TOKEN << "]\n";
        return false;
      }

      std::string_view size_value = cur_line.substr(cur_size_start, (cur_size_end - cur_size_start));
      std::string_view remaining_value = cur_line.substr(cur_remaining_start,
                                                         (cur_remaining_end - cur_remaining_start));
      if (size_value != remaining_value)
      {
        // Values don't match, so fail
        /*
        std::cerr << "Size [" << size_value << ", " << cur_size_start << ", " << cur_size_end << "], "
          "remaining size [" << remaining_value << ", " << cur_remaining_start << ", " << cur_remaining_end << "]\n";
        */
        return false;
      }
    }

    search_index += cur_line_size;
  }

  return true;
}

} // namespace ipc::shm::arena_lend::test
