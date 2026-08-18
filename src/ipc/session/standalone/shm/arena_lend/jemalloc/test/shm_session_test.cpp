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

#include "ipc/session/client_session.hpp"
#include "ipc/session/standalone/shm/arena_lend/detail/borrower_shm_pool_collection_repository.hpp"
#include "ipc/session/standalone/shm/arena_lend/jemalloc/shm_session.hpp"
#include "ipc/session/standalone/shm/arena_lend/jemalloc/test/test_shm_session.hpp"
#include "ipc/session/standalone/shm/arena_lend/jemalloc/test/test_shm_session_server_executor.hpp"
#include "ipc/session/standalone/shm/arena_lend/jemalloc/test/test_shm_session_server_launcher.hpp"
#include "ipc/session/standalone/shm/arena_lend/jemalloc/test/test_message.capnp.h"
#include "ipc/session/standalone/shm/arena_lend/jemalloc/test/test_session_metadata.capnp.h"
#include "ipc/shm/arena_lend/borrower_allocator_arena.hpp"
#include "ipc/shm/arena_lend/jemalloc/ipc_arena.hpp"
#include "ipc/shm/arena_lend/test/test_shm_object.hpp"
#include "ipc/shm/stl/stateless_allocator.hpp"
#include "ipc/shm/arena_lend/util.hpp"
#include "ipc/test/test_common_util.hpp"
#include "ipc/test/test_logger.hpp"
#include "ipc/transport/asio_local_stream_socket.hpp"
#include "ipc/transport/sync_io/native_socket_stream.hpp"
#include <flow/test/test_common_util.hpp>
#include <boost/thread/future.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <optional>
#include <thread>
#include <gtest/gtest.h>
#include <unistd.h>
#include <sys/types.h>

namespace chrono = boost::chrono;

using boost::promise;
using boost::future_status;
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
using collection_id_t = detail::collection_id_t;
using owner_id_t = detail::owner_id_t;

using ipc::shm::arena_lend::Borrower_allocator_arena;
using ipc::shm::arena_lend::Shm_pool;
using ipc::shm::arena_lend::jemalloc::Ipc_arena;
using ipc::shm::arena_lend::jemalloc::Memory_manager;

/* The borrower-side registry of borrowed pools; a singleton, templated on the arena type purely as a
 * compile-time discriminator. Note: in checks below we always use to_address_safe(), which returns null on an
 * unknown/no-longer-borrowed pool id; plain to_address() is undefined behavior in that case -- which is
 * exactly the case various checks probe for (and any check might hit, when failing). */
using Borrower_repo = detail::Borrower_shm_pool_collection_repository<Ipc_arena>;

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
using Shm_channel = Shm_session::Shm_channel; // (Unstructured; a Shm_session subsumes one.)

/* ==================================== What this suite aims to prove ====================================
 *
 * First, some assumptions worth stating plainly, for a reader who has not been living inside SHM-jemalloc.
 * SHM-jemalloc is an "arena-lending" shared-memory provider. One process -- call it the owner -- creates an
 * arena (Ipc_arena) and constructs C++ objects inside it. The memory itself lives in shared-memory pools,
 * which the arena creates as needed. A second process -- the borrower -- can be lent such an object: the
 * owner produces a small handle-blob via Shm_session::lend_object() (Shm_session being the per-conversation
 * lending engine), transmits it by whatever IPC means it likes, and the borrower turns it back into a usable
 * pointer via borrow_object(). From then on the two processes co-own the object, in the style of a
 * cross-process shared_ptr group: it is destroyed only once every side has dropped every handle. The
 * bookkeeping for that is a use-count living in shared memory itself. A session, meanwhile, can end in two
 * broad ways: civilized (each side finishes its business, releases what it borrowed, destroys its session
 * object) or uncivilized (a process crashes, or exits while the other side still holds borrowed objects).
 * Much of the subtlety below -- and in the production design -- concerns what exactly is guaranteed in each
 * kind of ending.
 *
 * The tests in this file were not written top-down from the following list. They accumulated over time, and
 * they overlap; the list came later, to state what we believe needs testing and to show how each item is in
 * fact covered. Each item has a codename (A1, B2, ...); test doc headers below cite the codenames they cover.
 *
 * Group A -- which things keep which other things alive:
 * - A1: An object handle from Ipc_arena::construct() keeps its arena alive. (Covered by ipc_arena_test, not
 *   here; the present suite merely relies on it throughout.)
 * - A2: lend_arena() stores its own reference to the arena, and there is no un-lend operation. Hence, once
 *   an arena is lent, the user may drop their own arena handle; everything keeps working.
 *   [Standalone_hold_edges]
 * - A3: Each borrow_object()-returned handle keeps the borrower-side Shm_session alive. Hence the user may
 *   drop their session handle while still holding borrowed objects; the session truly dies -- performing its
 *   borrower-side cleanup -- only once the last such handle is gone. [Standalone_hold_edges]
 * - A4: The reverse direction deliberately does *not* hold: a lent-but-unreturned object does not keep the
 *   owner's arena alive. If the arena gets destroyed anyway, its objects are considered moot -- use-counts
 *   notwithstanding -- and are reclaimed. [Disconnected pair; and see C2]
 *
 * Group B -- the everyday lend/borrow lifecycle, everything alive and connected:
 * - B1: Lending propagates all that is needed (the arena's identity, each shared-memory pool, the object
 *   blob itself), and the borrower reads back exactly the content the owner wrote -- for flat objects and
 *   for offset-pointer-based (STL-style) ones. [In_process_array and its siblings]
 * - B2: Returning an object requires no messaging: the borrower's final release decrements the use-count in
 *   shared memory; the owner side notices -- its garbage collection is piggy-backed onto other operations --
 *   and runs the object's destructor. [In_process_array and siblings; the FINISH-time explicit
 *   Ipc_arena::this_thread_gc() call in the server harness makes the detection deterministic]
 * - B3: When the owner removes a shared-memory pool mid-session, that is *not* propagated to the borrower;
 *   the borrower's pool registrations are cleaned up wholesale when its Shm_session is destroyed, and not
 *   before. [In_process_array and siblings, via the still-registered and deregistered-after checks]
 * - B4: When several sessions borrow from the same pool, the borrower side registers it once and
 *   reference-counts; it is deregistered only when the last of those sessions dies. [Multisession pair]
 * - B5: To lend -- or re-lend -- an object, the owner must itself hold a live handle to it; and every
 *   borrowing session sees the object at the same borrower-side address. [Multisession pair]
 *
 * Group C -- how sessions end:
 * - C1: The civilized, borrower-goes-first ending: release all borrowed handles, tell the owner (the app's
 *   own FINISH message here), then destroy the sessions. [every In_process/External/Multisession test]
 * - C2: The owner-side-goes-first ending, with a borrow still outstanding: per A4 the arena teardown simply
 *   proceeds. What the borrower keeps: its memory stays mapped while it holds the handle, so reading through
 *   the handle cannot crash; but the *content* is no longer guaranteed. [Disconnected pair]
 * - C3: Standalone Shm_session use (as in this file) has no automatic end-of-session handshake; that is what
 *   ipc::session's Graceful_finisher provides in the full-stack setup. A standalone user must therefore
 *   build their own -- here it is the FINISH message -- and this whole suite structurally relies on that
 *   gate; the Disconnected pair shows what happens when it is deliberately skipped.
 * - C4: Graceful_finisher itself is ipc::session territory and is deliberately not tested here; it needs
 *   its own coverage at that level.
 * - C5: When the opposing side goes away, one's error handler fires exactly once, with a truthy Error_code,
 *   from the session's own background thread. Afterward lend_arena() returns false and lend_object() returns
 *   an empty blob; but borrow_object() of an already-obtained blob still works normally. And destroying
 *   one's *own* session -- the local-trigger ending -- never fires one's own handler at all.
 *   [Standalone_session_end_errors]
 * - C6: Operations attempted after disconnection fail gracefully rather than crash. That includes creating a
 *   pool in an already-lent arena: the failed lend-notification must not fail the allocation itself (that
 *   one is verified by log phrase, as the allocation's success leaves nothing else to observe).
 *   [Disconnected pair, server side]
 * - C7: If the owner *process* exits while a borrow is outstanding, its exit-time cleanup removes the
 *   use-count pool from the file-system. The borrower's eventual release discovers the pool is gone,
 *   concludes the object is moot, and quietly does nothing -- rather than, say, throwing from a shared_ptr
 *   destructor. [Disconnected_external_process, which asserts that exact code path fired]
 *
 * Group D -- crashes (the fully uncivilized endings):
 * - D1: The owner crashes (SIGKILL, so zero teardown runs). The borrower detects the channel's death; the
 *   borrowed content is fully intact (nothing was torn down, so nothing scrubbed it); and the owner's pool
 *   files leak into the file-system -- documented behavior, and the test cleans them up.
 *   [Crash_external_process]
 * - D2: The borrower crashes. Not tested directly, on purpose: the owner-side machinery cannot distinguish a
 *   crashed borrower from a merely-slow one, or from a lend that was never borrowed at all -- and that last,
 *   equivalent scenario (no reclamation while the arena lives; mooting at arena death) is asserted by
 *   Standalone_session_end_errors.
 * - D3: Releasing a borrowed handle after the owner *crashed* -- contrast C7, where the owner exited
 *   cleanly: here the pool file was never unlinked, so the release takes the normal path and simply works.
 *   [Crash_external_process]
 *
 * Also intentionally untested, for the record: channel-hosing first noticed by a failed *send* (inherently
 * timing-dependent; the receive-side detection C5 covers the same handler contract); feeding borrow_object()
 * garbage of the correct size (documented undefined behavior); wrong-size or misaligned blobs (covered by
 * Lend_borrow_test's sabotage_* cases); crashing mid-message (torn channel frames; soak-test territory); and
 * the ipc::session-level concerns (Graceful_finisher per C4; the client/server start/stop cycle), which
 * belong at that level rather than here. The Allocation_performance_* trio, finally, is a smoke benchmark
 * rather than a contract test; see its helper's doc header.
 * ======================================================================================================== */

