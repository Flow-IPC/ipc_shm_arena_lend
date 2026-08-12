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
#include "ipc/session/standalone/shm/arena_lend/jemalloc/shm_session.hpp"
#include "ipc/session/standalone/shm/arena_lend/borrower_shm_pool_collection_repository.hpp"
#include "ipc/shm/arena_lend/jemalloc/ipc_arena.hpp"
#include "ipc/shm/arena_lend/jemalloc/stat_info_dump.hpp"
#include "ipc/shm/arena_lend/detail/owner_spc_impl.hpp"
#include "ipc/shm/arena_lend/detail/thread_lcl_obj_db.hpp"
#include "ipc/shm/arena_lend/detail/arena_lend_fwd.hpp"
#include "ipc/transport/struc/channel_base.hpp"
#include "ipc/common.hpp"
#include <flow/util/stat/stat_set.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <vector>

namespace ipc::session::shm::arena_lend::jemalloc
{

// Implementations.

std::shared_ptr<Shm_session> Shm_session::create(flow::log::Logger* logger,
                                                 Shm_channel&& shm_channel,
                                                 const transport::struc::Session_token& session_token_non_nil,
                                                 flow::async::Task_asio_err&& shm_channel_error_handler)
{
  return std::shared_ptr<Shm_session>(new Shm_session{logger, std::move(shm_channel), session_token_non_nil,
                                                      std::move(shm_channel_error_handler)});
}

Shm_session::Shm_session(flow::log::Logger* logger, Shm_channel&& shm_channel,
                         const transport::struc::Session_token& session_token_non_nil,
                         flow::async::Task_asio_err&& shm_channel_error_handler) :

