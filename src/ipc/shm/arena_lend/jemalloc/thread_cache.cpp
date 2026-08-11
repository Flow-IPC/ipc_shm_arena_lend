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
#include "ipc/shm/arena_lend/jemalloc/thread_cache.hpp"
#include "ipc/shm/arena_lend/jemalloc/detail/jemalloc.hpp"
#include "ipc/common.hpp"
#include <flow/util/util.hpp>
#include <flow/error/error.hpp>
#include <jemalloc/jemalloc.h>
#include <vector>

namespace ipc::shm::arena_lend::jemalloc
{

// Static initializers.

Thread_cache::Static_state Thread_cache::s_state;

// Implementations.

Thread_cache* Thread_cache::this_thread_cache() // Static.
{
  const auto cache = s_state.m_caches.this_thread_state_or_null();
  if (!cache)
  {
    return s_state.m_caches.this_thread_state(); // Create and return it.
  }
  /* else: Before returning existing cache, we may want to destroy some tcaches in it (see destroy_arena_safely()).
   * (Do note 99.99999% of the time there is no such work pending, and the following call returns after a couple
   * of instructions, a no-op.  Meanwhile this call is fully opportunistic piggy-backing.) */

  cache->if_requested_destroy_tcaches_and_possibly_finish_arena_kills();

  return cache;
}

Thread_cache* Thread_cache::this_thread_cache_or_null() // Static.
{
  const auto cache = s_state.m_caches.this_thread_state_or_null();
  if (!cache)
  {
    return nullptr; // Nota bene.
  }
  // else
  cache->if_requested_destroy_tcaches_and_possibly_finish_arena_kills();
  return cache;
}

Thread_cache::Thread_cache(flow::log::Logger* logger_ptr) :
  flow::log::Log_context_mt(logger_ptr, Log_component::S_SHM),
  m_thread_token(flow::util::this_thread_unique_token())
{
  namespace this_thread = flow::util::this_thread;
  using flow::util::Thread_id;
  using flow::log::Logger;

  Thread_id ignored;
  Logger::set_thread_info(&m_thread_nickname, &ignored);

  // That's it for now; id() -- likely to be called very soon -- will create an actual tcache.

  // All good; ostream<< will print all the stuff (m_id, m_thread_token, etc.) incidentally.
  FLOW_LOG_INFO_LOCKED("Jem_tcache[" << *this << "]: "
                       "Created cache object for this thread (details to the left in this message); "
                       "no tcaches yet until next id() call.");
} // Thread_cache::Thread_cache()

Thread_cache::~Thread_cache()
{
  const auto this_thread_unique_token = flow::util::this_thread_unique_token();
  if (this_thread_unique_token != thread_token())
  {
    FLOW_LOG_WARNING_LOCKED
      ("Jem_tcache[" << *this << "]: "
       "Shutting down from different thread (unique-token [" << this_thread_unique_token << "]) "
       "than the one (unique-token [" << thread_token() << "]) that created us; "
       "cannot flush/destroy anything; bailing out.  In any case, this can conceivably -- in the "
       "absence of misuse by the Thread_cache user -- only occur when program is exiting, at which "
       "point graceful shutdown of jemalloc resources is arguably less essential.");
    return;
  }
  // else

  /* Sanity-check about Polled_shared_state<> s_state.m_arenas_death_row_map: Recall it maps arena ID => set of
   * `this`es like us which should do if_requested_destroy_tcaches_and_possibly_finish_arena_kills().
   * Now, there's a s_state.m_caches.while_locked() section in destroy_arena_safely() that loads `this` into that map
   * (and then arms thread-local flag to be checked by if_requested_destroy_tcaches_and_possibly_finish_arena_kills()).
   * Also though this thread is exiting, and Thread_local_state_registry<Thread_cache> in its cleanup function
   * also has s_state.m_caches.while_locked() which removed `this` from the registry of extant `Thread_cache`s; this
   * has already happened -- which is why we're being deleted -- and we are outside that while_locked(): both
   * formally guaranteed (see the lifecycle section in Thread_local_state_registry doc header).  So: either
   * we've been added into m_arenas_death_row_map before that happened, or we couldn't
   * be added into it anymore, as we weren't extant during the first s_state.m_caches.while_locked() mentioned above.
   * The important thing is that at *this* point `this` will never be added into s_state.m_arenas_death_row_map again;
   * so like if we were to remove it from there in this dtor, it's not concurrent against more of that stuff;
   * we're "safe" from that craziness now.  However!  What if we're in m_arenas_death_row_map right now?  It's then
   * quite important to remove us!  If we don't do it now, it can never happen, and the damned arena(s) will never
   * get destroyed.  No problem: while `this` exists (until this dtor finishes) we can still do it.
   * So we in fact do so below: if_requested_destroy_tcaches_and_possibly_finish_arena_kills().
   * We don't bother checking the flag but simply assume it's armed this one time: thread exit is infrequent,
   * and the scan is cheap when nothing is pending. */

  FLOW_LOG_INFO_LOCKED("Jem_tcache[" << *this << "]: "
                       "Shutting down.  First, will check for any pending arena destruction progress to push-forward; "
                       "this will destroy 0+ tcaches, destroy 0+ arenas, run 0+ on-done functors for the latter; next "
                       "will destroy (includes flushing) any remaining tcaches.");

  // Tell it not to check the flag and just act as-if it's armed (see above).
  if_requested_destroy_tcaches_and_possibly_finish_arena_kills(true);

  for (const auto& arena_and_tcache_id : m_tcaches_by_arena)
  {
    destroy_tcache(arena_and_tcache_id.second);
  }
} // Thread_cache::~Thread_cache()

void Thread_cache::flush_tcache(tcache_id_t id)
{
  using flow::util::this_thread_unique_token;
  using boost::system::system_category;

  assert((this_thread_unique_token() == thread_token()) && "Please don't flush/etc. across threads.");

  FLOW_LOG_INFO_LOCKED("Jem_tcache[" << *this << "]: Tcache-flush of tcache [" << id << "] requested: proceeding.");

  const auto code = IPC_SHM_ARENA_LEND_JEMALLOC_API(mallctl)("tcache.flush", nullptr, nullptr, &id, sizeof(id));
  if (code != 0)
  {
    const Error_code sys_err_code{code, system_category()};
    FLOW_LOG_FATAL_LOCKED("System error occurred: [" << sys_err_code << "] [" << sys_err_code.message() << "].");
    assert(false && "Tcache-flush failed -- as of this writing we consider this catastrophic; "
                      "for some ops such as to allow for arena destruction subsequently it is essential "
                      "for it to succeed.  We don't consider this recoverable.  @todo Revisit.");
    std::abort();
  }
  // else
} // Thread_cache::flush_tcache()

void Thread_cache::destroy_tcache(tcache_id_t id)
{ // @todo Code reuse versus flush_tcache()?
  using flow::util::this_thread_unique_token;
  using boost::system::system_category;

  assert((this_thread_unique_token() == thread_token()) && "Please don't flush/etc. across threads.");

  FLOW_LOG_INFO_LOCKED("Jem_tcache[" << *this << "]: Tcache-destruction (includes flushing) of tcache [" << id << "] "
                       "requested: proceeding.");

  const auto code = IPC_SHM_ARENA_LEND_JEMALLOC_API(mallctl)("tcache.destroy", nullptr, nullptr, &id, sizeof(id));
  if (code != 0)
  {
    const Error_code sys_err_code{code, system_category()};
    FLOW_LOG_FATAL_LOCKED("System error occurred: [" << sys_err_code << "] [" << sys_err_code.message() << "].");
    assert(false && "Tcache-destruction failed -- as of this writing we consider this catastrophic; "
                      "for some ops such as to allow for arena destruction subsequently it is essential "
                      "for flushing and/or destruction to succeed.  We don't consider this recoverable.  "
                      "@todo Revisit.");
    // Technically only flushing is required; but let's just keep it consistent: this failing is too weird.
    std::abort();
  }
  // else
} // Thread_cache::destroy_tcache()

tcache_id_t Thread_cache::id_or_none(arena_id_t arena_id) const
{
  const auto it = m_tcaches_by_arena.find(arena_id);
  return (it == m_tcaches_by_arena.end()) ? S_NO_TCACHE_ID : it->second;
}

tcache_id_t Thread_cache::id(arena_id_t arena_id)
{
  using boost::system::system_category;

  constexpr tcache_id_t DUMMY_ID = S_NO_TCACHE_ID;

  const auto it_and_did_insert = m_tcaches_by_arena.emplace(arena_id, DUMMY_ID);
  auto& id = it_and_did_insert.first->second; // This is the tcache_id_t for arena_id.

  if (it_and_did_insert.second) // I.e.: if (it was not already there; we inserted it)
  {
    const auto mallctl_or_die = [&](const char* op_name, void* oldp, size_t* oldlenp, void* newp, size_t newlen)
    {
      const auto code = IPC_SHM_ARENA_LEND_JEMALLOC_API(mallctl)(op_name, oldp, oldlenp, newp, newlen);
      if (code != 0)
      {
        const Error_code sys_err_code{code, system_category()};
        FLOW_LOG_FATAL_LOCKED("Op [" << op_name << "]: System error occurred: [" << sys_err_code << "] "
                              "[" << sys_err_code.message() << "].");
        assert(false && "Tcache-creation-related mallctl() failed -- as of this writing we consider this "
                          "catastrophic.  "
                          "@todo We could probably add contingency steps and deal with it; just for the time "
                          "being we've seen no legit reason for this to fail and decided not to spend effort "
                          "in coding and testing such contingencies.");
        std::abort();
      }
    }; // auto mallctl_or_die =

    /* `id` is the dummy-value; we must create tcache and place its actual ID there.
     *
     * (Note: jemalloc auto-associates the new tcache -- an association governing stats-filing only -- with an
     * arena chosen via a thread-internal binding that public jemalloc API cannot influence; in particular it
     * is *not* `arena_id`, and bracketing the creation with `thread.arena` writes does not change it (verified
     * in 5.3.0 source: the choice is arena_ichoose(), the *internal*-arena chooser, which `thread.arena` does
     * not touch).  Consequence for our stats documented in Memory_manager_stats::Alloc doc header.) */
    size_t output_sz = sizeof(id);
    mallctl_or_die("tcache.create", &id, &output_sz, nullptr, 0);

    FLOW_LOG_INFO_LOCKED("Jem_tcache[" << *this << "]: A tcache ID was requested for an arena but did not exist; thus: "
                         "Tcache-create succeeded; tcache [" << id << "] for arena [" << arena_id << "] ready.");
  } // if (arena_id was not in m_tcaches_by_arena)
  // else { Fast-path. }

  return id;
} // Thread_cache::id()

void Thread_cache::if_requested_destroy_tcaches_and_possibly_finish_arena_kills(bool assume_requested)
{
  using std::vector;

  // Avoid logging work, or any unneeded work of any kind, in the fast-path....

  if ((!assume_requested) && (!m_destroy_tcaches_requested_poll_flag.poll_armed()))
  {
    return;
  }
  /* else if (it was requested) || assume_requested: We will do it.
   * Unless assume_requested: It is now re-marked as not-requested. */


  // Non-fast-path is in effect (can log, etc.).

  vector<decltype(Arena_death_progress::m_on_done_func)> on_done_funcs_to_execute;
  s_state.m_arenas_death_row_map.while_locked([&](auto* arenas_death_row_map_ptr)
  {
    auto& arenas_death_row_map = *arenas_death_row_map_ptr;
    vector<arena_id_t> arenas_to_kill;

    for (auto& arena_and_progress : arenas_death_row_map)
    {
      auto& arena_death_progress = arena_and_progress.second;
      auto& caches = arena_death_progress.m_caches_left_to_destroy;
      if (caches.erase(this) == 0)
      {
        continue; // This arena's doom is (no longer?) dependent on our having destroyed a tcache of ours.
      }
      // else

      const bool fatal_destroy = caches.empty();
      const auto arena_id = arena_and_progress.first;
      const auto it = m_tcaches_by_arena.find(arena_id);
      if (it == m_tcaches_by_arena.end())
      {
        FLOW_LOG_INFO_LOCKED("Jem_tcache[" << *this << "]: Arena [" << arena_id << "] is being destroyed; "
                             "this thread-cache object was marked to destroy any relevant tcache before "
                             "arena-destruction can proceed; however that arena has not been used w/r/t tcaching "
                             "in this thread yet; therefore there is nothing to destroy, and we shall cross ourselves "
                             "off that list.  If that makes the list empty (does it? = [" << fatal_destroy << "]), "
                             "we shall imminently destroy arena.");
      }
      else
      {
        const auto id = it->second;
        FLOW_LOG_INFO_LOCKED("Jem_tcache[" << *this << "]: Arena [" << arena_id << "] is being destroyed; "
                             "this thread-cache object was marked to destroy any relevant tcache before "
                             "arena-destruction can proceed; and indeed we hold tcache [" << id << "].  "
                             "Proceeding with destruction to cross ourselves off the list.  If that makes "
                             "the list empty (does it? = [" << fatal_destroy << "]), we shall imminently "
                             "destroy arena.");
        destroy_tcache(id);
        m_tcaches_by_arena.erase(it);
      }

      if (fatal_destroy)
      {
        arenas_to_kill.push_back(arena_id);

        on_done_funcs_to_execute.emplace_back(std::move(arena_death_progress.m_on_done_func));
        arena_death_progress.m_on_done_func.clear(); // Just in case move() didn't do it.
      }
    } // (arena_and_progress : arenas_death_row_map)

    // Clean out the entries we made empty (for which we'll run on_done_func() just below, finishing the exercise).
    for (const auto arena_id : arenas_to_kill)
    {
      const auto it = arenas_death_row_map.find(arena_id);
#ifndef NDEBUG
      auto& arena_death_progress = it->second;
#endif
      assert(arena_death_progress.m_caches_left_to_destroy.empty());
      assert(arena_death_progress.m_on_done_func.empty());

      arenas_death_row_map.erase(it);
    }
  }); // s_state.m_arenas_death_row_map.while_locked()

  for (auto& on_done_func : on_done_funcs_to_execute)
  {
    on_done_func(static_cast<const Log_context_mt*>(this));
  }
} // Thread_cache::if_requested_destroy_tcaches_and_possibly_finish_arena_kills()

void Thread_cache::caches_set_logger(flow::log::Logger* logger_ptr) // Static.
{
  s_state.m_caches.set_logger(logger_ptr); // It'll propagate it to extant and future per-thread objects.
}

void Thread_cache::destroy_arena(arena_id_t arena_id, const flow::log::Log_context_mt* log_ctx) // Static.
{
  using flow::util::ostream_op_string;
  using flow::util::Lock_guard;
  using flow::util::Mutex_non_recursive;
  using flow::error::Runtime_error;
  using boost::system::system_category;

  FLOW_LOG_SET_LOCKED_CONTEXT(log_ctx);

  size_t input_sz = 0;
  int code;
  {
    /* arena.*.destroy() mutates the jemalloc arena set, so serialize it against any concurrent
     * stat-dumps, arena-create (and other arena-destroys).  See detail::jemalloc_arena_list_mutex() doc header
     * for the whole story.  Do note that if some other code does arenas.create et al directly, then this
     * won't beat it; but if they use our nice APIs then all good. */
    Lock_guard<Mutex_non_recursive> lock{detail::jemalloc_arena_list_mutex()};

    code = IPC_SHM_ARENA_LEND_JEMALLOC_API(mallctl)(ostream_op_string("arena.", arena_id, ".destroy").c_str(),
                                                    nullptr, nullptr, nullptr, input_sz);
  }

  if (code != 0)
  { // In this specific case we specifically promised to throw -- not abort as elsewhere as of this writing.
    throw Runtime_error{Error_code{code, system_category()}, "Thread_cache::destroy_arena():mallctl(arena.destroy)"};
  }
  // else
  FLOW_LOG_INFO_LOCKED("Destroyed arena [" << arena_id << "].");
} // Thread_cache::destroy_arena()

flow::util::Thread_token Thread_cache::thread_token() const { return m_thread_token; }
const std::string& Thread_cache::thread_nickname() const { return m_thread_nickname; }

Thread_cache::Static_state::Static_state() :
  m_caches(nullptr, // Logger*.  Can `Thread_cache::caches_set_logger()`, e.g., in main().
           "JemThreadCaches")
{
  Set_logger_registry::register_action([](flow::log::Logger* logger_ptr)
  {
    caches_set_logger(logger_ptr);
  });
  // Now arena_lend::set_logger(x) will, among others potentially, do our `caches_set_logger(x)`.
}

std::ostream& operator<<(std::ostream& os, const Thread_cache& val)
{
  os << "[tcaches=" << val.m_tcaches_by_arena.size() << " thread=[tok-" << val.thread_token();
  if (!val.thread_nickname().empty())
  {
    os << '|' << val.thread_nickname();
  }
  return os << "]]@" << &val;
}

} // namespace ipc::shm::arena_lend::jemalloc
