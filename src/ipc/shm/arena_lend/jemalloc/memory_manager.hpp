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

namespace ipc::shm::arena_lend::jemalloc
{

// Types.

/**
 * Wrapper around jemalloc.  This is a thin wrapper around the jemalloc API: used by SHM-jemalloc for its internal
 * impl purposes but available for direct end user calls as well.
 *
 * ### Purpose ###
 * It is usable directly as a small jemalloc wrangler.  That is, it can make easier some of the more advanced
 * activities, like working with custom-made arenas, including the surprisingly tricky business of destroying
 * one safely.  The stats-dump helpers like stats_dump_to_string() and stats_dump_to_ostream() are potentially
 * nice too.  (However they're also available through Ipc_arena::info_dump().)  Last but not least, using this
 * wrangler instead of direct jemalloc-API calls can avoid certain jemalloc landmines not yet fixed as of
 * jemalloc-5.4.0 (see below).
 *
 * It won't block access to the actual jemalloc API.
 *
 * That said, the main purpose of its existence is for the SHM-provider SHM-jemalloc (see ipc::shm for overall
 * intro to Flow-IPC's SHM-providers) to implement its services.  For example Ipc_arena::construct() and
 * Ipc_arena::allocate() will use Memory_manager::allocate(), while Ipc_arena::create() will create_arena()
 * while hooking up its SHM-wrangling extent hooks that would execute as part of allocate() at times.
 *
 * The public access to these goodies seemed worthwhile as well, so we made it user-facing.
 *
 * @see also Thread_cache which provides a related, and fairly sophisticated, wrangler of the jemalloc tcache
 *      system.  How to properly use the jemalloc tcache feature is not necessarily obvious, especially so if
 *      custom arenas with arbitrarily overlapping finite lifetimes are involved.  Things are prone to crash --
 *      and not necessarily right when one does the wrong thing but, in some scenarios, later.
 *
 * ### SHM-jemalloc versus direct use of jemalloc ###
 * Using jemalloc directly ("directly" in this case includes calls through Memory_manager) is possible with or
 * without using SHM-jemalloc (centered on Ipc_arena) in parallel.  As far as conflicts of using jemalloc
 * directly versus SHM-jemalloc in parallel:
 *   - SHM-jemalloc [de]allocates() only through jemalloc-arenas it creates/destroys/manages by itself; never
 *     through the default arenas (i.e., it would not do a regular jemalloc-`malloc()` but rather always specifies
 *     some arena ID it itself maintains).  This holds whether or not jemalloc is also your default
 *     `malloc()`/`free()`/`new`/`delete` for regular heap purposes.
 *   - Certain *global* (as opposed to per-arena) jemalloc config options are required for SHM-jemalloc to work
 *     properly.  In some cases this is something we enforce programmatically: by emitting an error
 *     if something is set wrong (best-effort).  Otherwise we assume the default/common setup is what
 *     is in effect and proceed under that assumption (see below `todo`).  As of this writing the checking/emission
 *     occurs in Ipc_arena::create().
 *
 * @todo While the current SHM-jemalloc impl is thought to play nicely with other parallel jemalloc use (such
 * as for vanilla heap allocation) -- with all allocation segregated into separately maintained jemalloc-arenas and
 * with only very few enforced global option-setting requirements -- we should methodically explore
 * and document what exactly SHM-jemalloc requires of the jemalloc config (both compiled-in and dynamically
 * settable options).  Things are OK given a standard jemalloc build, within a fairly wide (calendar-wise)
 * version range, and we check some (a small # of) global settings we know we require, but a methodical analysis
 * in the guts of SHM-jemalloc might yield more and save users some pain.
 *
 * ### Stats-dump versus arena creation/destruction ###
 * We offer a thin wrapper for jemalloc's stats-dump-to-string dump feature (`malloc_stats_print()`):
 * stats_dump_to_ostream(), stats_dump_to_string(), log_stats_dump().  It turns out that `malloc_stats_print()`
 * is likely to crash (abort with a message like `<jemalloc>: Failure in xmallctlbymib()` and variations), if
 * concurrently an arena is being destroyed or (conservatively speaking) created.
 * (This is not fixed, or counted as a bug, as of jemalloc-5.4.0 (9/2026).)
 *
 * SHM-jemalloc works around it (internally by using a process-wide mutex), so as long as one uses our APIs
 * (Flow-IPC generally for SHM-jemalloc arena control as needed, naturally; Ipc_arena::info_dump() or
 * `Memory_manager::*stats_dump*()` for stats-dumping), one is safe from this problem.  Just be warned that using
 * these jemalloc APIs directly (for whatever reason) can lead to trouble, as direct calls will not know to
 * collaborate with Flow-IPC's use of the internal mutex; and Flow-IPC may itself create/destroy arenas and
 * dump stats at various times.
 */
class Memory_manager
{
public:
  // Types.

