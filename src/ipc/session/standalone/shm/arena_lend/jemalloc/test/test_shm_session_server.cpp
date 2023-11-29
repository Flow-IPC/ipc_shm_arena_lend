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

#include "ipc/session/standalone/shm/arena_lend/jemalloc/test/test_shm_session_server.hpp"
#include "ipc/session/standalone/shm/arena_lend/jemalloc/test/test_shm_session.hpp"
#include "ipc/session/standalone/shm/arena_lend/borrower_shm_pool_collection_repository.hpp"
#include "ipc/shm/arena_lend/jemalloc/jemalloc_pages.hpp"
#include "ipc/shm/arena_lend/test/test_shm_object.hpp"
#include "ipc/test/test_common_util.hpp"
#include <boost/filesystem.hpp>

namespace fs = boost::filesystem;

using std::atomic;
using std::make_shared;
using std::make_unique;
using std::ostream;
using std::shared_ptr;
using std::static_pointer_cast;
using std::string;
using std::unique_ptr;
using std::weak_ptr;

using namespace ipc::shm::arena_lend::jemalloc;
using namespace ipc::shm::arena_lend::test;
using ipc::Error_code;
using ipc::shm::arena_lend::Shm_pool;
using ipc::util::Process_credentials;
using ipc::test::check_output;
using ipc::test::get_process_creds;

