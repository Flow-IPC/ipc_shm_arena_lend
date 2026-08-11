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

#include "ipc/session/standalone/shm/arena_lend/jemalloc/jemalloc_fwd.hpp"
#include "ipc/session/standalone/shm/arena_lend/ipc_shm_message.capnp.h"
#include "ipc/session/standalone/shm/arena_lend/borrower_shm_pool_collection_repository.hpp"
#include "ipc/session/standalone/shm/arena_lend/arena_lend_fwd.hpp"
#include "ipc/shm/arena_lend/jemalloc/ipc_arena.hpp"
#include "ipc/shm/arena_lend/jemalloc/stat_info_dump.hpp"
#include "ipc/shm/arena_lend/jemalloc/jemalloc_fwd.hpp"
#include "ipc/shm/arena_lend/owner_shm_pool_listener.hpp"
#include "ipc/shm/arena_lend/borrower_allocator_arena.hpp"
#include "ipc/shm/arena_lend/arena_lend_stats.hpp"
#include "ipc/shm/arena_lend/detail/thread_lcl_obj_db.hpp"
#include "ipc/shm/arena_lend/detail/obj_disposer.hpp"
#include "ipc/shm/arena_lend/detail/owner_shm_pool_repository.hpp"
#include "ipc/shm/stl/stateless_allocator.hpp"
#include "ipc/transport/struc/heap_serializer.hpp"
#include "ipc/transport/struc/channel.hpp"
#include "ipc/transport/struc/struc_fwd.hpp"
#include "ipc/util/util_fwd.hpp"
#include <flow/util/basic_blob.hpp>
#include <flow/util/util.hpp>
#include <flow/error/error.hpp>
#include <boost/utility.hpp>
#include <atomic>
#include <vector>
#include <unordered_set>
#include <optional>
#include <cstring>