  /**
   * Wrapper around extent hooks, which are callbacks that can be instituted in jemalloc per arena.  The
   * prescribed usage is to implement desired callbacks that execute the implementation within the creating
   * object (arg `owner` to ctor).  The `extent_hooks*` callback parameter can be cast to a pointer of this type
   * to retrieve the owner of the callback for execution in that class object's context.
   *
   * So to jemalloc, which is provided `this` as cast to `extent_hooks_t*`: It sees a table of free/`static`
   * functions of yours and calls the appropriate one depending on the event (e.g.: new extent needed).  (Next to
   * that is stored the value `owner` given to ctor, but jemalloc does not know or care.) Each
   * of those functions is given the `extent_hooks_t*` plus the hook-specific args; so the function can then
   *   -# take `extent_hooks_t*` and cast it to `Extent_hooks_wrapper<T>*`;
   *   -# call `->get_owner()` on that which gets the aforementioned stored `T*`;
   *   -# on that, call the appropriate method (e.g., one that handles "new extent needed") of T, passing it
   *      the rest of the args following the `extent_hooks_t*` arg.
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
     * Returns the value `owner` as given to ctor earlier.
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
   * Allocates uninitialized memory by using jemalloc's vanilla `malloc()` implementation.
   *
   * This may use an automatically determined thread cache, transparently.
   *
   * A jemalloc-arena is internally chosen from jemalloc's own arena set, also transparently.
   *
   * @note If used in an STL-compliant allocator, proper behavior on null return from this is to throw an exception
   *       (such as `std::bad_alloc`).
   *
   * @param size
   *        The allocation size which must be greater than zero.
   * @return Upon success, a non-null pointer to the base address of the allocation; otherwise, null.
   */
  void* allocate(size_t size) const;

  /**
   * Allocates uninitialized memory, in the specified jemalloc-arena, without the use of a thread cache.
   *
   * @note If used in an STL-compliant allocator, proper behavior on null return from this is to throw an exception
   *       (such as `std::bad_alloc`).
   * @see Thread_cache class doc header for a description of how jemalloc tcache works.
   *
   * @param size
   *        The allocation size which must be greater than zero.
   * @param arena_id
   *        The jemalloc-arena ID.
   * @return Upon success, a non-null pointer to the base address of the allocation; otherwise, null.
   */
  void* allocate_no_tcache(size_t size, arena_id_t arena_id) const;

  /**
   * Allocates uninitialized memory, in the specified jemalloc-arena, with the (potential) use of the specified
   * thread cache.
   *
   * @note If used in an STL-compliant allocator, proper behavior on null return from this is to throw an exception
   *       (such as `std::bad_alloc`).
   * @see Thread_cache class doc header for a description of how jemalloc tcache works.  Using this API form,
   *      without knowing that information or equivalent, may lead to confusing and undefined behavior.
   *
   * @param size
   *        The allocation size which must be greater than zero.
   * @param arena_id
   *        The jemalloc-arena ID.
   * @param thread_cache_id
   *        The thread cache ID with which to associate the allocation.
   *        jemalloc requires that a given tcache is not used by more than one thread concurrently; a stronger (not
   *        strictly required but, informally speaking, convenient) rule is that the mention of a given tcache be
   *        restricted to the thread in which it was created.
   *        All in all: suggest using class Thread_cache to manage these.
   * @return Upon success, a non-null pointer to the base address of the allocation; otherwise, null.
   */
  void* allocate(size_t size, arena_id_t arena_id, tcache_id_t thread_cache_id) const;

