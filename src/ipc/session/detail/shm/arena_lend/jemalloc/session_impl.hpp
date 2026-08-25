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
#include "ipc/session/standalone/shm/arena_lend/detail/borrower_shm_pool_collection_repository.hpp"
#include "ipc/session/standalone/shm/arena_lend/jemalloc/shm_session.hpp"
#include "ipc/session/shm/arena_lend/jemalloc/error.hpp"
#include "ipc/session/shm/arena_lend/jemalloc/jemalloc.hpp"
#include "ipc/session/detail/shm/arena_lend/jemalloc/jemalloc_fwd.hpp"
#include "ipc/session/detail/session_shared_name.hpp"
#include "ipc/transport/struc/shm/arena_lend/jemalloc/jemalloc_fwd.hpp"
#include "ipc/transport/struc/shm/arena_lend/jemalloc/jemalloc.hpp"
#include "ipc/transport/struc/struc_fwd.hpp"
#include "ipc/util/util_fwd.hpp"

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
 * the differing APIs (e.g., Client_session_impl::sync_connect()).
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
  typename Arena::template Handle<T> borrow_object(const Blob& serialization) const;

  /**
   * See shm::arena_lend::jemalloc::Session_mv counterpart.
   *
   * @param segment1_sz
   *        See eponymous arg to, say, transport::struc::sync_io::Channel ctor with `Serialize_via_session_shm` tag.
   * @return See shm::arena_lend::jemalloc::Session_mv counterpart.
   */
  Structured_msg_builder_config session_shm_builder_config(size_t segment1_sz);

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
   *        Fragment to add, unless empty, into the SHM-pool name when pool(s) is/are created by SHM-provider.
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

  /**
   * With the pre-condition that this session is in PEER or (in `Server_session` case) at least almost-PEER state,
   * and we are currently in thread W (`Base::async_worker()`), and `Base::hose()` has not been called,
   * performs `session_shm()->sample_hi_wmarks()` and schedules for this to occur again in some regular period
   * of time.  Stops ticklings once PEER state is reached and `hosed() == true`; or `session_shm()` is null.
   * (Not saying the latter is even possible... but if it were possible and happened, then the stat-tickling and
   * further scheduling thereof stops.)
   *
   * ### Rationale / Background ###
   * Consider having *just* reached PEER or almost-PEER state (the latter = session is open, but user must still call
   * `Server_session::init_handlers()` to reach PEER state; we do not care about that either way here).  So then:
   *
   * Done!  Success!  At this moment: In (almost-)PEER state, in thread W:
   * We shall now add some value.  This requires a little background though.  SHM-jemalloc's jemalloc::Ipc_arena
   * and `Shm_session` (aliased for genericness in our `Session`-land as `Arena` and `Shm_session`
   * respectively), and a whole complicated host of thread-local and global-singleton modules that aid them,
   * maintain various stats a-la `flow::util::stat`.  That's all basically self-contained and not special.
   * Also, formally speaking, not special: For some of its many stats, it has to use particular stat-collection
   * techniques (spoiler alert: (1) TL-sharding; (2) querying memory manager -- jemalloc -- as data source)
   * that have the implication on how `_hi_wmark` stat-members are computed/what they mean.  See
   * `Sharded_stats` and `Memory_manager_stats` doc headers for details, but in short: a given `m_X_hi_wmark`
   * is only across samples taken at stat-consumption time, because the value of `m_X` cannot be
   * continuously watched in-between.  So, in order for those particular members -- the HWMs
   * (high-water-marks) -- to be at least somewhat useful, stats have to be sampled regularly.
   *
   * A direct user of `Ipc_arena`/`Shm_session`/SHM-jemalloc would -- if they even care about this corner which
   * is certainly not a given -- have to be responsible for the regular stat-consuming (even when not
   * interested in the actual stats at those times).  We, however, have a nice, low-activity worker thread
   * and event loop (thread W!), so we can easily do this for them.
   *
   * The mechanics: `Shm_session`-oriented stats are not involved in this; the HWMs if any are accurate sans
   * intervention.  So it's just `Ipc_arena`.  One just calls: `arena.sample_hi_wmarks()`.
   *
   * A question is, what's `arena` for us?  Answer:
   *   - There is `*(session_shm())`.  That's one.  We just set it up earlier using init_shm().
   *   - There is `*(app_shm())`.  That's the other one.  That has a lifetime unrelated to a particular
   *     single session (`*this` included), though, and is therefore maintained by jemalloc::Session_server.
   *     So `Session_server` is responsible for handling the `app_shm()` guys in this respect.
   *
   * So that's that.  Let us set-up the periodic `session_shm()`-tickling.  Specifically: Tickle it "now,"
   * upon having reached (almost-)PEER state, and schedule the next one.
   */
  void session_shm_stats_tickle_and_schedule();

