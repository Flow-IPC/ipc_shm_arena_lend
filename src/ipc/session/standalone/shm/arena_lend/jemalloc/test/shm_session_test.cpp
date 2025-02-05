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

#include <gtest/gtest.h>
#include <unistd.h>
#include <sys/types.h>
#include <future>
#include <optional>
#include "ipc/session/client_session.hpp"
#include "ipc/session/standalone/shm/arena_lend/borrower_shm_pool_collection_repository.hpp"
#include "ipc/session/standalone/shm/arena_lend/jemalloc/shm_session.hpp"
#include "ipc/session/standalone/shm/arena_lend/jemalloc/test/test_shm_session.hpp"
#include "ipc/session/standalone/shm/arena_lend/jemalloc/test/test_shm_session_server_executor.hpp"
#include "ipc/session/standalone/shm/arena_lend/jemalloc/test/test_shm_session_server_launcher.hpp"
#include "ipc/session/standalone/shm/arena_lend/jemalloc/test/test_message.capnp.h"
#include "ipc/session/standalone/shm/arena_lend/jemalloc/test/test_session_metadata.capnp.h"
#include "ipc/shm/arena_lend/borrower_allocator_arena.hpp"
#include "ipc/shm/arena_lend/jemalloc/ipc_arena.hpp"
#include "ipc/shm/arena_lend/shm_pool_repository_singleton.hpp"
#include "ipc/shm/arena_lend/test/test_shm_object.hpp"
#include "ipc/shm/stl/stateless_allocator.hpp"
#include "ipc/test/test_common_util.hpp"
#include "ipc/test/test_logger.hpp"
#include <flow/test/test_common_util.hpp>

namespace chrono = std::chrono;

using std::future;
using std::future_status;
using std::promise;
using std::make_pair;
using std::make_shared;
using std::make_unique;
using std::optional;
using std::ostream;
using std::set;
using std::shared_ptr;
using std::static_pointer_cast;
using std::string;
using std::string_view;
using std::to_string;
using std::unique_ptr;
using std::unordered_map;
using std::vector;
using flow::async::Single_thread_task_loop;

