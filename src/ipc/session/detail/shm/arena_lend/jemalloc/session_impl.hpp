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

#include "ipc/shm/arena_lend/jemalloc/ipc_arena.hpp"
#include "ipc/shm/arena_lend/jemalloc/jemalloc.hpp"
#include "ipc/session/standalone/shm/arena_lend/borrower_shm_pool_collection_repository.hpp"
#include "ipc/session/standalone/shm/arena_lend/jemalloc/shm_session.hpp"
#include "ipc/session/shm/arena_lend/jemalloc/error.hpp"
#include "ipc/session/shm/arena_lend/jemalloc/jemalloc.hpp"
#include "ipc/session/detail/shm/arena_lend/jemalloc/jemalloc_fwd.hpp"
#include "ipc/session/detail/session_shared_name.hpp"
#include "ipc/transport/struc/channel_base.hpp"
#include "ipc/transport/struc/shm/arena_lend/jemalloc/jemalloc_fwd.hpp"
#include "ipc/transport/struc/shm/arena_lend/jemalloc/jemalloc.hpp"

namespace ipc::session::shm::arena_lend::jemalloc
{

// Types.

/**
 * Common data and logic for shm::arena_lend::jemalloc::Server_session_impl and
 * shm::arena_lend::jemalloc::Client_session_impl.  Arena-lending SHM-providers are almost exactly the same,
 * once everything is open; the only difference is session-server provides an extra `Arena` shared among
 * all sessions with the same opposing Client_app (created the first time such a session-client establishes
 * a session); jemalloc::Session_server tracks these `Arena`s and coordinates with jemalloc::Server_session.
 * So all the in-common stuff is here in this internally-used super-class of the 2 `*_impl`.  The setup
 * procedure is asymmetrical, and the two sides of that asymmetry are in those 2 `*_impl`s -- including
 * the differing APIs (e.g., Client_session_impl::async_connect()).
 *
 * @tparam Session_impl_t
 *         Our base that our sub-class wants to build on-top-of.  E.g., `Server_session_impl<...>`.
 */
template<typename Session_impl_t>
class Session_impl : public Session_impl_t
{
public:
  // Types.

  /// Short-hand for base class.
  using Base = Session_impl_t;

  /// Short-hand for Session_base super-class.
  using Session_base_obj = typename Base::Session_base_obj;

  /**
   * See shm::arena_lend::jemalloc::Session_mv counterpart for public description.
   *
   * Internally:
   * An arena-lending SHM-provider's arena is simply this.  The key is each side (including ours) maintains
   * 1+ (2 on server, 1 on client) such arenas and then all allocations in this process are from there, never
   * from an opposing process.  This is in contrast to, in particular, non-arena-lending
   * session::shm::classic::Session_impl::Arena which is symmetrically shared (allocated-in, etc.) by both sides.
   */
  using Arena = ipc::shm::arena_lend::jemalloc::Ipc_arena;

  /// See shm::arena_lend::jemalloc::Session_mv counterpart for public description.
  using Shm_session = jemalloc::Shm_session;

  /// See shm::arena_lend::jemalloc::Session_mv counterpart for public description.
  template<typename Message_body>
  using Structured_channel
    = transport::struc::shm::arena_lend::jemalloc::Channel<typename Base::Channel_obj, Message_body>;

  /// See shm::arena_lend::jemalloc::Session_mv counterpart for public description.
  using Structured_msg_builder_config = typename transport::struc::shm::arena_lend::jemalloc::Builder::Config;

  /// See shm::arena_lend::jemalloc::Session_mv counterpart for public description.
  using Structured_msg_reader_config = typename transport::struc::shm::arena_lend::jemalloc::Reader::Config;

  /// Alias for a light-weight blob used in borrow_object() and lend_object().
  using Blob = Shm_session::Blob;

  // Constructors/destructor.

  /// Inherit ctor.
  using Base::Base;

  /**
   * See shm::arena_lend::jemalloc::Session_mv counterpart.
   *
   * @note `app_shm()` is only in our sub-class Server_session_impl, not Client_session_impl.
   *
   * @return See shm::arena_lend::jemalloc::Session_mv counterpart.
   */
  Arena* session_shm();

