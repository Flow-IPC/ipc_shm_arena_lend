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
#include "ipc/shm/arena_lend/detail/lend_tracker_pool.hpp"
#include "ipc/shm/arena_lend/arena_lend_fwd.hpp"
#include "ipc/util/util.hpp"
#include "ipc/util/detail/util.hpp"
#include "ipc/shm/classic/error.hpp"
#include "ipc/shm/classic/pool_arena.hpp"
#include <flow/util/util.hpp>
#include <limits>
#include <cstdint>

namespace ipc::shm::arena_lend::detail
{

Lend_tracker_pool::Lend_tracker_pool(const flow::log::Log_context_mt* log_ctx,
                                     const std::atomic<bool>* skip_fast_path_verbose_logging_ptr,
                                     const Shared_name& pool_name, util::Create_only,
                                     Obj_db_aux_pool_stats* stats, const util::Permissions& perms) :
  m_is_creator(true),
  m_pool_name(pool_name),
  m_log_ctx(log_ctx),
  m_skip_fast_path_verbose_logging_ptr(skip_fast_path_verbose_logging_ptr),
  m_stats(stats)
{
  using util::Permissions;
  using flow::util::stat::fetch_add;
  using flow::util::stat::update_hi_wmark;
  // using flow::util::construct_at; // C++20 => can conflict with incidentally included std:: counterpart.
  using boost::io::ios_all_saver;

  const auto POOL_SZ = Pool::segment_manager::get_min_size() + Use_count_registry::S_ASSUMED_BASE_OFFSET;

  log_while_locked([&](auto&& get_logger, auto&& get_log_component)
  {
    const auto logger_ptr = get_logger();
    if (logger_ptr && logger_ptr->should_log(flow::log::Sev::S_INFO, get_log_component()))
    {
      ios_all_saver saver{*(logger_ptr->this_thread_ostream())}; // Revert std::oct/etc. soon.
      FLOW_LOG_INFO_WITHOUT_CHECKING
        ("Lend-tracker pool (addr [" << this << "]): " // Do not print *this yet -- not until we're ready to rock.
         "Constructing heap handle to tracker pool at name [" << m_pool_name << "] in "
         "create-only (admin) mode; pool size [" << flow::util::ceil_div(POOL_SZ, size_t(1024)) << "Ki]; "
         "perms = [" << std::setfill('0') << std::setw(4) << std::oct << perms.get_permissions() << "].  "
         "Then will initialize state.");
    }

    // m_pool is null.  Try to create+open it; it may throw exception which is allowed by contract.
    util::op_with_possible_bipc_exception
      (logger_ptr, nullptr, classic::error::Code::S_SHM_BIPC_MISC_LIBRARY_ERROR, "Lend_tracker_pool(): Pool()", [&]()
    {
      m_pool.emplace(util::CREATE_ONLY, m_pool_name.native_str(), POOL_SZ, nullptr, perms);
    });
  }); // log_while_locked()

  // Great... and we are the creating guy... so get it ready for use (whether via *this or others cted via other ctor).

  // Cannot do the following due to null_index: construct<Metadata>(::ipc::bipc::anonymous_instance)().
  m_metadata = static_cast<decltype(m_metadata)>(pool_allocate(sizeof(*m_metadata)));
  flow::util::construct_at(m_metadata);

  /* This atomic<uint> m_metadata->m_n_unused is constructed via atomic<uint>{0} (see Metadata definition).
   * Thus omitting: m_metadata->m_n_unused = 0; */

  /* However m_metadata->m_unused_idx_hints is an array<atomic<use_ct_idx_t>>, and array<> elements are emphatically
   * *not* initialized.  So formally it would appear the most correct thing to do is use construct_at() on each
   * element. */
  auto& hints = m_metadata->m_unused_idx_hints;
  for (auto hint = hints.begin(); hint != hints.end(); ++hint)
  {
    flow::util::construct_at(hint, 0);
  }

  assert((m_metadata == m_pool->get_segment_manager()->get_memory_algorithm().get_metadata<Metadata>())
         && "By definition this should be that: we just made it!");

} // Lend_tracker_pool::Lend_tracker_pool(Create_only)

Lend_tracker_pool::Lend_tracker_pool(const flow::log::Log_context_mt* log_ctx,
                                     const std::atomic<bool>* skip_fast_path_verbose_logging_ptr,
                                     const Shared_name& pool_name, util::Open_only) :
  m_is_creator(false),
  m_pool_name(pool_name),
  m_log_ctx(log_ctx),
  m_skip_fast_path_verbose_logging_ptr(skip_fast_path_verbose_logging_ptr),
  m_stats(nullptr) // It's an admin-mode thing.
{
  log_while_locked([&](auto&& get_logger, auto&& get_log_component)
  {
    // m_pool is null.  Try to open it; it may throw exception which is allowed by contract.
    util::op_with_possible_bipc_exception
      (get_logger(), nullptr, classic::error::Code::S_SHM_BIPC_MISC_LIBRARY_ERROR, "Lend_tracker_pool(): Pool()", [&]()
    {
      m_pool.emplace(util::OPEN_ONLY, m_pool_name.native_str());
    });

    m_metadata = m_pool->get_segment_manager()->get_memory_algorithm().get_metadata<Metadata>();

    FLOW_LOG_INFO
      ("Lend-tracker pool [" << *this << "]: "
       "Constructed heap handle to tracker pool at name [" << m_pool_name << "] in open-only (client) mode.");
  });
} // Lend_tracker_pool::Lend_tracker_pool(Open_only)

Lend_tracker_pool::~Lend_tracker_pool()
{
  using flow::util::stat::fetch_sub;

  if (m_is_creator)
  {
    FLOW_LOG_INFO_LOCKED
      ("Lend-tracker pool [" << *this << "]: Creating/admin pool (essentially pool handle) shutting down.  This "
       "indicates no tracked-objects relevant to this pool will ever be allocated or deallocated.  Therefore "
       "(1) setting the DEAD flag to true in pool; and (2) "
       "removing pool-name [" << m_pool_name << "] from file-system (any already-opened handles will not be "
       "disrupted, though for unrelated reasons they too should be shutting down or already shut-down).");

    // See doc header "Cleanup" section and dead() doc header for explanation.
    m_metadata->m_dead.store(true);
    /* Detail: We used seq_cst (fully-synchronized) memory ordering there as advertised on dead().
     * Why do this here (and also the .load() in dead())?  Answer: Basically it means no surprises and not having
     * to think about corner cases.  Only question is, is dead() called often enough for this to affect perf.
     * Formally speaking dead() doc header says it is assumed it will not be called frequently enough for that.
     * How to guarantee that is outside Lend_tracker_pool's (our) purview. */

    // Remove SHM-pool from file-system; RAM is returned once all handles -- `Lend_tracker_pool`s -- get destroyed.
    Error_code err_code_ignored;
    log_while_locked([&](auto&& get_logger, auto&&)
    {
      shm::classic::Pool_arena::remove_persistent(get_logger(), m_pool_name, &err_code_ignored);
    });

    if (m_stats)
    {
      m_pool->get_segment_manager()->get_memory_algorithm().stats_record_at_deletion(m_stats);
    }
  }
  else
  {
    FLOW_LOG_INFO_LOCKED
      ("Lend-tracker pool [" << *this << "]: Non-creating/admin pool (essentially pool handle) shutting down.");

    assert((!m_stats)
           && "As of this writing the convention is that client-mode Lend_tracker_pool keeps no stats.  "
                "Maintenance bug in Lend_tracker_pool?");
  }
} // Lend_tracker_pool::~Lend_tracker_pool()

bool Lend_tracker_pool::dead() const
{
  return m_metadata->m_dead.load();
  // See dtor for relevant details (including why we just used seq_cst instead of a more relaxed access mode).
}

use_ct_idx_t Lend_tracker_pool::use_count_new()
{
  // using flow::util::construct_at; // C++20 => can conflict with incidentally included std:: counterpart.
  using std::exception;

  Atomic_use_ct* use_ct_ptr{};
  try
  {
    // Cannot do the following due to null_index: m_pool->construct<Atomic_use_ct>(::ipc::bipc::anonymous_instance)(1).
    use_ct_ptr = static_cast<decltype(use_ct_ptr)>(pool_allocate(sizeof(*use_ct_ptr)));
  }
  catch (const exception& exc)
  {
    FLOW_LOG_WARNING_LOCKED
      ("Lend-tracker pool [" << *this << "]: "
       "Unable to obtain a new use-count slot; appears a thread created a huge # of live objects at "
       "the same time; we cannot handle this.  Our code may need to change to allow for more slots "
       "at the cost of somewhat higher mandatory aux RAM use.  Will re-throw likely bad_alloc: "
       "[" << exc.what() << "].");
    throw;
  }
  assert(use_ct_ptr && "allocate() would have thrown (with WARNING logged), else should have returned non-null.");

  flow::util::construct_at(use_ct_ptr, 1);

  const auto idx = use_ct_ptr_to_idx(use_ct_ptr);
  if (!skip_fast_path_verbose_logging())
  {
    FLOW_LOG_TRACE_LOCKED("Lend-tracker pool [" << *this << "]: "
                          "New object presumably constructed: new use-count `1` registered at idx [" << idx << "], "
                          "process-local addr [" << use_ct_ptr << "] off base addr [" << m_metadata << "].");
  }
  // else { Skip ~any cycle use on logging along fast-path. }

  return idx;
} // Lend_tracker_pool::use_count_new()

unsigned int Lend_tracker_pool::use_count_inc(use_ct_idx_t use_ct_idx)
{
  auto const use_ct_ptr = use_ct_idx_to_ptr(use_ct_idx);
  auto use_ct
    = static_cast<unsigned int>(use_ct_ptr->fetch_add(1, std::memory_order_relaxed));

  assert((use_ct != 0)
         && "Use-count starts at 1; and on reaching 0 must never be touched again.  Bug?");

  // (There's a minor, very unlikely problem here; see discussion at the end of function.)
  constexpr auto MAX_USE_CT = std::numeric_limits<Atomic_use_ct::value_type>::max();
  if (use_ct == MAX_USE_CT)
  {
    FLOW_LOG_FATAL_LOCKED("Lend-tracker pool [" << *this << "]: "
                          "Object use-count INCremented from [" << use_ct << "] at idx [" << use_ct_idx << "], "
                          "process-local addr [" << use_ct_ptr << "] off base addr [" << m_metadata << "]; "
                          "overflow!  This means you've lent -- and held simultaneously without dropping by receiving "
                          "process -- this object that many times, plus one.  Either reconsider your use case (are you "
                          "lending many times to one process? if so can you detect this and simply copy the shared_ptr "
                          "instead?); or modify Use_count_registry::S_ALLOC_SZ and Lend_tracker_pool::Atomic_use_ct "
                          "definitions to handle even more lending.  Aborting.");
    assert(false && "Object use-count INCremented from the max-value of Atomic_use_ct::value_type; "
                      "overflow!  This means you've lent -- and held simultaneously without dropping by receiving "
                      "process -- this object that many times, plus one.  Either reconsider your use case (are you "
                      "lending many times to one process? if so can you detect this and simply copy the shared_ptr "
                      "instead?); or modify Use_count_registry::S_ALLOC_SZ and Lend_tracker_pool::Atomic_use_ct "
                      "definitions to handle even more lending.  Aborting.");
    std::abort();
  }
  // else

  ++use_ct;

  if (!skip_fast_path_verbose_logging())
  {
    FLOW_LOG_TRACE_LOCKED("Lend-tracker pool [" << *this << "]: "
                          "Object use-count INCremented to [" << use_ct << "] at idx [" << use_ct_idx << "], "
                          "process-local addr [" << use_ct_ptr << "] off base addr [" << m_metadata << "].");
  }
  // else { Skip ~any cycle use on logging along fast-path. }

  return use_ct;

  /* @todo (Possibly) The above code that checks for overflow is nice and elegant, but it comes with a caveat:
   * The post-increment ->fetch_add(1) that returns MAX_USE_CT will cause an ~instant crash, at which point loosely
   * speaking all bets are off.  However, the corrupt value (0) would still be written.  This in itself wouldn't
   * dealloc that slot; but a concurrent scan in an admin-mode other Lend_tracker_pool (assuming we are another
   * thread => client-mode Lend_tracker_pool trying to lend => trying to use_count_inc()) could (assuming the
   * hint mechanism + n_unused() don't prevent it) do use_count_return_if_unused(), detect the corrupt-0 value,
   * dealloc, return true, destroy object (general mayhem).  This would be racing against the abort() which would
   * end all this in more civilized (relatively) fashion.  In this extremely unlikely -- even given how unlikely we
   * feel the overflow itself is -- scenario, the result probably would still look like a crash of some kind...
   * probably... or maybe some other madness (but soon followed by the abort() here).
   *
   * A suggested fix, roughly:
   *   do
   *   {
   *     if (use_ct == MAX_USE_CT) { ...log/assert/abort()... }
   *     new_use_ct = use_ct + 1;
   *   }
   *   while (!use_ct_ptr->compare_exchange_weak(use_ct, new_use_ct, std::memory_order_relaxed));
   * This should avoid storing the bad value while remaining close to equally as fast in terms of the instructions
   * generated.  However it is somewhat precious given the actual danger involved.  Still, it should guard against
   * any theoretical entropy present in the existing code.  Reconsider at some point; for now we don't rock the boat. */
} // Lend_tracker_pool::use_count_inc()

unsigned int Lend_tracker_pool::use_count_dec(use_ct_idx_t use_ct_idx)
{
  using std::find_if;

  auto const use_ct_ptr = use_ct_idx_to_ptr(use_ct_idx);
  auto use_ct
    = static_cast<unsigned int>(use_ct_ptr->fetch_sub(1, std::memory_order_relaxed));

  assert((use_ct != 0) && "Use-count on reaching 0 must never be touched again... yet has become negative.  Bug?");

  --use_ct;

  decltype(Metadata::m_n_unused)::value_type n_unused{};
  decltype(Metadata::m_unused_idx_hints)::iterator found_hint_ptr{};
  if (use_ct == 0)
  {
    n_unused = m_metadata->m_n_unused.fetch_add(1, std::memory_order_relaxed);

    // (Log about it below.)

    auto& hints = m_metadata->m_unused_idx_hints;
    use_ct_idx_t to_replace;

    /* At least in gcc-9 STL, there's a random-accessor-iterator loop-unroll specialization of find_if(), so this
     * can actually help perf in this search, versus a manual `for` and the like. */
    ++use_ct_idx; // 0 is used as a special value, so add 1 before saving in hint array.
    found_hint_ptr = find_if(hints.begin(), hints.end(), [&](auto& hint)
    {
      to_replace = 0; // It might be overwritten by ->compare_exchange_strong(), so have to keep reassigning.
      if (hint.compare_exchange_strong(to_replace, use_ct_idx, std::memory_order_relaxed))
      {
        return true;
      }
      // else
      return false;
    });

    // Either we saved it and are done (and will log below), or it was full and are done (and will not so log).
  } // if (use_ct == 0)
  // else if (use_ct > 0) { That's it for now; festivities begin once another use_count_dec() gets it to zero. }

  // Combine all fast-path logging in one code area for perf.
  if (!skip_fast_path_verbose_logging())
  {
    log_while_locked([&](auto&& get_logger, auto&& get_log_component)
    {
      const auto logger_ptr = get_logger();
      if (logger_ptr && logger_ptr->should_log(flow::log::Sev::S_TRACE, get_log_component()))
      {
        FLOW_LOG_TRACE_WITHOUT_CHECKING
          ("Lend-tracker pool [" << *this << "]: "
           "Object use-count DECremented to [" << use_ct << "] at idx [" << (use_ct_idx - 1) /* <- was ++ed */ << "], "
           "process-local addr [" << use_ct_ptr << "] off base addr [" << m_metadata << "].  If "
           "reached zero will record this in pool metadata area presently.");
        if (use_ct == 0)
        {
          FLOW_LOG_TRACE_WITHOUT_CHECKING
            ("Yes, reached zero: count of all such (unused-object-not-yet-deleted) slots is now INCremented to "
             "[" << (n_unused + 1) << "]; recorded in pool metadata area.  Communicated this fact "
             "via hint array, if possible.");
          const auto& hints = m_metadata->m_unused_idx_hints;
          if (found_hint_ptr != hints.end())
          {
            FLOW_LOG_TRACE_WITHOUT_CHECKING
              ("Recorded index of unused-object-not-yet-deleted slot in hint-array slot "
               "[" << (found_hint_ptr - hints.begin()) << "] (0-based) of "
               "[" << decltype(Metadata::m_unused_idx_hints)::static_size << "] (1-based).");
          }
        }
      } // if (TRACE-logging on)
    }); // log_while_locked()
  } // if (!skip_fast_path_verbose_logging())
  // else { Skip ~any cycle use on logging along fast-path. }

  return use_ct;
} // Lend_tracker_pool::use_count_dec()

unsigned int Lend_tracker_pool::use_count_dec_admin(use_ct_idx_t* use_ct_idx_ptr)
{
  using std::find_if;

  auto& use_ct_idx = *use_ct_idx_ptr;
  auto const use_ct_ptr = use_ct_idx_to_ptr(use_ct_idx);
  auto use_ct
    = static_cast<unsigned int>(use_ct_ptr->fetch_sub(1, std::memory_order_relaxed));

  assert((use_ct != 0) && "Use-count on reaching 0 must never be touched again... yet has become negative.  Bug?");

  --use_ct;

  if (!skip_fast_path_verbose_logging())
  {
    FLOW_LOG_TRACE_LOCKED
      ("Lend-tracker pool [" << *this << "]: "
       "Object use-count DECremented admin-style to [" << use_ct << "] at idx [" << use_ct_idx << "], "
       "process-local addr [" << use_ct_ptr << "] off base addr [" << m_metadata << "].  If "
       "reached zero will (admin-style!) return this slot of use-count immediately.  "
       "(If so then presumably caller, as admin, can finish the tracked-object's existence immediately, so "
       "no need to record this use-ct=0 in pool metadata.)");
  }
  // else { Skip ~any cycle use on logging along fast-path. }

  if (use_ct == 0)
  {
    use_count_return_bc_unused(use_ct_idx_ptr,
                               // We never incremented m_n_unused -- and must not decrement it!  Admin-style.
                               true);
  }

  return use_ct;
} // Lend_tracker_pool::use_count_dec_admin()

bool Lend_tracker_pool::use_count_return_if_unused(use_ct_idx_t* use_ct_idx_ptr)
{
  auto& use_ct_idx = *use_ct_idx_ptr;
  auto const use_ct_ptr = use_ct_idx_to_ptr(use_ct_idx);
  if (use_ct_ptr->load(std::memory_order_relaxed) != 0)
  {
    if (!skip_fast_path_verbose_logging())
    {
      FLOW_LOG_TRACE_LOCKED("Lend-tracker pool [" << *this << "]: Object would be deleted if use-count were 0: "
                            "but it is not; ignoring; registered at idx [" << use_ct_idx << "], "
                            "process-local addr [" << use_ct_ptr << "] off base addr [" << m_metadata << "].");
    }
    // else { Skip ~any cycle use on logging along fast-path. }
    return false;
  }
  // else

  use_count_return_bc_unused(use_ct_idx_ptr,
                             // Non-admin-style: having earlier reached 0, m_n_unused was incremented; so decrement it.
                             false);
  return true;
} // Lend_tracker_pool::use_count_return_if_unused()

void Lend_tracker_pool::use_count_return_bc_unused(use_ct_idx_t* use_ct_idx_ptr, bool was_not_counted_as_unused)
{
  auto& use_ct_idx = *use_ct_idx_ptr;
  auto const use_ct_ptr = use_ct_idx_to_ptr(use_ct_idx);

  if (was_not_counted_as_unused)
  {
    if (!skip_fast_path_verbose_logging())
    {
      FLOW_LOG_TRACE_LOCKED("Lend-tracker pool [" << *this << "]: Object presumably being deleted: "
                            "returning slot of use-count (by contract equal to 0) registered at idx "
                            "[" << use_ct_idx << "], process-local addr [" << use_ct_ptr << "] off base addr "
                            "[" << m_metadata << "].  Admin-style, count of all unused-object-not-yet-deleted slots "
                            "was *neither* incremented nor decremented.");
    }
    // else { Skip ~any cycle use on logging along fast-path. }
  }
  else // if (!was_not_counted_as_unused)
  {
    auto n_unused = m_metadata->m_n_unused.fetch_sub(1, std::memory_order_relaxed);

    if (!skip_fast_path_verbose_logging())
    {
      FLOW_LOG_TRACE_LOCKED("Lend-tracker pool [" << *this << "]: Object presumably being deleted: "
                            "returning slot of use-count (by contract equal to 0) registered at idx "
                            "[" << use_ct_idx << "], process-local addr [" << use_ct_ptr << "] off base addr "
                            "[" << m_metadata << "].  Count of all unused-object-not-yet-deleted slots is now "
                            "DECremented to [" << (n_unused - 1) << "]; recorded in pool metadata area.");
    }
    // else { Skip ~any cycle use on logging along fast-path. }
  } // else if (!was_not_counted_as_unused)

  // Cannot do the following due to null_index: m_pool->destroy_ptr(use_ct_ptr).
  use_ct_ptr->~Atomic_use_ct();
  m_pool->deallocate(use_ct_ptr);

  use_ct_idx = 0;
} // Lend_tracker_pool::use_count_return_bc_unused()

unsigned int Lend_tracker_pool::use_count(use_ct_idx_t use_ct_idx) const
{
  return use_ct_idx_to_ptr(use_ct_idx)->load(std::memory_order_relaxed);
}

use_ct_idx_t Lend_tracker_pool::use_ct_ptr_to_idx(const Atomic_use_ct* use_ct_ptr) const
{
  constexpr auto USE_CT_SZ = static_cast<use_ct_idx_t>(sizeof(Atomic_use_ct));
  return static_cast<use_ct_idx_t>(reinterpret_cast<uintptr_t>(use_ct_ptr)
                                   - reinterpret_cast<uintptr_t>(m_metadata))
         / USE_CT_SZ;
}

Lend_tracker_pool::Atomic_use_ct* Lend_tracker_pool::use_ct_idx_to_ptr(use_ct_idx_t use_ct_idx) const
{
  constexpr auto USE_CT_SZ = static_cast<use_ct_idx_t>(sizeof(Atomic_use_ct));
  return reinterpret_cast<Atomic_use_ct*>
           (reinterpret_cast<uintptr_t>(m_metadata)
            + (use_ct_idx * USE_CT_SZ));
}

unsigned int Lend_tracker_pool::n_unused() const
{
  return m_metadata->m_n_unused.load(std::memory_order_relaxed);
}

void* Lend_tracker_pool::pool_allocate(size_t sz)
{
  /* Basically we just want to forward to m_pool->allocate(sz) which really forwards to
   * Use_count_registry::allocate().  However, to get nice but performant stats in m_stats, follow
   * the protocol documented on Use_count_registry::stats_record().  (See also dtor + stats_record_at_deletion().)
   *
   * @todo Use_count_registry::stats_record() doc header "Rationale" section explains why we do this; but upon
   * gazing at the below 5-line dance, just to get the stats to work, maybe it would be after all better to
   * access mem_algo.allocate() directly and have it take m_stats and just update it as-needed based on
   * its own, local, tracking of what is now accessed by us via stat_quanta_active().  It would at least produce
   * shorter code; as for total perf impact... probably small.  Though it has not as of this writing been profiled. */
  if (m_stats)
  {
    auto& mem_algo = m_pool->get_segment_manager()->get_memory_algorithm();
    const auto qta = mem_algo.stat_quanta_active();
    const auto ret = m_pool->allocate(sz);
    mem_algo.stats_record(qta, m_stats); // To restate some of aforementioned doc header: almost always no-op.
    return ret;
  }
  // else
  return m_pool->allocate(sz);
}

void Lend_tracker_pool::update_log_context(const flow::log::Log_context_mt* log_ctx,
                                           const std::atomic<bool>* skip_fast_path_verbose_logging_ptr)
{
  m_log_ctx = log_ctx;
  m_skip_fast_path_verbose_logging_ptr = skip_fast_path_verbose_logging_ptr;
}

bool Lend_tracker_pool::skip_fast_path_verbose_logging() const
{
  return m_skip_fast_path_verbose_logging_ptr->load(std::memory_order_relaxed);
}

std::ostream& operator<<(std::ostream& os, const Lend_tracker_pool& val)
{
  constexpr auto USE_CT_SZ = sizeof(Lend_tracker_pool::Atomic_use_ct);

  return os << (val.m_is_creator ? "adm" : "cli")
            << "[sh_name[" << val.m_pool_name << "] unused-cts[" << val.n_unused() << "] use-cts-used/free"
               "[" << (((Use_count_registry::S_USE_COUNTS_CAPACITY * USE_CT_SZ) - val.m_pool->get_free_memory())
                       / USE_CT_SZ)
            << '/'
            << (val.m_pool->get_free_memory() / USE_CT_SZ) << "]]@" << &val;
}

} // namespace ipc::shm::arena_lend::detail