namespace ipc::session::shm::arena_lend::jemalloc::test
{

using Blob = Shm_session::Blob;
using Object_type = Test_shm_session_server::Object_type;
using Server_operation_mode = Test_shm_session_server::Operation_mode;
using Simple_object = Test_shm_session_server_executor::Simple_object;
using pool_id_t = ipc::shm::arena_lend::Shm_pool::pool_id_t;
using pool_offset_t = ipc::shm::arena_lend::Shm_pool::size_t;

using ipc::shm::arena_lend::Borrower_allocator_arena;
using ipc::shm::arena_lend::Shm_pool;
using ipc::shm::arena_lend::jemalloc::Ipc_arena;
using ipc::shm::arena_lend::jemalloc::Memory_manager;

using flow::test::check_output;
using ipc::test::Test_logger;
using flow::test::to_underlying;
using ipc::session::schema::MqType;

using Mutex = std::mutex;
// Exclusive lock for the mutex
using Lock = std::lock_guard<Mutex>;

// Client aliases
using Client_session = ipc::session::Client_session<MqType::NONE, false, TestSessionMetadata>;
using Client_session_mdt_reader = Client_session::Mdt_reader_ptr;
using Client_session_mdt_builder = Client_session::Mdt_builder_ptr;
using Client_channel_base = Client_session::Channel_obj;
using Client_channel = typename Client_session::Structured_channel<TestMessage>;

// Channel aliases
using Shm_channel_base = Shm_session::Shm_channel_base;
using Shm_channel = Shm_session::Shm_channel;

/* @todo Notes from ygoldfel's working with these (incredibly useful) test cases -- as well as other test cases
 * in this suite -- when debugging a major change in how pointers are represented:
 *   - In Shm_session_test: the logging situation is a bit unpleasant at times, when volume is relatively high.
 *     2 processes send output to stdout/stderr, and these 2 streams are interleaved pretty often.
 *     Technically one should not be using 2+ `Simple_ostream_logger`s to one stream simultaenously per its docs;
 *     that's why. The idea of using one timestamp format for 1 versus the other, to distinguish them (if that was
 *     the idea), works pretty well in a pinch, but the interleaving is tough.
 *     - Would suggest -- for the overall test suite -- perhaps using `Async_file_logger`s to files and so on.
 *       By the way another idea would be to use stderr versus stdout; then at least redirection could be used.
 *       That has downsides too.
 *     - Might be nice to output some (e.g. `ipc` objects') logging to files; test result logging to console.
 *       Otherwise it is a chore to understand for some tests. By the way it can be helpful to write a simple
 *       console logger that forwards both to a console Simple_ostream_logger *and* the file logger; so when
 *       it is necessary one can see the two interleaved (in the file log), while normally one just glances
 *       at the console log and sees only a few result/status lines.
 *  - For some tests (actually Shm_session_test, I believe) setting min log level arg does not seem to affect
 *    much of the resulting verbosity; I think this is for those tests that launch helper executables.
 *    One could use `popen()`-type functionality or some other solution.
 *    - For some (other) tests setting the min log level causes them to fail due to certain log lines not appearing
 *      as expected.
 *    - All such issues are understandable; it's just a matter of either working around them to make it nice or
 *      alternatively perhaps clearly warning when a confusing situation might occur. It's just difficult to orient
 *      oneself, for a newbie at least, due to the sheer volume of output. (Segregating test output versus
 *      library output, as suggested above, would help with this too.)
 *
 * I suspect this can all be addressed by looking at logging in the unit test suite holistically and then making
 * some changes having considered everything. Shm_session_test is, I believe, special due to using 2 helper
 * executables; there are some other tests that launch processes too but not in the same way (I think). */

/// Google test fixture. Contains server test information.
class Shm_session_test :
  public ::testing::Test
{
public:
  /// Constructor.
  Shm_session_test() :
    m_logger(flow::log::Sev::S_INFO),
    m_log_component(Log_component::S_TEST)
  {
    // Establish logger prior to execution
    Borrower_shm_pool_collection_repository_singleton::get_instance().set_logger(get_logger());
  }

  /**
   * Retrieves the logger used in log messages, especially utilized with macros.
   *
   * @return See above.
   */
  inline flow::log::Logger* get_logger()
  {
    return &m_logger;
  }

  /**
   * Retrieves the log component used in log messages, especially utilized with macros.
   *
   * @return See above.
   */
  inline const Log_component& get_log_component() const
  {
    return m_log_component;
  }

  /**
   * Waits a maximum time threshold for the server session to indicate that it is finished. If it is not complete
   * within the threshold, an error is emitted and the method returns.
   */
  void wait_for_server_completion(
    const std::chrono::duration<size_t>& wait_duration = Test_shm_session_server_executor::S_TEST_TIMEOUT)
  {
    // Wait for server to complete
    auto server_future = m_server_promise.get_future();
    auto server_status = server_future.wait_for(wait_duration);
    if (server_status == future_status::ready)
    {
      EXPECT_TRUE(server_future.get());
    }
    else
    {
      EXPECT_EQ(server_status, future_status::timeout);
      ADD_FAILURE() << "Server timeout";
    }
  }

  /**
   * Sets the result of the server, if not already set.
   *
   * @param result Whether the server passed.
   */
  void set_server_result(bool result)
  {
    Lock lock(m_server_result_mutex);
    if (m_server_result)
    {
      FLOW_LOG_INFO("Server result already set [" << *m_server_result << "], ignoring new result [" << result << "]");
      return;
    }
    m_server_result = result;
    m_server_promise.set_value(result);
  }

private:
  /// Used for logging to the console.
  Test_logger m_logger;
  /// Used for log message categorization.
  Log_component m_log_component;

  /// Synchronizes access to m_server_result.
  mutable Mutex m_server_result_mutex;
  /// Stores the server's result.
  boost::optional<bool> m_server_result;
  /// Used to wait until the server result is available or a timeout is reached.
  promise<bool> m_server_promise;
}; // class Shm_session_test

namespace
{

/**
 * The amount of time in microseconds that will be delayed to allow the external server process to start up and be
 * in a steady state.
 */
static const chrono::duration S_WAIT_FOR_SERVER_START_DURATION = chrono::milliseconds(1000);
/// Invalid process id used in return values.
static constexpr util::process_id_t S_INVALID_PROCESS_ID = -1;
/// The wait time before retrying the check to observe the test shared memory pool was removed by the client.
static const chrono::duration S_WAIT_FOR_SHM_POOL_REMOVAL_DURATION = chrono::milliseconds(10);
/// The number of times to check (pool) that the test shared memory pool was removed by the client.
static constexpr unsigned int S_MAX_SHM_POOL_REMOVAL_CHECK_RETRIES = 10UL;

/**
 * A sample client application that communicates with a server to obtain an object.
 */
class Test_client :
  public flow::log::Log_context
{
public:
  /// Lowest client id to be used by the test.
  static constexpr unsigned int S_LOWEST_CLIENT_ID = 0UL;

  /**
   * The way the client needs to operate, which basically is a test use case.
   */
  enum class Operation_mode
  {
    /// General operation, which is receive arena, receive shm pool, receive object, object return.
    S_NORMAL = 0,
    /// Performs tasks to measure shared memory object allocation performance.
    S_ALLOCATION_PERFORMANCE
  }; // enum class Operation_mode

  /**
   * Notification handler for test-related events.
   */
  class Event_listener
  {
  public:
    /**
     * Notification executed prior to starting the client channels, which includes the shared memory internal
     * channel and the application channel.
     *
     * @param shm_session The shared memory session.
     */
    virtual void notify_starting_channels(const shared_ptr<Test_shm_session>& shm_session) = 0;
    /**
     * Notification executed when the client successfully receives an object from the server.
     *
     * @param object The object received.
     * @param collection_id The shared memory pool collection where the object resides.
     * @param shm_pool_id The shared memory pool id where the object resides.
     * @param pool_offset The offset within the pool where the object resides.
     */
    virtual void notify_object_received(shared_ptr<void>&& object,
                                        Collection_id collection_id,
                                        pool_id_t shm_pool_id,
                                        pool_offset_t pool_offset) = 0;
    /**
     * Notification executed when the client receives instruction from the server to cleanup.
     */
    virtual void notify_cleanup_received() = 0;
    /**
     * Notification executed when the client result is determined.
     *
     * @param result Whether the expected outcomes were correct in the client.
     */
    virtual void notify_completion(bool result) = 0;
  }; // class Event_listener

  /**
   * Constructor.
   *
   * @param logger Used for logging messages.
   * @param client_id Identifier for a client; it should be unique per test run.
   * @param operation_mode The mode the client operates under.
   */
  Test_client(flow::log::Logger* logger,
              unsigned int client_id = S_LOWEST_CLIENT_ID,
              Operation_mode operation_mode = Operation_mode::S_NORMAL) :
    flow::log::Log_context(logger, Log_component::S_TEST),
    m_client_id(client_id),
    m_started(false),
    m_task_loop(get_logger(), Test_shm_session_server::S_CLIENT_APP_NAME + "_loop_" + to_string(client_id)),
    m_event_listener(nullptr),
    m_session(get_logger(),
              Test_shm_session_server::get_client_app(),
              Test_shm_session_server::get_server_app(),
              [&](const ipc::Error_code& ec)
              {
                if (ec)
                {
                  m_task_loop.post([this, ec]()
                                   {
                                     FLOW_LOG_INFO("Session ended with error [" << ec << "]");
                                     if (m_shm_session != nullptr)
                                     {
                                       m_shm_session->set_disconnected();
                                     }
                                   });
                }
              },
              [&](Shm_channel_base&& channel, Client_session_mdt_reader&& mdt_reader)
              {
                auto channel_ptr = make_shared<Shm_channel_base>(std::move(channel));
                EXPECT_EQ(mdt_reader->getPayload().getType(), TestSessionMetadata::ChannelType::SHM);
                m_task_loop.post([&, shm_channel_base = std::move(channel_ptr)]() mutable
                                 {
                                   handle_shm_channel(std::move(shm_channel_base));
                                 });
              }),
    m_operation_mode(operation_mode)
  {
  }

  /// Destructor.
  ~Test_client()
  {
    m_task_loop.stop();
    /* Thread joined. Now nothing we'd post()ed onto m_task_loop will execute past this line. If allowed to
     * execute we'd introduce a race between destruction of *this members and post()ed code touching those members. For
     * example the error handler given to m_session in ctor above will, from m_task_loop thread, touch
     * m_shm_session: a race between that and m_shm_session being destroyed.
     *
     * TSAN caught this. I (ygoldfel) added that line as a result. @todo It may or may not be the most graceful
     * way of handling it; sometimes properly ordering data members for destruction in the right order = better.
     * Not being the original author of the test I went with smallest safe change I could carry out to eliminate
     * the race. */
  }

  unsigned int get_client_id() const
  {
    return m_client_id;
  }

  /**
   * Sets the event listener, which receives test-related notifications.
   *
   * @param event_listener The event listener to set.
   */
  void set_event_listener(Event_listener* event_listener)
  {
    EXPECT_FALSE(m_started);

    if (m_event_listener != nullptr)
    {
      ADD_FAILURE() << "An event listener was previously set!";
    }
    m_event_listener = event_listener;
  }

  /**
   * If they are available, retrieves the test shared memory pool information.
   *
   * @param shm_pool_id If the pool information is available, the shared memory pool id will be populated here.
   * @param shm_pool_address If the pool information is available, the base address of the shared memory pool will
   *                         be populated here.
   *
   * @return Whether the test shared memory pool information is available.
   */
  bool get_test_shm_pool_data(pool_id_t& shm_pool_id, void*& shm_pool_address) const
  {
    Lock lock(m_test_shm_pool_data_mutex);

    if ((m_test_shm_pool_id == 0) || !m_test_shm_pool_address)
    {
      return false;
    }

    shm_pool_id = m_test_shm_pool_id;
    shm_pool_address = *m_test_shm_pool_address;
    return true;
  }

  /**
   * Checks that the test shared memory pool has been deregistered from the repository.
   *
   * @return Whether the check succeeded.
   */
  bool check_repository_for_test_shm_pool_removal() const
  {
    Lock lock(m_test_shm_pool_data_mutex);

    if (m_test_shm_pool_id == 0)
    {
      ADD_FAILURE() << "SHM pool id not set";
      return false;
    }

    const auto shm_pool_id = m_test_shm_pool_id;

    // If we convert the shared memory pool to the null address, that means the pool doesn't exist
    bool success = !(Borrower_shm_pool_collection_repository_singleton::to_address(shm_pool_id, 0));

    for (unsigned int i = 1; (!success && (i <= S_MAX_SHM_POOL_REMOVAL_CHECK_RETRIES)); ++i)
    {
      size_t wait_us = chrono::duration_cast<chrono::microseconds>(S_WAIT_FOR_SHM_POOL_REMOVAL_DURATION).count();
      FLOW_LOG_INFO("Shared memory pool was not removed, retrying in [" << wait_us << "] us, retry atttempt [" <<
                    i << "] / [" << S_MAX_SHM_POOL_REMOVAL_CHECK_RETRIES << "]");
      usleep(wait_us);

      success = (Borrower_shm_pool_collection_repository_singleton::to_address(shm_pool_id, 0) == nullptr);
    }

    if (success)
    {
      FLOW_LOG_INFO("Test shared memory pool [" << shm_pool_id << "] was removed");
    }
    else
    {
      FLOW_LOG_WARNING("Test shared memory pool [" << shm_pool_id << "] was not removed");
    }

    return success;
  }

  /**
   * Starts the client, namely its task engine.
   */
  void start()
  {
    EXPECT_FALSE(m_started);

    m_started = true;
    m_task_loop.start();
    // Start up client
    run();
  }

  /**
   * Starts the channel that is used for application purposes.
   */
  void start_app_channel()
  {
    EXPECT_TRUE(m_started);
    m_task_loop.post([this]()
                     {
                       if (!open_app_channel())
                       {
                         fail_test();
                         return;
                       }

                       FLOW_LOG_INFO("Client App channel established");
                     });
  }

  /**
   * Sends a message on the application channel indicating to the server that the object has been received.
   */
  void start_object_response()
  {
    EXPECT_TRUE(m_started);
    m_task_loop.post([this]() { send_request(TestMessage::RequestType::RECEIVED); });
  }

  /**
   * Attempts to remove the borrowed shared memory pool as if the owner had sent a message.
   *
   * @param collection_id The identifier for the collection where the object resides.
   * @param shm_pool_id The identifier of the shared memory pool where the object resides.
   *
   * @return Whether the operation was successful.
   *
   * @see Shm_session::receive_shm_pool_removal
   */
  bool simulate_shm_pool_removal_message(Collection_id collection_id, pool_id_t shm_pool_id)
  {
    if (m_shm_session == nullptr)
    {
      ADD_FAILURE() << "Shm session not yet established";
      return false;
    }

    return m_shm_session->receive_shm_pool_removal(collection_id, shm_pool_id);
  }

  /**
   * After a session is established, returns the process id on the other end of the session; otherwise,
   * S_INVALID_PROCESS_ID.
   *
   * @return See above.
   */
  util::process_id_t get_remote_process_id() const
  {
    if (m_shm_session == nullptr)
    {
      ADD_FAILURE() << "Shm session not yet established";
      return S_INVALID_PROCESS_ID;
    }

    return m_shm_session->get_remote_process_id();
  }

  /**
   * Performs test cleanup and completion.
   */
  void start_cleanup()
  {
    EXPECT_TRUE(m_started);

    if (m_operation_mode != Operation_mode::S_ALLOCATION_PERFORMANCE)
    {
      // Ensure the test shared memory pool was removed from the repository
      EXPECT_TRUE(check_repository_for_test_shm_pool_removal());
    }

    m_task_loop.post([this]()
                     {
                       // Send completion message
                       send_request(TestMessage::RequestType::FINISH);

                       // Completed task; if we previously had an error, this will have no effect
                       set_result(true);
                     });
  }

  /**
   * Stops the client, namely its task engine.
   */
  void stop()
  {
    EXPECT_TRUE(m_started);
    m_task_loop.stop();
  }

private:
  /**
   * Triggers the start of the test, which attempts to connect the client with a server.
   */
  void run()
  {
    // Connect client to server
    ipc::Error_code ec;
    if (!m_session.sync_connect(&ec))
    {
      ADD_FAILURE() << "Could not connect";
      fail_test();
      return;
    }
    if (ec)
    {
      ADD_FAILURE() << "Error occurred when connecting, error [" << ec << "]";
      fail_test();
      return;
    }
    m_task_loop.post([this]() { handle_connect(); });

    // Wait for completion
    auto future = m_promise.get_future();
    auto status = future.wait_for(Test_shm_session_server_executor::S_TEST_TIMEOUT);
    if (status == future_status::ready)
    {
      EXPECT_TRUE(future.get());
    }
    else
    {
      EXPECT_EQ(status, future_status::timeout);
      ADD_FAILURE() << "Client timeout";
    }
  }

  /**
   * Handles callback from connection establishment.
   *
   * @param session The client session associated with the connection.
   */
  void handle_connect()
  {
    FLOW_LOG_INFO("Client session established");
  }

  /**
   * Handles callback from client receiving the internal shared memory channel being opened by the server.
   * In particular, the client will start the channel as well and open a separate (application) channel for
   * communication.
   *
   * @param channel_base The unstructured shared memory channel.
   */
  void handle_shm_channel(shared_ptr<Shm_channel_base>&& channel_base)
  {
    // Create structured channel
    m_shm_channel =
      make_unique<Shm_session::Shm_channel>(get_logger(),
                                            std::move(*channel_base),
                                            transport::struc::Channel_base::S_SERIALIZE_VIA_HEAP,
                                            m_session.session_token());

    m_shm_session =
      Test_shm_session::create(get_logger(),
                               Borrower_shm_pool_collection_repository_singleton::get_instance(),
                               *m_shm_channel,
                               [this](const Error_code& ec) { shm_channel_error_handler(ec); });
    if (m_shm_session == nullptr)
    {
      fail_test();
      return;
    }

    if (m_event_listener != nullptr)
    {
      m_event_listener->notify_starting_channels(m_shm_session);
    }

    m_shm_channel->start([this](const Error_code& ec) { shm_channel_error_handler(ec); });

    FLOW_LOG_INFO("Client SHM channel established");

    start_app_channel();
  }

  /**
   * Error handler for the internal channel used to communicate shared memory information for the session.
   *
   * @param ec The error code.
   */
  void shm_channel_error_handler(const Error_code& ec)
  {
    FLOW_LOG_INFO("Client SHM channel encountered close, error [" << ec << "]");
    if (m_shm_session != nullptr)
    {
      m_shm_session->set_disconnected();
    }
    set_result(false);
  }

  /**
   * Opens a channel in the client for application purposes.
   *
   * @return Whether the operation was successful.
   */
  bool open_app_channel()
  {
    if (m_app_channel != nullptr)
    {
      ADD_FAILURE() << "App channel is already used";
      return false;
    }

    auto mdt_builder = m_session.mdt_builder();
    mdt_builder->initPayload().setType(TestSessionMetadata::ChannelType::APP);

    Client_channel_base channel_base;
    ipc::Error_code ec;
    if (!m_session.open_channel(&channel_base, mdt_builder, &ec) || ec)
    {
      ADD_FAILURE() << "Error in opening app channel, error [" << ec << "]";
      return false;
    }

    m_app_channel = make_unique<Client_channel>(get_logger(),
                                                std::move(channel_base),
                                                transport::struc::Channel_base::S_SERIALIZE_VIA_HEAP,
                                                m_session.session_token());

    m_app_channel->expect_msgs(
      TestMessage::Which::RESPONSE_OBJECT,
      [this](const auto& resp)
      {
        const auto& reader = resp->body_root().getResponseObject();

        // Verify object description
        string object_desc = reader.getDescription();
        EXPECT_EQ(object_desc, Test_shm_session_server::S_OBJECT_DESC);

        // Check that the SHM pool is properly registered
        {
          Lock lock(m_test_shm_pool_data_mutex);

          EXPECT_EQ(m_test_shm_pool_id, pool_id_t(0));
          m_test_shm_pool_id = reader.getShmPoolIdToCheck();
          EXPECT_FALSE(m_test_shm_pool_address);
          m_test_shm_pool_address =
            Borrower_shm_pool_collection_repository_singleton::to_address(m_test_shm_pool_id, 0);
          if (*m_test_shm_pool_address == nullptr)
          {
            ADD_FAILURE() << "Could not locate SHM pool [" << m_test_shm_pool_id << "]";
            return;
          }
        }

        // Translate object type
        Object_type object_type;
        switch (reader.getType())
        {
          case TestObjectMessage::ObjectType::CHAR_ARRAY:
            object_type = Object_type::S_ARRAY;
            break;
          case TestObjectMessage::ObjectType::VECTOR_CHAR:
            object_type = Object_type::S_VECTOR;
            break;
          case TestObjectMessage::ObjectType::STRING:
            object_type = Object_type::S_STRING;
            break;
          case TestObjectMessage::ObjectType::LIST:
            object_type = Object_type::S_LIST;
            break;
          default:
            ADD_FAILURE() << "Bad object type [" <<
              to_underlying(reader.getType()) << "]";
            return;
        }

        // Convert to blob
        auto object_reader = reader.getSerializedObject();
        size_t object_size = object_reader.size();
        Blob blob(object_size);
        memcpy(blob.data(), object_reader.begin(), object_size);

        m_task_loop.post([this, object_type, blob, object_desc = std::move(object_desc)]()
                         {
                           handle_object(object_type, blob, object_desc);
                         });
      });
    m_app_channel->expect_msgs(TestMessage::Which::RESPONSE_TYPE,
                               [this](const auto& resp)
                               {
                                 const auto& reader = resp->body_root();
                                 auto response_type = reader.getResponseType();
                                 switch (response_type)
                                 {
                                   case TestMessage::ResponseType::CLEANUP:
                                     m_task_loop.post([this]()
                                                      {
                                                        if (m_event_listener != nullptr)
                                                        {
                                                          m_event_listener->notify_cleanup_received();
                                                        }
                                                      });
                                     break;
                                 }
                               });

    m_app_channel->start([&](const auto& ec)
                         {
                           FLOW_LOG_INFO("Client SHM channel encountered close, error [" << ec << "]");
                         });

    if (m_operation_mode != Operation_mode::S_ALLOCATION_PERFORMANCE)
    {
      send_request(TestMessage::RequestType::START);
    }

    return true;
  }

  /**
   * Handles an incoming shared memory serialized object in the client sent by the server. In particular, the client
   * will deserialize the object and notify any listener about the object.
   *
   * @param object_type The type of object contained in the serialized object.
   * @param serialized_object The serialized object.
   * @param object_desc The description association with the object.
   */
  void handle_object(Object_type object_type, const Blob& serialized_object, const string& object_desc)
  {
    shared_ptr<void> object;

    switch (object_type)
    {
      case Object_type::S_ARRAY:
      {
        // Deserialize object
        auto simple_object = m_shm_session->borrow_object<Simple_object>(serialized_object);
        if (simple_object == nullptr)
        {
          ADD_FAILURE() << "Failed to borrow object with description [" << object_desc << "]";
          break;
        }

        // Make sure we got the expected message
        string_view message = simple_object->m_message;
        const string& EXPECTED_MSG = Test_shm_session_server_executor::S_MESSAGE;
        EXPECT_EQ(EXPECTED_MSG.compare(0, EXPECTED_MSG.size(), message), 0);

        FLOW_LOG_INFO("Got message from server with description [" << object_desc << "], object message [" <<
                      message << "]");

        object = std::move(simple_object);
        break;
      }

      case Object_type::S_VECTOR:
      {
        // Deserialize object
        using Shm_vector = Test_shm_session_server_executor::Vector_type<Borrower_arena_allocator>;
        auto vec = m_shm_session->borrow_object<Shm_vector>(serialized_object);
        if (vec == nullptr)
        {
          ADD_FAILURE() << "Failed to borrow object with description [" << object_desc << "]";
          break;
        }

        const string& EXPECTED_MSG = Test_shm_session_server_executor::S_MESSAGE;
        EXPECT_EQ(vec->size(), EXPECTED_MSG.size());
        EXPECT_TRUE(std::equal(vec->begin(), vec->end(), EXPECTED_MSG.begin()));

        FLOW_LOG_TRACE("Got vector of size [" << vec->size() << "]");

        object = std::move(vec);
        break;
      }

      case Object_type::S_STRING:
      {
        // Deserialize object
        using Shm_string = Test_shm_session_server_executor::String_type<Borrower_arena_allocator>;
        auto str = m_shm_session->borrow_object<Shm_string>(serialized_object);
        if (str == nullptr)
        {
          ADD_FAILURE() << "Failed to borrow object with description [" << object_desc << "]";
          break;
        }

        EXPECT_EQ(str->size(), Test_shm_session_server_executor::S_STRING_SIZE);
        EXPECT_EQ(str->find_first_not_of(Test_shm_session_server_executor::S_STRING_CHAR), str->npos);

        object = std::move(str);
        break;
      }

      case Object_type::S_LIST:
      {
        // Deserialize object
        using Shm_list = Test_shm_session_server_executor::List_type<Borrower_arena_allocator>;
        auto shm_list = m_shm_session->borrow_object<Shm_list>(serialized_object);
        if (shm_list == nullptr)
        {
          ADD_FAILURE() << "Failed to borrow object with description [" << object_desc << "]";
          break;
        }

        EXPECT_EQ(shm_list->size(), Test_shm_session_server_executor::S_LIST_SIZE);
        // Make sure we got the expected messages
        for (const auto& cur_object : *shm_list)
        {
          string_view cur_message = cur_object.m_message;
          const string& EXPECTED_MSG = Test_shm_session_server_executor::S_MESSAGE;
          EXPECT_EQ(EXPECTED_MSG.compare(0, EXPECTED_MSG.size(), cur_message), 0);
        }

        object = std::move(shm_list);
        break;
      }
    }

    Collection_id collection_id = 0;
    pool_id_t shm_pool_id = 0;
    pool_offset_t pool_offset = 0;
    if (!m_shm_session->deserialize_handle(serialized_object, collection_id, shm_pool_id, pool_offset))
    {
      ADD_FAILURE() << "Deserialization failed";
    }
    else if ((m_event_listener != nullptr) && (object != nullptr))
    {
      m_event_listener->notify_object_received(std::move(object), collection_id, shm_pool_id, pool_offset);
    }
  }

  /**
   * Send a message to the server requesting a particular action to be done to advance the test.
   *
   * @param request_type The action to be performed by the server.
   */
  void send_request(TestMessage::RequestType request_type)
  {
    // Construct message
    auto message = m_app_channel->create_msg();
    message.body_root()->setRequestType(request_type);

    ipc::Error_code ec;
    if (!m_app_channel->send(message, nullptr, &ec))
    {
      FLOW_LOG_WARNING("Could not send message, error [" << ec << "]");
      fail_test();
      return;
    }

    FLOW_LOG_INFO("Successfully sent message of type [" << request_type << "]");
  }

  /**
   * Sets the result of the client, if not already set.
   *
   * @param result Whether the client passed.
   */
  void set_result(bool result)
  {
    {
      Lock lock(m_result_mutex);
      if (m_result)
      {
        FLOW_LOG_INFO("Client result already set [" << *m_result << "], ignoring new result [" << result << "]");
        return;
      }
      m_result = result;
      m_promise.set_value(result);
    }

    if (m_event_listener != nullptr)
    {
      m_event_listener->notify_completion(result);
    }
  }

  /**
   * Fails the test by setting the results of both the client and server, if they are not already set.
   */
  void fail_test()
  {
    set_result(false);
  }

private:
  /// Identifier for this client.
  unsigned int m_client_id;
  /// Whether the client has started.
  bool m_started;
  /// The task engine.
  Single_thread_task_loop m_task_loop;
  /// Event listener for test purposes.
  Event_listener* m_event_listener;
  /// The session with the server.
  Client_session m_session;
  /// The mode the client operates under.
  Operation_mode m_operation_mode;

  /// Guards access to the shared memory pool data, namely #m_test_shm_pool_id and #m_test_shm_pool_address.
  mutable Mutex m_test_shm_pool_data_mutex;
  /// Identifier to a shared memory pool to be checked versus the repository.
  pool_id_t m_test_shm_pool_id = 0;
  /// The address of a shared memory pool to be checked against other sessions.
  optional<void*> m_test_shm_pool_address;

  /// Synchronizes access to #m_client_result.
  mutable Mutex m_result_mutex;
  /// Stores the client's result.
  boost::optional<bool> m_result;
  /// Used to wait until the client result is available or a timeout is reached.
  promise<bool> m_promise;

  /// The shared memory channel from the client perspective.
  unique_ptr<Shm_session::Shm_channel> m_shm_channel;
  /// The shared memory session information on the client.
  shared_ptr<Test_shm_session> m_shm_session;
  /// The application channel from the client perspective.
  unique_ptr<Client_channel> m_app_channel;
}; // class Test_client

using Client_operation_mode = Test_client::Operation_mode;

/**
 * Base (abstract) client event listener. Handles common notification actions.
 */
class Basic_event_listener :
  public Test_client::Event_listener
{
public:
  /**
   * Constructor.
   *
   * @param test_harness The test fixture.
   * @param client The client application, which must be valid for the lifetime of this instance.
   */
  Basic_event_listener(Shm_session_test& test_harness, Test_client& client) :
    m_test_harness(test_harness),
    m_client(client)
  {
  }

  /// Default destructor.
  virtual ~Basic_event_listener() = default;

  /**
   * Notification executed prior to starting the client channels, which includes the shared memory internal
   * channel and the application channel.
   *
   * @param shm_session The shared memory session.
   */
  virtual void notify_starting_channels([[maybe_unused]] const shared_ptr<Test_shm_session>& shm_session)
    override
  {
    // Do nothing
  }

  /**
   * Stores object received from the server for later use.
   *
   * @param object The object received.
   * @param collection_id The shared memory pool collection where the object resides.
   * @param shm_pool_id The shared memory pool id where the object resides.
   * @param pool_offset The offset within the pool where the object resides.
   *
   * @see Event_listener::notify_object_received
   */
  virtual void notify_object_received([[maybe_unused]] shared_ptr<void>&& object,
                                      [[maybe_unused]] Collection_id collection_id,
                                      [[maybe_unused]] pool_id_t shm_pool_id,
                                      [[maybe_unused]] pool_offset_t pool_offset) override
  {
    // Unexpected
    ADD_FAILURE() << "Unexpected callback for object received";
  }

  /**
   * Resets any object previously received from the server and executes the client cleanup procedure.
   *
   * @see Event_listener::notify_cleanup_received
   */
  virtual void notify_cleanup_received() override
  {
    m_client.start_cleanup();
  }

  /**
   * Upon negative result, sets the server result to be negative for faster test completion.
   *
   * @param result Whether the client completed successfully.
   */
  virtual void notify_completion(bool result) override
  {
    if (!result)
    {
      m_test_harness.set_server_result(false);
    }
  }

protected:
  /**
   * Returns the client application.
   *
   * @return See above.
   */
  Test_client& get_client()
  {
    return m_client;
  }

  /**
   * Retrieves the logger used in log messages, especially utilized with macros.
   *
   * @return See above.
   */
  inline flow::log::Logger* get_logger()
  {
    return m_test_harness.get_logger();
  }

  /**
   * Retrieves the log component used in log messages, especially utilized with macros.
   *
   * @return See above.
   */
  inline const Log_component& get_log_component() const
  {
    return m_test_harness.get_log_component();
  }

private:
  /// The test fixture.
  Shm_session_test& m_test_harness;
  /// The client application.
  Test_client& m_client;
}; // class Basic_event_listener

/**
 * Client event listener involving object transfer, which is abstract.
 */
class Object_event_listener :
  public Basic_event_listener
{
public:
  /**
   * Returns the object received from the server. If the object has not yet been received or we're at the
   * clean up stage, this will be a nullptr.
   *
   * @return See above.
   */
  shared_ptr<void> get_object() const
  {
    Lock lock(m_object_mutex);
    return m_object;
  }

  /**
   * Returns the object data received from the server. If the object has not yet been received or we're at the
   * clean up stage, the object will be a nullptr.
   *
   * @param object The object to be filled in.
   * @param collection_id The collection id where the object resides to be filled in.
   * @param shm_pool_id The shared memory pool id where the object resides to be filled in.
   * @param pool_offset The offset within the pool where the object resides.
   */
  void get_object_data(shared_ptr<void>& object,
                       Collection_id& collection_id,
                       pool_id_t& shm_pool_id,
                       pool_offset_t& pool_offset) const
  {
    Lock lock(m_object_mutex);

    object = m_object;
    collection_id = m_object_collection_id;
    shm_pool_id = m_object_shm_pool_id;
    pool_offset = m_object_pool_offset;
  }

  /**
   * Stores object received from the server for later use.
   *
   * @param object The object received.
   * @param collection_id The shared memory pool collection where the object resides.
   * @param shm_pool_id The shared memory pool id where the object resides.
   * @param pool_offset The offset within the pool where the object resides.
   *
   * @see Event_listener::notify_object_received
   */
  virtual void notify_object_received(shared_ptr<void>&& object,
                                      Collection_id collection_id,
                                      pool_id_t shm_pool_id,
                                      pool_offset_t pool_offset) override
  {
    EXPECT_NE(object, nullptr);

    Lock lock(m_object_mutex);
    EXPECT_EQ(m_object, nullptr);
    m_object = std::move(object);
    m_object_collection_id = collection_id;
    m_object_shm_pool_id = shm_pool_id;
    m_object_pool_offset = pool_offset;
  }

  /**
   * Resets any object previously received from the server and executes the client cleanup procedure.
   *
   * @see Event_listener::notify_cleanup_received
   */
  virtual void notify_cleanup_received() override
  {
    release_object();
    Basic_event_listener::notify_cleanup_received();
  }

protected:
  /**
   * Constructor.
   *
   * @param test_harness The test fixture.
   * @param client The client application, which must be valid for the lifetime of this instance.
   */
  Object_event_listener(Shm_session_test& test_harness, Test_client& client) :
    Basic_event_listener(test_harness, client),
    m_object_collection_id(0),
    m_object_pool_offset(0)
  {
  }

  /**
   * Releases any object that was received from the server.
   */
  void release_object()
  {
    Lock lock(m_object_mutex);

    EXPECT_EQ(m_object.use_count(), 1);
    EXPECT_NE(m_object, nullptr);
    // Release object, which may send a message to server
    m_object.reset();
  }

private:
  /// Guards against access to the object.
  mutable Mutex m_object_mutex;
  /// The object received from the server.
  shared_ptr<void> m_object;
  /// The shared memory pool collection where the object resides.
  Collection_id m_object_collection_id;
  /// The shared memory pool id where the object resides.
  pool_id_t m_object_shm_pool_id;
  /// The offset within the pool where the object resides.
  pool_offset_t m_object_pool_offset;
}; // class Object_event_listener

/**
 * Event listener used for tests that don't need special handling nor checks.
 */
class Auto_event_listener :
  public Object_event_listener
{
public:
  /**
   * Constructor.
   *
   * @param test_harness The test fixture.
   * @param client The client application.
   */
  Auto_event_listener(Shm_session_test& test_harness, Test_client& client) :
    Object_event_listener(test_harness, client)
  {
  }

  /**
   * Stores the object received and notifies the server that the object has been received.
   *
   * @param object The object received.
   * @param collection_id The shared memory pool collection where the object resides.
   * @param shm_pool_id The shared memory pool id where the object resides.
   * @param pool_offset The offset within the pool where the object resides.
   *
   * @see Event_listener::notify_object_received
   */
  virtual void notify_object_received(shared_ptr<void>&& object,
                                      Collection_id collection_id,
                                      pool_id_t shm_pool_id,
                                      pool_offset_t pool_offset) override
  {
    Object_event_listener::notify_object_received(std::move(object), collection_id, shm_pool_id, pool_offset);
    get_client().start_object_response();
  }
}; // class Auto_event_listener

/**
 * Event listener used for tests that potentially involve more than one client, such that advancement to a stage
 * is synchronized.
 */
class Synchronized_event_listener :
  public Object_event_listener
{
public:
  /// Alias for a functor that is executed after an object is received from the server.
  using Object_received_functor = std::function<void(const shared_ptr<void>& object)>;

  /**
   * Constructor.
   *
   * @param test_harness The test fixture.
   * @param client The client application.
   * @param object_received_functor The functor that is executed after an object is received from the server.
   */
  Synchronized_event_listener(Shm_session_test& test_harness,
                              Test_client& client,
                              Object_received_functor&& object_received_functor) :
    Object_event_listener(test_harness, client),
    m_object_received_functor(std::move(object_received_functor))
  {
  }

  /**
   * Stores the object received and executes a callback.
   *
   * @param object The object received.
   * @param collection_id The shared memory pool collection where the object resides.
   * @param shm_pool_id The shared memory pool id where the object resides.
   * @param pool_offset The offset within the pool where the object resides.
   *
   * @see Event_listener::notify_object_received
   */
  virtual void notify_object_received(shared_ptr<void>&& object,
                                      Collection_id collection_id,
                                      pool_id_t shm_pool_id,
                                      pool_offset_t pool_offset) override
  {
    Object_event_listener::notify_object_received(std::move(object), collection_id, shm_pool_id, pool_offset);
    m_object_received_functor(get_object());
  }

private:
  /// The functor that is executed after an object is received from the server.
  Object_received_functor m_object_received_functor;
}; // class Synchronized_event_listener

/**
 * Event listener used for tests that has client object removal delayed until after the session is disconnected.
 */
class Delayed_object_removal_event_listener :
  public Auto_event_listener
{
public:
  /**
   * Constructor.
   *
   * @param test_harness The test fixture.
   * @param client The client application.
   */
  Delayed_object_removal_event_listener(Shm_session_test& test_harness, Test_client& client) :
    Auto_event_listener(test_harness, client)
  {
  }

  /**
   * Executes the client cleanup procedure.
   *
   * @see Event_listener::notify_cleanup_received
   */
  virtual void notify_cleanup_received() override
  {
    get_client().start_cleanup();
  }

  // Make public
  using Object_event_listener::release_object;
}; // class Delayed_object_removal_event_listener

class Error_handling_event_listener :
  public Auto_event_listener
{
public:
  /**
   * Constructor.
   *
   * @param test_harness The test fixture.
   * @param client The client application.
   * @param error_handling_collection_id The collection id of the arena to register in advance of the server
   *                                     attempting to lend it such that it triggers an error.
   */
  Error_handling_event_listener(Shm_session_test& test_harness,
                                Test_client& client,
                                Collection_id error_handling_collection_id) :
    Auto_event_listener(test_harness, client),
    m_error_handling_collection_id(error_handling_collection_id)
  {
  }

  /**
   * Stores the shared memory session object for later use and registers the error handling collection id.
   *
   * @param shm_session The shared memory session.
   */
  virtual void notify_starting_channels(const shared_ptr<Test_shm_session>& shm_session) override
  {
    if (shm_session->receive_arena(m_error_handling_collection_id))
    {
      FLOW_LOG_INFO("Successfully registered collection id [" << m_error_handling_collection_id << "]");
    }
    else
    {
      FLOW_LOG_WARNING("Error occurred in registering collection id [" << m_error_handling_collection_id << "]");
    }
  }

private:
  /// The collection id of the arena that will be preregistered to cause an error on the lender side.
  const Collection_id m_error_handling_collection_id;
}; // class Error_handling_event_listener

/**
 * Orchestrates the execution of a test involving multiple clients.
 */
class Test_client_manager
{
public:
  /// The default number of clients involved in the test.
  static constexpr size_t S_DEFAULT_NUM_CLIENTS = 3;