private:
  // Data.

  /// See session_shm().  This becomes non-null in init_shm().
  std::shared_ptr<Arena> m_session_shm;

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

  /* Arena needs a pool name prefix it will use each time the memory manager decides it wants a new mmap()ped vaddr
   * space (in the SHM context, <-> SHM-pool).  We use our standard semantics for per-session items; but
   * possibly add pool_name_fragment_or_empty for reasons explained in our method doc header. */
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

  m_session_shm
    = Arena::create(get_logger(),
                    /* @todo At the moment this is fine: it is even data-free in practice.
                     * TBD whether "formally" this is correct, or we should be storing a singleton or let user
                     * pass-in a thing, or what; particularly if this goes beyond jemalloc specifically.
                     * At the moment it really is fine; even though it derives from a generic-looking Memory_manager,
                     * echan confirms that's at the moment a basically-abandoned idea.  Nor is there a practical plan
                     * to really un-abandon it. */
                    make_shared<Memory_manager>(),
                    std::move(shm_pool_name_prefix),
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
    local_hndl.close();

    // See @todo on this Code; in short if create() emitted an Error_code, we'd just emit that instead here.
    return error::Code::S_SHM_ARENA_CREATION_FAILED;
  }
  // else

  auto setup_done = make_shared<bool>(false);

  /* Next m_shm_session (the lender from any locally-managed arena(s), including m_session_shm; the borrower
   * from opposing-process-managed arena(s)).
   *
   * It needs this channel for internal communications, notably about lending arenas (as we will below)
   * and subsequently pools within those (automatically). */

  const auto nickname = ostream_op_string("jem-", *this);
  /* Sure, Shm_session gave us this nice alias; but really it's a specific type, and we must feed it specific things;
   * namely (substantively speaking) the local_hndl we painstakingly obtained during our session setup. */
  Shm_session::Shm_channel raw_shm_channel{get_logger(), nickname,
                                           transport::sync_io::Native_socket_stream
                                             {get_logger(), nickname, std::move(local_hndl)}};

  /* Please see Native_socket_stream::remote_peer_process_credentials() doc header for explanation of this.
   * (Technically this is only necessary server-side; client-side the values will already be correct, because
   * the connect_pair() call occurred on the opposing-to-us side; and hence will overwrite X with X -- a no-op.
   * Nevertheless doing it this way is (1) simpler code; (2) perf-wise of negligible impact; and (3) more
   * maintainable.  In any case master_channel_const().owned_channel()->remote_peer_process_credentials() has
   * correct values, so shoving them into raw_shm_channel is not-wrong.) */
  {
    Error_code err_code;
    raw_shm_channel.blob_snd()->remote_peer_process_credentials
      (Base::master_channel_const().owned_channel()->remote_peer_process_credentials(&err_code));

    assert((!err_code) && "By contract that should only fail if the socket got hosed via transmission; but "
                            "either it is a local socket we just established (in which case there is no way) or "
                            "one just generated by opposing server, and it should be fine as coming from "
                            "trusted source; though it might be nice (@todo) to emit it as an error anyway.");
  }

  // Recommend reading create() doc header; it explains how error reporting works w/r/t raw_shm_channel.
  m_shm_session = Shm_session::create(get_logger(),
                                      std::move(raw_shm_channel), // m_shm_session owns it fully now.
                                      Base::master_channel_const().session_token(),

                                      /* During its operation (not the init we're doing right now!) a synchronous
                                       * Shm_channel op (send(), sync_request()) may yield a channel-hosing error --
                                       * at most once -- and then this will be called: */
                                      [this, setup_done]
                                        (const Error_code& err_code) mutable
  {
    /* We are in m_shm_session's background thread.  It calls it thread W, but that's not our
     * thread W (of vanilla Server/Client_session_impl).
     *
     * Shm_session got an incoming-direction channel-hosing error, or an outgoing-direction error trying to do
     * send() or eqiuvalent; either way per contract it reports such a thing up to *once* from the aforementioned
     * thread.  As discussed in Shm_session::create() our choices here are:
     *   - Eat it (log and that's it), because we have our own way(s) of reponsively detecting session-hosing
     *     and then <handle session hosing> (discussed a bit down).
     *   - Don't eat it: It indicates Shm_session permanently lost contact with its peer, hence we must now
     *     <handle session hosing>.
     *
     * We are going with "eat it."  It is just simpler.
     *   - If this added socket-stream-based channel dies, so would m_master_channel (also socket-stream-based).
     *     We have to, and do, handle that.
     *   - So it would hose the session anyway, maybe even sooner if it's due to zombification
     *     (it has keep-alive/idle-timer in both directions, unlike at this time this new channel).
     *
     * What is <handle session hosing>?  There are 3 phases involved, and finding out about session-hosing
     * could happen in any of them.
     *   -# Before *setup_done becomes true, a few lines below.  Namely we will make 1-2 m_shm_session->lend_arena()
     *      calls.
     *   -# After *setup_done becomes true but before PEER state; also quite soon.  The actual steps taken
     *      depend on whether this is Server_session or Client_session.
     *   -# Upon reaching PEER state.
     *
     * In phase 3, which is the long one, as of this writing do basically: `if !Base::hosed() { Base::hose(err_code; }`
     * which invokes the session on-error handler that must have been provided by user to enter PEER state.
     *
     * In phase 2: details omitted, but at a high level both Client_session and Server_session paths properly
     * deal with whatever might go wrong (on_master_channel_error() on server, and what-not).
     *
     * In phase 1: it is not per se special; in this context it is the same as phase 2.  That said, each of the 1-2
     * m_shm_session->lend_arena() calls can return false.  By contract the meaning is: "couldn't do it, because
     * the attempt to inform peer Shm_session via raw_shm_channel.send()/equivalent synchronously failed, and we will
     * fire the callback you gave us [what you are reading now] from a background thread with more info, 1x."
     * Hence we could literally ignore lend_arena()'s return value.  Our own session-hosing sensing will handle it,
     * and the fact that the other side isn't aware of 1 or both of the relevant of our arenas won't matter:
     * we're going to fail to open the session, or worst case detect it immediately in phase 3 and fire the
     * session on-error handler.
     *
     * Nevertheless we don't ignore lend_arena()'s return value(s).  If 1 fails, we emit an error to that effect
     * and fail immediately in phase 1.  (Note that all this is quite unlikely in the first place.  Nevertheless
     * we should do the right thing.)  Why?  It isn't cut-and-dried.  The drawback is that we won't know the
     * exact way the link was severed, in the form of an Error_code: our session-hosing sensors produce this
     * (basically: m_master_channel's socket stream disconnected with errno so-and-so => Error_code), but we will
     * shortcircuit the (very) short wait for that and just synchronously emit a generic Error_code here.
     * The benefit is simply a reduction of entropy by acting synchronously with information that's synchronously
     * available.  (At least logs will include the "real" Error_code; but programmatically it's not available to
     * the user which is a bit of a downgrade.  It's not a massive loss given the context.)
     *
     * So that's the grand plan.  And as part of that plan, here we will only log. */
    Base::async_worker()->post([this, setup_done = std::move(setup_done), err_code]()
    {
      // We are in thread W (of vanilla Server/Client_session_impl).
      if (*setup_done)
      {
        FLOW_LOG_WARNING("Session [" << * this << "]: Internal-use (for SHM) channel reported "
                         "error [" << err_code << "] [" << err_code.message() << "].  This occured after SHM-setup; "
                         "almost certainly the session master channel and/or attempts to lend/borrow "
                         "will catch a problem or have caught it; "
                         "session will be hosed, or session opening will fail, depending on the situation.");
        // That last part is an oblique reference to the above so-called phases 2 and 3.
      }
      else
      { // So-called phase 1 above.
        FLOW_LOG_WARNING("Session [" << * this << "]: Internal-use (for SHM) channel reported "
                         "error [" << err_code << "] [" << err_code.message() << "].  This occured during SHM-setup, "
                         "so we will catch it or have caught it and will fail to open session.");
      }
    });
  }); // m_shm_session = Shm_session::create()

  assert(m_shm_session && "It's not supposed to fail according to contract.");

  // Next register shm_session() with session_shm(), so stuff constructed in session_shm() can be lend_object()ed.

  if (!shm_session()->lend_arena(session_shm_ptr()))
  {
    /* This message recaps the trade-off between failing out here versus letting session-hosing-sensing occur
     * that we discussed in the comment inside the functor given to create() above. */
    FLOW_LOG_WARNING("Session [" << * this << "]: Registering session-scope local Arena with borrow/lend engine: "
                     "failed (details likely above).  Assuming we are bug-free in using the SHM-provider API "
                     "this would occur only on internal SHM-provider IPC channel error; but due to certain internal "
                     "reasons the triggering Error_code has not reached this thread yet; reporting a more general "
                     "Error_code.  That detail aside: Session will not open.");

    m_shm_session.reset(); // Be somewhat aggressive, though they'd go away in destructor anyway.
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
    m_session_shm.reset();
    return error::Code::S_SHM_ARENA_LEND_FAILED;
  }
  // else

  *setup_done = true;

  FLOW_LOG_INFO("Session [" << * this << "]: Successfully created session-scope Arena; and "
                "borrow/lend engine Shm_session; registered the former to the latter; "
                "registered app-scope arena too? = [" << bool(app_shm_or_null) << "].");

  return {};
} // Session_impl::init_shm()

