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
#include "ipc/common.hpp"
#include <flow/util/string_ostream.hpp>
#include <flow/util/util.hpp>
#include <boost/system/error_code.hpp>
#include <cassert>
#include <ostream>

namespace ipc::shm::arena_lend::jemalloc
{

// Implementations.

void* Memory_manager::allocate(size_t size) const
{
  return IPC_SHM_ARENA_LEND_JEMALLOC_API(malloc)(size); // It returns null <=> we do (as advertised).
}

void* Memory_manager::allocate_no_tcache(size_t size, arena_id_t arena_id) const
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
  // It returns null <=> we do (as advertised).
  return IPC_SHM_ARENA_LEND_JEMALLOC_API(mallocx)(size, (MALLOCX_ARENA(arena_id) | thread_cache_flags));
}

void Memory_manager::deallocate(void* address) const noexcept
{
  IPC_SHM_ARENA_LEND_JEMALLOC_API(free)(address);
}

void Memory_manager::deallocate_no_tcache(void* address) const noexcept
{
  deallocate_helper(address, MALLOCX_TCACHE_NONE);
}

void Memory_manager::deallocate(void* address, tcache_id_t thread_cache_id) const noexcept
{
  deallocate_helper(address, MALLOCX_TCACHE(thread_cache_id));
}

void Memory_manager::deallocate_helper(void* address, int thread_cache_flags) const noexcept
{
  assert(address);

  IPC_SHM_ARENA_LEND_JEMALLOC_API(dallocx)(address, thread_cache_flags);
}

arena_id_t Memory_manager::create_arena(extent_hooks_t* extent_hooks) const
{
  using flow::util::Lock_guard;
  using flow::util::Mutex_non_recursive;
  using flow::error::Runtime_error;
  using boost::system::system_category;

  arena_id_t arena_id;
  size_t output_size = sizeof(arena_id);
  extent_hooks_t** input_param;
  size_t input_size;
  if (extent_hooks)
  {
    input_param = &extent_hooks;
    input_size = sizeof(extent_hooks);
  }
  else
  {
    input_param = nullptr;
    input_size = 0;
  }

  int ec;
  {
    /* `arenas.create` mutates the jemalloc arena set, so serialize it against any concurrent
     * whole-arena-set stats dump (and against arena-create/destroy).  See detail::jemalloc_arena_list_mutex() doc
     * header for the whole story.  Do note that if some other code does arenas.create et al directly, then this
     * won't beat it; but if they use our nice API here then all good. */
    Lock_guard<Mutex_non_recursive> lock{detail::jemalloc_arena_list_mutex()};
    ec = IPC_SHM_ARENA_LEND_JEMALLOC_API(mallctl)("arenas.create", &arena_id, &output_size, input_param, input_size);
  }
  if (ec != 0)
  {
    throw Runtime_error{Error_code{ec, system_category()}, "je_mallctl() error on creating arena"};
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
    /* malloc_stats_print() refreshes jemalloc's stats and then reads many values; so firstly serialize it against
     * Flow-IPC's other stats refresh-then-read sequences, lest they tear each other.  See
     * detail::jemalloc_stats_mutex() doc header.
     *
     * Secondly: malloc_stats_print() walks the jemalloc arena set unguarded, so serialize it against any concurrent
     * arena-create and arena-destroy (which mutate that set).  See detail::jemalloc_arena_list_mutex() doc header
     * for the whole story.  Do note that if some other code does arenas.create et al directly, then this
     * won't beat it; but if they use our nice APIs then all good.  (Lock order: stats mutex, then this leaf.) */
    Lock_guard<Mutex_non_recursive> stats_lock{detail::jemalloc_stats_mutex()};
    Lock_guard<Mutex_non_recursive> lock{detail::jemalloc_arena_list_mutex()};
    IPC_SHM_ARENA_LEND_JEMALLOC_API(malloc_stats_print)
      ([](void* cbopaque, const char* chunk)
         { *(static_cast<std::ostream*>(cbopaque)) << chunk; },
       // (^-- Capture-less lambda => function pointer conversion.  Use their cbopaque dealio for access to `os`.)
       &os, opts.empty() ? nullptr : opts.c_str());
  }

  if (format == Stat_format::S_JSON)
  {
    // The JSON is very compact (no white-space... and no newline); but our contract promises a trailing newline.
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

  /* @todo Not a big deal, at least not yet, but it'd be nice to provide `ostream << Memory_manager::Stat_format`
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

flow::util::Mutex_non_recursive& jemalloc_stats_mutex()
{
  using flow::util::Mutex_non_recursive;

  // Immortal per our doc header.
  static const auto s_mutex = new Mutex_non_recursive;
  return *s_mutex;
}

} // namespace ipc::shm::arena_lend::jemalloc::detail