namespace ipc::session::shm::arena_lend::jemalloc
{

// Types.

/**
 * In SHM-jemalloc, a context between this process and a single (typically other) opposing process w/r/t
 * lending (sharing) in-SHM objects from our `Ipc_arena`s to them, and their doing the same conversely.
 *
 * @note In the wider Flow-IPC world, centered around ipc::session::Session, a session is -- quite similarly --
 *       a context for process A IPCing with process B (again A is usually not the same as B but can be, such as
 *       in testing/debugging).  We are too, really; just it's not general IPCing but rather sharing of in-SHM items
 *       via the lend-borrow paradigm.
 *
 * @note These notes (in Shm_session doc header) are written from the standpoint of presenting it (and the rest of
 *       SHM-jemalloc) as a standalone system.  In most situations a general Flow-IPC user can think of
 *       SHM-providers, whether SHM-jemalloc or another one (such as SHM-classic in shm::classic), interchangeably.
 *
 * @see ipc::shm namespace doc header for high-level description of the lend-borrow SHM-arena paradigm.
 *
 * @see Ipc_arena: to put things into one's own SHM arena.  Then to share such a thing with another process --
 *      possibly more than one -- establish a Shm_session and use Shm_session::lend_object(); on the other
 *      side Shm_session::borrow_object() (peer Shm_session object).  Ipc_arena doc header explains this setup --
 *      from a SHM-jemalloc-as-standalone-module PoV -- in more detail than the present recap.
 *
 * The aforementioned Ipc_arena doc header hopefully explains most things of interest including where a `*this`
 * fits into things.  If you read/grok that first, then Shm_session should make sense.  We cover anything
 * left-over in that vein next:
 *
 * ### Shm_session lifetime versus Ipc_arena ###
 * Like Ipc_arena, one accesses `Shm_session`s exclusively via `shared_ptr` handles, including using
 * the create() factory instead of any direct ctor.  As noted regarding Ipc_arena, if one does
 * `session->lend_arena(arena)` (with `shared_ptr<Ipc_arena> arena`), then `*arena` shall outlive `*session`.
 * Thus, after you've lent an arena through a session, you must end the conversation with the opposing process
 * before the arena (most notably its in-SHM resources) is allowed to be destroyed.  (As of this writing
 * there is no undo-`lend_arena()` API, but it could be added.  That would modify the preceding statements in
 * a self-explanatory way.)
 *
 * ### Shm_session lifetime versus object-handles returned via borrow_object() ###
 * In short: When `this->borrow_object<T>()` returns a handle -- `shared_ptr<T>` -- to an in-SHM object constructed
 * in an opposing arena -- the resulting `shared_ptr` group silently retains a `shared_ptr<Session>` to
 * `*this`, dropped only when that smart-pointer group reaches ref-count zero.  (As with Ipc_arena: that's not
 * the same as the actual in-SHM object being destroyed; that requires the owner and all other borrowers to
 * also drop their handles.  Here it's about just the returned, process-local `shared_ptr<T>`.)  Hence a
 * Shm_session outlives all objects borrowed through it.
 *
 * ### When/why and how to destroy your Shm_session ###
 * Firstly, how: Step 1, let all the `borrow_object()`ed handles reach ref-count zero.  (Ipc_arena equivalent of
 * this, w/r/t letting Ipc_arena go away, is to let `construct()`ed handles do the same.)  Step 2, nullify your
 * handle(s) to `*this`.
 *
 * Secondly, why/when: This mirrors the identical question about the more general Flow-IPC sessions.  The answer is
 * two-fold (the two triggers being mutually exclusive):
 *   - Local trigger: That is, you don't want to talk to them anymore.  So you destroy your Shm_session to them.
 *   - Opposing trigger: That is, they don't want to talk to you anymore.  This requires your finding out that
 *     fact somehow.  Formally any technique will do.  Informally/in practice:
 *     - If you're using a `*this` as supplied to your by wider Flow-IPC ipc::session mechanism: It is handled
 *       for you.  This isn't in our purview but for the sake of providing context: A `Session` issues the
 *       registed on-error callback; hence you shall destroy that `Session` -- the connection is no more.
 *       That SHM-enabled `Session` will therefore destroy its internally stored Shm_session
 *       (the thing you access via Session::shm_session() if desired).
 *     - If you're using SHM-jemalloc in standalone fashion: Detect error via the `shm_channel_error_handler`
 *       you gave us via Shm_session::create().  Once that fires, destroy the Shm_session.
 *
 * ### The SHM-channel ###
 * To create/start a `*this`, you must provide to create() a freshly established #Shm_channel
 * (`sync_io`-pattern core, basically a socket ready to transmit).
 *
 * Why is it needed?  Answer: Internally an arena-lending SHM-provider, SHM-jemalloc in this case, must sometimes
 * send messages for internal purposes to synchronize certain key state.  (High-level example for context:
 * a lend-arena message with some info about A is internally sent when one does `session->lend_arena(A)`;
 * now the opposing Shm_session will be able to borrow_object() items that are `A->construct()`ed across process
 * boundary.)
 *
 * If you're using a `*this` via an ipc::session general `Session`, then it worries about all that, so you don't
 * need to; you won't even be calling create() in the first place.  If however you are using it standalone
 * (example: you **are** the code in ipc::session general `Session` eating our own dog food!):
 *
 * Establish a #Shm_channel yourself (various techniques are documented elswhere) and provide it to
 * Shm_session::create(); from that point on the resulting `*this` takes over that object.
 *
 * @internal
 *
 * @todo shm::arena_lend::jemalloc::Shm_session could probably become `shm::arena_lend::Shm_session`, handling
 * sessions dealing with arena-lending SHM-arenas backed by any memory-manager-backed #Arena type (which could
 * become a template parameter).  Historically that had likely been a tall order, but looking at `Shm_session`
 * insides now, it is actually (1) fairly contained in its duties, and (2) seems to make no jemalloc-specific
 * mentions other #Arena itself (which, again, would become a template parameter).  (Regarding "fairly contained
 * in its duties": E.g., borrowed-pool and borrowed-object handling is now in `Thread_lcl_obj_db_*` and
 * Borrower_shm_pool_collection_repository singletons -- both already `Shm_arena`-parameterized and -segregated.
 * Owner_shm_pool_repository handles owner-side pools and objects.  Shm_session forwards things to them when
 * needed is all.)
 */
class Shm_session :
  /* `private`, not `public`: end-users have no business with our logging accessors, and (relatedly) there is
   * internal concurrency, so we must not expose a thread-unsafe set_logger()/get_logger().  The logger is fixed
   * at construction; our own (possibly-concurrent) logging reads it safely as it never changes thereafter. */
  private flow::log::Log_context,
  public std::enable_shared_from_this<Shm_session>,
  private boost::noncopyable
{
public:
  // Types.

  /// Short-hand for the SHM-arena type.
  using Arena = ipc::shm::arena_lend::jemalloc::Ipc_arena;

  /**
   * First-class handle to an outer object as returned by borrow_object(); equals `Arena::Handle`
   * a/k/a shm::arena_lend::Obj_handle.
   *
   * @tparam T
   *         Outer object type.
   */
  template<typename T>
  using Handle = Arena::Handle<T>;

  /**
   * Alias for an unstructured channel for our internal use, as passed to create() and taken-over from there
   * by a `*this`.  Again: it must a fresh object of this type.  In particular, no
   * `start_*_ops()` should have been called (which implies no send/receive transmission API calls either).
   *
   * @internal
   * ### Choice of `transport::Channel` type ###
   * We don't need to transmit native handles, so it'll
   * have just a blobs pipe; so as of this writing that leaves 3 possibilities (perusing channel.hpp):
   *   - transport::Posix_mqs_channel_of_blobs (2 POSIX MQs facing each other);
   *   - transport::Bipc_mqs_channel_of_blobs (2 SHM-backed bipc-supplied MQs facing each other -- the SHM
   *     system is not us, so this isn't an infinite loop);
   *   - transport::Socket_stream_channel_of_blobs (1 bidirectional local stream, a/k/a Unix-domain socket).
   *
   * All would work, and tests show perf differences are minute for small messages like ours.  In terms of ease of
   * setup of an existing channel `Socket_stream_channel_of_blobs` is hard to beat (just send over a single
   * native handle); and in terms of low-maintenance of cleanup even more so (just close the handle on each side;
   * done).  So let's go with that.
   */
  using Shm_channel = transport::Socket_stream_channel_of_blobs<true>;

  /**
   * Alias for a light-weight blob used to encode SHM-handles emitted by lend_object() and consumed by borrow_object().
   *
   * @internal
   * They're little; TRACE-logging of deallocs and copies is of low value; otherwise this can be switched
   * to `flow::util::Blob`.  There's also a to-do somewhere to switch these to `array` (fixed-length buffer)
   * which would be nice for some extra perf; however note lend_object() and borrow_object() are forwarded-to
   * via ipc::session layer (optional to use but recommended), so such a change would need to be potentially
   * coordinated with the rest of Flow-IPC including other SHM-providers (starting with SHM-classic in
   * ipc::shm::classic).
   */
  using Blob = flow::util::Blob_sans_log_context;

  /**
   * Convenience alias for a `Stateless_allocator` suitable when *borrowing* STL-compliant containers et al through
   * a SHM-jemalloc Shm_session.  So if you're using owner-side, e.g.,
   * `auto p_there = arena->construct<vector<T, Ipc_arena::Allocator<T>>>()`,
   * then borrower-side to obtain a (reading-capable) handle to same you would:
   * `auto p_here = session->borrow_object<vector<T, Shm_session::Borrower_arena_allocator<T>>>()`.
   *
   * @note If you're using general ipc::session::Session et al to establish a SHM-jemalloc-enabled setup,
   *       as opposed to using SHM-jemalloc in standalone fashion, then it is probably best
   *       to use `Session::Borrower_allocator` which would also work with other SHM-providers like
   *       SHM-classic.
   *
   * @tparam T
   *         Pointed-to type for the allocator.  See standard C++ `Allocator` concept.
   */
  template<typename T>
  using Borrower_arena_allocator
    = ipc::shm::stl::Stateless_allocator
        <T, ipc::shm::arena_lend::Borrower_allocator_arena<Borrower_shm_pool_collection_repository<Arena>>>;

  /// Alias for a stats/info bundle type.
  using Info_dump = ipc::shm::arena_lend::jemalloc::stat::Shm_session_info_dump;

  /// Alias for a stats type.
  using Borrower_pool_stats = ipc::shm::arena_lend::stat::Borrower_pool_stats;

  /// Alias for a stats type.
  using Borrower_pool_lookup_global_stats = ipc::shm::arena_lend::stat::Borrower_pool_lookup_global_stats;

  /**
   * Alias for a (per-arena) stats-list type, as returned by borrower_pool_stats_process_wide().
   *
   * Spoiler alert: It is `vector<Own<Borrower_pool_stats>>`, where `Own` is a single-owner-pointer handle.
   */
  using Borrower_pool_stats_list = ipc::session::shm::arena_lend::Borrower_pool_stats_list;

  /// Alias for the per-pool info type, as returned by borrowed_shm_pool_live_info().
  using Shm_pool_info = ipc::shm::arena_lend::stat::Shm_pool_info;

  /// Constructors/destructor.
  ~Shm_session();

  // Methods.

  /**
   * Creates an instance of this class; the resulting Shm_arena object is immediately ready to receive incoming
   * opposing lend_arena() calls, and you can call lend_arena(), lend_object(), and borrow_object().
   *
   * See class doc header for motivation for using the ref-counting handle paradigm.
   *
   * @warning `*logger` -- unless `logger` is null -- must exist at least past (1) all
   *          `borrow_object()`-returned pointers' groups and (2) of course the returned pointer group.
   *
   * @warning It is important that `shm_channel.remote_peer_process_credentials()` is properly set
   *          (we need the opposing PID for internal purposes).  If the `Native_socket_stream` was set-up
   *          from a socket handle transmitted via IPC (and originally `connect_pair()`ed or equivalent),
   *          then it may be necessary for the caller to perform an extra step.
   *          See transport::Native_socket_stream::remote_peer_process_credentials() doc header for this info.
   *
   * ### About `shm_channel_error_handler` (and `shm_channel`) and error reporting ###
   * Firstly:
   *
   * @see class doc header discussion of error handling -- or more specifically, the whys and hows of
   *      detecting that the IPC connection to the other side has gone down (usually gracefully) on account of
   *      the other side ending it (as opposed to the end user here deciding to end it).
   *      `shm_channel_error_handler` is a part of that picture but, as you'll note, not *necessarily* a
   *      mandatory one to use.
   *
   * `shm_channel` is a low-level (unstructured) channel -- transport::Channel -- of the specific type
   * #Shm_channel, such that a similar peer object is passed to the opposing Shm_session to which we're connecting,
   * typically (though not mandatorily; e.g. for testing, debugging) across a process boundary.
   * It is a `sync_io`-pattern variant (formally with `transport::Channel::S_IS_SYNC_IO_OBJ == true`).
   * (Arguably in plainer English: it's essentially a synchronously-used core, not dissimilar to
   * a Unix domain socket FD (with Flow-IPC's internal protocol and API niceties of course).)  It must be
   * a freshly established channel: no calls to `Blob_{sender|receiver}`-concept APIs.  `*this` shall subsume
   * it via move semantics and own it.  It'll be used internally for important communications such as pool-
   * and arena-lending notifications and acks, but to a `*this` user it is black box stuff.  Hold that thought.
   *
   * With one important exception (which we tackle just below) a `*this` cannot fail.  Things like "Linux `mmap()`
   * failed" or "we cannot open a named SHM object that the opposing `Ipc_arena` says it created a split second ago"
   * are not reasonably recoverable; we log and abort or otherwise catastrophically fail.  Don't worry about these.
   *
   * @note A Shm_session, by the arena-lending design, never allocates in the user's SHM arena(s);
   *       #Arena (`Ipc_arena`) does that.  Shm_session exists entirely to enable lend_object() (prep
   *       an `Arena::construct<T>()`ed `T` to be borrowed by opposing side) and borrow_object() (accept that,
   *       yielding a handle-to-`T` on this side, so that it is accessible + participates in GC together with
   *       the opposing `construct()`ed handle and any other-process-similarly-borrowed handles-to-`T`).
   *       The point: at least #Arena can fail due to failure to allocate (out of RAM according to special SHM
   *       limits configured in OS: not atypical).  Shm_session doesn't do that, so it can't.  So:
   *
   * The exception is simply this: the session (a/k/a conversation with the other arena-lending-and-borrowing
   * thing like us) may have ended, due to the other side deciding to do so (or crash or whatever).  This
   * is called "channel is hosed."  `shm_channel` being hosed has the following effect:
   *
   * `*this` detects it, typically, as soon as it occurs.  It is also technically possible that it first
   * detects it when trying to *send* an internal message.  Regardless: it is detected *at most once*
   * and therefore is then reported to you immediately and *at most once*.  To wit: `*this` shall
   * execute *from thread W*: `shm_channel_error_handler(E)` (`E` being the reason for the channel-hosing).
   * This mechanism is identical to async error reporting via the various async-I/O-pattern IPC primitives
   * in ipc::transport (e.g.: transport::Blob_receiver::async_receive_blob()).  It just used for this one
   * limited thing.
   *   - If you *do* need to use `*this`'s detecting the channel-hosing as *the* method of detecting
   *     other-side-caused ("opposing trigger") session-hosing, then in `shm_channel_error_handler()` --
   *     which runs from thread W -- we recommend you signal one of your user threads to end all use
   *     of `Shm_session`-borrowed objects, nullifying them; and destroy Shm_session.  E.g., if you use
   *     boost.asio or `flow::async`, then it'd be typical to `post()` those steps onto your main thread.
   *   - If you *do not* need to do that -- ipc::session is wrangling create() and `*this` for you, or
   *     you detect session-hosing via the hosing of some other channel or socket -- then in
   *     `shm_channel_error_handler` you could just log; or do nothing.
   *
   * Nevertheless, after this event (after channel's hosing is detected) but before Shm_session destruction
   * (which you would allow, by dropping all borrowed handles and the `create()`-returned `Shm_session`-handle),
   * APIs shall act as follows.
   *   - lend_arena() will return `false`.  (Object-lending wouldn't work properly anyway so why lie to you?)
   *   - lend_object() will return an empty blob (so there's nothing for the opposing to borrow_object()).
   *     (We could return a functional/`borrow_object()`able blob here; but channel-hosed means there's a
   *     high chance the opposing (borrowing) process will have *no* information about the latest SHM-pools
   *     from our `lend_arena()`d `Ipc_arena`s, so passing a thing into borrow_object() is an invitation for
   *     crashing and other chaos.  Plus in any case: if the session is down, then it's likely you wouldn't be
   *     able to transmit the blob anyway let alone then `borrow_object()` it.
   *   - borrow_object() (on `*this`) however will work normally.  If you've managed to receive the blob
   *     to pass to that method, then both general IPC and opposing lend_arena() and lend_object() worked fine
   *     for in-SHM objects including that one; there's no reason to create more failure modes by having this
   *     artifically fail.
   *
   * Because Shm_session is not itself a communication channel, we're belaboring these points.  That said this is
   * no different from other async-I/O-pattern objects in Flow-IPC.  Simply:
   *   - Be ready for lend_arena() and lend_object() to fail, indicating "this `Shm_session` is already hosed,
   *     and `shm_channel_error_handler()` shall fire imminently or already has."  Typically one would stop
   *     normal processing but otherwise no-op.  And orthogonally:
   *   - Handle `shm_channel_error_handler(E)` -- or your other method of session-hosing detection -- swiftly.
   *     As noted before: nullify all `this->borrow_object()`-returned handles; and destroy `*this` by dropping
   *     any handles from create().
   *   - You could also preempt the preceding bullet point *if* you happen to detect failed lend_arena() or
   *     lend_object() first.  If you don't care about logging/reporting the `Error_code` associated with
   *     the channel-hosing, then you could end the Shm_session immediately.  However you should still be ready
   *     to handle `shm_channel_error_handler(E)`, if it happens first.  After all lend_object() and
   *     definitely lend_arena() are not done continuously, and it is proper to react to session-hosing ASAP.
   *
   * ### Details ##
   * The following information is for context only, if you want to understand more precisely how/why we detect
   * and report channel-hosing.  It should not affect how you handle all this, and it is somewhat white-boxy info.
   * Nevertheless: Here are the paths as of this writing to triggering `shm_channel_error_handler()` (and thus
   * setting the internal flag that'll cause lend_arena() and lend_object() to fail subsequently as well).
   *   - lend_arena() itself tries to send internal `LendArena` message over `shm_channel`; the send fails
   *     synchronously.  Thread: end user thread.
   *     - Result: post `shm_channel_error_handler(E)` onto thread W to execute ASAP.  lend_arena() returns false.
   *   - `Arena::allocate()`, for an #Arena that has been successfuly `lend_arena()`ed earlier, is told
   *     synchronously (by memory manager jemalloc) that a new SHM-pool is required, so it created it and informed
   *     every `Shm_session` (including `*this`) through which it has been let that this occurred, and that it
   *     must inform the opposing process(es), receive ack(s), and only then let `Arena::allocate()` return.
   *     However the send of the info fails synchronously.  Thread: end user thread.
   *     - Result: post `shm_channel_error_handler(E)` onto thread W to execute ASAP.  Arena `allocate()` proceeds
   *       normally after that.  (Nota bene: Shm_session failure does not cause Ipc_arena allocation failure.)
   *   - The internal channel itself detects hosing while awaiting (like always) in-messages for internal use.
   *     Thread: that channel's own background thread.
   *     - Result: post `shm_channel_error_handler(E)` onto thread W to execute ASAP.
   *   - In thread W, we receive a message (`LendArena`, `LendPool`) and reply with ack response.
   *     However the send of the ack fails synchronously.  Thread: W.
   *     - Result: execute `shm_channel_error_handler(E)` (already in thread W).
   *
   * @todo It'd be nice for Shm_session::create() to become a template with parameterized functor type
   * instead of `flow::async::Task_asio_err` for `shm_channel_error_handler`.  There is zero functional
   * difference, as internally it is stored as a `Task_asio_err` (which a `Function<>`) anyway; but
   * for consistency and maintainability it'd be nice.
   *
   * @param logger
   *        Used for logging purposes.
   * @param shm_channel
   *        The channel used for transmitting internal messages.  See above for requirements (which are simple).
   *        Subsumed/owned by the returned Shm_session.
   * @param session_token_non_nil
   *        A session token to use internally for `shm_channel` communications; must be non-nil (else undefined
   *        behavior/assert may trip) and equal to what is given to the opposing `Shm_session`.
   *        If you have an `ipc::session::Session` (or equivalent) available: use its
   *        ipc::session::Session::session_token().  Otherwise use a value generated some other way.
   *        To forego the safety value offered here, you can manually generate a random one
   *        (`boost::uuids::random_generator()()`) offline and hard-code the resulting value on both sides.
   * @param shm_channel_error_handler
   *        See above.
   * @return An instance of this class.
   */
  static std::shared_ptr<Shm_session> create(flow::log::Logger* logger, Shm_channel&& shm_channel,
                                             const transport::struc::Session_token& session_token_non_nil,
                                             flow::async::Task_asio_err&& shm_channel_error_handler);

  /**
   * Registers an arena for lending to the borrower process (usually a separate process) on the other end of the
   * session; it is required to do so before `this->lend_object<T>(p)` for any `p` that was
   * `arena->construct<T>()`ed.
   *
   * @param arena
   *        The arena to be lent.
   * @return Whether registration was successful.  Failure indicates that either this lend_arena() itself failed
   *         on account of the hosing of `shm_channel`, or `shm_channel`'s hosing had been
   *         detected earlier.  In any case, if so, `shm_channel_error_handler(E)` will fire or has fired
   *         in a background thread, and `*this` is hosed.  See create() doc header.
   */
  bool lend_arena(const std::shared_ptr<Arena>& arena);

  /**
   * Registers an in-SHM object for lending to the borrower on the other end of the session; and returns a small blob
   * to pass to borrow_object() in the opposing Shm_session to obtain access to the same in-SHM object.
   * borrow_object() returns a handle much like `object` -- and participating in the same GC (garbage collection)
   * of `*object` (thus making it effectively a cross-process `shared_ptr` group).
   *
   * An object that is `lend_object()`ed but not yet (or ever) subsequently `borrow_object()`ed against the
   * opposing Shm_session is extant: it will not be destroyed until either (1) `*this` is destroyed or (2) the
   * `borrow_object()` et al does occur.  So if you fail to ever `borrow_object()` the returned blob, it is a leak
   * until the lending `Shm_session` is destroyed, at the earliest.  In more detail:
   *   - Think of it as a cross-process ref-count (*use-count*) of processes (owner, lenders).  Ipc_arena::construct()
   *     sets it to 1.  Local lend_object() increments it by 1.  `construct()`ed `shared_ptr` group reaches local
   *     ref-count zero (owner-side disposer runs) => decrement by 1.  Opposing-process `borrow_object()`ed
   *     `shared_ptr` group reaches local ref-count zero (borrower-side disposer runs) => decrement by 1.  Note
   *     that opposing-process borrow_object() itself does not affect this use-count; this lend_object() already did
   *     so.
   *   - Until one does opposing-process borrow_object(), object is therefore (under continuing normal operation,
   *     nothing shutting down) safe from in-SHM garbage-collection (freeing), as use-count remains at least 1
   *     due to this lend_object().  By the same token, though, if one *never* calls it, then it remains "safe" from
   *     freeing for the same reason...
   *   - ...except that the destruction of the Ipc_arena itself will in fact free all owned memory including this
   *     object, whether or not its use-count had reached zero.  However, Ipc_arena itself cannot be
   *     destroyed (even if you drop your `shared_ptr<Ipc_arena>` handle from Ipc_arena::create() and all
   *     `construct()`ed handles, including possibly `object` itself), until all
   *     `Shm_session`s through which it was `lend_arena()`d have been destroyed.  That includes `*this`; so `*this`
   *     Shm_session going away is a necessary (not sufficient though) condition for `object` getting GCed --
   *     in that scenario.
   *
   * @note The serialization returned includes information on how to deserialize the object and not the contents
   *       of the object itself.  It is small.
   *
   * @note It is up to the caller/user to transmit the returned blob via any IPC technique whatsoever.  (Flow-IPC
   *       does provide a bunch of them; and ipc::transport::structured::Channel does it for you in a couple of
   *       senses that make sense for its mission.)  `shm_channel` -- passed to `*this` via create() for "internal
   *       use" -- is *not* to be used by anyone for this purpose.
   *
   * @warning Be ready for this method returning empty blob.  See below regarding return value.
   *
   * @tparam T
   *         The object type being lent.  As of this writing it actually does not need to match the `T` used
   *         for Ipc_arena::construct() to obtain `object`; e.g., `T = void` might be useful in some cases.
   *         (borrow_object() is a different story.  See its doc header.)
   * @param object
   *        The handle to object to be lent; it *must* have come from Ipc_arena::construct().
   *        If not the method throws a `flow::error::Runtime_error` (itself an `std::runtime_error`).
   * @return Non-empty blob on success; empty on failure.  Failure indicates `shm_channel`'s hosing had been
   *         detected earlier.  If so `shm_channel_error_handler(E)` has fired
   *         in a background thread, and `*this` is hosed.  See create() doc header.
   */
  template<typename T>
  Blob lend_object(const Handle<T>& object);

  /**
   * Yields a handle to the cross-process-GCed in-SHM object that was earlier `S->lend_object()`ed based on the
   * small blob returned by the latter and passed to the present method.  See lend_object() (and
   * possibly Ipc_arena::construct() before that) first.
   *
   * @tparam T
   *         The object type that is being borrowed.  While for plain old data this is straightforward -- typically
   *         the same `Tc` as given to `arena->construct<Tc>()` -- things change if that `Tc` involves at
   *         any level STL-compliant structure(s).  A full explanation is impractical here; please see other
   *         docs including the guided manual.  Here we'll just note the main points: 1: Wherever `Tc` used
   *         a `Stateless_allocator<R, Ipc_arena>`, `T` should use Shm_session::Borrower_arena_allocator<R>`.
   *         2: There is no allocation possible as a borrower.  As of this writing the SHM mapped borrower-side
   *         is simply read-only; but even if we hadn't made it so via kernel, allocation is still only doable
   *         by the owner, per arena-lending-SHM-provider design.  In any case you shall never perform
   *         anything -- like `vector::resize()` -- on the borrower side here, as the aforementioned
   *         #Borrower_arena_allocator simply cannot allocate; it only provides the (quite essential)
   *         `Pointer` type.
   * @param serialized_object
   *        Thing returned by `opposing_session->lend_object()`.
   * @return If successful, handle to cross-process-garbage-collectable object in SHM.  If unsuccessful: null.
   *         The latter can only occur if `serialized_object` is of the wrong length and therefore clearly
   *         not coming from `lend_object()`.  However, any other problem such as wrong data of the proper size =>
   *         undefined behavior.  See create() doc header.
   */
  template<typename T>
  Handle<T> borrow_object(const Blob& serialized_object) const;

  /**
   * Fills-out the stats/info contents of the given stat::Shm_session_info_dump: a printable bundling of stats/info
   * relevant to (but not all necessarily owned by) `*this` Ipc_arena at this point in time.  To summarize the
   * resulting `*target_info_dump`... (see Ipc_arena::info_dump() doc header -- same deal).
   *
   * ### Rationale ###
   * (See Ipc_arena::info_dump() doc header -- same deal.)
   *
   * @param target_info_dump
   *        The non-`->m_fmt` parts shall be assigned.
   * @param call_timing
   *        This is ignored and can be left at its default.  It is present for generic-programming synergy
   *        versus jemalloc::Ipc_arena versus SHM-classic's classic::Pool_arena.  The latter, in the land of
   *        arena-sharing SHM-providers (cf. SHM-jemalloc: arena-lending SHM-provider) is *both* the arena
   *        type *and* the SHM-session type.  So all 3 objects have the same info_dump() signature (modulo
   *        `const`ness potentially).
   */
  void info_dump(Info_dump* target_info_dump,
                 util::Call_timing call_timing = util::Call_timing::S_ALWAYS_SAFE);

  /**
   * Returns the process-wide borrower-side stat-set Borrower_pool_lookup_global_stats,
   * aggregated across all `Shm_session`s in this process.  See that type's doc header for the meaning of its
   * stat-members; and see borrower_pool_stats_process_wide() for the notes on informational use, the
   * live-`atomic<>` nature of the returned reference, consumption, thread safety, and resetting -- all of which
   * apply equally here.
   *
   * @return See above.
   */
  static const Borrower_pool_lookup_global_stats& borrower_pool_lookup_global_stats();

  /**
   * Returns the process-wide borrower-side stat-set Borrower_pool_stats -- the
   * running totals across all `Shm_session`s in this process -- and (optionally) output a copy of similar information
   * broken down by arena, each identified and in ascending order by stat::Uniq_arena_id
   * Borrower_pool_stats::m_uniq_arena_id.  See stat::Borrower_pool_stats doc header for background on interpreting
   * the data; some related notes follow.
   *
   * The returned (totals) reference is to live `atomic<>`s which can change at any moment concurrently;
   * hence even values grabbed in immediate succession can be slightly mutually incoherent.  Consume via
   * `flow::util::stat::load()` / `stats_assign()` / `print()` (et al); see `flow::util::stat` doc header.
   *
   * @note The returned-by-ref Borrower_pool_stats has its Borrower_pool_stats::m_uniq_arena_id all-zeroes, as it
   *       is aggregated info, not per-arena.
   * @note Each element in out-arg `*per_arena_stats`, by contrast, is a snapshot copy as of this call; it will
   *       not change on return.
   * @note The returned totals could almost be computed by summing the per-arena `Borrower_pool_stats` (via
   *       `flow::util::stat::stats_sum()`) -- except the `HI_WMARK`s, which are accurate only on the returned
   *       totals (continuously updated there); that historical info is lost when split by arena across time.
   * @note In each stat-set in `*per_arena_stats` Borrower_pool_stats::m_n_borrowed_arenas cannot exceed 1.
   *
   * ### Thread safety ###
   * Safe to call concurrently with anything on any `Shm_session` (or none).
   *
   * @param per_arena_stats
   *        If null, ignored; else cleared, then loaded with per-arena stat copies (the `unique_ptr` wrapper is
   *        for your convenience -- cheap moves; and the stat-sets are not natively copyable due to their `atomic`
   *        members).  Sorted ascending by Uniq_arena_id::m_id1 (owner-PID), then `m_id2` (per-process ordinal).
   * @return See above (the across-all-`Shm_session`s totals).
   */
  static const Borrower_pool_stats&
    borrower_pool_stats_process_wide(Borrower_pool_stats_list* per_arena_stats = nullptr);

  /**
   * Resets borrower_pool_lookup_global_stats() and borrower_pool_stats_process_wide().  The formal meaning of a reset
   * is discussed in `flow::util::stat` doc header.
   *
   * @note These are process-global stat-sets; hence this resets them across the entire process.
   *       (Cf. the non-`static` borrower_pool_stats_reset().)
   *       To be clear, though, this does not touch anything but the data for the aforementioned
   *       two `static ..._globals_stats()`.  Anything accessed per-`Shm_session` (so non-`static`
   *       borrower_pool_stats_reset()) is reset only via correspoding non-`static` `borrower_pool_stats_reset()`,
   *       for each Shm_session of interest.
   */
  static void global_stats_reset();

  /**
   * Returns the borrower-side stat-set Borrower_pool_stats limited to `*this`
   * `Shm_session` -- i.e., the same kind of data as borrower_pool_stats_process_wide() but covering only the
   * borrowing activity conducted through `*this`.  See stat::Borrower_pool_stats doc header for the meaning of
   * its stat-members (including how a per-`Shm_session` view subdivides versus the process-wide one).
   *
   * All notes for return-value of borrower_pool_stats_process_wide() apply to the returned reference
   * (live `atomic<>`s; consumption; also thread safety in general).
   *
   * @return See above.
   */
  const Borrower_pool_stats& borrower_pool_stats() const;

  /**
   * Resets borrower_pool_stats() -- the per-`*this`-`Shm_session` view only; does not affect the process-wide
   * stats (borrower_pool_lookup_global_stats() / borrower_pool_stats_process_wide()).  The formal meaning of a reset is
   * discussed in `flow::util::stat` doc header.
   */
  void borrower_pool_stats_reset();

  /**
   * For informational/stats-adjacent purposes, returns information on currently open (and memory-mapped)
   * SHM-pools, as borrowed across all `Shm_session`s in this process / from all borrowed arenas, sorted by
   * pool ID -- that is, in chronological order of creation, across owner processes.  Each pool record includes
   * the ID of the containing arena as uniquely identified machine-wide (between boots); thus one can tell the
   * order in which the borrowed pools were created machine-wide -- the arena IDs are interleaved.
   *
   * Informational purposes only (logging/monitoring/etc.).  The borrowed pool-set can change at any moment
   * concurrently.
   *
   * ### Thread safety ###
   * Safe to call concurrently with anything on any `Shm_session` (or none).
   *
   * @return See above.
   */
  static std::vector<Shm_pool_info> borrowed_shm_pool_live_info();

  // Would be private, but as of this writing some items are used by white-boxy unit tests.
protected:
  // Types.

  /// Alias for a SHM pool.
  using Shm_pool = ipc::shm::arena_lend::Shm_pool;
  /// Short-hand for pool ID type.
  using pool_id_t = Shm_pool::pool_id_t;
  /// Short-hand for pool offset type.
  using pool_offset_t = Shm_pool::size_t;

  /**
   * Alias for a structured channel for our internal use.
   *
   * ### Rationale for alias target ###
   * The basic choices are transport::struc::Channel_via_heap and `transport::struc::shm::*::Channel`.
   * The latter is pretty crazy.  That'd be, as of this writing, either
   * transport::struc::shm::arena_lend::jemalloc::Channel
   * (infinite compile-time recursion!) or transport::struc::shm::classic::Channel (which is somewhat more
   * conceivable but a huge pain in the butt for our chief user, namely ipc::session::shm::Session_mv and company --
   * they'd have to set up SHM-classic just to have a chance to set up SHM-jemalloc).  Anyway clearly we must use
   * the non-zero-copy transport::struc::Channel_via_heap.  Our messages are small, so it is a good use of it.
   */
  using Shm_struc_channel = transport::struc::Channel_via_heap<Shm_channel, schema::IpcShmMessage>;

  /**
   * A serialized object handle: a compact representation of an in-SHM object's location and lending metadata,
   * transmitted as a blob via IPC.  Produced by lend_object(), consumed by borrow_object(), usually across
   * process boundary.
   *
   * Why borrow_object() needs these particular data ultimately:
   *   - #m_pool_id, #m_pool_offset: Needed simply to obtain the vaddr of the object in this (borrowing) process;
   *     the vaddr in the opposing (owner) process is an unrelated number.
   *     Borrower_shm_pool_collection_repository::to_address() (a `static`) makes this translation.
   *     (This is not the only place such a lookup is performed; borrower-side Shm_pool_offset_ptr does the same.
   *     This comes up typically in SHM-stored STL-compliant containers which internally store these pointers
   *     due to the allocator used on owner side.  However, borrower-side, that is all subsequent to this
   *     initial outer-object setup in borrow_object().  Then the STL magic, for applicable `T`s, can begin.)
   *   - #m_lend_tracker_pool_id, #m_use_ct_idx: In the fast path the disposer attached to `borrow_object()`-returned
   *     `shared_ptr` shall thread-locally (1) look up the aux SHM-pool `Lend_tracker_pool` in a map and (2) decrement
   *     the atomic use-count for this particular object, at that index.
   *     - #m_collection_id: In doing so it may need to open that pool by name (slow-path) in which case
   *       this collection ID is needed: the pool name base is saved on a per-arena (collection) basis,
   *       when we receive the opposing `Shm_session`'s message during its lend_arena() call.
   *       - The owner ID (`owner_id_t` type) is also necessary here; but that identifies the entire owner
   *         (opposing) process; that is Shm_session::m_remote_process_id.
   */
  struct Shm_object_handle
  {
    // Data.

    /// The collection ID of the arena in which this object was created.  Reminder: it is unique only given a process.
    collection_id_t m_collection_id;
    /// Pool identifier.  Reminder: this is globally unique until reboot.
    pool_id_t m_pool_id;
    /// The offset within the pool where this object resides.
    pool_offset_t m_pool_offset;
    /// Pool ID of the Lend_tracker_pool holding the use-count slot for this object.  Same namespace as #m_pool_id.
    pool_id_t m_lend_tracker_pool_id;
    /// Index of the use-count slot within the above Lend_tracker_pool.
    ipc::shm::arena_lend::detail::use_ct_idx_t m_use_ct_idx;
  }; // struct Shm_object_handle

  /**
   * The `Owner_shm_pool_listener` impl object that we register within a particular `Ipc_arena`
   * (Ipc_arena::add_shm_pool_listener()) when it is lent via lend_arena().  As a result we receive these
   * basic events:
   *   - a SHM-pool was added (initially in lend_arena(); or during allocation on-demand from the memory manager
   *     jemalloc) [so we need to inform the opposing -- borrower -- side, so it is able to access the stuff
   *     in SHM in that pool];
   *   - a SHM-pool was removed (likely during deallocation as requested by ditto).
   *
   * We unregister it from its `Ipc_arena` in Shm_session destructor.  If there were an un-`lend_arena()`
   * (not as of this writing but could happen), then we'd do it there too.  (Destructor = un-lending all
   * arenas being lent.)
   */
  class Shm_pool_listener_impl final :
    public ipc::shm::arena_lend::Owner_shm_pool_listener
  {
  public:
    // Constructors/destructor.

    /**
     * Constructor.
     *
     * @param owner
     *        Containing Shm_session.
     * @param collection_id
     *        The arena (collection) ID that the notifications are emitting from; the owner is our process
     *        (owned ID = our PID).
     */
    Shm_pool_listener_impl(Shm_session& owner, collection_id_t collection_id);

    // Methods.

    /**
     * Implements super-class API (notification called upon initial registration in lend_arena() in a synchronous
     * manner once).  Essentially equivalent to notify_created_shm_pool() for each element in `shm_pool`.
     * @param shm_pools
     *        The current set of active SHM pools; may be empty.
     */
    void notify_initial_shm_pools(const std::set<std::shared_ptr<Shm_pool>>& shm_pools) override;

    /**
     * Implements super-class API (see inner class doc header for recap).
     * @param shm_pool
     *        See above.
     */
    void notify_created_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool) override;

    /**
     * Implements super-class API.
     * @param shm_pool
     *        See above.
     */
    void notify_removed_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool) override;

