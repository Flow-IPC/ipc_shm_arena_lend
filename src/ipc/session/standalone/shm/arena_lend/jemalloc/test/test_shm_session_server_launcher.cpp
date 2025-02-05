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

#include "ipc/session/standalone/shm/arena_lend/jemalloc/test/test_shm_session_server_launcher.hpp"
#include "ipc/session/standalone/shm/arena_lend/jemalloc/test/test_shm_session_server.hpp"
#include "ipc/test/test_common_util.hpp"
#include <ipc/common.hpp>
#include <flow/test/test_common_util.hpp>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
#pragma GCC diagnostic ignored "-Wnarrowing"
#include <boost/process/system.hpp>
#pragma GCC diagnostic pop
#include <boost/process/io.hpp>

using std::ostream;
using std::string;
using flow::test::to_underlying;

namespace ipc::session::shm::arena_lend::jemalloc::test
{

using Object_type = Test_shm_session_server::Object_type;
using Operation_mode = Test_shm_session_server::Operation_mode;

Test_shm_session_server_launcher::Test_shm_session_server_launcher(flow::log::Logger* logger) :
  flow::log::Log_context(logger, Log_component::S_TEST),
  m_task_loop(get_logger(), "Launcher_loop"),
  m_is_running(false)
{
  m_task_loop.start();
}

Test_shm_session_server_launcher::~Test_shm_session_server_launcher()
{
  m_task_loop.stop();
}

bool Test_shm_session_server_launcher::async_run(Object_type object_type,
                                                 Result_callback&& result_callback,
                                                 Operation_mode operation_mode)
{
  {
    Lock lock(m_mutex);
    if (m_is_running)
    {
      FLOW_LOG_WARNING("Already running; ignoring request to run");
      return false;
    }
    m_is_running = true;
  }

  namespace bp = boost::process;

  FLOW_LOG_INFO("Executing: [" << Test_shm_session_server::get_server_path() << "]");

  m_task_loop.post([this, object_type, operation_mode, result_callback = std::move(result_callback)]()
                   {
                     // Execute program and pipe stderr and stdout to this process
                     int exit_code = bp::system(Test_shm_session_server::get_server_path(),
                                                std::to_string(to_underlying(object_type)),
                                                std::to_string(to_underlying(operation_mode)),
                                                std::to_string(ipc::test::get_process_creds().process_id()),
                                                bp::std_out > stdout,
                                                bp::std_err > stderr);
                     Result result;
                     if (exit_code == static_cast<int>(Result::S_SUCCESS))
                     {
                       result = Result::S_SUCCESS;
                     }
                     else if (exit_code == static_cast<int>(Result::S_FAIL))
                     {
                       result = Result::S_FAIL;
                     }
                     else if (exit_code == static_cast<int>(Result::S_TIMEOUT))
                     {
                       result = Result::S_TIMEOUT;
                     }
                     else
                     {
                       result = Result::S_UNKNOWN_FAILURE;
                     }

                     FLOW_LOG_INFO("Translated exit code [" << exit_code << "] to result [" << result << "]");

                     {
                       Lock lock(m_mutex);
                       m_is_running = false;
                     }

                     result_callback(result);
                   });

  return true;
}

ostream& operator<<(ostream& os, Test_shm_session_server_launcher::Result result)
{
  switch (result)
  {
    case Test_shm_session_server_launcher::Result::S_SUCCESS:
      os << "Success";
      break;
    case Test_shm_session_server_launcher::Result::S_FAIL:
      os << "Fail";
      break;
    case Test_shm_session_server_launcher::Result::S_TIMEOUT:
      os << "Timeout";
      break;
    case Test_shm_session_server_launcher::Result::S_UNKNOWN_FAILURE:
      os << "Unknown failure";
      break;
    case Test_shm_session_server_launcher::Result::S_INVALID_ARGUMENT:
      os << "Invalid argument";
      break;
  }
  return os;
}

} // namespace ipc::session::shm::arena_lend::jemalloc::test
