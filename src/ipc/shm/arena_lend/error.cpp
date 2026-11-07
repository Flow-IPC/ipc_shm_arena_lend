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
#include "ipc/shm/arena_lend/error.hpp"
#include "ipc/util/util_fwd.hpp"
#include <flow/util/util.hpp>

namespace ipc::shm::arena_lend::error
{

// Types.

/**
 * The boost.system category for errors returned by the ipc::shm::arena_lend module.  Analogous to
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
   * Analogous to transport::error::Category::message().
   *
   * @param val
   *        See above.
   * @return See above.
   */
  std::string message(int val) const override;

  /**
   * Analogous to transport::error::Category::code_symbol().
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
  return {static_cast<int>(err_code), Category::S_CATEGORY};
}

Category::Category() = default;

const char* Category::name() const noexcept // Virtual.
{
  return "ipc/shm/arena_lend";
}

std::string Category::message(int val) const // Virtual.
{
  using std::string;

  // KEEP THESE STRINGS IN SYNC WITH COMMENT IN error.hpp ON THE INDIVIDUAL ENUM MEMBERS!

  // See notes in transport::error in same spot.
  switch (static_cast<Code>(val))
  {
  case Code::S_SHM_BIPC_MISC_LIBRARY_ERROR:
    return "Low-level boost.ipc.shm: boost.interprocess emitted miscellaneous library exception sans a system code; "
           "a WARNING message at throw-time should contain all possible details.";

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
  case Code::S_SHM_BIPC_MISC_LIBRARY_ERROR:
    return "SHM_BIPC_MISC_LIBRARY_ERROR";

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

} // namespace ipc::shm::arena_lend::error