  /**
   * See shm::arena_lend::jemalloc::Session_mv counterpart.
   * @return See shm::arena_lend::jemalloc::Session_mv counterpart.
   */
  std::shared_ptr<Arena> session_shm_ptr();

  /**
   * See shm::arena_lend::jemalloc::Session_mv counterpart.
   *
   * @param handle
   *        See shm::arena_lend::jemalloc::Session_mv counterpart.
   * @return See shm::arena_lend::jemalloc::Session_mv counterpart.
   */
  template<typename T>
  Blob lend_object(const typename Arena::template Handle<T>& handle);

  /**
   * See shm::arena_lend::jemalloc::Session_mv counterpart.
   *
   * @param serialization
   *        See shm::arena_lend::jemalloc::Session_mv counterpart.
   * @return See shm::arena_lend::jemalloc::Session_mv counterpart.
   */
  template<typename T>
  typename Arena::template Handle<T> borrow_object(const Blob& serialization);

  /**
   * See shm::arena_lend::jemalloc::Session_mv counterpart.
   * @return See shm::arena_lend::jemalloc::Session_mv counterpart.
   */
  Structured_msg_builder_config session_shm_builder_config();

  /**
   * See shm::arena_lend::jemalloc::Session_mv counterpart.
   * @return See shm::arena_lend::jemalloc::Session_mv counterpart.
   */
  typename Structured_msg_builder_config::Builder::Session session_shm_lender_session();

  /**
   * See shm::arena_lend::jemalloc::Session_mv counterpart.
   * @return See shm::arena_lend::jemalloc::Session_mv counterpart.
   */
  Structured_msg_reader_config session_shm_reader_config();

  /**
   * See shm::arena_lend::jemalloc::Session_mv counterpart.
   *
   * @note `app_shm_builder_config()` is only in our sub-class Server_session_impl, not Client_session_impl.
   *       Yet this app_shm_reader_config() exists in both.  Again see the public counterpart for discussion
   *       as to why/the exact meaning of app_shm_reader_config() for arena-lending SHM-providers.
   *
   * @return See shm::arena_lend::jemalloc::Session_mv counterpart.
   */
  Structured_msg_reader_config app_shm_reader_config();

  /**
   * See shm::arena_lend::jemalloc::Session_mv counterpart.
   * @return See shm::arena_lend::jemalloc::Session_mv counterpart.
   */
  Shm_session* shm_session();

  /**
   * See shm::arena_lend::jemalloc::Session_mv counterpart.
   * @return See shm::arena_lend::jemalloc::Session_mv counterpart.
   */
  Structured_msg_reader_config shm_reader_config();

  // The LOG_*() macros don't see Log_context::get_log*() from base otherwise....
  using flow::log::Log_context::get_logger;
  using flow::log::Log_context::get_log_component;

protected:
  // Types.

  /**
   * How the SHM-provider refers to a #Shm_session.  As of this writing publicly they are exclusively available via
   * these handles (I (ygoldfel) believe there might be plans to eliminate this requirement).
   * At any rate, until then, an #Shm_session_ptr null state is useful as "not-an-arena-yet" semantic;
   * if indeed the mandatory-ref-counted-handle semantic goes away, this should probably become
   * `unique_ptr<Shm_session>`.
   */
  using Shm_session_ptr = std::shared_ptr<Shm_session>;

  // Methods.

