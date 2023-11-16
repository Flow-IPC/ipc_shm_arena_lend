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

#include "ipc/session/session_fwd.hpp"

namespace ipc::session::shm::arena_lend::jemalloc
{

// Types.

// Find doc headers near the bodies of these compound types.

template<typename Session_impl_t>
class Session_impl;

template<session::schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
class Server_session_impl;

template<session::schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
class Client_session_impl;

// Constants.

/**
 * Internally used in pool names generated in this namespace to differentiate from those of other SHM-supporting
 * session providers' pools (for example session::shm::classic versus session::shm::arena_lend::jemalloc).
 */
extern const Shared_name SHM_SUBTYPE_PREFIX;

// Free functions.

/**
 * Prints string representation of the given `Session_impl` to the given `ostream`.
 *
 * @relatesalso Session_impl
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Session_impl_t>
std::ostream& operator<<(std::ostream& os, const Session_impl<Session_impl_t>& val);

/**
 * Prints string representation of the given `Server_session_impl` to the given `ostream`.
 *
 * @relatesalso Server_session_impl
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<session::schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
std::ostream& operator<<(std::ostream& os,
                         const Server_session_impl<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>& val);

/**
 * Prints string representation of the given `Client_session_impl` to the given `ostream`.
 *
 * @relatesalso Client_session_impl
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<session::schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
std::ostream& operator<<(std::ostream& os,
                         const Client_session_impl<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>& val);

} // namespace ipc::session::shm::arena_lend::jemalloc