/* @todo Notes from ygoldfel's working with these (incredibly useful) test cases -- as well as other test cases
 * in this suite -- when debugging a major change in how pointers are represented:
 *   - In Shm_session_test: the logging situation is a bit unpleasant at times, when volume is relatively high.
 *     2 processes send output to stdout/stderr, and these 2 streams are interleaved pretty often.
 *     Technically one should not be using 2+ `Simple_ostream_logger`s to one stream simultaneously per its docs;
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
 * I suspect this can all be addressed by looking at logging in the unit test suite holistically and then making
 * some changes having considered everything. Shm_session_test is, I believe, special due to using 2 helper
 * executables; there are some other tests that launch processes too but not in the same way (I think).
 *
 * Many tests, at this point, are quite simple; so really what to do depends. It won't be rocket science,
 * but it's a matter of classifying the nicest thing to for each test, write up some simple utilities that
 * cover all needs, and then apply them.
 *
 * Note: there is no need to work overtime for some sort of consistency just for its own sake.
 * E.g., if some test uses plain `cout`, while another uses FLOW_LOG_INFO()
 * to console, that doesn't mean one of them has to be changed. The test suite is not a work of art, nor is
 * it production code; and there will be different authors with their own preferences. The goal here is
 * convenience, for the maintainer and test-runner; that's all.)
 */

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
    /* Nickname the main (test-driving) thread, so its log lines are easy to pick out among the various
     * worker-thread logging. (Skip renaming the OS thread: for the process' main thread that would rename
     * the process in `ps` et al.) */
    flow::log::Logger::this_thread_set_logged_nickname("testMain", get_logger(), false);

    // Establish logger prior to execution
    ipc::shm::arena_lend::set_logger(get_logger());
  }

  ~Shm_session_test() override
  {
    // Must remember to remove our stuff from singleton so as to not mess over subsequent tests.
    ipc::shm::arena_lend::set_logger(nullptr);
    flow::log::Logger::this_thread_set_logged_nickname({}, get_logger(), false);
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
    const chrono::duration<size_t>& wait_duration = Test_shm_session_server_executor::S_TEST_TIMEOUT)
  {
    // Wait for server to complete
    auto server_future = m_server_promise.get_future();
    auto server_status = server_future.wait_for(wait_duration);
    if (server_status == future_status::ready)
    {
      EXPECT_TRUE(server_future.get()) <<
        "The server reported failure. If it ran as a separate process: see launcher warnings above "
        "(abnormal-exit info) and its log output, which is interleaved with ours in the console.";
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
  optional<bool> m_server_result;
  /// Used to wait until the server result is available or a timeout is reached.
  promise<bool> m_server_promise;
}; // class Shm_session_test

namespace
{

/**
 * The delay between client attempts to connect to a server that may still be starting up (notably an
 * external-process server, which is spawned concurrently with the client's execution).
 */
static const chrono::duration S_CONNECT_RETRY_DELAY = chrono::milliseconds(100);
/// The max number of client connect attempts (spaced S_CONNECT_RETRY_DELAY apart) before declaring failure.
static constexpr unsigned int S_MAX_CONNECT_ATTEMPTS = 50;
/// Invalid process id used in return values.
static constexpr util::process_id_t S_INVALID_PROCESS_ID = -1;

/**
 * A sample client application that communicates with a Test_shm_session_server to obtain an object. One
 * instance = one full ipc::session::Client_session against the server (which may be in this process or a
 * separate one). start() connects the session and blocks until the test completes or times out. The server
 * opens the SHM channel toward us (we hand it to a Test_shm_session); we then open the app channel and drive
 * the choreography described in Test_shm_session_server's doc header (START -> borrow_object() and validate
 * contents plus repository bookkeeping -> RECEIVED -> on CLEANUP release the object and verify the probe
 * pool got deregistered -> FINISH). An optional Event_listener is notified at the interesting junctures;
 * the fancier tests (multisession lock-step, disconnect, crash) plug in specialized listeners.
 * In Operation_mode::S_ALLOCATION_PERFORMANCE the client merely connects and idles (no START), serving as
 * session-lending load for the server's timed allocation.
 */
class Test_client :
  public flow::log::Log_context
{
public:
  /// Lowest client id to be used by the test.
  static constexpr unsigned int S_LOWEST_CLIENT_ID = 0;

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
                                        collection_id_t collection_id,
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
    m_expect_abrupt_session_end(false),
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
                                     // Shm_session handles its own disconnection internally.
                                   });
                }
              },
              [&](Shm_channel&& channel, Client_session_mdt_reader&& mdt_reader)
              {
                auto channel_ptr = make_shared<Shm_channel>(std::move(channel));
                EXPECT_EQ(mdt_reader->getPayload().getType(), TestSessionMetadata::ChannelType::SHM);
                m_task_loop.post([&, shm_channel = std::move(channel_ptr)]() mutable
                                 {
                                   handle_shm_channel(std::move(shm_channel));
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
   * Directs the client to treat abrupt session/SHM-channel death as its *successful* outcome -- for tests
   * whose server intentionally dies (e.g., Operation_mode::S_CRASH server-side) instead of completing the
   * normal choreography. Call before start().
   */
  void expect_abrupt_session_end()
  {
    EXPECT_FALSE(m_started);
    m_expect_abrupt_session_end = true;
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
   * Checks that the test shared memory pool is still registered in the repository. "Still," because by the
   * time this is used the server has already removed the pool on its side: owner-side pool removals are not
   * propagated to borrowers mid-session (see Shm_session::remove_lender_shm_pool() and the @todo therein);
   * borrower-side deregistration happens wholesale in ~Shm_session(). The test helpers assert that
   * post-destruction counterpart separately.
   *
   * @return Whether the check succeeded.
   */
  bool check_test_shm_pool_still_registered() const
  {
    Lock lock(m_test_shm_pool_data_mutex);

    if (m_test_shm_pool_id == 0)
    {
      ADD_FAILURE() << "SHM pool id not set";
      return false;
    }

    return Borrower_repo::to_address_safe(m_test_shm_pool_id, 0) != nullptr;
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
      /* The server has removed the test pool on its side by now; per the deferred-cleanup contract we should
       * nevertheless still have it registered. See the check's doc header. */
      EXPECT_TRUE(check_test_shm_pool_still_registered());
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
    /* Connect client to server. The server may still be starting up -- notably the external-process tests
     * spawn it concurrently with us -- and per sync_connect() docs the intended idiom for that is simply to
     * sleep and retry on failure (a failed attempt leaves the session in NULL state, so retrying on the same
     * object is fine). */
    ipc::Error_code ec;
    bool connected = false;
    for (unsigned int attempt = 1; attempt <= S_MAX_CONNECT_ATTEMPTS; ++attempt)
    {
      if (!m_session.sync_connect(&ec))
      {
        ADD_FAILURE() << "Could not connect (session in unexpected state)";
        fail_test();
        return;
      }
      if (!ec)
      {
        connected = true;
        break;
      }
      FLOW_LOG_INFO("Connect attempt [" << attempt << "] / [" << S_MAX_CONNECT_ATTEMPTS << "] failed, error [" <<
                    ec << "]; server may still be starting up; will retry");
      flow::util::this_thread::sleep_for(S_CONNECT_RETRY_DELAY);
    }
    if (!connected)
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
  void handle_shm_channel(shared_ptr<Shm_channel>&& channel)
  {
    /* Unstructured channel: Shm_session subsumes it as-is (upgrading it to a structured channel internally;
     * hence also no need to start() anything here). */
    m_shm_session =
      Test_shm_session::create(get_logger(),
                               std::move(*channel),
                               m_session.session_token(),
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
    // Shm_session handles its own disconnection internally.
    if (m_expect_abrupt_session_end)
    {
      /* The server dies on purpose in this test mode (e.g., S_CRASH), so losing the session mid-choreography
       * is the client's expected -- successful -- ending.  (Should the server instead die *prematurely*,
       * before lending the object, this alone would wrongly report success; but the test body independently
       * verifies the object was received and correct, so a premature death still fails the test.) */
      set_result(true);
      return;
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
          m_test_shm_pool_address = Borrower_repo::to_address_safe(m_test_shm_pool_id, 0);
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
                           FLOW_LOG_INFO("Client app channel encountered close, error [" << ec << "]");
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
        using Shm_vector = Test_shm_session_server_executor::Vector_type<Shm_session::Borrower_arena_allocator>;
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
        using Shm_string = Test_shm_session_server_executor::String_type<Shm_session::Borrower_arena_allocator>;
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
        using Shm_list = Test_shm_session_server_executor::List_type<Shm_session::Borrower_arena_allocator>;
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

    const auto* handle
      = reinterpret_cast<const Test_shm_session::Shm_object_handle*>(serialized_object.const_data());
    if ((m_event_listener != nullptr) && (object != nullptr))
    {
      m_event_listener->notify_object_received(std::move(object),
                                               handle->m_collection_id, handle->m_pool_id, handle->m_pool_offset);
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
    if (!m_app_channel->send(&message, nullptr, &ec))
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
    }

    /* Notify the listener *before* fulfilling the promise: start() unblocks on the promise, after which the
     * test body may destroy the listener (typically a stack object) -- so the reverse order would let this
     * thread's virtual call race that destruction.  (No double-set possible despite the promise now being
     * outside the lock: the m_result guard above lets only one thread reach this point.) */
    if (m_event_listener != nullptr)
    {
      m_event_listener->notify_completion(result);
    }
    m_promise.set_value(result);
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
  /// Whether abrupt session/SHM-channel death counts as the client's *successful* outcome (crash-mode tests).
  bool m_expect_abrupt_session_end;
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
  optional<bool> m_result;
  /// Used to wait until the client result is available or a timeout is reached.
  promise<bool> m_promise;

  /// The shared memory session information on the client.  (Owns the SHM channel.)
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
                                      [[maybe_unused]] collection_id_t collection_id,
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
                       collection_id_t& collection_id,
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
                                      collection_id_t collection_id,
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
  collection_id_t m_object_collection_id;
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
                                      collection_id_t collection_id,
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
                                      collection_id_t collection_id,
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

/**
 * Orchestrates the execution of a test involving multiple concurrent clients. start(N) creates N Test_client
 * instances (each with an event listener from the create_event_listener() factory, which subclasses
 * override to install specialized listeners) and runs each client's blocking start() in its own thread;
 * wait_for_completion() joins those threads -- each client's pass/fail lands via the usual
 * EXPECT/set_server_result paths, so this class itself only tracks gross lifecycle state. Since all clients
 * connect to the one server, this is how the multi-session scenarios (N sessions sharing one lent object)
 * get set up.
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
      const auto cur_client_id = cur_pair.first;
      auto cur_thread = make_unique<std::thread>([this, &cur_client, cur_client_id]()
      {
        // Nickname this client-running thread, so its log lines are easy to pick out.
        flow::log::Logger::this_thread_set_logged_nickname("testCliRun" + to_string(cur_client_id), get_logger());
        cur_client.start();
      });
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
     * be purely positive) it should be not-worse. I only pontificate this text to make clear it's possible
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
                       collection_id_t& collection_id,
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
  owner_id_t object_owner_id = {};
  collection_id_t object_collection_id = {};
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
      EXPECT_NE(Borrower_repo::to_address_safe(object_shm_pool_id, object_pool_offset), nullptr);

      object_owner_id = client.get_remote_process_id();
      EXPECT_NE(object_owner_id, S_INVALID_PROCESS_ID);
    }

    client.stop();
  }

  /* Client -- and with it the borrower-side Shm_session -- is now destroyed; ~Shm_session() performs the
   * wholesale borrower-side cleanup, so the object's pool must now be deregistered. (Mid-session it stayed
   * registered even after owner-side removal; see check_test_shm_pool_still_registered().) */
  EXPECT_EQ(Borrower_repo::to_address_safe(object_shm_pool_id, object_pool_offset), nullptr);

  // Stop and destroy server
  if (server != nullptr)
  {
    server->stop();
    server.reset();
  }

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
  owner_id_t object_owner_id = {};
  collection_id_t object_collection_id = {};
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
      EXPECT_NE(Borrower_repo::to_address_safe(object_shm_pool_id, object_pool_offset), nullptr);

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
    EXPECT_NE(Borrower_repo::to_address_safe(object_shm_pool_id, object_pool_offset), nullptr);
  }

  /* All clients -- and with them their borrower-side Shm_sessions -- are now destroyed: the pool's
   * registration ref-count reached zero, so it must now be deregistered. */
  EXPECT_EQ(Borrower_repo::to_address_safe(object_shm_pool_id, object_pool_offset), nullptr);
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
 * Executes a series of tests wherein the server lends an object to a client, and the session then ends --
 * by simulated disconnect -- while the client still holds the borrowed object. The server, having lent
 * arena + pool + object, marks the session disconnected and attempts further lending operations (lend a new
 * arena; create -- and thus lend -- a pool in the already-lent arena; lend another object), verifying on its
 * side that each fails gracefully with the expected log phrase. The server is then destroyed -- in the
 * external-process variant its whole process exits -- and the client verifies the post-owner-death contract:
 * the borrowed pool remains registered borrower-side, and the object's memory remains mapped (readable),
 * while the handle is held; but content is no longer guaranteed (the arena's destruction moots the
 * outstanding borrow); and releasing the handle with the owner gone quietly no-ops. In the external-process
 * variant we additionally assert -- via log phrase -- that the release took the tolerant slow path
 * (lend-tracker pool discovered removed => object moot => no-op).
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
  const bool server_is_external = (server == nullptr);
  owner_id_t object_owner_id = {};
  collection_id_t object_collection_id = {};
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
      EXPECT_EQ(Borrower_repo::to_address_safe(object_shm_pool_id, object_pool_offset), object.get());
    }
    else
    {
      ADD_FAILURE() << "Object is nullptr";
    }

    object_owner_id = client.get_remote_process_id();
    EXPECT_NE(object_owner_id, S_INVALID_PROCESS_ID);

    if ((object != nullptr) && (!server_is_external))
    {
      /* In-process server: it -- and with it the arena and hence the object -- is still alive here; and while
       * the arena lives, our borrowed handle's nonzero use-count prevents the object's reclamation. So its
       * content must be intact. (In the external-process variant no such check is possible even at this
       * early point: the server process has already exited; see the next check.) */
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
      /* The server -- and with it the arena -- is destroyed (in-process: just above; external: its process
       * exited even before the preceding checks). Our un-returned borrowed handle does not prevent that: by
       * contract an arena's destruction moots all use-counts, and its objects get force-reclaimed (possibly
       * deferred, but as late as owner-process exit). What *is* still guaranteed: the pool stays mapped in our
       * (borrower) process while we hold the handle, so reading through it cannot crash -- but the content is
       * indeterminate (in practice: zeroed, if the owner-side pool teardown ran by now; unchanged otherwise,
       * as nothing scrubs it). So: perform the read -- its not-crashing is the assertable thing -- and log,
       * but do not assert, what we saw. */
      FLOW_LOG_SET_CONTEXT(test_harness.get_logger(), Log_component::S_TEST);
      const auto& raw_message = object->m_message;
      const string message{raw_message, ::strnlen(raw_message, sizeof(raw_message))};
      FLOW_LOG_INFO("Borrowed object content after server destruction (indeterminate by contract; "
                    "informational only): [" << message << "].");
    }

    /* Release the object despite the disconnected (and by now destroyed) session and server. Object return
     * involves no messaging (a use-count atomic in SHM is decremented in-place), so no session is needed; and
     * with the arena gone the decrement itself is moot -- the owner side already force-reclaimed the object.
     * In the external-process variant we can be more specific: the owner process' exit removed the
     * lend-tracker pool (which holds the use-count) from the file-system, and this thread has never opened
     * that pool; so this release must take the tolerant slow path -- discover the pool is gone, conclude the
     * object is moot, quietly no-op -- and we assert exactly that via its log phrase. (In-process, the timing
     * of the owner-side teardown steps is not deterministic enough to predict which path the release takes;
     * there we settle for its not blowing up.) */
    object.reset(); // (Not the last handle: the event listener holds another. So this much is unremarkable.)
    if (server_is_external)
    {
      const auto open_fail_ct_pre
        = Ipc_arena::obj_db_aux_pool_global_stats().m_client_tl_aux_pool_hndl_open_fail_count.load();
      EXPECT_TRUE(check_output([&]() { event_listener.release_object(); },
                               std::cout,
                               "lend-tracker-pool has been removed from the file-system"));
      // The benign-open-failure stat must reflect the same event the log phrase just showed.
      EXPECT_EQ(Ipc_arena::obj_db_aux_pool_global_stats().m_client_tl_aux_pool_hndl_open_fail_count.load(),
                open_fail_ct_pre + 1);
    }
    else
    {
      event_listener.release_object();
    }

    client.stop();
  }

  /* Client -- and with it the borrower-side Shm_session -- is now destroyed; ~Shm_session() performs the
   * wholesale borrower-side cleanup, so the object's pool must now be deregistered. */
  EXPECT_EQ(Borrower_repo::to_address_safe(object_shm_pool_id, object_pool_offset), nullptr);
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

/// Returns how many test-suite-created SHM pool files currently sit in the SHM file-system dir.
size_t count_test_shm_object_files()
{
  using ipc::shm::arena_lend::test::S_SHM_OBJECT_NAME_PREFIX;
  using ipc::shm::arena_lend::test::S_SHM_OBJECT_DIR;

  size_t count = 0;
  for (const auto& dir_entry : std::filesystem::directory_iterator(S_SHM_OBJECT_DIR))
  {
    if (dir_entry.path().filename().string().rfind(S_SHM_OBJECT_NAME_PREFIX, 0) == 0)
    {
      ++count;
    }
  }
  return count;
}

/**
 * Executes the owner-crash test: necessarily external-process -- the server process SIGKILLs itself, at the
 * moment the client provably holds the borrowed object (see Operation_mode::S_CRASH). What this verifies --
 * contrast each item with execute_disconnect_tests(), where the server side ends *civilizedly* with the
 * borrow outstanding:
 * 1. The borrowed object's content survives the owner's crash fully intact -- and, unlike after a civilized
 *    owner-process exit (whose teardown purges pool contents, leaving them indeterminate), here that is
 *    guaranteed and asserted: the crashed owner ran zero teardown, so nothing scrubbed anything.
 * 2. Releasing the borrowed handle afterward works via the *normal* path: the crash never unlinked the
 *    lend-tracker pool, so the disposer's use-count decrement lands in the orphaned-but-openable pool
 *    (asserted via the *absence* of the tolerant-disposer log phrase; contrast with the civilized variant,
 *    where the pool is gone and the disposer detects that and quietly no-ops). Both endings must be -- and
 *    with this test, are -- crash-proof for the borrower.
 * 3. The owner's SHM pool files leak into the file-system: documented/expected on owner crash. (In
 *    ipc::session-land the next server-app instance's kernel-persistent cleanup sweeps such leftovers;
 *    standalone, it is the user's responsibility.) The test asserts the leak occurred, then sweeps it.
 *
 * Intentionally not tested: crashing *mid-message* (torn channel frames -- the receiver would treat it as
 * channel-hosing); provoking that deterministically is impractical here; it is soak-test territory.
 *
 * @param test_harness The outer scope of the tests.
 */
void execute_crash_tests(Shm_session_test& test_harness)
{
  using ipc::shm::arena_lend::test::S_SHM_OBJECT_NAME_PREFIX;
  using ipc::shm::arena_lend::test::remove_shm_objects_filesystem;

  /* Pre-clean any leftover test SHM pool files (e.g., pools known to be leaked by earlier tests' owner
   * processes at exit), so that the post-crash leak check below is attributable to our crashed server alone.
   * (Like the rest of this suite, this assumes no concurrent test runs on the machine.)  Ditto the server's
   * kernel-persistent run-dir (see remove_kernel_persistent_state() doc header) -- e.g., an earlier crashed
   * run may have left a stale CNS (PID) file; a connect attempt reading it merely fails and gets retried,
   * so this is not strictly required -- but starting from a known-clean state reduces entropy. */
  remove_shm_objects_filesystem(S_SHM_OBJECT_NAME_PREFIX);
  Test_shm_session_server::remove_kernel_persistent_state();

  owner_id_t object_owner_id = {};
  collection_id_t object_collection_id = {};
  pool_id_t object_shm_pool_id = {};
  pool_offset_t object_pool_offset = {};
  {
    Test_client client(test_harness.get_logger());
    Delayed_object_removal_event_listener event_listener(test_harness, client);
    client.set_event_listener(&event_listener);
    client.expect_abrupt_session_end(); // The server will die mid-choreography; that is this test's point.

    client.start();
    /* The server self-SIGKILLs once we confirm receiving the object; the launcher observes the death and
     * reports it (the TEST_F body inverts the usual success expectation accordingly), unblocking this. */
    test_harness.wait_for_server_completion();

    shared_ptr<Simple_object> object;
    {
      shared_ptr<void> generic_object;
      event_listener.get_object_data(generic_object, object_collection_id, object_shm_pool_id, object_pool_offset);
      object = static_pointer_cast<Simple_object>(generic_object);
    }
    if (object == nullptr)
    {
      ADD_FAILURE() << "Object is nullptr";
      client.stop();
      return;
    }
    EXPECT_EQ(Borrower_repo::to_address_safe(object_shm_pool_id, object_pool_offset), object.get());
    object_owner_id = client.get_remote_process_id();
    EXPECT_NE(object_owner_id, S_INVALID_PROCESS_ID);

    {
      // Doc header item 1: content must be fully intact -- the crashed owner ran zero teardown.
      string_view message = object->m_message;
      EXPECT_EQ(message, Test_shm_session_server_executor::S_MESSAGE);
    }

    /* Doc header item 2: release must work, and via the normal path -- assert the tolerant-disposer
     * pool-is-gone phrase did *not* appear (the crash never unlinked the lend-tracker pool), and neither
     * did the benign-open-failure stat budge. */
    object.reset(); // (Not the last handle: the event listener holds another.)
    const auto open_fail_ct_pre
      = Ipc_arena::obj_db_aux_pool_global_stats().m_client_tl_aux_pool_hndl_open_fail_count.load();
    EXPECT_FALSE(check_output([&]() { event_listener.release_object(); },
                              std::cout,
                              "lend-tracker-pool has been removed from the file-system"));
    EXPECT_EQ(Ipc_arena::obj_db_aux_pool_global_stats().m_client_tl_aux_pool_hndl_open_fail_count.load(),
              open_fail_ct_pre);

    client.stop();
  }

  /* Client -- and with it the borrower-side Shm_session -- is now destroyed; ~Shm_session() performs the
   * wholesale borrower-side cleanup, so the object's pool must now be deregistered. */
  EXPECT_EQ(Borrower_repo::to_address_safe(object_shm_pool_id, object_pool_offset), nullptr);

  // Doc header item 3: the crashed owner's pool files leaked; assert that, then sweep them.
  EXPECT_GT(count_test_shm_object_files(), 0u);
  EXPECT_TRUE(remove_shm_objects_filesystem(S_SHM_OBJECT_NAME_PREFIX));
  EXPECT_EQ(count_test_shm_object_files(), 0u);

  /* Similarly the crashed server orphaned its kernel-persistent run-dir (CNS/PID file); remove it. (A
   * subsequent test's client reading the stale CNS would merely waste a connect attempt and retry; still,
   * not leaving junk behind for others = less entropy.) */
  Test_shm_session_server::remove_kernel_persistent_state();
}

/**
 * Body of the `Allocation_performance_*` trio.  What this tests:
 *
 * It is a cheap smoke-benchmark of in-SHM allocation -- not a benchmark with a pass/fail threshold.
 * The server allocates a 1-million-node list in SHM
 * (Test_shm_session_server_executor::many_objects_creator_functor()) under a `flow::perf::Checkpointing_timer`,
 * which *logs* the elapsed time for eyeballing; the only actual assertions are that everything completes,
 * cleanly, within a timeout. So functionally it is a sanity check ("mass allocation under N live sessions
 * works and does not take absurdly long"), with the logged timing available when one cares to look.
 * It is not intended as a formal benchmark, but as a quick assessment it can be helpful when making changes.
 *
 * How this is tested: Everything is in-process (no server executable is spawned; contrast with the
 * `External_*` tests elsewhere in this file). A Test_shm_session_server runs in S_ALLOCATION_PERFORMANCE
 * mode; `n_clients` real clients connect via full ipc::session sessions, each getting the usual two channels
 * (internal SHM-lend channel + app channel) -- but in this mode clients never request the test object: they
 * connect and wait. Once all `n_clients` app channels are up (immediately, if zero), the server runs the
 * timed allocation. The point of nonzero `n_clients`: each new SHM pool the allocation forces into
 * existence is lent (`lend_shm_pool` message) to every live session -- so this exercises allocation *under
 * session-lending load*, versus the zero-client baseline.  Afterward the server broadcasts CLEANUP, the
 * clients finish, and both sides report success.
 *
 * @param test_harness The outer scope of the tests.
 * @param n_clients Number of concurrent (idle) client sessions during the allocation; 0 = pure baseline.
 */
void execute_allocation_performance_test(Shm_session_test& test_harness, size_t n_clients)
{
  /* The allocation takes ~1 sec with decent hardware and full optimization; allow for much worse
   * (unoptimized/instrumented builds). */
  constexpr chrono::duration<size_t> TIMEOUT = chrono::seconds(10);

  auto server = make_unique<Test_shm_session_server>(
    test_harness.get_logger(),
    ipc::test::get_process_creds().process_id(),
    Test_shm_session_server_executor::many_objects_creator_functor(),
    [&](bool result) { test_harness.set_server_result(result); },
    Server_operation_mode::S_ALLOCATION_PERFORMANCE,
    n_clients);
  if (!server->start())
  {
    ADD_FAILURE() << "Could not start server";
    return;
  }

  if (n_clients == 0)
  {
    test_harness.wait_for_server_completion(TIMEOUT);
    return;
  }

  Test_client_manager client_manager(test_harness);
  // Start clients
  client_manager.start(n_clients, Client_operation_mode::S_ALLOCATION_PERFORMANCE);
  // Wait for clients to finish
  EXPECT_TRUE(client_manager.wait_for_completion());
  // Wait for server to finish
  test_harness.wait_for_server_completion(TIMEOUT);
}

/* In-SHM payload for the Standalone_* tests below: carries a canary value; counts its destructions, so a test
 * can assert exactly when the owner-side reclamation machinery ran (the dtor runs owner-side, which in those
 * tests is this same process). Reminder: tests must reset s_dtor_ct -- after flushing any prior test's
 * leftovers via Ipc_arena::this_thread_gc() -- before relying on it. */
struct Reclaim_probe
{
  /// Total ~Reclaim_probe() invocations in this process.
  inline static std::atomic<unsigned int> s_dtor_ct{0};

  /// The canary payload.
  int m_value;

  explicit Reclaim_probe(int value) : m_value(value) {}
  ~Reclaim_probe() { ++s_dtor_ct; }
}; // struct Reclaim_probe

/* Rig for tests of Shm_session used in *standalone* fashion (no ipc::session anywhere): a pair of production
 * Shm_sessions connected by a fresh Unix-domain-socket pair within this same process -- plus one owner-side
 * Ipc_arena. Self-borrowing (process X borrowing from process X) is explicitly allowed and unremarkable per
 * Shm_session docs; it lets these tests exercise both sides' behavior deterministically, with no helper
 * process. By convention side A (index 0) is the owner/lender side; side B (index 1) the borrower side.
 *
 * The error handler given to each side's create() records its firings: a count and the first Error_code,
 * awaitable via await_first_fire() (the handler fires from the session's internal thread). Per Shm_session
 * contract it must fire at most once, and only upon channel-hosing (opposing trigger); locally-triggered
 * destruction must not fire it. Tests reset/drop the public m_arena / m_session_a / m_session_b handles at
 * will; whatever remains is destroyed at rig destruction (in safe order: sessions before arena). */
class Standalone_session_pair :
  public flow::log::Log_context
{
public:
  explicit Standalone_session_pair(flow::log::Logger* logger) :
    flow::log::Log_context(logger, Log_component::S_TEST)
  {
    using ipc::shm::arena_lend::test::create_test_pool_name_base;
    namespace local_ns = transport::asio_local_stream_socket::local_ns;
    using flow::util::Task_engine;
    using boost::uuids::random_generator;

    // The arena (owner-side). Nothing session-specific about it.
    m_arena = Ipc_arena::create(get_logger(),
                                make_shared<Memory_manager>(),
                                create_test_pool_name_base("shmSessionStandalone"),
                                util::shared_resource_permissions(util::Permissions_level::S_GROUP_ACCESS));
    EXPECT_NE(m_arena, nullptr);

    // A pre-connected local-socket pair; each end becomes one side's (subsumed) Shm_channel.
    Task_engine asio_engine; // Formally required to construct asio sockets; unused otherwise.
    using Peer_socket = transport::asio_local_stream_socket::Peer_socket<transport::Native_socket_stream_cfg::Protocol>;
    Peer_socket asio_sock_a(asio_engine);
    Peer_socket asio_sock_b(asio_engine);
    ipc::Error_code sys_err_code;
    local_ns::connect_pair(asio_sock_a, asio_sock_b, sys_err_code);
    EXPECT_FALSE(sys_err_code) << "connect_pair() failed: [" << sys_err_code.message() << "].";

    // Same (random) token on both sides, as prescribed by Shm_session::create() docs for standalone use.
    const auto token = random_generator()();

    const auto make_session
      = [&](Peer_socket& asio_sock, unsigned int side_idx, const string& nickname)
    {
      Shm_channel shm_channel{get_logger(), nickname,
                              transport::sync_io::Native_socket_stream
                                {get_logger(), nickname, util::Native_handle{asio_sock.release()}}};
      /* A connect_pair()ed socket lacks the normally-auto-detected opposing-process info; per
       * Shm_session::create() docs set it explicitly. Both sides are this process. */
      shm_channel.blob_snd()->remote_peer_process_credentials(ipc::test::get_process_creds());
      return Shm_session::create(get_logger(), std::move(shm_channel), token,
                                 [this, side_idx](const ipc::Error_code& err_code)
                                   { on_channel_hosed(side_idx, err_code); });
    };
    m_session_a = make_session(asio_sock_a, 0, "shmStandaloneA");
    m_session_b = make_session(asio_sock_b, 1, "shmStandaloneB");
    EXPECT_NE(m_session_a, nullptr);
    EXPECT_NE(m_session_b, nullptr);
  } // Standalone_session_pair()

  /**
   * How many times the given side's channel-hosing error handler has fired so far (contract: 0 or 1).
   *
   * @param side_idx 0 for side A, 1 for side B.
   * @return See above.
   */
  unsigned int fire_count(unsigned int side_idx) const
  {
    Lock lock(m_fire_mutex);
    return m_fire_counts[side_idx];
  }

  /**
   * Awaits (a few seconds max) the given side's first error-handler firing; returns its Error_code, or empty
   * optional on timeout.
   *
   * @param side_idx 0 for side A, 1 for side B.
   * @return See above.
   */
  optional<ipc::Error_code> await_first_fire(unsigned int side_idx)
  {
    // Poll (test-code simplicity) with a generous deadline; the detection itself is typically ~instant.
    for (unsigned int attempt_idx = 0; attempt_idx != 500; ++attempt_idx)
    {
      {
        Lock lock(m_fire_mutex);
        if (m_fire_counts[side_idx] != 0)
        {
          return m_first_codes[side_idx];
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return {};
  }

private:
  /**
   * The sessions' error handler: records the firing (thread: the respective session's internal thread W).
   *
   * @param side_idx 0 for side A, 1 for side B.
   * @param err_code The reported channel-hosing reason.
   */
  void on_channel_hosed(unsigned int side_idx, const ipc::Error_code& err_code)
  {
    Lock lock(m_fire_mutex);
    if (m_fire_counts[side_idx]++ == 0)
    {
      m_first_codes[side_idx] = err_code;
    }
  }

  /// Guards the handler-firing records below.
  mutable Mutex m_fire_mutex;
  /// Per-side count of error-handler firings.
  std::array<unsigned int, 2> m_fire_counts{};
  /// Per-side first-firing Error_code (meaningful once the respective count is nonzero).
  std::array<ipc::Error_code, 2> m_first_codes;

public:
  // Public and reset()able at will by tests. Declared after the handler state (which they may touch at death).

  /// The owner-side arena. Hold-edge tests drop this deliberately.
  shared_ptr<Ipc_arena> m_arena;
  /// Side A = owner/lender-side session.
  shared_ptr<Shm_session> m_session_a;
  /// Side B = borrower-side session. Hold-edge tests drop this deliberately.
  shared_ptr<Shm_session> m_session_b;
}; // class Standalone_session_pair

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
 * 11. Client releases object (i.e., handles reach zero); the cross-process GC machinery (use-count atomics
 *     in SHM) leads to the owner-side destructor running, whereby the server detects the object's return
 * 12. Client sends a notification to server indicating test completion
 * 13. Server receives test completion notification and ensures the object was removed successfully
 * 14. Client checks that the test shared memory pool is *still* registered in the borrower repository despite
 *     the server-side removal (mid-session removals are not propagated to borrowers; borrower-side cleanup
 *     happens wholesale at session end -- and step 17 checks exactly that counterpart)
 * 15. Client checks that the object's shared memory pool was properly registered in the repository
 * 16. Server and client are destroyed
 * 17. Client checks that the object's shared memory pool was properly deregistered from the repository
 *
 * This test has the client and server in the same process as the unit test execution.
 *
 * Covers, per the master list at the top of this file: B1, B2, B3, C1. (The External_* and
 * other-object-type sibling tests below cover the same, varying the process split and the object shape.)
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

  execute_general_tests(*this);
}

/**
 * See "In_process_array" test above. This test has the client receiving an object with offset pointer handles,
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

  execute_general_tests(*this);
}

/**
 * See "In_process_array" test above. This test has the client receiving a large object with offset pointer handles,
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

  execute_general_tests(*this);
}

/**
 * See "In_process_array" test above. This test has the client receiving an object containing offset pointer
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
 *
 * Covers, per the master list at the top of this file: B4, B5, C1.
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
 * See "Multisession_in_process" test above. This test has clients in the same process as the unit test
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
  execute_multisession_tests(*this);
}

/**
 * See "In_process_array" test above. This test has the following differences:
 * 1. The server simulates session disconnection with the client and then attempts to lend shared memory data
 *    (i.e., arena, shared memory pool, shared memory object), verifying each attempt fails gracefully.
 * 2. The server is destroyed before the client, while the client still holds the borrowed object.
 * 3. The client verifies the borrowed object's memory remains mapped (readable) -- though its content is no
 *    longer guaranteed, the arena's destruction having mooted the outstanding borrow.
 * 4. The client releases the borrowed object with the owner gone, which must quietly no-op.
 *
 * See execute_disconnect_tests() doc header for the full discussion.
 *
 * Covers, per the master list at the top of this file: A4, C2, C3, C6.
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
 *
 * Covers, per the master list at the top of this file: the same as its in-process sibling, plus C7 (only
 * here does the owner *process* exit, unlinking the use-count pool before the client's release).
 */
TEST_F(Shm_session_test, Disconnected_external_process)
{
  Test_shm_session_server_launcher launcher(get_logger());
  launcher.async_run(Object_type::S_ARRAY,
                     [&](Test_shm_session_server_launcher::Result result)
                     {
                       set_server_result(result == Test_shm_session_server_launcher::Result::S_SUCCESS);
                     },
                     Server_operation_mode::S_DISCONNECT);

  execute_disconnect_tests(*this);
}

/**
 * The owner-crash (uncivilized-ending) test: the server, in a separate process, SIGKILLs itself at the
 * moment the client holds the borrowed object; the client then verifies the post-owner-crash contract
 * (content intact; release works via the normal path; pool files leaked, swept). See
 * execute_crash_tests() doc header for the full discussion including the intentional exclusions.
 * Necessarily external-process-only: a process cannot crash itself and go on testing.
 *
 * Covers, per the master list at the top of this file: D1, D3.
 */
TEST_F(Shm_session_test, Crash_external_process)
{
  Test_shm_session_server_launcher launcher(get_logger());
  launcher.async_run(Object_type::S_ARRAY,
                     [&](Test_shm_session_server_launcher::Result result)
                     {
                       /* Death by SIGKILL maps to no Result enum value, so the launcher classifies it as
                        * S_UNKNOWN_FAILURE (its WARNING will show the killing signal, 9). Uniquely among
                        * these tests, that *is* the expected outcome. */
                       set_server_result(result == Test_shm_session_server_launcher::Result::S_UNKNOWN_FAILURE);
                     },
                     Server_operation_mode::S_CRASH);

  execute_crash_tests(*this);
}

/// Allocation-performance smoke-benchmark, zero clients (pure-allocation baseline).  See helper's doc header.
TEST_F(Shm_session_test, Allocation_performance_zero)
{
  execute_allocation_performance_test(*this, 0);
}

/// Allocation-performance smoke-benchmark, one live (idle) client session.  See helper's doc header.
TEST_F(Shm_session_test, Allocation_performance_one)
{
  execute_allocation_performance_test(*this, 1);
}

/// Allocation-performance smoke-benchmark, five live (idle) client sessions.  See helper's doc header.
TEST_F(Shm_session_test, Allocation_performance_five)
{
  execute_allocation_performance_test(*this, 5);
}

/**
 * Standalone-mode (no ipc::session) probe of the two documented hold-edges:
 * 1. Session->arena: lend_arena() stores its own arena handle (there is deliberately no unlend API), so
 *    dropping the user's arena handle mid-session must change nothing observable -- reads, further
 *    lend/borrow round-trips, and reclamation all keep working.
 * 2. Borrowed-handle->session: each borrow_object()-returned handle's disposer holds the borrower-side
 *    Shm_session, so dropping the user's session handle while borrowed handles live must likewise change
 *    nothing observable; the session truly dies -- borrower-side wholesale cleanup and all -- only once the
 *    last borrowed handle is dropped.
 * Additionally asserts the reclamation contract at the end: with all borrower handles released and the owner
 * handle dropped, the (piggy-backed) owner-side GC destroys the object -- observed via ~Reclaim_probe().
 *
 * Covers, per the master list at the top of this file: A2, A3.
 */
TEST_F(Shm_session_test, Standalone_hold_edges)
{
  /* Flush any prior test's deferred owner-side reclamations (queued for this thread), so s_dtor_ct can't get
   * spuriously bumped by a piggy-backed scan mid-test; then zero it. */
  Ipc_arena::this_thread_gc();
  Reclaim_probe::s_dtor_ct = 0;

  Standalone_session_pair rig(get_logger());
  auto& session_a = rig.m_session_a;
  auto& session_b = rig.m_session_b;

  ASSERT_TRUE(session_a->lend_arena(rig.m_arena));
  auto owner_handle = rig.m_arena->construct<Reclaim_probe>(1337);
  ASSERT_NE(owner_handle, nullptr);
  auto blob = session_a->lend_object(owner_handle);
  ASSERT_FALSE(blob.empty());
  auto borrowed_handle = session_b->borrow_object<Reclaim_probe>(blob);
  ASSERT_NE(borrowed_handle, nullptr);
  EXPECT_EQ(borrowed_handle->m_value, 1337);

  // Hold-edge 1 (see doc header): drop our arena handle; nothing observable may change.
  rig.m_arena.reset();
  EXPECT_EQ(owner_handle->m_value, 1337);
  auto blob_2 = session_a->lend_object(owner_handle); // Re-lending through the lent arena still works.
  ASSERT_FALSE(blob_2.empty());
  auto borrowed_handle_2 = session_b->borrow_object<Reclaim_probe>(blob_2);
  ASSERT_NE(borrowed_handle_2, nullptr);
  EXPECT_EQ(borrowed_handle_2->m_value, 1337);

  // Hold-edge 2 (see doc header): drop our borrower-side session handle; nothing observable may change.
  session_b.reset();
  EXPECT_EQ(borrowed_handle->m_value, 1337);
  EXPECT_EQ(borrowed_handle_2->m_value, 1337);

  /* Unwind. Borrower handles first (the 2nd reset is the borrower-side session's true death, wholesale
   * borrower-side cleanup included); no reclamation may occur while the owner handle is held. */
  borrowed_handle.reset();
  borrowed_handle_2.reset();
  EXPECT_EQ(Reclaim_probe::s_dtor_ct.load(), 0u);
  /* Now the owner handle: use-count reaches zero with the arena alive => the owner-side GC must reclaim.
   * (The same-thread dispose typically reclaims right then; the explicit GC call makes it deterministic.) */
  owner_handle.reset();
  Ipc_arena::this_thread_gc();
  EXPECT_EQ(Reclaim_probe::s_dtor_ct.load(), 1u);

  /* Rig destruction cleans up the rest. Note side A's error handler may fire during all this (the borrower
   * session's death above hoses side A's channel); that is contractual and harmless -- the Standalone
   * session-end test asserts the specifics. */
} // TEST_F(Shm_session_test, Standalone_hold_edges)

/**
 * Standalone-mode (no ipc::session) test of the advertised Shm_session session-end/error contract, in both
 * directions. Specifically, per create() + class doc headers:
 * - Local trigger: destroying one's Shm_session is the entire end-of-session procedure; one's own error
 *   handler must never fire for that.
 * - Opposing trigger: the surviving side's error handler must fire -- with a truthy Error_code, at most once
 *   (even across its own subsequent destruction) -- upon detecting the channel-hosing.
 * - Post-hosing: lend_arena() returns false; lend_object() returns an empty blob; but borrow_object() (of a
 *   pre-hosing-obtained blob) works normally -- a deliberate, documented asymmetry.
 * (Not tested intentionally: garbage blob contents of the correct size -- documented as undefined behavior;
 * wrong-size/misaligned blobs -- covered by Lend_borrow_test's sabotage_shm_level_* cases.)
 *
 * Covers, per the master list at the top of this file: C5 (and D2, by the equivalence explained there).
 */
TEST_F(Shm_session_test, Standalone_session_end_errors)
{
  // Phase 1: borrower side ends locally; owner side detects. Post-hosing lend behavior asserted.
  {
    Ipc_arena::this_thread_gc(); // See Standalone_hold_edges.
    Reclaim_probe::s_dtor_ct = 0;

    Standalone_session_pair rig(get_logger());
    auto& session_a = rig.m_session_a;

    ASSERT_TRUE(session_a->lend_arena(rig.m_arena));
    auto owner_handle = rig.m_arena->construct<Reclaim_probe>(42);
    ASSERT_NE(owner_handle, nullptr);
    auto blob = session_a->lend_object(owner_handle);
    ASSERT_FALSE(blob.empty());

    // Local trigger on side B: destroy it (no borrowed handles exist); its own handler must never fire.
    rig.m_session_b.reset();
    EXPECT_EQ(rig.fire_count(1), 0u);

    // Opposing trigger on side A: exactly one firing, truthy code.
    const auto err_code_or_none = rig.await_first_fire(0);
    ASSERT_TRUE(err_code_or_none.has_value()) << "Side A failed to detect opposing session end in time.";
    EXPECT_TRUE(bool(*err_code_or_none));
    EXPECT_EQ(rig.fire_count(0), 1u);

    // Advertised post-hosing behavior.
    EXPECT_FALSE(session_a->lend_arena(rig.m_arena));
    EXPECT_TRUE(session_a->lend_object(owner_handle).empty());

    // Destroying the hosed session must not re-fire the handler (at-most-once).
    session_a.reset();
    EXPECT_EQ(rig.fire_count(0), 1u);

    /* (The lent-but-never-borrowed lend above means the object's use-count stays elevated: dropping
     * owner_handle here does *not* reclaim it -- that happens only via arena-death mooting at rig
     * destruction. So, deliberately, no s_dtor_ct assertion in this phase.) */
  }

  // Phase 2: owner side ends locally; borrower side detects -- and borrow_object() still works post-hosing.
  {
    Ipc_arena::this_thread_gc(); // See Standalone_hold_edges.
    Reclaim_probe::s_dtor_ct = 0;

    Standalone_session_pair rig(get_logger());

    ASSERT_TRUE(rig.m_session_a->lend_arena(rig.m_arena));
    auto owner_handle = rig.m_arena->construct<Reclaim_probe>(1943);
    ASSERT_NE(owner_handle, nullptr);
    auto blob = rig.m_session_a->lend_object(owner_handle);
    ASSERT_FALSE(blob.empty());

    // Local trigger on side A. The arena and object live on: we hold handles, and lending already occurred.
    rig.m_session_a.reset();
    EXPECT_EQ(rig.fire_count(0), 0u);

    // Opposing trigger on side B.
    const auto err_code_or_none = rig.await_first_fire(1);
    ASSERT_TRUE(err_code_or_none.has_value()) << "Side B failed to detect opposing session end in time.";
    EXPECT_TRUE(bool(*err_code_or_none));
    EXPECT_EQ(rig.fire_count(1), 1u);

    /* borrow_object() of the pre-hosing-obtained blob must work normally on the hosed side: the pool-lend
     * messages all completed back when lend_object() succeeded, so everything needed is on hand. */
    auto borrowed_handle = rig.m_session_b->borrow_object<Reclaim_probe>(blob);
    ASSERT_NE(borrowed_handle, nullptr);
    EXPECT_EQ(borrowed_handle->m_value, 1943);

    // Full civilized unwind works despite the dead session: release, then owner drop => reclamation.
    borrowed_handle.reset();
    EXPECT_EQ(Reclaim_probe::s_dtor_ct.load(), 0u);
    owner_handle.reset();
    Ipc_arena::this_thread_gc();
    EXPECT_EQ(Reclaim_probe::s_dtor_ct.load(), 1u);
  }
} // TEST_F(Shm_session_test, Standalone_session_end_errors)

} // namespace ipc::session::shm::arena_lend::jemalloc::test
