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

#include <flow/async/single_thread_task_loop.hpp>
#include "ipc/session/standalone/shm/arena_lend/jemalloc/test/test_shm_session_server_executor.hpp"

namespace ipc::session::shm::arena_lend::jemalloc::test
{

/**
 * Launches a process that executes a server to be used for
 * #ipc::session::shm::arena_lend::jemalloc::test::Shm_session_test.
 * The process is waited on asynchronously to the calling thread.
 */
class Test_shm_session_server_launcher :
  public flow::log::Log_context
{
public:
  /**
   * The result domain of the test from the server perspective. This will be the exit code emitted.
   */
  enum class Result
  {
    /// An invalid parameter value was passed.
    S_INVALID_ARGUMENT = -1,
    /// Server completed successfully.
    S_SUCCESS,
    /// Server failed in a predictable way.
    S_FAIL,
    /// Server did not complete within a time threshold.
    S_TIMEOUT,
    /// Server failed in an unpredictable way.
    S_UNKNOWN_FAILURE
  }; // enum class Result

  /**
   * Alias for functor used as a callback when the result of the server is determined.
   *
   * @param result The server result.
   */
  using Result_callback = std::function<void(Result result)>;

  /**
   * Constructor. The event loop will be started here.
   *
   * @param logger Used for logging purposes.
   */
  Test_shm_session_server_launcher(flow::log::Logger* logger);
  /// Destructor. The event loop will be stopped here.
  ~Test_shm_session_server_launcher();
  /**
   * When succcessful, asynchronously executes the server in another process and waits for its completion.
   *
   * @param object_type The type of object the server should send to the client.
   * @param result_callback The callback to be executed when the result of the server is determined.
   * @param operation_mode The server behavior, which correlates to the test that is being run.
   *
   * @return Whether the server was not previously running, which means that execution occurred.
   */
  bool async_run(Test_shm_session_server::Object_type object_type,
                 Result_callback&& result_callback,
                 Test_shm_session_server::Operation_mode operation_mode =
                   Test_shm_session_server::Operation_mode::S_NORMAL);

private:
  /// The mutex type.
  using Mutex = std::mutex;
  /// Exclusive lock.
  using Lock = std::lock_guard<Mutex>;

  /// The task engine to run the program asynchronously and wait for it to complete.
  flow::async::Single_thread_task_loop m_task_loop;
  /// Synchronizes the execution of the program, so that execution is serial.
  Mutex m_mutex;
  /// Whether the program is currently running.
  bool m_is_running;
}; // class Test_shm_session_server_launcher

/**
 * Outputs the enumeration to the stream.
 *
 * @param os The output stream.
 * @param result The enumeration to output.
 *
 * @return The output stream parameter.
 */
std::ostream& operator<<(std::ostream& os, Test_shm_session_server_launcher::Result result);

} // namespace ipc::session::shm::arena_lend::jemalloc::test