  flow::log::Log_context(logger, Log_component::S_SESSION),
  m_remote_process_id(shm_channel.remote_peer_process_credentials().process_id()), // <-- Caution (see our doc header).
  m_connected(true),
  /* Immediately upgrade the sync_io unstructured core Channel into our struc::Channel.
   * We still have to start() it and stuff which we shall do presently.  Nothing can fail here though. */
  m_shm_channel(std::in_place,
                get_logger(), std::move(shm_channel), transport::struc::Channel_base::S_SERIALIZE_VIA_HEAP,
                session_token_non_nil),
  m_shm_channel_error_handler(std::move(shm_channel_error_handler)),
  m_serial_task_loop(logger, "JSSS_" + std::to_string(m_remote_process_id))
{
  using flow::async::reset_this_thread_pinning;

  FLOW_LOG_TRACE("Constructing [" << this << "].");

  // Start thread W in background.
  m_serial_task_loop.start(reset_this_thread_pinning);
  // Don't inherit any strange core-affinity!  ^-- Workers must float free.

  /* Let's set this up before .start(); then it can't return false indicating channel-hosing by Channel contract.
   * Less stuff to worry about.  .start() itself can totally trigger the error handler we give it, immediately,
   * but from another thread, and we have to handle that anyway.  So let's keep it simple(r). */
#ifndef NDEBUG
  const bool ok =
#endif
  m_shm_channel->expect_msgs(schema::IpcShmMessage::Which::LEND_ARENA,
                             [this](auto&& req) mutable
  {
    // We are in a struc::Channel thread (not our thread W a/k/a m_serial_task_loop).
    m_serial_task_loop.post([this, req = std::move(req)]()
    { // In thread W.
      const auto& reader = req->body_root().getLendArena();
      receive_arena(reader.getCollectionId(), Shared_name::ct(reader.getPoolNameBase())); // Can't fail.
      send_response(req.get(), "lend arena response"); // Can fail but will handle it.
    });
  })
    && m_shm_channel->expect_msgs(schema::IpcShmMessage::Which::LEND_POOL,
                                  [this](auto&& req) mutable
  {
    // We are in a struc::Channel thread (not our thread W a/k/a m_serial_task_loop).
    m_serial_task_loop.post([this, req = std::move(req)]()
    { // In thread W.
      const auto& reader = req->body_root().getLendPool();
      receive_shm_pool(reader.getCollectionId(), reader.getPoolId(), reader.getPoolSize()); // Can't fail.
      send_response(req.get(), "lend pool response"); // Can fail but will handle it.
    });
  });
  assert(ok && "struc::Channel::expect_msgs() is not supposed to report hose channel before start().");

  /* At the moment m_shm_channel (a struc::Channel) is basically inert.  For it to be able to accept incoming messages
   * we must .start() it.  Easy enough but the harder thing is that it takes the on-channel-hosed handler as an arg.
   * Now is the time to read the create() doc header about how we promised to report *all* errors.  This here is
   * the main path -- most likely we'll be informed this way first -- but it's possible it'll be when trying
   * send() or sync_request().  Whichever way it is, all subsequent m_shm_channel->xyz() calls will return false
   * or equivalent which indicates "something else has or concurrently is reporting channel-hosing."  This is all
   * fine for our purposes; we'll get exactly one channel-hosing indication, and then we'll pass that on
   * via set_disconnected() also exactly once.  Anyhoo... this is one of those places. */

  m_shm_channel->start([this](const Error_code& err_code) mutable
  {
    // We are in a struc::Channel thread (not our thread W a/k/a m_serial_task_loop).
    set_disconnected(err_code); // This can handle being called in any thread.
  });

} // Shm_session::Shm_session()

Shm_session::~Shm_session()
{
  // Log ~final info/stats.
  {
    Info_dump dump; // Multi-line.
    info_dump(&dump);
    FLOW_LOG_INFO("Shm_session [" << this << "] shutdown: "
                  "~Final state (includes ~final this-session + ~current global):"
                  "\n" << dump << '.'); // Note: no newline at end of info_dump.
  }

  /* For this thread W (m_serial_task_loop) we started at session start:
   *   - If a task is concurrently executing right now, wait until it (and only it) finishes.
   *     It could conceivably be processing a message.  (Sanity-checking thought experiment: This side's
   *     end user decides to end session: frees all borrowed-through-it objects and lets go of any Shm_session
   *     handle.  Right then opposing Ipc_arena that was lent to us issues LendPool, simply because opposing
   *     end user construct<T>()ed a thing in SHM.  That could be being handled right now among others.)
   *     - If none is concurrently executing then nothing to do; continue immediately.
   *   - Stop/join the thread.
   * Otherwise such code in such threads can access stuff we access/modify below.
   *
   * (Is the stopping, in and of itself, dangerous somehow?  Well, if something is concurrently happening already,
   * then we cannot stop it now; we are then acting as-if we (dtor) were called a tiny bit later by waiting for
   * it to complete.  If something is about to happen, but we prevented it, then it is ~no different from the
   * precipitating event occurring a tiny bit later -- when there is no Shm_session through which to speak anymore.)
   *
   * Recap (1): The above concerns safety of concurrent interactions between:
   * *this dtor <=> m_serial_task_loop's thread m_serial_task_loop.W.
   *
   * Similarly wrap-up any handler -- especially error handler (that calls set_disconnected()) -- since stuff
   * is shutting down around now -- concurrently executing for m_shm_channel; and join that thread.  It's
   * why we made m_shm_channel optional<>, as struc::Channel lacks a stop(), so instead we delete it.
   *
   * Recap (2): The above concerns safety of concurrent interactions between:
   * *this dtor <=> m_shm_channel's thread m_shm_channel.W.
   *
   * Careful, however: the deletion of *m_shm_channel is itself "*this dtor" code.  If it happens while
   * m_serial_task_loop.W is running, then "Recap (1)" gets broken.  Therefore the order must be:
   * stop m_serial_task_loop, stop (delete) m_shm_channel.  Though... are m_shm_channel.W-posted tasks
   * going to break with m_serial_task_loop stop()ed?  Answer: no; by fundamental design, they merely post tasks onto
   * thread W (m_serial_task_loop.W); this is allowed and fine (no different, in fact it essentially *is*,
   * boost::asio::post() onto a non-run()ing io_context); such tasks are queued and don't run (and will be very
   * soon then deleted with m_serial_task_loop). */
  m_serial_task_loop.stop();
  m_shm_channel.reset();
  /* Now m_connected can't change -- only set_disconnected() modifies it and only in thread W -- which is nice
   * for cleanliness though shouldn't matter.
   *
   * Anything m_shm_channel would fire now would also be done in thread W -- and we just joined thread W -- which
   * is great.  m_shm_channel and everything inside it including the socket-stream socket and all will be cleaned up
   * shortly.
   *
   * Now that we're at async peace... clean up SHM-jemalloc stuff.
   *
   * Regarding Thread_lcl_obj_db_admin<Arena>::forgetting_shm_arena(): should we call it?
   *
   * First recall that this cleanup has to do with resource return; resources being at most some stuff in heap;
   * Lend_tracker_pool pool handles and underlying named SHM-pools potentially; and construct<T>()ed objects.
   *
   * It is clearly wrong to _admin::forgetting_shm_arena(); opposing-entity (process) arena is not reachable by us;
   * while our-side arenas are ones we were lending; they may survive a long time after this dtor and have live
   * objects; it makes no sense to forget anything, just because we're no longer going to be lending from such an
   * arena through this session. */

  /* Remove listeners registered to the respective arenas so that no notifications are further processed from the
   * arenas, which may survive much longer than the session. */
  for (const auto& cur_map_pair : m_shm_pool_listener_map)
  {
    auto& cur_arena = cur_map_pair.first;
    auto& cur_listener_ptr = cur_map_pair.second;
    cur_arena->remove_shm_pool_listener(cur_listener_ptr.get());
  }

  /* Deregister SHM-pools from repository of borrower-side such pools.  These are arranged by arena (collection)
   * (each belonging to the owner a/k/a process on the opposing side of this session a/k/a IPC conversation);
   * Borrower_shm_pool_collection_repository requires we then deregister the arena (once all pools in it, that we've
   * borrowed, have been deregistered).
   *
   * Recall, for general understanding, that a given pool P we're deregistering here may well be being borrowed
   * by another Shm_session (necessarily to the same owner/process/PID).  (Incidentally, that owner/process/PID
   * may even be *this* process: borrowing by process X from process X!  That's sometimes useful for testing,
   * probably not so much in production, but it is fully allowed and not special.)  Therefore deregistering here
   * will unmap memory/close SHM-pool handle if and only if we are the last Shm_session to have been borrowing
   * the pool.  Otherwise Borrower_shm_pool_collection_repository --es a per-pool ref-count. */
  const auto& borrower_pool_id_map = m_borrower_pool_id_map;
  auto& borrower_repository = Borrower_shm_pool_collection_repository<Arena>::get_instance();
  for (const auto& cur_map_pair : borrower_pool_id_map)
  {
    auto cur_collection_id = cur_map_pair.first;
    const auto& cur_shm_pool_ids = cur_map_pair.second;

    for (const auto cur_shm_pool_id : cur_shm_pool_ids)
    {
      borrower_repository.deregister_shm_pool(m_remote_process_id, cur_collection_id, cur_shm_pool_id);
    }
    borrower_repository.deregister_collection(m_remote_process_id, cur_collection_id);
  }

  { /* Stats.
     * ...nothing to do for m_borrower_pool_stats, as per-session stats are going poof anyway.  Don't worry... the
     * global Borrower_shm_pool_collection_repository will track its things within the above .deregister_*()s.
     *
     * Maintainers: unborrowing pre-here could be added -- that would certainly mean m_borrower_pool_stats
     * would need updating there, unlike here. */
  }
} // Shm_session::~Shm_session()

void Shm_session::set_disconnected(const Error_code& err_code)
{
  /* By our contract we are to handle being called from any thread.  As of this writing at least that can be:
   * thread W (receive_*()), an end user thread (lend_*()), or m_shm_channel's background thread from channel-hosed
   * handler (set up in ctor via m_shm_channel->start()).
   *
   * We've promised in m_connected doc header to only modify it in thread W (for reason(s) explained there), and
   * calling m_shm_channel_error_handler(err_code) is to be from thread W as well.  So yeah, we need to
   * post to thread W.  However if we're already there then no need. */

  auto do_it_func = [this, err_code]()
  {
    // We are in thread W.

    bool expected_pre = true; // Could just assign or .store(), but we can do this for the following check.
    m_connected.compare_exchange_strong(expected_pre, false);
    assert(expected_pre
           && "By our contract we are to be called at most once, on m_shm_channel hosing being self-reported; "
                "yet disconnected-flag was already true.  Bug in struc::Channel or in Shm_session.");

    FLOW_LOG_WARNING("Relaying channel-hosing (reason [" << err_code << "] [" << err_code.message() << "]).");
    m_shm_channel_error_handler(err_code);
    m_shm_channel_error_handler.clear(); // No need to keep it in memory.
  }; // auto do_it_func =

  if (m_serial_task_loop.in_thread())
  {
    do_it_func(); // In thread W.
  }
  else
  {
    m_serial_task_loop.post([do_it_func = std::move(do_it_func)]()
    {
      do_it_func(); // In thread W.
    });
  }
} // Shm_session::set_disconnected()

bool Shm_session::lend_arena(const std::shared_ptr<Arena>& arena)
{
  using ipc::shm::arena_lend::detail::Owner_spc_impl;

  // We are in an end user thread.

  if (!m_connected) // This best-effort gating is discussed in create() doc header.
  {
    FLOW_LOG_WARNING("Disconnected, so ignoring arena lending attempt.");
    return false;
  }
  // else

  collection_id_t collection_id = arena->get_id();

  const auto result_pair
    = m_shm_pool_listener_map.emplace(arena, std::make_unique<Shm_pool_listener_impl>(*this, collection_id));
  assert(result_pair.second && "Duplicate arena should never be lend_arena()ed.");

  // Compose and send message.
  auto message = m_shm_channel->create_msg();
  auto lend_arena = message.body_root()->initLendArena();
  lend_arena.setCollectionId(collection_id);
  lend_arena.setPoolNameBase(Owner_spc_impl<Arena>{*arena}.get_pool_name_base().str());
  if (!send_sync_request(message, "lend arena")) // We may have triggered set_disconnected() in here.
  {
    // Revert stuff already done.
#ifndef NDEBUG
    const bool ok = 0 !=
#endif
    m_shm_pool_listener_map.erase(arena);
    assert(ok && "We just inserted this; erase must succeed.");

    return false;
  }

  // After registering this, we will get a notification for the initial shared memory pools in the arena.
#ifndef NDEBUG
  const bool ok =
#endif
  arena->add_shm_pool_listener(result_pair.first->second.get());
  assert(ok && "We just created a fresh listener; add must succeed.");

  FLOW_LOG_INFO("Successfully registered arena [" << collection_id << "] (opposing owner ID (PID) "
                "[" << m_remote_process_id << "]) for lending.");
  return true;
} // Shm_session::lend_arena()

void Shm_session::lend_shm_pools(collection_id_t collection_id, const std::set<std::shared_ptr<Shm_pool>>& shm_pools)
{
  for (const auto& cur_shm_pool : shm_pools)
  {
    if (!lend_shm_pool(collection_id, cur_shm_pool))
    {
      return; // It'd check #m_connected and no-op anyway.
    }
  }
}

bool Shm_session::lend_shm_pool(collection_id_t collection_id, const std::shared_ptr<Shm_pool>& shm_pool)
{
  // We are in an end user thread (ultimately from Arena::allocate() presumably).

  if (!m_connected) // This best-effort gating is discussed in create() doc header.
  {
    FLOW_LOG_WARNING("Disconnected, so ignoring pool lending attempt.");
    return false;
  }
  // else

  // Compose and send message.
  auto message = m_shm_channel->create_msg();
  auto lend_pool = message.body_root()->initLendPool();
  lend_pool.setCollectionId(collection_id);
  lend_pool.setPoolId(shm_pool->get_id());
  lend_pool.setPoolSize(shm_pool->get_size());
  return send_sync_request(message, "lend SHM pool"); // We may have triggered set_disconnected() in here.
}

void Shm_session::remove_lender_shm_pool(collection_id_t collection_id, const std::shared_ptr<Shm_pool>& shm_pool)
{
  /* @todo Restore RemovePool propagation to borrower.  Motivation: Without it borrower-side pool resources
   * (fd, mmap, Borrower_shm_pool_collection_repository entries, thread-local caches) persist until ~Shm_session().
   * That session-teardown cleanup (see our dtor) is the backstop and always works; but for long-running sessions
   * pool mappings accumulate as dead weight.  Sending RemovePool here would let the borrower eagerly call
   * Borrower_shm_pool_collection_repository::deregister_shm_pool() and reclaim resources mid-session.
   *
   * Considerations for the re-implementation:
   *   - Pool removal on the owner side (remaining_size -> 0) means all objects have been deallocated; use-counts
   *     (atomics in SHM) reached 0 on both sides; so the borrower has no valid references into the pool.
   *     It should be safe to send RemovePool unconditionally.
   *   - A much older version of this code checked whether any construct()ed objects still belonged to the pool
   *     and skipped RemovePool if so (arena-teardown path with live objects).  However: (1) we now lack a DB of
   *     constructed-objects-per-pool; (2) a pool also contains non-first-class buffers (STL-allocator-allocated),
   *     so the check was incomplete anyway; (3) detail::Owner_obj_disposer_and_mdt shared_ptr machinery keeps
   *     Ipc_arena alive until all first-class objects are destroyed, so the teardown-with-live-objects scenario
   *     should not arise normally.  Still, worth keeping in mind.
   *   - On the borrower side, receiving RemovePool would call
   *     Borrower_shm_pool_collection_repository::deregister_shm_pool() (ref-count decrement; munmap + close(fd)
   *     when ref-count hits 0) and remove the pool_id from m_borrower_pool_id_map.
   *   - The IpcShmMessage.capnp schema needs a RemovePool struct re-added (slot @2 is reserved as Void).
   *   - Must handle m_connected == false gracefully (can't send if disconnected; that's fine, dtor handles it).
   *
   * Note regarding the impact of lacking RemovePool/related:
   *   - One: can SHM-pools in a lender even go away mid-arena-life (which is when RemovePool would matter)?
   *     Much code supports it; but under the present policy: no.  When jemalloc decides -- via decay or
   *     forced purge -- that an unused extent (= SHM-pool space) could be let go, it asks via the
   *     extent-dalloc hook, and Ipc_arena::optional_remove_shm_pool() always declines; jemalloc then parks
   *     the still-mapped space in its *retained* set for later reuse.  (The physical pages do get released
   *     -- via SHM-object hole-punching in our purge/decommit hooks -- but the mapping and the SHM-pool
   *     live on.)  Pool removal therefore fires only via the extent-destroy hook at arena teardown.  So
   *     today RemovePool would have nothing to do mid-session; that is policy though, not a hard
   *     invariant -- see optional_remove_shm_pool().  Against the day the policy is relaxed, one danger
   *     was investigated (jemalloc 5.3.1 source) and dismissed: a new pool appearing at a dead pool's
   *     owner-side vaddr.  (jemalloc itself cannot directly cause it: an extent freed via a *successful* dalloc hook
   *     is deregistered and forgotten (retention happens only on declining, and then nothing was unmapped
   *     to begin with); the only specific-address requests jemalloc makes via the alloc hook are for
   *     in-place *expansion* (one-past-the-end of a live extent), never a freed address; and our mapping
   *     code uses mmap()-hint semantics, never `MAP_FIXED`.  No matter; nothing will try to specifically reuse a
   *     vaddr, true; but vaddr reuse is technically possible even so... read on.) OS-level vaddr reuse (a later mmap()
   *     landing on a previously-munmap()ed range) remains possible but is benign owner-side: removal
   *     erases the pool from Owner_shm_pool_repository (canonical maps + push-updated reverse-lookup
   *     caches) before any new pool can exist there, and the never-pruned forward-lookup caches are keyed
   *     by never-reused pool IDs (admittedly and knowingly leaky but harmless).  The
   *     erase-before-unmap ordering that benignity relies upon is an explicit
   *     invariant of both removal paths -- see the comment in Owner_shm_pool_collection::remove_shm_pool()
   *     (mid-life path) and the teardown-sequence recap in jemalloc::Ipc_arena (bulk path).
   *   - Two: also, though, consider whether it is possible to destroy an Ipc_arena
   *     that is being lent (Shm_session::lend_arena()) via a session.  The answer is, as of this writing, no.
   *     This is likely (original author: echan; author of this comment: ygoldfel) intentional.
   *     Shm_session::lend_arena(A), where A is shared_ptr<Ipc_arena>, memorizes A.  As of this writing it's in
   *     m_shm_pool_listener_map -- easy to forget about it -- but it's there and *does* prevent Ipc_arena
   *     death until Shm_session death.  To be clear that refers to *owner*-side Shm_session death -- not
   *     borrower-side -- which is the one that needs to be destroyed for the borrower-side pool cleanup.
   *     However, if opposing Shm_session is done-for, then by definition the local one is useless and to be
   *     destroyed.  (Certainly if these are being driven by ipc::session, as opposed to a user manually using
   *     SHM-jemalloc API directly -- allowed but more exotic -- then yes, Shm_session here only exists if
   *     the opposing one does.  Direct use wouldn't get anything out of trying to break this pattern either.)
   *     - (There is, as of this writing, no Shm_session::unlend_arena() (or however one might name it).  If there
   *       were, which sounds pretty useful, that still would not mean RemovePool is required; but a RemoveArena
   *       would be necessary, probably.  On receipt would just do a subset of what Shm_session dtor does:
   *       for that specific arena, deregister all pools relevant to it (~Shm_session() does same but for all arenas,
   *       period).)
   *
   * Also -- m_borrower_pool_stats would need updates (when receiving hypothetical RemovePool and/or on
   * unlend-arena, if that becomes a thing). */

  FLOW_LOG_TRACE("Pool removal notification for pool [" << shm_pool->get_id() << "], collection [" <<
                 collection_id << "]; RemovePool not re-implemented (possible @todo), borrower cleanup deferred to "
                 "session teardown.");
} // Shm_session::remove_lender_shm_pool()

bool Shm_session::send_sync_request(Shm_struc_channel::Msg_out& message, util::String_view operation)
{
  // We are in an end user thread.

  /* No timeout: any kind of wait here is essentially infinite and the result of a catastrophic bug somewhere;
   * better not to mask it, and no use in trying to recover from it. */
  Error_code err_code;
  const auto response = m_shm_channel->sync_request(&message, nullptr, &err_code);

  if (!response)
  {
    if (err_code)
    {
      FLOW_LOG_WARNING("Failed to complete request on channel, operation [" << operation << "] due to "
                       "error [" << err_code << "] [" << err_code.message() << "].  Reporting channel-hosing.");
      set_disconnected(err_code); // This can handle being called in any thread.
    }
    else
    {
      FLOW_LOG_WARNING("Failed to send request on already hosed channel, operation [" << operation << "].  "
                       "The official channel-hosing would have been reported/is being reported concurrently "
                       "via on-channel-hosed handler.");
    }
    return false;
  }
  // else

  /* Response.success is always true: the borrower-side actions (open SHM-pool, register collection) are
   * catastrophic on failure (abort there); so by the time we get a response it succeeded.  Assert just in case. */
  assert(response->body_root().getResponse().getSuccess()
         && "Borrower indicated failure; should have aborted there instead.");

  FLOW_LOG_TRACE("Successfully fulfilled [" << operation << "] sync request.");
  return true;
}

void Shm_session::send_response(const Shm_struc_channel::Msg_in* original_message, util::String_view operation)
{
  // We are in thread W (m_serial_task_loop).

  assert(original_message);

  auto message = m_shm_channel->create_msg();
  auto response = message.body_root()->initResponse();
  response.setSuccess(true); // Always true: borrower-side actions abort on failure.

  Error_code err_code;
  if (!m_shm_channel->send(&message, original_message, &err_code))
  {
    FLOW_LOG_WARNING("Failed to send reponse message on already hosed channel, operation [" << operation << "].  "
                     "The official channel-hosing would have been reported/is being reported concurrently "
                     "via on-channel-hosed handler.");
  }
  else if (err_code) // && (not previously-hosed)
  {
    FLOW_LOG_WARNING("Failed to send message on channel, operation [" << operation << "] due to "
                     "error [" << err_code << "] [" << err_code.message() << "].  Reporting channel-hosing.");
    set_disconnected(err_code); // This can handle being called in any thread.
  }
  else
  {
    FLOW_LOG_TRACE("Successfully sent [" << operation << "] response message.");
  }
}

void Shm_session::receive_arena(collection_id_t collection_id, Shared_name&& pool_name_base)
{
  using flow::util::stat::fetch_add;
  using flow::util::stat::update_hi_wmark;

  // We are in thread W (m_serial_task_loop).

  Borrower_shm_pool_collection_repository<Arena>::get_instance()
    .register_collection(m_remote_process_id, collection_id, std::move(pool_name_base));

#ifndef NDEBUG
  const auto insert_result =
#endif
  m_borrower_pool_id_map.emplace(collection_id, std::unordered_set<pool_id_t>{});
  assert(insert_result.second && "Duplicate collection; opposing process double-lent arena -- bug.");

  { /* Stats.
     * Arena can only be registered once per session. */
    fetch_add(&m_borrower_pool_stats.m_arena_register_count, 1);
    fetch_add(&m_borrower_pool_stats.m_arena_first_register_count, 1);
    update_hi_wmark(&m_borrower_pool_stats.m_n_borrowed_arenas_hi_wmark,
                    fetch_add(&m_borrower_pool_stats.m_n_borrowed_arenas, 1) + 1);
  }
} // Shm_session::receive_arena()

void Shm_session::receive_shm_pool(collection_id_t collection_id, pool_id_t shm_pool_id, pool_offset_t pool_size)
{
  using flow::util::stat::fetch_add;
  using flow::util::stat::update_hi_wmark;

  // We are in thread W (m_serial_task_loop).

  // register_shm_pool() aborts on failure (most notably: SHM-pool open/mmap failure is catastrophic).
  Borrower_shm_pool_collection_repository<Arena>::get_instance()
    .register_shm_pool(m_remote_process_id, collection_id, shm_pool_id, pool_size);

  const auto iter = m_borrower_pool_id_map.find(collection_id);
  assert((iter != m_borrower_pool_id_map.end()) && "Pool opened fine, but collection not in local map; bug?");
#ifndef NDEBUG
  const auto insert_result =
#endif
  iter->second.insert(shm_pool_id);
  assert(insert_result.second && "Pool opened fine, but pool ID already in local map; bug?");

  { /* Stats.
     * Arena -- therefore contained SHM-pool -- can only be registered once per session. */
    fetch_add(&m_borrower_pool_stats.m_pool_register_count, 1);
    fetch_add(&m_borrower_pool_stats.m_pool_open_count, 1);
    update_hi_wmark(&m_borrower_pool_stats.m_n_open_pools_hi_wmark,
                    fetch_add(&m_borrower_pool_stats.m_n_open_pools, 1) + 1);
    update_hi_wmark(&m_borrower_pool_stats.m_mapped_sz_hi_wmark,
                    fetch_add(&m_borrower_pool_stats.m_mapped_sz, pool_size) + pool_size);
  } // Stats.
} // Shm_session::receive_shm_pool()

util::process_id_t Shm_session::get_remote_process_id() const
{
  return m_remote_process_id;
}

Shm_session::Shm_pool_listener_impl::Shm_pool_listener_impl(Shm_session& owner,
                                                            collection_id_t collection_id) :
  m_owner(owner),
  m_collection_id(collection_id)
{
}

void Shm_session::Shm_pool_listener_impl::notify_initial_shm_pools(const std::set<std::shared_ptr<Shm_pool>>& shm_pools)
{
  m_owner.lend_shm_pools(m_collection_id, shm_pools);
}

void Shm_session::Shm_pool_listener_impl::notify_created_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool)
{
  m_owner.lend_shm_pool(m_collection_id, shm_pool);
}