namespace ipc::session::shm::arena_lend::jemalloc::test
{

using ipc::session::schema::MqType;
using ipc::transport::struc::Channel;
using ipc::util::Fine_duration;
using Blob = Shm_session::Blob;
using Fs_path = Test_shm_session_server::Fs_path;

// Static constants
const string Test_shm_session_server::S_CLASS_NAME("Test_shm_session_server");
const string Test_shm_session_server::S_CLIENT_APP_NAME("Test_client");
const string Test_shm_session_server::S_SERVER_APP_NAME("Test_server");
const Fs_path Test_shm_session_server::S_TEMP_DIR("/tmp");
const string Test_shm_session_server::S_SERVER_PROGRAM_NAME(
  "libipc_test_jemalloc_shm_session_server.exec");
const Fs_path Test_shm_session_server::S_KERNEL_PERSISTENT_RUN_DIR(S_TEMP_DIR / S_CLASS_NAME);
const string Test_shm_session_server::S_OBJECT_DESC("Object description");
// This value must coincide with the log messages in Shm_session
const string Test_shm_session_server::S_SESSION_DISCONNECTION_PHRASE("Disconnected,");
const Fine_duration Test_shm_session_server::S_SHM_CHANNEL_RESPONSE_TIMEOUT = boost::chrono::seconds(1);
void* Test_shm_session_server::S_ERROR_HANDLING_SHM_POOL_ADDRESS = reinterpret_cast<void*>(0xBEEF);
// This value must coincide with the log messages in Shm_session
const string Test_shm_session_server::S_ERROR_HANDLING_REMOTE_PEER_FAILED_PHRASE(
  "Remote peer failed to complete action");
// This value must coincide with the log messages in Shm_session, Shm_session_data and Lender_collection
const string Test_shm_session_server::S_ERROR_HANDLING_DUPLICATE_MESSAGE_PHRASE(
  "Could not register duplicate");
// This value must coincide with the log messages in Lender_collection
const string Test_shm_session_server::S_ERROR_HANDLING_UNREGISTERED_MESSAGE_PHRASE(
  "Could not deregister unregistered");
// This value must coincide with the log messages in Shm_session_data
const string Test_shm_session_server::S_ERROR_HANDLING_UNREGISTERED_COLLECTION_MESSAGE_PHRASE(
  "Could not find lender collection");

// Static method
Fs_path Test_shm_session_server::get_program_path(pid_t process_id)
{
  Process_credentials creds(process_id, get_process_creds().user_id(), get_process_creds().group_id());
  return creds.process_invoked_as();
}

// Static method
const Fs_path& Test_shm_session_server::get_server_path()
{
  static const Fs_path S_SERVER_PATH = form_server_path();
  return S_SERVER_PATH;
}

// Static method
Fs_path Test_shm_session_server::form_server_path()
{
  return (get_program_path(get_process_creds().process_id()).parent_path() / S_SERVER_PROGRAM_NAME);
}

// Static method
const Server_app& Test_shm_session_server::get_server_app()
{
  static const Server_app S_SERVER_APP = {
    { S_SERVER_APP_NAME, get_server_path(), get_process_creds().user_id(), get_process_creds().group_id() },
    { S_CLIENT_APP_NAME },
    S_KERNEL_PERSISTENT_RUN_DIR,
    util::Permissions_level::S_GROUP_ACCESS
  };

  return S_SERVER_APP;
}

// Static method
Client_app Test_shm_session_server::form_client_app(pid_t process_id)
{
  return {{ S_CLIENT_APP_NAME, get_program_path(process_id),
            get_process_creds().user_id(), get_process_creds().group_id() }};
}

// Static method
const Client_app& Test_shm_session_server::get_client_app()
{
  static const Client_app S_CLIENT_APP = form_client_app(get_process_creds().process_id());
  return S_CLIENT_APP;
}

// Static method
Client_app::Master_set Test_shm_session_server::form_allowed_client_apps(pid_t process_id)
{
  return {{ S_CLIENT_APP_NAME, form_client_app(process_id) }};
}

Test_shm_session_server::Test_shm_session_server(flow::log::Logger* logger,
                                                 pid_t client_process_id,
                                                 Object_creation_callback&& object_creation_callback,
                                                 Result_callback&& result_callback,
                                                 Operation_mode operation_mode,
                                                 size_t performance_expected_clients) :
  flow::log::Log_context(logger, Log_component::S_TEST),
  m_task_loop(get_logger(), (S_SERVER_APP_NAME + "_loop")),
  m_arena_shm_pool_listener(get_logger()),
  m_memory_manager(make_shared<Memory_manager>(get_logger())),
  m_shm_arena(Test_ipc_arena::create(get_logger(),
                                     m_memory_manager,
                                     create_shm_object_name_generator(S_CLASS_NAME))),
  m_test_shm_pool_committed(false),
  m_test_object_deleted(false),
  m_client_process_id(client_process_id),
  m_operation_mode(operation_mode),
  m_object_creation_callback(std::move(object_creation_callback)),
  m_result_callback(std::move(result_callback)),
  m_error_handling_shm_arena(
    ((operation_mode == Operation_mode::S_ERROR_HANDLING) ?
     Test_ipc_arena::create(get_logger(),
                            m_memory_manager,
                            create_shm_object_name_generator(S_CLASS_NAME)) :
     nullptr)),
  m_error_handling_shm_pool(
    new Shm_pool(ipc::shm::arena_lend::detail::Shm_pool_offset_ptr_data_base::generate_pool_id(),
                 generate_shm_object_name(S_CLASS_NAME),
                 S_ERROR_HANDLING_SHM_POOL_ADDRESS,
                 S_ERROR_HANDLING_SHM_POOL_SIZE,
                 S_ERROR_HANDLING_SHM_POOL_FD)),
  m_performance_expected_clients(performance_expected_clients)
{
  Error_code ec;
  if (!ipc::test::create_directory_if_not_exists(S_KERNEL_PERSISTENT_RUN_DIR, ec))
  {
    FLOW_LOG_WARNING("Could not create kernel persistent directory [" << S_KERNEL_PERSISTENT_RUN_DIR << "]");
    return;
  }

  FLOW_LOG_INFO("Created kernel persistent directory [" << S_KERNEL_PERSISTENT_RUN_DIR << "]");

  m_shm_arena->add_shm_pool_listener(&m_arena_shm_pool_listener);
}

Test_shm_session_server::~Test_shm_session_server()
{
  boost::system::error_code ec;
  uintmax_t result = fs::remove_all(S_KERNEL_PERSISTENT_RUN_DIR, ec);
  if (ec)
  {
    FLOW_LOG_WARNING("Error occurred when trying to remove kernel persistent directory [" <<
      S_KERNEL_PERSISTENT_RUN_DIR << "], error [" << ec << "]");
    return;
  }

  FLOW_LOG_INFO("Removed [" << result << "] files");
}

shared_ptr<Test_shm_session_server::App_channel> Test_shm_session_server::get_app_channel(
  const shared_ptr<Server_session>& session) const
{
  const auto session_data = find_session_data(session);
  if (session_data == nullptr)
  {
    FLOW_LOG_WARNING("Could not find session");
    return nullptr;
  }

  return session_data->get_app_channel();
}

bool Test_shm_session_server::get_app_channel_state(const shared_ptr<Server_session>& session,
                                                    Channel_state& state) const
{
  auto session_data = find_session_data(session);
  if (session_data == nullptr)
  {
    FLOW_LOG_WARNING("Could not find session");
    return false;
  }

  state = session_data->get_app_channel_state();
  return true;
}

bool Test_shm_session_server::set_app_channel_state(const shared_ptr<Server_session>& session,
                                                    Channel_state state)
{
  auto session_data = find_session_data(session);
  if (session_data == nullptr)
  {
    FLOW_LOG_WARNING("Could not find session");
    return false;
  }
  session_data->set_app_channel_state(state);

  check_test_completed();
  return true;
}

bool Test_shm_session_server::set_result(bool result)
{
  Lock lock(m_result_mutex);
  if (m_result)
  {
    FLOW_LOG_INFO("Result already set [" << *m_result << "], ignoring new result [" << result << "]");
    return false;
  }
  m_result = result;
  m_result_callback(result);
  return true;
}

bool Test_shm_session_server::start()
{
  if (m_session_server != nullptr)
  {
    FLOW_LOG_WARNING("Server previously started");
    return false;
  }

  if (m_operation_mode != Operation_mode::S_ALLOCATION_PERFORMANCE)
  {
    // Create test SHM pool, which id will be relayed to clients.
    size_t size = Jemalloc_pages::get_page_size();
    size_t alignment = size;
    bool zero = false;
    m_test_shm_pool_committed = false;
    Shm_pool_collection::Arena_id arena_id = m_shm_arena->get_arena_id();
    void* pool_address =
      m_shm_arena->create_shm_pool(nullptr, size, alignment, &zero, &m_test_shm_pool_committed, arena_id);
    m_test_shm_pool = m_shm_arena->lookup_shm_pool_exact(pool_address);
    if (m_test_shm_pool == nullptr)
    {
      FLOW_LOG_WARNING("Could not locate SHM pool at address [" << pool_address << "]");
      return false;
    }
    FLOW_LOG_INFO("Created SHM pool [" << *m_test_shm_pool << "]");
  }

  m_allowed_client_apps = form_allowed_client_apps(m_client_process_id);
  m_session_server = make_unique<Session_server>(get_logger(), get_server_app(), m_allowed_client_apps);

  if ((m_operation_mode == Operation_mode::S_ALLOCATION_PERFORMANCE) && (m_performance_expected_clients == 0))
  {
    m_task_loop.post([this]() { execute_allocation_performance_tests(); });
  }
  else
  {
    // Start up server
    async_accept();
  }

  // Start loop
  m_task_loop.start();

  return true;
}

void Test_shm_session_server::async_accept()
{
  auto session = make_shared<Server_session>();
  m_session_server->async_accept(session.get(),
                                 [this, session](const Error_code& ec)
                                 {
                                   if (ec)
                                   {
                                     FLOW_LOG_INFO("Error occurred [" << ec << "]");
                                     set_result(false);
                                     return;
                                   }

                                   m_task_loop.post([this, session]()
                                                    {
                                                      if (!handle_accept(session))
                                                      {
                                                        set_result(false);
                                                      }
                                                    });

                                   async_accept();
                                 });
}

void Test_shm_session_server::stop()
{
  FLOW_LOG_INFO("Stopping server");

  m_task_loop.stop();

  FLOW_LOG_INFO("Stopped server");
}

bool Test_shm_session_server::handle_accept(const shared_ptr<Server_session>& session)
{
  FLOW_LOG_INFO("Server session established");

  if (!session->init_handlers(
        [&](const Error_code& ec)
        {
          FLOW_LOG_INFO("Error [" << ec << "] occurred, but it may be expected upon graceful close");
          // If we haven't set a result yet, then let's count this as an error
          if (set_result(false))
          {
            auto session_data = find_session_data(session);
            if (session_data != nullptr)
            {
              session_data->get_shm_session()->set_disconnected();
            }
          }
        },
        [this, session_wp = weak_ptr<Server_session>(session)](App_channel_base&& app_channel_base,
                                                               Session_mdt_reader&& mdt_reader) mutable
        {
          auto session = session_wp.lock();
          if (session == nullptr)
          {
            FLOW_LOG_WARNING("Could not convert session back");
            set_result(false);
            return;
          }

          auto app_channel_base_ptr = make_shared<App_channel_base>(std::move(app_channel_base));
          auto channel_type = mdt_reader->getPayload().getType();
          if (channel_type != TestSessionMetadata::ChannelType::APP)
          {
            FLOW_LOG_WARNING("Channel type [" << channel_type << "] does not match expected [" <<
                             TestSessionMetadata::ChannelType::APP << "]");
            set_result(false);
            return;
          }

          m_task_loop.post([this, session, app_channel_base = std::move(app_channel_base_ptr)]() mutable
                           {
                             if (!handle_app_channel(session, std::move(app_channel_base)))
                             {
                               set_result(false);
                             }
                           });
        }))
  {
    FLOW_LOG_WARNING("Failed to initialize handlers");
    return false;
  }

  if (!open_shm_channel(session))
  {
    return false;
  }

  return true;
}

bool Test_shm_session_server::open_shm_channel(const shared_ptr<Server_session>& session)
{
  auto mdt_builder = session->mdt_builder();
  mdt_builder->initPayload().setType(TestSessionMetadata::ChannelType::SHM);

  Shm_session::Shm_channel_base shm_channel_base;
  Error_code ec;
  if (!session->open_channel(&shm_channel_base, mdt_builder, &ec) || ec)
  {
    FLOW_LOG_WARNING("Error in opening SHM channel, error [" << ec << "]");
    return false;
  }

  // Create structured channel
  auto shm_channel =
    make_shared<Shm_session::Shm_channel>(get_logger(),
                                          std::move(shm_channel_base),
                                          transport::struc::Channel_base::S_SERIALIZE_VIA_HEAP,
                                          session->session_token());
  shm_channel->start([&](const auto& ec) { shm_channel_error_handler(ec); });

  auto shm_session =
    Test_shm_session::create(get_logger(),
                             Borrower_shm_pool_collection_repository_singleton::get_instance(),
                             *shm_channel,
                             [&](const auto& ec) { shm_channel_error_handler(ec); },
                             S_SHM_CHANNEL_RESPONSE_TIMEOUT);
  if (!shm_session->lend_arena(m_shm_arena))
  {
    return false;
  }

  if (m_operation_mode == Operation_mode::S_ERROR_HANDLING)
  {
    // Lend same arena as previously registered
    bool return_value = true;
    if (!check_output([&]() { return_value = shm_session->lend_arena(m_shm_arena); },
                      std::cerr,
                      S_ERROR_HANDLING_DUPLICATE_MESSAGE_PHRASE))
    {
      FLOW_LOG_WARNING("Did not encounter expected failure phrase [" <<
                       S_ERROR_HANDLING_DUPLICATE_MESSAGE_PHRASE << "] when relending arena");
      return false;
    }
    if (return_value)
    {
      FLOW_LOG_WARNING("Unexpected success in relending collection [" << m_shm_arena->get_id() << "]");
      return false;
    }
    FLOW_LOG_INFO("Expected failure in relending collection [" << m_shm_arena->get_id() << "]");

    // Lend an additional arena, but the borrower should already have the id registered, so we expect failure
    if (!check_output([&]() { return_value = shm_session->lend_arena(m_error_handling_shm_arena); },
                      std::cerr,
                      S_ERROR_HANDLING_REMOTE_PEER_FAILED_PHRASE))
    {
      FLOW_LOG_WARNING("Did not encounter expected failure message [" <<
                       S_ERROR_HANDLING_REMOTE_PEER_FAILED_PHRASE << "] when lending arena");
      return false;
    }
    if (return_value)
    {
      FLOW_LOG_WARNING("Unexpected success in lending collection [" << get_error_handling_collection_id() << "]");
      return false;
    }
    FLOW_LOG_INFO("Expected failure in lending arena [" << get_error_handling_collection_id() << "]");

    // Lend same shared memory pool as previously registered
    if (!check_output([&]() { return_value = shm_session->lend_shm_pool(m_shm_arena->get_id(), m_test_shm_pool); },
                      std::cerr,
                      S_ERROR_HANDLING_DUPLICATE_MESSAGE_PHRASE))
    {
      FLOW_LOG_WARNING("Did not encounter expected failure phrase [" <<
                       S_ERROR_HANDLING_DUPLICATE_MESSAGE_PHRASE << "] when relending shared memory pool");
      return false;
    }
    if (return_value)
    {
      FLOW_LOG_WARNING("Unexpected success in relending shared memory pool [" << m_test_shm_pool->get_id() << "]");
      return false;
    }
    FLOW_LOG_INFO("Expected failure in relending shared memory pool [" << m_test_shm_pool->get_id() << "]");

    // Lend object from unregistered collection id
    shared_ptr<int> test_object = m_error_handling_shm_arena->construct<int>();
    if (test_object == nullptr)
    {
      FLOW_LOG_WARNING("Could not create object");
      return false;
    }
    Blob blob;
    if (!check_output([&]() { blob = shm_session->lend_object(test_object); },
                      std::cerr,
                      S_ERROR_HANDLING_UNREGISTERED_COLLECTION_MESSAGE_PHRASE))
    {
      FLOW_LOG_WARNING("Did not encounter expected failure phrase [" <<
                       S_ERROR_HANDLING_UNREGISTERED_COLLECTION_MESSAGE_PHRASE << "] when lending object");
      return false;
    }
    if (!blob.empty())
    {
      FLOW_LOG_WARNING("Unexpected success in lending object in an unregistered collection");
      return false;
    }
    FLOW_LOG_INFO("Expected failure in lending object in an unregistered collection");
  }

  if (!register_session(session, std::move(shm_channel), std::move(shm_session)))
  {
    return false;
  }

  FLOW_LOG_INFO("Server SHM channel established");

  return true;
}

void Test_shm_session_server::shm_channel_error_handler(const Error_code& ec)
{
  if (ec != ipc::transport::error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE)
  {
    FLOW_LOG_INFO("Server SHM channel error occurred [" << ec << "]");
    set_result(false);
  }
  else
  {
    FLOW_LOG_INFO("Server SHM channel graceful close received");
  }
}

bool Test_shm_session_server::register_session(const shared_ptr<Server_session>& session,
                                               shared_ptr<Shm_channel>&& shm_channel,
                                               shared_ptr<Test_shm_session>&& shm_session)
{
  auto result = m_session_map.emplace(session, Session_data(std::move(shm_channel), std::move(shm_session)));
  if (!result.second)
  {
    FLOW_LOG_WARNING("Could not register session [" << session.get() << "]");
    return false;
  }

  return true;
}

bool Test_shm_session_server::handle_app_channel(const shared_ptr<Server_session>& session,
                                                 shared_ptr<App_channel_base>&& app_channel_base)
{
  auto app_channel = make_shared<App_channel>(get_logger(),
                                              std::move(*app_channel_base),
                                              transport::struc::Channel_base::S_SERIALIZE_VIA_HEAP,
                                              session->session_token());

  set_app_channel_state(session, Channel_state::S_WAITING_TO_START);

  if (!app_channel->expect_msgs(TestMessage::Which::REQUEST_TYPE,
                                [this, session](const auto& req)
                                {
                                  auto request_type = req->body_root().getRequestType();
                                  m_task_loop.post([this, session = std::move(session), request_type]()
                                                   {
                                                     if (!process_request(session, request_type))
                                                     {
                                                       set_result(false);
                                                     }
                                                   });
                                }))
  {
    FLOW_LOG_WARNING("Could not set expected messages");
    return false;
  }

  if (!app_channel->start(
        [&](const auto& ec)
        {
          if (ec != ipc::transport::error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE)
          {
            FLOW_LOG_INFO("Server App channel error occurred [" << ec << "]");
            set_result(false);
            return;
          }

          FLOW_LOG_INFO("Server App channel graceful close received, error [" << ec << "]");
        }))
  {
    FLOW_LOG_WARNING("Could not start Server App channel");
    return false;
  }

  if (!register_app_channel(session, std::move(app_channel)))
  {
    FLOW_LOG_WARNING("Could not register Server App channel");
    return false;
  }

  FLOW_LOG_INFO("Server App channel successfully opened");

  if (m_operation_mode == Operation_mode::S_ALLOCATION_PERFORMANCE)
  {
    async_execute_allocation_performance_tests_if_ready();
  }

  return true;
}

Test_shm_session_server::Session_data* Test_shm_session_server::find_session_data(
  const shared_ptr<Server_session>& session)
{
  const auto* const_this = this;
  return const_cast<Session_data*>(const_this->find_session_data(session));
}

const Test_shm_session_server::Session_data* Test_shm_session_server::find_session_data(
  const shared_ptr<Server_session>& session) const
{
  auto iter = m_session_map.find(session);
  if (iter == m_session_map.end())
  {
    FLOW_LOG_WARNING("Could not find session [" << session.get() << "]");
    return nullptr;
  }

  return &iter->second;
}

bool Test_shm_session_server::register_app_channel(const shared_ptr<Server_session>& session,
                                                   shared_ptr<App_channel>&& app_channel)
{
  auto session_data = find_session_data(session);
  if (session_data == nullptr)
  {
    FLOW_LOG_WARNING("Could not find session");
    return false;
  }

  session_data->set_app_channel(std::move(app_channel));
  return true;
}

bool Test_shm_session_server::process_request(const shared_ptr<Server_session>& session,
                                              TestMessage::RequestType request_type)
{
  Channel_state channel_state;
  if (!get_app_channel_state(session, channel_state))
  {
    return false;
  }

  FLOW_LOG_INFO("Got request of type [" << request_type << "]");

  switch (request_type)
  {
    case TestMessage::RequestType::START:
    {
      if (channel_state != Channel_state::S_WAITING_TO_START)
      {
        FLOW_LOG_WARNING("Improper request type [" << request_type << "] in channel state [" << channel_state << "]");
        return false;
      }
      set_app_channel_state(session, Channel_state::S_STARTING);

      if (!send_object(session))
      {
        return false;
      }
      set_app_channel_state(session, Channel_state::S_RUNNING);
      break;
    }

    case TestMessage::RequestType::RECEIVED:
    {
      if (channel_state != Channel_state::S_RUNNING)
      {
        FLOW_LOG_WARNING("Improper request type [" << request_type << "] in channel state [" << channel_state << "]");
        return false;
      }

      if (m_operation_mode == Operation_mode::S_ERROR_HANDLING)
      {
        // Lend shared memory pool, which should fail as it should be already registered on the borrower side
        auto* session_data = find_session_data(session);
        if (session_data != nullptr)
        {
          const auto& shm_session = session_data->get_shm_session();
          bool return_value = true;
          if (!check_output([&]()
                            {
                              return_value = shm_session->lend_shm_pool(m_shm_arena->get_id(),
                                                                        get_error_handling_shm_pool());
                            },
                            std::cerr,
                            S_ERROR_HANDLING_REMOTE_PEER_FAILED_PHRASE))
          {
            FLOW_LOG_WARNING("Did not encounter expected failure message [" <<
                             S_ERROR_HANDLING_REMOTE_PEER_FAILED_PHRASE << "]");
            return false;
          }
          if (!return_value)
          {
            FLOW_LOG_INFO("Expected failure lending error handling SHM pool [" <<
                          get_error_handling_shm_pool()->get_id() << "]");
          }
          else
          {
            FLOW_LOG_WARNING("Unexpected success lending error handling SHM pool [" <<
                             get_error_handling_shm_pool()->get_id() << "]");
            set_result(false);
          }
        }
        else
        {
          FLOW_LOG_WARNING("Could not find session data to lend error handling SHM pool");
          set_result(false);
        }
      }

      if (!send_cleanup(session))
      {
        return false;
      }
      break;
    }

    case TestMessage::RequestType::FINISH:
    {
      if (channel_state != Channel_state::S_CLEANING_UP)
      {
        FLOW_LOG_WARNING("Improper request type [" << request_type << "] in channel state [" << channel_state << "]");
        return false;
      }

      // Special tests for disconnection simulation
      if ((m_operation_mode == Operation_mode::S_DISCONNECT) && !execute_disconnection_tests(session))
      {
        set_result(false);
      }

      set_app_channel_state(session, Channel_state::S_FINISH);
      break;
    }
  }

  return true;
}

bool Test_shm_session_server::send_object(const shared_ptr<Server_session>& session)
{
  if (m_test_shm_pool == nullptr)
  {
    FLOW_LOG_WARNING("SHM pool is not set");
    return false;
  }

  const auto session_data = find_session_data(session);
  if (session_data == nullptr)
  {
    FLOW_LOG_WARNING("Could not find session");
    return false;
  }

  const auto& shm_session = session_data->get_shm_session();
  if (shm_session == nullptr)
  {
    FLOW_LOG_WARNING("Server SHM session is not initialized");
    return false;
  }

  const auto& app_channel = session_data->get_app_channel();
  if (app_channel == nullptr)
  {
    FLOW_LOG_WARNING("Application channel is not initialized");
    return false;
  }

  shared_ptr<void> object;
  if (!is_weak_ptr_initialized(m_test_object_weak))
  {
    // Create object
    FLOW_LOG_INFO("Creating object for lending");
    auto object_pair = m_object_creation_callback(m_shm_arena, [this]()
                                                  {
                                                    m_task_loop.post([this]() { handle_object_deleted(); });
                                                  });
    object = object_pair.second;
    assert(object != nullptr);

    // Save object type and object as weak pointer; we will be storing object in the session registry
    m_test_object_type = object_pair.first;
    m_test_object_weak = object_pair.second;
  }
  else
  {
    object = m_test_object_weak.lock();
    if (object == nullptr)
    {
      FLOW_LOG_WARNING("Object already released and invalid");
      return false;
    }
  }

  // Serialize object
  auto serialized_object = shm_session->lend_object(object);

  TestObjectMessage::ObjectType message_object_type = {};
  switch (m_test_object_type)
  {
    case Object_type::S_ARRAY:
      message_object_type = TestObjectMessage::ObjectType::CHAR_ARRAY;
      break;
    case Object_type::S_VECTOR:
      message_object_type = TestObjectMessage::ObjectType::VECTOR_CHAR;
      break;
    case Object_type::S_STRING:
      message_object_type = TestObjectMessage::ObjectType::STRING;
      break;
    case Object_type::S_LIST:
      message_object_type = TestObjectMessage::ObjectType::LIST;
      break;
  }

  // Compose and send message containing object
  auto message = app_channel->create_msg();
  auto capnp_object = message.body_root()->initResponseObject();
  capnp_object.setDescription(S_OBJECT_DESC);
  capnp_object.setType(message_object_type);
  capnp_object.setSerializedObject(capnp::Data::Reader(serialized_object.data(), serialized_object.size()));
  capnp_object.setShmPoolIdToCheck(m_test_shm_pool->get_id());

  Error_code ec;
  if (!app_channel->send(message, nullptr, &ec))
  {
    FLOW_LOG_WARNING("Could not send message, error [" << ec << "]");
    return false;
  }

  return true;
}

bool Test_shm_session_server::send_cleanup(const shared_ptr<Server_session>& session)
{
  // Remove previously created SHM pool, if it is still around
  if (m_test_shm_pool != nullptr)
  {
    if (!m_shm_arena->remove_shm_pool(m_test_shm_pool->get_address(),
                                      m_test_shm_pool->get_size(),
                                      m_test_shm_pool_committed,
                                      m_shm_arena->get_arena_id()))
    {
      FLOW_LOG_WARNING("Could not remove SHM pool");
      return false;
    }
    FLOW_LOG_INFO("Removed SHM pool [" << m_test_shm_pool->get_id() << "]");

    if (m_operation_mode == Operation_mode::S_ERROR_HANDLING)
    {
      auto* session_data = find_session_data(session);
      if (session_data == nullptr)
      {
        FLOW_LOG_WARNING("Could not locate session");
        return false;
      }

      // Attempt to remove just removed shared memory pool
      const auto& shm_session = session_data->get_shm_session();
      bool return_value = true;
      if (!check_output([&]()
                        {
                          return_value = shm_session->remove_lender_shm_pool(m_shm_arena->get_id(), m_test_shm_pool);
                        },
                        std::cerr,
                        S_ERROR_HANDLING_UNREGISTERED_MESSAGE_PHRASE))
      {
        FLOW_LOG_WARNING("Did not encounter expected failure phrase [" <<
                         S_ERROR_HANDLING_UNREGISTERED_MESSAGE_PHRASE <<
                         "] when removing unregistered shared memory pool");
        return false;
      }
      if (return_value)
      {
        FLOW_LOG_WARNING("Unexpected success in removing unregistered shared memory pool [" <<
                         m_test_shm_pool->get_id() << "]");
        return false;
      }
      FLOW_LOG_INFO("Expected failure in removing unregistered shared memory pool [" <<
                    m_test_shm_pool->get_id() << "]");
    }

    m_test_shm_pool.reset();
  }

  shared_ptr<App_channel> app_channel = get_app_channel(session);
  if (app_channel == nullptr)
  {
    FLOW_LOG_WARNING("Application channel is not initialized");
    return false;
  }

  // Compose and send message containing instruction
  auto message = app_channel->create_msg();
  message.body_root()->setResponseType(TestMessage::ResponseType::CLEANUP);

  Error_code ec;
  if (!app_channel->send(message, nullptr, &ec))
  {
    FLOW_LOG_WARNING("Could not send message, error [" << ec << "]");
    return false;
  }

  set_app_channel_state(session, Channel_state::S_CLEANING_UP);

  FLOW_LOG_INFO("Sent cleanup on server App channel");

  return true;
}

void Test_shm_session_server::handle_object_deleted()
{
  FLOW_LOG_INFO("Server generated object deleted");

  m_test_object_deleted = true;

  check_test_completed();
}

bool Test_shm_session_server::check_test_completed()
{
  if ((m_operation_mode != Operation_mode::S_DISCONNECT) && !m_test_object_deleted)
  {
    return false;
  }

  // Check if the channels have completed
  for (const auto& cur_map_pair : m_session_map)
  {
    if (cur_map_pair.second.get_app_channel_state() != Channel_state::S_FINISH)
    {
      return false;
    }
  }

  // We completed the test
  // Ensure we didn't delete the object if we weren't supposed to
  set_result((m_operation_mode != Operation_mode::S_DISCONNECT) || !m_test_object_deleted);
  return true;
}

bool Test_shm_session_server::execute_disconnection_tests(const shared_ptr<Server_session>& session)
{
  auto session_data = find_session_data(session);
  if (session_data == nullptr)
  {
    FLOW_LOG_WARNING("Could not find session");
    return false;
  }

  auto shm_session = session_data->get_shm_session();
  if (shm_session == nullptr)
  {
    FLOW_LOG_WARNING("Shm session is nullptr");
    return false;
  }

  size_t size = Jemalloc_pages::get_page_size();
  size_t alignment = size;
  auto arena_id = m_shm_arena->get_arena_id();
  bool zero = false;
  bool committed = false;
  void* pool_address = m_shm_arena->create_shm_pool(nullptr, size, alignment, &zero, &committed, arena_id);

  // Simulate disconnection
  shm_session->set_disconnected();

  // Attempt to lend arena
  bool result = true;
  auto test_arena =
    Test_ipc_arena::create(get_logger(),
                           m_memory_manager,
                           create_shm_object_name_generator(S_CLASS_NAME));
  if (test_arena == nullptr)
  {
    FLOW_LOG_WARNING("Could not create arena");
    return false;
  }

  {
    bool return_value = true;
    // Configure log severity override for log output checks
    *flow::log::Config::this_thread_verbosity_override() = flow::log::Sev::S_TRACE;
    if (!check_output([&]() { return_value = shm_session->lend_arena(test_arena); },
                      std::cout,
                      S_SESSION_DISCONNECTION_PHRASE))
    {
      FLOW_LOG_WARNING("Did not get expected message output when attempting to lend arena");
      result = false;
    }
    // Reset log severity override
    *flow::log::Config::this_thread_verbosity_override() = flow::log::Sev::S_END_SENTINEL;

    if (return_value)
    {
      FLOW_LOG_WARNING("Expected failure to lend arena, but got success");
      result = false;
    }
  }

  // Attempt to (create and) lend shared memory pool in registered arena
  if (m_shm_arena == nullptr)
  {
    FLOW_LOG_WARNING("Existing arena is non-null");
    return false;
  }

  {
    bool zero = false;
    bool committed = false;
    void* local_pool_address;
    // Configure log severity override for log output checks
    *flow::log::Config::this_thread_verbosity_override() = flow::log::Sev::S_TRACE;
    if (!check_output([&]()
                      {
                        local_pool_address =
                          m_shm_arena->create_shm_pool(nullptr, size, alignment, &zero, &committed, arena_id);
                      },
                      std::cout,
                      S_SESSION_DISCONNECTION_PHRASE))
    {
      FLOW_LOG_WARNING("Did not get expected message output when attempting to lend pool");
      result = false;
    }
    // Reset log severity override
    *flow::log::Config::this_thread_verbosity_override() = flow::log::Sev::S_END_SENTINEL;

    // Clean up test
    bool return_value = m_shm_arena->remove_shm_pool(local_pool_address, size, committed, arena_id);
    if (!return_value)
    {
      FLOW_LOG_WARNING("Did not successfully remove pool [" << local_pool_address << "]");
      result = false;
    }
  }

  // Attempt to lend object
  {
    shared_ptr<int> test_object = m_shm_arena->construct<int>();
    if (test_object == nullptr)
    {
      FLOW_LOG_WARNING("Could not create object");
      result = false;
      return false;
    }

    Blob blob;
    // Configure log severity override for log output checks
    *flow::log::Config::this_thread_verbosity_override() = flow::log::Sev::S_TRACE;
    if (!check_output([&]() { blob = shm_session->lend_object(test_object); },
                      std::cout,
                      S_SESSION_DISCONNECTION_PHRASE))
    {
      FLOW_LOG_WARNING("Did not get expected message output when attempting to lend object");
      result = false;
    }
    // Reset log severity override
    *flow::log::Config::this_thread_verbosity_override() = flow::log::Sev::S_END_SENTINEL;

    if (!blob.empty())
    {
      FLOW_LOG_WARNING("Expected failed serialization of object");
      result = false;
    }

    if (test_object.use_count() != 1)
    {
      FLOW_LOG_WARNING("Object has an unexpected use count [" << test_object.use_count() << "]");
      result = false;
    }
  }

  // Attempt to send remove shared memory pool
  {
    bool return_value = true;
    // Configure logger for log output checks
    *flow::log::Config::this_thread_verbosity_override() = flow::log::Sev::S_TRACE;
    if (!check_output([&]() { return_value = m_shm_arena->remove_shm_pool(pool_address, size, committed, arena_id); },
                      std::cout,
                      S_SESSION_DISCONNECTION_PHRASE))
    {
      FLOW_LOG_WARNING("Did not get expected message output when attempting to remove pool");
      result = false;
    }
    // Reset log severity override
    *flow::log::Config::this_thread_verbosity_override() = flow::log::Sev::S_END_SENTINEL;

    if (!return_value)
    {
      FLOW_LOG_WARNING("Removal of pool failed");
      result = false;
    }
  }

  return result;
}

bool Test_shm_session_server::async_execute_allocation_performance_tests_if_ready()
{
  if (m_session_map.size() == m_performance_expected_clients)
  {
    for (const auto& cur_map_pair : m_session_map)
    {
      if (cur_map_pair.second.get_app_channel_state() != Channel_state::S_WAITING_TO_START)
      {
        // Not ready
        return false;
      }
    }

    // Start performance tests
    m_task_loop.post([this]() { execute_allocation_performance_tests(); });
  }

  return true;
}

void Test_shm_session_server::execute_allocation_performance_tests()
{
  // Execute object creation, which should test performance of doing this during established sessions
  m_object_creation_callback(m_shm_arena, [](){});

  // Notify all clients to cleanup
  bool success = true;
  for (const auto& cur_map_pair : m_session_map)
  {
    if (!send_cleanup(cur_map_pair.first))
    {
      success = false;
    }
  }

  set_result(success);
}

// Static method
bool Test_shm_session_server::is_weak_ptr_initialized(const weak_ptr<void>& weak)
{
  const weak_ptr<void> empty_weak;
  return (weak.owner_before(empty_weak) || empty_weak.owner_before(weak));
}

// Static method
shared_ptr<Test_shm_session_server::Test_ipc_arena>
Test_shm_session_server::Test_ipc_arena::create(
  flow::log::Logger* logger,
  const shared_ptr<Memory_manager>& memory_manager,
  Shm_object_name_generator&& name_generator)
{
  auto arena = shared_ptr<Test_ipc_arena>(
    new Test_ipc_arena(logger, memory_manager, std::move(name_generator)));
  if (!arena->start())
  {
    return nullptr;
  }

  return arena;
}

Test_shm_session_server::Test_ipc_arena::Test_ipc_arena(
  flow::log::Logger* logger,
  const shared_ptr<Memory_manager>& memory_manager,
  Shm_object_name_generator&& name_generator) :
  Ipc_arena(logger,
            memory_manager,
            std::move(name_generator),
            util::shared_resource_permissions(util::Permissions_level::S_GROUP_ACCESS))
{
}

ostream& operator<<(ostream& os, TestSessionMetadata::ChannelType channel_type)
{
  switch (channel_type)
  {
    case TestSessionMetadata::ChannelType::APP:
      os << "App channel";
      break;
    case TestSessionMetadata::ChannelType::SHM:
      os << "SHM channel";
      break;
  }

  return os;
}

ostream& operator<<(ostream& os, TestMessage::RequestType request_type)
{
  switch (request_type)
  {
    case TestMessage::RequestType::START:
      os << "Start";
      break;
    case TestMessage::RequestType::RECEIVED:
      os << "Object received";
      break;
    case TestMessage::RequestType::FINISH:
      os << "Finish";
      break;
  }

  return os;
}

ostream& operator<<(ostream& os, Test_shm_session_server::Channel_state channel_state)
{
  using Channel_state = Test_shm_session_server::Channel_state;

  switch (channel_state)
  {
    case Channel_state::S_RESET:
      os << "Reset";
      break;
    case Channel_state::S_WAITING_TO_START:
      os << "Waiting to start";
      break;
    case Channel_state::S_STARTING:
      os << "Starting";
      break;
    case Channel_state::S_RUNNING:
      os << "Running";
      break;
    case Channel_state::S_CLEANING_UP:
      os << "Cleaning up";
      break;
    case Channel_state::S_FINISH:
      os << "Finish";
      break;
  }

  return os;
}

} // namespace ipc::session::shm::arena_lend::jemalloc::test
