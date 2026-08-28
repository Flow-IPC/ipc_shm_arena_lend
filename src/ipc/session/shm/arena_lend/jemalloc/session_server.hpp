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

#include "ipc/session/shm/arena_lend/jemalloc/jemalloc_fwd.hpp"
#include "ipc/session/shm/arena_lend/jemalloc/server_session.hpp"
#include "ipc/session/detail/session_server_impl.hpp"
#include "ipc/shm/classic/pool_arena.hpp"
#include "ipc/transport/struc/struc_fwd.hpp"
#include "ipc/transport/transport_fwd.hpp"
#include "ipc/util/util_fwd.hpp"
#include <boost/move/make_unique.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <string>

namespace ipc::session::shm::arena_lend::jemalloc
{

// Types.

/**
 * This is to vanilla Session_server what shm::arena_lend::jemalloc::Server_session is to vanilla #Server_session:
 * it is the session-server type that starts SHM-enabled sessions with SHM-jemalloc provider
 * (ipc::shm::arena_lend::jemalloc).  Its API is identical to that of Session_server, except that it emits
 * #Server_session_obj that are shm::arena_lend::jemalloc::Server_session and not vanilla #Server_session.
 *
 * @internal
 *
 * ### Implementation ###
 * See similar section of session::Session_server.  It explains why we sub-class Session_server_impl and even how
 * that's used for this SHM-jemalloc scenario.  To reiterate:
 *
 * We use 2 of 2 available customization points of `private` super-class Session_server_impl.  We:
 *   - pass-up a `per_app_setup_func()` that, given the new session's desired Client_app, creates-if-needed the per-app
 *     SHM-arena and keeps it open as well as available via `this->app_shm(Client_app::m_name)`; and
 *   - parameterize Session_server_impl on shm::arena_lend::jemalloc::Server_session which, during log-in, creates
 *     the per-session SHM-arena and keeps it open; saves `this->app_shm(Client_app::m_name)`;
 *     and partners with Client_session_impl to set up the lend/borrowing capability between us.
 *
 * shm::arena_lend::jemalloc::Server_session doc header delves deeply into the entire impl strategy for setting up
 * these arenas.  If you read/grok that, then the present class's impl should be straightforward to follow.
 *
 * @endinternal
 *
 * @tparam MQ_TYPE_OR_NONE
 *         See vanilla #Session_server.
 * @tparam TRANSMIT_NATIVE_HANDLES
 *         See vanilla #Session_server.
 * @tparam Mdt_payload
 *         See vanilla #Session_server.
 */
template<session::schema::MqType MQ_TYPE_OR_NONE, bool TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
class Session_server :
  private Session_server_impl // Attn!  Emit `shm::arena_lend::jemalloc::Server_session`s (impl customization point).
            <Session_server<MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES, Mdt_payload>,
             Server_session<MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES, Mdt_payload>>
{
private:
  // Types.

  /// Short-hand for our base/core impl.
  using Impl = Session_server_impl
                 <Session_server<MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES, Mdt_payload>,
                  Server_session<MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES, Mdt_payload>>;

public:
  // Types.

  /// Short-hand for the concrete `Server_session`-like type emitted by async_accept().
  using Server_session_obj
    = shm::arena_lend::jemalloc::Server_session<MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES, Mdt_payload>;

  /// Short-hand for Session_mv::Mdt_reader_ptr.
  using Mdt_reader_ptr = typename Impl::Mdt_reader_ptr;

  /// Metadata builder type passed to `mdt_load_func()` in advanced async_accept() overload.
  using Mdt_builder = typename Server_session_obj::Mdt_builder;

  /// Short-hand for Session_mv::Channels.
  using Channels = typename Impl::Channels;

  /// Short-hand for shm::arena_lend::jemalloc::Session_mv::Arena.  See app_shm() in particular.
  using Arena = typename Server_session_obj::Base::Arena;

  /// Short-hand for shm::arena_lend::jemalloc::Session_mv::Structured_msg_builder_config.
  using Structured_msg_builder_config = typename Server_session_obj::Base::Structured_msg_builder_config;

  /// Short-hand for shm::arena_lend::jemalloc::Session_mv::Structured_msg_reader_config.
  using Structured_msg_reader_config = typename Server_session_obj::Base::Structured_msg_reader_config;

  /// You may disregard.
  using Async_io_obj = transport::Null_peer;
  /// Useful for generic programming, the `sync_io`-pattern counterpart to `*this` type.
  using Sync_io_obj = sync_io::Session_server_adapter<Session_server>;

  // Constructors/destructor.

  /**
   * Constructor: identical to session::Session_server ctor.  See its doc header.
   *
   * @warning See same-named section of session::Session_server ctor doc header.  In short: `srv_app_ref`
   *          and `cli_app_master_set_ref` (and its `Client_app`s) must outlive `*this` and any yielded
   *          `Server_session`.
   *
   * @param logger_ptr
   *        See above.
   * @param srv_app_ref
   *        See above.
   * @param cli_app_master_set_ref
   *        See above.
   * @param err_code
   *        See above.
   */
  explicit Session_server(flow::log::Logger* logger_ptr, const Server_app& srv_app_ref,
                          const Client_app::Master_set& cli_app_master_set_ref,
                          Error_code* err_code = nullptr);

  // Methods.

  /**
   * Contract identical to simpler session::Session_server::async_accept() overload; but internally ensures that
   * the appropriate SHM-jemalloc arenas are available for use in the emitted #Server_session_obj.  See doc header for
   * session::Session_server::async_accept() simple overload.  However additional `Error_code`s may be emitted on error.
   *
   * @tparam Task_err
   *         See above.
   * @param target_session
   *        See above.  Reminder (though this is enforced at compile-time):
   *        the type of `*target_session` for Session_server::async_accept() is #Server_session;
   *        whereas here it is shm::arena_lend::jemalloc::Server_session.
   * @param on_done_func
   *        See above.
   */
  template<typename Task_err>
  void async_accept(Server_session_obj* target_session, Task_err&& on_done_func);

  /**
   * Contract identical to advanced Session_server::async_accept() overload; but internally ensures that the appropriate
   * SHM-jemalloc arenas are available for use in the emitted #Server_session_obj.  See doc header for
   * Session_server::async_accept() advanced overload.  However additional `Error_code`s may be emitted on error.
   *
   * @tparam Task_err
   *         See above.
   * @tparam N_init_channels_by_srv_req_func
   *         See above.
   * @tparam Mdt_load_func
   *         See above.
   * @param target_session
   *        See other async_accept() overload.
   * @param init_channels_by_srv_req
   *        See above.
   * @param mdt_from_cli_or_null
   *        See above.
   * @param init_channels_by_cli_req
   *        See above.
   * @param n_init_channels_by_srv_req_func
   *        See above.
   * @param mdt_load_func
   *        See above.
   * @param on_done_func
   *        See above.
   */
  template<typename Task_err,
           typename N_init_channels_by_srv_req_func, typename Mdt_load_func>
  void async_accept(Server_session_obj* target_session,
                    Channels* init_channels_by_srv_req,
                    Mdt_reader_ptr* mdt_from_cli_or_null,
                    Channels* init_channels_by_cli_req,
                    N_init_channels_by_srv_req_func&& n_init_channels_by_srv_req_func,
                    Mdt_load_func&& mdt_load_func,
                    Task_err&& on_done_func);

  /**
   * Identical to eponymous accessor in session::Session_server.
   * @return See above.
   */
  size_t mq_msg_size_limit() const;

  /**
   * Identical to eponymous mutator in session::Session_server.
   * @param limit
   *        See above.
   */
  void mq_msg_size_limit(size_t limit);

  /**
   * Returns pointer to the per-`app` SHM-arena, whose lifetime extends until `*this` is destroyed;
   * or null if the given Client_app has not yet opened at least 1 shm::arena_lend::jemalloc::Server_session via
   * async_accept().  Alternatively you may use shm::arena_lend::jemalloc::Session_mv::app_shm() off any session object
   * filled-out by `*this` async_accept(), as long as its Server_session_mv::client_app() equals
   * `app` (by App::m_name equality).
   *
   * If non-null is returned, then the same pointer value shall be returned for all subsequent calls
   * with the same (by App::m_name equality) `app`.  The non-null pointers returned for any 2 calls, where `app`
   * is different (by App::m_name equality) among them, shall always differ.
   *
   * See shm::arena_lend::jemalloc::Session_mv::Arena doc header for useful instructions on working with #Arena,
   * `lend_object()`, and `borrow_object()`.
   *
   * ### Perf ###
   * Given the choice between Server_session_mv::app_shm() and the present method, the latter is somewhat
   * slower; internally it involves a mutex-protected map lookup, while the former simply returns a cached
   * pointer as of this writing.
   *
   * Generally it is also quite fast for the user to save any non-null value returned by either `app_shm()`;
   * the pointer returned shall always be the same after all.
   *
   * @internal
   * ### Thread safety ###
   * For internal use, namely by shm::arena_lend::jemalloc::Server_session_impl::async_accept_log_in() at least,
   * it is guaranteed the app_shm() may be called on the same `*this` concurrently to itself
   * and init_app_shm_as_needed().  Formally speaking this isn't publicly documented, as I (ygoldfel) didn't want
   * to get users into any bad habit, but internally it does have this property -- as it is required.
   * @endinternal
   *
   * @param app
   *        Client_app whose segregated SHM-arena to return, if a session for a client of the app has been
   *        opened prior to this call.
   * @return Pointer to `*this`-held per-`app` SHM-arena, if it has been created; null otherwise.
   *         See above.
   */
  Arena* app_shm(const Client_app& app);

  /**
   * Identical to app_shm() but returns that via a `shared_ptr`-handle as required for work directly within
   * ipc::shm::arena_lend::jemalloc APIs.  Note this is a quirk of that particular API and, in particular, has
   * no equivalent in ipc::session::shm::classic `Session` and `Session_server` counterparts.
   * It should not be necessary to use except when going beyond ipc::session for your SHM-arena needs.
   *
   * @param app
   *        See above.
   * @return See above.
   */
  std::shared_ptr<Arena> app_shm_ptr(const Client_app& app);

  /**
   * Returns builder config suitable for capnp-serializing out-messages in SHM arena app_shm() for
   * the same `Client_app app`.  Alternatively you may use
   * shm::arena_lend::jemalloc::Session_mv::app_shm_builder_config()
   * off any session object filled-out by `*this` async_accept(), as long as its Server_session_mv::client_app() equals
   * `app` (by App::m_name equality).
   *
   * Unlike app_shm() this method does not allow the case where `app_shm(app)` would have returned null.
   * In that case the present method yields undefined behavior (assertion may trip).
   *
   * ### Perf ###
   * Given the choice between Server_session_mv::app_shm_builder_config() and the present method, the latter is somewhat
   * slower (reason: same as listed in app_shm() doc header).
   *
   * Generally it is also quite fast for the user to save any value returned by either `app_shm_builder_config()`,
   * as an equal-by-value `Config` object shall be returned for the same (by App::m_name equality) `app`.
   *
   * @param app
   *        See app_shm().
   * @param segment1_sz
   *        See eponymous arg to, say, transport::struc::sync_io::Channel ctor with `Serialize_via_app_shm` tag.
   * @return See above.
   */
  Structured_msg_builder_config
    app_shm_builder_config(const Client_app& app,
                           size_t segment1_sz
                                    = sizeof(::capnp::word) * ::capnp::SUGGESTED_FIRST_SEGMENT_WORDS);

  /**
   * Prints string representation to the given `ostream`.
   *
   * @param os
   *        Stream to which to write.
   */
  void to_ostream(std::ostream* os) const;

  // The LOG_*() macros don't see Log_context::get_log*() from base otherwise....
  using flow::log::Log_context::get_logger;
  using flow::log::Log_context::get_log_component;

private:
  // Types.

  /// Short-hand for #m_app_shm_mutex type.
  using Mutex = flow::util::Mutex_non_recursive;

  /// Short-hand for #Mutex lock.
  using Lock_guard = flow::util::Lock_guard<Mutex>;

  // Methods.

  /**
   * Analogous to classic::Session_server::init_app_shm_as_needed().  See that doc header.
   *
   * @param app
   *        See above.
   * @return See above.
   */
  Error_code init_app_shm_as_needed(const Client_app& app);

  /// Invoked on #m_async_periodic_worker, performs a round of the SHM-pool cleanup algorithm and schedules same.
  void cleanup();

  /**
   * Performs `A->sample_hi_wmarks()`, where `A = m_app_shm_by_name[app_name]` is looked-up based on the
   * caller-supplied `app_name`; and schedules for this to occur again in some regular period
   * of time.  Call this upon adding `app_name` to #m_app_shm_by_name; it will then self-perpetuate.
   *
   * Pre-condition: #m_app_shm_mutex must be locked (it will remain locked throughout).
   *
   * ### Rationale / Background ###
   * See Session_impl::session_shm_stats_tickle_and_schedule() doc header.  See how it mentions that `app_shm()`
   * is `Session_server`'s responsibility, since `app_shm()` arena lifetime is orthogonal to individual session?
   * This is our handling that responsibility.
   *
   * ### Corner case / defensive behavior / maintenance notes ###
   * Method does not tickle/stops ticklings if `app_name` is not in #m_app_shm_by_name
   * (or that entry is null; same treatment).  We are not saying that removal of non-null entry is
   * even possible... in fact as of this writing it is not; we are being defensive here.  So: if it were possible
   * and happened, then the stat-tickling and scheduling thereof would stop.  In that case, if there is such a
   * thing as re-adding it to #m_app_shm_by_name, one can invoke the method to start the ticklings again.
   * That said: As currently written, it is not really totally robust: If, in particular, `app_name` were
   * erased and then re-added during the "rest" period of this method's last invocation with `app_name`, then
   * it would just keep going with the ticklings, which would probably be wrong -- as one would re-invoke it
   * upon re-adding `m_app_shm_by_name[app_name]`; so there would now be two in-phase periodic ticklings going
   * instead of the appropriate one tickling.
   *
   * To avoid this silliness: Add a mechanism for canceling the currently-scheduled tickling of `app_name`, whenever
   * `app_name` is erased from `m_app_shm_by_name`.  Would just need to save a handle from `.schedule_from_now()`
   * and do the flow.async cancel op on that.
   *
   * Reminder: This whole premise is currently not a thing; the above is in case it becomes true in the future.
   * Oh, also, be sure in that case to not mess-up the thing where `app_name` is in the map but the "arena"
   * is a null handle.  (As of this writing a failed `init_app_shm_as_needed` leaves null in the map for simplicity,
   * so just make sure all that is properly handled still, if conceptual remove from `m_app_shm_by_name` becomes
   * possible.)
   *
   * @param app_name
   *        App::m_name, a key into #m_app_shm_by_name.  If not present there, then there is no arena to tickle,
   *        and scheduling thereof stops.  (See above discussion of that corner case.  We could take a
   *        `shared_ptr<Arena>`, or at least a `const Client_app&`, instead; but again we're trying to keep this
   *        little mechanism as simple and self-contained/resilient as possible.)
   */
  void app_shm_stats_tickle_and_schedule(const std::string& app_name);

  // Data.

  /// Identical to Session_server::m_srv_app_ref.  Used in init_app_shm_as_needed() name calc.
  const Server_app& m_srv_app_ref;

  /// Identical Session_base::m_srv_namespace.  Used in init_app_shm_as_needed() name calc.
  Shared_name m_srv_namespace;

  /// Protects #m_app_shm_by_name.
  mutable Mutex m_app_shm_mutex;

  /**
   * The per-app-scope SHM arenas by App::m_name.  If it's not in the map, it has not been needed yet.
   * If it is but is null, it has but error caused it to not be set-up successfully.
   */
  boost::unordered_flat_map<std::string, std::shared_ptr<Arena>> m_app_shm_by_name;

  /// Thread used for low-priority periodic work.  See: cleanup(), app_shm_stats_tickle_and_schedule().
  flow::async::Single_thread_task_loop m_async_periodic_worker;
}; // class Session_server

// Free functions: in *_fwd.hpp.

// Template implementations.

/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define TEMPLATE_JEM_SESSION_SRV \
  template<session::schema::MqType MQ_TYPE_OR_NONE, bool TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define CLASS_JEM_SESSION_SRV \
  Session_server<MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES, Mdt_payload>

TEMPLATE_JEM_SESSION_SRV
CLASS_JEM_SESSION_SRV::Session_server(flow::log::Logger* logger_ptr, const Server_app& srv_app_ref_arg,
                                      const Client_app::Master_set& cli_app_master_set_ref,
                                      Error_code* err_code) :
  Impl(logger_ptr, this, srv_app_ref_arg, cli_app_master_set_ref, err_code,
       [this](const Client_app& app) -> Error_code
         { return init_app_shm_as_needed(app); }), // Impl customization point: create *(app_shm()) for the `app`.
  m_srv_app_ref(Impl::m_srv_app_ref),
  // (m_srv_namespace: initialized just below.)
  m_async_periodic_worker(get_logger(),
                          /* (Linux) OS thread name will truncate the this-addr snippet to 15-5=10 chars here;
                           * which should actually just fit.  Nothing else seems particularly useful;
                           * like in non-exotic setups our srv-name is pretty much known. */
                          flow::util::ostream_op_string("SSvJ-", this))
{
  using transport::sync_io::Native_socket_stream;
  using flow::async::reset_this_thread_pinning;

  // Before we continue: handle that Impl ctor may have thrown (then we don't get here) or emitted error via *err_code.
  if (err_code && *err_code)
  {
    return;
  }
  // else Impl ctor executed fine.

  {
    auto empty_session_public
      = Server_session_dtl<Server_session_obj>::ct_base(nullptr, m_srv_app_ref, Native_socket_stream{});
    m_srv_namespace = Server_session_dtl<Server_session_obj>{ empty_session_public }.base().srv_namespace();
  }

  m_async_periodic_worker.start(reset_this_thread_pinning);
  // Don't inherit any strange core-affinity!  ^-- Worker must float free.

  m_async_periodic_worker.post([this]() { cleanup(); });
} // Session_server::Session_server()

TEMPLATE_JEM_SESSION_SRV
Error_code CLASS_JEM_SESSION_SRV::init_app_shm_as_needed(const Client_app& app)
{
  using ipc::shm::arena_lend::jemalloc::Memory_manager;
  using boost::movelib::make_unique;
  using std::make_shared;
  using std::to_string;
  using std::string;

  /* We are in some unspecified thread; actually *a* Session_server_impl thread Ws (a Server_session_impl thread W).
   * Gotta lock at least to protect from concurrent calls to ourselves on behalf of other async_accept()s. */

  Lock_guard app_shm_lock{m_app_shm_mutex};

  auto& app_shm = m_app_shm_by_name[app.m_name]; // Find; insert if needed.
  if (app_shm)
  {
    // Cool; already exists, as app.m_name seen already (and successfully set-up, as seen below) by us.
    return {};
  }
  // else

  // Below should INFO-log already; let's not litter logs explaining why this is being created; context = sufficient.

  /* Arena needs a pool name prefix it will use each time the memory manager decides it wants a new mmap()ped vaddr
   * space (in the SHM context, <-> SHM-pool).  We use our standard semantics for per-session items.  Arena will
   * append whatever pool ID stuff after this prefix.
   *
   * This is the same as Session_mv::init_shm() but simpler due to being a server-side thing (as opposed to possibly
   * client-side) and thus not needing to add any client-PID for cleanup purposes (server-PID is already
   * the srv_namespace baked into every Shared_name).  Also it's per-app, not per-session, hence cli_namespace is
   * not needed. */
  auto shm_pool_name_prefix = build_conventional_shared_name(Shared_name::S_RESOURCE_TYPE_ID_SHM,
                                                             Shared_name::ct(m_srv_app_ref.m_name), m_srv_namespace,
                                                             Shared_name::ct(app.m_name));
  shm_pool_name_prefix /= SHM_SUBTYPE_PREFIX;

  app_shm
    = Arena::create(get_logger(),
                    // See equally-relevant note/@todo in Session_mv::init_shm().
                    make_shared<Memory_manager>(),
                    std::move(shm_pool_name_prefix),
                    util::shared_resource_permissions(m_srv_app_ref.m_permissions_level_for_client_apps));
  if (!app_shm)
  {
    /* app_shm, which is in the map directly, is null.  Just leave null in the map; meh.
     * .erase()ing it from there is just pedantic at best.  (The [] lookup above will do the right thing next time.) */

    FLOW_LOG_WARNING("Session_server [" << * this << "]: Failed to create session-scope Arena; "
                     "details may be found above.  Session will not open.");

    // See @todo on this Code; in short if create() emitted an Error_code, we'd just emit that instead here.
    return error::Code::S_SHM_ARENA_CREATION_FAILED;
  }
  // else: Cool!

  // Its doc header explains things.  Mutex is locked as required.
  app_shm_stats_tickle_and_schedule(app.m_name);

  return {};
  // Lock_guard app_shm_lock{m_app_shm_mutex}: unlocks here.
} // Session_server::init_app_shm_as_needed()

TEMPLATE_JEM_SESSION_SRV
void CLASS_JEM_SESSION_SRV::app_shm_stats_tickle_and_schedule(const std::string& app_name)
{
  using util::Call_timing;
  using boost::chrono::seconds;
  using std::shared_ptr;
  using std::string;

  /* If you read our doc header and possibly consult Session_impl::session_shm_stats_tickle_and_schedule() -- our
   * cousin -- then the below should be quite straightforward.
   *
   * A question might arise: the Session_impl guy is still simple, but it is worried about session-hosings
   * and "thread W" -- whereas we really don't care; why not?  Answer: That's just the nature of these app-scope
   * arenas.  They outlive any particular Session(s), potentially, and are available for use through any of
   * them (well, ones w/r/t the same Client_app::m_name).  Even if some session fails to start-up -- maybe even
   * if it is the first one for this Client_app and hence the one that made m_app_shm_by_name[app_name] appear
   * in the first place -- the *arena* is totally fine and cool to keep existing.  Lastly: there is no such
   * thing as *this Session_server (cf. various `Session`s) being "down": while `*this` exists, it's "up."  So
   * even if we wanted to stop these (harmless) ticklings due to *this no longer operating, it just isn't a
   * thing as of this writing.  Though even if it were... meh.  Tickle away.
   *
   * Secondly let's discuss the appropriate (default at least) period for this tickling.  ...See
   * Session_impl::session_shm_stats_tickle_and_schedule().  We use a similar period here.  Do realize:
   * that one is per-session; this one is per-distinct Client_app that has connected to *this session-server
   * at least once.  Realistically: there might 1 or 2 or maybe 5 of these; like assuming it is 5, it means
   * this application is capable of doing IPC (opening IPC sessions -- contexts basically) with 5 different
   * applications (like, distinct executables... not processes/instances thereof); that's quite ambitious.
   * That is to say... it's not going to be, like, 100 of these timers here. */
  constexpr util::Fine_duration TICKLE_PERIOD = seconds{10}; // @todo Provide a knob including ability to turn off?

  /* We are in thread <whichever one calls init_app_shm_as_needed()> or, after that, m_async_periodic_worker's.
   * Does not matter: arena->sample_hi_wmarks() is safe from any thread by its contract.
   *
   * Mutex is locked by contract. */

  const auto map_it = m_app_shm_by_name.find(app_name);
  /* Subtlety: Due to an intentional quirk of init_app_shm_as_needed(), if it's in the map, the ptr may still be null:
   * init_app_shm_as_needed() failed for app.m_name but does not erase in that case and just leaves null. */
  const auto arena = (map_it == m_app_shm_by_name.end()) ? shared_ptr<Arena>{} : map_it->second;

  if (!arena)
  {
    return; // As promised in contract: no tickle, no further tickles.
  }
  // else:

  // Background for the Call_timing thing: <see comment in Session_impl::session_shm_stats_tickle_and_schedule()>.
  arena->sample_hi_wmarks(Call_timing::S_POSSIBLY_UNSAFE);

  m_async_periodic_worker.schedule_from_now(TICKLE_PERIOD, [this, app_name](auto&&) mutable
  {
    Lock_guard app_shm_lock{m_app_shm_mutex}; // Required by our contract.
    app_shm_stats_tickle_and_schedule(app_name);
  });
} // Session_server::app_shm_stats_tickle_and_schedule()

TEMPLATE_JEM_SESSION_SRV
typename CLASS_JEM_SESSION_SRV::Arena* CLASS_JEM_SESSION_SRV::app_shm(const Client_app& app)
{
  return app_shm_ptr(app).get();
}

TEMPLATE_JEM_SESSION_SRV
std::shared_ptr<typename CLASS_JEM_SESSION_SRV::Arena> CLASS_JEM_SESSION_SRV::app_shm_ptr(const Client_app& app)
{
  using std::shared_ptr;

  // We are in some unspecified thread; we promised thread safety from any concurrency situation.

  Lock_guard app_shm_lock{m_app_shm_mutex};

  /* Subtlety: Due to an intentional quirk of init_app_shm_as_needed(), if it's in the map, the ptr may still be null:
   * init_app_shm_as_needed() failed for app.m_name but does not erase in that case and just leaves null. */
  const auto map_it = m_app_shm_by_name.find(app.m_name);
  return (map_it == m_app_shm_by_name.end()) ? shared_ptr<Arena>{} : map_it->second;

  // Lock_guard app_shm_lock{m_app_shm_mutex}: unlocks here.
}

TEMPLATE_JEM_SESSION_SRV
typename CLASS_JEM_SESSION_SRV::Structured_msg_builder_config
  CLASS_JEM_SESSION_SRV::app_shm_builder_config(const Client_app& app, size_t segment1_sz)
{
  using transport::struc::shm::stat::Outer_serializer_global_stats;
  using transport::struc::shm::stat::Core_serializer_global_stats;

  const auto arena = app_shm(app);
  assert(arena && "By contract do not call this for not-yet-encountered Client_app.");

  return Structured_msg_builder_config{ get_logger(), segment1_sz,
                                        transport::struc::BUILDER_CONFIG_FRAME_PREFIX_SZ_VIA_STRUC_CHANNEL,
                                        arena,
                                        // Default snd-stats targets: per-Arena SHM-msg-{inner,outer} globals.
                                        &Core_serializer_global_stats<Arena>::get()
                                          .stats_mutable_default().m_snd,
                                        &Outer_serializer_global_stats<Arena>::get()
                                          .stats_mutable_default().m_snd };
}

TEMPLATE_JEM_SESSION_SRV
template<typename Task_err>
void CLASS_JEM_SESSION_SRV::async_accept(Server_session_obj* target_session, Task_err&& on_done_func)
{
  // As advertised this overload always means:

  auto ignored_func = [](auto&&...) -> size_t { return 0; };
  auto no_op_func = [](auto&&...) {};

  async_accept(target_session, nullptr, nullptr, nullptr, std::move(ignored_func), std::move(no_op_func),
               std::move(on_done_func));

  /* @todo That's a copy-paste of Session_server::async_accept() counterpart.  Maybe the design can be amended
   * for greater code reuse/maintainability?  This isn't *too* bad but.... */
}

TEMPLATE_JEM_SESSION_SRV
template<typename Task_err,
         typename N_init_channels_by_srv_req_func, typename Mdt_load_func>
void CLASS_JEM_SESSION_SRV::async_accept(Server_session_obj* target_session,
                                         Channels* init_channels_by_srv_req,
                                         Mdt_reader_ptr* mdt_from_cli_or_null,
                                         Channels* init_channels_by_cli_req,
                                         N_init_channels_by_srv_req_func&& n_init_channels_by_srv_req_func,
                                         Mdt_load_func&& mdt_load_func,
                                         Task_err&& on_done_func)
{
  Impl::async_accept(target_session, init_channels_by_srv_req, mdt_from_cli_or_null, init_channels_by_cli_req,
                     std::move(n_init_channels_by_srv_req_func), std::move(mdt_load_func), std::move(on_done_func));
}

TEMPLATE_JEM_SESSION_SRV
size_t CLASS_JEM_SESSION_SRV::mq_msg_size_limit() const
{
  return Impl::mq_msg_size_limit();
}

TEMPLATE_JEM_SESSION_SRV
void CLASS_JEM_SESSION_SRV::mq_msg_size_limit(size_t limit)
{
  Impl::mq_msg_size_limit(limit);
}

TEMPLATE_JEM_SESSION_SRV
void CLASS_JEM_SESSION_SRV::cleanup()
{
  // `classic`?  Pool_arena?!  But this is jemalloc!?!?!  Worry not.  See remove_each_persistent_if() doc header.
  using ipc::shm::classic::Pool_arena;
  using util::String_view;
  using util::process_id_t;
  using boost::chrono::seconds;
  using boost::lexical_cast;

  constexpr util::Fine_duration CLEANUP_PERIOD = seconds{30};

  FLOW_LOG_TRACE("Client session [" << *this << "]: Periodic (or initial) cleanup starting.");

  /* This is just like Client_session_impl::cleanup(), except for some small differences explained therein; but
   * the main explanation is right here.  Read on.
   *
   * As with SHM-classic, cleanup of SHM-pools = 2 tasks; graceful cleanup, when objects are destroyed normally
   * on process exit() or earlier; and cleanup after a process dies or zombifies/later dies and thus the
   * regular destructors don't run.  Graceful cleanup for SHM-jemalloc is accomplished in each Arena's dtor
   * and is, to us, a black box; the point is it takes care of it, so we need not do anything further.
   * (This is in contrast to SHM-classic which centers on classic::Pool_arena which is so minimalistic that it leaves
   * that task to the user of Pool_arena; in our case ipc::session::shm::classic code is that user and takes care of it.
   * Point is, with SHM-jemalloc we need not.)  So that leaves ungraceful cleanup.  In point of fact, SHM-jemalloc
   * explicitly leaves the task of handling that to its user (that's us).  So:
   *
   * In short, SHM-jemalloc at its core is symmetric in the sense that in an A-B session, where (say) A happens
   * to be the Session_server side (SHM-jemalloc is not aware of that, but you and I are here), A maintains
   * SHM-arena(s); and B maintains its own SHM-arena(s) (and the two cross-borrow objects from each other).
   * If A aborts before destructor(s) completely run, then its arena(s) shall leak, and it's up to us to
   * remove_persistent() (really, SHM-unlink) them; and just the same for B.  While it is conceivable to try to
   * impose the ipc::session ethos, where session-server is responsible for cleaning up everything -- including
   * stuff leaked by session-client(s) -- in this (rare) case that's not really natural.  It is difficult to make
   * any assumptions about what state some creator-process of a given SHM-pool may be in: SHM-jemalloc is a (complex)
   * black box.  Instead it is better to treat each side as similarly as possible, mirroring SHM-jemalloc's own
   * design (even though it left this task to us).
   *
   * Exact rationale aside, here's the deal.  If a given creator (owner) process of a SHM-jemalloc-managed pool
   * is not running, then it is correct and safe to remove_persistent() that pool: any existing handles to that
   * pool in other processes that may be alive will continue to be valid; and no more such processes shall arise.
   * Why?  Answer: because they only *can* arise through the Shm_session::lend_arena(Arena*) operation within the
   * creator (owner) of that Arena (which = pool collection = all SHM pools in arena)... but that process is dead.
   * Nor does SHM-jemalloc support an owner re-opening (in the owner capacity) a previously created SHM-pool.
   * Therefore, logically, *if* we can determine that a given pool's owner is dead, *then* we can/should SHM-unlink
   * that pool.  We'll discuss how to do that next.  First, though, is it ever safe/correct to SHM-unlink a pool,
   * while its creator (owner) is *not* dead?  The answer is no, except if that process is zombified (not "really"
   * active).  We have no good way of determining that, as of this writing anyway.  There are some exceptions to
   * this within ipc::session, and we are indeed in ipc::session right now.  Namely:
   *   - If the owner of a pool was a Session_server (or Server_session borne of one), then in our paradigm there
   *     must be at most one active Session_server; so if we are alive, then the owner of the pool must be inactive,
   *     assuming things are working properly.  (SHM-classic cleanup relies on that in its cleanup algorithm.)
   *   - However no such inference can be on Client_session side: Multiple `Client_session`s of one Client_app
   *     may live concurrently.
   * Since we can't really rely on that stuff, we elect to instead go with the simple aforementioned rule:
   *   - A SHM-pool was created by this app's SHM-jemalloc; and its owner (creator) process is now dead
   *     <=> remove_persistent() that SHM-pool.
   *
   * That leaves only the following challenges:
   *   - List all SHM-pools created by this app's SHM-jemalloc.  Solution: Pool_arena::for_each_persistent(), etc.,
   *     which can scan /dev/shm (in Linux anyway) and find the pools with a certain name pattern; our Shared_name
   *     ipc::session semantics (formally implemented in session_shared_name.hpp and described in Shared_name doc
   *     header) name things in such a way as to make this pretty simple.
   *   - Determine PID of creator of given pool.  Solution: Our PID is encoded into the Shared_name due to the
   *     aforementioned ipc::session Shared_name semantics.
   *   - Determne whether process with a certain PID is alive.  Solution: util::process_running().
   *
   * A note on stats: A stat surface for this sweep (scanned/removed/skipped counts and such) has been considered
   * and deliberately omitted.  Rationale: These events are rare by construction (crash aftermath); and the sweep
   * is fully log-observable -- each removal is INFO-logged with the pool name and reasoning, anomalies are
   * WARNING-logged, and the removed-count is logged at the end.  For rare events those logs are strictly richer
   * than counters would be; nor is this a hot path where instrumentation would come along ~free.  Revisit only
   * if a monitoring consumer materializes that needs queryable counts.  (A cross-process SHM-resident stats
   * store has also been considered and rejected: it would itself be a persistent resource requiring
   * crash-cleanup -- the very problem class this algorithm handles -- plus versioning/consistency headaches.)
   * (End of note on stats.)
   *
   * Let's do it. */

  const auto n_removed
    = util::remove_each_persistent_if<Pool_arena>(get_logger(),
                                                  [&](const Shared_name& shm_pool_name)
  {
    Shared_name resource_type;
    Shared_name srv_app_name;
    Shared_name srv_namespace_aka_pid;
    Shared_name the_rest;

    if (!(decompose_conventional_shared_name(shm_pool_name,
                                             // We always identify resource type even though redundant in this case.
                                             &resource_type,
                                             &srv_app_name, // This must be us, not some other app.
                                             &srv_namespace_aka_pid, // This is what must be dead.
                                             nullptr, // cli_app_name: Needs to be there, but we don't care what it is.
                                             nullptr, // cli_namespace_or_sentinel: Ditto.
                                             &the_rest)
          && (resource_type == Shared_name::S_RESOURCE_TYPE_ID_SHM)
          && (srv_app_name == m_srv_app_ref.m_name)
          && String_view{the_rest.str()}.starts_with((SHM_SUBTYPE_PREFIX + Shared_name::S_SEPARATOR).str())))
    {
      /* Not our pool to possibly delete.  Misnamed; or not from a split with our Server_app;
       * or not from this SHM-provider. */
      return false;
    }
    // else

    /* We could actually skip this next check -- then we'd be deleting client-created stuff too.  It doesn't seem
     * like there's any practical downside.  Just... I (ygoldfel) have really convinced myself by that text
     * in the big comment above regarding arena-lending SHM-providers being symmetrical in how each side deals
     * with the resources they owned.  So going out of our way to clean server stuff by server, client stuff by
     * by client.  @todo Reconsider maybe.  It's also not of huge import, this question: If it gets cleaned, cool. */
    if (String_view{the_rest.str()}.find_first_of(Shared_name::S_SEPARATOR,
                                                  SHM_SUBTYPE_PREFIX.size() + 1) != String_view::npos)
    {
      return false; // Not our pool to possibly delete.  From a split with our Server_app but created by client side.
    }
    // else

    process_id_t pid = 0;
    try
    {
      pid = lexical_cast<process_id_t>(srv_namespace_aka_pid.str());
    }
    catch (...) {}
    if (pid == 0)
    {
      FLOW_LOG_WARNING("Session server [" << *this << "]: During periodic cleanup found seemingly relevant "
                       "(based on prefix/form) SHM-pool named [" << shm_pool_name << "]; but server-PID (a/k/a "
                       "server-namespace) term [" << srv_namespace_aka_pid << "] is not a valid-looking PID.  "
                       "Ignoring; but this warrants investigation.");
      return false;
    }
    // else

    if (util::process_running(pid))
    {
      return false;
    }
    // else

    FLOW_LOG_INFO("Session server [" << *this << "]: During periodic cleanup found SHM-pool "
                  "named [" << shm_pool_name << "]; appears to have been created by a process of this application "
                  "that is no longer running, even as a zombie; perhaps it exited without graceful cleanup.  Will "
                  "attempt to remove it.  Competing cleanup(s) may be occurring concurrently; that is fine.");
    return true;
  }); // remove_each_persistent_if()
  if (n_removed == 0)
  {
    FLOW_LOG_TRACE("Session server [" << *this << "]: Cleanup finished: remove none.");
  }
  else
  {
    FLOW_LOG_INFO("Session server [" << *this << "]: Cleanup finished: Removed [" << n_removed << "] pools "
                  "successfully.");
  }

  m_async_periodic_worker.schedule_from_now(CLEANUP_PERIOD, [this](auto&&) { cleanup(); });
} // Session_server::cleanup()

TEMPLATE_JEM_SESSION_SRV
void CLASS_JEM_SESSION_SRV::to_ostream(std::ostream* os) const
{
  Impl::to_ostream(os);
}

TEMPLATE_JEM_SESSION_SRV
std::ostream& operator<<(std::ostream& os, const CLASS_JEM_SESSION_SRV& val)
{
  val.to_ostream(&os);
  return os;
}

} // namespace ipc::session::shm::arena_lend::jemalloc
