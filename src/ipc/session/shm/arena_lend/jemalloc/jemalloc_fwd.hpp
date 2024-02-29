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

#include "ipc/transport/struc/schema/common.capnp.h"
#include "ipc/session/detail/shm/arena_lend/jemalloc/jemalloc_fwd.hpp"

/**
 * Bundles ipc::session::shm support for the various arena-lend-style SHM-providers, as of this writing
 * at least ipc::session::shm::arena_lend::jemalloc.
 */
namespace ipc::session::shm::arena_lend { }
/* @todo There is probably a better place for that doc header; which would be finalized once SHM-jemalloc proper
 * has been integrated into the Doxygen-docs run (as of this writing it is not quite yet, even though the
 * non-standalone/ SHM-jemalloc session items are so integrated). */

/**
 * Support for SHM-backed ipc::session sessions and session-servers with the SHM-jemalloc
 * (ipc::shm::arena_lend::jemalloc) provider.  See the doc header for the general ipc::session::shm namespace.
 */
namespace ipc::session::shm::arena_lend::jemalloc
{

// Types.

// Find doc headers near the bodies of these compound types.

template<typename Session_t>
class Session_mv;

template<typename Server_session_impl_t>
class Server_session_mv;
template<typename Client_session_impl_t>
class Client_session_mv;

template<session::schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES,
         typename Mdt_payload = ::capnp::Void>
class Server_session;

template<session::schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES,
         typename Mdt_payload = ::capnp::Void>
class Session_server;

/**
 * This is to session::Client_session what shm::arena_lend::jemalloc::Server_session is to session::Server_session.
 * In terms of additional (to that of #Client_session) public behavior, from the public API user's point of view,
 * this class template alias is almost identical to shm::arena_lend::jemalloc::Server_session (see its doc header).
 * (That is during PEER state.)  There is however one difference:
 *
 * #Client_session lacks an `app_shm()` method (as well as the related APIs
 * `app_shm_builder_config()` and `app_shm_lender_session()`).  This is due to the nature of arena-lending
 * SHM-providers.  Namely each side maintains its own `Arena`s in which only it can allocate (`Arena::construct()`);
 * and by definition of session-client, there is no longer-than-lifetime object to maintain any app-scope
 * `Arena`s.  In a session-server process, by contrast, there is Session_server which does
 * just that and maintains `app_shm()`; hence `Server_session` can return one of those.
 *
 * @internal
 * ### Implementation ###
 * First see the Implementation section of shm::arena_lend::jemalloc::Server_session.  It gives a detailed
 * description of the overall design -- both its own side and this (client) side too.  So read it; then come back here.
 *
 * That said, shm::arena_lend::jemalloc::Client_session is a much simpler situation.  It is not inter-operating with
 * any server object; and (as explained in the referenced doc header) all it needs to do is create 1 each of
 * `Shm_session` and `Arena` objects and register the latter with the former to enable lending objects
 *
 * So all we have to do is provide a modified `async_connect()` which does the above.  How?  Answer:
 *   - execute vanilla session::Client_session_impl::async_connect(); once that triggers the on-done handler:
 *   - set up `Shm_session` and `Arena` (though for the former there's a prerequisite async step to establish an
 *     interal-use IPC channel);
 *   - invoke the user's original on-done handler.
 *
 * In `*this`, mechanically: the true implementation of the needed setup and accessors (explained above) is
 * split between shm::classic::Session_mv (common with `Server_session_impl`) and
 * shm::arena_lend::jemalloc::Client_session_impl, with the vanilla core in super-class
 * session::Client_session_impl.
 *
 * That's the key; then session::Client_session_mv adds movability around session::Server_session_impl; and lastly
 * this type aliases to *that* (by way of `Session_mv`).  Session_mv completes the puzzle by pImpl-forwarding to the
 * added (SHM-focused) API.  (session::Client_session_mv only pImpl-forwards to the vanilla API.)
 *
 * The last and most boring piece of the puzzle are the pImpl-lite wrappers around the SHM-specific API
 * that shm::arena_lend::jemalloc::Client_session adds to super-class Client_session_mv: `session_shm()` and so on.
 * Just see same spot in shm::arena_lend::jemalloc::Server_session doc header; it explains both
 * client and server sides.
 * @endinternal
 *
 * @tparam S_MQ_TYPE_OR_NONE
 *         Identical to session::Client_session.
 * @tparam S_TRANSMIT_NATIVE_HANDLES
 *         Identical to session::Client_session.
 * @tparam Mdt_payload
 *         Identical to session::Client_session.
 */
template<session::schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES,
         typename Mdt_payload = ::capnp::Void>
using Client_session
  = Session_mv
      <session::Client_session_mv
        <Client_session_impl<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>>>;

// Free functions.

/**
 * Prints string representation of the given `Session_mv` to the given `ostream`.
 *
 * @relatesalso Session_mv
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Session_t>
std::ostream& operator<<(std::ostream& os, const Session_mv<Session_t>& val);

/**
 * Prints string representation of the given `Server_session` to the given `ostream`.
 *
 * @relatesalso Server_session
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<session::schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
std::ostream& operator<<(std::ostream& os,
                         const Server_session
                                 <S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>& val);

/**
 * Prints string representation of the given `Session_server` to the given `ostream`.
 *
 * @relatesalso Session_server
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<session::schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
std::ostream& operator<<(std::ostream& os,
                         const Session_server
                                 <S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>& val);

} // namespace ipc::session::shm::arena_lend::jemalloc