void Shm_session::Shm_pool_listener_impl::notify_removed_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool)
{
  m_owner.remove_lender_shm_pool(m_collection_id, shm_pool);
}

void Shm_session::info_dump(Info_dump* target_info_dump, [[maybe_unused]] util::Call_timing)
{
  using flow::util::stat::stats_assign;
  using boost::adaptors::transformed;
  using std::vector;

  assert(target_info_dump);

  stats_assign(&target_info_dump->m_borrower_pool_stats, borrower_pool_stats());
  {
    auto& target_vec = target_info_dump->m_borrower_pool_stats_process_wide_per_arena;

    Borrower_pool_stats_list vec;
    stats_assign(&target_info_dump->m_borrower_pool_stats_process_wide_total, // <-- attn.
                 borrower_pool_stats_process_wide(&vec));
    /* borrower_pool_stats_process_wide() sets a vector<Own<X>>, while target_vec in *target_info_dump is
     * just vector<X>.  Why the dichotomy?  Answer: It is a bit subtle and subjective.  The individual
     * stat-accessors are a lower-level thing, intended to be user-friendly but tight -- e.g., don't copy
     * a thing if not needed; don't force the user to do the same, unless it's something small and simple.
     * So that one chose to set Own<X>, so the user can -- if they wish -- shuffle them around via handle
     * instead of *having to* in that situation copy the `X`s.  Meanwhile the info_dump() is at a higher level
     * which prioritized uniformity and simplicity; e.g. if stores a copy of things like borrower_pool_stats(),
     * where it could store a ptr.  So we are following this here too; that's all. */
    const auto vec_rng = vec | transformed([](const auto& stats_ptr) -> const Borrower_pool_stats&
                                             { return *stats_ptr; });

    target_vec = vector<Borrower_pool_stats>{vec.size()}; // Default-ct.  All shall be overwritten.
    auto target_vec_it = target_vec.begin();
    for (const auto& stats : vec_rng)
    {
      stats_assign(&(*target_vec_it++), stats);
    }
  } // { auto& target_vec = }

  stats_assign(&target_info_dump->m_borrower_pool_lookup_global_stats, borrower_pool_lookup_global_stats());
  target_info_dump->m_borrowed_shm_pool_live_info = borrowed_shm_pool_live_info();
} // Shm_session::info_dump()

