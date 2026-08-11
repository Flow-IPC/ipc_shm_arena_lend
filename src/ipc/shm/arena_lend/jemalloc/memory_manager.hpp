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

#include "ipc/shm/arena_lend/memory_manager.hpp"
#include "ipc/shm/arena_lend/jemalloc/jemalloc_fwd.hpp"
#include "ipc/shm/arena_lend/jemalloc/thread_cache.hpp"
#include "ipc/util/util_fwd.hpp"
#include <flow/error/error.hpp>
#include <flow/log/log.hpp>
#include <flow/util/string_view.hpp>
#include <jemalloc/jemalloc.h>
#include <cstddef>
#include <iostream>
#include <string>

// Macros.

#ifdef FLOW_DOXYGEN_ONLY // Compiler ignores; Doxygen sees.
/**
 * Macro (preprocessor symbol) to set to the integer literal `1` (one) if and only if
 * the library containing SHM-jemalloc was linked to the memory-manager jemalloc of version predating the fix to the
 * concurrent-arena bug as described in jemalloc::Memory_manager doc header; namely any jemalloc<5.3.0.
 *
 * @see jemalloc::Memory_manager doc header for all relevant details, including the effect that setting this
 *      macro to `1` (<=> jemalloc with bug linked <=> jemalloc<5.3.0 linked) shall have.
 *
 * @internal
 *
 * This should be `#if`ed only in .cpp code.  In .hpp (templates, etc.) it would mean we have to rejigger the
 * build process, so that it is defined and correctly set in all compiled code.  (See build script(s), where
 * we currently set/don't set this, for more detail.)
 */
#  define IPC_SHM_ARENA_LEND_JEMALLOC_CONCURRENT_ARENAS_UNSAFE value_for_exposition
#endif

