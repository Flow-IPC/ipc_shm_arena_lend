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
#include "ipc/shm/arena_lend/owner_shm_pool_listener_for_repository.hpp"
#include "ipc/shm/classic/pool_arena.hpp"
#include <boost/move/make_unique.hpp>

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
 * ### Implementation ###
 * See similar section of session::Session_server.  It explains why we sub-class Session_server_impl and even how
 * how that's used for this SHM-jemalloc scenario.  To reiterate:
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
 * @endinternal
 *
 * @tparam S_MQ_TYPE_OR_NONE
 *         See vanilla #Session_server.
 * @tparam S_TRANSMIT_NATIVE_HANDLES
 *         See vanilla #Session_server.
 * @tparam Mdt_payload
 *         See vanilla #Session_server.
 */
template<session::schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
class Session_server :
  private Session_server_impl // Attn!  Emit `shm::arena_lend::jemalloc::Server_session`s (impl customization point).
            <Session_server<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>,
             Server_session<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>>
{
private:
  // Types.

  /// Short-hand for our base/core impl.
  using Impl = Session_server_impl
                 <Session_server<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>,
                  Server_session<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>>;

public:
  // Types.

  /// Short-hand for the concrete `Server_session`-like type emitted by async_accept().
  using Server_session_obj
    = shm::arena_lend::jemalloc::Server_session<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>;

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

  // Constructors/destructor.

  /**
   * Constructor: identical to session::Session_server ctor.  See its doc header.
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
                          Error_code* err_code = 0);

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
   * @return See above.
   */
  Structured_msg_builder_config app_shm_builder_config(const Client_app& app);

  /**
   * In short, what app_shm_builder_config() is to shm::arena_lend::jemalloc::Session_mv::app_shm_builder_config(),
   * this is to shm::arena_lend::jemalloc::app_shm_lender_session().  Notes in app_shm_builder_config() doc header apply
   * here.
   *
   * @param app
   *        See app_shm().
   * @return See above.
   */
  typename Structured_msg_builder_config::Builder::Session app_shm_lender_session(const Client_app& app);

  /**
   * Returns reader config counterpart to app_shm_builder_config() for a given `Client_app app`.
   *
   * @param app
   *        See app_shm().
   * @return See above.
   */
  Structured_msg_reader_config app_shm_reader_config(const Client_app& app);

  /**
   * Returns the arena-pool listener maintained throughout `*this` Session_server's life.
   * One must register it with any arena `A`, if one wants to do `A.construct<T>()`, where `T` is
   * an allocator-compliant data structure type with shm::stl::Stateless_allocator.  (Session_server does so
   * automatically for any #Arena it maintains out of the box, namely Session_mv::session_shm() and
   * the ones returned by app_shm().)  This may be useful if creating your own custom-purpose #Arena; instead
   * you can create your own `Owner_shm_pool_listener_for_repository`, but it is generally encouraged to not
   * do so unnecessarily.
   *
   * @return See above.
   */
  ipc::shm::arena_lend::Owner_shm_pool_listener_for_repository* shm_arena_pool_listener();

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

  /// Invoked on #m_async_cleanup_worker, performs a round of the SHM-pool cleanup algorithm and schedules same.
  void cleanup();

  // Data.

  /// Identical to Session_server::m_srv_app_ref.  Used in init_app_shm_as_needed() name calc.
  const Server_app& m_srv_app_ref;

  /// Identical Session_base::m_srv_namespace.  Used in init_app_shm_as_needed() name calc.
  const Shared_name m_srv_namespace;

  /// Protects #m_app_shm_by_name.
  mutable Mutex m_app_shm_mutex;

  /**
   * This guy needs to be around, with each arena (in our case, out of the box, `session_shm()`s and `app_shm()`s)
   * registered against it, for shm::stl::Arena_allocator (and thus STL/allocator-compliant data structures, including
   * containers like our own transport::struc::shm::Capnp_message_builder::Segments_in_shm) to work properly with that
   * arena.
   *
   * Details:
   *   - It is recommended there are not tons of these lying around, but in a production application there
   *     should be one session::Session_server around at a time at most.  So keeping this here makes sense.
   *     Note we didn't, say, put one in each Server_session_impl.
   *   - It is *not* a problem if there is more than one per process; so if there are more sessions around
   *     in the process, it won't cause any problem.  It's not a singleton situation.
   *
   * There is also one in each Client_session_impl.
   */
  ipc::shm::arena_lend::Owner_shm_pool_listener_for_repository m_shm_arena_pool_listener;

  /**
   * The per-app-scope SHM arenas by App::m_name.  If it's not in the map, it has not been needed yet.
   * If it is but is null, it has but error caused it to not be set-up successfully.
   */
  boost::unordered_map<std::string, std::shared_ptr<Arena>> m_app_shm_by_name;

  /// Thread used for low-priority periodic low-priority cleanup work.  See cleanup().
  flow::async::Single_thread_task_loop m_async_cleanup_worker;
}; // class Session_server