TEMPLATE_JEM_SESSION_IMPL
void CLASS_JEM_SESSION_IMPL::session_shm_stats_tickle_and_schedule()
{
  using util::Call_timing;
  using boost::chrono::seconds;

  /* Firstly see our doc header; then come back here.
   *
   * Secondly let's discuss the appropriate (default at least) period for this tickling.  In actual fact this
   * will ultimately invoke, at most, Ipc_arena::sharded_stats() and Ipc_arena::memory_manager_stats().
   * The tickling won't log anything INFO-or-higher, so it isn't about that.  Computationally it's not exactly cheap --
   * have to aggregate a bunch of thread-local stat-shards and query jemalloc-stats respectively.  It is probably
   * okay every second though.  We're going on the safe side and doing it 10x less frequently than that.
   * The tension is of course with trying to increase the resolution of the handful of HWM values involved.
   * 10 seconds "feels" decent. */
  constexpr util::Fine_duration TICKLE_PERIOD = seconds{10}; // @todo Provide a knob including ability to turn off?

  // We are in thread W.

  assert(((!Base::Base::on_err_func_set()) || (!Base::Base::hosed()))
         && "Bug: internal contract is to call this only pre-hose().");

  const auto arena = session_shm();
  if (!arena)
  {
    /* Don't even care why or how (maybe it's even impossible; does not matter).  No arena anymore for some reason?
     * Then nothing to tickle.  Move on.  It's deliberately defensive/self-contained logic. */
    return;
  }
  // else

  /* Background for the Call_timing thing is (1) in jemalloc::Memory_manager doc header and (2) in Call_timing docs.
   * We won't repeat the details; but as for the timing here: It is just periodic; we have no idea what is happening
   * simultaneously.  Hence it's pretty much one of the 2 model examples of POSSIBLY_UNSAFE (the other being:
   * doing this during shutdown).
   *
   * (If you're reading this and feeling worried: Don't be worried.  With jemalloc>=5.3.0 there is no issue at all:
   * period.  With <5.3.0 this takes care of business appropriately.)
   *
   * Thread safety: It is safe from any thread by its contract. */
  arena->sample_hi_wmarks(Call_timing::S_POSSIBLY_UNSAFE);

  Base::async_worker()->schedule_from_now(TICKLE_PERIOD, [this](auto&&) mutable
  {
    // We are in thread W.
    if ((!Base::Base::on_err_func_set()) || (!Base::Base::hosed()))
    {
      session_shm_stats_tickle_and_schedule();
    }
    // else { Same as above: Whatever; we're done with the ticklings then.  GTFO. }
  });
} // Session_impl::session_shm_stats_tickle_and_schedule()