namespace ipc::shm::arena_lend::jemalloc
{

// Types.

/**
 * Wrapper around jemalloc.  This is a thin wrapper around the jemalloc API.
 *
 * @warning There is a jemalloc-version-dependent bug (in jemalloc); it is present before (not including)
 *          jemalloc-5.3.0.  In short: the following operations are known to result in undefined behavior (crashing
 *          and failure to setup Ipc_arena respectively): (1) *while a jemalloc arena (including in Ipc_arena) is
 *          being destroyed* stats-consumption as string-dump (Memory_manager::log_stats_dump(),
 *          Memory_manager::stats_dump_to_ostream(), Memory_manager::stats_dump_to_string()) or via
 *          curated stats access from jemalloc (via Ipc_arena::memory_manager_stats()); (2) two-or-more arenas
 *          (including `Ipc_arena`s) being created concurrently (multi-threads).  See more commentary below
 *          this comment block in the code.
 *
 * Arena-concurrency jemalloc bug discussion
 * -----------------------------------------
 * ### What it is ###
 * To continue about the above warning: First we note we have not tested or dealt with jemalloc<5.2.1; formally
 * the build requires at least that version.  As for 5.2.1, it has the bug; and it was fixed in 5.3.0.
 *
 * Looking at https://github.com/jemalloc/jemalloc/blob/5.3.0/ChangeLog we see:
 * May 6, 2022, in the bug-fixes list:
 * "Fix the locking on the arena destroy `mallctl`, which could cause concurrent arena creations to fail."
 *
 * In jemalloc source code `arena_i_destroy_ctl()` in `src/ctl.c` is the scene.  In 5.2.1 there is no mutex
 * lock/unlock of `ctl_mtx`; in 5.3.0 there is.  (This is at least part of the fix.)  As noted in the change-log
 * one result is failure to create an arena concurrently with creating another (call it problem A).  A problem
 * we detected via Flow-IPC/SHM-jemalloc is that attempting to read stats during arena destruction -- even if
 * interested in a specific set of stats for a live arena -- can reliably lead
 * to ugly crashing (call it problem B).  Problem A is *usually* not an issue; with or without Flow-IPC, it is
 * not typical to be setting up two arenas concurrently (e.g., if using ipc::session, one would need to accept
 * -- or even less likely, connect -- two sessions at the same time in two threads; it is normal to either
 * have one session only, as session-client, or to have a thread handling incoming session-connects in series).
 * Problem B can be avoided by generally querying jemalloc-stats (see APIs above) at session steady-state:
 * when sessions are operating, not at setup time.  This is also pretty typical.
 *
 * Nevertheless, both can be sources of instability, if (1) one has not upgraded to jemalloc-5.3.0+ *and* (2) one
 * has not heeded the above warnings.  Probably problem B is the likelier one to cause trouble.
 *
 * ### How Flow-IPC/SHM-jemalloc handles it ###
 * With jemalloc-5.3.0 there is no issue.
 *
 * With lower versions, the official build as of this writing by default refuses to build.  If it were forced to
 * build anyway, the following would happen:
 *   -# Problem A could happen, under the aforementioned use patterns on your part (fairly unlikely).
 *   -# Problem B -- ditto... but somewhat more likely... but avoidable.
 *   -# Flow-IPC itself would *potentially* trigger problem B: ipc::session::arena_lend::jemalloc::Session hierarchy,
 *      which is (while optional) recommended as the best/easiest way to setup SHM-jemalloc arenas/sessions,
 *      periodically queries Memory_manager::memory_manager_stats(): at least to make certain `_hi_wmark`
 *      stat-members (in arena_lend::stat::Memory_manager_stats `struct`) higher-resolution.
 *      Ipc_arena, near shutdown, also outputs these stats as a nicety.  If (another)
 *      arena is shutting down around the same time, it can trigger a crash.
 *
 * While bullets 1 and 2 are out of our hands (reminder: this is all assuming jemalloc<5.3.0; otherwise all cool),
 * we avoid bullet 3 as follows.  The official build, if forced via the build script(s) to build against
 * jemalloc<5.3.0, defines macro `IPC_SHM_ARENA_LEND_JEMALLOC_CONCURRENT_ARENAS_UNSAFE=1`.  SHM-jemalloc, on
 * detecting this, avoids both danger spots: periodic jemalloc-stat-grabbing and on-arena-shutdown stat-logging.
 * That's basically fine, though the `_hi_wmark` members will sample only at stat-consumption time, so if
 * they need to be of decent value, then you (the user) will need to stat-consume periodically yourself (while
 * avoiding arena-destruction concurrently).  (Generally it should be possible to survive without the `_hi_wmark`s.)
 *
 * That said: it's really best to use jemalloc-5.3.0+ and stop worrying.
 *
 * ### Similar but different issue: stats_dump_to_ostream() et al ###
 * We offer a thin wrapper for jemalloc's stats-dump-to-string dump feature (`malloc_stats_print()`):
 * stats_dump_to_ostream(), stats_dump_to_string(), log_stats_dump().  It turns out that `malloc_stats_print()`
 * is likely to crash (abort with a message like `<jemalloc>: Failure in xmallctlbymib()` and variations), if
 * concurrently an arena is being destroyed or created.  *This is not fixed, or counted as a bug, as of jemalloc-5.3.1*
 * (4/2026).
 *
 * SHM-jemalloc works around it (internally by using a process-wide mutex), so as long as one uses our APIs
 * (Flow-IPC generally for SHM-jemalloc arena control as needed, naturally; Ipc_arena::info_dump() or
 * `Memory_manager::*stats_dump*()` for stats-dumping), one is safe from this problem.  Just be warned that using
 * these jemalloc APIs directly (for whatever reason) can lead to trouble, as direct calls will not know to
 * collaborate with Flow-IPC's use of the internal mutex; and Flow-IPC may make its own calls to all 3 jemalloc
 * APIs at various times.
 */
class Memory_manager :
  public arena_lend::Memory_manager
{
public:
  // Types.

  /**
   * Wrapper around extent hooks, which are callbacks that can be instituted in jemalloc per arena.  The
   * prescribed usage is to implement desired callbacks that execute the implementation within the creating
   * object.  The `extent_hooks` callback parameter can be cast to this type to retrieve the owner of the
   * callback for execution in that class's context.
   *
   * @tparam T
   *         Any type; see get_owner().
   */
  template<typename T>
  class Extent_hooks_wrapper :
    public extent_hooks_t
  {
  public:
    // Constructors/destructor.

    /**
     * Constructor.
     *
     * @param extent_hooks
     *        The set of callback functions.
     * @param owner
     *        See get_owner().
     */
    Extent_hooks_wrapper(extent_hooks_t&& extent_hooks, T* owner);

    // Methods.

    /**
     * Returns the owner of this class; or more to the point the value `owner` as given to ctor earlier.
     * @return See above.
     */
    T* get_owner() const;

  private:
    // Data.

    /// The holder of this object.
    T* m_owner;
  }; // class Extent_hooks_wrapper

  /// Output format for stats_dump_to_ostream() and the sibling stats-dump methods.
  enum class Stat_format
  {
    /// Human-readable text (jemalloc `malloc_stats_print()` default formatting).
    S_TEXT,
    /// JSON (jemalloc `malloc_stats_print()` `"J"` option).
    S_JSON
  };

  // Methods.

  /**
   * Allocates uninitialized memory designated for the default memory areas, which are also known as arenas,
   * without the use of a thread cache.
   *
   * @param size
   *        The allocation size, which must be greater than zero.
   * @return Upon success, a non-null pointer to the base address of the allocation; otherwise, nullptr.
   */
  void* allocate(size_t size) const override;

  /**
   * Allocates uninitialized memory designated in a segregated memory area, which is also known as an arena,
   * without the use of a thread cache.
   *
   * @param size
   *        The allocation size, which must be greater than zero.
   * @param arena_id
   *        The id of the memory area.
   * @return Upon success, a non-null pointer to the base address of the allocation; otherwise, nullptr.
   */
  void* allocate(size_t size, arena_id_t arena_id) const;

  /**
   * Allocates uninitialized memory designated in a segregated memory area, which is also known as an arena.
   *
   * @param size
   *        The allocation size, which must be greater than zero.
   * @param arena_id
   *        The id of the memory area.
   * @param thread_cache_id
   *        The thread cache ID to associate the allocation with.
   * @return Upon success, a non-null pointer to the base address of the allocation; otherwise, nullptr.
   */
  void* allocate(size_t size, arena_id_t arena_id, tcache_id_t thread_cache_id) const;

  /**
   * Deallocates memory from the default arenas without using a thread cache.
   *
   * @param address
   *        The address to be deallocated, which must be non-null.
   */
  void deallocate(void* address) const override;

  /**
   * Deallocates memory from a specific arena without using a thread cache.  If `address = allocate(..., arena_id, T)`
   * was used -- where `T` is a thread-cache identifier -- this will still work (from any thread at that) albeit
   * at a perf penalty versus `deallocate(address, arena_id, T)` (which however must be done from the allocating
   * thread).
   *
   * @param address
   *        The address to be deallocated, which must be non-null.
   * @param arena_id
   *        The id of the memory area that initially allocated the memory.
   */
  void deallocate(void* address, arena_id_t arena_id) const;

  /**
   * Deallocates memory from a specific arena.
   * Must be invoked from the thread that created the cache `thread_cache_id`; else behavior is undefined.
   * If the calling thread has no such cache (for `arena_id`), use deallocate() sans `thread_cache_id` arg;
   * at a perf penalty it will work.  (You can also create a cache first; this is reasonable most of the time,
   * and can help perf as noted, but be careful about "most of the time."  See Thread_cache doc header.)
   *
   * @param address
   *        The address to be deallocated, which must be non-null.
   * @param arena_id
   *        The id of the memory area that initially allocated the memory.
   * @param thread_cache_id
   *        A thread cache ID created in the calling thread, for `arena_id`.  (It need not be the cache -- nor
   *        its thread the thread -- involved in `address`'s original allocation.)
   */
  void deallocate(void* address, arena_id_t arena_id, tcache_id_t thread_cache_id) const;

  /**
   * Creates a new segregated memory area.  Throws `flow::error::Runtime_error` on jemalloc error.
   *
   * @param extent_hooks
   *        A borrowed handle to a set of callbacks, which may be null to use the default set.
   *        Note that any non-null value must be kept valid for the arena's lifetime.
   * @return The id of the memory area that is created.
   */
  arena_id_t create_arena(extent_hooks_t* extent_hooks) const;

  /**
   * Forwards to Thread_cache::destroy_arena_safely().  Please see that method and in particular note that
   * depending on the tcache situation the *actual* arena destruction may occur synchronously or asynchronously.
   *
   * @param arena_id
   *        See above.
   * @param log_ctx
   *        See above.
   * @param on_done_func
   *        See above.
   */
  template<typename On_done_func>
  void destroy_arena(arena_id_t arena_id, const flow::log::Log_context_mt* log_ctx, On_done_func&& on_done_func) const;

  /**
   * Equivalent of the other overload but lacks the `on_done_func` argument and therefore offers no opportunity
   * to execute code of caller's choice when the arena destruction actually occurs (which may be asynchronous).
   *
   * @param arena_id
   *        See above.
   * @param log_ctx
   *        See above.
   */
  void destroy_arena(arena_id_t arena_id, const flow::log::Log_context_mt* log_ctx) const;

  /**
   * Writes a broad, raw, process-wide jemalloc statistics dump (all arenas) to `os`.  Thin wrapper around
   * jemalloc `malloc_stats_print()`.  For curated, typed, per-Flow-IPC-arena stats use
   * jemalloc::Ipc_arena::memory_manager_stats() instead; this dump is the complementary "everything jemalloc
   * knows" firehose.
   *
   * The output ends in a newline.  No bracketing text is added by us: it all comes from jemalloc.  (OK, fine,
   * in a certain corner case we add a newline at the end.)
   *
   * If the linked jemalloc was built without statistics support (a configure-time option, `--disable-stats`,
   * reported at runtime by the read-only `config.stats` option), the output is still well-formed and non-empty.
   * Roughly, it'll include at least things like jemalloc version + build config + settings but will
   * omit per-arena runtime statistics (alloc counts/bytes, bins/large/extents/mutex).
   *
   * @note Relatedly: our curated stats-accessor, Ipc_arena::memory_manager_stats(), will also produce
   *       nominally non-empty output, if jemalloc not built with stats support, but essentially it'll be
   *       zeroes all-over.
   *
   * @param os
   *        Stream to which to write.  Defaults to standard output.
   * @param format
   *        Format of the output.  Defaults to human-readable text.
   * @param extra_opts
   *        If not empty: appended verbatim to jemalloc's `malloc_stats_print()` options string (its flag letters
   *        toggle output sections -- merged-vs-per-arena, bins/large/extents/mutex; see jemalloc docs).
   */
  void stats_dump_to_ostream(std::ostream& os = std::cout, Stat_format format = Stat_format::S_TEXT,
                             util::String_view extra_opts = {}) const;

  /**
   * Like stats_dump_to_ostream() but returns the dump as a string.
   *
   * @param format
   *        See stats_dump_to_ostream().
   * @param extra_opts
   *        See stats_dump_to_ostream().
   * @return The dump.
   */
  std::string stats_dump_to_string(Stat_format format = Stat_format::S_TEXT,
                                   util::String_view extra_opts = {}) const;

  /**
   * Like stats_dump_to_ostream() but logs the dump -- as a single (multi-line) message -- via `logger_ptr` at
   * severity `sev`.  The message will bracket stats_dump_to_ostream() with header/footer.
   *
   * @param logger_ptr
   *        Logger to use (null is allowed as usual).
   * @param format
   *        See stats_dump_to_ostream().
   * @param sev
   *        Severity at which to log (INFO-level by default); normal severity/component-filter must pass for
   *        logging to happen (as usual).
   * @param extra_opts
   *        See stats_dump_to_ostream().
   */
  void log_stats_dump(flow::log::Logger* logger_ptr, Stat_format format = Stat_format::S_TEXT,
                      flow::log::Sev sev = flow::log::Sev::S_INFO, util::String_view extra_opts = {}) const;

private:
  // Methods.

  /**
   * Allocates memory associated designated in a segregated memory area, which is also known as an arena.
   *
   * @param size
   *        The allocation size, which must be greater than zero.
   * @param arena_id
   *        The id of the memory area.
   * @param thread_cache_flags
   *        The jemalloc flags specifying a thread cache.
   * @return Upon success, a non-null pointer to the base address of the allocation; otherwise, nullptr.
   */
  void* allocate_helper(size_t size, arena_id_t arena_id, int thread_cache_flags) const;

  /**
   * Deallocates memory from a specific arena.
   *
   * @param address
   *        The address to be deallocated, which must be non-null.
   * @param arena_id
   *        The id of the memory area that initially allocated the memory.
   * @param thread_cache_flags
   *        The jemalloc flags specifying a thread cache.
   */
  void deallocate_helper(void* address, arena_id_t arena_id, int thread_cache_flags) const;
}; // class Memory_manager

// Template implementations.

template<typename T>
Memory_manager::Extent_hooks_wrapper<T>::Extent_hooks_wrapper(extent_hooks_t&& extent_hooks, T* owner) :
  extent_hooks_t(std::move(extent_hooks)),
  m_owner(owner)
{
  // Nothing.
}

template<typename T>
T* Memory_manager::Extent_hooks_wrapper<T>::get_owner() const
{
  return m_owner;
}

template<typename On_done_func>
void
  Memory_manager::destroy_arena(arena_id_t arena_id, const flow::log::Log_context_mt* log_ctx,
                                On_done_func&& on_done_func) const
{
  Thread_cache::destroy_arena_safely(arena_id, log_ctx, std::move(on_done_func));
  // Incidentally: Thread_cache is aware of the detail::jemalloc_arena_list_mutex() issue and will work with us.
}

} // namespace ipc::shm::arena_lend::jemalloc