  /**
   * Deallocates memory by using jemalloc's vanilla `free()` implementation.
   *
   * This may use an automatically determined thread cache, transparently.
   *
   * The jemalloc-arena is (here and elsewhere) always internally determined by jemalloc based on `address`.
   *
   * @param address
   *        The address to be deallocated; must be non-null.
   */
  void deallocate(void* address) const noexcept;

  /**
   * Deallocates memory similarly to `deallocate(address)`, except that this is done without the use of a thread
   * cache.  That is: `address` is not fed to any tcache for later reuse by some `allocate()` or equivalent.
   *
   * The jemalloc-arena is (here and elsewhere) always internally determined by jemalloc based on `address`.
   *
   * @see Thread_cache class doc header for a description of how jemalloc tcache works.
   *
   * @param address
   *        The address to be deallocated; must be non-null.
   */
  void deallocate_no_tcache(void* address) const noexcept;

  /**
   * Deallocates memory similarly to `deallocate(address)`, except that this is done with the (potential) use of
   * the *specified* thread cache.
   *
   * The jemalloc-arena is (here and elsewhere) always internally determined by jemalloc based on `address`.
   *
   * @see Thread_cache class doc header for a description of how jemalloc tcache works.  Using this API form,
   *      without knowing that information or equivalent, may lead to confusing and undefined behavior.
   *
   * @param address
   *        The address to be deallocated; must be non-null.
   * @param thread_cache_id
   *        The thread cache ID with which to associate the deallocation.
   *        jemalloc requires that a given tcache is not used by more than one thread concurrently; a stronger (not
   *        strictly required but, informally speaking, convenient) rule is that the mention of a given tcache be
   *        restricted to the thread in which it was created.
   *        All in all: suggest using class Thread_cache to manage these.
   */
  void deallocate(void* address, tcache_id_t thread_cache_id) const noexcept;

  /**
   * Creates a new segregated memory area (jemalloc-arena), wired to the given extent hooks (if any).
   * Throws `flow::error::Runtime_error` on jemalloc error.
   *
   * @param extent_hooks
   *        A handle (which is memorized) to a set of callbacks; or null to use the default set.
   *        Note that any non-null value must be kept valid for the arena's lifetime.
   * @return jemalloc-arena ID.  (It is not unique; in that destroying arena X may mean create_arena() will
   *         subsequently return X; jemalloc tends to reuse arena IDs.)
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
   *        See above.  Note `on_done_func` is potentially consumed.
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
   * knows" firehose of information.
   *
   * The output ends in a newline.  No bracketing text is added by us: it all comes from jemalloc.  (OK, fine,
   * for Stat_format::S_JSON we add a newline at the end.)
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
   * Boring helper for allocate_no_tcache() and `allocate(size, arena_id, tcache_id)`.
   *
   * @param size
   *        See allocate_no_tcache().
   * @param arena_id
   *        See allocate_no_tcache().
   * @param thread_cache_flags
   *        A `MALLOCX_TCACHE_*` expression.
   * @return Upon success, a non-null pointer to the base address of the allocation; otherwise, null.
   */
  void* allocate_helper(size_t size, arena_id_t arena_id, int thread_cache_flags) const;

  /**
   * Boring helper for deallocate_no_tcache() and `deallocate(address, tcache_id)`.
   *
   * @param address
   *        See any `deallocate*()`.
   * @param thread_cache_flags
   *        See allocate_helper().
   */
  void deallocate_helper(void* address, int thread_cache_flags) const noexcept;
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
void Memory_manager::destroy_arena(arena_id_t arena_id, const flow::log::Log_context_mt* log_ctx,
                                   On_done_func&& on_done_func) const
{
  Thread_cache::destroy_arena_safely(arena_id, log_ctx, std::move(on_done_func));
  // Incidentally: Thread_cache is aware of the detail::jemalloc_arena_list_mutex() issue and will work with us.
}

} // namespace ipc::shm::arena_lend::jemalloc
