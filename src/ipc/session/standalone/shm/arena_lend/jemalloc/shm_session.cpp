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

#include "ipc/session/standalone/shm/arena_lend/jemalloc/shm_session.hpp"
#include "ipc/session/standalone/shm/arena_lend/borrower_shm_pool_collection_repository.hpp"
#include "ipc/shm/arena_lend/jemalloc/ipc_arena.hpp"
#include "ipc/transport/struc/channel.hpp"

using ipc::shm::arena_lend::jemalloc::Ipc_arena;

using std::make_shared;
using std::make_unique;
using std::set;
using std::shared_ptr;
using std::string;
using std::to_string;
using std::unique_ptr;

namespace ipc::session::shm::arena_lend::jemalloc
{

shared_ptr<Shm_session> Shm_session::create(
  flow::log::Logger* logger,
  Borrower_shm_pool_collection_repository& borrower_repository,
  Shm_channel& shm_channel,
  flow::async::Task_asio_err&& shm_channel_error_handler,
  util::Fine_duration shm_channel_request_timeout)
{
  return shared_ptr<Shm_session>(
    new Shm_session(logger,
                    borrower_repository,
                    shm_channel,
                    std::move(shm_channel_error_handler),
                    shm_channel_request_timeout));
}

Shm_session::Shm_session(flow::log::Logger* logger,
                         Borrower_shm_pool_collection_repository& borrower_repository,
                         Shm_channel& shm_channel,
                         flow::async::Task_asio_err&& shm_channel_error_handler,
                         util::Fine_duration shm_channel_request_timeout) :
  flow::log::Log_context(logger, Log_component::S_SESSION),
  m_connected(true),
  m_borrower_repository(borrower_repository),
  m_session_data(logger),
  m_shm_channel(shm_channel),
  m_remote_process_id(shm_channel.owned_channel().remote_peer_process_credentials().process_id()),
  m_shm_channel_error_handler(std::move(shm_channel_error_handler)),
  m_shm_channel_request_timeout(shm_channel_request_timeout),
  m_serial_task_loop(logger, ("JSSS_" + to_string(m_remote_process_id))),
  m_parallel_task_loop(logger, ("JSSP_" + to_string(m_remote_process_id)), 0)
{
  FLOW_LOG_TRACE("Constructing [" << this << "]");

  if (!register_expected_messages())
  {
    throw std::runtime_error("Could not register expected messages");
  }

  m_borrower_repository.register_owner(m_remote_process_id);
  m_serial_task_loop.start();
  m_parallel_task_loop.start();
}

Shm_session::~Shm_session()
{
  FLOW_LOG_TRACE("Destructing [" << this << "]");

  /* For each thread in each of these 2 event loops (1 single-thread, 1 multi-thread):
   *   - If a task is concurrently executing right now, wait until it (and only it) finishes. For example
   *     it could be processing a pool-removal message.
   *     - If none is concurrently executing then nothing to do; continue immediately.
   *   - Stop/join the thread.
   * Otherwise such code in such threads can access stuff we access/modify below.
   *
   * Is the stopping, in and of itself, dangerous somehow? Well, if something was concurrently happening already,
   * then we cannot stop it now; we are then acting as-if we (dtor) were called a tiny bit later by waiting for
   * it to compelte. If something was about to happen, but we prevented it, then it is ~no different from the
   * precipitating event occurring a tiny bit later -- when there is no Shm_session through which to speak anymore. */
  m_parallel_task_loop.stop();
  m_serial_task_loop.stop();

  // Removed registration of expected channel messages
  deregister_expected_messages();

  // Remove listeners registered to the respective arenas so that no notifications are further processed from the
  // arenas, which may survive much longer than the session
  for (const auto& cur_map_pair : m_shm_pool_listener_map)
  {
    auto& cur_arena = cur_map_pair.first;
    auto& cur_listener_ptr = cur_map_pair.second;
    cur_arena->remove_shm_pool_listener(cur_listener_ptr.get());
  }

  // Deregister entities from repository - We need to go through each one as there may be multiple sessions each
  // with different entities registered
  const auto& borrower_collection_map = m_session_data.get_borrower_collection_map();
  for (const auto& cur_map_pair : borrower_collection_map)
  {
    auto cur_collection_id = cur_map_pair.first;
    auto cur_shm_pool_ids = cur_map_pair.second->get_shm_pool_ids();

    for (const auto& cur_shm_pool_id : cur_shm_pool_ids)
    {
      m_borrower_repository.deregister_shm_pool(m_remote_process_id, cur_collection_id, cur_shm_pool_id);
    }
    m_borrower_repository.deregister_collection(m_remote_process_id, cur_collection_id);
  }
  m_borrower_repository.deregister_owner(m_remote_process_id);
}

bool Shm_session::lend_arena(const shared_ptr<Ipc_arena>& arena)
{
  if (!m_connected)
  {
    // We can't lend anything when we're disconnected
    FLOW_LOG_TRACE("Disconnected, so ignoring arena lending attempt");
    return false;
  }

  Collection_id collection_id = arena->get_id();

  {
    Lock lock(m_shm_pool_listener_map_mutex);

    auto result_pair =
      m_shm_pool_listener_map.emplace(arena, make_unique<Shm_pool_listener_impl>(*this, collection_id));
    if (!result_pair.second)
    {
      FLOW_LOG_WARNING("Could not register duplicate arena [" << collection_id << "] for lending");
      return false;
    }

    if (!m_session_data.register_lender_collection(arena))
    {
      // We just added this, so this should work
      if (m_shm_pool_listener_map.erase(arena) == 0)
      {
        FLOW_LOG_FATAL("Could not remove arena [" << collection_id << "] from listener map we just registered");
        assert(false);
      }

      FLOW_LOG_WARNING("Failed to register arena [" << collection_id << "] to session data, possible internal error");
      return false;
    }

    // Compose and send message
    auto message = m_shm_channel.create_msg();
    auto lend_arena = message.body_root()->initLendArena();
    lend_arena.setCollectionId(collection_id);
    if (!send_sync_request(message, "lend arena"))
    {
      // Revert stuff already done
      // We just added this, so this should work
      if (!m_session_data.deregister_lender_collection(collection_id))
      {
        FLOW_LOG_FATAL("Could not deregister arena [" << collection_id << "] from session data we just registered");
        assert(false);
      }

      // We just added this, so this should work
      if (m_shm_pool_listener_map.erase(arena) == 0)
      {
        FLOW_LOG_FATAL("Could not remove arena [" << collection_id << "] from listener map we just registered");
        assert(false);
      }

      return false;
    }

    // After registering this, we will get a notification for the initial shared memory pools in the arena
    if (!arena->add_shm_pool_listener(result_pair.first->second.get()))
    {
      FLOW_LOG_FATAL("Could not insert listener we just created into arena [" << collection_id << "]");
      assert(false);
      return false;
    }
  }

  FLOW_LOG_TRACE("Successfully registered arena [" << collection_id << "] for lending");
  return true;
}

bool Shm_session::lend_shm_pools(Collection_id collection_id, const set<shared_ptr<Shm_pool>>& shm_pools)
{
  bool result = true;
  for (const auto& cur_shm_pool : shm_pools)
  {
    result = lend_shm_pool(collection_id, cur_shm_pool) && result;
  }

  return result;
}

bool Shm_session::lend_shm_pool(Collection_id collection_id, const shared_ptr<Shm_pool>& shm_pool)
{
  if (!m_connected)
  {
    // We can't lend anything when we're disconnected
    FLOW_LOG_TRACE("Disconnected, so ignoring pool lending attempt");
    return false;
  }

  if (!m_session_data.register_lender_shm_pool(collection_id, shm_pool))
  {
    // @todo - May need to indicate an error occurred here, so caller eventually will know - ECOGS-517
    return false;
  }

  // Compose and send message
  auto message = m_shm_channel.create_msg();
  auto lend_pool = message.body_root()->initLendPool();
  lend_pool.setCollectionId(collection_id);
  lend_pool.setPoolId(shm_pool->get_id());
  lend_pool.setPoolName(shm_pool->get_name());
  lend_pool.setPoolSize(shm_pool->get_size());
  if (!send_sync_request(message, "lend SHM pool"))
  {
    // Could not lend shared memory pool successfully, so undo registration as the other end cannot receive anything
    // within the pool
    bool was_empty;
    if (!m_session_data.deregister_lender_shm_pool(collection_id, shm_pool, was_empty))
    {
      FLOW_LOG_WARNING("Could not deregister SHM pool [" << shm_pool << "], collection id [" << collection_id <<
                       "] that was just added");
    }
    else if (!was_empty)
    {
      FLOW_LOG_WARNING("Deregistered just added SHM pool [" << shm_pool << "], collection id [" << collection_id <<
                       "], but it was not empty");
    }

    // @todo - May need to indicate an error occurred here, so caller eventually will know - ECOGS-517
    return false;
  }

  return true;
}

bool Shm_session::remove_lender_shm_pool(Collection_id collection_id, const shared_ptr<Shm_pool>& shm_pool)
{
  bool was_empty;
  if (!m_session_data.deregister_lender_shm_pool(collection_id, shm_pool, was_empty))
  {
    return false;
  }

  if (!m_connected)
  {
    // We can't communicate removal when we're disconnected
    FLOW_LOG_TRACE("Disconnected, so ignoring pool removal communication");
  }
  else if (!was_empty)
  {
    // We don't send a message to borrower when the pool is not empty as this may be a degenerate case where the
    // borrower may still have objects borrowed, but we're closing down the arena
    FLOW_LOG_TRACE("SHM pool [" << shm_pool->get_id() << "] was not empty, so ignoring informing borrower");
  }
  else
  {
    // Compose and send message
    auto message = m_shm_channel.create_msg();
    auto remove_pool = message.body_root()->initRemovePool();
    remove_pool.setCollectionId(collection_id);
    remove_pool.setPoolId(shm_pool->get_id());
    send_message(message, "remove SHM pool");
  }

  // We return success even if we can't/don't send a message, because we shouldn't be utilizing the pool anymore
  return true;
}

Shm_session::Blob Shm_session::lend_object_impl(const std::shared_ptr<void>& object)
{
  if (!m_connected)
  {
    // We can't lend anything when we're disconnected
    FLOW_LOG_TRACE("Disconnected, so ignoring object lending attempt");
    return Blob();
  }

  Collection_id collection_id = Ipc_arena::get_collection_id(object);

  pool_id_t shm_pool_id;
  pool_offset_t pool_offset;
  if (!m_session_data.register_lender_object(collection_id, object, shm_pool_id, pool_offset))
  {
    return Blob();
  }

  return serialize_handle(collection_id, shm_pool_id, pool_offset);
}

Shm_session::Blob Shm_session::serialize_handle(Collection_id collection_id,
                                                pool_id_t shm_pool_id,
                                                pool_offset_t pool_offset) const
{
  // Construct blob of appropriate size. @todo This is constant-sized, and per to-do elsewhere we can switch to array<>.
  Blob blob(sizeof(Shm_object_handle));
  uint8_t* blob_ptr = blob.data();
  // Set values
  auto object_handle = reinterpret_cast<Shm_object_handle*>(blob_ptr);
  object_handle->m_collection_id = collection_id;
  object_handle->m_pool_id = shm_pool_id;
  object_handle->m_pool_offset = pool_offset;

  return blob;
}

bool Shm_session::deserialize_handle(const Blob& blob,
                                     Collection_id& collection_id,
                                     pool_id_t& shm_pool_id,
                                     pool_offset_t& pool_offset) const
{
  // Deserialize with sanity-check

  size_t blob_size = blob.size();
  if (blob_size != sizeof(Shm_object_handle))
  {
    FLOW_LOG_WARNING("Blob size [" << blob_size << "] does not match expected "
                     "size [" << sizeof(Shm_object_handle) << "].");
    return false;
  }

  const uint8_t* blob_ptr = blob.const_data();
  const auto object_handle = reinterpret_cast<const Shm_object_handle*>(blob_ptr);
  collection_id = object_handle->m_collection_id;
  shm_pool_id = object_handle->m_pool_id;
  pool_offset = object_handle->m_pool_offset;

  return true;
}

bool Shm_session::return_object(Collection_id collection_id, pool_id_t shm_pool_id, pool_offset_t pool_offset)
{
  if (!m_session_data.deregister_borrower_object(collection_id, shm_pool_id, pool_offset))
  {
    return false;
  }

  if (!m_connected)
  {
    // We can't communicate removal when we're disconnected
    FLOW_LOG_TRACE("Disconnected, so ignoring object removal communication");
  }
  else
  {
    // Compose and send message
    auto message = m_shm_channel.create_msg();
    auto return_object = message.body_root()->initReturnObject();
    return_object.setCollectionId(collection_id);
    return_object.setPoolId(shm_pool_id);
    return_object.setPoolOffset(pool_offset);
    send_message(message, "remove object");
  }

  // We return success whether we can't/don't communicate successfully with lender as we won't use the object anymore
  return true;
}

bool Shm_session::send_message(const Shm_channel::Msg_out& message,
                               const string& operation,
                               const Shm_channel::Msg_in* original_message)
{
  flow::Error_code ec;
  if (!m_shm_channel.send(message, original_message, &ec))
  {
    FLOW_LOG_WARNING("Failed to send message on already hosed channel, operation [" << operation << "]");
    return false;
  }

  if (ec)
  {
    FLOW_LOG_WARNING("Failed to send message on channel, operation [" << operation << "], error [" << ec << "]");
    // Execute error callback
    m_shm_channel_error_handler(ec);
    return false;
  }

  FLOW_LOG_TRACE("Successfully sent [" << operation << "] message");
  return true;
}

bool Shm_session::send_sync_request(const Shm_channel::Msg_out& message, const string& operation)
{
  flow::Error_code ec;
  auto response = m_shm_channel.sync_request(message, nullptr, m_shm_channel_request_timeout, &ec);
  if (!response && !ec)
  {
    FLOW_LOG_WARNING("Failed to send request on already hosed channel, operation [" << operation << "]");
    return false;
  }

  if (ec)
  {
    if (ec == ipc::transport::error::Code::S_TIMEOUT)
    {
      FLOW_LOG_WARNING("Failed to complete transaction within the timeout [" << m_shm_channel_request_timeout << "]");
    }
    else
    {
      FLOW_LOG_WARNING("Failed to complete request on channel, operation [" << operation << "], error [" << ec << "]");
    }
    // Execute error callback
    m_shm_channel_error_handler(ec);
    return false;
  }

  if (!response->body_root().getResponse().getSuccess())
  {
    // Borrower could not complete action successfully
    FLOW_LOG_WARNING("Remote peer failed to complete action, operation [" << operation << "]");
    return false;
  }

  FLOW_LOG_TRACE("Successfully fulfilled [" << operation << "] sync request");
  return true;
}

bool Shm_session::send_response(const Shm_channel::Msg_in* original_message,
                                const string& operation,
                                bool success)
{
  assert(original_message != nullptr);

  // Compose and send message
  auto message = m_shm_channel.create_msg();
  auto response = message.body_root()->initResponse();
  response.setSuccess(success);
  return send_message(message, operation, original_message);
}

bool Shm_session::register_expected_messages()
{
  return (
    m_shm_channel.expect_msgs(
      schema::IpcShmMessage::Which::LEND_ARENA,
      [this](auto&& req) mutable
      {
        m_serial_task_loop.post(
          [this, req = std::move(req)]()
          {
            const auto& reader = req->body_root().getLendArena();
            send_response(req.get(), "lend arena response", receive_arena(reader.getCollectionId()));
          });
      }) &&
    m_shm_channel.expect_msgs(
      schema::IpcShmMessage::Which::LEND_POOL,
      [this](auto&& req) mutable
      {
        m_serial_task_loop.post(
          [this, req = std::move(req)]()
          {
            const auto& reader = req->body_root().getLendPool();
            send_response(req.get(), "lend pool response", receive_shm_pool(reader.getCollectionId(),
                                                                            reader.getPoolId(),
                                                                            reader.getPoolName(),
                                                                            reader.getPoolSize()));
          });
      }) &&
    m_shm_channel.expect_msgs(
      schema::IpcShmMessage::Which::REMOVE_POOL,
      [this](const auto& req) mutable
      {
        m_serial_task_loop.post(
          [this, req = std::move(req)]()
          {
            const auto& reader = req->body_root().getRemovePool();
            receive_shm_pool_removal(reader.getCollectionId(), reader.getPoolId());
          });
      }) &&
    m_shm_channel.expect_msgs(
      schema::IpcShmMessage::Which::RETURN_OBJECT,
      [this](auto&& req) mutable
      {
        m_parallel_task_loop.post(
          [this, req = std::move(req)]()
          {
            const auto& reader = req->body_root().getReturnObject();
            receive_object_return(reader.getCollectionId(),
                                  reader.getPoolId(),
                                  reader.getPoolOffset());
          });
      }));
}

void Shm_session::deregister_expected_messages()
{
  m_shm_channel.undo_expect_msgs(schema::IpcShmMessage::Which::RETURN_OBJECT);
  m_shm_channel.undo_expect_msgs(schema::IpcShmMessage::Which::REMOVE_POOL);
  m_shm_channel.undo_expect_msgs(schema::IpcShmMessage::Which::LEND_POOL);
  m_shm_channel.undo_expect_msgs(schema::IpcShmMessage::Which::LEND_ARENA);
}

bool Shm_session::receive_arena(Collection_id collection_id)
{
  auto borrower_shm_pool_collection = m_borrower_repository.register_collection(m_remote_process_id, collection_id);
  if (borrower_shm_pool_collection == nullptr)
  {
    return false;
  }
  return m_session_data.register_borrower_collection(borrower_shm_pool_collection);
}

bool Shm_session::receive_shm_pool(Collection_id collection_id, pool_id_t shm_pool_id,
                                   const string& pool_name, pool_offset_t pool_size)
{
  auto shm_pool = m_borrower_repository.register_shm_pool(m_remote_process_id, collection_id,
                                                          shm_pool_id, pool_name, pool_size);
  if (shm_pool == nullptr)
  {
    return false;
  }

  if (!m_session_data.register_borrower_shm_pool(collection_id, shm_pool))
  {
    // Something went wrong, revert prior change
    if (!m_borrower_repository.deregister_shm_pool(m_remote_process_id, collection_id, shm_pool_id))
    {
      FLOW_LOG_WARNING("Could not deregister SHM pool ID [" << shm_pool_id << "], name [" << pool_name << "] "
                       "collection [" << collection_id <<
                       "], owner [" << m_remote_process_id << "] we just registered");
      return false;
    }
  }

  return true;
}

bool Shm_session::receive_shm_pool_removal(Collection_id collection_id, pool_id_t shm_pool_id)
{
  FLOW_LOG_TRACE("Received SHM pool removal for collection id [" << collection_id << "], SHM pool [" <<
                 shm_pool_id << "]");

  // Remove from local repository first
  if (!m_session_data.deregister_borrower_shm_pool(collection_id, shm_pool_id))
  {
    return false;
  }

  // Remove from process repository
  if (!m_borrower_repository.deregister_shm_pool(m_remote_process_id, collection_id, shm_pool_id))
  {
    FLOW_LOG_WARNING("Deregistered SHM pool [" << shm_pool_id << "], collection [" << collection_id << "], owner [" <<
                     m_remote_process_id << "] from session, but repository deregistration failed");
    return false;
  }

  return true;
}

bool Shm_session::receive_object_return(Collection_id collection_id,
                                        pool_id_t shm_pool_id,
                                        pool_offset_t pool_offset)
{
  return bool(m_session_data.deregister_lender_object(collection_id, shm_pool_id, pool_offset));
}

Shm_session::Shm_pool_listener_impl::Shm_pool_listener_impl(Shm_session& owner,
                                                            Collection_id collection_id) :
  m_owner(owner),
  m_collection_id(collection_id)
{
}

void Shm_session::Shm_pool_listener_impl::notify_initial_shm_pools(const set<shared_ptr<Shm_pool>>& shm_pools)
{
  m_owner.lend_shm_pools(m_collection_id, shm_pools);
}

void Shm_session::Shm_pool_listener_impl::notify_created_shm_pool(const shared_ptr<Shm_pool>& shm_pool)
{
  m_owner.lend_shm_pool(m_collection_id, shm_pool);
}

void Shm_session::Shm_pool_listener_impl::notify_removed_shm_pool(const shared_ptr<Shm_pool>& shm_pool)
{
  m_owner.remove_lender_shm_pool(m_collection_id, shm_pool);
}

std::ostream& operator<<(std::ostream& os, const Shm_session& val)
{
  // @todo Something more useful than just this?
  return os << '@' << &val;
}

} // namespace ipc::session::shm::arena_lend::jemalloc
