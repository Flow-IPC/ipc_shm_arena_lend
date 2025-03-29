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

#include "ipc/shm/arena_lend/jemalloc/jemalloc.hpp"
#include "ipc/shm/stl/stateless_allocator.hpp"
#include "ipc/transport/struc/shm/shm_fwd.hpp"
#include "ipc/transport/transport_fwd.hpp"

namespace ipc::session::shm::arena_lend::jemalloc
{

// Types.

/**
 * Implements the SHM-related API common to shm::arena_lend::jemalloc::Server_session and
 * shm::arena_lend::jemalloc::Client_session.   It is, as of this writing, not to be instantiated by itself.
 * Rather see shm::arena_lend::jemalloc::Server_session (and
 * shm::arena_lend::jemalloc::Client_session) regarding what to actually instantiate and
 * further context.  As for the API in Session_mv itself, it is quite critical.
 * shm::arena_lend::jemalloc::Server_session doc header explains the background.  Member doc headers formally document
 * those members given that background.  #Arena doc header is particularly useful.
 *
 * @tparam Session_t
 *         Class for us to `public`ly sub-class: `Server_session_mv<...>` or `Client_session_mv<...>`.
 *         Note its `public` API is inherited.  See the relevant class template's doc header.
 */
template<typename Session_t>
class Session_mv :
  public Session_t
{
public:
  // Types.

  /// Short-hand for our base class.  To the user: note its `public` API is inherited.
  using Base = Session_t;

  /**
   * The arena object on which one may call `construct<T>(ctor_args...)`, where `ctor_args` are arguments
   * to the `T::T()` constructor.  While a full-length discussion of the allowed properties of `T` is outside
   * out scope here (belongs in ipc::shm docs), here's a short version for convenience:
   * PODs work; STL nested container+POD combos work, as long as
   * a shm::stl allocator is used at all levels; manually-implemented non-STL-compliant data
   * structures work if care is taken to use `Arena::allocate()` and `Arena::Pointer`.
   *
   * Suppose `A->construct()` yields handle `h`, where `A` is any `Arena*` returned by a `*this` accessor --
   * `this->app_shm()`, `this->session_shm()` (the former available only is `*this` is really an instance of sub-class
   * Server_session_mv).  Then `h` can be passed to `this->lend_object()` which yields `Blob b`.
   * Further, an `h` returned by `s->borrow_object(b)`, executed on the opposing
   * `shm::arena_lend::jemalloc::Session_mv s` (after receiving `b` via IPC) shall point to the same location in
   * `A` and will become part of a cross-process `shared_ptr` group with the original `h` (and any copies/moves
   * thereof), `*h` garbage-collected based on a (conceptual) cross-process ref-count.
   *
   * Full #Arena capabilities are described in the documentation for the alias's target,
   * shm::arena_lend::jemalloc::Ipc_arena.
   *
   * `this->lend_object()` and `this->borrow_object()` really forward to the same methods of
   * #Shm_session.  That #Shm_session can itself be accessed via shm_session().  That said it is likely best
   * to use the forwarding Session_mv methods, as it is conducive to generic programming in which an
   * alternate-SHM-provider `Session_mv` could be used instead.  In particular SHM-classic-based
   * session::shm::classic::Session_mv::lend_object() and session::shm::classic::Session_mv::borrow_object() APIs
   * behave identically; whereas `session::shm::classic::Session_mv::shm_session()` does not exist (because the
   * mechanics of sharing SHM-stored objects in SHM-classic fundamentally differ from those of SHM-jemalloc).  Hence
   * code that uses `Session_mv::lend_object()` and `Session_mv::borrow_object()` would work with either
   * `Session_mv`.
   *
   * @note Unlike with SHM-classic, Session_mv::borrow_object() can accept a `Blob` generated by opposing process's
   *       `Session_mv::Shm_session::lend_object()`; and conversely `Session_mv::Shm_session::borrow_object()`
   *       can accept a `Blob` from Session_mv::lend_object().  While, again, it is probably best to just use
   *       Session_mv methods on both sides, if the need does arise, one can take advantage of this compatibility.
   */
  using Arena = typename Base::Base::Impl::Arena;

  /**
   * The lend/borrow engine type which allows one to "lend" a `T` (obtained via `Arena::construct<T>()`)
   * to the opposing process, ahead of transmitting the object over a channel obtained from `*this` session.
   * The main methods of interest -- `lend_object()` and `borrow_object()` -- are explained in context in
   * #Arena doc header and are more naturally accessed via Session_mv::lend_object() and Session_mv::borrow_object().
   *
   * That said, #Shm_session can be used for further advanced purposes.  For example one could separately create
   * an #Arena (beyond those supplied via session_shm() and Server_session_mv::app_shm()) and register it
   * via `Shm_session::lend_arena()`; then it would be possible to `Shm_session::lend_object()` and transmit
   * objects `construct()`ed in that additional #Arena.  Why do such a thing?  Answer: There are various conceivable
   * use cases.  Perhaps a shorter-lived segregated `Arena` is desirable for security or safety reasons for example.
   *
   * The point is: ipc::session, per its mission, pre-creates the SHM infrastructure for you thus eliminating the
   * various difficulties inherent in this task -- while making `Session`s backed by this SHM-provider largely
   * interchangeable with ones backed by another, such as SHM-classic.  In doing so it hides the SHM-provider-specific
   * work required.  Yet it *also* provides to you the ability to use the SHM-provider-specific API directly as
   * desired.  Should you avoid this?  Informally: Yes, as it'll make your life easier... when it is sufficient.
   * If it is *not* sufficient, you have the tools to go beyond it.
   */
  using Shm_session = jemalloc::Shm_session;

  /**
   * Convenience alias to be used in STL-compliant `T`s with `"Arena::construct<T>()"`, our own lend_object<T>(), and
   * `"Shm_session::lend_object<T>()"`.
   *
   * @see #Borrower_allocator which must be the allocator type used on the borrower side.
   *
   * @tparam T
   *         See standard C++ `Allocator` concept.
   */
  template<typename T>
  using Allocator = ipc::shm::stl::Stateless_allocator<T, Arena>;

  /**
   * Convenience alias to be used in STL-compliant `T`s with our own borrow_object<T>() and
   * `"Shm_session::borrow_object<T>()"`.
   *
   * @tparam T
   *         See standard C++ `Allocator` concept.
   */
  template<typename T>
  using Borrower_allocator
    = ipc::shm::stl::Stateless_allocator<T, ipc::shm::Arena_to_borrower_allocator_arena_t<Arena>>;

  /**
   * Implements Session API per contract.
   * @see Session::Structured_channel: implemented concept.
   */
  template<typename Message_body>
  using Structured_channel
    = typename transport::struc::shm::arena_lend::jemalloc::Channel
                 <typename Base::Base::Impl::Session_base_obj::Channel_obj,
                  Message_body>;

  /**
   * Implements Session API per contract.
   * @see Session::Structured_msg_builder_config: implemented concept.
   */
  using Structured_msg_builder_config = typename Base::Base::Impl::Structured_msg_builder_config;

  /**
   * Implements Session API per contract.
   * @see Session::Structured_msg_reader_config: implemented concept.
   */
  using Structured_msg_reader_config = typename Base::Base::Impl::Structured_msg_reader_config;

  /// Alias for a light-weight blob used in borrow_object() and lend_object().
  using Blob = typename Base::Base::Impl::Blob;

  /**
   * Server_session::Vat_network and `Client_session::Vat_network` are reasonable concrete types
   * of template transport::struc::shm::rpc::Session_vat_network for an ipc::session user to use on opposing
   * sides of a session; use the mainstream-form ctor to straightforwardly construct your zero-copy-enabled
   * `Vat_network` (from a `*this`) for blazing-fast capnp-RPC.
   */
  using Vat_network = transport::struc::shm::rpc::Session_vat_network<Session_mv, Arena>;

  /// You may disregard.
  using Async_io_obj = transport::Null_peer;
  /**
   * Useful for generic programming, the `sync_io`-pattern counterpart to `*this` type.
   *
   * @internal
   * @todo The impl for alias jemalloc::Client_session::Sync_io_obj is hacky and should be reconsidered, even
   * though in practice it works more or less.  Just `Client_session` is an alias to `Session_mv` parameterized
   * a certain way, so the alias is defined inside `Session_mv` and is written in terms of `Client_session_adapter`
   * due to knowing this fact.  Maybe classic::Client_session should be a thin wrapper instead of an alias,
   * but that's a ton of lines for such a small thing... or maybe some `rebind` craziness would work....
   */
  using Sync_io_obj = sync_io::Client_session_adapter<Session_mv>;

  // Constants.

  /**
   * Implements Session API per contract.
   * @internal
   * This is explicitly here (it's inherited anyway) mostly to document it as being available at this level;
   * in particular a certain parameterization of this class template is aliased to the commonly used #Client_session.
   */
  static constexpr bool S_IS_SRV_ELSE_CLI = Base::S_IS_SRV_ELSE_CLI;

  // Constructors/destructor.

  /// Inherit default, move ctors.
  using Base::Base;

  // Methods.

  /**
   * Returns SHM #Arena with per-session scope, meaning it shall be accessible only during the lifetime of
   * this session, by this Session (via this accessor).  In the opposing Session (via its `session_shm()` accessor)
   * a similar #Arena is also available and has the same conceptual lifetime (until Session end).
   * See #Arena doc header for useful instructions on working with #Arena, lend_object(), and borrow_object().
   *
   * @return See above.
   */
  Arena* session_shm();

  /**
   * Identical to session_shm() but returns that via a `shared_ptr`-handle as required for work directly within
   * ipc::shm::arena_lend::jemalloc APIs.  Note this is a quirk of that particular API and, in particular, has
   * no equivalent in ipc::session::shm::classic `Session` and `Session_server` counterparts.
   * It should not be necessary to use except when going beyond ipc::session for your SHM-arena needs.
   *
   * @return See above.
   */
  std::shared_ptr<Arena> session_shm_ptr();

  /**
   * Adds an owner process to the owner set of the given `Arena::construct()`-ed handle; and returns an opaque blob,
   * such that if one passes it to opposing Session_mv::borrow_object() in the receiving process, that
   * `borrow_object()` shall return an equivalent `Handle` in that process.  The returned `Blob`
   * is guaranteed to have non-zero size that is small enough to be considered very cheap to copy and hence
   * transmit over IPC.
   *
   * It is the user's responsibility to transmit the returned blob, such as via a transport::Channel,
   * to the owning process.  Failing to do so may leak the object until arena cleanup.  (Arena cleanup time
   * depends on the source #Arena.  If it came from session_shm(), arena cleanup occurs at Session destruction.
   * If from Server_session_mv::app_shm(), arena cleanup occurs at Server_session destruction.  If it is a
   * custom-created other #Arena -- not managed by ipc::session -- then it would occur whenever you choose to destroy
   * that #Arena.  If a destructor does not run, due to crash/etc., then the leaked ipc::session-managed `Arena`s'
   * pools are cleaned zealously by ipc::session via a heuristic algorithm.)
   *
   * ### What it really does ###
   * It forwards to `shm_session()->lend_object()`.  I mention this for context; it is arguably desirable to not
   * count on these details in code that can generically work with a different SHM-enabled Session, such as
   * shm::classic::Session_mv, taken as a template param.  E.g., shm::arena_lend::jemalloc::Session_mv::lend_object()
   * does not, and cannot (as it does not exist), call any `Session_mv::shm_session()` on which to invoke
   * `lend_object()`.
   *
   * However, if your code specifically counts on `*this` being a shm::arena_lend::jemalloc::Server_session, then
   * it is not wrong to rely on this knowledge.
   *
   * ### Possibility of error ###
   * This method returns an `.empty()` blob in the event of error.  Assuming you used it with proper inputs, this
   * indicates the session is hosed -- most likely the opposing process is down -- and therefore your code should
   * be ready for this absolutely possible eventuality.  The session-hosed condition *will* shortly be indicated via
   * session on-error handler as well.  Informally we suggest handling the first sign of the session going down (whether
   * this returning empty, borrow_object() returning null, or the session on-error handler firing) uniformly by
   * abandoning the entire session ASAP.
   *
   * @tparam T
   *         See #Arena.
   * @param handle
   *        Value returned by #Arena method `construct<T>()`.  Note this is a mere `shared_ptr<T>` albeit with
   *        unspecified custom deleter logic attached.  The #Arena instance can be session_shm(),
   *        Server_session_mv::app_shm(), or a non ipc::session-managed custom #Arena, as long as it has been
   *        registered via `shm_session()->lend_arena()`.
   * @return See above.  On success, non-empty; otherwise an `.empty()` blob.
   */
  template<typename T>
  Blob lend_object(const typename Arena::template Handle<T>& handle);

  /**
   * Completes the cross-process operation begun by oppsing Session_mv::lend_object() that returned `serialization`;
   * to be invoked in the intended new owner process which is operating `*this`.
   *
   * ### Possibility of error ###
   * Indicated by this returning null, the remarks in the lend_object() doc header "Possibility of error" section
   * apply here equally.  Reminder: be ready for this to return null; it can absolutely happen.
   *
   * @tparam T
   *         See lend_object().
   * @param serialization
   *        Value, not `.empty()`, returned by opposing lend_object() and transmitted bit-for-bit to this process.
   * @return See above.  On success, non-null pointer; otherwise a null pointer.
   */
  template<typename T>
  typename Arena::template Handle<T> borrow_object(const Blob& serialization);

  /**
   * Returns builder config suitable for capnp-serializing out-messages in SHM arena session_shm().
   *
   * @return See above.
   */
  Structured_msg_builder_config session_shm_builder_config();

  /**
   * When transmitting items originating in #Arena session_shm() via
   * transport::struc::shm::Builder::emit_serialization() (and/or transport::struc::Channel send facilities),
   * returns additional-to-payload information necessary to target the opposing process properly.
   *
   * Internally: Since `*this` type is based on arena-lending SHM-provider type, this is simply shm_session().
   * Generic code of yours should not need to rely on that impl detail, but we mention it for education/context.
   *
   * @return See above.
   */
  typename Structured_msg_builder_config::Builder::Session session_shm_lender_session();

  /**
   * Returns shm_reader_config().
   *
   * ### Rationale ###
   * Why does this exist, given that shm_reader_config() exists?  Answer: It is for generic programming,
   * for code that would like to work independently of which SHM-enabled Session is in use; for example
   * interchangeably between `*this` class or shm::classic::Session_mv.  Hence code that wants to set up
   * a transport::struc::Msg_in (or transport::struc::Channel that receives them), and requires
   * a reader-config object capable of decoding messages stored in the opposing process's `session_shm()`
   * (session-scope #Arena), then this method can be used -- and will work, because shm_reader_config()
   * will work for *any* `Arena` -- which trivially includes opposing `session_shm()`.
   *
   * For example, transport::struc::Channel_base::Serialize_via_session_shm tag ctor of
   * transport::struc::Channel will internally invoke this when loading up its internal
   * per-session reader-config for you.
   *
   * @return See above.
   */
  Structured_msg_reader_config session_shm_reader_config();

  /**
   * Returns shm_reader_config().
   *
   * ### Rationale ###
   * Why does this exist, given that shm_reader_config() exists?  Answer: It is for generic programming,
   * for code that would like to work independently of which SHM-enabled Session is in use; for example
   * interchangeably between `*this` class or shm::classic::Session_mv.  Hence code that wants to set up
   * a transport::struc::Msg_in (or transport::struc::Channel that receives them), and requires
   * a reader-config object capable of decoding messages stored in the opposing process's `app_shm()`
   * (session-scope #Arena), then this method can be used -- and will work, because shm_reader_config()
   * will work for *any* `Arena` -- which trivially includes opposing `app_shm()`.
   *
   * For example, transport::struc::Channel_base::Serialize_via_session_shm tag ctor of
   * transport::struc::Channel will internally invoke this when loading up its internal
   * per-app reader-config for you.
   *
   * What if `*this` is really super-classing Server_session_mv?  The opposing object is of type
   * Client_session_mv which lacks `app_shm()`; so isn't the present method meaningless?  Not exactly:
   * if someday a message *were* to arrive from a somehow-app-scope-#Arena-capable client, then this
   * would decode it just fine.  As of this writing that's impossible, so it's a moot question;
   * yet it allows for code such as the aforementioned transport::struc::Channel tag ctor to be written
   * simply in a way that'll work for shm::classic (which does have a shm::classic::Session_mv::app_shm()) and
   * `*this` class as well.
   *
   * @return See above.
   */
  Structured_msg_reader_config app_shm_reader_config();

  /**
   * Returns lend/borrow engine #Shm_session for `*this` session.
   *
   * @see #Shm_session doc header.
   *
   * @return See above.
   */
  Shm_session* shm_session();

  /**
   * Returns reader config that can be used to deserialize any transport::struc::Msg_in to have arrived
   * from the opposing process in `*this` session.
   *
   * Internally: This essentially contains simply shm_session().  Generic code of yours should not need to rely
   * on that impl detail, but we mention it for education/context.
   *
   * @return See above.
   */
  Structured_msg_reader_config shm_reader_config();
}; // class Session_mv

// Free functions: in *_fwd.hpp.

// Template implementations.

/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define TEMPLATE_JEM_SESSION_MV \
  template<typename Session_t>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define CLASS_JEM_SESSION_MV \
  Session_mv<Session_t>

// The rest just continues the pImpl-lite paradigm, same as our public super-class.

TEMPLATE_JEM_SESSION_MV
typename CLASS_JEM_SESSION_MV::Arena* CLASS_JEM_SESSION_MV::session_shm()
{
  assert(Base::Base::impl() && "Do not call this on as-if-default-cted Session.");
  return Base::Base::impl()->session_shm();
}

TEMPLATE_JEM_SESSION_MV
std::shared_ptr<typename CLASS_JEM_SESSION_MV::Arena> CLASS_JEM_SESSION_MV::session_shm_ptr()
{
  assert(Base::Base::impl() && "Do not call this on as-if-default-cted Session.");
  return Base::Base::impl()->session_shm_ptr();
}

TEMPLATE_JEM_SESSION_MV
template<typename T>
typename CLASS_JEM_SESSION_MV::Blob
  CLASS_JEM_SESSION_MV::lend_object(const typename Arena::template Handle<T>& handle)
{
  assert(Base::Base::impl() && "Do not call this on as-if-default-cted Session.");
  return Base::Base::impl()->template lend_object<T>(handle);
}

TEMPLATE_JEM_SESSION_MV
template<typename T>
typename CLASS_JEM_SESSION_MV::Arena::template Handle<T>
  CLASS_JEM_SESSION_MV::borrow_object(const Blob& serialization)
{
  assert(Base::Base::impl() && "Do not call this on as-if-default-cted Session.");
  return Base::Base::impl()->template borrow_object<T>(serialization);
}

TEMPLATE_JEM_SESSION_MV
typename CLASS_JEM_SESSION_MV::Structured_msg_builder_config
  CLASS_JEM_SESSION_MV::session_shm_builder_config()
{
  assert(Base::Base::impl() && "Do not call this on as-if-default-cted Session.");
  return Base::Base::impl()->session_shm_builder_config();
}

TEMPLATE_JEM_SESSION_MV
typename CLASS_JEM_SESSION_MV::Structured_msg_builder_config::Builder::Session
  CLASS_JEM_SESSION_MV::session_shm_lender_session()
{
  assert(Base::Base::impl() && "Do not call this on as-if-default-cted Session.");
  return Base::Base::impl()->session_shm_lender_session();
}

TEMPLATE_JEM_SESSION_MV
typename CLASS_JEM_SESSION_MV::Structured_msg_reader_config
  CLASS_JEM_SESSION_MV::session_shm_reader_config()
{
  assert(Base::Base::impl() && "Do not call this on as-if-default-cted Session.");
  return Base::Base::impl()->session_shm_reader_config();
}

TEMPLATE_JEM_SESSION_MV
typename CLASS_JEM_SESSION_MV::Structured_msg_reader_config
  CLASS_JEM_SESSION_MV::app_shm_reader_config()
{
  assert(Base::Base::impl() && "Do not call this on as-if-default-cted Session.");
  return Base::Base::impl()->app_shm_reader_config();
}

TEMPLATE_JEM_SESSION_MV
typename CLASS_JEM_SESSION_MV::Shm_session*
  CLASS_JEM_SESSION_MV::shm_session()
{
  assert(Base::Base::impl() && "Do not call this on as-if-default-cted Session.");
  return Base::Base::impl()->shm_session();
}

TEMPLATE_JEM_SESSION_MV
typename CLASS_JEM_SESSION_MV::Structured_msg_reader_config
  CLASS_JEM_SESSION_MV::shm_reader_config()
{
  assert(Base::Base::impl() && "Do not call this on as-if-default-cted Session.");
  return Base::Base::impl()->shm_reader_config();
}

TEMPLATE_JEM_SESSION_MV
std::ostream& operator<<(std::ostream& os, const CLASS_JEM_SESSION_MV& val)
{
  return os << static_cast<const typename CLASS_JEM_SESSION_MV::Base::Base&>(val);
}

#undef CLASS_JEM_SESSION_MV
#undef TEMPLATE_JEM_SESSION_MV

} // namespace ipc::session::shm::arena_lend::jemalloc
