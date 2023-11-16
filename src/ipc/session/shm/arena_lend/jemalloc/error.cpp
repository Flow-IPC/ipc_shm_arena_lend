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
#include "ipc/session/shm/arena_lend/jemalloc/error.hpp"
#include "ipc/util/util_fwd.hpp"

namespace ipc::session::shm::arena_lend::jemalloc::error
{

// Types.

/**
 * The boost.system category for errors returned by the ipc::session module.  Analogous to
 * transport::error::Category.  All notes therein apply.
 */
class Category :
  public boost::system::error_category
{
public:
  // Constants.

  /// The one Category.
  static const Category S_CATEGORY;

  // Methods.

  /**
   * Analogous to transport::error::Category::name().
   *
   * @return See above.
   */
  const char* name() const noexcept override;

  /**
   * Analogous to transport::error::Category::name().
   *
   * @param val
   *        See above.
   * @return See above.
   */
  std::string message(int val) const override;

  /**
   * Analogous to transport::error::Category::name().
   * @param code
   *        See above.
   * @return See above.
   */
  static util::String_view code_symbol(Code code);

private:
  // Constructors.

  /// Boring constructor.
  explicit Category();
}; // class Category

// Static initializations.

const Category Category::S_CATEGORY;

// Implementations.

Error_code make_error_code(Code err_code)
{
  /* Assign Category as the category for flow::error::Code-cast error_codes;
   * this basically glues together Category::name()/message() with the Code enum. */
  return Error_code(static_cast<int>(err_code), Category::S_CATEGORY);
}

Category::Category() = default;

const char* Category::name() const noexcept // Virtual.
{
  return "ipc/session/shm/arena_lend/jemalloc";
}

std::string Category::message(int val) const // Virtual.
{
  using std::string;

  // KEEP THESE STRINGS IN SYNC WITH COMMENT IN error.hpp ON THE INDIVIDUAL ENUM MEMBERS!

  // See notes in transport::error in same spot.
  switch (static_cast<Code>(val))
  {
  case Code::S_SHM_ARENA_CREATION_FAILED:
    return "Session opening: While setting up the session's locally-managed SHM-arena(s) of an arena-lending "
           "SHM-provider, an error occurred thus hosing the session before it could be opened.  Logs may "
           "indicate the reason.";
  case Code::S_SHM_ARENA_LEND_FAILED:
    return "Session opening: While registering the session's locally-managed SHM-arena(s) of an arena-lending "
           "SHM-provider with the lend/borrow engine, an error occurred thus hosing the session before it could "
           "be opened.  Logs may indicate the reason.  Most likely it was an internal-IPC failure when sending "
           "arena info.";
  case Code::S_SERVER_MASTER_SHM_UNEXPECTED_TRANSPORT_ERROR:
    return "Session opening: While trying to transmit resource for internal-IPC use in an arena-lending SHM-provider, "
           "server encountered incoming-direction channel error whose exact nature could not be determined at that "
           "exact moment.  Logs will indicate that exact nature; meanwhile session opening failed.";

  case Code::S_END_SENTINEL:
    assert(false && "SENTINEL: Not an error.  "
                    "This Code must never be issued by an error/success-emitting API; I/O use only.");
  }
  assert(false);
  return "";
} // Category::message()

util::String_view Category::code_symbol(Code code) // Static.
{
  // Note: Must satisfy istream_to_enum() requirements.

  switch (code)
  {
  case Code::S_SHM_ARENA_CREATION_FAILED:
    return "SHM_ARENA_CREATION_FAILED";
  case Code::S_SHM_ARENA_LEND_FAILED:
    return "SHM_ARENA_LEND_FAILED";
  case Code::S_SERVER_MASTER_SHM_UNEXPECTED_TRANSPORT_ERROR:
    return "SERVER_MASTER_SHM_UNEXPECTED_TRANSPORT_ERROR";

  case Code::S_END_SENTINEL:
    return "END_SENTINEL";
  }
  assert(false);
  return "";
}

std::ostream& operator<<(std::ostream& os, Code val)
{
  // Note: Must satisfy istream_to_enum() requirements.
  return os << Category::code_symbol(val);
}

std::istream& operator>>(std::istream& is, Code& val)
{
  /* Range [<1st Code>, END_SENTINEL); no match => END_SENTINEL;
   * allow for number instead of ostream<< string; case-insensitive. */
  val = flow::util::istream_to_enum(&is, Code::S_END_SENTINEL, Code::S_END_SENTINEL, true, false,
                                    Code(S_CODE_LOWEST_INT_VALUE));
  return is;
}

} // namespace ipc::session::shm::arena_lend::jemalloc::error
