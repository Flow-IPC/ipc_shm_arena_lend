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

/// @file
#pragma once

#include "ipc/shm/arena_lend/owner_shm_pool_listener_for_repository.hpp"
#include "ipc/shm/classic/pool_arena.hpp"
#include "ipc/session/detail/shm/arena_lend/jemalloc/session_impl.hpp"
#include "ipc/session/detail/client_session_impl.hpp"
#include "ipc/session/error.hpp"

namespace ipc::session::shm::arena_lend::jemalloc
{

// Types.

/**
 * Core internally-used implementation of shm::arena_lend::jemalloc::Client_session: it is to the latter what its
 * `public` super-class Client_session_impl is to #Client_session.
 *
 * @see shm::arena_lend::jemalloc::Client_session doc header which covers both its public behavior/API and a
 *      detailed sketch of the entire implementation.
 *
 * @tparam S_MQ_TYPE_OR_NONE
 *         See shm::arena_lend::jemalloc::Client_session counterpart.
 * @tparam S_TRANSMIT_NATIVE_HANDLES
 *         See shm::arena_lend::jemalloc::Client_session counterpart.
 * @tparam Mdt_payload
 *         See shm::arena_lend::jemalloc::Client_session counterpart.
 */
template<session::schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
class Client_session_impl :
  public Session_impl<session::Client_session_impl<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload,
                                                   session::schema::ShmType::JEMALLOC,
                                                   // Session_base::Graceful_finisher doc header explains why true here:
                                                   true>>
{
public:
  // Types.

  /// Short-hand for our non-`virtual` base.
  using Base = Session_impl
                 <session::Client_session_impl<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload,
                                               session::schema::ShmType::JEMALLOC, true>>;

  // Constructors/destructor.

  /**
   * See session::Client_session_impl::Client_session_impl() counterpart.  Internally, in addition, we set up
   * a SHM-pool cleanup algorithm.
   *
   * @param logger_ptr
   *        See above.
   * @param cli_app_ref
   *        See above.
   * @param srv_app_ref
   *        See above.
   * @param on_err_func
   *        See above.
   * @param on_passive_open_channel_func
   *        See above.
   */
  template<typename On_passive_open_channel_handler, typename Task_err>
  explicit Client_session_impl(flow::log::Logger* logger_ptr,
                               const Client_app& cli_app_ref, const Server_app& srv_app_ref,
                               Task_err&& on_err_func,
                               On_passive_open_channel_handler&& on_passive_open_channel_func);

  /**
   * See session::Client_session_impl::Client_session_impl() counterpart.  Internally, in addition, we set up
   * a SHM-pool cleanup algorithm.
   *
   * @param logger_ptr
   *        See above.
   * @param cli_app_ref
   *        See above.
   * @param srv_app_ref
   *        See above.
   * @param on_err_func
   *        See above.
   */
  template<typename Task_err>
  explicit Client_session_impl(flow::log::Logger* logger_ptr,
                               const Client_app& cli_app_ref, const Server_app& srv_app_ref,
                               Task_err&& on_err_func);

  /**
   * Destructor.
   * Internally: at least does what it must according to session::Client_session_impl::dtor_async_worker_stop()
   * doc header contract.  See inside for comment more resembling English hopefully.
   */
  ~Client_session_impl();

  // Methods.

  /**
   * See Client_session_mv counterpart.
   *
   * @param err_code
   *        See Client_session_mv counterpart.
   * @return See Client_session_mv counterpart.
   */
  bool sync_connect(Error_code* err_code);

  /**
   * See Client_session_mv counterpart.
   *
   * @param mdt
   *        See Client_session_mv counterpart.
   * @param init_channels_by_cli_req_pre_sized
   *        See Client_session_mv counterpart.
   * @param mdt_from_srv_or_null
   *        See Client_session_mv counterpart.
   * @param init_channels_by_srv_req
   *        See Client_session_mv counterpart.
   * @param err_code
   *        See Client_session_mv counterpart.
   * @return See Client_session_mv counterpart.
   */
  bool sync_connect(const typename Base::Base::Base::Mdt_builder_ptr& mdt,
                    typename Base::Base::Base::Channels* init_channels_by_cli_req_pre_sized,
                    typename Base::Base::Base::Mdt_reader_ptr* mdt_from_srv_or_null,
                    typename Base::Base::Base::Channels* init_channels_by_srv_req,
                    Error_code* err_code);

  // The LOG_*() macros don't see Log_context::get_log*() from base otherwise....
  using flow::log::Log_context::get_logger;
  using flow::log::Log_context::get_log_component;

private:
  // Methods.

  /**
   * See session::Client_session_impl counterpart.
   *
   * @param mdt
   *        See session::Client_session_impl counterpart.
   * @param init_channels_by_cli_req_pre_sized
   *        See session::Client_session_impl counterpart.
   * @param mdt_from_srv_or_null
   *        See session::Client_session_impl counterpart.
   * @param init_channels_by_srv_req
   *        See session::Client_session_impl counterpart.
   * @param on_done_func
   *        See session::Client_session_impl counterpart.
   * @return See session::Client_session_impl counterpart.
   */
  template<typename Task_err>
  bool async_connect(const typename Base::Base::Base::Mdt_builder_ptr& mdt,
                     typename Base::Base::Base::Channels* init_channels_by_cli_req_pre_sized,
                     typename Base::Base::Base::Mdt_reader_ptr* mdt_from_srv_or_null,
                     typename Base::Base::Base::Channels* init_channels_by_srv_req,
                     Task_err&& on_done_func);

  /// Invoked on #m_async_cleanup_worker, performs a round of the SHM-pool cleanup algorithm and schedules same.
  void cleanup();

  // Data.

  /**
   * This guy needs to be around, with each arena (in our case, out of the box, just session_shm()) registered
   * against it, for shm::stl::Arena_allocator (and thus STL/allocator-compliant data structures, including
   * containers like our own transport::struc::shm::Capnp_message_builder::Segments_in_shm) to work properly with that
   * arena.
   *
   * Details:
   *   - It is recommended there are not tons of these lying around, but in a production application there
   *     should be one session::Client_session around at a time at most.  So keeping this here makes sense.
   *   - It is *not* a problem if there is more than one per process; so if there are more sessions around
   *     in the process, it won't cause any problem.  It's not a singleton situation.
   *
   * There is also one in each Session_server.
   */
  ipc::shm::arena_lend::Owner_shm_pool_listener_for_repository m_shm_arena_pool_listener;

  /**
   * Thread used for low-priority periodic low-priority cleanup work.  See cleanup().
   *
   * ### Rationale ###
   * Why not piggy-back on session::Client_session_impl's thread W?  Answer: That guy actually does important stuff
   * sometimes; we would not want some endless scan through /dev/shm (or whatever) to delay a channel opening
   * or some-such.  So just parallelize it.
   */
  flow::async::Single_thread_task_loop m_async_cleanup_worker;
}; // class Client_session_impl

// Free functions: in *_fwd.hpp.

// Template implementations.

/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define TEMPLATE_JEM_CLI_SESSION_IMPL \
  template<session::schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define CLASS_JEM_CLI_SESSION_IMPL \
  Client_session_impl<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>

TEMPLATE_JEM_CLI_SESSION_IMPL
template<typename On_passive_open_channel_handler, typename Task_err>
CLASS_JEM_CLI_SESSION_IMPL::Client_session_impl(flow::log::Logger* logger_ptr,
                                                const Client_app& cli_app_ref, const Server_app& srv_app_ref,
                                                Task_err&& on_err_func,
                                                On_passive_open_channel_handler&& on_passive_open_channel_func) :
  Base(logger_ptr, cli_app_ref, srv_app_ref, std::move(on_err_func), std::move(on_passive_open_channel_func)),
  m_shm_arena_pool_listener(get_logger()),
  m_async_cleanup_worker(get_logger(), flow::util::ostream_op_string("jem_cli_sess_cln[", *this, ']'))
{
  m_async_cleanup_worker.start();
  m_async_cleanup_worker.post([this]() { cleanup(); });
}

TEMPLATE_JEM_CLI_SESSION_IMPL
template<typename Task_err>
CLASS_JEM_CLI_SESSION_IMPL::Client_session_impl(flow::log::Logger* logger_ptr,
                                                const Client_app& cli_app_ref, const Server_app& srv_app_ref,
                                                Task_err&& on_err_func) :
  Base(logger_ptr, cli_app_ref, srv_app_ref, std::move(on_err_func)),
  m_shm_arena_pool_listener(get_logger()),
  m_async_cleanup_worker(get_logger(), flow::util::ostream_op_string("jem_cli_sess_cln[", *this, ']'))
{
  m_async_cleanup_worker.start();
  m_async_cleanup_worker.post([this]() { cleanup(); });
}

TEMPLATE_JEM_CLI_SESSION_IMPL
CLASS_JEM_CLI_SESSION_IMPL::~Client_session_impl()
{
  // See explanation in ~Server_session_impl().  Same deal here.
  Base::Base::dtor_async_worker_stop();
  // Thread W has been joined.
}

TEMPLATE_JEM_CLI_SESSION_IMPL
bool CLASS_JEM_CLI_SESSION_IMPL::sync_connect(Error_code* err_code)
{
  return sync_connect(Base::Base::mdt_builder(), nullptr, nullptr, nullptr, err_code);
}

TEMPLATE_JEM_CLI_SESSION_IMPL
bool CLASS_JEM_CLI_SESSION_IMPL::sync_connect(const typename Base::Base::Base::Mdt_builder_ptr& mdt,
                                              typename Base::Base::Base::Channels* init_channels_by_cli_req_pre_sized,
                                              typename Base::Base::Base::Mdt_reader_ptr* mdt_from_srv_or_null,
                                              typename Base::Base::Base::Channels* init_channels_by_srv_req,
                                              Error_code* err_code)
{
  using flow::async::Task_asio_err;

  Function<bool (Task_asio_err&&)> async_connect_impl_func = [&](Task_asio_err&& on_done_func) -> bool
  {
    return async_connect(mdt, init_channels_by_cli_req_pre_sized, mdt_from_srv_or_null, init_channels_by_srv_req,
                         std::move(on_done_func));
  };
  return Base::Base::sync_connect_impl(err_code, &async_connect_impl_func);
}

TEMPLATE_JEM_CLI_SESSION_IMPL
template<typename Task_err>
bool CLASS_JEM_CLI_SESSION_IMPL::async_connect
       (const typename Base::Base::Base::Mdt_builder_ptr& mdt,
        typename Base::Base::Base::Channels* init_channels_by_cli_req_pre_sized,
        typename Base::Base::Base::Mdt_reader_ptr* mdt_from_srv_or_null,
        typename Base::Base::Base::Channels* init_channels_by_srv_req,
        Task_err&& on_done_func)
{
  using flow::util::ostream_op_string;
  using boost::make_shared;
  using boost::shared_ptr;
  using std::to_string;
  using Channels = typename Base::Base::Base::Channels;
  using Mdt_reader_ptr = typename Base::Base::Base::Mdt_reader_ptr;
  using Master_structured_channel = typename Base::Base::Base::Master_structured_channel;

  /* This being an arena-lending style of SHM-provider, it's quite different and somewhat more complex than
   * shm::classic to deal with; but basically there are two items to set up: the Arena(s) where stuff is allocated
   * on *this* side; and the Shm_session which can lend from said Arena(s) and borrow from *any* opposing Arena(s)
   * Session_server sets up.  They're just objects which are synchronously constructed, and that's it.  However
   * Shm_session requires a Channel over which to conduct internal communications needed by this and opposing
   * Shm_session.  So that's the main difficulty, as it's asynchronous.  That said the base vanilla Client_impl
   * provides some protected methods -- cancel_peer_state_to_connecting() on --
   * that make it pretty simple.  The Channel, specifically, just needs a single
   * bidirectional Native_socket_stream.  Same as when opening that type of channel for the user, Session_server
   * creates a socket-pair and sends us one of the `Native_handle`s (FDs) to us.  So we simply expect_msg() it
   * on the session-master-channel; we do it after the vanilla async_connect() completes successfully (the log-in
   * and all that).  OK: let's go.  First do the vanilla async_connect() but substitute our additional steps
   * as the handler, remembering the real user-provided handler to invoke eventually, god willing.
   *
   * Okay, another wrinkle (same as in shm::classic equivalent async_connect()); don't set the out-args, until
   * we are really successful; so put stuff into some intermediate targets first. */

  auto temp_init_channels_by_cli_req_pre_sized
    = init_channels_by_cli_req_pre_sized ? make_shared<Channels>(init_channels_by_cli_req_pre_sized->size())
                                         : shared_ptr<Channels>();
  auto temp_mdt_from_srv_or_null
    = mdt_from_srv_or_null ? make_shared<Mdt_reader_ptr>() : shared_ptr<Mdt_reader_ptr>();
  auto temp_init_channels_by_srv_req
    = init_channels_by_srv_req ? make_shared<Channels>() : shared_ptr<Channels>();

  return Base::Base::async_connect(mdt,
                                   temp_init_channels_by_cli_req_pre_sized.get(),
                                   temp_mdt_from_srv_or_null.get(),
                                   temp_init_channels_by_srv_req.get(),
                                   [this, on_done_func = std::move(on_done_func),
                                    init_channels_by_cli_req_pre_sized, mdt_from_srv_or_null, init_channels_by_srv_req,
                                    temp_init_channels_by_cli_req_pre_sized, // @todo Avoid s_p<> copies here.
                                    temp_mdt_from_srv_or_null,
                                    temp_init_channels_by_srv_req]
                                     (const Error_code& async_err_code)
  {
    // We are in thread W (of vanilla Client_session_impl).

    if (async_err_code)
    {
      // No questions asked.  GTFO.
      on_done_func(async_err_code);
      return;
    }
    // else: Cool; now do our added steps.

    /* We're in PEER state... but as foreshadowed above, undo that.  Provide, as required, the new handler (like us...
     * but this time really-really final) that will run once either *we*
     * eventually complete_async_connect_after_canceling_peer_state(success/failure); or a master-channel error
     * hoses *this.  So this is the final stage of this whole thing. */
    const auto master_channel
      = Base::Base::cancel_peer_state_to_connecting([on_done_func = std::move(on_done_func),
                                                     init_channels_by_cli_req_pre_sized,
                                                     mdt_from_srv_or_null,
                                                     init_channels_by_srv_req,
                                                     // @todo Avoid s_p<> copies here --v.
                                                     temp_init_channels_by_cli_req_pre_sized,
                                                     temp_mdt_from_srv_or_null,
                                                     temp_init_channels_by_srv_req]
                                                      (const Error_code& err_code)
    {
      // We are in thread W (of vanilla Client_session_impl).

      if (!err_code)
      {
        // Still good!  Finalize out-args.
        if (init_channels_by_cli_req_pre_sized)
        {
          *init_channels_by_cli_req_pre_sized = std::move(*temp_init_channels_by_cli_req_pre_sized);
        }
        if (mdt_from_srv_or_null)
        {
          *mdt_from_srv_or_null = std::move(*temp_mdt_from_srv_or_null);
        }
        if (init_channels_by_srv_req)
        {
          *init_channels_by_srv_req = std::move(*temp_init_channels_by_srv_req);
        }
      } // if (!err_code)

      on_done_func(err_code); // err_code could be truthy.
    }); // cancel_peer_state_to_connecting()

    /* That was aspirational and hopeful.  Now, by cancel_peer_state_to_connecting() contract, actually kick off the
     * the actions that will async-lead to complete_async_connect_after_canceling_peer_state() which would
     * cause the above handler to finally run (unless a session-master-channel error does so first).
     *
     * Namely we just async-await the message with the Native_handle for the internal-use Channel.  So:
     * m_master_channel->expect_msg(..., [...](Msg_in_ptr m) { post([..., m]() { handle in-message m; }); });
     * But m_master_channel is private in super-class, and that super-class instead gave us this nice
     * protected utility to use: */
    master_channel->expect_msg(Master_structured_channel::Msg_which_in::JEMALLOC_SHM_SETUP,
                               [this, master_channel]
                                 (typename Master_structured_channel::Msg_in_ptr&& msg) mutable
    {
      // We are in thread Wc (unspecified, really struc::Channel async callback thread).
      Base::Base::async_worker()->post([this, master_channel, msg = std::move(msg)]() mutable
      {
        // We are in thread W.
        if (Base::Base::Base::hosed())
        {
          // Must have emitted error somehow elsewhere.  async_connect() will fail.
          return;
        }
        // else

        // That's the entire payload of the in-message.
        auto local_hndl_or_null = msg->native_handle_or_null();

        if (local_hndl_or_null.null())
        {
          FLOW_LOG_WARNING("Client session [" << *this << "]: Session-connect request: Vanilla async-connect"
                           "succeeded, but opposing server failed to create the resources (pre-connected native handle "
                           "pair for local stream socket connection) necessary for arena-lending SHM provider's "
                           "internal IPC needs.  Will go back to NULL state and report to user via "
                           "on-async-connect handler.");
          Base::Base::complete_async_connect_after_canceling_peer_state
            (session::error::Code::S_SESSION_OPEN_CHANNEL_SERVER_CANNOT_PROCEED_RESOURCE_UNAVAILABLE);
          return;
        }
        /* else: We can make Channel, and upgrade to struc::Channel, with no further potential for errors.
         *       Our super-class (shm::...::Session_mv) can handle those details and everything else including
         *       setting up the Arena.  That stuff is the same on the server and client, except server also sets
         *       up an extra arena; but we don't have any extra steps.  So that's it!
         *
         * Well, one last thing actually: The cleanup algorithm started/contained in our ctor depends on being able
         * to parse-out the PID of the creating process from the pool name as obtained by
         * doing Pool_arena::for_each_persistent() which trolls /dev/shm (in Linux at least).  Hence we shall place
         * our PID at the expect place in the name via the mechanism provided for such things by init_shm(). */

        FLOW_LOG_INFO("Client session [" << *this << "]: Session-connect request: Vanilla async-connect succeeded, "
                      "and opposing server sent a resource (pre-connected native handle for "
                      "local stream socket connection) necessary for arena-lending SHM provider's internal "
                      "IPC needs.  Will send ack and complete local SHM setup synchronously.");

        // Don't forget to ack it.
        Error_code err_code;
        if (!master_channel->send(master_channel->create_msg(), msg.get(), &err_code))
        {
          if (err_code)
          {
            // The send() detected new master_channel hosing.
            Base::Base::complete_async_connect_after_canceling_peer_state(err_code);
          }
          // else: Must have emitted error somehow elsewhere.  async_connect() will fail.
          return;
        }
        // else

        /* So just init_shm() left.
         * It might have failed; if so, then that's that (nothing else to undo); or conversely, if it succeeds,
         * on this side there's nothing further to do either.  So just report the result of init_shm(). */
        err_code = Base::init_shm(std::move(local_hndl_or_null),
                                  Shared_name::ct(to_string(util::Process_credentials::own_process_id())));

        if (!err_code)
        {
          /* I lied (Arnie voice from Commando).  We also need to do this for each arena (we have just the one).
           * Need this for arena->construct<T>(), where T is allocator-compliant thing such as STL containers,
           * to work. */
#ifndef NDEBUG
          const bool ok =
#endif
          Base::session_shm()->add_shm_pool_listener(&m_shm_arena_pool_listener);
          assert(ok && "Maintenance bug?  It should only fail if already registered.");
        }

        Base::Base::complete_async_connect_after_canceling_peer_state(err_code);
      }); // async_worker()->post()
    }); // master_channel->expect_msg()

    // if (returned false) { Must have emitted error somehow elsewhere.  async_connect() will fail. }
  }); // return async_connect()
} // Client_session_impl::async_connect()

TEMPLATE_JEM_CLI_SESSION_IMPL
void CLASS_JEM_CLI_SESSION_IMPL::cleanup()
{
  // `classic`?  Pool_arena?!  But this is jemalloc!?!?!  Worry not.  See remove_each_persistent_if() doc header.
  using ipc::shm::classic::Pool_arena;
  using util::String_view;
  using util::process_id_t;
  using boost::chrono::seconds;
  using boost::lexical_cast;

  constexpr util::Fine_duration CLEANUP_PERIOD = seconds(30);

  FLOW_LOG_TRACE("Client session [" << *this << "]: Periodic (or initial) cleanup starting.");

  /* This is just like Session_server::cleanup(), so see that guy for discussion first.  The differences here:
   *   - Like Session_server, we decode the pool creator's PID from a given pool's name; but in our case the
   *     creator is a client, so the srv_namespace() -- the server process's PID -- is not the right thing.
   *     That's why we added it specifically into the name in async_connect() when invoking Base::init_shm().
   *     So now we just parse it right out of there instead of the standard srv_namespace component.
   *   - Not a really-real difference but good to note: In practice (in production anyway) there's only one
   *     Session_server per process ever, so probably one Session_server's async cleanup algorithm won't
   *     be running concurrently with another.  There can definitely be multiple `Client_session`s though, albeit
   *     (in production anyway) not usually concurrently (but even then it's quite conceivable one Client_session
   *     is hosed but not destroyed as a C++ object; while another has been created; it's not recommended but should
   *     be harmless).  So there's a higher potential, arguably, for our async cleanup()-started algorithm
   *     concurrently running with another for the same Client_app.  It is fine however: again, the whole point of it
   *     is to only touch pools created by PIDs that are not around, not even in zombie form; and if one is indeed
   *     identified, then the worst thing that can possibly happen is the remove_persistent() (unlink) attempt
   *     will fail due to being unnecessary -- this is perfectly fine (we even suppress logs/errors, IIRC).
   *     Anyway: Noting here FYI.
   *
   * That said proceed in the (almost) same way as Session_server.  Keeping comments light. */

  const auto n_removed
    = util::remove_each_persistent_if<Pool_arena>(get_logger(),
                                                  [&](const Shared_name& shm_pool_name)
  {
    Shared_name resource_type;
    Shared_name cli_app_name;
    Shared_name the_rest;

    if (!(decompose_conventional_shared_name(shm_pool_name,
                                             &resource_type,
                                             nullptr, // srv_app_name: Needs to be there, but we don't care what it is.
                                             nullptr, // srv_namespace: Ditto.
                                             &cli_app_name,
                                             nullptr, // cli_namespace_or_sentinel: Ditto.
                                             &the_rest)
          && (resource_type == Shared_name::S_RESOURCE_TYPE_ID_SHM)
          && (cli_app_name == Base::Base::Base::cli_app_ptr()->m_name)
          && String_view(the_rest.str()).starts_with((SHM_SUBTYPE_PREFIX + Shared_name::S_SEPARATOR).str())))
    {
      // Not our pool to possibly delete.  Misnamed; or not by this Client_app; or not from this SHM-provider.
      return false;
    }
    // else

    /* We must parse out the client-PID (as noted in big comment above).
     * the_rest should look like: SHM_SUBTYPE_PREFIX / -->client-PID<-- / more-stuff.
     * Hence make past_prefix point to: ---------------^ */
    String_view past_prefix(the_rest.str());
    past_prefix.remove_prefix(SHM_SUBTYPE_PREFIX.str().size() + 1);
    // Now cut off from here -------------------------------------------^.
    const size_t sep_pos = past_prefix.find_first_of(Shared_name::S_SEPARATOR);
    if (sep_pos == String_view::npos)
    {
      // Created by server; not us.
      return false;
    }
    // else
    past_prefix.remove_suffix(past_prefix.size() - sep_pos);

    process_id_t pid = 0;
    try
    {
      pid = lexical_cast<process_id_t>(past_prefix);
    }
    catch (...) {}

    if (pid == 0)
    {
      FLOW_LOG_WARNING("Client session [" << *this << "]: During periodic cleanup found seemingly relevant "
                       "(based on prefix/form) SHM-pool named [" << shm_pool_name << "]; but client-PID term "
                       "[" << past_prefix << "] is not a valid-looking PID.  Ignoring; but this warrants "
                       "investigation.");
      return false;
    }
    // else

    if (util::process_running(pid))
    {
      return false;
    }
    // else

    FLOW_LOG_INFO("Client session [" << *this << "]: During periodic cleanup found SHM-pool "
                  "named [" << shm_pool_name << "]; appears to have been created by a process of this application "
                  "that is no longer running, even as a zombie; perhaps it exited without graceful cleanup.  Will "
                  "attempt to remove it.  Competing cleanup(s) may be occurring concurrently; that is fine.");
    return true;
  }); // remove_each_persistent_if()
  if (n_removed == 0)
  {
    FLOW_LOG_TRACE("Client session [" << *this << "]: Cleanup finished: remove none.");
  }
  else
  {
    FLOW_LOG_INFO("Client session [" << *this << "]: Cleanup finished: Removed [" << n_removed << "] pools "
                  "successfully.");
  }

  m_async_cleanup_worker.schedule_from_now(CLEANUP_PERIOD, [this](auto&&) { cleanup(); });
} // Client_session_impl::cleanup()

TEMPLATE_JEM_CLI_SESSION_IMPL
std::ostream& operator<<(std::ostream& os, const CLASS_JEM_CLI_SESSION_IMPL& val)
{
  return os << static_cast<const typename CLASS_JEM_CLI_SESSION_IMPL::Base&>(val);
}

#undef CLASS_JEM_CLI_SESSION_IMPL
#undef TEMPLATE_JEM_CLI_SESSION_IMPL

} // namespace ipc::session::shm::arena_lend::jemalloc
