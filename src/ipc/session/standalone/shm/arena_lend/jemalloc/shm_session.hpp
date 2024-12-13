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

#include "ipc/session/standalone/shm/arena_lend/ipc_shm_message.capnp.h"
#include "ipc/session/standalone/shm/arena_lend/shm_session_data.hpp"
#include "ipc/session/standalone/shm/arena_lend/arena_lend_fwd.hpp"
#include "ipc/shm/arena_lend/jemalloc/jemalloc_fwd.hpp"
#include "ipc/shm/arena_lend/owner_shm_pool_listener.hpp"
#include "ipc/transport/struc/heap_serializer.hpp"
#include "ipc/transport/struc/channel.hpp"
#include <flow/util/basic_blob.hpp>
#include <atomic>

namespace ipc::session::shm::arena_lend::jemalloc
{

/**
 * Manages shared memory data related to communication with another entity.
 */
class Shm_session :
  public flow::log::Log_context,
  public std::enable_shared_from_this<Shm_session>
{
public:
  /**
   * Alias for an unstructured channel for our internal use.
   *
   * ### Choice of transport::Channel type ###
   * We don't need to to transmit native handles, so it'll
   * have just a blobs pipe; so as of this writing that leaves 3 possibilities (perusing channel.hpp):
   *   - transport::Posix_mqs_channel_of_blobs (2 POSIX MQs facing each other);
   *   - transport::Bipc_mqs_channel_of_blobs (2 SHM-backed boost.ipc-supplied MQs facing each other -- the SHM
   *     system is not us, so this isn't an infinite loop);
   *   - transport::Socket_stream_channel_of_blobs (1 bidirectional local stream, a/k/a Unix-domain stream socket).
   *
   * All would work, and tests show perf differences are minute for small messages like ours. In terms of ease of
   * setup of an existing channel `Socket_stream_channel_of_blobs` is hard to beat (just send over a single
   * native handle); and in terms of low-maintenance of cleanup even more so (just close the handle on each side;
   * done). So let's go with that.
   * @todo Should #Shm_channel_base type be compile-time-configurable?
   */
  using Shm_channel_base = transport::Socket_stream_channel_of_blobs<true>;
  /**
   * Alias for a structured channel for our internal sue.
   *
   * The basic choices are transport::struc::Channel_via_heap and `transport::struc::shm::*::Channel`.
   * The latter is pretty crazy.  That'd be, as of this writing, either
   * transport::struc::shm::arena_lend::jemalloc::Channel
   * (infinite compile-time recursion!) or transport::struc::shm::classic::Channel (which is somewhat more
   * conceivable but a huge pain in the butt our chief user, namely ipc::session::shm::Session_mv and company --
   * they'd have to set up SHM-classic just to have a chance to set up SHM-jemalloc).  Anyway clearly we must use
   * the non-zero-copy transport::struc::Channel_via_heap.  Our messages are small, so it is a good use of it.
   */
  using Shm_channel = transport::struc::Channel_via_heap<Shm_channel_base, schema::IpcShmMessage>;
  /**
   * Alias for a light-weight blob. They're little; TRACE-logging of deallocs and copies is of low value;
   * otherwise this can be switched to `flow::util::Blob`.
   */
  using Blob = flow::util::Blob_sans_log_context;

  /**
   * Creates an instance of this class. The motivation for utilizing a shared pointer is that borrowed object
   * handles refer back to the instance that generated the handle.
   *
   * @param logger Used for logging purposes.
   * @param borrower_repository The repository where borrower collection information is stored. The instance will
   *                            use this borrowed reference, which must be valid for the object's lifetime. This
   *                            is generally used to funnel information from multiple sessions into one, such that
   *                            it is shared instead of similar SHM pools potentially opened more than once. This
   *                            makes the utilization of offset pointers feasible in a multi-session to one process
   *                            environment.
   * @param shm_channel The channel used for transmitting shared memory pool messages. The instance will use
   *                    this borrowed reference, which must be valid for the object's lifetime.
   * @param shm_channel_error_handler Callback executed when a channel error code is emitted when using the
   *                                  shm_channel.
   * @param shm_channel_request_timeout The timeout when sending a synchronous request using the shm_channel.
   *
   * @return An instance of this class.
   *
   * @see ipc::shm::arena_lend::Shm_pool_offset_ptr
   */
  static std::shared_ptr<Shm_session> create(
    flow::log::Logger* logger,
    Borrower_shm_pool_collection_repository& borrower_repository,
    Shm_channel& shm_channel,
    flow::async::Task_asio_err&& shm_channel_error_handler,
    util::Fine_duration shm_channel_request_timeout = util::Fine_duration::max());

  /// Destructor.
  ~Shm_session();

  /**
   * Registers an arena for lending to the borrower on the other end of the session. There are multiple tasks that
   * are performed here.
   * 1. The arena is placed in a (unique) registry.
   * 2. The borrower is told to register the collection id of the arena.
   * 3. We listen for events within the arena, which may cause lending of shared memory pools to the borrower.
   *
   * @param arena The arena to be lent.
   *
   * @return Whether registration was successful.
   */
  bool lend_arena(const std::shared_ptr<ipc::shm::arena_lend::jemalloc::Ipc_arena>& arena);

  /**
   * Serializes and registers an object for lending to the borrower on the other end of the session. The user
   * of this interface should transmit the serialized object to the borrower, who should deserialize it via
   * #borrow_object. The serialization contents include information on how to deserialize the object and not
   * contents of the object itself.
   *
   * @warning Be ready for this method returning empty blob. Assuming you provided proper inputs, this indicates session
   *          is hosed; in particular no further lending/borrowing is likely to work. Most likely the opposing process
   *          is down or chose to close the session.
   *
   * @param object The object to be lent.
   *
   * @return If successful, the serialized handle of the object, which should be transmitted by the caller (lender)
   *         and deserialized by the borrower. If unsuccessful, an empty blob.
   */
  template <typename T>
  Blob lend_object(const std::shared_ptr<T>& object);

  /**
   * Deserializes and registers a handle to an object for borrowing from a lender on the other end of the session.
   * The caller must have prior knowledge of what type the serialized object is. When the object is released (i.e.,
   * shared pointer handle count drops to zero), the object will be deregistered and communicated back to the lender.
   *
   * @warning Be ready for this method returning null. Assuming you provided proper inputs, this indicates the session
   *          is hosed; in particular no further lending/borrowing is likely to work. Most likely the opposing process
   *          is down or chose to close the session.
   *
   * @tparam T The object type that is being borrowed.
   * @param serialized_object A serialized handle of an object, which was previously serialized via #lend_object.
   *
   * @return If successful, the (deserialized) object. If unsuccessful, a null object.
   */
  template <typename T>
  std::shared_ptr<T> borrow_object(const Blob& serialized_object);

  /**
   * Sets the session as disconnected. This will prevent transmission of any messages to the remote peer.
   */
  inline void set_disconnected();

protected:
  /// Alias for a shared memory pool.
  using Shm_pool = ipc::shm::arena_lend::Shm_pool;
  /// Short-hand for pool ID type.
  using pool_id_t = Shm_pool::pool_id_t;
  /// Short-hand for pool offset type.
  using pool_offset_t = Shm_pool::size_t;

  /**
   * The prefix of a serialized object handle. The additional information is a variable length shared memory pool
   * identifier.
   */
  struct Shm_object_handle
  {
    /// The collection id this object was created in.
    Collection_id m_collection_id;
    /// Pool identifier.
    pool_id_t m_pool_id;
    /// The offset within the pool this object resides.
    pool_offset_t m_pool_offset;
  }; // class Shm_object_handle

  /**
   * Listener for shared memory pool events within the underlying collection.
   */
  class Shm_pool_listener_impl final :
    public ipc::shm::arena_lend::Owner_shm_pool_listener
  {
  public:
    /**
     * Constructor.
     *
     * @param owner The containing entity of this instance. The lifetime of it must be longer than this instance.
     * @param collection_id The collection id that the notifications are emitting from.
     */
    Shm_pool_listener_impl(Shm_session& owner, Collection_id collection_id);

    /**
     * Notification called upon initial registration in a synchronous manner; this will only be called once.
     *
     * @param shm_pools The current set of active SHM pools, which may be empty.
     */
    void notify_initial_shm_pools(const std::set<std::shared_ptr<Shm_pool>>& shm_pools) override;
    /**
     * Notification called upon creation of a shared memory pool.
     *
     * @param shm_pool The shared memory pool that was created.
     */
    void notify_created_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool) override;
    /**
     * Notification called upon removal of a shared memory pool. No objects should be resident within this
     * pool at the time of removal.
     *
     * @param shm_pool The shared memory pool that is being removed.
     */
    void notify_removed_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool) override;