  /**
   * Constructor.
   *
   * @param logger The logger to use in the clients.
   */
  Test_client_manager(Shm_session_test& test_harness) :
    m_test_harness(test_harness),
    m_state(State::S_RESET)
  {
  }

  /// Default destructor.
  virtual ~Test_client_manager() = default;

  /**
   * Creates and starts the clients.
   *
   * @param num_clients The number of clients to instantiate, which can have values >= 1.
   * @param operation_mode The mode the clients operate under.
   *
   * @return Whether the clients were started and registered successfully.
   */
  bool start(size_t num_clients = S_DEFAULT_NUM_CLIENTS,
             Client_operation_mode operation_mode = Client_operation_mode::S_NORMAL)
  {
    if (num_clients <= 0)
    {
      ADD_FAILURE() << "Illegal number of clients [" << num_clients << "]";
      return false;
    }

    {
      Lock lock(m_state_mutex);
      if (m_state != State::S_RESET)
      {
        ADD_FAILURE() << "Bad state [" << m_state << "]";
        return false;
      }
      m_state = State::S_START;
    }

    Lock lock(m_client_data_map_mutex);
    for (unsigned int cur_client_id = Test_client::S_LOWEST_CLIENT_ID;
         cur_client_id < (Test_client::S_LOWEST_CLIENT_ID + num_clients);
         ++cur_client_id)
    {
      auto client_ptr = make_unique<Test_client>(get_logger(), cur_client_id, operation_mode);
      auto& client = *client_ptr;
      auto event_listener = create_event_listener(client);
      if (event_listener == nullptr)
      {
        return false;
      }
      client.set_event_listener(event_listener.get());

      if (!m_client_data_map.emplace(
            cur_client_id, make_unique<Client_data>(std::move(client_ptr), std::move(event_listener))).second)
      {
        ADD_FAILURE() << "Could insert Client_data for client [" << cur_client_id << "]";
        {
          Lock lock(m_state_mutex);
          m_state = State::S_ERROR;
        }
        return false;
      }
    }

    // Start clients
    for (auto& cur_pair : m_client_data_map)
    {
      auto& cur_client = cur_pair.second->get_client();
      auto cur_thread = make_unique<std::thread>([&cur_client]() { cur_client.start(); });
      m_threads.emplace_back(std::move(cur_thread));
    }

    return true;
  }

