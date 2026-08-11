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
#include "ipc/shm/arena_lend/jemalloc/memory_manager.hpp"
#include "ipc/shm/arena_lend/jemalloc/detail/jemalloc.hpp"
#include "ipc/shm/arena_lend/jemalloc/detail/jemalloc_fwd.hpp"
#include "ipc/shm/arena_lend/detail/shm_pool_offset_ptr_data.hpp"
#include "ipc/common.hpp"
#include <flow/util/string_ostream.hpp>
#include <flow/util/util.hpp>
#include <limits>
#include <ostream>

namespace ipc::shm::arena_lend::jemalloc
{

// Implementations.

void* Memory_manager::allocate(size_t size) const
{
  return IPC_SHM_ARENA_LEND_JEMALLOC_API(malloc)(size);
}

void* Memory_manager::allocate(size_t size, arena_id_t arena_id) const
{
  return allocate_helper(size, arena_id, MALLOCX_TCACHE_NONE);
}

void* Memory_manager::allocate(size_t size, arena_id_t arena_id, tcache_id_t thread_cache_id) const
{
  return allocate_helper(size, arena_id, MALLOCX_TCACHE(thread_cache_id));
}

void* Memory_manager::allocate_helper(size_t size, arena_id_t arena_id, int thread_cache_flags) const
{
  assert(size > 0);
  return IPC_SHM_ARENA_LEND_JEMALLOC_API(mallocx)(size, (MALLOCX_ARENA(arena_id) | thread_cache_flags));
}

void Memory_manager::deallocate(void* address) const
{
  IPC_SHM_ARENA_LEND_JEMALLOC_API(free)(address);
}

void Memory_manager::deallocate(void* address, arena_id_t arena_id) const
{
  deallocate_helper(address, arena_id, MALLOCX_TCACHE_NONE);
}

void Memory_manager::deallocate(void* address, arena_id_t arena_id, tcache_id_t thread_cache_id) const
{
  deallocate_helper(address, arena_id, MALLOCX_TCACHE(thread_cache_id));
}

void Memory_manager::deallocate_helper(void* address, arena_id_t arena_id, int thread_cache_flags) const
{
  assert(address);

  IPC_SHM_ARENA_LEND_JEMALLOC_API(dallocx)(address, (MALLOCX_ARENA(arena_id) | thread_cache_flags));
}

arena_id_t Memory_manager::create_arena(extent_hooks_t* extent_hooks) const
{
  using arena_lend::detail::Shm_pool_offset_ptr_data_base;
  using flow::util::ostream_op_string;
  using flow::util::Lock_guard;
  using flow::util::Mutex_non_recursive;
  using flow::error::Runtime_error;
  using boost::system::system_category;
  using std::numeric_limits;

  arena_id_t arena_id;

  {
    size_t output_size = sizeof(arena_id);
    extent_hooks_t** input_param;
    size_t input_size;
    if (!extent_hooks)
    {
      input_param = nullptr;
      input_size = 0;
    }
    else
    {
      input_param = &extent_hooks;
      input_size = sizeof(extent_hooks);
    }

    int ec;
    {
      /* `arenas.create` mutates the jemalloc arena set, so serialize it against any concurrent
       * whole-arena-set stats dump (and against arena-destroy).  See detail::jemalloc_arena_list_mutex() doc header
       * for the whole story.  Do note that if some other code does arenas.create et al directly, then this
       * won't beat it; but if they use our nice API here then all good. */
      Lock_guard<Mutex_non_recursive> lock{detail::jemalloc_arena_list_mutex()};
      ec = IPC_SHM_ARENA_LEND_JEMALLOC_API(mallctl)("arenas.create",
                                                    &arena_id, &output_size,
                                                    input_param, input_size);
    }
    if (ec != 0)
    {
      throw Runtime_error{Error_code{ec, system_category()}, "je_mallctl() error on creating arena"};
    }
  }

#ifndef FLOW_OS_LINUX
static_assert(false, "The logic below might be Linux-specific and has only been tested in Linux as of this writing.");
#endif
  /* (This comment applies to Linux; for other OS, when supported, the same concepts may apply, but we
   * should check it and reword as needed, when this happens.)
   *
   * Background for the next thing: jemalloc grows its mapped vaddr footprint via
   * a series of extent_alloc requests -- each new extent (in our setup: each new
   * SHM-pool) is sized progressively larger than the last to amortize per-mmap() cost and
   * reduce the frequency of future grow events.  The retain_grow_limit option caps the size
   * of each such grow step; i.e., it directly limits the size of any individual extent jemalloc will
   * request from us.  (The "retain" in the name is historical: the geometric-grow machinery
   * pairs naturally with the *retain* feature -- enabled via `retain` option, on by default for
   * 64-bit Linux -- but retain_grow_limit caps each such grow request.)  Default on 64-bit Linux
   * is no limit.
   *
   * Why we set it:
   *   - Hard correctness bound.  In Flow-IPC-SHM-jemalloc each jemalloc extent is backed
   *     by a real SHM-pool, and our custom-pointer representation cannot encode addresses
   *     within a pool larger than <see below> (as of this writing ~2Gi bytes).  retain_grow_limit
   *     must therefore be at most that size, or larger extents would yield un-encodable pointers
   *     (observed in the field; now it won't happen).
   *   - Defensive bound against pathologically-large pools.  SHM objects are subject to
   *     OS-imposed size/count limits that ordinary heap mappings (jemalloc's "no limit"
   *     default audience) are not.  Without a cap, a single arena can grow a pool to many
   *     GBs of vaddr/SHM-pool size even when RAM-resident usage is much smaller -- observed in the
   *     field, in fact, prompting the original choice of this value.
   *
   * The current limit is reasonable for both of these purposes.
   *
   * An added note on this: This is opportunistic, as it's not about this knob itself per se, but
   * the topic has caused enough confusion in the past to where the chance of helping preempt further
   * such confusion makes the opportunism worthwhile:
   *   - This knob, retain_grow_limit, is not really about whether, or how aggressively, the
   *     seemingly-relatedly-named `retain`-flag-knob-controlled feature mmap()s or munmap()s (for us:
   *     creates/destroys SHM-pools) vaddr areas (extents).  It's just the cap of how large
   *     an extent (SHM-pool) is allowed to be.  It's in effect regardless of the `retain` flag-option.
   *   - This knob, retain_grow_limit, is also unrelated to the stat `stats.*.retained`, which counts
   *     vaddr space that jemalloc has *retained*: excluded from the `stats.*.mapped` accounting yet kept
   *     mapped -- specifically *not* munmap()ed/returned to the OS -- saved for cheap later reuse (no
   *     memory-map op needed; just hand the range back out).  There is only that one kind of retention:
   *     "we could munmap() now, but we keep the mapping and reuse the space when demand rises."  In
   *     particular there is no unmapped-but-remembered-vaddr state, nor any mechanism wherein jemalloc asks
   *     to memory-map at a previously-freed address.  (In our arenas the retained state is entered when our
   *     extent-dalloc hook *declines* extent removal -- see Ipc_arena::optional_remove_shm_pool() -- at
   *     which point the physical pages are released via SHM-object hole-punching, while the vaddr range and
   *     its SHM-pool live on.)  And, again, all this is basically orthogonal to retain_grow_limit despite
   *     the name. */

  {
    constexpr size_t SLACK_SZ = 4 * 1024;
    unsigned int exponent_of_2 = Shm_pool_offset_ptr_data_base::S_N_POOL_OFFSET_BITS;
    if constexpr(numeric_limits<Shm_pool_offset_ptr_data_base::pool_offset_t>::is_signed)
    {
      --exponent_of_2;
    }
    size_t input_param = (size_t(1) << exponent_of_2) - SLACK_SZ; // A/k/a: 2^exponent_of_2 - SLACK_SZ.

    const int ec
      = IPC_SHM_ARENA_LEND_JEMALLOC_API(mallctl)(ostream_op_string("arena.", arena_id, ".retain_grow_limit").c_str(),
                                                 nullptr, nullptr, &input_param, sizeof(input_param));
    if (ec != 0)
    {
      throw Runtime_error{Error_code{ec, system_category()}, "je_mallctl() error on setting retain_grow_limit"};
    }
  }

  return arena_id;
} // Memory_manager::create_arena()

void Memory_manager::destroy_arena(arena_id_t arena_id, const flow::log::Log_context_mt* log_ctx) const
{
  destroy_arena(arena_id, log_ctx, [](auto&&, auto&& destroy_arena_func)
  {
    destroy_arena_func();
  });
  // Incidentally: that overload is aware of the detail::jemalloc_arena_list_mutex() issue and will do the right thing.
}

void Memory_manager::stats_dump_to_ostream(std::ostream& os, Stat_format format, util::String_view extra_opts) const
{
  using flow::util::Lock_guard;
  using flow::util::Mutex_non_recursive;
  using std::string;

  /* jemalloc's malloc_stats_print() drives output via a write-callback invoked repeatedly with NUL-terminated
   * chunks; route those straight to `os`.  opts: null => human-readable text; "J" => JSON; plus any caller
   * section flags appended via extra_opts. */
  string opts{extra_opts};
  if (format == Stat_format::S_JSON)
  {
    opts += 'J';
  }

  {
    /* malloc_stats_print() mutates the jemalloc arena set, so serialize it against any concurrent
     * arena-create, arena-destroy (and other stat-dumps).  See detail::jemalloc_arena_list_mutex() doc header
     * for the whole story.  Do note that if some other code does arenas.create et al directly, then this
     * won't beat it; but if they use our nice APIs then all good. */
    Lock_guard<Mutex_non_recursive> lock{detail::jemalloc_arena_list_mutex()};
    IPC_SHM_ARENA_LEND_JEMALLOC_API(malloc_stats_print)
      ([](void* cbopaque, const char* chunk)
         { *(static_cast<std::ostream*>(cbopaque)) << chunk; },
       // (^-- Capture-less lambda => function pointer conversion.  Use their cbopaque dealio for access to `os`.)
       &os, opts.empty() ? nullptr : opts.c_str());
  }

  if (format == Stat_format::S_JSON)
  {
    /* In 5.3.0 the JSON is very compact (no white-space... and no newline).  In 5.2.1 this adds an extra newline
     * which isn't amazing, but lacking one in 5.3.0+ is worse (and technically against our contract). */
    os << '\n';
  }
}

std::string Memory_manager::stats_dump_to_string(Stat_format format, util::String_view extra_opts) const
{
  flow::util::String_ostream os;
  stats_dump_to_ostream(os.os(), format, extra_opts);
  os.os() << std::flush;
  return os.str();
}

void Memory_manager::log_stats_dump(flow::log::Logger* logger_ptr, Stat_format format,
                                    flow::log::Sev sev, util::String_view extra_opts) const
{
  FLOW_LOG_SET_CONTEXT(logger_ptr, Log_component::S_SHM);
  /* Single (multi-line) message; the dump is built (and run) only if `sev` passes the filter.
   * With or without J[SON] option it ends in newline, so let's not add more in that spot. */
  FLOW_LOG_WITH_CHECKING(sev,
                         "jemalloc stats dump (knobs: "
                           "[as-" << ((format == Stat_format::S_JSON) ? "JSON" : "text") << ", extra-opts"
                           "[" << extra_opts << "]]): "
                           "STATS_DUMP[[[\n" << stats_dump_to_string(format, extra_opts) << "]]]STATS_DUMP.");

  /* @todo Not a big deal, at least not yet, but it it'd be nice to provide `ostream << Memory_manager::Stat_format`
   * as well as istream>> (using flow::util::istream_to_enum()).  Could then just output `format` above and
   * elsewhere; and it could be used in flow.cfg (and/or naked program_options and similar) options/settings
   * directly.  Its being an inner-class type would complicate putting those things in _fwd.hpp, so perhaps
   * it would also become jemalloc::Memory_manager_stat_format... though that would be a breaking change; so
   * maybe just forget about _fwd.hpp (an exception).
   *
   * Its use currently is so limited, that it seemed like overkill, and that may well remain the case. */
}

} // namespace ipc::shm::arena_lend::jemalloc

namespace ipc::shm::arena_lend::jemalloc::detail
{

// Implementations.

flow::util::Mutex_non_recursive& jemalloc_arena_list_mutex()
{
  using flow::util::Mutex_non_recursive;

  // Immortal per our doc header.
  static const auto s_mutex = new Mutex_non_recursive;
  return *s_mutex;
}

} // namespace ipc::shm::arena_lend::jemalloc::detail
