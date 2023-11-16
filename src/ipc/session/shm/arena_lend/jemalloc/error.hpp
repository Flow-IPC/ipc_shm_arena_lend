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

#include "ipc/common.hpp"

/// Analogous to ipc::session::error but for errors pertaining to SHM-jemalloc-enabled sessions specifically.
namespace ipc::session::shm::arena_lend::jemalloc::error
{

// Types.

/// Numeric value of the lowest Code.
constexpr int S_CODE_LOWEST_INT_VALUE = 1;

/**
 * All possible errors returned (via `Error_code` arguments) by ipc::session functions/methods *outside of*
 * ipc::transport-triggered errors involved in transport involved in doing session-related work; and possibly
 * system-triggered errors.
 *
 * All notes from transport::error::Code doc header apply here.
 */
enum class Code
{
  /**
   * Session opening: While setting up the session's locally-managed SHM-arena(s) of an arena-lending SHM-provider,
   * an error occurred thus hosing the session before it could be opened.  Logs may indicate the reason.
   *
   * @internal
   * @todo If ipc::shm::arena_lend arena-creation API(s) are modified to output an `Error_code` instead of
   * reporting just success-versus-failure, then ipc::session::error::Code::S_SHM_ARENA_CREATION_FAILED should go
   * away.
   */
  S_SHM_ARENA_CREATION_FAILED,

  /**
   * Session opening: While registering the session's locally-managed SHM-arena(s) of an arena-lending SHM-provider
   * with the lend/borrow engine, an error occurred thus hosing the session before it could be opened.  Logs may
   * indicate the reason.  Most likely it was an internal-IPC failure when sending arena info.
   *
   * @internal
   * @todo If ipc::shm::arena_lend arena-lending API(s) are modified to output an `Error_code` instead of
   * reporting just success-versus-failure and reporting problem through an async callback, then
   * ipc::session::error::Code::S_SHM_ARENA_CREATION_FAILED should go away.  See
   * shm::arena_lend::jemalloc::init_shm() body for more discussion.
   */
  S_SHM_ARENA_LEND_FAILED,

  /**
   * Session opening: While trying to transmit resource for internal-IPC use in an arena-lending SHM-provider,
   * server encountered incoming-direction channel error whose exact nature could not be determined at that
   * exact moment.  Logs will indicate that exact nature; meanwhile session opening failed.
   */
  S_SERVER_MASTER_SHM_UNEXPECTED_TRANSPORT_ERROR,

  /// SENTINEL: Not an error.  This Code must never be issued by an error/success-emitting API; I/O use only.
  S_END_SENTINEL
}; // enum class Code

// Free functions.

/**
 * Analogous to transport::error::make_error_code().
 *
 * @param err_code
 *        See above.
 * @return See above.
 */
Error_code make_error_code(Code err_code);

/**
 * Analogous to transport::error::operator>>().
 *
 * @param is
 *        See above.
 * @param val
 *        See above.
 * @return See above.
 */
std::istream& operator>>(std::istream& is, Code& val);
// @todo - `@relatesalso Code` makes Doxygen complain; maybe it doesn't work with `enum class`es like Code.

/**
 * Analogous to transport::error::operator<<().
 *
 * @param os
 *        See above.
 * @param val
 *        See above.
 * @return See above.
 */
std::ostream& operator<<(std::ostream& os, Code val);
// @todo - `@relatesalso Code` makes Doxygen complain; maybe it doesn't work with `enum class`es like Code.

} // namespace ipc::session::shm::arena_lend::jemalloc::error

namespace boost::system
{

// Types.

/**
 * Ummm -- it specializes this `struct` to -- look -- the end result is boost.system uses this as
 * authorization to make `enum` `Code` convertible to `Error_code`.  The non-specialized
 * version of this sets `value` to `false`, so that random arbitary `enum`s can't just be used as
 * `Error_code`s.  Note that this is the offical way to accomplish that, as (confusingly but
 * formally) documented in boost.system docs.
 */
template<>
struct is_error_code_enum<::ipc::session::shm::arena_lend::jemalloc::error::Code>
{
  /// Means `Code` `enum` values can be used for `Error_code`.
  static const bool value = true;
  // See note in similar place near transport::error.
};

} // namespace boost::system
