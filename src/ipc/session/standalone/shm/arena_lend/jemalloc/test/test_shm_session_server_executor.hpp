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

#include <flow/log/log.hpp>
#include <chrono>
#include <boost/container/string.hpp>
#include <boost/container/list.hpp>
#include "ipc/session/standalone/shm/arena_lend/jemalloc/test/test_shm_session_server.hpp"

namespace ipc::session::shm::arena_lend::jemalloc::test
{

/**
 * Executes the server portion of the test for
 * #ipc::session::shm::arena_lend::jemalloc::test::Shm_session_test.
 */
class Test_shm_session_server_executor :
  public flow::log::Log_context
{
public:
  struct Simple_object;

  /// Object vector type.
  template <template<typename> typename Allocator>
  using Vector_type = std::vector<char, Allocator<char>>;
  /// Object string type.
  template <template<typename> typename Allocator>
  using String_type = boost::container::basic_string<char, std::char_traits<char>, Allocator<char>>;
  /// Object list type.
  template <template<typename> typename Allocator>
  using List_type = boost::container::list<Simple_object, Allocator<Simple_object>>;
  /// The timeout to furnish a result.
  static const std::chrono::duration<size_t> S_TEST_TIMEOUT;
  /// The timeout to furnish a result during performance testing.
  static const std::chrono::duration<size_t> S_PERFORMANCE_TEST_TIMEOUT;
  /// The text to store in the object.
  static const std::string S_MESSAGE;
  /// The character stored in a string object.
  static constexpr char S_STRING_CHAR = 'a';
  /// The length of a string object.
  static constexpr std::size_t S_STRING_SIZE = (10 * 1024 * 1024);
  /// The number of elements in the list.
  static constexpr std::size_t S_LIST_SIZE = 1000;
  /// The number of elements in the list for performance testing.
  static constexpr std::size_t S_PERFORMANCE_LIST_SIZE = 1000000;

  /// The result types.
  enum class Result
  {
    S_SUCCESS = 0,
    S_FAIL,
    S_TIMEOUT
  }; // enum class Result

  /// Simple object containing a fixed length message that will be placed in shared memory and shared with a borrower.
  struct Simple_object
  {
    /// The message contents.
    char m_message[256];
  }; // struct Simple_object

  /**
   * Constructor.
   *
   * @param logger Used for logging purposes.
   */
  Test_shm_session_server_executor(flow::log::Logger* logger);
  /**
   * Executes the server and produces a result from the server perspective.
   *
   * @param object_type The type of object the server should send to the client.
   * @param client_process_id The process id of the client that will establish a session with the server.
   * @param operation_mode The server behavior, which correlates to the test that is being run.
   *
   * @return The server result.
   */
  Result run(Test_shm_session_server::Object_type object_type,
             pid_t client_process_id,
             Test_shm_session_server::Operation_mode operation_mode);

  /**
   * Returns a functor that creates a Simple_object containing a fixed message. The contents are stored in
   * shared memory.
   *
   * @return See above.
   */
  static Test_shm_session_server::Object_creation_callback char_array_creator_functor();
  /**
   * Returns a functor that creates a vector of characters containing a fixed message. The contents of the vector
   * are stored in shared memory.
   *
   * @return See above.
   */
  static Test_shm_session_server::Object_creation_callback vector_char_creator_functor();
  /**
   * Returns a functor that creates a string containing a large fixed message. The contents of the string are
   * stored in shared memory.
   *
   * @return See above.
   */
  static Test_shm_session_server::Object_creation_callback string_creator_functor();
  /**
   * Returns a functor that creates a list containing fixed messages. The contents of the list are stored in
   * shared memory.
   *
   * @return See above.
   */
  static Test_shm_session_server::Object_creation_callback list_creator_functor();
  /**
   * Returns a functor that creates many objects in shared memory.
   *
   * @return See above.
   */
  static Test_shm_session_server::Object_creation_callback many_objects_creator_functor();

private:
  /**
   * Wrapper around an object to store a callback to be executed when the object is released. This is only to
   * be utilized by the owner and not the borrower.
   *
   * @tparam T The object type being wrapped.
   */
  template <typename T, typename... Args>
  class Owner_object_wrapper :
    public T
  {
  public:
    /**
     * Constructor.
     *
     * @param callback The callback to be executed when the destructor is called.
     */
    Owner_object_wrapper(Test_shm_session_server::Object_destructor_callback&& callback, Args&&... args);
    /// Destructor. Executes any registered callback.
    ~Owner_object_wrapper();

  private:
    /// The callback to be executed when the destructor is called.
    Test_shm_session_server::Object_destructor_callback m_destructor_callback;
  }; // class Owner_object_wrapper
}; // class Test_shm_session_server_executor

template <typename T, typename... Args>
Test_shm_session_server_executor::Owner_object_wrapper<T, Args...>::Owner_object_wrapper(
  Test_shm_session_server::Object_destructor_callback&& callback, Args&&... args) :
  T(std::forward<Args>(args)...),
  m_destructor_callback(std::move(callback))
{
}

template <typename T, typename... Args>
Test_shm_session_server_executor::Owner_object_wrapper<T, Args...>::~Owner_object_wrapper()
{
  m_destructor_callback();
}

} // namespace ipc::session::shm::arena_lend::jemalloc::test