  /**
   * Waits for the clients to complete their execution.
   *
   * @return Whether the call was performed at an appropriate stage (e.g., after starting).
   */
  bool wait_for_completion()
  {
    {
      Lock lock(m_state_mutex);
      if (m_state != State::S_START)
      {
        ADD_FAILURE() << "Bad state [" << m_state << "]";
        return false;
      }
    }

    for (auto& cur_thread : m_threads)
    {
      cur_thread->join();
    }

    {
      Lock lock(m_state_mutex);
      m_state = State::S_COMPLETE;
    }

    return true;
  }

  /**
   * Stops the clients.
   */
  void stop()
  {
    {
      Lock lock(m_state_mutex);
      m_state = State::S_STOP;
    }

    Lock lock(m_client_data_map_mutex);
    for (auto& cur_pair : m_client_data_map)
    {
      auto& cur_client = cur_pair.second->get_client();
      cur_client.stop();
    }
  }

  /**
   * Returns the number of client registered.
   *
   * @return See above.
   */
  size_t get_num_clients() const
  {
    Lock lock(m_client_data_map_mutex);
    return m_client_data_map.size();
  }

  /**
   * If there is a client registered, removes the first client in the map.
   *
   * @return Whether the first client was removed successfully.
   */
  bool pop_client()
  {
    Lock lock(m_client_data_map_mutex);

    auto iter = m_client_data_map.begin();
    if (iter == m_client_data_map.end())
    {
      ADD_FAILURE() << "Client map is empty";
      return false;
    }

    m_client_data_map.erase(iter);
    return true;
  }