  /**
   * To be invoked at most once, synchronously sets up nearly-all aspects of this arena-lending-SHM-capable `Session`;
   * the prerequisite from sub-class `_impl` being that the possibly-asynchronous steps leading up to this have been
   * completed successfully.  Namely the arg `local_hndl` is the local half of the connected socket-pair created
   * by the server as needed to create the internal-use IPC channel for the SHM-provider; and
   *   - Client_session_impl: it has async-received this item, after opposing Server_session_impl generated it;
   *   - Server_session_impl: it has generated it and successfully sent the other (of 2) handle to opposing
   *     Client_session_impl.
   *
   * That says "nearly-all aspects."  That is because Server_session_impl must do one extra thing; namely also create
   * the `.app_shm()`-returned #Arena and pass that to us as `app_shm_or_null`.
   *
   * @param local_hndl
   *        See above.  Must not be `.null()`; else behavior undefined (assertion may trip).
   *        `local_hndl` becomes `.null()` at exit.  Regardless of init_shm() success or failure, it shall ensure
   *        return of the contained native handle to the OS at the proper time (the caller can rest easy).
   * @param pool_name_fragment_or_empty
   *        Function that shall help compute SHM-pool names as needed by the SHM-provider.
   *        In particular the final absolute pool name, for each pool, shall be /P/X/R, where P and R will
   *        be determined by `*this` internally; while X -- which shall exclude the leading and trailing
   *        util::Shared_name::S_SEPARATOR (represented as forward-slash in this explanation) -- is
   *        supplied in `pool_name_fragment_or_empty`.  If the arg is `.empty()`, then the /X/ part shall be
   *        omitted.  Rationale: for cleanup purposes Client_session_impl needs to encode certain info
   *        (spoiler alert: session-client process ID) that can be later parsed-out upon performing a
   *        `for_each_persistent()` (spoiler alert: directory-listing of /dev/shm in Linux at least).
   *        More generally perhaps other info could be encoded there so as to be acquired from an existing
   *        pool-name later.  So this here provides the opportunity for our sub-classes to do that.
   * @param app_shm_or_null
   *        See above.  It'll be registered with shm_session() unless null.
   * @return Whether it was successful (creation of, at least, the #Arena may fail); falsy if so,
   *         the reason describing failure if not.  In the former case, post-conditions:
   *         session_shm() and shm_session() are not null.
   */
  Error_code init_shm(util::Native_handle&& local_hndl,
                      const Shared_name& pool_name_fragment_or_empty,
                      const std::shared_ptr<Arena>& app_shm_or_null = {});

  /**
   * Undoes init_shm_arenas(), namely destroying shm_session() pointee and session_shm() pointee.  Note
   * that any other arena (perhaps Server_session_impl::app_shm() pointee?) registered with
   * shm_session() continues to exist; just objects from it cannot be `lend_object()`ed through that
   * shm_session() anymore (but may well be lendable through other sessions -- to other processes).
   *
   * Intended as of this writing as a one-time resource clean in case of subsequent failure.
   */
  void reset_shm();

private:
  // Data.

  /// See session_shm().  This becomes non-null in init_shm().
  std::shared_ptr<Arena> m_session_shm;

  /// Internal-use transport::struc::Channel for #m_shm_session.  null <=> #m_shm_session is null.
  std::optional<typename Shm_session::Shm_channel> m_shm_channel;