  private:
    // Data.

    /// See ctor.
    Shm_session& m_owner;
    /// See ctor.
    const collection_id_t m_collection_id;
  }; // class Shm_pool_listener_impl

  // Constructors.

  /**
   * Constructor (private due to `shared_ptr` factory pattern).
   *
   * @param logger
   *        See create().
   * @param shm_channel
   *        See create().
   * @param session_token_non_nil
   *        See create().
   * @param shm_channel_error_handler
   *        See create().
   */
  Shm_session(flow::log::Logger* logger, Shm_channel&& shm_channel,
              const transport::struc::Session_token& session_token_non_nil,
              flow::async::Task_asio_err&& shm_channel_error_handler);

  // Methods.

  /**
   * Returns the remote process ID.
   * @return See above.
   */
  util::process_id_t get_remote_process_id() const;

  /**
   * Equivalent to `lend_shm_pool(collection_id, P)` for each `P` in `shm_pools`.  As of this writing
   * occurs within lend_arena() via Shm_pool_listener_impl.
   *
   * @param collection_id
   *        The arena (collection) ID of our owner-side Ipc_arena owning these pools.
   * @param shm_pools
   *        See above.
   */
  void lend_shm_pools(collection_id_t collection_id, const std::set<std::shared_ptr<Shm_pool>>& shm_pools);

  /**
   * Registers a SHM pool for lending and sends messages to the borrower about it;
   * returns on acknowledgment (or failure to send) only.  As of this writing triggered by either
   * lend_shm_pools() or, arguably more significantly, Shm_pool_listener_impl::notify_created_shm_pool().
   *
   * On error: set_disconnected(); return `false`.
   *
   * Regarding the aforementioned ack:
   *   - It shall arrive immediately (think of it as a function call across potential process boundary).
   *   - It is not safe for our caller, presumably Ipc_arena::allocate() (by way of jemalloc and then more
   *     owner-y arena-y Flow-IPC stuff), to proceed until it occurs.  In fact the same procedure must repeat
   *     for each Shm_session (like `*this`) through which that Ipc_arena has been `lend_arena()`d.  Otherwise
   *     those borrower process(es) may not be able to properly access in-SHM things in these pools (there would
   *     be a race between user sending-over `lend_borrow()`-returned blobs and our sending `LendPool` internal
   *     messages).
   *
   * @param collection_id
   *        The arena (collection) ID of our owner-side Ipc_arena owning these pools.
   * @param shm_pool
   *        The SHM pool.
   * @return Whether the operation was successful.  It would fail if only if we cannot communicate the info
   *         to borrower.
   */
  bool lend_shm_pool(collection_id_t collection_id, const std::shared_ptr<Shm_pool>& shm_pool);

  /**
   * As Shm_pool_listener_impl::notify_created_shm_pool() (by Ipc_arena) => lend_shm_pool(),
   * Shm_pool_listener_impl::notify_removed_shm_pool() => this method.
   *
   * Currently a no-op (logs only); borrower-side cleanup is deferred to ~Shm_session().
   * See `@todo` in the implementation about that.
   *
   * @param collection_id
   *        See lend_shm_pool().
   * @param shm_pool
   *        See lend_shm_pool().
   */
  void remove_lender_shm_pool(collection_id_t collection_id, const std::shared_ptr<Shm_pool>& shm_pool);

  /**
   * Helper that sends message over internal-use #m_shm_channel and returns on ack (or failure to send).
   * Proceeds regardless of #m_connected (to gate based on it, do so before calling).
   *
   * On error: set_disconnected(); return `false`.
   *
   * @param message
   *        The message to send.
   * @param operation
   *        The use case that the message is related to (for logging).
   * @return `true` iff sent OK and got ack.  See ipc_shm_message.capnp for the (straightforward) schema.
   */
  bool send_sync_request(Shm_struc_channel::Msg_out& message, util::String_view operation);

  /**
   * Helper that sends ack to having received a thing that the opposing side sent via its send_sync_request().
   * Proceeds regardless of #m_connected (to gate based on it, do so before calling).
   *
   * On error: set_disconnected().
   *
   * @param original_message
   *        Message we are acking.
   * @param operation
   *        See send_sync_request().
   */
  void send_response(const Shm_struc_channel::Msg_in* original_message, util::String_view operation);

  /**
   * Borrows an arena shared by the lender: in thread W a/k/a #m_serial_task_loop handle `LendArena` message.
   *
   * @param collection_id
   *        The identifier of the arena.
   * @param pool_name_base
   *        The pool-name prefix for this arena's SHM pools.
   */
  void receive_arena(collection_id_t collection_id, Shared_name&& pool_name_base);

  /**
   * Helper that permanently sets #m_connected `true` => `false`, so that lend_arena(), lend_object(), and
   * lend_shm_pool() short-circuit to no-op/failure subsequently.  #m_connected shall be set to `false` exclusively via
   * this method, as it's the way we trigger the callback in the promised manner.
   *
   * @note This shall be called at most once.  That is natural, because #m_shm_channel by struc::Channel contract
   *       can report channel-hosing at most once.  If that contract is broken (or we have a Shm_session bug in
   *       relaying channel-hosing to set_disconnected()) then assert may trip.
   *
   * It is thread-safe in that we may call it from any end user thread
   * (lend_arena() from user, lend_shm_pool() from `Arena::allocate()`); or from thread W (#m_serial_task_loop)
   * when handling incoming internal messages for opposing `lend_arena()` and so on; or from #m_shm_channel
   * background thread (the channel-hosed handler).
   *
   * ### Discussion ###
   * Blocking the future `lend_*()` is a best-effort thing.  In create() doc header we explain that the on-error handler
   * `shm_channel_error_handler()` is how we communicate the first and only session-hosing officially; and certain
   * public API calls will intentionally stop working for the cited reasons, those reasons being *not* communicating
   * the hosing but avoiding entropy and undefined behavior (implication being, what if the user continues to try
   * using stuff they shouldn't pass that callback executing?).  Subtlety: lend_shm_pool() is not a public API
   * for the end user, though it is invoked from the #Arena allocation path (sometimes).  It involves sending
   * things via #m_shm_channel; and it stands to reason that once we know the channel is hosed, there's no point
   * trying it; it is probably harmless but entropy-increasing.
   *
   * @param err_code
   *        Cause of channel-hosing.
   */
  void set_disconnected(const Error_code& err_code);