  /**
   * Retrieves the test fixture.
   *
   * @return See above.
   */
  Shm_session_test& get_test_harness()
  {
    return m_test_harness;
  }

  /**
   * Retrieves the logger used in log messages.
   *
   * @return See above.
   */
  flow::log::Logger* get_logger()
  {
    return m_test_harness.get_logger();
  }

protected:
  /**
   * Information pertaining to a particular client session.
   */
  class Client_data
  {
  public:
    /**
     * Constructor.
     *
     * @param client The client application.
     * @param event_listener The event listener that is registered to the client.
     */
    Client_data(unique_ptr<Test_client>&& client, unique_ptr<Basic_event_listener>&& event_listener) :
      m_event_listener(std::move(event_listener)),
      m_client(std::move(client))
    {
    }

    /**
     * Returns the client application.
     *
     * @return See above.
     */
    Test_client& get_client() const
    {
      return *m_client;
    }

    /**
     * Returns the event listener registered in the client.
     *
     * @return See above.
     */
    Basic_event_listener& get_event_listener() const
    {
      return *m_event_listener;
    }

  private:
    /// The event listener registered in the client.
    const unique_ptr<Basic_event_listener> m_event_listener;
    /**
     * The client application.
     *
     * I (ygoldfel, not original test author) placed this member after m_event_listener; otherwise TSAN
     * detected a race between some deinit code in F(), where F was m_client->m_task_loop.post(F)ed; F() was
     * touching event listener stuff (`m_event_listener->notify_completion(result);`) that was being destroyed
     * around the same time from main thread. Anyway m_client shutting down its thread first (by being listed
     * second here) avoids that chaos, if only because the thread that would be touching dying stuff simply
     * no longer exists, by the time that stuff begins to die. Other than in that regard (where it appears to
     * be purely positive, it should be not-worse). I only pontificate this text to make clear it's possible
     * I am missing some key subtlety (but do feel I understand enough to make this fix reasonably
     * confidently still).
     */
    const unique_ptr<Test_client> m_client;
  }; // class Client_data

  /// Alias for the map of client id -> Client_data
  using Client_data_map = unordered_map<unsigned int, unique_ptr<Client_data>>;

  /**
   * Create event listener to register with the client.
   *
   * @param client The client to register the event listener with.
   *
   * @return The event listener.
   */
  virtual unique_ptr<Basic_event_listener> create_event_listener(Test_client& client)
  {
    return make_unique<Basic_event_listener>(m_test_harness, client);
  }

  /**
   * Returns the client data map.
   *
   * @return See above.
   */
  Client_data_map& get_client_data_map()
  {
    return m_client_data_map;
  }

  /**
   * Returns the mutex for the client data map.
   *
   * @return See above.
   */
  Mutex& get_client_data_map_mutex()
  {
    return m_client_data_map_mutex;
  }

  /**
   * Returns the first registered client application or nullptr if there is none.
   *
   * @return See above.
   */
  Test_client* get_first_client() const
  {
    Lock lock(m_client_data_map_mutex);

    auto iter = m_client_data_map.begin();
    if (iter == m_client_data_map.end())
    {
      ADD_FAILURE() << "Client map is empty";
      return nullptr;
    }

    return &iter->second->get_client();
  }

  /**
   * Returns the event listener associated with the first registered client application or nullptr is there is none.
   *
   * @return See above.
   */
  Basic_event_listener* get_first_event_listener() const
  {
    Lock lock(m_client_data_map_mutex);

    auto iter = m_client_data_map.begin();
    if (iter == m_client_data_map.end())
    {
      ADD_FAILURE() << "Client map is empty";
      return nullptr;
    }

    return &iter->second->get_event_listener();
  }

private:
  /**
   * Stages of the manager, namely:
   * 1. Reset - Initialization state
   * 2. Start - Manager has been instructed to create the clients and start the test
   * 3. Complete - Clients have completed their tests (pass or fail)
   * 4. Stop - Clients have been stopped
   * 5. Error - An error has been encountered
   */
  enum class State : size_t
  {
    S_RESET = 0,
    S_START,
    S_COMPLETE,
    S_STOP,
    S_ERROR
  }; // enum class State

  /**
   * Outputs a textual representation of a State.
   *
   * @param os The stream to output to.
   * @param state The state to convert to a textual representation.
   *
   * @return The parameter "os".
   */
  friend ostream& operator<<(ostream& os, State state)
  {
    switch (state)
    {
      case State::S_RESET:
        os << "Reset";
        break;
      case State::S_START:
        os << "Start";
        break;
      case State::S_COMPLETE:
        os << "Complete";
        break;
      case State::S_STOP:
        os << "Stop";
        break;
      case State::S_ERROR:
        os << "Error";
        break;
    }

    return os;
  }