  /// See shm_session().  This becomes non-null in init_shm().
  Shm_session_ptr m_shm_session;
}; // class Session_impl

// Free functions: in *_fwd.hpp.

// Template implementations.

/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define TEMPLATE_JEM_SESSION_IMPL \
  template<typename Session_impl_t>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define CLASS_JEM_SESSION_IMPL \
  Session_impl<Session_impl_t>

TEMPLATE_JEM_SESSION_IMPL
Error_code CLASS_JEM_SESSION_IMPL::init_shm
             (util::Native_handle&& local_hndl,
              const Shared_name& pool_name_fragment_or_empty,
              const std::shared_ptr<Arena>& app_shm_or_null)
{
  using ipc::shm::arena_lend::jemalloc::Memory_manager;
  using flow::util::ostream_op_string;
  using std::make_shared;
  using std::string;
  using std::to_string;

  // We are in thread W (of vanilla Server/Client_session_impl).

  assert(!local_hndl.null());

  assert(!m_session_shm);
  assert(!m_shm_session);

  // First m_session_shm (per-session-scope Arena).  This can fail.

  /* Arena needs a pool namer it will invoke each time the memory manager decides it wants a new mmap()ped vaddr
   * space (in the SHM context, <-> SHM-pool).  We use our standard semantics for per-session items; but
   * possibly add pool_name_fragment_or_empty for reasons explained in our method doc header.  To save some compute
   * cycles pre-compute the prefix and remember it via capture. */
  auto shm_pool_name_prefix = build_conventional_shared_name(Shared_name::S_RESOURCE_TYPE_ID_SHM,
                                                             Shared_name::ct(Base::Base::m_srv_app_ref.m_name),
                                                             Base::Base::srv_namespace(),
                                                             Shared_name::ct(Base::Base::cli_app_ptr()->m_name),
                                                             Base::Base::cli_namespace());
  shm_pool_name_prefix /= SHM_SUBTYPE_PREFIX;
  if (!pool_name_fragment_or_empty.empty())
  {
    shm_pool_name_prefix /= pool_name_fragment_or_empty;
  }
  shm_pool_name_prefix /= ""; // Might as well remember the last separator too.

  auto pool_naming_func = [shm_pool_name_prefix = std::move(shm_pool_name_prefix)](auto pool_id) -> string
  {
    // @todo Arena expects string at the moment; might/should change to Shared_name at some point.

    string shm_pool_name = shm_pool_name_prefix.str();
    // @todo to_string() deals with locales which may be slow.  Consider std::to_chars().
    return shm_pool_name += to_string(pool_id);
  }; // auto pool_naming_func =

  m_session_shm
    = Arena::create(get_logger(),
                    /* @todo At the moment this is fine: other that storing Logger* this is data-free in practice.
                     * TBD whether "formally" this is correct, or we should be storing a singleton or let user
                     * pass-in a thing, or what; particularly if this goes beyond jemalloc specifically.
                     * At the moment it really is fine; even though it derives from a generic-looking Memory_manager,
                     * echan confirms that's at the moment a basically-abandoned idea.  Nor is there a practical plan
                     * to un-abandon it. */
                    make_shared<Memory_manager>(get_logger()),
                    typename Arena::Shm_object_name_generator(std::move(pool_naming_func)),
                    /* This is clearly correct if *this super-classes Server_session_impl; but what about
                     * Client_session_impl, given that the setting comes from Server_app m_srv_app_ref?
                     * Answer: Yes, still; see discussion in Server_app::m_permissions_level_for_client_apps doc
                     * header. */
                    util::shared_resource_permissions
                      (Base::Base::m_srv_app_ref.m_permissions_level_for_client_apps));
  if (!m_session_shm)
  {
    FLOW_LOG_WARNING("Session [" << * this << "]: Failed to create session-scope Arena; "
                     "details may be found above.  Session will not open.");

    /* We promised to dispose of this at the proper time.  Since we won't create the Channel that would do it,
     * let's do it directly. */
    transport::asio_local_stream_socket::release_native_peer_socket(std::move(local_hndl));

    // See @todo on this Code; in short if create() emitted an Error_code, we'd just emit that instead here.
    return error::Code::S_SHM_ARENA_CREATION_FAILED;
  }
  // else

  auto setup_done = make_shared<bool>(false);

  /* Next m_shm_session (the lender from any locally-managed arena(s), including m_session_shm; the borrower
   * from opposing-process-managed arena(s)). */
  const auto nickname = ostream_op_string("jem-", *this);
  m_shm_channel.emplace(get_logger(),
                        transport::Socket_stream_channel_of_blobs<true>
                          (get_logger(), nickname,
                           transport::sync_io::Native_socket_stream
                             (get_logger(), nickname, std::move(local_hndl))),
                        transport::struc::Channel_base::S_SERIALIZE_VIA_HEAP,
                        Base::master_channel_const().session_token());
  m_shm_channel->start([this, setup_done](const Error_code& err_code) mutable
  {
    // We are in a struc::Channel thread (not our thread W).

    /* Shm_session got an incoming-direction channel-hosing error.  I don't think there should be spurious
     * traffic, until we give this thing (SHM-session, SHM-arena) to actually use to user, so whatever is
     * captured here before *setup_done=true should lead to Shm_session::create() or Shm_session::lend_arena()
     * failing which we handle properly.  So eat it until *setup_done is true.
     *
     * Come to think of it, I (ygoldfel) decided to more-or-less just eat it after *setup_done is true too.
     * Basically, once in PEER state, we would just do `if !Base::hosed() { Base::hose(err_code; }`.  Until then,
     * though (but after *setup_done) what to do depends on whether this is Server_session or Client_session.
     * Like for Server_session we could probably do something like on_master_channel_error(); for
     * Client_session similarly.  Thing is, it's painful to set up that plumbing just for this unlikely
     * corner case.  (That might be because of how I've set up this non-virtual hierarchy.  Could probably be
     * rejiggered to by very slick.  @todo maybe.)  So what to do?  For now I feel this:
     *   - If this added socket-stream-based channel dies, so would m_master_channel (also socket-stream-based).
     *   - So it would hose the session anyway, maybe even significantly sooner if it's due to zombification
     *     (it has keep-alive/idle-timer in both directions, unlike at this time this new channel).
     * Thus eating it seems all right in that highly catastrophic scenario, as the session-hosing sensors do
     * their best already.
     *
     * @todo Do set up the plumbing, and don't eat the error.  Consider also @todo above about perhaps
     * rejiggering the Session_impl hierarchy to make that plumbing easy-peasy to set up.  If we develop
     * more Session_impl guys -- for other SHM-providers perhaps -- this would rise in priority.  For now this
     * seems reasonable. */
    Base::async_worker()->post([this, setup_done = std::move(setup_done), err_code]()
    {
      if (*setup_done)
      {
        FLOW_LOG_WARNING("Session [" << * this << "]: Internal-use (for SHM) channel reported incoming-direction "
                         "error [" << err_code << "] [" << err_code.message() << "].  This occured after SHM-setup; "
                         "almost certainly the session master channel will catch a problem or has caught it; "
                         "session will be hosed, or session opening will fail, depending on the situation.");
      }
      else
      {
        FLOW_LOG_WARNING("Session [" << * this << "]: Internal-use (for SHM) channel reported incoming-direction "
                         "error [" << err_code << "] [" << err_code.message() << "].  This occured during SHM-setup, "
                         "so we will catch it or have caught it and will fail to open session.");
      }
    });
  });

  m_shm_session = Shm_session::create(get_logger(),
                                      /* This is correct/vanilla value create() expects; but see note below
                                       * regarding this guy's logging. */
                                      Borrower_shm_pool_collection_repository_singleton::get_instance(),
                                      *m_shm_channel,
                                      /* During its operation (not the init we're doing right now!) a synchronous
                                       * Shm_channel op (send(), sync_request()) may yield a channel-hosing error --
                                       * at most once -- and then this will be called: */
                                      [this, setup_done]
                                        (const Error_code& err_code) mutable
  {
    /* We are in thread... I (ygoldfel) am not 100% sure at the moment (@todo would be nice to write it down here).
     * But actually it does not matter too much; at worst we're already in our thread W somehow,
     * and we're about to do an unnecessary post().  It is fine.
     *
     * What is important it that Shm_session tried to do a struc::Channel::send() (or sync_request()), and it
     * failed indicating that channel just got hosed; which means the *this Session can't really continue operating;
     * which means, similarly to the above incoming-direction handler do the following.
     *
     * Same comment and @todo(s) apply as in the handler above.  This is a little different, as it's a send error,
     * which wouldn't be reported as a channel error; but still we figure it something went this wrong with this
     * channel, the session master channel will be hosed too.  And even that caveat (again) only matters
     * after *setup_done; before *setup_done we will absolutely catch the problem before exiting the present method. */
    Base::async_worker()->post([this, setup_done = std::move(setup_done), err_code]()
    {
      if (*setup_done)
      {
        FLOW_LOG_WARNING("Session [" << * this << "]: Internal-use (for SHM) channel reported outgoing-direction "
                         "error [" << err_code << "] [" << err_code.message() << "].  This occured after SHM-setup; "
                         "almost certainly the session master channel will catch a problem or has caught it; "
                         "session will be hosed, or session opening will fail, depending on the situation.");
      }
      else
      {
        FLOW_LOG_WARNING("Session [" << * this << "]: Internal-use (for SHM) channel reported outgoing-direction "
                         "error [" << err_code << "] [" << err_code.message() << "].  This occured during SHM-setup, "
                         "so we will catch it or have caught it and will fail to open session.");
      }
    });
  });
  /* @todo Shm_session::create() takes an optional timeout for any Shm_channel::sync_request() (send, await response)
   * op it might do; if omitted it's infinite.  Assuming no unfathomable Shm_channel IPC error, a long duration
   * for such a .sync_request() in this context implies some kind of pathologically bad SHM-related operation.
   * For the time being we have no guidance on what that would look like, so we're assuming things will be fine
   * to not worry about limiting this.  Reconsider (perhaps after observing in the field). */

  /* Regarding Borrower_shm_pool_collection_repository logging:
   * Borrower_shm_pool_collection_repository is used in singleton fashion across the entire process, period.
   * Hence it's a glorified global.  That's fine; not our problem; but one aspect that's potentially our problem
   * is its logging.  It has a thread-unsafe .set_logger(Logger*) API subsequent to which it will do mostly-TRACE
   * logging -- though in unlikely error situations also WARNINGs which would be important (as usual for WARNINGs)
   * to see.  If not called, it just won't log (the usual behavior in flow::log when a Logger* is null) which is not
   * ideal.  So should we call it here?  Or somewhere?  Answer: Well, no.  In particular if *this is
   * Server_session_impl, then there can be 2+ of us; and while in production there shouldn't be 2+
   * `Client_session_impl`s around, it is allowed for test/debug/profile purposes.  Now, we could make some inferences
   * and do something like:
   *   - Session_server -- of which there should be (in production anyway) only 1 ever in a given process -- can
   *     do it in its ctor.
   *   - Client_session -- of which (as just noted) there should be at most 1 at a time (in production anyway) -- can
   *     do it here; meaning the "active" Client_session is always sharing its get_logger() with this singleton guy.
   * It's too janky.  Can I guarantee the singleton guy isn't logging *something* while we do our "heuristic"
   * .set_logger() call on it?  Not really.  For that matter these "in production anyway" claims aren't exactly
   * iron-clas guarantees either.  So what to do?
   *
   * There may be something smarter we can do to make this automagic (@todo look into it; discuss with echan), but
   * in the meantime it seems all-right to ask the user to make the proper
   *
   *   Borrower_shm_pool_collection_repository_singleton::get_instance().set_logger()
   *
   * call in a truly pre-session-work, single-thread context -- at start of main() or something -- to set up
   * this special guy's logging independently of (prior to) various ipc::session lifecycles.  So we do that
   * in public docs elsewhere.
   *
   * @todo Again -- it would be nice to make this automagic but not exactly high-priority.  It *would* be nice, as
   * literally everything else arena_lend-related is not all global-like like this, and we set it up internally
   * without making the user do *anything*.  Just this particular ask is fairly timid, so it's all right. */

  assert(m_shm_session && "It's not supposed to fail according to contract.");

  // Next register one with the other, so stuff constructed in session_shm() can be lend_object()ed.

  if (!shm_session()->lend_arena(session_shm_ptr()))
  {
    FLOW_LOG_WARNING("Session [" << * this << "]: Registering session-scope local Arena with borrow/lend engine: "
                     "failed (details likely above).  Assuming we are bug-free in using the SHM-provider API "
                     "this would occur only on internal SHM-provider IPC channel error; but due to certain internal "
                     "reasons the triggering Error_code has not reached this thread yet; reporting a more general "
                     "Error_code.  That detail aside: Session will not open.");
    /* Like it says, it'd be nice to report the triggering Error_code.  As of this writing lend_arena() does not
     * report such a thing.  In reality, as we just said, probably the just-created (successfully!) Channel
     * reported error on send() or sync_request() or maybe incoming-direction error somehow;
     * so it will be reported through an on-error guy up above.
     * We cannot wait for that though -- I mean we could, but in the meantime what do we return, success?  No.
     * So we just eat that error in that handler and emit the following catch-all thing.
     * @todo Ideally lend_arena(), being a synchronous API that does internal IPC (synchronously, meaning it sends)
     * would just emit an Error_code and not (in that case) send it through the post_hose()ing guy.  Then we'd just
     * emit it here and not need the setup_done thing either.  What we do, in fact, do is decent enough though. */

    m_shm_session.reset(); // Be somewhat aggressive, though they'd go away in destructor anyway.
    m_shm_channel.reset();
    m_session_shm.reset();
    return error::Code::S_SHM_ARENA_LEND_FAILED;
  } // if (!shm_session()->lend_arena(session_shm()))
  // else

  // Lastly register per-app-scope one if so asked (if Server_session_impl is the sub-class invoking us).

  if (app_shm_or_null && (!shm_session()->lend_arena(app_shm_or_null)))
  {
    FLOW_LOG_WARNING("Session [" << * this << "]: Registering app-scope local Arena with borrow/lend engine: "
                     "failed (details likely above).  Assuming we are bug-free in using the SHM-provider API "
                     "this would occur only on internal SHM-provider IPC channel error; but due to certain internal "
                     "reasons the triggering Error_code has not reached this thread yet; reporting a more general "
                     "Error_code.  That detail aside: Session will not open.");

    m_shm_session.reset();
    m_shm_channel.reset();
    m_session_shm.reset();
    return error::Code::S_SHM_ARENA_LEND_FAILED;
  }
  // else

  *setup_done = true;

  FLOW_LOG_INFO("Session [" << * this << "]: Successfully created session-scope Arena; and "
                "borrow/lend engine Shm_session; registered the former to the latter; "
                "registered app-scope arena too? = [" << bool(app_shm_or_null) << "].");

  return Error_code();
} // Session_impl::init_shm()

TEMPLATE_JEM_SESSION_IMPL
void CLASS_JEM_SESSION_IMPL::reset_shm()
{
  m_shm_session.reset();
  m_shm_channel.reset();
  m_session_shm.reset();
}

TEMPLATE_JEM_SESSION_IMPL
typename CLASS_JEM_SESSION_IMPL::Arena* CLASS_JEM_SESSION_IMPL::session_shm()
{
  return m_session_shm.get();
}

TEMPLATE_JEM_SESSION_IMPL
std::shared_ptr<typename CLASS_JEM_SESSION_IMPL::Arena> CLASS_JEM_SESSION_IMPL::session_shm_ptr()
{
  return m_session_shm;
}

TEMPLATE_JEM_SESSION_IMPL
typename CLASS_JEM_SESSION_IMPL::Shm_session* CLASS_JEM_SESSION_IMPL::shm_session()
{
  return m_shm_session.get();
}

TEMPLATE_JEM_SESSION_IMPL
template<typename T>
typename CLASS_JEM_SESSION_IMPL::Blob
  CLASS_JEM_SESSION_IMPL::lend_object(const typename Arena::template Handle<T>& handle)
{
  auto blob = shm_session()->template lend_object<T>(handle);

  // @todo Maybe we should just return empty Blob?  For now doing this: SHM-classic will assert() too.  Reconsider both.
  assert((!blob.empty()) && "Most likely `handle` did not come validly from one of `*this`-owned `Arena`s.");

  return blob;
}

TEMPLATE_JEM_SESSION_IMPL
template<typename T>
typename CLASS_JEM_SESSION_IMPL::Arena::template Handle<T>
  CLASS_JEM_SESSION_IMPL::borrow_object(const Blob& serialization)
{
  const auto obj = shm_session()->template borrow_object<T>(serialization);

  // @todo Maybe we should just return nullptr?  For now doing this: SHM-classic will assert() too.  Reconsider both.
  assert(obj && "Most likely `serialization` is invalid.");

  return obj;
}

TEMPLATE_JEM_SESSION_IMPL
typename CLASS_JEM_SESSION_IMPL::Structured_msg_builder_config
  CLASS_JEM_SESSION_IMPL::session_shm_builder_config()
{
  return Structured_msg_builder_config{ get_logger(), 0, 0, session_shm() };
}

TEMPLATE_JEM_SESSION_IMPL
typename CLASS_JEM_SESSION_IMPL::Structured_msg_builder_config::Builder::Session
  CLASS_JEM_SESSION_IMPL::session_shm_lender_session()
{
  return shm_session();
}

TEMPLATE_JEM_SESSION_IMPL
typename CLASS_JEM_SESSION_IMPL::Structured_msg_reader_config
  CLASS_JEM_SESSION_IMPL::shm_reader_config()
{
  return Structured_msg_reader_config{ get_logger(), shm_session() };
}

TEMPLATE_JEM_SESSION_IMPL
typename CLASS_JEM_SESSION_IMPL::Structured_msg_reader_config
  CLASS_JEM_SESSION_IMPL::session_shm_reader_config()
{
  return shm_reader_config();
}

TEMPLATE_JEM_SESSION_IMPL
typename CLASS_JEM_SESSION_IMPL::Structured_msg_reader_config
  CLASS_JEM_SESSION_IMPL::app_shm_reader_config()
{
  return shm_reader_config();
}

TEMPLATE_JEM_SESSION_IMPL
std::ostream& operator<<(std::ostream& os, const CLASS_JEM_SESSION_IMPL& val)
{
  return os << static_cast<const typename CLASS_JEM_SESSION_IMPL::Base&>(val);
}

#undef CLASS_JEM_SESSION_IMPL
#undef TEMPLATE_JEM_SESSION_IMPL

} // namespace ipc::session::shm::arena_lend::jemalloc
