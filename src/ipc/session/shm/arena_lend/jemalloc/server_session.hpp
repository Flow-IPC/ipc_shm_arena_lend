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

#include "ipc/session/shm/arena_lend/jemalloc/jemalloc.hpp"
#include "ipc/shm/arena_lend/jemalloc/jemalloc.hpp"
#include "ipc/session/detail/shm/arena_lend/jemalloc/server_session_impl.hpp"
#include "ipc/session/shm/arena_lend/jemalloc/session.hpp"
#include "ipc/session/server_session.hpp"
#include "ipc/transport/struc/shm/shm_fwd.hpp"
#include "ipc/transport/transport_fwd.hpp"
#include <boost/move/make_unique.hpp>

namespace ipc::session::shm::arena_lend::jemalloc
{

// Types.

/**
 * Identical to session::Server_session in every way, except that it makes available two SHM arenas, from the
 * SHM-jemalloc provider (ipc::shm::arena_lend::jemalloc).  These SHM arenas (see #Arena doc header) have different
 * scopes:
 *   - Per-session-scope, accessible via session_shm(): meaning it shall be accessible only during the lifetime of
 *     this session, by this Session (via this accessor).
 *     - Hence this arena is created at the time `*this` enters PEER state (near Session_server::async_accept()
 *       successfully yielding `*this`).  When `*this` is destroyed or hosed, the arena is invalidated.
 *   - Per-app-scope, meaning its pool shall be accessible potentially beyond the lifetime of
 *     this session but rather until the generating Session_server (i.e., server process) shuts down, by any
 *     Session under that umbrella, now or in the future, as long as its Client_app equals that of `*this` session.
 *     - Hence this arena is created at the time the shm::arena_lend::jemalloc::Session_server (emitter of `*this`
 *       to user) first establishes a session with the *first* shm::arena_lend::jemalloc::Client_session matching
 *       the same Client_app as `*this`.  I.e., it's created at the same time as `this->session_shm()` is, if
 *       `*this` is the first such session; otherwise earlier.  When `*this` is destroyed or hosed (gracefully at
 *       least), the arena is *not* invalidated but persists.
 *
 * @note The opposing arena_lend::jemalloc::Client_session is similar but lacks the app_shm() #Arena.
 *
 * ### Explicitly constructing an object in SHM; lending to opposing side ###
 * This setup is symmetrical w/r/t a given
 * shm::arena_lend::jemalloc::Server_session<->shm::arena_lend::jemalloc::Client_session object pair.  Both
 * can create new objects (via respective `Arena::construct()` methods).  Both can write and, of course, read
 * within locally `construct()`ed objects.  Lastly each can transmit any outer
 * handle, returned by `construct()`, to the opposing side.  To do this
 * call lend_object(), transmit the returned `Blob` to the opposing side via any desired IPC form (probably a
 * transport::Channel or transport::struc::Channel), then recover a handle via borrow_object() on that opposing
 * side.  As of this writing access to objects on the borrowing side is read-only, for safety reasons,
 * but that could change (most likely becoming an optional mode) at the prerogative of ipc::shm::arena_lend
 * developers.  (Do note that ipc::shm::classic allows full write access on the borrower side, as in an
 * arena-sharing SHM-provider by definition both sides allocate and thus write to the same SHM pool(s).
 * However this comes at the expense of safety.  Further discussion is up to ipc::shm docs.)
 *
 * @note Be ready for Session_mv::lend_object() to return an empty blob and Session_mv::borrow_object() to return
 *       a null pointer, both indicating an error: the session is hosed (opposing process likely down).  See their
 *       doc headers for a bit more info on this.
 *
 * By using `shm::stl::Arena_activator<Arena>` (alias shm::arena_lend::jemalloc::Ipc_arena_activator) and
 * #Allocator (alias shm::arena_lend::jemalloc::Ipc_arena_allocator)
 * it is possible to construct and transmit not just POD (Plain Old Datatype) objects but combinations of those
 * with unlimited nested levels of STL-compliant containers.  On the borrowing side use
 * #Borrower_allocator (no activator necessary).
 *
 * Detailed instructions can be found in those shm::stl and ipc::shm::arena_lend doc headers.
 *
 * ### Using SHM as backing memory for transport::struc::Channel ###
 * Another use of session_shm() and app_shm() (though only on this session-server side in case of the latter) --
 * in some ways perhaps superior -- is indirect.  The above technique involves manually constructing a
 * C++ data structure and transmitting a short handle to it over IPC thus achieving high performance.
 * It may be desirable, instead, to use a structured message, filled out according to schema.  This, of course,
 * is why transport::struc::Channel exists.  As is explained in its docs, there is nothing SHM-centered about it.
 * It does however have the key feature that allows one to supply, at compile-time, the builder and reader engines
 * that acquire memory while the user mutates an out-message and later accesses it upon IPC-receipt.  Naturally one of
 * the builder/reader pairs uses SHM, instead of regular heap, as the supplier of RAM.  Thus when
 * one invokes transport::struc::Channel::send() and receives it in the opposing object, the actual bits
 * copied into/out of the low-level transport are merely the SHM handle (and all of this is hidden from the user
 * outside of the delightful near-zero-copy perf properties).
 *
 * There are two ways to make this happen.  The easiest and best way is, when constructing the
 * `struc::Channel`, to use the alias session::shm::arena_lend::jemalloc::Session_mv::Structured_channel and
 * create one via tag-form ctor with tag transport::struc::Channel_base::Serialize_via_session_shm
 * (or `Serialize_via_app_shm`, server-side only).  Simply provide that tag and `this` (or, symmetrically,
 * `shm::arena_lend::jemalloc::Client_session::this` on the other side).
 *
 * The harder way, albeit allowing for certain advanced setups, is to manually create a
 * `transport::struc::shm::arena_lend::jemalloc::Builder::Config` and/or `Reader::Config`,
 * passing in `this->session_shm()` and/or `this->app_shm()` and/or `this->session_shm()`, to those;
 * and then pass the `Config` or `Config`s, plus `this->shm_session()`, to the non-tag-form of `struc::Channel`
 * ctor.
 *
 * This is all documented on transport::struc::Channel.  Do realize, though, that those niceties are
 * really built on this class template and/or the opposing shm::arena_lend::jemalloc::Client_session.
 * To use them with ipc::session, you must thus choose shm::arena_lend::jemalloc::Server_session and
 * shm::arena_lend::jemalloc::Client_session as your Session impls.
 *
 * @internal
 * ### Implementation ###
 * It is probably not too difficult to understand how it works by just reading the code and doc headers.
 * The key is that the core is still just what's supplied by a vanilla (non-SHM-aware) #Server_session.
 * Ultimately all we need to add is some setup near the end of the vanilla session-opening procedure.
 * This is easiest to discuss holistically, meaning both the server (`*this`) and client (opposing) side,
 * so the discussion is all here.
 *
 * As with the vanilla session setup, remember that while in PEER state the APIs are identical/symmetrical,
 * the roles of the server and client vary quite sharply internally -- especially before PEER state.
 * The general outline is: Session_server listens for socket connections; a given
 * `Client_session::sync_connect()` connects; at this stage Session_server constructs a not-yet-user-emitted
 * #Server_session and invokes its `async_accept_log_in()`.  Now the #Client_session and #Server_session
 * have established a channel (internal-use session master channel) and undergo the log-in exchange.
 * #Client_session sends log-in request.  #Server_session verifies that, then replies with log-in response
 * (including important bits of generated naming) and enters PEER state.  Once #Client_session (shortly) receives
 * this response, it knows the session is ready to rock and also enters PEER state.  Voila.
 *
 * Where does SHM-jemalloc setup hook into this?  The simpler case is the session-client which manages only
 * one #Arena (per-session scope: session_shm()), so let's describe it first.  In arena-lending SHM-providers,
 * what it takes to construct and lend/borrow object in SHM is:
 *   - An #Arena (managed in this process).  Things are constructed (allocated) by this.
 *   - A #Shm_session (not to be confused with ipc::Session and company).  `Arena`-constructed objects are
 *     *lent* (`lend_object()`) via this object.  Opposing process's `Arena`-constructed objects are
 *     locally *borrowed* (`borrow_object()`) via this object.
 *   - The #Arena must be registered with the #Shm_session (`lend_arena()`) during setup.
 *
 * In some ways this is simpler than in SHM-classic, as in that one the arena is shared by the two processes, so
 * one (we chose server) must create it and keep handle; while the other (client) must open it by a known name.
 * In our case each side maintains its own arena, so the order does not matter per se.  There is however one
 * added complication:
 *   - For the #Shm_session to be created, on either side, it must be passed a pre-opened `struc::Channel`
 *     of a specific type which it shall use for internally-needed IPC.  Specifically it just wants a
 *     transport::Socket_stream_channel_of_blobs which internally consists of a pre-connected local stream socket
 *     (Unix domain socket).  Much as with user-requested channels in vanilla sessions, we assign the session-server
 *     side the task of creating the socket-pair, sending one socket (transport::Native_handle) to client,
 *     and expecting a quick ack message before proceeding to create #Shm_session.
 *   - Accordingly the client side awaits the message (along session master channel) containing the socket,
 *     acks it, and creates the #Shm_session.
 *
 * To summarize, then, this is the flow for Client_session_impl::async_connect().
 *   -# Vanilla session::Client_session_impl::async_connect() gets it to PEER state.  Cancel PEER state immediately
 *      in our substituted on-connect-done handler.
 *   -# Await the `Native_handle` to arrive on the session master channel (which was just used for vanilla
 *      session opening procedure and is now not being used).  Upon receipt send ack message and using the
 *      received handle:
 *   -# Create #Shm_session (Client_session_impl::shm_session() shall make it available).
 *      - Session_impl::init_shm() does this.
 *   -# Create per-session arena (Client_session_impl::session_shm() shall make it available).
 *      - Session_impl::init_shm() does this.
 *   -# Register the latter with the former.
 *      - Session_impl::init_shm() does this.
 *   -# Re-enter PEER state; and invoke the true (user-supplied) on-connect-done handler.
 *
 * If the server had symmetrical needs only, it'd be easy to describe its algorithm -- basically mirror
 * the above.  However it also has the per-app arena (app_shm()).  It must be created, but only when an incoming
 * session-open request is the first for its Client_app.  (This logic is above the layer of sessions and must
 * reside in Session_server, not in Server_session_impl; but the two interact.)  Later, regardless of whether it
 * was just created, app_shm() must be registered with the #Shm_session, once that is created.  Thus on the
 * server side the async_accept_log_in() steps are:
 *   -# Vanilla async_accept_log_in() gets it to almost-PEER state.
 *      -# During the session::Session_server_impl "per-Client_app setup" customization-point hook:
 *         Session_server: Create the per-app arena for that Client_app and save it in its app-to-arena map, unless
 *         it is already there (then do nothing).
 *      -# During the session::Server_session_impl `pre_rsp_setup_func` hook that executes shortly after that:
 *         Cache the appropraite Session_server::app_shm() inside `*this` Server_session_impl, for quick
 *         subsequent access via Server_session::app_shm().
 *   -# Before invoking the user's on-accept-done handler -- giving them their new `Server_session` --
 *      perform the following *synchronous* steps:
 *      -# Create the pre-connected native-handle pair (socket-pair).
 *      -# Synchronously send it via session master channel (which was just used for vanilla session opening
 *         procedure and is now not being used) and get ack from client (see above).
 *   -# Create #Shm_session (Client_session_impl::shm_session() shall make it available).
 *      - Session_impl::init_shm() does this.
 *   -# Create per-session arena (Client_session_impl::session_shm() shall make it available).
 *      - Session_impl::init_shm() does this.
 *   -# Register the latter with the former.
 *      - Session_impl::init_shm() does this.
 *   -# Register app_shm() (cached a few steps earlier) with the former (#Shm_session) also.
 *      - Session_impl::init_shm() does this.
 *   -# Invoke the true (user-supplied) on-accept-done handler.
 *
 * The last and most boring piece of the puzzle are the pImpl-lite wrappers around the SHM-specific API
 * that shm::arena_lend::jemalloc::Server_session adds to super-class Server_session_mv: `session_shm()` and so on.
 * Because this API is (with the exception of app_shm() (and buddies) being server-only) exactly identical for
 * both shm::arena_lend::jemalloc::Server_session and shm::arena_lend::jemalloc::Client_session,
 * to avoid torturous copy/pasting these common wrappers are collected in shm::arena_lend::jemalloc::Session_mv
 * which we sub-class.  So the `public` API is distributed as follows from bottom to top:
 *   - shm::arena_lend::jemalloc::Server_session
 *     => shm::arena_lend::jemalloc::Session_mv => session::Server_session_mv => session::Session_mv
 *     => shm::arena_lend::jemalloc::Server_session_impl
 *   - shm::arena_lend::jemalloc::Client_session
 *     == shm::arena_lend::jemalloc::Session_mv => session::Client_session_mv => session::Session_mv
 *     => shm::arena_lend::jemalloc::Client_session_impl
 *     - The bottom two in this case are not even inherited but one aliases to the other.  This is because
 *       there's nothing added API-wise on the client side.  On the server side, as you see below, at least
 *       app_shm() is necessary to add, so we can't alias and must sub-class.
 *
 * You'll notice, like, `Base::Base::Base::Impl` and `Base::Base::Base::Impl()` in certain code below;
 * that's navigating the above hierarchy.
 *
 * @endinternal
 *
 * @tparam S_MQ_TYPE_OR_NONE
 *         Identical to session::Server_session.
 * @tparam S_TRANSMIT_NATIVE_HANDLES
 *         Identical to session::Server_session.
 * @tparam Mdt_payload
 *         Identical to session::Server_session.
 */
template<session::schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
class Server_session : public Session_mv
                                <session::Server_session_mv
                                   <Server_session_impl<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>>>
{
public:
  // Types.

  /// Short-hand for our base class.  To the user: note its `public` API is inherited.
  using Base = Session_mv
                 <session::Server_session_mv
                    <Server_session_impl<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>>>;

  /// Short-hand for base class member alias Session_mv::Arena.  Its doc header contains useful context and tips.
  using Arena = typename Base::Arena;

  /// Short-hand for base class member alias Session_mv::Structured_msg_builder_config.
  using Structured_msg_builder_config = typename Base::Structured_msg_builder_config;

  /**
   * Server_session::Vat_network and `Client_session::Vat_network` are reasonable concrete types
   * of template transport::struc::shm::rpc::Session_vat_network for an ipc::session user to use on opposing
   * sides of a session; use the mainstream-form ctor to straightforwardly construct your zero-copy-enabled
   * `Vat_network` (from a `*this`) for blazing-fast capnp-RPC.
   */
  using Vat_network = typename Base::Vat_network;

  /// You may disregard.
  using Async_io_obj = transport::Null_peer;
  /// Useful for generic programming, the `sync_io`-pattern counterpart to `*this` type.
  using Sync_io_obj = sync_io::Server_session_adapter<Server_session>;

  // Constants.

  /**
   * Implements Session API per contract: equals `true`.
   * @internal
   * This is explicitly here (it's inherited anyway) mostly to document it as being available at this level.
   */
  static constexpr bool S_IS_SRV_ELSE_CLI = Base::S_IS_SRV_ELSE_CLI;

  // Constructors/destructor.

  /// Inherit default, move ctors.
  using Base::Base;

  // Methods.

  /**
   * Returns SHM #Arena with per-app scope, meaning its pool shall be accessible potentially beyond the lifetime of
   * this session but rather until the generating Session_server (i.e., server process) shuts down, by any
   * Session under that umbrella, now or in the future, as long as its Client_app equals that of `*this` session.
   *
   * @note A similar #Arena is *not* available in the opposing #Client_session.
   *
   * See #Arena doc header for useful instructions on working with #Arena, lend_object(), and borrow_object().
   *
   * @return See above.
   */
  Arena* app_shm();

  /**
   * Identical to app_shm() but returns that via a `shared_ptr`-handle as required for work directly within
   * ipc::shm::arena_lend::jemalloc APIs.  Note this is a quirk of that particular API and, in particular, has
   * no equivalent in ipc::session::shm::classic `Session` and `Session_server` counterparts.
   * It should not be necessary to use except when going beyond ipc::session for your SHM-arena needs.
   *
   * @return See above.
   */
  std::shared_ptr<Arena> app_shm_ptr();

  /**
   * Returns builder config suitable for capnp-serializing out-messages in SHM arena app_shm().
   *
   * @return See above.
   */
  Structured_msg_builder_config app_shm_builder_config();

  /**
   * When transmitting items originating in #Arena app_shm() via transport::struc::shm::Builder::emit_serialization()
   * (and/or transport::struc::Channel send facilities), returns additional-to-payload information necessary to
   * target the opposing process properly.
   *
   * Internally: Since `*this` type is based on arena-lending SHM-provider type, this is simply shm_session().
   * Generic code of yours should not need to rely on that impl detail, but we mention it for education/context.
   *
   * @return See above.
   */
  typename Structured_msg_builder_config::Builder::Session app_shm_lender_session();

protected:
  // Constructors.

  /**
   * For use by internal user Session_server_impl: constructor.  Identical to Server_session_mv ctor.
   *
   * @param logger_ptr
   *        See Server_session_mv ctor.
   * @param srv_app_ref
   *        See Server_session_mv ctor.
   * @param master_channel_sock_stm
   *        See Server_session_mv ctor.
   */
  explicit Server_session(flow::log::Logger* logger_ptr, const Server_app& srv_app_ref,
                          transport::Native_socket_stream&& master_channel_sock_stm);
}; // class Server_session

// Free functions: in *_fwd.hpp.

// Template implementations.

/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define TEMPLATE_JEM_SRV_SESSION \
  template<session::schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, \
           typename Mdt_payload>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define CLASS_JEM_SRV_SESSION \
  Server_session<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>

TEMPLATE_JEM_SRV_SESSION
CLASS_JEM_SRV_SESSION::Server_session(flow::log::Logger* logger_ptr, const Server_app& srv_app_ref_arg,
                                       transport::Native_socket_stream&& master_channel_sock_stm)
{
  Base::Base::Base::impl() = boost::movelib::make_unique<typename Base::Base::Base::Impl>
                               (logger_ptr, srv_app_ref_arg, std::move(master_channel_sock_stm));
}

// The rest just continues the pImpl-lite paradigm, same as our public super-class.

TEMPLATE_JEM_SRV_SESSION
typename CLASS_JEM_SRV_SESSION::Arena* CLASS_JEM_SRV_SESSION::app_shm()
{
  assert(Base::Base::Base::impl() && "Do not call this on as-if-default-cted Session.");
  return Base::Base::Base::impl()->app_shm();
}

TEMPLATE_JEM_SRV_SESSION
std::shared_ptr<typename CLASS_JEM_SRV_SESSION::Arena> CLASS_JEM_SRV_SESSION::app_shm_ptr()
{
  assert(Base::Base::Base::impl() && "Do not call this on as-if-default-cted Session.");
  return Base::Base::Base::impl()->app_shm_ptr();
}

TEMPLATE_JEM_SRV_SESSION
typename CLASS_JEM_SRV_SESSION::Structured_msg_builder_config
  CLASS_JEM_SRV_SESSION::app_shm_builder_config()
{
  assert(Base::Base::Base::impl() && "Do not call this on as-if-default-cted Session.");
  return Base::Base::Base::impl()->app_shm_builder_config();
}

TEMPLATE_JEM_SRV_SESSION
typename CLASS_JEM_SRV_SESSION::Structured_msg_builder_config::Builder::Session
  CLASS_JEM_SRV_SESSION::app_shm_lender_session()
{
  assert(Base::Base::Base::impl() && "Do not call this on as-if-default-cted Session.");
  return Base::Base::Base::impl()->app_shm_lender_session();
}

TEMPLATE_JEM_SRV_SESSION
std::ostream& operator<<(std::ostream& os, const CLASS_JEM_SRV_SESSION& val)
{
  return os << static_cast<const typename CLASS_JEM_SRV_SESSION::Base&>(val);
}

#undef CLASS_JEM_SRV_SESSION
#undef TEMPLATE_JEM_SRV_SESSION

} // namespace ipc::session::shm::arena_lend::jemalloc