private:
  // Methods.

  /**
   * Borrows a SHM pool from the lender: in thread W a/k/a #m_serial_task_loop handle `LendPool` message.
   *
   * @param collection_id
   *        The identifier of the collection in which the pool resides.
   * @param shm_pool_id
   *        The identifier of the SHM pool.
   * @param pool_size
   *        The size of the SHM pool.
   */
  void receive_shm_pool(collection_id_t collection_id, pool_id_t shm_pool_id, pool_offset_t pool_size);

  // Data.

  /// The other end's process ID, cached from #m_shm_channel; used for registering borrowed items.
  const util::process_id_t m_remote_process_id;

  /**
   * Whether the session is connected; `true` to start; when set to `false` #m_shm_channel_error_handler is invoked
   * from thread W (#m_serial_task_loop); and this state is permanent.
   *
   * Note that the value may change within a method concurrently.  Use set_disconnected(), exclusively, to
   * modify this value; as it will properly trigger #m_shm_channel_error_handler as needed and never again.
   *
   * Also set_disconnected() will only set `m_connected = false` in thread W; that's a choice to reduce entropy:
   * `m_serial_task_loop.stop()` (at shutdown) can thus cause `m_connected` to be immutable subsequently.
   *
   * ### Perf discussion ###
   * The most frequent, by far, reason to access this is lend_shm_pool().  While any #Arena `allocate()` *can*
   * trigger that, only a small % of them will do so.  Therefore we can use the most restrictive ordering
   * specifier (as-if `m_connected` is protected by a standard mutex) for simplicity with negligible perf impact.
   */
  std::atomic<bool> m_connected;

  /**
   * Per-collection set of borrowed pool IDs; used for cleanup in dtor.
   *
   * ### Thread safety ###
   * It is only accessed in receive_arena() and receive_shm_pool(), both in thread W; and then in
   * dtor -- after thread W is joined -- so no synchronization is required.
   */
  std::unordered_map<collection_id_t, std::unordered_set<pool_id_t>> m_borrower_pool_id_map;

  /**
   * Maps an arena to its SHM-pool listener; this is used for receiving changes in the SHM pools.
   *
   * ### Thread safety ###
   * Accessed in lend_arena() (user thread) and dtor (after thread W is joined).  No synchronization:
   * lend_arena() is not thread-safe w/r/t concurrent calls (nor documented as such), and dtor runs
   * after all user activity has ceased.
   */
  std::unordered_map<std::shared_ptr<Arena>,
                     std::unique_ptr<Shm_pool_listener_impl>> m_shm_pool_listener_map;

  /**
   * Stats about borrower SHM-pool (and containing SHM-arena) borrowing (opening) and
   * unborrowing (closing) *from the PoV of `this` Shm_session object*.  For background on the latter
   * emphasized point please see stat::Borrower_pool_stats doc header.  In short, though, it means that:
   *   - borrowing/registering an arena (and borrowing/registering contained SHM-pool) is equal to
   *     first-registering that arena (and first-registering contained SHM-pool); and
   *   - unborrowing/deregistering an arena (and unborrowing/deregistering contained SHM-pool) is equal to
   *     last-deregistering that arena (and last-deregistering contained SHM-pool).
   *
   * That is, some of the counters are degenerate in that they will equal related counters.
   * There is also global counting of such things in Borrower_shm_pool_collection_repository (singleton), where
   * that is not the case: e.g., a pool can be borrowed twice and then unborrowed twice -- that would count
   * 2 registerings, 2 unregisterings, 1 first-registering (= open pool + memory-map it), 1 last-deregistering
   * (= memory-unmap + close it).  That is because the same pool (also arena) can be borrowed twice, once
   * through one Shm_session, again through another; but in reality it is only opened/mapped the first time.
   *
   * ### Design/performance ###
   * The most frequent events are borrow-pool and unborrow-pool and are fairly rare.  They can be stat-consumed
   * by user (also rare).  Concurrency is possible at least because consumption can occur concurrently with
   * the borrow-pool stat-updates, so we use `atomic<>`s, but due to the rarity of updates
   * no sharding is necessary.  This follows the concurrent, non-sharded design (as introduced in
   * `flow::util::stat` doc header).
   *
   * No mutex is necessary.
   */
  Borrower_pool_stats m_borrower_pool_stats;

  /**
   * Task engine to process tasks in a serial manner (a/k/a thread W).  The only use case currently is incoming
   * SHM-channel messages from the lender.  The messages must be serialized, as out-of-ordering may cause issues.
   * In particular:
   *   -# An object being borrowed is processed prior to the SHM pool containing it.
   *   -# A SHM pool being borrowed is processed prior to a SHM pool being removed with the same address.
   *   -# A SHM pool being borrowed is registered prior to the collection containing it.
   */
  flow::async::Single_thread_task_loop m_serial_task_loop;

  /**
   * The channel used for transmitting SHM-pool messages.
   * Note that by transport::struc::Channel contract it *is safe* to execute `m_shm_channel.X()`
   * and `m_shm_channel.Y()` concurrently for all `X` and `Y` (whether they're the same method or not).
   */
  std::optional<Shm_struc_channel> m_shm_channel;

  /**
   * Callback executed when a channel error code is emitted when using #m_shm_channel.
   * Emptied after calling it which must only occur once (for cleanliness + lower memory use).
   */
  flow::async::Task_asio_err m_shm_channel_error_handler;
}; // class Shm_session

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename T>
Shm_session::Blob Shm_session::lend_object(const Handle<T>& object)
{
  using flow::error::Runtime_error;
  using Owner_shm_pool_repository = ipc::shm::arena_lend::detail::Owner_shm_pool_repository<Arena>;
  using ipc::shm::arena_lend::detail::Thread_lcl_obj_db_admin;
  using Thread_lcl_obj_db_client = ipc::shm::arena_lend::detail::Thread_lcl_obj_db_client<Arena>;
  using ipc::shm::arena_lend::detail::Owner_obj_disposer_and_mdt;

  Thread_lcl_obj_db_admin<Arena>::this_thread_piggy_scan(); // Opportunistic!

  if (!m_connected) // This best-effort gating is discussed in create() doc header.
  {
    FLOW_LOG_TRACE("Disconnected, so ignoring object lending attempt.");
    return {};
  }
  // else

  /* Note: This is the one Obj_handle-impl-specific call around here: should Obj_handle ever
   * switch impls (e.g., to boost::shared_ptr), use that impl's get_deleter() equivalent. */
  auto const * const disposer = std::get_deleter<Owner_obj_disposer_and_mdt<Arena>>(object);
  if (!disposer)
  {
    /* @todo In the rest of Flow[-IPC] we might assert(false) here; or emit an error Flow-style (which in fact
     * throws Runtime_error, albeit with a nice Error_code, if one doesn't supply non-null Error_code* err_code).
     * We could also return an empty Blob here per our own doc header.  Leaving it alone for now so as to not
     * rock the boat.  Deciding this would be part of to-do(s) around SHM-jemalloc w/r/t straightening out
     * error reporting for consistency (internally and versus Flow-IPC in general) and robustness.  For now, guidance:
     * 1, assert() would not be crazy; it's not that different from passing null when non-null is expected.
     * 2, on the other hand, if one is not used to the project they might pass some random shared_ptr<T> in here,
     * and a nice error might be a more civilized way of informing them.  If so: probably this should just be
     * one of many Flow-IPC APIs with Flow error-emission semantics (take Error_code*, on error throw if null,
     * set out-arg otherwise; on success don't throw/set to falsy Error_code respectively). -ygoldfel */
    throw Runtime_error{"lend_object() was given handle (shared_ptr) without disposer of the expected type; "
                          "did you get it from Ipc_arena::construct() as required?"};
  }
  // else

  const auto object_ptr = object.get();

  Blob blob{sizeof(Shm_object_handle)};
  const auto& shm_arena = *disposer->m_shm_arena;
  const auto collection_id = shm_arena.get_id();
  const auto use_ct_idx = disposer->m_use_ct_idx;
  const auto lend_tracker_pool_id = disposer->m_lend_tracker_pool_id;
  const auto object_handle = reinterpret_cast<Shm_object_handle*>(blob.data());
  object_handle->m_collection_id = collection_id;
  object_handle->m_lend_tracker_pool_id = lend_tracker_pool_id;
  object_handle->m_use_ct_idx = use_ct_idx;
  Owner_shm_pool_repository::from_address(object_ptr, // <-- from that | compute these --v
                                          object_handle->m_pool_id, object_handle->m_pool_offset);

  if (flow::util::this_thread_unique_token() == disposer->m_cting_thread_token)
  {
    /* shared_ptr being lent in the same thread that created the shared_ptr: we can report it
     * to the thread-local obj-DB *admin* directly.  We know it exists in this thread, so might as well;
     * in any case admin can do anything client can do; so that's our policy; no need to duplicate the associated
     * data in the same thread unnecessarily. */
    Thread_lcl_obj_db_admin<Arena>
      ::this_thread_obj_db()->lending_obj(collection_id, use_ct_idx);
  }
  else
  {
    Thread_lcl_obj_db_client
      ::this_thread_obj_db()->lending_obj(shm_arena, lend_tracker_pool_id, use_ct_idx);
  }

  return blob;
} // Shm_session::lend_object()

