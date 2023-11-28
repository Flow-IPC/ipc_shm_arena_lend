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

#include "ipc/session/standalone/shm/arena_lend/jemalloc/test/test_shm_session_server_executor.hpp"
#include "ipc/shm/arena_lend/jemalloc/ipc_arena.hpp"
#include "ipc/shm/stl/stateless_allocator.hpp"
#include "ipc/test/test_logger.hpp"
#include "ipc/test/test_common_util.hpp"
#include <future>

namespace chrono = std::chrono;
using std::future_status;
using std::make_unique;
using std::promise;
using std::shared_ptr;
using std::size_t;
using std::string;
using std::unique_ptr;
using std::vector;

using ipc::test::Test_logger;
using ipc::shm::arena_lend::jemalloc::Ipc_arena;
using ipc::shm::arena_lend::jemalloc::Ipc_arena_activator;
using ipc::shm::arena_lend::jemalloc::Ipc_arena_allocator;

namespace ipc::session::shm::arena_lend::jemalloc::test
{

template <typename T>
using Owner_object_wrapper = Test_shm_session_server_executor::Owner_object_wrapper<T>;
using Destructor_callback = Test_shm_session_server::Object_destructor_callback;
using Object_creation_callback = Test_shm_session_server::Object_creation_callback;
using Object_type = Test_shm_session_server::Object_type;
using Operation_mode = Test_shm_session_server::Operation_mode;

// Static constants
const chrono::duration<size_t> Test_shm_session_server_executor::S_TEST_TIMEOUT = chrono::seconds(60);
/* ^-- 5 sec is plenty with decent hardware with full optimization, but even 10 is pushing it when built unoptimized.
 * Increasing to 60 to avoid misleading failure. @todo This can all probably be fine-tuned more cleverly with enough
 * effort. */
const chrono::duration<size_t> Test_shm_session_server_executor::S_PERFORMANCE_TEST_TIMEOUT =
  chrono::seconds(10);
const string Test_shm_session_server_executor::S_MESSAGE("Server says hello");

// Static method
Object_creation_callback Test_shm_session_server_executor::char_array_creator_functor()
{
  return
    [](const shared_ptr<Ipc_arena>& shm_arena,
       Destructor_callback&& destructor_callback) -> std::pair<Object_type, shared_ptr<void>>
    {
      auto object = shm_arena->construct<Owner_object_wrapper<Simple_object>>(std::move(destructor_callback));

      /* Avoid gcc-9 (at least) warning by copying up-to N-1 chars and forcibly NUL-terminating just in case.
       * Furthermore some gcc versions/build configs still issue an obscure bounds-checking warning, possibly due to
       * the shared_ptr dereference confusing the optimizer/bounds-checker; so that's why the #pragma.
       * (A glance at the gcc bug database shows this particular set or warnings is not the most robust thing ever
       * and has(d) both reporting bugs and a penchant for paranoia.) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas" // For older versions, where the following does not exist/cannot be disabled.
#pragma GCC diagnostic ignored "-Wunknown-warning-option" // (Similarly for clang.)
#pragma GCC diagnostic ignored "-Warray-bounds"
      std::strncpy(object->m_message, S_MESSAGE.c_str(), sizeof(Simple_object) - 1);
#pragma GCC diagnostic pop
      object->m_message[sizeof(Simple_object) - 1] = '\0';

      return {Object_type::S_ARRAY, object};
    };
}

// Static method
Object_creation_callback Test_shm_session_server_executor::vector_char_creator_functor()
{
  return
    [](const shared_ptr<Ipc_arena>& shm_arena,
       Destructor_callback&& destructor_callback) -> std::pair<Object_type, shared_ptr<void>>
    {
      using Shm_vector = Vector_type<Ipc_arena_allocator>;
      auto v = shm_arena->construct<Owner_object_wrapper<Shm_vector>>(std::move(destructor_callback));

      {
        // Scoped specification of arena to use in the allocator
        Ipc_arena_activator ctx(shm_arena.get());

        // Add more values
        for (size_t i = 0; i < S_MESSAGE.size(); ++i)
        {
          v->push_back(S_MESSAGE[i]);
        }

        return {Object_type::S_VECTOR, v};
      }
    };
}

// Static method
Object_creation_callback Test_shm_session_server_executor::string_creator_functor()
{
  return
    [](const shared_ptr<Ipc_arena>& shm_arena,
       Destructor_callback&& destructor_callback) -> std::pair<Object_type, shared_ptr<void>>
    {
      using Shm_string = String_type<Ipc_arena_allocator>;
      auto s = shm_arena->construct<Owner_object_wrapper<Shm_string,
                                                         const Shm_string::size_type&,
                                                         const char&>>(std::move(destructor_callback),
                                                                       S_STRING_SIZE,
                                                                       S_STRING_CHAR);
      return {Object_type::S_STRING, s};
    };
}

// Static method
Object_creation_callback Test_shm_session_server_executor::list_creator_functor()
{
  return
    [](const shared_ptr<Ipc_arena>& shm_arena,
       Destructor_callback&& destructor_callback) -> std::pair<Object_type, shared_ptr<void>>
    {
      using Shm_list = List_type<Ipc_arena_allocator>;
      auto shm_list = shm_arena->construct<Owner_object_wrapper<Shm_list>>(std::move(destructor_callback));

      {
        // Scoped specification of arena to use in the allocator
        Ipc_arena_activator ctx(shm_arena.get());

        for (size_t i = 0; i < S_LIST_SIZE; ++i)
        {
          auto& cur_entry = shm_list->emplace_back();

          /* Avoid gcc-9 (at least) warning by copying up-to N-1 chars and forcibly NUL-terminating just in case.
           * Furthermore some gcc versions/build configs still issue an obscure bounds-checking warning, possibly due to
           * the shared_ptr dereference confusing the optimizer/bounds-checker; so that's why the #pragma.
           * (A glance at the gcc bug database shows this particular set or warnings is not the most robust thing ever
           * and has(d) both reporting bugs and a penchant for paranoia.) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas" // For older versions, where the following does not exist/cannot be disabled.
#pragma GCC diagnostic ignored "-Wunknown-warning-option" // (Similarly for clang.)
#pragma GCC diagnostic ignored "-Warray-bounds"
          std::strncpy(cur_entry.m_message, S_MESSAGE.c_str(), sizeof(Simple_object) - 1);
#pragma GCC diagnostic pop
          cur_entry.m_message[sizeof(Simple_object) - 1] = '\0';
        }
      }

      return {Object_type::S_LIST, shm_list};
    };
}

// Static method
Object_creation_callback Test_shm_session_server_executor::many_objects_creator_functor()
{
  using Timer = flow::perf::Checkpointing_timer;
  using flow::perf::Clock_type;
  using flow::perf::Clock_types_subset;
  using ipc::test::to_underlying;

  return
    [](const shared_ptr<Ipc_arena>& shm_arena,
       Destructor_callback&& destructor_callback) -> std::pair<Object_type, shared_ptr<void>>
    {
      Test_logger logger(flow::log::Sev::S_TRACE);
      Clock_types_subset clock_types;
      clock_types.set(to_underlying(Clock_type::S_REAL_HI_RES));
      Timer perf_timer(&logger, "Jemalloc allocation perf timer", clock_types, 1);

      using Shm_list = List_type<Ipc_arena_allocator>;
      auto shm_list = shm_arena->construct<Owner_object_wrapper<Shm_list>>(std::move(destructor_callback));

      {
        // Scoped specification of arena to use in the allocator
        Ipc_arena_activator ctx(shm_arena.get());

        for (size_t i = 0; i < S_PERFORMANCE_LIST_SIZE; ++i)
        {
          shm_list->emplace_back();
        }
      }

      perf_timer.checkpoint("Total");

      return {Object_type::S_LIST, shm_list};
    };
}

Test_shm_session_server_executor::Test_shm_session_server_executor(flow::log::Logger* logger) :
  flow::log::Log_context(logger, Log_component::S_TEST)
{
}

Test_shm_session_server_executor::Result Test_shm_session_server_executor::run(
  Object_type object_type, pid_t client_process_id, Operation_mode operation_mode)
{
  promise<bool> server_promise;

  Object_creation_callback object_creation_callback;
  switch (object_type)
  {
    case Object_type::S_ARRAY:
      object_creation_callback = char_array_creator_functor();
      break;

    case Object_type::S_VECTOR:
      object_creation_callback = vector_char_creator_functor();
      break;

    case Object_type::S_STRING:
      object_creation_callback = string_creator_functor();
      break;

    case Object_type::S_LIST:
      object_creation_callback = list_creator_functor();
      break;
  }

  Test_shm_session_server server(get_logger(),
                                 client_process_id,
                                 std::move(object_creation_callback),
                                 [&](bool result) { server_promise.set_value(result); },
                                 operation_mode);
  if (!server.start())
  {
    FLOW_LOG_WARNING("Could not start server");
    return Result::S_FAIL;
  }

  FLOW_LOG_INFO("Server started");

  auto server_future = server_promise.get_future();
  auto server_status = server_future.wait_for(S_TEST_TIMEOUT);
  Result result;
  if (server_status == future_status::ready)
  {
    result = server_future.get() ? Result::S_SUCCESS : Result::S_FAIL;
  }
  else
  {
    result = Result::S_TIMEOUT;
  }

  server.stop();

  return result;
}

} // namespace ipc::session::shm::arena_lend::jemalloc::test