std::vector<Shm_session::Shm_pool_info> Shm_session::borrowed_shm_pool_live_info() // Static.
{
  return Borrower_shm_pool_collection_repository<Arena>::get_instance().shm_pool_live_info();
}

const Shm_session::Borrower_pool_stats& Shm_session::borrower_pool_stats() const
{
  return m_borrower_pool_stats;
}

void Shm_session::borrower_pool_stats_reset()
{
  flow::util::stat::stats_reset(&m_borrower_pool_stats, Borrower_pool_stats{});
}

const Shm_session::Borrower_pool_lookup_global_stats& Shm_session::borrower_pool_lookup_global_stats() // Static.
{
  using ipc::shm::arena_lend::detail::Pool_lookup_global_stats;
  return Pool_lookup_global_stats<Arena, false>::stats();
}

const Shm_session::Borrower_pool_stats&
  Shm_session::borrower_pool_stats_process_wide(Borrower_pool_stats_list* per_arena_stats) // Static.
{
  return Borrower_shm_pool_collection_repository<Arena>::get_instance().stats(per_arena_stats);
}

void Shm_session::global_stats_reset() // Static.
{
  using ipc::shm::arena_lend::detail::Pool_lookup_global_stats;
  Pool_lookup_global_stats<Arena, false>::stats_reset();
  Borrower_shm_pool_collection_repository<Arena>::get_instance().stats_reset();
}

std::ostream& operator<<(std::ostream& os, const Shm_session& val)
{
  // @todo Something more useful than just this?
  return os << '@' << &val;
}

} // namespace ipc::session::shm::arena_lend::jemalloc
