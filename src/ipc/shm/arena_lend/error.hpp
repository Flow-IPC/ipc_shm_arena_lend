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

/**
 * Namespace containing the ipc::shm::arena_lend module's (including SHM-jemalloc's) extension of boost.system
 * error conventions, so that that API can return codes/messages from within its own new set of error codes/messages.
 * Analogous to ipc::shm::classic::error; and essentially all the notes in ipc::transport::error doc header and
 * otherwise within that namespace apply equally here.  Therefore please:
 *
 * @see ipc::transport::error documentation; notes therein (such as to-dos) likely apply here equally.
 */
namespace ipc::shm::arena_lend::error
{

// Types.

/// Numeric value of the lowest Code.
constexpr int S_CODE_LOWEST_INT_VALUE = 1;

/**
 * All possible errors returned (via `Error_code` arguments) by ipc::shm::arena_lend functions/methods *outside of*
 * possibly system-triggered errors.
 *
 * All notes from transport::error::Code doc header apply here.
 */
enum class Code
{
  /**
   * Low-level boost.ipc.shm: boost.interprocess emitted miscellaneous library exception sans a system code;
   * a WARNING message at throw-time should contain all possible details.
   */
  S_SHM_BIPC_MISC_LIBRARY_ERROR = S_CODE_LOWEST_INT_VALUE,

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
// @todo `@relatesalso Code` makes Doxygen complain; maybe it doesn't work with `enum class`es like Code.

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
// @todo `@relatesalso Code` makes Doxygen complain; maybe it doesn't work with `enum class`es like Code.

} // namespace ipc::shm::arena_lend::error

namespace boost::system
{

// Types.

/**
 * Ummm -- it specializes this `struct` to -- look -- the end result is boost.system uses this as
 * authorization to make `enum` `Code` convertible to `Error_code`.  The non-specialized
 * version of this sets `value` to `false`, so that random arbitrary `enum`s can't just be used as
 * `Error_code`s.  Note that this is the official way to accomplish that, as (confusingly but
 * formally) documented in boost.system docs.
 */
template<>
struct is_error_code_enum<::ipc::shm::arena_lend::error::Code>
{
  /// Means `Code` `enum` values can be used for `Error_code`.
  static const bool value = true;
  // See note in similar place near transport::error.
};

} // namespace boost::system