template<typename T>
Shm_session::Handle<T> Shm_session::borrow_object(const Blob& blob) const
{
  using ipc::shm::arena_lend::detail::Thread_lcl_obj_db_admin;
  using ipc::shm::arena_lend::detail::construct_with_borrower_obj_disposer;
  using Borrower_shm_pool_collection_repository
    = ipc::session::shm::arena_lend::Borrower_shm_pool_collection_repository<Arena>;
  using std::memcpy;

  Thread_lcl_obj_db_admin<Arena>::this_thread_piggy_scan(); // Opportunistic!

  const size_t blob_size = blob.size();
  if (blob_size != sizeof(Shm_object_handle))
  {
    FLOW_LOG_WARNING("Blob size [" << blob_size << "] does not match expected "
                     "size [" << sizeof(Shm_object_handle) << "].  This indicates a bug on someone's part; "
                     "either the blob was somehow improperly formed owner-side, or the user has supplied something "
                     "unequal to what was formed there.  borrow_object() returning null as advertised.");
    return {};
  }
  // else

  /* memcpy() it out of there: the source address may not be aligned.  (In many APIs such things are assumed as a
   * matter of course, but as `blob` may be IPCed-over to us via any technique, we're being
   * extra defensive.) */
  Shm_object_handle object_handle;
  memcpy(&object_handle, blob.const_data(), sizeof(object_handle));

  const auto obj_addr
    = Borrower_shm_pool_collection_repository::to_address_safe(object_handle.m_pool_id,
                                                               object_handle.m_pool_offset);
  if (!obj_addr)
  {
    /* We used to_address_safe() (rather than the marginally quicker and subsequently-used to resolve various
     * pointers such as inside STL-compliant containers (off borrowed handles like this one... but valid)
     * to_address()) as a safety measure against bogus/buggy `blob`s.  This follows the philosophy in
     * classic::Pool_arena::borrow_object() to be defensive at this particular point, where we process an
     * ostensibly IPCed-over datum.  That said: this is a best-effort thing: In particular m_pool_offset as of
     * this writing can point outside the pool (to_address_safe() will not detect it); never mind any check for
     * whether sizeof(T) would lie beyond the pool's bounds.  classic's equivalent as of this writing can and
     * does perform those checks.
     *
     * Inside Borrower_shm_pool_collection_repository::to_address_safe() there are some notes on what it would take to
     * extend this -- but, again, it is a best-effort safety measure. */

    FLOW_LOG_WARNING("Borrowed blob encodes pool ID [" << object_handle.m_pool_id << "], but this pool ID "
                     "is no longer borrowed or has never been borrowed; treating the blob as invalid.  "
                     "borrow_object() returning null as advertised for invalid SHM-handle blobs.");
    return {};
  }
  // else

  /* See doc header for Shm_object_handle for recap of which pieces of info below are used when/why.
   * See doc header for construct_with_borrower_obj_disposer() for a recap of the properties of the disposer
   * we are attacking to the shared_ptr returned here. */
  return construct_with_borrower_obj_disposer<T, Shm_session>
           (static_cast<T*>(obj_addr),
            object_handle.m_lend_tracker_pool_id, // Datum 1/1 used in the fast path.
            object_handle.m_use_ct_idx, // Datum 2/2 used in the fast path.
            m_remote_process_id, // A/k/a owner_id of type owner_id_t.  Used in slow path.
            object_handle.m_collection_id, // Used in slow path.
            /* As of this writing this value (shared_ptr<const Shm_session> pointing to *this) isn't used per se;
             * but it prevents death of *this Shm_session while shared_ptr<T> handles that came from its
             * .borrow_object() still live.  For better or worse those are the semantics we want to guarantee with the
             * factory+private ctor pattern.  See Shm_session doc header. */
            shared_from_this());

  /* Interesting fact; could be useful at some point: *this is actually only a little necessary for borrow_object()'s
   * impl.  Suppose it were not necessary: borrow_object() could be static; that change could be propagated to
   * ipc::session::Session (though then it would become more asymmetrical to arena-sharing providers including
   * SHM-classic) and possibly somewhat beyond.  At any rate in and of itself relationships would become simpler,
   * all else being equal (which it's not, but read on).  As for how *this is required as of this writing:
   *   - m_remote_process_id: Convenient, but could be obtained some other way such as directly from the
   *     m_shm_channel API or encoded in object_handle.
   *   - shared_from_this() a/k/a `this` itself: Passed to construct_with_borrower_obj_disposer(), but as noted
   *     it is only stored to ++ its group ref-count, preventing *this death before all the borrowed SHM-handles
   *     it generates (in this method) are dropped.  That is actually an ontensibly important guarantee, and
   *     other things document it.  Though, as I (ygoldfel) understand it, it is apparently a stand-alone feature;
   *     don't think anything specifically relies on it, outside of potentially user code to have been written
   *     by now (removing it would be a breaking change).  It's a nice guarantee; at least it is quasi-symmetrical
   *     with owner-side Ipc_arena similarly not dying until all it-construct()ed SHM-handles are dropped.
   * Not suggesting removing this dependency; as written or implied just above it makes some sense.  Good to keep
   * in mind though. */
} // Shm_session::borrow_object()

} // namespace ipc::session::shm::arena_lend::jemalloc
