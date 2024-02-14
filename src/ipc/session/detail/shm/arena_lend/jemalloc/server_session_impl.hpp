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

#include "ipc/session/detail/shm/arena_lend/jemalloc/session_impl.hpp"
#include "ipc/session/detail/server_session_impl.hpp"
#include "ipc/session/error.hpp"
#include <boost/move/make_unique.hpp>

namespace ipc::session::shm::arena_lend::jemalloc
{

// Types.

/**
 * Core internally-used implementation of shm::arena_lend::jemalloc::Server_session: it is to the latter what
 * its `public` super-class Server_session_impl is to #Server_session.
 *
 * @see shm::arena_lend::jemalloc::Server_session doc header which covers both its public behavior/API and a
 *      detailed sketch of the entire implementation.
 *
 * @tparam S_MQ_TYPE_OR_NONE
 *         See shm::arena_lend::jemalloc::Server_session counterpart.
 * @tparam S_TRANSMIT_NATIVE_HANDLES
 *         See shm::arena_lend::jemalloc::Server_session counterpart.
 * @tparam Mdt_payload
 *         See shm::arena_lend::jemalloc::Server_session counterpart.
 */
template<session::schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
class Server_session_impl :
  public Session_impl
           <session::Server_session_impl<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload,
                                         session::schema::ShmType::JEMALLOC,
                                         transport::struc::shm::Builder_base::S_MAX_SERIALIZATION_SEGMENT_SZ>>
{
public:
  // Types.

  /// Short-hand for our non-`virtual` base.
  using Base = Session_impl
                 <session::Server_session_impl<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload,
                                               session::schema::ShmType::JEMALLOC,
                                               transport::struc::shm::Builder_base::S_MAX_SERIALIZATION_SEGMENT_SZ>>;

  /// See shm::arena_lend::jemalloc::Server_session_mv counterpart for public description.
  using Arena = typename Base::Arena;

  /// See shm::arena_lend::jemalloc::Server_session_mv counterpart for public description.
  using Structured_msg_builder_config = typename Base::Structured_msg_builder_config;

  // Constructors/destructor.

  /// Inherit ctor.
  using Base::Base;

  /**
   * Destructor.
   * Internally: at least does what it must according to session::Server_session_impl::dtor_async_worker_stop()
   * doc header contract.  See inside for comment more resembling English hopefully.
   */
  ~Server_session_impl();

  // Methods.

  /**
   * For use by internal user Session_server: See Server_session_mv counterpart.  While the formal contract
   * here is unchanged from vanilla Server_session_impl, internally: this one, before `on_done_func()` can be invoked
   * successfully, takes care of a number of SHM setup tasks including establishing an internal-SHM-provider-use
   * IPC channel to the opposing Client_session_impl.
   *
   * @param srv
   *        See Server_session_mv counterpart.
   * @param init_channels_by_srv_req
   *        See Server_session_mv counterpart.
   * @param mdt_from_cli_or_null
   *        See Server_session_mv counterpart.
   * @param init_channels_by_cli_req
   *        See Server_session_mv counterpart.
   * @param cli_app_lookup_func
   *        See Server_session_mv counterpart.
   * @param cli_namespace_func
   *        See Server_session_mv counterpart.
   * @param pre_rsp_setup_func
   *        See Server_session_mv counterpart.
   * @param n_init_channels_by_srv_req_func
   *        See Server_session_mv counterpart.
   * @param mdt_load_func
   *        See Server_session_mv counterpart.
   * @param on_done_func
   *        See Server_session_mv counterpart.
   */
  template<typename Session_server_impl_t,
           typename Task_err, typename Cli_app_lookup_func, typename Cli_namespace_func, typename Pre_rsp_setup_func,
           typename N_init_channels_by_srv_req_func, typename Mdt_load_func>
  void async_accept_log_in(Session_server_impl_t* srv,
                           typename Base::Base::Channels* init_channels_by_srv_req,
                           typename Base::Base::Mdt_reader_ptr* mdt_from_cli_or_null,
                           typename Base::Base::Channels* init_channels_by_cli_req,
                           Cli_app_lookup_func&& cli_app_lookup_func, Cli_namespace_func&& cli_namespace_func,
                           Pre_rsp_setup_func&& pre_rsp_setup_func,
                           N_init_channels_by_srv_req_func&& n_init_channels_by_srv_req_func,
                           Mdt_load_func&& mdt_load_func,
                           Task_err&& on_done_func);

  /**
   * See shm::arena_lend::jemalloc::Server_session_mv counterpart.
   * @return See shm::arena_lend::jemalloc::Server_session_mv counterpart.
   */
  Arena* app_shm();

  /**
   * See shm::arena_lend::jemalloc::Server_session_mv counterpart.
   * @return See shm::arena_lend::jemalloc::Server_session_mv counterpart.
   */
  std::shared_ptr<Arena> app_shm_ptr();

  /**
   * See shm::arena_lend::jemalloc::Server_session_mv counterpart.
   * @return See shm::arena_lend::jemalloc::Server_session_mv counterpart.
   */
  Structured_msg_builder_config app_shm_builder_config();

  /**
   * See shm::arena_lend::jemalloc::Session_mv counterpart.
   * @return See shm::arena_lend::jemalloc::Session_mv counterpart.
   */
  typename Structured_msg_builder_config::Builder::Session app_shm_lender_session();

  // The LOG_*() macros don't see Log_context::get_log*() from base otherwise....
  using flow::log::Log_context::get_logger;
  using flow::log::Log_context::get_log_component;

private:
  // Data.

  /// See app_shm(): The cached value from Session_server::app_shm_ptr() applicable to our opposing Client_app.
  std::shared_ptr<Arena> m_app_shm;
}; // class Server_session_impl

// Free functions: in *_fwd.hpp.

// Template implementations.

/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define TEMPLATE_JEM_SRV_SESSION_IMPL \
  template<session::schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define CLASS_JEM_SRV_SESSION_IMPL \
  Server_session_impl<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>

TEMPLATE_JEM_SRV_SESSION_IMPL
CLASS_JEM_SRV_SESSION_IMPL::~Server_session_impl()
{
  /* Since we are subclass of Base::Base (session::Server_session_impl), and we do post our own tasks onto thread W
   * (otherwise maintained by Base::Base), this protected guy's doc header says we must call it.  So formally speaking
   * that's why we do it.  More in English, though: if a subclass of session::Server_session_impl (us, or perhaps
   * Base) has queued a task F on thread W (async_worker()), and this dtor is called, and F() touches some part of
   * *this or (Base&)(*this), then there can be undefined-behavior and/or concurrent-access trouble.  Once
   * Base::Base::~Base() is reached, we're safe, as it stop()s thread W in civilized fashion first-thing, synchronously,
   * before letting its own destruction continue.  So this causes the stop()page to occur somewhat earlier, so that
   * we too can feel peace: */
  Base::Base::dtor_async_worker_stop();
  // Thread W has been joined.
}

TEMPLATE_JEM_SRV_SESSION_IMPL
template<typename Session_server_impl_t,
         typename Task_err, typename Cli_app_lookup_func, typename Cli_namespace_func, typename Pre_rsp_setup_func,
         typename N_init_channels_by_srv_req_func, typename Mdt_load_func>
void CLASS_JEM_SRV_SESSION_IMPL::async_accept_log_in
       (Session_server_impl_t* srv,
        typename Base::Base::Channels* init_channels_by_srv_req,
        typename Base::Base::Mdt_reader_ptr* mdt_from_cli_or_null,
        typename Base::Base::Channels* init_channels_by_cli_req,
        Cli_app_lookup_func&& cli_app_lookup_func,
        Cli_namespace_func&& cli_namespace_func, Pre_rsp_setup_func&& pre_rsp_setup_func,
        N_init_channels_by_srv_req_func&& n_init_channels_by_srv_req_func, Mdt_load_func&& mdt_load_func,
        Task_err&& on_done_func)
{
  using util::Native_handle;
  namespace asio_local = transport::asio_local_stream_socket::local_ns;
  using asio_local::connect_pair;
  using transport::asio_local_stream_socket::Peer_socket;
  using boost::movelib::make_unique;

  // The extra stuff to do on top of the base vanilla Server_session_impl.
  auto real_pre_rsp_setup_func
    = [this,
       // Get the Session_server<> such that its core comprises the arg `srv`.
       srv = srv->this_session_srv(),
       pre_rsp_setup_func = std::move(pre_rsp_setup_func)]
        () -> Error_code
  {
    // We are in thread W.

    auto err_code = pre_rsp_setup_func();
    if (err_code)
    {
      // Any Session_server-given setup failed => no point in doing our SHM-classic per-session setup.
      return err_code;
    }
    // else

    /* Do our extra stuff on top of the base vanilla Server_session_impl.  That is Base::session_shm() must
     * work (return non-null), as must app_shm() (ditto, cached in this->m_app_shm); plus lend_object()ing
     * objects constructed from either must work, and borrow_object()ing from anything the opposing process
     * transmits must work.  The lend/borrow stuff will work if and only if session_shm() and app_shm() have
     * been registered with the lend/borrow engine, namely shm_session().
     * Incidentally Base::shm_session() must also be available (return non-null), in case the user wants to
     * access that guy directly.
     *
     * m_app_shm first: please see our class doc header; then come back here.  To summarize:
     *   - pre_rsp_setup_func() had to have created what we want app_shm() to return.
     *     - If it already existed by then (Client_app seen already), even better.
     *     - If that creation failed, then pre_rsp_setup_func() just failed, so we are not here.
     *   - Parent session server's app_shm(<the Client_app>) = what we want. */
    m_app_shm = srv->app_shm_ptr(*(Base::Base::Base::cli_app_ptr()));
    assert(m_app_shm && "How can it be null, if pre_rsp_setup_func() returned success?  Contract broken internally?");

    /* Now session_shm(), shm_session(), and registering of the former (and of app_shm()) into shm_session().
     * Base::init_shm() takes care of all of it.  However it's not time
     * for that yet: the log-in is not yet complete.  We can't do other master_channel() traffic yet (and we need
     * to for init_shm() purposes).  See below for the post-vanilla-async_accept_log_in() handler steps. */

    return err_code; // == Error_code().
  }; // auto real_pre_rsp_setup_func =

  Base::Base::async_accept_log_in(srv,
                                  init_channels_by_srv_req, mdt_from_cli_or_null, init_channels_by_cli_req,
                                  std::move(cli_app_lookup_func),
                                  std::move(cli_namespace_func),
                                  std::move(real_pre_rsp_setup_func),
                                  std::move(n_init_channels_by_srv_req_func),
                                  std::move(mdt_load_func),

                                  [this, srv = srv->this_session_srv(), on_done_func = std::move(on_done_func)]
                                    (const Error_code& async_err_code)
  {
    // We are in thread W (from Base).

    if (async_err_code == session::error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER)
    {
      return; // Stuff is shutting down.  GTFO.
    }
    // else

    auto err_code = async_err_code; // We need to modify it below.

    if (!err_code)
    {
      // Now resume from inside real_pre_rsp_setup_func().  init_shm() needs our side of the internal-use channel.

      FLOW_LOG_INFO("Server session [" << *this << "]: Vanilla accept-log-in succeeded; now we will send "
                    "a resource to opposing session-client to enable an internal-IPC channel for arena-lending "
                    "SHM-provider; and set up local SHM objects synchronously.  If it all succeeds we will "
                    "complete the accept-log-in.");

      Peer_socket local_hndl_asio(*(Base::async_worker()->task_engine()));
      Peer_socket remote_hndl_asio(std::move(local_hndl_asio));
      Error_code sys_err_code;
      connect_pair(local_hndl_asio, remote_hndl_asio, sys_err_code);
      if (sys_err_code)
      {
        FLOW_LOG_WARNING("Server session [" << *this << "]: While setting up SHM internal-use channel: "
                         "connect_pair() failed.  Details follow.");
        FLOW_ERROR_SYS_ERROR_LOG_WARNING(); // Log based on sys_err_code.
        err_code = sys_err_code;
      }
      else // if (!sys_err_code)
      {
        auto local_hndl = Native_handle(local_hndl_asio.release());
        auto remote_hndl = Native_handle(remote_hndl_asio.release());

        /* Before we do our local stuff we should enable the other side to do the complementary thing:
         * send them (with return ack) `remote_hndl`. */
        auto msg = Base::master_channel()->create_msg();
        msg.body_root()->initJemallocShmSetup();
        msg.store_native_handle_or_null(std::move(remote_hndl));
        // We don't care about contents of the ack (it's just an ack).
        if ((!Base::master_channel()->sync_request(msg, nullptr, &err_code)) // @todo Consider using finite timeout.
            && (!err_code))
        {
          /* Annoying corner case.  Incoming-direction error occurred on master_channel() before we could
           * sync_request(); it'll be reported through its on-error handler.  We really don't want to enter
           * async-land and wait for that, though: we're technically in almost-PEER state already and merely
           * haven't informed user of that via their on_done_func().  Let's just report a catch-all thing;
           * in any case the true error is logged when detected earlier inside struc::Channel. */
          err_code = error::Code::S_SERVER_MASTER_SHM_UNEXPECTED_TRANSPORT_ERROR;
        }
        /* else if ((!sync_request()) && err_code) { New error detected via sync_request(). }
         * else { All good so far.  Ready for init_shm()! } */

        if (!err_code)
        {
          err_code = Base::init_shm(std::move(local_hndl), Shared_name(), app_shm_ptr());
          /* If that returned falsy: Fantastic.  To quote our mission from earlier:
           * "Now session_shm(), shm_session(), and registering of the former (and of app_shm()) into shm_session().
           *  Base::init_shm() takes care of all of it."  It INFO-logged too. */

          if (!err_code)
          {
            /* One last thing.  We also need to do this for each arena (we have the two; but Session_server takes care
             * of it for app_shm() upon creating it; which -- reminder -- occurs only the first time a session for
             * that Client_app appears; we may be the 2nd/3rd/... one).
             * Need this for arena->construct<T>(), where T is allocator-compliant thing such as STL containers,
             * to work. */
#ifndef NDEBUG
            const bool ok =
#endif
            Base::session_shm()->add_shm_pool_listener(srv->shm_arena_pool_listener());
            assert(ok && "Maintenance bug?  It should only fail if already registered.");
          }

        } // if (!err_code) (but it may have become truthy inside)
      } // if (!sys_err_code)
    } // if (!err_code) (but it may have become truthy inside)

    // Be Aggressive in cleaning up early (early versus waiting for our dtor that is).
    if (err_code)
    {
      Base::reset_shm();
      // For cleanliness/does not matter.  (m_app_shm *pointee* can and must continue to exist for other sessions.)
      m_app_shm.reset();
    }

    on_done_func(err_code);
  }); // Base::Base::async_accept_log_in()
} // Server_session_impl::async_accept_log_in()

TEMPLATE_JEM_SRV_SESSION_IMPL
typename CLASS_JEM_SRV_SESSION_IMPL::Arena* CLASS_JEM_SRV_SESSION_IMPL::app_shm()
{
  return m_app_shm.get();
}

TEMPLATE_JEM_SRV_SESSION_IMPL
std::shared_ptr<typename CLASS_JEM_SRV_SESSION_IMPL::Arena> CLASS_JEM_SRV_SESSION_IMPL::app_shm_ptr()
{
  return m_app_shm;
}

TEMPLATE_JEM_SRV_SESSION_IMPL
typename CLASS_JEM_SRV_SESSION_IMPL::Structured_msg_builder_config
  CLASS_JEM_SRV_SESSION_IMPL::app_shm_builder_config()
{
  return Structured_msg_builder_config{ get_logger(), 0, 0, app_shm() };
}

TEMPLATE_JEM_SRV_SESSION_IMPL
typename CLASS_JEM_SRV_SESSION_IMPL::Structured_msg_builder_config::Builder::Session
  CLASS_JEM_SRV_SESSION_IMPL::app_shm_lender_session()
{
  return Base::shm_session();
}

TEMPLATE_JEM_SRV_SESSION_IMPL
std::ostream& operator<<(std::ostream& os, const CLASS_JEM_SRV_SESSION_IMPL& val)
{
  return os << static_cast<const typename CLASS_JEM_SRV_SESSION_IMPL::Base&>(val);
}

#undef CLASS_JEM_SRV_SESSION_IMPL
#undef TEMPLATE_JEM_SRV_SESSION_IMPL

} // namespace ipc::session::shm::arena_lend::jemalloc