// Free functions: in *_fwd.hpp.

// Template implementations.

/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define TEMPLATE_JEM_SESSION_SRV \
  template<session::schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define CLASS_JEM_SESSION_SRV \
  Session_server<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>

TEMPLATE_JEM_SESSION_SRV
CLASS_JEM_SESSION_SRV::Session_server(flow::log::Logger* logger_ptr, const Server_app& srv_app_ref_arg,
                                      const Client_app::Master_set& cli_app_master_set_ref,
                                      Error_code* err_code) :
  Impl(logger_ptr, this, srv_app_ref_arg, cli_app_master_set_ref, err_code,
       [this](const Client_app& app) -> Error_code
         { return init_app_shm_as_needed(app); }), // Impl customization point: create *(app_shm()) for the `app`.
  m_srv_app_ref(Impl::m_srv_app_ref),
  m_srv_namespace
    (Server_session_dtl<Server_session_obj>(nullptr, m_srv_app_ref, transport::sync_io::Native_socket_stream())
       .base().srv_namespace()),
  m_shm_arena_pool_listener(get_logger()),
  m_async_cleanup_worker(get_logger(),
                         /* (Linux) OS thread name will truncate the this-addr snippet to 15-5=10 chars here;
                          * which should actually just fit.  Nothing else seems particularly useful;
                          * like in non-exotic setups our srv-name is pretty much known. */
                         flow::util::ostream_op_string("SSvJ-", this))
{
  using flow::async::reset_this_thread_pinning;

  // Before we continue: handle that Impl ctor may have thrown (then we don't get here) or emitted error via *err_code.
  if (err_code && *err_code)
  {
    return;
  }
  // else Impl ctor executed fine.

  m_async_cleanup_worker.start(reset_this_thread_pinning);
  // Don't inherit any strange core-affinity!  ^-- Worker must float free.

  m_async_cleanup_worker.post([this]() { cleanup(); });
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

  Lock_guard app_shm_lock(m_app_shm_mutex);

  auto& app_shm = m_app_shm_by_name[app.m_name]; // Find; insert if needed.
  if (app_shm)
  {
    // Cool; already exists, as app.m_name seen already (and successfully set-up, as seen below) by us.
    return Error_code();
  }
  // else

  // Below should INFO-log already; let's not litter logs explaining why this is being created; context = sufficient.

  /* Arena needs a pool namer it will invoke each time the memory manager decides it wants a new mmap()ped vaddr
   * space (in the SHM context, <-> SHM-pool).  We use our standard semantics for per-session items.  To save some
   * compute cycles pre-compute the prefix and remember it via capture.
   *
   * This is the same as Session_mv::init_shm() but simpler due to being a server-side thing (as opposed to possibly
   * client-side) and thus not needing to add any client-PID for cleanup purposes (server-PID is already
   * the srv_namespace baked into every Shared_name).  Also it's per-app, not per-session, hence cli_namespace is
   * not needed. */
  auto pool_naming_func = [shm_pool_name_prefix
                             = build_conventional_shared_name(Shared_name::S_RESOURCE_TYPE_ID_SHM,
                                                              Shared_name::ct(m_srv_app_ref.m_name), m_srv_namespace,
                                                              Shared_name::ct(app.m_name))
                               / SHM_SUBTYPE_PREFIX / ""] // Might as well remember the last separator too.
                            (auto pool_id) -> string
  {
    // @todo Arena expects string at the moment; might/should change to Shared_name at some point.

    string shm_pool_name = shm_pool_name_prefix.str();

    /* Per contract pool_id is unique entirely globally and is informally recommended to encode here; so we do.
     * We could instead keep our own ++ing pool-ID, since we guarantee non-conflicting names from other
     * apps/servers/etc. using our Shared_name-building conventions; but since they're giving us this
     * thing for free (as of this writing it is globally-unique -- even across apps, etc. -- for
     * SHM-jemalloc's own internal purposes), we might as well use it. */
    return shm_pool_name += to_string(pool_id);
    // @todo to_string() deals with locales which may be slow.  Consider std::to_chars().
  }; // auto pool_naming_func =

  app_shm
    = Arena::create(get_logger(),
                    // See equally-relevant note/@todo in Session_mv::init_shm().
                    make_shared<Memory_manager>(get_logger()),
                    typename Arena::Shm_object_name_generator(std::move(pool_naming_func)),
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
  // else: Cool!  One last thing (which cannot really fail).

  /* Need this for arena->construct<T>(), where T is allocator-compliant thing such as STL containers,
   * to work. */
#ifndef NDEBUG
  const bool ok =
#endif
  app_shm->add_shm_pool_listener(&m_shm_arena_pool_listener);
  assert(ok && "Maintenance bug?  It should only fail if already registered.");

  return Error_code();
  // Lock_guard app_shm_lock(m_app_shm_mutex): unlocks here.
} // Session_server::init_app_shm_as_needed()

TEMPLATE_JEM_SESSION_SRV
typename CLASS_JEM_SESSION_SRV::Arena* CLASS_JEM_SESSION_SRV::app_shm(const Client_app& app)
{
  return app_shm_ptr(app).get();
}

TEMPLATE_JEM_SESSION_SRV
std::shared_ptr<typename CLASS_JEM_SESSION_SRV::Arena> CLASS_JEM_SESSION_SRV::app_shm_ptr(const Client_app& app)
{
  using std::shared_ptr;

  // We are in some unspecified thread; we promised thread safety form any concurrency situation.

  Lock_guard app_shm_lock(m_app_shm_mutex);

  /* Subtlety: Due to an intentional quirk of init_app_shm_as_needed(), if it's in the map, the ptr may still be null:
   * init_app_shm_as_needed() failed for app.m_name but does not erase in that case and just leaves null. */
  const auto map_it = m_app_shm_by_name.find(app.m_name);
  return (map_it == m_app_shm_by_name.end()) ? shared_ptr<Arena>() : map_it->second;

  // Lock_guard app_shm_lock(m_app_shm_mutex): unlocks here.
}

TEMPLATE_JEM_SESSION_SRV
typename CLASS_JEM_SESSION_SRV::Structured_msg_builder_config
  CLASS_JEM_SESSION_SRV::app_shm_builder_config(const Client_app& app)
{
  const auto arena = app_shm(app);
  assert(arena && "By contract do not call this for not-yet-encountered Client_app.");

  return Structured_msg_builder_config(get_logger(), 0, 0, arena);
}

TEMPLATE_JEM_SESSION_SRV
typename CLASS_JEM_SESSION_SRV::Structured_msg_reader_config
  CLASS_JEM_SESSION_SRV::app_shm_reader_config(const Client_app& app)
{
  const auto arena = app_shm(app);
  assert(arena && "By contract do not call this for not-yet-encountered Client_app.");

  return Structured_msg_reader_config(get_logger(), arena);
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
ipc::shm::arena_lend::Owner_shm_pool_listener_for_repository*
  CLASS_JEM_SESSION_SRV::shm_arena_pool_listener()
{
  return &m_shm_arena_pool_listener;
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

  constexpr util::Fine_duration CLEANUP_PERIOD = seconds(30);

  FLOW_LOG_TRACE("Client session [" << *this << "]: Periodic (or initial) cleanup starting.");

  /* This is just like Client_session_impl::cleanup(), except for some small differences explained therein; but
   * the main explanation is right here.  Read on.
   *
   * As with SHM-classic, cleanup of SHM-pools = 2 tasks; graceful cleanup, when objects are destroyed normally
   * on process exit() or earlier; and cleanup after a process dies or zombifies/later dies and thus the
   * regular destructor don't run.  Graceful cleanup for SHM-jemalloc is accomplished in each Arena's dtor
   * and is, to us, a black box; the point is it takes care of it, so we need not do anything further.
   * (This is in contrast to SHM-classic which centers on classic::Pool_arena which is so minimalistic that it leaves
   * that task to the user of Pool_arena; in our case ipc::session::shm::classic code is that user and takes care of it.
   * Point is, with SHM-jemalloc we need not.)  So that leaves ungraceful cleanup.  In point of fact, SHM-jemalloc
   * explicitly leaves the task of handling to that to its user (that's us).  So:
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
          && String_view(the_rest.str()).starts_with((SHM_SUBTYPE_PREFIX + Shared_name::S_SEPARATOR).str())))
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
    if (String_view(the_rest.str()).find_first_of(Shared_name::S_SEPARATOR,
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

  m_async_cleanup_worker.schedule_from_now(CLEANUP_PERIOD, [this](auto&&) { cleanup(); });
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