TEMPLATE_JEM_SESSION_IMPL
void CLASS_JEM_SESSION_IMPL::reset_shm()
{
  m_shm_session.reset();
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
  return shm_session()->template lend_object<T>(handle);
}

TEMPLATE_JEM_SESSION_IMPL
template<typename T>
typename CLASS_JEM_SESSION_IMPL::Arena::template Handle<T>
  CLASS_JEM_SESSION_IMPL::borrow_object(const Blob& serialization) const
{
  return m_shm_session->template borrow_object<T>(serialization);
}

TEMPLATE_JEM_SESSION_IMPL
typename CLASS_JEM_SESSION_IMPL::Structured_msg_builder_config
  CLASS_JEM_SESSION_IMPL::session_shm_builder_config(size_t segment1_sz)
{
  using transport::struc::shm::stat::Outer_serializer_global_stats;
  using transport::struc::shm::stat::Core_serializer_global_stats;

  return Structured_msg_builder_config{ get_logger(), segment1_sz,
                                        transport::struc::BUILDER_CONFIG_FRAME_PREFIX_SZ_VIA_STRUC_CHANNEL,
                                        session_shm(),
                                        // Default snd-stats targets: per-Arena SHM-msg-{inner,outer} globals.
                                        &Core_serializer_global_stats<Arena>::get()
                                          .stats_mutable_default().m_snd,
                                        &Outer_serializer_global_stats<Arena>::get()
                                          .stats_mutable_default().m_snd };
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
  using transport::struc::shm::stat::Outer_serializer_global_stats;

  return Structured_msg_reader_config{ get_logger(), shm_session(),
                                       // Default rcv-stats target: per-Arena SHM-msg-outer global.
                                       &Outer_serializer_global_stats<Arena>::get()
                                         .stats_mutable_default().m_rcv };
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