  private:
    /// The containing entity of this instance, which lifetime must be longer than this instance.
    Shm_session& m_owner;
    /// The collection id that the notifications are emitting from.
    const Collection_id m_collection_id;
  }; // class Shm_pool_listener_impl

  /**
   * Constructor.
   *
   * @param logger Used for logging purposes.
   * @param borrower_repository The repository where borrower collection information is stored. The instance will
   *                            use this borrowed reference, which must be valid for the object's lifetime. This
   *                            is generally used to funnel information from multiple sessions into one, such that
   *                            it is shared instead of similar SHM pools potentially opened more than once. This
   *                            makes the utilization of offset pointers feasible in a multi-session to one process
   *                            environment.
   * @param shm_channel The channel used for transmitting shared memory pool messages. The instance will use
   *                    this borrowed reference, which should be valid for the object's lifetime.
   * @param shm_channel_error_handler Callback executed when a channel error code is emitted when using the
   *                                  shm_channel.
   * @param shm_channel_request_timeout The timeout when sending a synchronous request using the shm_channel.
   */
  Shm_session(flow::log::Logger* logger,
              Borrower_shm_pool_collection_repository& borrower_repository,
              Shm_channel& shm_channel,
              flow::async::Task_asio_err&& shm_channel_error_handler,
              util::Fine_duration shm_channel_request_timeout);

  /**
   * Returns the remote process id.
   *
   * @return See above.
   */
  inline util::process_id_t get_remote_process_id() const;

  /**
   * Registers a set of shared memory pools for lending and sends messages to the borrower to borrow the pools.
   *
   * @param collection_id The identifier of the collection where the shared memory pools are created in.
   * @param shm_pools The set of shared memory pools.
   *
   * @return Whether the operation was completely successful.
   *
   * @see lend_shm_pool
   */
  bool lend_shm_pools(Collection_id collection_id, const std::set<std::shared_ptr<Shm_pool>>& shm_pools);
  /**
   * Registers a shared memory pool for lending and sends a message to the borrower to borrow the pool. If there
   * is a failure, the shared memory pool will not be registered.
   *
   * @param collection_id The identifier of the collection where the shared memory pool is created in.
   * @param shm_pool The shared memory pool.
   *
   * @return Whether the operation was successful, which means that that the pool was registered, notified
   *         to the borrower, and acknowledged by the borrower as registered.
   */
  bool lend_shm_pool(Collection_id collection_id, const std::shared_ptr<Shm_pool>& shm_pool);
  /**
   * Non-template impl of lend_object(); the latter simply creates `shared_ptr<void>` as needed and invokes us.
   *
   * @todo For a slight perf bump it should be possible to write lend_object() without requiring the creation of
   * an additional `shared_ptr` object which currently occurs for all types except `void`. It might be doable with
   * even just an added `reinterpret_cast<const shared_ptr<void>&>`.
   *
   * @param object See lend_object().
   * @return See lend_object().
   */
  Blob lend_object_impl(const std::shared_ptr<void>& object);
  /**
   * Deregisters a shared memory pool for lending and if the pool is non-empty (i.e., does not have any
   * registered objects for it), sends a message to the borrower to remove the pool. Note that if we do not
   * communicate with the borrower, we still deregister the shared memory pool as we will no longer be using
   * it anymore.
   *
   * @param collection_id The identifier of the collection where the shared memory pool is created in.
   * @param shm_pool The shared memory pool.
   *
   * @return Whether the shared memory pool was successfully deregistered. Note that the result does not depend
   *         on whether we can communicate with the borrower or not.
   */
  bool remove_lender_shm_pool(Collection_id collection_id, const std::shared_ptr<Shm_pool>& shm_pool);
  /**
   * Serializes information for an object handle.
   *
   * @param collection_id The identifier for the collection where the object resides.
   * @param shm_pool_id The identifier of the shared memory pool where the object resides.
   * @param pool_offset The offset in the shared memory pool where the object resides.
   *
   * @return Serialized handle of the object.
   */
  Blob serialize_handle(Collection_id collection_id,
                        pool_id_t shm_pool_id,
                        pool_offset_t pool_offset) const;
  /**
   * Deserializes an object handle.
   *
   * @param serialized_object The serialized handle of an object.
   * @param collection_id The resulting identifier for the collection where the object resides.
   * @param shm_pool_id The resulting identifier of the shared memory pool where the object resides.
   * @param pool_offset The resulting offset in the shared memory pool where the object resides.
   *
   * @return Whether deserialization was successful, which would only fail if the size of the object
   *         is insufficient for proper deserialization.
   */
  bool deserialize_handle(const Blob& serialized_object,
                          Collection_id& collection_id,
                          pool_id_t& shm_pool_id,
                          pool_offset_t& pool_offset) const;
  /**
   * Deregisters a borrowed object and sends a message back to the lender indicating the return.
   *
   * @param collection_id The identifier for the collection where the object resides.
   * @param shm_pool_id The identifier of the shared memory pool where the object resides.
   * @param pool_offset The offset in the shared memory pool where the object resides.
   *
   * @return Whether the operation was successful, which would only fail if the object was not previously registered.
   */
  bool return_object(Collection_id collection_id, pool_id_t shm_pool_id, pool_offset_t pool_offset);
  /**
   * Sends a shared memory related message to the entity on the other end of the shared memory channel.
   *
   * @param message The message to send.
   * @param operation The use case that the message is related to.
   * @param original_message If non-null, the message is a response to a this request.
   *
   * @return Whether the message was sent successfully, which means it was added to the channel successfully.
   */
  bool send_message(const Shm_channel::Msg_out& message,
                    const std::string& operation,
                    const Shm_channel::Msg_in* original_message = nullptr);
  /**
   * Sends a shared memory related request to the entity on the other end of the shared memory channel and waits
   * for a response that the operation completed.
   *
   * @param message The message to send.
   * @param operation The use case that the message is related to.
   *
   * @return Whether the message was sent successfully and a successful response received.
   */
  bool send_sync_request(const Shm_channel::Msg_out& message, const std::string& operation);
  /**
   * Sends a shared memory related response to the entity on the other end of the shared memory channel. The
   * response is due to a prior sync request.
   *
   * @param original_message The original request messsage that was sent.
   * @param operation The use case that the response message is related to.
   * @param success Whether the operation was successful.
   *
   * @return Whether the response was sent successfully.
   */
  bool send_response(const Shm_channel::Msg_in* original_message, const std::string& operation, bool success);
  /**
   * Listens for new messages on the shared memory channel. Note that per specification, the handlers for the
   * messages will not be called concurrently to each other.
   *
   * @return Whether the operation was successful.
   */
  bool register_expected_messages();
  /**
   * Stop listening for new messages on the shared memory channel.
   */
  void deregister_expected_messages();

  /**
   * Borrows an arena shared by the lender.
   *
   * @param collection_id The identifier of the arena.
   *
   * @return Whether the operation was successful, which would only fail if the identifier has already been registered.
   */
  bool receive_arena(Collection_id collection_id);
  /**
   * Borrows a shared memory pool from the lender.
   *
   * @todo The (relatively) few times SHM pool names are mentioned in ipc::shm::arena_lend and
   * ipc::session::shm::arena_lend, the type util::Shared_name should be used, not string type(s).
   *
   * @param collection_id The identifier for the collection where the object resides.
   * @param shm_pool_id The identifier of the shared memory pool where the object resides.
   * @param pool_name Name of SHM-pool (as for `shm_open()`).
   * @param pool_size The size of the shared memory pool.
   *
   * @return Whether the operation was successful.
   */
  bool receive_shm_pool(Collection_id collection_id, pool_id_t shm_pool_id, const std::string& pool_name,
                        pool_offset_t pool_size);
  /**
   * Removes a shared memory pool that was previously registered from the lender.
   *
   * @param collection_id The identifier for the collection where the object resides.
   * @param shm_pool_id The identifier of the shared memory pool where the object resides.
   *
   * @return Whether the operation was successful.
   */
  bool receive_shm_pool_removal(Collection_id collection_id, pool_id_t shm_pool_id);
  /**
   * Removes a shared memory pool that was previously registered to be lent.
   *
   * @param collection_id The identifier for the collection where the object resides.
   * @param shm_pool_id The identifier of the shared memory pool where the object resides.
   * @param pool_offset The offset in the shared memory pool where the object resides.
   *
   * @return Whether the operation was successful.
   */
  bool receive_object_return(Collection_id collection_id, pool_id_t shm_pool_id, pool_offset_t pool_offset);

