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

#include "ipc/session/standalone/shm/arena_lend/jemalloc/shm_session.hpp"
#include "ipc/session/standalone/shm/arena_lend/jemalloc/test/test_message.capnp.h"
#include "ipc/session/standalone/shm/arena_lend/jemalloc/test/test_session_metadata.capnp.h"
#include "ipc/shm/arena_lend/detail/owner_spc_impl.hpp"
#include "ipc/shm/arena_lend/jemalloc/ipc_arena.hpp"
#include "ipc/session/session_server.hpp"
#include <flow/test/test_file_util.hpp>
#include <boost/filesystem.hpp>

namespace ipc::session::shm::arena_lend::jemalloc::test
{

class Test_shm_session;

/**
 * Server harness utilizing Shm_session, sessions and structured channels; the counterpart of one or more
 * Test_client (see shm_session_test.cpp). What it sets up:
 * 1. An ipc::session::Session_server accepts sessions from Test_client(s) -- which run in the same process
 *    or, via Test_shm_session_server_launcher, with this server in a separate child process.
 * 2. Per accepted session, the server actively opens the *SHM channel* (session metadata type SHM) and hands
 *    it to a new Test_shm_session (thin Shm_session subclass), then lends its jemalloc SHM arena through it.
 *    From then on SHM pools created in the arena are automatically lent to that session's peer.
 * 3. The client then opens the *app channel* (metadata type APP) -- a structured channel of TestMessage --
 *    over which the test choreography runs: client sends START -> server responds with a lend_object()ed
 *    object (created once, via the Object_creation_callback, and shared by all sessions) plus the id of a
 *    separately created probe SHM pool (which the client uses to verify borrower-side pool registration) ->
 *    client sends RECEIVED -> server removes the probe pool and responds CLEANUP -> client releases the
 *    borrowed object and sends FINISH.
 * 4. The server tracks per-session state (Session_data, Channel_state) and reports overall success exactly
 *    once via the Result_callback: every session must reach FINISH, and the lent object must get
 *    garbage-collected (its Owner_object_wrapper destructor fires the destructor callback).
 *
 * Operation_mode selects the use case: S_NORMAL as above; S_DISCONNECT and S_CRASH layer negative
 * checks on top (see the enum doc headers); S_ALLOCATION_PERFORMANCE skips the object exchange entirely and
 * instead runs a timed mass allocation once all expected clients' app channels are up (see
 * execute_allocation_performance_test() in shm_session_test.cpp for the full story).
 *
 * Client/server discovery is path- and PID-based, with no config files: the Server_app/Client_app specs are
 * derived from the current (or given) process's binary path, and /tmp/<class name> serves as the
 * kernel-persistent run dir (created in ctor, removed in dtor).
 */
class Test_shm_session_server :
  public flow::log::Log_context
{
public:
  /**
   * The server object to send.
   */
  enum class Object_type
  {
    /// An array of text.
    S_ARRAY = 0,
    /// A character vector stored in shared memory.
    S_VECTOR,
    /// A large string stored in shared memory.
    S_STRING,
    /// A list of fixed sized structures stored in shared memory.
    S_LIST
  }; // enum class Object_type

  /**
   * The way the server needs to operate, which basically is a test use case.
   */
  enum class Operation_mode
  {
    /// General operation, which is lend arena, lend shm pool, lend object, receive object return.
    S_NORMAL = 0,
    /// Mark the session as disconnected to exercise lending and communication stoppage.
    S_DISCONNECT,
    /// Performs tasks to measure shared memory object allocation performance.
    S_ALLOCATION_PERFORMANCE,
    /// Once the client confirms receiving the lent object: crash (SIGKILL self) to exercise uncivilized ending.
    S_CRASH
  }; // enum class Operation_mode

  /// Alias for functor to be executed when the object's destructor is called.
  using Object_destructor_callback = std::function<void()>;
  /**
   * Alias for functor used as a callback to create an object.
   *
   * @param arena The arena to use when constructing the object.
   * @param destructor_callback The functor to be executed in the object's destructor.
   *
   * @return The object created.
   */
  using Object_creation_callback =
    std::function<std::pair<Object_type, std::shared_ptr<void>>(
      const std::shared_ptr<ipc::shm::arena_lend::jemalloc::Ipc_arena>& arena,
      Object_destructor_callback&& destructor_callback)>;
  /**
   * Alias for functor used as a callback when the execution of the server is determined to have passed or failed.
   *
   * @param result Whether the result is success.
   */
  using Result_callback = std::function<void(bool result)>;
  /// Alias for a filesystem path.
  using Fs_path = boost::filesystem::path;

  /// The application name used in categorization and naming.
  static const std::string S_CLIENT_APP_NAME;
  /// The description to use in the message sent to the client.
  static const std::string S_OBJECT_DESC;
  /// Message phrase of failed channel communication operation due to session disconnection.
  static const std::string S_SESSION_DISCONNECTION_PHRASE;

  /**
   * Constructor.
   *
   * @param logger Used for logging purposes.
   * @param client_process_id The process id of the client that will connect to the server. This may be the
   *                          same as the server.
   * @param object_creation_callback Functor executed when the server wants to create an object.
   * @param result_callback Functor executed when the server has completed (the test).
   * @param operation_mode The testing condition in which the server will operate.
   */
  Test_shm_session_server(flow::log::Logger* logger,
                          pid_t client_process_id,
                          Object_creation_callback&& object_creation_callback,
                          Result_callback&& result_callback,
                          Operation_mode operation_mode = Operation_mode::S_NORMAL,
                          size_t performance_expected_clients = 0);
  /// Destructor.
  ~Test_shm_session_server();

  /**
   * Removes the kernel-persistent run-time state a server instance maintains outside its process (namely the
   * run-dir holding the CNS a/k/a PID file). A civilized server does this itself at destruction; a crashed
   * (e.g., Operation_mode::S_CRASH) server cannot, leaving a stale CNS behind -- which costs a subsequent
   * session's client a wasted (retried) connect attempt and leaves junk in the file-system. Tests that kill
   * the server should therefore call this afterward (and defensively beforehand): not strictly required,
   * but it reduces entropy. Best-effort: errors ignored.
   */
  static void remove_kernel_persistent_state();

  /**
   * Returns the path to the server application with the assumption that the server application is in the same
   * directory as the process that is running.
   *
   * @return See above.
   */
  static const Fs_path& get_server_path();
  /**
   * Returns the server application specification used in establishing a session.
   *
   * @return See above.
   */
  static const Server_app& get_server_app();
  /**
   * Returns the client application used in establishing a session with the assumption that the client application
   * is running in the current process.
   *
   * @return See above.
   */
  static const Client_app& get_client_app();
  /**
   * Starts the server, namely its task engine and acceptor.
   *
   * @return Whether the server was not previously started.
   */
  bool start();
  /// Stops the server, namely its task engine and acceptor.
  void stop();

private:
  /// Alias for ajemalloc-based memory manager.
  using Memory_manager = ipc::shm::arena_lend::jemalloc::Memory_manager;
  /// Alias for a shared memory pool.
  using Shm_pool = ipc::shm::arena_lend::Shm_pool;

  // Session aliases
  /// Convenience alias for MqType.
  using MqType = ipc::session::schema::MqType;
  /// Convenience alias for a session server.
  using Session_server = ipc::session::Session_server<MqType::NONE, false, TestSessionMetadata>;
  /// Convenience alias for a server session.
  using Server_session = Session_server::Server_session_obj;
  /// Convenience alias for a session metadata reader.
  using Session_mdt_reader = Server_session::Mdt_reader_ptr;
  /// Convenience alias for a session metadata builder.
  using Session_mdt_builder = Server_session::Mdt_builder_ptr;

  // Channel aliases
  /// Convenience alias for a application channel.
  using App_channel_base = Server_session::Channel_obj;
  /// Convenience alias for a structured application channel.
  using App_channel = Server_session::Structured_channel<TestMessage>;
  /// Convenience alias for a (SHM-session-internal, unstructured) shared memory channel.
  using Shm_channel = Shm_session::Shm_channel;

  /// The mutex type.
  using Mutex = std::mutex;
  /// Exclusive lock for the mutex.
  using Lock = std::lock_guard<Mutex>;

  /// Tracks channel progress from start to completion.
  enum class Channel_state
  {
    /// Initial state
    S_RESET,
    /// Waiting for client request to start
    S_WAITING_TO_START,
    /// Received client request to start and in the process of starting further communication exchange
    S_STARTING,
    /// Started communication with client
    S_RUNNING,
    /// Instructing client to perform clean up
    S_CLEANING_UP,
    /// Completed communication with client
    S_FINISH
  }; // enum class Channel_state

  /**
   * Overrides Ipc_arena to make public some interfaces for testing purposes.
   *
   * @see Ipc_arena
   */
  class Test_ipc_arena :
    public ipc::shm::arena_lend::jemalloc::Ipc_arena
  {
  public:
    /**
     * Creates an instance of this class with a single jemalloc arena. We require the use of a shared pointer,
     * because the construct() interfaces requires a handle for their destruction operations.
     *
     * @param logger Used for logging purposes.
     * @param memory_manager The memory allocator.
     * @param pool_name_base Pool-name prefix.
     *
     * @return A shared pointer to an instance of this class; not null.
     */
    static std::shared_ptr<Test_ipc_arena> create(
      flow::log::Logger* logger,
      const std::shared_ptr<Memory_manager>& memory_manager,
      Shared_name&& pool_name_base);

    // Make public
    using Ipc_arena::create_shm_pool;
    using Ipc_arena::remove_shm_pool;

  private:
    /**
     * Constructor.
     *
     * @param logger Used for logging purposes.
     * @param memory_manager The memory allocator.
     * @param pool_name_base Pool-name prefix.
     */
    Test_ipc_arena(flow::log::Logger* logger,
                   const std::shared_ptr<Memory_manager>& memory_manager,
                   Shared_name&& pool_name_base);
  }; // class Test_ipc_arena

  /**
   * Information related to a session.
   */
  class Session_data
  {
  public:
    /**
     * Constructor.  (The SHM channel is not stored here: the Shm_session subsumes and owns it.)
     *
     * @param shm_session Shared memory information for the session.
     */
    explicit Session_data(std::shared_ptr<Test_shm_session>&& shm_session) :
      m_shm_session(std::move(shm_session)),
      m_app_channel_state(Channel_state::S_RESET)
    {
    }

    /**
     * Returns the shared memory information for the session.
     *
     * @return See above.
     */
    const std::shared_ptr<Test_shm_session>& get_shm_session() const
    {
      return m_shm_session;
    }

    /**
     * Returns the channel used for transmitting application information.
     *
     * @return See above.
     */
    const std::shared_ptr<App_channel>& get_app_channel() const
    {
      return m_app_channel;
    }
    /**
     * Sets the channel used for transmitting application information.
     *
     * @param app_channel The channel to set.
     */
    void set_app_channel(std::shared_ptr<App_channel>&& app_channel)
    {
      m_app_channel = std::move(app_channel);
    }

    /**
     * Returns the application channel state.
     *
     * @return See above.
     */
    Channel_state get_app_channel_state() const
    {
      return m_app_channel_state;
    }
    /**
     * Set the application channel state. If the state is S_FINISH and the object is removed, the event sequence has
     * properly finished.
     *
     * @param state The state to set.
     */
    void set_app_channel_state(Channel_state state)
    {
      m_app_channel_state = state;
    }

  private:
    /// Shared memory management for the session.
    std::shared_ptr<Test_shm_session> m_shm_session;
    /// The channel used for transmitting application information.
    std::shared_ptr<App_channel> m_app_channel;

    /// The state of the application channel.
    Channel_state m_app_channel_state;
  }; // class Session_data

  /**
   * Returns the session data of the session.
   *
   * @param session The session.
   *
   * @return If the session is registered, the session data; otherwise, nullptr.
   */
  Session_data* find_session_data(const std::shared_ptr<Server_session>& session);
  /**
   * Returns the session data of the session.
   *
   * @param session The session.
   *
   * @return If the session is registered, the session data; otherwise, nullptr.
   */
  const Session_data* find_session_data(const std::shared_ptr<Server_session>& session) const;
  /**
   * Returns the application channel in the session.
   *
   * @param session The session where the application channel resides.
   *
   * @return If the session is registered, the application channel of the session; otherwise, nullptr.
   */
  std::shared_ptr<App_channel> get_app_channel(const std::shared_ptr<Server_session>& session) const;
  /**
   * Returns the application channel state.
   *
   * @param session The session where the application channel resides.
   * @param state If the session is registered, this will be filled with the application channel state.
   *
   * @return Whether the session is registered.
   */
  bool get_app_channel_state(const std::shared_ptr<Server_session>& session, Channel_state& state) const;
  /**
   * Set the application channel state. If the state is S_FINISH and the object is removed, the event sequence has
   * properly finished.
   *
   * @param session The session where the application channel resides.
   * @param state The state to set.
   *
   * @return Whether the operation was successful, which would fail if the session is not registered.
   */
  bool set_app_channel_state(const std::shared_ptr<Server_session>& session, Channel_state state);
  /**
   * If not already set, sets the test result and executes the callback.
   *
   * @param result Whether the result is success.
   * @return `true` unless result already set.
   */
  bool set_result(bool result);
  /**
   * Asynchronously waits to accept an incoming session.
   */
  void async_accept();
  /**
   * Handles connection acceptance.
   *
   * @param session The session associated with the connection.
   *
   * @return Whether the session was initialized properly.
   */
  bool handle_accept(const std::shared_ptr<Server_session>& session);
  /**
   * Open the internal channel used to communicate shared memory information for the session.
   *
   * @param session The session the channel is associated with.
   *
   * @return Whether the channel was opened successfully and other initialization execution succeeded.
   */
  bool open_shm_channel(const std::shared_ptr<Server_session>& session);
  /**
   * Error handler for the internal channel used to communicate shared memory information for the session.
   *
   * @param ec The error code.
   */
  void shm_channel_error_handler(const Error_code& ec);
  /**
   * Registers a shared memory information with a session.
   *
   * @param session The session to register the information with.
   * @param shm_session The shared memory information.
   *
   * @return Whether the operation succeeded, which would only fail if the session was already registered.
   */
  bool register_session(const std::shared_ptr<Server_session>& session,
                        std::shared_ptr<Test_shm_session>&& shm_session);
  /**
   * Handles receiving an opened application-based channel by the client.
   *
   * @param session The session the channel is associated with.
   * @param app_channel_base The unstructured channel.
   *
   * @return Whether the channel was started successfully by the server.
   */
  bool handle_app_channel(const std::shared_ptr<Server_session>& session,
                          std::shared_ptr<App_channel_base>&& app_channel_base);
  /**
   * Registers the application channel with a session.
   *
   * @param session The session the channel is associated with.
   * @param app_channel The application channel to register.
   *
   * @return Whether the operation was successful, which only fails if the session is not registered.
   */
  bool register_app_channel(const std::shared_ptr<Server_session>& session,
                            std::shared_ptr<App_channel>&& app_channel);

  /**
   * Processes a request sent by the client across the application channel.
   *
   * @param session The session the request's channel is associated with.
   * @param request_type The type of action requested.
   *
   * @return Whether the request was processed successfully.
   */
  bool process_request(const std::shared_ptr<Server_session>& session, TestMessage::RequestType request_type);
  /**
   * Sends a message containing a shared memory object via the application channel.
   *
   * @param session The session to send the object to.
   *
   * @return Whether the message was sent successfully.
   */
  bool send_object(const std::shared_ptr<Server_session>& session);
  /**
   * Sends a message instructing the client to clean up the test object, thereby releasing it back to the server.
   *
   * @param session The session to send the command to.
   *
   * @return Whether the message was sent successfully.
   */
  bool send_cleanup(const std::shared_ptr<Server_session>& session);
  /**
   * Handles the destruction of an object that was stored in shared memory.
   */
  void handle_object_deleted();
  /**
   * Returns whether all the sessions are in the completed state.
   *
   * @return See above.
   */
  bool check_test_completed();
  /**
   * Returns whether every registered session's application channel has reached the S_FINISH state.
   *
   * @return See above.
   */
  bool all_app_channels_finished() const;
  /**
   * Executes a series of tests demonstrating that simulated session disconnection will prevent further messages and
   * lending via the session.
   *
   * @param session The session to perform the tests on.
   *
   * @return Whether the series of tests passed.
   */
  bool execute_disconnection_tests(const std::shared_ptr<Server_session>& session);
  /**
   * Asynchronously executes a series of test to output the performance of allocations if the appropriate number
   * of clients have sessions established.
   *
   * @return Whether the tests have been executed.
   */
  bool async_execute_allocation_performance_tests_if_ready();
  /**
   * Executes a series of tests to output the performance of allocations.
   */
  void execute_allocation_performance_tests();

  /**
   * Returns the path of the program with the given process id.
   *
   * @param process_id The process id of the program to look up.
   *
   * @return See above.
   *
   * @throw A runtime system error if the process id could not be looked up.
   */
  static Fs_path get_program_path(pid_t process_id);
  /**
   * Returns the full path of the server application with the assumption that it lives in the same directory as
   * the running process.
   *
   * @return See above.
   */
  static Fs_path form_server_path();
  /**
   * Returns the client application specification used in establishing a session.
   *
   * @param client_process_id The process id of the client that will establish a session with the server.
   *
   * @return See above.
   */
  static Client_app form_client_app(pid_t client_process_id);
  /**
   * Returns the client applications that can establish a session with the server.
   *
   * @param client_process_id The process id of the client that will establish a session with the server.
   *
   * @return See above.
   */
  static Client_app::Master_set form_allowed_client_apps(pid_t client_process_id);

  /// The task engine.
  flow::async::Single_thread_task_loop m_task_loop;
  /// The set of client applications that can establish a session with the server.
  Client_app::Master_set m_allowed_client_apps;
  /// The acceptor for connections.
  std::unique_ptr<Session_server> m_session_server;
  /// The memory manager on the server.
  std::shared_ptr<Memory_manager> m_memory_manager;
  /// The shared memory arena on the server.
  const std::shared_ptr<Test_ipc_arena> m_shm_arena;
  /// Used for testing borrower SHM pool notifications.
  std::shared_ptr<Shm_pool> m_test_shm_pool;
  /// Whether the SHM pool was committed to physical memory by the memory manager.
  bool m_test_shm_pool_committed;
  /// The type of the object that was created.
  Object_type m_test_object_type;
  /**
   * Strong handle to the object that was created (null until then): the owner must hold its handle in order
   * to (re)lend the object to further sessions. Dropped -- allowing garbage collection -- once every session
   * has FINISHed; see process_request().
   */
  std::shared_ptr<void> m_test_object;
  /**
   * Relay through which the lent object's destructor callback (see send_object()) notifies `*this` of the
   * object's deletion -- without touching `*this` if it is already destroyed. That is a real possibility:
   * when the arena is torn down with the object still lent out (e.g., S_DISCONNECT mode: the borrower holds
   * it past our destruction), the object is reclaimed on the object-DB's own schedule -- via the
   * degraded-mode thread once our constructing thread exits, at the latest at process exit via the atexit
   * machinery -- in any case after this server, and its task loop, are gone.
   * The destructor callback shares ownership of this relay; our destructor nulls #m_server within it.
   */
  struct Object_deleted_relay
  {
    /// Synchronizes access to #m_server (destruction of the server versus the destructor callback firing).
    Mutex m_mutex;
    /// The server to notify of object deletion; null once that server is destroyed.
    Test_shm_session_server* m_server = nullptr;
  }; // struct Object_deleted_relay
  /// See Object_deleted_relay.
  const std::shared_ptr<Object_deleted_relay> m_object_deleted_relay;
  /// Whether the test object was deleted.
  bool m_test_object_deleted;
  /// Associates session to session data.
  std::unordered_map<std::shared_ptr<Server_session>, Session_data> m_session_map;

  /// Synchronizes access to #m_result.
  mutable Mutex m_result_mutex;
  /// Stores the test result from the server perspective.
  std::optional<bool> m_result;
  /// The process id of the client that will establish a session with the server.
  const pid_t m_client_process_id;
  /// The server behavior, which correlates to the test that is being run.
  const Operation_mode m_operation_mode;
  /// The callback to be executed to create an object.
  const Object_creation_callback m_object_creation_callback;
  /// The callback to be executed when the test result is determined.
  const Result_callback m_result_callback;
  /// The expected clients to establish a session during performance tests.
  const size_t m_performance_expected_clients;

  /// The name of this class.
  static const std::string S_CLASS_NAME;
  /// The server name used in categorization and naming.
  static const std::string S_SERVER_APP_NAME;
  /// The directory (no trailing slash) to a temporary directory that is cleaned up on reboot.
  static const Fs_path S_TEMP_DIR;
  /// The server executable name when run external to the unit tests program.
  static const std::string S_SERVER_PROGRAM_NAME;
  /// The directory (no trailing slash) to put server information at runtime.
  static const Fs_path S_KERNEL_PERSISTENT_RUN_DIR;

  friend std::ostream& operator<<(std::ostream& os, Test_shm_session_server::Channel_state channel_state);
}; // class Test_shm_session_server

/**
 * Outputs the enumeration to the stream.
 *
 * @param os The output stream.
 * @param channel_type The enumeration to output.
 *
 * @return The output stream parameter.
 */
std::ostream& operator<<(std::ostream& os, TestSessionMetadata::ChannelType channel_type);
/**
 * Outputs the enumeration to the stream.
 *
 * @param os The output stream.
 * @param request_type The enumeration to output.
 *
 * @return The output stream parameter.
 */
std::ostream& operator<<(std::ostream& os, TestMessage::RequestType request_type);
/**
 * Outputs the enumeration to the stream.
 *
 * @param os The output stream.
 * @param channel_state The enumeration to output.
 *
 * @return The output stream parameter.
 */
std::ostream& operator<<(std::ostream& os, Test_shm_session_server::Channel_state channel_state);

} // namespace ipc::session::shm::arena_lend::jemalloc::test