  /// The test fixture.
  Shm_session_test& m_test_harness;
  /// Provides exclusive access to #m_state.
  mutable Mutex m_state_mutex;
  /// The current state for tracking stage progression.
  State m_state;
  /// Provides exclusive access to #m_client_data_map.
  mutable Mutex m_client_data_map_mutex;
  /// Maps client id to client data.
  Client_data_map m_client_data_map;
  /// Threads to perform concurrent operations for each client session.
  vector<unique_ptr<std::thread>> m_threads;
}; // class Test_client_manager

/**
 * Orchestrates the execution of a test involving multiple clients. In particular, it will conduct synchronization
 * after the object is received from each client.
 */
class Test_object_client_manager :
  public Test_client_manager
{
public:
  /**
   * Constructor.
   *
   * @param test_harness The test fixture.
   */
  Test_object_client_manager(Shm_session_test& test_harness) :
    Test_client_manager(test_harness)
  {
  }

  /**
   * Retrieves whether the check for shared memory pool and object addresses succeeded.
   *
   * @return See above.
   */
  bool get_validation_status() const
  {
    Lock lock(m_validation_status_mutex);

    if (!m_validation_status)
    {
      ADD_FAILURE() << "Validation status not set";
      return false;
    }
    return *m_validation_status;
  }

  /**
   * Validates that the test shared memory pool is not in the shared memory pool repository.
   *
   * @return Whether validation succeeded.
   */
  bool check_repository_for_test_shm_pool_removal() const
  {
    // Only use the first client for checking, because if the others didn't match, there would be a failure elsewhere
    Test_client* client = get_first_client();
    if (client == nullptr)
    {
      return false;
    }

    return client->check_repository_for_test_shm_pool_removal();
  }

  /**
   * Returns the object data received from the server. If the object has not yet been received or we're at the
   * clean up stage, the object will be a nullptr.
   *
   * @param object The object to be filled in.
   * @param collection_id The collection id where the object resides to be filled in.
   * @param shm_pool_id The shared memory pool id where the object resides to be filled in.
   *
   * @return Whether the object data was found.
   */
  bool get_object_data(shared_ptr<void>& object,
                       Collection_id& collection_id,
                       pool_id_t& shm_pool_id,
                       pool_offset_t& pool_offset) const
  {
    // Only use the first client for checking, because if the others didn't match, there would be a failure elsewhere
    const Basic_event_listener* listener = get_first_event_listener();
    if (listener == nullptr)
    {
      return false;
    }

    const auto* object_listener = static_cast<const Object_event_listener*>(listener);
    object_listener->get_object_data(object, collection_id, shm_pool_id, pool_offset);
    return true;
  }

  /**
   * For the first client, after a session is established, returns the process id on the other end of the session;
   * otherwise, S_INVALID_PROCESS_ID.
   *
   * @return See above.
   */
  util::process_id_t get_remote_process_id() const
  {
    // Only use the first client for checking, because if the others didn't match, there would be a failure elsewhere
    Test_client* client = get_first_client();
    if (client == nullptr)
    {
      return -1;
    }

    return client->get_remote_process_id();
  }

protected:
  /**
   * Perform per client initialization by creating a listener and registering it with the client.
   *
   * @param client The client to perform initialization with.
   *
   * @return Whether initialization was successful.
   */
  virtual unique_ptr<Basic_event_listener> create_event_listener(Test_client& client) override
  {
    unsigned int client_id = client.get_client_id();
    return
      make_unique<Synchronized_event_listener>(
        get_test_harness(),
        client,
        [this, client_id](const shared_ptr<void>& object)
        {
          Lock lock(get_client_data_map_mutex());
          {
            Lock lock(m_validation_status_mutex);

            // This may be previously set due to race condition of waiting for lock and already setting object
            if (m_validation_status.has_value())
            {
              // Already validated
              return;
            }
          }

          auto& client_data_map = get_client_data_map();
          auto iter = client_data_map.find(client_id);
          if (iter == client_data_map.end())
          {
            ADD_FAILURE() << "Could not find client data corresponding to id [" << client_id << "]";
            return;
          }

          auto& listener = static_cast<Object_event_listener&>(iter->second->get_event_listener());
          EXPECT_EQ(listener.get_object(), object);
          EXPECT_NE(object, nullptr);
          auto& client = iter->second->get_client();

          pool_id_t expected_shm_pool_id = {};
          void* expected_shm_pool_address = {};
          if (!client.get_test_shm_pool_data(expected_shm_pool_id, expected_shm_pool_address))
          {
            ADD_FAILURE() << "Received object but shm pool id and address are not set for client [" <<
              client_id << "]";
            return;
          }

          // Compare object and shared memory pool information with all other clients
          for (auto& cur_pair : client_data_map)
          {
            auto cur_client_id = cur_pair.first;
            if (cur_client_id == client_id)
            {
              // Don't compare same client
              continue;
            }

            // Compare objects
            auto& cur_listener = static_cast<Object_event_listener&>(cur_pair.second->get_event_listener());
            const auto& cur_object = cur_listener.get_object();
            if (cur_object == nullptr)
            {
              // Session has not yet received object, so try again later
              return;
            }
            if (cur_object != object)
            {
              ADD_FAILURE() << "Object for client [" << client_id << "] does match client [" << cur_client_id << "]";
              return;
            }

            // Compare shared memory pool information
            pool_id_t cur_shm_pool_id = {};
            void* cur_shm_pool_address = {};
            if (!cur_pair.second->get_client().get_test_shm_pool_data(cur_shm_pool_id, cur_shm_pool_address))
            {
              ADD_FAILURE() << "Received object but shm pool id and address are not set for client [" <<
                client_id << "]";
              return;
            }
            if (cur_shm_pool_id != expected_shm_pool_id)
            {
              ADD_FAILURE() << "Shm pool id [" << expected_shm_pool_id << "] for client [" << client_id <<
                "] does match shm pool id [" << cur_shm_pool_id << "] for client [" << cur_client_id << "]";
              return;
            }
            if (cur_shm_pool_address != expected_shm_pool_address)
            {
              ADD_FAILURE() << "Shm pool address [" << expected_shm_pool_address << "] for client [" << client_id <<
                "] does match shm pool address [" << cur_shm_pool_address << "] for client [" << cur_client_id << "]";
              return;
            }
          }

          {
            Lock lock(m_validation_status_mutex);
            assert(!m_validation_status.has_value());
            m_validation_status = true;
          }

          // Send notification to server that object was received by all sessions
          for (auto& cur_pair : client_data_map)
          {
            auto& cur_client = cur_pair.second->get_client();
            cur_client.start_object_response();
          }
        });
  }

private:
  /// Provides exclusive access to #m_validation_status.
  mutable Mutex m_validation_status_mutex;
  /// When set, whether validation of the shared memory pool or object succeeded.
  optional<bool> m_validation_status;
}; // class Test_object_client_manager

/**
 * Executes a series of tests by starting up a client, waiting for the server to complete, and waiting for the
 * client to finish.
 *
 * After the clients are cleaned up, we make sure that the global shared memory pool collection repository
 * contains proper data in that the borrowed data from the server is no longer registered.
 *
 * @param test_harness The outer scope of the tests.
 * @param server The server object or nullptr if the server runs in an external process.
 */
void execute_general_tests(Shm_session_test& test_harness,
                           unique_ptr<Test_shm_session_server>& server)
{
  Owner_id object_owner_id = {};
  Collection_id object_collection_id = {};
  pool_id_t object_shm_pool_id = {};
  pool_offset_t object_pool_offset = {};
  {
    Test_client client(test_harness.get_logger());
    Auto_event_listener event_listener(test_harness, client);
    client.set_event_listener(&event_listener);

    // Start client and execute test
    client.start();
    test_harness.wait_for_server_completion();

    {
      // The generic object will have been reset as we are testing object return
      shared_ptr<void> generic_object;
      event_listener.get_object_data(generic_object, object_collection_id, object_shm_pool_id, object_pool_offset);
      EXPECT_EQ(generic_object, nullptr);
      // Shared memory pool should most likely still be registered
      EXPECT_NE(Borrower_shm_pool_collection_repository_singleton::to_address(object_shm_pool_id, object_pool_offset),
                nullptr);

      object_owner_id = client.get_remote_process_id();
      EXPECT_NE(object_owner_id, S_INVALID_PROCESS_ID);
    }

    client.stop();
  }

  // Stop and destroy server
  if (server != nullptr)
  {
    server->stop();
    server.reset();
  }

  // Check that all borrowed entities have been deregistered
  auto& collection_repository = Borrower_shm_pool_collection_repository_singleton::get_instance();
  EXPECT_EQ(collection_repository.to_address(object_shm_pool_id, object_pool_offset), nullptr);
  EXPECT_FALSE(collection_repository.deregister_shm_pool(object_owner_id, object_collection_id, object_shm_pool_id));
  EXPECT_FALSE(collection_repository.deregister_collection(object_owner_id, object_collection_id));
  EXPECT_FALSE(collection_repository.deregister_owner(object_owner_id));
}

/**
 * This is similar to the two parameter variant but for an external server.
 *
 * @param test_harness The outer scope of the tests.
 *
 * @see execute_general_tests(Shm_session_test&, unique_ptr<Test_shm_session_server>&)
 */
void execute_general_tests(Shm_session_test& test_harness)
{
  unique_ptr<Test_shm_session_server> empty_server;
  execute_general_tests(test_harness, empty_server);
}

/**
 * Executes a series of tests by starting multiple clients, waiting for the server to complete, and waiting
 * for the clients to finish.
 *
 * After the clients are cleaned up, we make sure that the global shared memory pool collection repository
 * contains proper data in that the borrowed data from the server is no longer registered.
 *
 * @param test_harness The outer scope of the tests.
 * @param server The server object or nullptr if the server runs in an external process.
 */
void execute_multisession_tests(Shm_session_test& test_harness,
                                unique_ptr<Test_shm_session_server>& server)
{
  Owner_id object_owner_id = {};
  Collection_id object_collection_id = {};
  pool_id_t object_shm_pool_id = {};
  pool_offset_t object_pool_offset = {};
  {
    Test_object_client_manager client_manager(test_harness);

    // Start clients
    EXPECT_TRUE(client_manager.start());
    // Wait for clients to finish
    EXPECT_TRUE(client_manager.wait_for_completion());
    // Ensure client checks passed
    EXPECT_TRUE(client_manager.get_validation_status());
    // Wait for server to finish
    test_harness.wait_for_server_completion();

    {
      // The generic object will have been reset as we are testing object return
      shared_ptr<void> generic_object;
      EXPECT_TRUE(client_manager.get_object_data(generic_object,
                                                 object_collection_id,
                                                 object_shm_pool_id,
                                                 object_pool_offset));
      EXPECT_EQ(generic_object, nullptr);
      // Shared memory pool backing object should most likely still be registered
      EXPECT_NE(Borrower_shm_pool_collection_repository_singleton::to_address(object_shm_pool_id, object_pool_offset),
                nullptr);

      object_owner_id = client_manager.get_remote_process_id();
      EXPECT_NE(object_owner_id, S_INVALID_PROCESS_ID);
    }

    // Stop clients
    client_manager.stop();
    // Stop and destroy server
    if (server != nullptr)
    {
      server->stop();
      server.reset();
    }

    // Remove all but one client
    while (client_manager.get_num_clients() > 1)
    {
      if (!client_manager.pop_client())
      {
        break;
      }
    }

    // Shared memory pool should still be registered
    EXPECT_NE(Borrower_shm_pool_collection_repository_singleton::to_address(object_shm_pool_id, object_pool_offset),
              nullptr);
  }

  // Check that all borrowed entities have been deregistered
  auto& collection_repository = Borrower_shm_pool_collection_repository_singleton::get_instance();
  EXPECT_EQ(collection_repository.to_address(object_shm_pool_id, object_pool_offset), nullptr);
  EXPECT_FALSE(collection_repository.deregister_shm_pool(object_owner_id, object_collection_id, object_shm_pool_id));
  EXPECT_FALSE(collection_repository.deregister_collection(object_owner_id, object_collection_id));
  EXPECT_FALSE(collection_repository.deregister_owner(object_owner_id));
}

/**
 * This is similar to the two parameter variant but for an external server.
 *
 * @param test_harness The outer scope of the tests.
 *
 * @see execute_multisession_tests(Shm_session_test&, unique_ptr<Test_shm_session_server>&)
 */
void execute_multisession_tests(Shm_session_test& test_harness)
{
  unique_ptr<Test_shm_session_server> empty_server;
  execute_multisession_tests(test_harness, empty_server);
}

/**
 * Executes a series of tests that include the server lending an object to a client, but the client does
 * not return the object back to the server before the server exits. We then attempt to remove the shared
 * memory pool that backs the object but it is still registered, so it should fail. After releasing all
 * the handles to the object, we ensure that the object does not trigger communication to the server as
 * the session is disconnected. Lastly, the attempt to remove the shared memory pool should succeed.
 *
 * After the client is cleaned up, we make sure that the global shared memory pool collection repository
 * contains proper data in that the borrowed data from the server is no longer registered.
 *
 * @param test_harness The outer scope of the tests.
 * @param server The server object or nullptr if the server runs in an external process.
 */
void execute_disconnect_tests(Shm_session_test& test_harness,
                              unique_ptr<Test_shm_session_server>& server)
{
  Owner_id object_owner_id = {};
  Collection_id object_collection_id = {};
  pool_id_t object_shm_pool_id = {};
  pool_offset_t object_pool_offset = {};
  {
    Test_client client(test_harness.get_logger());
    Delayed_object_removal_event_listener event_listener(test_harness, client);
    client.set_event_listener(&event_listener);

    // Start client and execute test
    client.start();
    // Wait for server to finish
    test_harness.wait_for_server_completion();

    shared_ptr<Simple_object> object;
    {
      shared_ptr<void> generic_object;
      event_listener.get_object_data(generic_object, object_collection_id, object_shm_pool_id, object_pool_offset);
      object = static_pointer_cast<Simple_object>(generic_object);
    }

    if (object != nullptr)
    {
      // The object should be the same as the address in the repository
      EXPECT_EQ(Borrower_shm_pool_collection_repository_singleton::to_address(object_shm_pool_id, object_pool_offset),
                object.get());
    }
    else
    {
      ADD_FAILURE() << "Object is nullptr";
    }

    object_owner_id = client.get_remote_process_id();
    EXPECT_NE(object_owner_id, S_INVALID_PROCESS_ID);

    // The shared memory pool backing the object should not be removed as the object still exists
    bool return_value;
    EXPECT_TRUE(
      check_output([&]()
                   {
                     return_value = client.simulate_shm_pool_removal_message(object_collection_id, object_shm_pool_id);
                   },
                   std::cerr,
                   "Could not deregister non-empty SHM pool"));
    EXPECT_FALSE(return_value);

    // For external server, the server already exited, so we don't perform this check as the object's memory will
    // be zeroed (see below)
    if ((server != nullptr) && (object != nullptr))
    {
      // Object should still be intact
      string_view message = object->m_message;
      EXPECT_EQ(message, Test_shm_session_server_executor::S_MESSAGE);
    }

    // Stop and destroy server
    if (server != nullptr)
    {
      server->stop();
      server.reset();
    }

    if (object != nullptr)
    {
      // Object is zeroed out due to shared memory pool removal (purging the pools) by server
      string_view message = object->m_message;
      EXPECT_EQ(message, string(message.size(), '\0'));
    }

    // Check release of object does not send message to server; match prefix of message in
    // Shm_session::return_object()
    object.reset();
    // Configure logger for log output checks
    *flow::log::Config::this_thread_verbosity_override() = flow::log::Sev::S_TRACE;
    EXPECT_TRUE(check_output([&]() { event_listener.release_object(); },
                             std::cout,
                             Test_shm_session_server::S_SESSION_DISCONNECTION_PHRASE));
    // Reset log severity override
    *flow::log::Config::this_thread_verbosity_override() = flow::log::Sev::S_END_SENTINEL;

    // Ensure that the object's shared memory pool can be removed now
    EXPECT_TRUE((object == nullptr) ||
                client.simulate_shm_pool_removal_message(object_collection_id, object_shm_pool_id)) <<
      "Shared memory pool removal yielded unexpected failure after object was released";

    client.stop();
  }

  // Check that all borrowed entities have been deregistered
  auto& collection_repository = Borrower_shm_pool_collection_repository_singleton::get_instance();
  EXPECT_EQ(collection_repository.to_address(object_shm_pool_id, object_pool_offset), nullptr);
  EXPECT_FALSE(collection_repository.deregister_shm_pool(object_owner_id, object_collection_id, object_shm_pool_id));
  EXPECT_FALSE(collection_repository.deregister_collection(object_owner_id, object_collection_id));
  EXPECT_FALSE(collection_repository.deregister_owner(object_owner_id));
}

/**
 * This is similar to the two parameter variant but for an external server.
 *
 * @param test_harness The outer scope of the tests.
 *
 * @see execute_disconnect_tests(Shm_session_test&, unique_ptr<Test_shm_session_server>&)
 */
void execute_disconnect_tests(Shm_session_test& test_harness)
{
  unique_ptr<Test_shm_session_server> empty_server;
  execute_disconnect_tests(test_harness, empty_server);
}

} // Anonymous namespace

/**
 * Demonstrates that functionality of Shm_session works properly. This includes ensuring that the following
 * are correctly implemented:
 * 1. Arena registration
 * 2. Notifications of shared memory pool changes are received from memory manager
 * 3. Messaging of shared memory information from owner, including arena and shared memory pools
 * 4. Object serialization and deserialization
 * 5. Object registration and deregistration
 * 6. Borrowed object release results in object return to lender
 * 7. Shared memory pool repository is correctly utilized
 *
 * The sequence of the test is the following:
 * 1. Client connects with the server to establish a new session
 * 2. Server creates a new channel for internal SHM information
 * 3. Client opens a new channel for application communication
 * 4. Client sends a request to the server to obtain an object
 * 5. Server received request and sends object back to client
 * 6. Client retrieves a message from the server containing a serialized object and a shared memory pool id
 * 7. Client checks test shared memory pool id versus repository to make sure it is registered
 * 8. Client converts the serialized object into an object and compares versus expected
 * 9. Client notifies server of object received
 * 10. Server instructs client to perform cleanup
 * 11. Client releases object (i.e., handles reach zero., which will internally notify server of object return
 * 12. Client sends a notification to server indicating test completion
 * 13. Server receives test completion notification and ensures the object was removed successfully
 * 14. Client checks that the test shared memory pool was deregistered from the repository
 * 15. Client checks that the object's shared memory pool was properly registered in the repository
 * 16. Server and client are destroyed
 * 17. Client checks that the object's shared memory pool was properly deregistered from the repository
 *
 * This test has the client and server in the same process as the unit test execution.
 */
TEST_F(Shm_session_test, In_process_array)
{
  auto server = make_unique<Test_shm_session_server>(
    get_logger(),
    ipc::test::get_process_creds().process_id(),
    Test_shm_session_server_executor::char_array_creator_functor(),
    [&](bool result) { set_server_result(result); });
  if (!server->start())
  {
    ADD_FAILURE() << "Could not start server";
    return;
  }

  execute_general_tests(*this, server);
}

/**
 * See "In_process_array" test above. This test has the client in the same process as the unit test execution and
 * the server in a separate process.
 */
TEST_F(Shm_session_test, External_process_array)
{
  Test_shm_session_server_launcher launcher(get_logger());
  launcher.async_run(Object_type::S_ARRAY,
                     [&](Test_shm_session_server_launcher::Result result)
                     {
                       set_server_result(result == Test_shm_session_server_launcher::Result::S_SUCCESS);
                     });

  // Delay execution of client so that server has sufficient cycles to start up
  usleep(chrono::duration_cast<chrono::microseconds>(S_WAIT_FOR_SERVER_START_DURATION).count());

  execute_general_tests(*this);
}

/**
 * See "In_process_array" test above.  This test has the client receiving an object with offset pointer handles,
 * meaning that the object is stored in shared memory.
 */
TEST_F(Shm_session_test, In_process_vector_offset_ptr)
{
  auto server = make_unique<Test_shm_session_server>(
    get_logger(),
    ipc::test::get_process_creds().process_id(),
    Test_shm_session_server_executor::vector_char_creator_functor(),
    [&](bool result) { set_server_result(result); });
  if (!server->start())
  {
    ADD_FAILURE() << "Could not start server";
    return;
  }

  execute_general_tests(*this, server);
}

/**
 * See "In_process_vector_offset_ptr" test above. This test has the client in the same process as the unit test
 * execution and the server in a separate process.
 */
TEST_F(Shm_session_test, External_process_vector_offset_ptr)
{
  Test_shm_session_server_launcher launcher(get_logger());
  launcher.async_run(Object_type::S_VECTOR,
                     [&](Test_shm_session_server_launcher::Result result)
                     {
                       set_server_result(result == Test_shm_session_server_launcher::Result::S_SUCCESS);
                     });

  // Delay execution of client so that server has sufficient cycles to start up
  usleep(chrono::duration_cast<chrono::microseconds>(S_WAIT_FOR_SERVER_START_DURATION).count());

  execute_general_tests(*this);
}

/**
 * See "In_process_array" test above.  This test has the client receiving a large object with offset pointer handles,
 * meaning that the object is stored in shared memory.
 */
TEST_F(Shm_session_test, In_process_string_offset_ptr)
{
  auto server = make_unique<Test_shm_session_server>(
    get_logger(),
    ipc::test::get_process_creds().process_id(),
    Test_shm_session_server_executor::string_creator_functor(),
    [&](bool result) { set_server_result(result); });
  if (!server->start())
  {
    ADD_FAILURE() << "Could not start server";
    return;
  }

  execute_general_tests(*this, server);
}

/**
 * See "In_process_string_offset_ptr" test above. This test has the client in the same process as the unit test
 * execution and the server in a separate process.
 */
TEST_F(Shm_session_test, External_process_string_offset_ptr)
{
  Test_shm_session_server_launcher launcher(get_logger());
  launcher.async_run(Object_type::S_STRING,
                     [&](Test_shm_session_server_launcher::Result result)
                     {
                       set_server_result(result == Test_shm_session_server_launcher::Result::S_SUCCESS);
                     });

  // Delay execution of client so that server has sufficient cycles to start up
  usleep(chrono::duration_cast<chrono::microseconds>(S_WAIT_FOR_SERVER_START_DURATION).count());

  execute_general_tests(*this);
}

/**
 * See "In_process_array" test above.  This test has the client receiving an object containing offset pointer
 * handles to fixed sized structures. The object and the structures are stored in shared memory.
 */
TEST_F(Shm_session_test, In_process_list_offset_ptr)
{
  auto server = make_unique<Test_shm_session_server>(
    get_logger(),
    ipc::test::get_process_creds().process_id(),
    Test_shm_session_server_executor::list_creator_functor(),
    [&](bool result) { set_server_result(result); });
  if (!server->start())
  {
    ADD_FAILURE() << "Could not start server";
    return;
  }

  execute_general_tests(*this, server);
}

/**
 * See "In_process_list_offset_ptr" test above. This test has the client in the same process as the unit test
 * execution and the server in a separate process.
 */
TEST_F(Shm_session_test, External_process_list_offset_ptr)
{
  Test_shm_session_server_launcher launcher(get_logger());
  launcher.async_run(Object_type::S_LIST,
                     [&](Test_shm_session_server_launcher::Result result)
                     {
                       set_server_result(result == Test_shm_session_server_launcher::Result::S_SUCCESS);
                     });

  // Delay execution of client so that server has sufficient cycles to start up
  usleep(chrono::duration_cast<chrono::microseconds>(S_WAIT_FOR_SERVER_START_DURATION).count());

  execute_general_tests(*this);
}

/**
 * This test has multiple client sessions concurrently communicating with one server to obtain a similar object.
 * It illustrates that the object is similar among all sessions and the underlying shared memory pool is similar.
 * The sequence of the test is similar to the "In_process_offset_ptr" test above, but with the following changes:
 * 1. Run multiple clients concurrently
 * 2. Synchronize clients when their objects are received (i.e., clients wait for each other to get their object)
 * 3. Ensure that the shared memory pool and object received by each client has the same address
 * 4. Concurrently instruct clients to send message to server indicating that object was received and complete the
 *    test
 * 5. Ensure that the (borrowed) shared memory pool repository contains the communicated shared memory pools until
 *    the client is destroyed.
 *
 * This test has the clients and server in the same process as the unit test execution.
 */
TEST_F(Shm_session_test, Multisession_in_process)
{
  auto server = make_unique<Test_shm_session_server>(
    get_logger(),
    ipc::test::get_process_creds().process_id(),
    Test_shm_session_server_executor::vector_char_creator_functor(),
    [&](bool result) { set_server_result(result); });

  // Start server
  if (!server->start())
  {
    ADD_FAILURE() << "Could not start server";
    return;
  }

  execute_multisession_tests(*this, server);
}

/**
 * See "Multisession_in_process" test above.  This test has clients in the same process as the unit test
 * execution and the server in a separate process.
 */
TEST_F(Shm_session_test, Multisession_external_process)
{
  Test_shm_session_server_launcher launcher(get_logger());
  launcher.async_run(Object_type::S_VECTOR,
                     [&](Test_shm_session_server_launcher::Result result)
                     {
                       set_server_result(result == Test_shm_session_server_launcher::Result::S_SUCCESS);
                     });
  // Delay execution of client so that server has sufficient cycles to start up
  usleep(chrono::duration_cast<chrono::microseconds>(S_WAIT_FOR_SERVER_START_DURATION).count());

  execute_multisession_tests(*this);
}

/**
 * See "In_process_array" test above.  This test has the following differences:
 * 1. The server simulates session disconnection with the client and attempts to lend shared memory data (i.e.,
 *    arena, shared memory pool, shared memory object) as well as remove a shared memory pool after disconnection.
 * 2. The server is destroyed before the client.
 * 3. The client attempts to remove a borrowed shared memory pool while an object that is backed by it is still
 *    registered.
 * 4. The client does not release the borrowed object until after the server is destroyed and attempts to return
 *    the object back to the owner.
 * 5. The client removes a borrowed shared memory pool after a borrowed object that is backed by it is deregistered.
 */
TEST_F(Shm_session_test, Disconnected_in_process)
{
  auto server = make_unique<Test_shm_session_server>(
    get_logger(),
    ipc::test::get_process_creds().process_id(),
    Test_shm_session_server_executor::char_array_creator_functor(),
    [&](bool result) { set_server_result(result); },
    Server_operation_mode::S_DISCONNECT);
  if (!server->start())
  {
    ADD_FAILURE() << "Could not start server";
    return;
  }

  execute_disconnect_tests(*this, server);
}

/**
 * See "Disconnected_in_process" test above. This test has the client in the same process as the unit test execution
 * and the server in a separate process.
 */
TEST_F(Shm_session_test, Disconnected_external_process)
{
  // Configure logger for log output checks
  auto* so_logger = static_cast<Test_logger*>(get_logger());
  if (so_logger != nullptr)
  {
    so_logger->get_config().configure_default_verbosity(flow::log::Sev::S_TRACE, false);
  }

  Test_shm_session_server_launcher launcher(get_logger());
  launcher.async_run(Object_type::S_ARRAY,
                     [&](Test_shm_session_server_launcher::Result result)
                     {
                       set_server_result(result == Test_shm_session_server_launcher::Result::S_SUCCESS);
                     },
                     Server_operation_mode::S_DISCONNECT);

  // Delay execution of client so that server has sufficient cycles to start up
  usleep(chrono::duration_cast<chrono::microseconds>(S_WAIT_FOR_SERVER_START_DURATION).count());

  execute_disconnect_tests(*this);
}

/**
 * Tests error handling situations.
 */
TEST_F(Shm_session_test, Error_handling)
{
  auto server = make_unique<Test_shm_session_server>(
    get_logger(),
    ipc::test::get_process_creds().process_id(),
    Test_shm_session_server_executor::char_array_creator_functor(),
    [&](bool result) { set_server_result(result); },
    Server_operation_mode::S_ERROR_HANDLING);
  if (!server->start())
  {
    ADD_FAILURE() << "Could not start server";
    return;
  }

  {
    Test_client client(get_logger());
    Error_handling_event_listener event_listener(*this, client, server->get_error_handling_collection_id());
    client.set_event_listener(&event_listener);

    // Start client and execute test
    client.start();
    wait_for_server_completion();

    client.stop(); // Stop background processing before destroying event_listener, then client.
    /* Without the previous line TSAN detected `client` was invoking stuff in its m_task_loop thread including
     * set_result() which was accessing its m_event_listener -- while here that same guy `event_listener` was
     * being destroyed (before `client` dtor). I (ygoldfel, not original test author) chose client.stop() simply
     * as a way to ensure that doesn't happen. There's still arguably the issue of what should have the longer
     * lifetime; or maybe there should be a reset_event_listener(); or... it's just test code, so it's not a major
     * problem. Anyway at the moment we're fine. */
  }
}

/// Allocation_performance testing with zero clients.
TEST_F(Shm_session_test, Allocation_performance_zero)
{
  auto server = make_unique<Test_shm_session_server>(
    get_logger(),
    ipc::test::get_process_creds().process_id(),
    Test_shm_session_server_executor::many_objects_creator_functor(),
    [&](bool result) { set_server_result(result); },
    Server_operation_mode::S_ALLOCATION_PERFORMANCE);
  if (!server->start())
  {
    ADD_FAILURE() << "Could not start server";
    return;
  }

  // Wait for server to finish
  wait_for_server_completion(Test_shm_session_server_executor::S_PERFORMANCE_TEST_TIMEOUT);
}

/// Allocation_performance testing with one client.
TEST_F(Shm_session_test, Allocation_performance_one)
{
  size_t NUM_CLIENTS = 1;

  auto server = make_unique<Test_shm_session_server>(
    get_logger(),
    ipc::test::get_process_creds().process_id(),
    Test_shm_session_server_executor::many_objects_creator_functor(),
    [&](bool result) { set_server_result(result); },
    Server_operation_mode::S_ALLOCATION_PERFORMANCE,
    NUM_CLIENTS);
  if (!server->start())
  {
    ADD_FAILURE() << "Could not start server";
    return;
  }

  Test_client_manager client_manager(*this);
  // Start clients
  client_manager.start(NUM_CLIENTS, Client_operation_mode::S_ALLOCATION_PERFORMANCE);
  // Wait for clients to finish
  EXPECT_TRUE(client_manager.wait_for_completion());
  // Wait for server to finish
  wait_for_server_completion(Test_shm_session_server_executor::S_PERFORMANCE_TEST_TIMEOUT);
}

/// Allocation_performance testing with five clients.
TEST_F(Shm_session_test, Allocation_performance_five)
{
  size_t NUM_CLIENTS = 5;

  auto server = make_unique<Test_shm_session_server>(
    get_logger(),
    ipc::test::get_process_creds().process_id(),
    Test_shm_session_server_executor::many_objects_creator_functor(),
    [&](bool result) { set_server_result(result); },
    Server_operation_mode::S_ALLOCATION_PERFORMANCE,
    NUM_CLIENTS);
  if (!server->start())
  {
    ADD_FAILURE() << "Could not start server";
    return;
  }

  Test_client_manager client_manager(*this);
  // Start clients
  client_manager.start(NUM_CLIENTS, Client_operation_mode::S_ALLOCATION_PERFORMANCE);
  // Wait for clients to finish
  EXPECT_TRUE(client_manager.wait_for_completion());
  // Wait for server to finish
  wait_for_server_completion(Test_shm_session_server_executor::S_PERFORMANCE_TEST_TIMEOUT);
}

} // namespace ipc::session::shm::arena_lend::jemalloc::test