private:
  /// The mutex type.
  using Mutex = flow::util::Mutex_non_recursive;
  /// Exclusive lock for the mutex.
  using Lock = flow::util::Lock_guard<Mutex>;

  /**
   * Whether the session is connected, which it is assumed to be at initialization. Note that the value may change
   * within a method, so care should be taken to cache the value as needed. Once disconnected (i.e., set to false),
   * the value should not be set to true again.
   */
  std::atomic<bool> m_connected;
  /// The repository where borrower collection information is stored. This may be across sessions.
  Borrower_shm_pool_collection_repository& m_borrower_repository;
  /// The shared memory database that tracks data that is lent and borrowed by this session.
  Shm_session_data m_session_data;
  /// Mutex to protect access to #m_shm_pool_listener_map.
  Mutex m_shm_pool_listener_map_mutex;
  /// Maps an arena to an arena shared memory pool listener; this is used for receiving changes in the SHM pools.
  std::unordered_map<std::shared_ptr<ipc::shm::arena_lend::jemalloc::Ipc_arena>,
                     std::unique_ptr<Shm_pool_listener_impl>> m_shm_pool_listener_map;
  /// The channel used for transmitting shared memory pool messages.
  Shm_channel& m_shm_channel;
  /// The other end's process id cached from #m_shm_channel used for registering borrowed items.
  const util::process_id_t m_remote_process_id;
  /// Callback executed when a channel error code is emitted when using the shm_channel.
  const flow::async::Task_asio_err m_shm_channel_error_handler;
  /// The timeout when sending a request (that requires a response from remote).
  const util::Fine_duration m_shm_channel_request_timeout;
  /**
   * The task engine to process tasks in a serial manner. The only use case currently is incoming shared memory
   * channel messages from the lender. The messages must be serialized as out of ordering may cause issues. For
   * example, the issues may be:
   * 1. An object that is being borrowed is processed prior to the shared memory pool containing it.
   * 2. A shared memory pool being borrowed is processed prior to a shared memory pool being removed with the same
   *    address.
   * 3. An shared memory pool being borrowed is registered prior to the collection containing it.
   */
  flow::async::Single_thread_task_loop m_serial_task_loop;
  /// The task engine to process order agnostic tasks in a potentially parallel manner.
  flow::async::Cross_thread_task_loop m_parallel_task_loop;
}; // class Shm_session

/**
 * Prints string representation of the given `Shm_session` to the given `ostream`.
 *
 * @relatesalso Shm_session
 *
 * @param os Stream to which to write.
 * @param val Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Shm_session& val);

template <typename T>
std::shared_ptr<T> Shm_session::borrow_object(const Blob& serialized_object)
{
  Collection_id collection_id;
  pool_id_t shm_pool_id;
  pool_offset_t pool_offset;
  if (!deserialize_handle(serialized_object, collection_id, shm_pool_id, pool_offset))
  {
    return nullptr;
  }

  return m_session_data.construct_and_register_borrower_object<T>(
    collection_id,
    shm_pool_id,
    pool_offset,
    [session = shared_from_this(), collection_id, shm_pool_id, pool_offset] (void*)
    {
      session->return_object(collection_id, shm_pool_id, pool_offset);
    });
}

template <typename T>
Shm_session::Blob Shm_session::lend_object(const std::shared_ptr<T>& object)
{
  return lend_object_impl(object);
}

void Shm_session::set_disconnected()
{
  m_connected = false;
}

util::process_id_t Shm_session::get_remote_process_id() const
{
  return m_remote_process_id;
}

} // namespace ipc::session::shm::arena_lend::jemalloc
