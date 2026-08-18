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

#include "ipc/common.hpp"
#include "ipc/shm/arena_lend/arena_lend_fwd.hpp"
#include <flow/util/util.hpp>
#include <boost/interprocess/sync/mutex_family.hpp>
#include <boost/interprocess/offset_ptr.hpp>
#include <boost/array.hpp>

namespace ipc::shm::arena_lend::detail
{

/**
 * A boost.ipc-compliant memory-algorithm suitable for use in a `boost::interprocess::basic_managed_shared_memory`
 * whose mission is narrow: to allocate N-byte use-counts in Lend_tracker_pool.  (N is a compile-time constant,
 * namely #S_ALLOC_SZ.  Therefore in particular a `static_assert()` elsewhere guarantees that
 * `sizeof(Lend_tracker_pool::Atomic_use_ct::value_type) == Use_count_registry::S_ALLOC_SZ`.  When thinking about
 * this generically, we recommend imagining N is either 1 or 4 so as to ballpark practical implications.)
 *
 * That is, it essentially works only for `allocate(S_ALLOC_SZ)` (N-byte allocations) and corresponding
 * deallocate() calls but is internally optimized to store such N-byte individual objects tightly and to use a
 * custom bitmap-based algorithm to find free slots quite quickly (and return them even more quickly).
 *
 * ### Rationale ###
 * Why write it as a boost.ipc memory-algorithm specifically, particularly since it cannot be used for general
 * data but only N-byte objects (not to mention its, as of this writing, rigid size properties)?  Answer:
 * it is convenient to then create a boost.ipc `basic_managed_shared_memory` -- with Use_count_registry as a
 * key template parameter thereof -- at which point stuff slots-into the appropriate vaddr location with great
 * ease of coding.  The algorithm of finding an N-byte slot efficiently would have been what it is here; and it
 * very much resembles a (narrow-use) memory-allocator; and we intended to use it in Lend_tracker_pool in SHM; so it
 * is quite a natural fit.
 *
 * ### How to use ###
 * Make a `basic_managed_shared_memory` M in the normal way but with Use_count_registry as a template parameter.
 * Probably you'll want to use `null_index` as a template parameter as well; though maybe not (not our business).
 * Size the segment `"decltype(M)::segment_manager::get_min_size() + Use_count_registry::S_ASSUMED_BASE_OFFSET"`
 * (or larger, but there's no point).
 *
 * We assume you'll want a single `Metadata` struct which is the only thing a `*this` will allocate() other
 * than the 1-sized slots -- but only one, and it must be allocated up-front.  Therefore, upon creating the pool,
 * first-thing:
 *   - `const auto mdt = M->allocate(sizeof(Metadata)); util::construct_at(mdt, ...);`
 *     - If you don't need any such `Metadata`, use an empty `struct Metadata`.  You must do that one allocation,
 *       even if it is 0-sized.
 *
 * You can now use `*mdt` for whatever.  Subsequently, you can (must) issue
 * `const auto p = M->allocate(N); ...; M->deallocate(p);` at will.  If storing things such as
 * `atomic<uint32_t>` within these N-byte slots, remember to use:
 *   - `util::construct_at<T>(p, ...); // ctor args in ...; T is the particular N-byte-sized type.`
 *     - following `p = M->allocate(sizeof(T))`.
 *   - `static_cast<T*>(p)->~T()`
 *     - preceding `M->deallocate(p)`.
 *
 * Note: or if you chose to use non-`null_index` index, then you can do all that sweet built-in
 * `M->construct<T>(...)(...)` stuff (and so on).  That said, with `null_index`, one might guess they can
 * still use `M->construct<T>(anonymous_instance)(...)` -- its being anonymous and all -- but no; that
 * does not compile; apparently even anonymous-instance things still require a real index.
 * Still, with util::construct_at() utility it's close to the same level of syntactic sugar anyway.
 * (In C++20 it is available as `std::construct_at()`, but as of this writing we are on C++17.)
 *
 * ### Resource use: RAM ###
 * In the worst case a `*this` uses (as of this writing; see constants in body which can be adjusted):
 *   - 4 Mi (#S_USE_COUNTS_CAPACITY x N) bytes for the data
 *     (about `USE_COUNTS_CAPACITY` use-count slots and 1 metadata);
 *   - 1/8 Mi bytes for the bitmap and change (mandatory).
 *
 * However the former item starts at only 128 Ki (#S_USE_COUNTS_CAPACITY_QUANTUM_SZ times N) bytes RAM used.
 * Another 128 Ki is added once (and if) that ever becomes insufficient; then another; and so on until all 4 Mi are
 * taken.  After that allocate() fails (returns null; so `M->allocate()` that wraps it throws `bad_alloc`).
 *
 * ### Resource use: processor cycles ###
 * The algorithm and assumptions made (also in Lend_tracker_pool) are optimized for speed.  As of this writing
 * benchmark results are TBD however.  There is no locking (see below).  There is some trade-off to be had
 * between the way the algorithm insists on using all of a *quantum* (128 Ki bytes as of this writing) before
 * expanding to use another such quantum.  It could be modified to expand earlier if, e.g., searches seem to be
 * scanning too many slots during allocate() -- more aggressive (earlier) RAM use to increase speed.  Such
 * adjustments would be downstream of benchmarking in real-world situations.  As of this writing there's decent
 * reason to believe the way the bitmapping/scanning is set up allows for even worst-case searches to be quick.
 *
 * @todo Benchmark Use_count_registry, especially in the field, and adjust the allocate() algorithm accordingly,
 * especially as concerns aggressiveness of expanding the active search field (for speed) at the expense of
 * higher (earlier) RAM use.  If in reality the number of live objects, e.g., rarely goes above 1,000, then there
 * is no need to worry about it, as searches will be quick, *and* RAM use will be low.
 *
 * ### Limitations ###
 * As noted at the top, this is not a memory-algorithm usable for storing generally-sized objects, as it supports
 * exactly one such object (*metadata*) and beyond that only N-byte objects, where N is a constant (as of this writing:
 * it is 4).
 *
 * Use_count_registry is, together with Lend_tracker_pool, based on the assumption that only one thread shall
 * ever do allocate() and deallocate() on a given `*this`.  Therefore it mandates the null "mutex" family
 * inside any containing `basic_managed_shared_memory`.
 *
 * For simplicity and resulting speed there is a rigid limit to how many N-byte use-counts can be stored in a
 * `*this`: #S_USE_COUNTS_CAPACITY (as of this writing 1 Mi a/k/a ~1 million) slots.  That is, it can represent
 * 1 million live outer a/k/a first-class a/k/a `Lend_tracker_pool`-tracked a/k/a arena-`construct<>()`ed objects
 * per admin-mode `*this`, a/k/a per (constructing) thread.
 *   - This should be plenty.  (Side note: the first-`allocate()`-reserved metadata header will reduce the
 *     size available for use-counts but probably only a little.  So we ignore this in these discussions.)
 *   - However if it is not plenty, we could increase #S_USE_COUNTS_CAPACITY.  This would increase the
 *     bitmap size-in-bytes which is by definition 1/8 latter number -- and is like `S_USE_COUNTS_CAPACITY x N`
 *     paged-in only according to the quanta in use.
 *     - And/or we could be smarter/more dynamic about the bitmap itself also at the expense of code simplicity
 *       (and, possibly, speed).
 *
 * We never give RAM "back," outside of deleting the backing SHM-pool.
 *
 * ### The algorithm ###
 * The above probably gives decent hints as to how it works (bitmap with 1 bit per N-byte/use-count slot;
 * a "playing field" divided into smaller *quanta*, with more joining the field
 * if needed).  The rest can likely be gleaned by just reading the code, starting with the API and then the
 * `private: // Data.` area.
 *
 * @todo Tactically, Use_count_registry does much pointer arithmetic such as add/subtract/compare on actual
 *       pointer types, particularly `uint8_t`; but it is formally safer (details omitted here) to convert
 *       to `uintptr_t` (an integer type that can hold ptr vals) before doing such math.
 */
class Use_count_registry :
  private boost::noncopyable
{
public:
  // Types.

  /**
   * This indicates that when using this as the memory-algorithm, we shall use no locking; this is a key
   * assumption of our narrow use case (see class doc header).  In reality as of this writing the
   * `basic_managed_shared_memory` that will instantiate us will also use `null_index` -- there will be no
   * lookup of objects by name, etc. -- so actually setting this has no effect; but if that were to change
   * then this would matter.
   */
  using Mutex_family = ::ipc::bipc::null_mutex_family;

  /// boost.ipc expects this stylization of #Mutex_family.
  using mutex_family = Mutex_family;

  /**
   * This alias is required for use in a `basic_managed_shared_memory`; `offset_ptr` is a solid default choice --
   * though in our use case we do not believe this is actually used by anything externally (definitely not
   * internally).
   */
  using Void_pointer = ::ipc::bipc::offset_ptr<void>;

  /// boost.ipc expects this stylization of #Void_pointer.
  using void_pointer = Void_pointer;

  /// The usual `difference_type`.
  using difference_type = ptrdiff_t;

  /// The usual `size_type`.
  using size_type = size_t;

  /// Required for things to build, but we do not supply an `allocate_many()`, so we set it to a dummy value.
  using multiallocation_chain = int;

  /// Short-hand for arena_lend::stat::Obj_db_aux_pool_stats;
  using Obj_db_aux_pool_stats = arena_lend::stat::Obj_db_aux_pool_stats;

  // Constants.

  /**
   * This important constant defines the mandatory size of each allocation -- that is argument to allocate() --
   * after the initial (metadata) allocation.  So if it is 1, then you'll be storing, e.g., `atomic<uint8_t>`s or
   * `uint8_t`s here; if it is 4 then, e.g., `atomic<uint32_t>`s or `uint32_t`s.
   *
   * ### Trade-offs ###
   * The higher this is, the higher the (presumably) max use-count value you can store.  That is good.
   * On the other hand, the higher this is, the more RAM is used (at mimimum and at maximum, both) to store
   * the use-counts.  The bitmap area's size does not depend on this; however the main/largest area
   * can be sized up to `S_ALLOC_SZ * S_USE_COUNTS_CAPACITY` bytes; and is at minimum
   * sized `S_ALLOC_SZ * S_USE_COUNTS_CAPACITY_QUANTUM_SZ`.
   */
  static constexpr size_t S_ALLOC_SZ = 4;

  /**
   * Compile-time knob: how many (N-byte) use-counts one `*this` can allocate.
   * get_free_memory() is counted against this value times #S_ALLOC_SZ (a/k/a N) (hence begins equal to this times N,
   * before first allocate()).
   *
   * The first `allocate(n)` is a special allocation, always at the top of this space, for the `*this` user(s)
   * to hold a single metadata area; so in reality the use-counts capacity in bytes will be #S_USE_COUNTS_CAPACITY times
   * N, minus `n` (rounded-up to a certain small multiple).  Nevertheless it'll be close to this value all-in-all.
   */
  static constexpr size_t S_USE_COUNTS_CAPACITY = 1024 * 1024;

  /// The most stringent alignment.  E.g., in x86-64 this is 16, we've observed.
  static constexpr size_t S_ALIGN_SZ = flow::util::max_align_sz();

  /// boost.ipc expects this stylization of #S_ALIGN_SZ.
  static constexpr size_t Alignment = S_ALIGN_SZ;

  /**
   * boost.ipc `segment_manager` requires this of a memory-algorithm: per-allocation book-keeping overhead in bytes.
   * We keep all book-keeping externally (the use-count bitmap), so there is no such overhead.
   *
   * (In any case, as of this writing, in our use-case this should not be mentioned by anything at compile-time;
   * but the member is required for the class to be instantiable at all, for some clang versions at least, while
   * at least some compilers do not complain.)
   */
  static constexpr size_t PayloadPerAllocation = 0;

  /**
   * This (a bit of a hack) is a compile-time knob/constant containing the number of bytes assumed to be
   * between the start of the SHM-pool (segment) storing us, and us (`*this`).
   *
   * ### Rationale ###
   * Internally, we prefer to keep the main data area (where #S_USE_COUNTS_CAPACITY times #S_ALLOC_SZ bytes available
   * to allocate()) aligned to the page size (and a multiple of page size), and certain subdivisions therein
   * are also similarly aligned to/multiples of page size (4Ki bytes as of this writing/Linux/x86-64 at least),
   * relative to the start of the containing SHM-pool.  Whether that's a worthwhile endeavour or not is not
   * to be litigated here; but as of this writing we undertake it.
   *
   * So in order to undertake it successfully, knowing where `this` is and `extra_hdr_sz` passed to ctor (which
   * specifies size of area *after* `*this` but before our data area sized #S_USE_COUNTS_CAPACITY) is not quite
   * enough, because if the whole thing starts off a few bytes into the SHM-segment, then that slides the whole
   * thing over we don't know how much.  (Note: `extra_hdr_sz` is used by `segment_manager` to store its stuff --
   * though in our case as of this writing we have `null_index`, so perhaps it is zero... but we digress.)
   *
   * In reality as of this writing `basic_managed_shared_memory` happens to shove a little header into the
   * pre-`*this` area.  It's hard to tell exactly, but I think it's where the segment size is stored for its
   * internal purposes (is what scouring boost.ipc code seems to indicate)... but we digress again.
   *
   * Bottom line: it can be seen in boost.ipc source code that it's a little header <= #S_ALIGN_SZ, rounded up
   * to #S_ALIGN_SZ.  So that's what we set this to now.  The hackiness is that technically that could change --
   * perhaps to be a multiple of #S_ALIGN_SZ that is not 1x -- and until then this value is "empirically observed."
   */
  static constexpr size_t S_ASSUMED_BASE_OFFSET = S_ALIGN_SZ;

  /**
   * Default page size.  We generally try to make larger vaddr areas sized as multiples of this and aligned
   * on this versus the start of the SHM-pool in which we reside.  That is not life-or-death whatsoever, but
   * we figure, if there will be stats about things like dirty pages, we might as well cleanly operate on entire
   * such pages and get nice clean counts perhaps.
   *
   * Public for informational (not algorithmic) purposes: reporting, logging, diagnostics.
   */
  static constexpr size_t S_PAGE_SZ = 4 * 1024;

  /**
   * The size of each *quantum* into a whole number of which the data area (sized #S_USE_COUNTS_CAPACITY N-byte values)
   * is divided.  At any given time, the data area we've actually touched (thus paged-in: meaning actual RAM is
   * being used as opposed to mere vaddr space) consists of 1+ quanta, starting at the start of the
   * data area.  As of this writing the algorithm is: start with 1 quantum (hence this much of the actual data
   * area = real RAM); allocate from there, until no use-count slots are free to use; extend to 2 quanta;
   * repeat; ...; until all `S_USE_COUNTS_CAPACITY` slots have been used (no more quanta available).
   * As of this writing we do not un-page any pages or decrement the # of quanta.
   *
   * Public for informational (not algorithmic) purposes: reporting, logging, diagnostics.
   */
  static constexpr size_t S_USE_COUNTS_CAPACITY_QUANTUM_SZ = 32 * 1024; // 32Ki use-counts (each N bytes) per quantum.

  // Constructors/destructor.

  /**
   * Constructs a `*this`, assumed to be in a SHM-pool (segment) at the time the `basic_managed_shared_memory` is
   * being created in SHM.  So the creating-side would use this ctor, whereas any opening-side(s) would
   * already work on a existing `*this` (already cted).
   *
   * @param sz
   *        Total size of SHM-segment.
   * @param extra_hdr_sz
   *        Size required for us to not touch, between the end of our header (`*this`) and the start of the
   *        area from which allocate() shall draw.  (It is used by `segment_manager` to stores its stuff --
   *        though in our case as of this writing we have `null_index` and `null_mutex_family`, so this is probably
   *        zero.  We don't assume that though.)
   */
  Use_count_registry(size_t sz, size_t extra_hdr_sz);

  // Methods.

  /**
   * Size we require at max capacity (all #S_USE_COUNTS_CAPACITY times #S_ALLOC_SZ bytes used) for the containing
   * SHM-pool, including us and the `extra_hdr_sz` for the segment manager, excluding #S_ASSUMED_BASE_OFFSET.
   *
   * @param extra_hdr_sz
   *        See above.
   * @return See above.
   */
  static constexpr size_t get_min_size(size_t extra_hdr_sz);

  /**
   * Allocates space out of #S_USE_COUNTS_CAPACITY times #S_ALLOC_SZ bytes available originally, and returns the
   * address to the start of the following: the metadata header area (first call since pool creation) or a new
   * use-count (subsequently).
   *
   * As explained in our class doc header, we are not a general memory-algorithm but rather for a very narrow use.
   * Specifically:
   *   - The 1st allocate() call shall always return address at the start of the data area (sized
   *     up to #S_USE_COUNTS_CAPACITY times #S_ALLOC_SZ).  `sz` bytes are available as the *metadata area* for whatever
   *     stuff you want to store in addition to the bulk use-counts (maybe stats or make-shift communications between
   *     pool users).
   *     - `sz == 0` is allowed (meaning no metadata area needed).  You must still make the `allocate(0)` initial
   *       call in this case.
   *     - If `sz > #S_USE_COUNTS_CAPACITY * S_ALLOC_SZ`, behavior is undefined.
   *   - All allocate() calls after the 1st are *normal* allocations.
   *     - `sz != S_ALLOC_SZ`, in the spirit of forgiveness, yields null and allocates no space.  Otherwise:
   *     - `sz == S_ALLOC_SZ`: N (a/k/a #S_ALLOC_SZ) bytes are allocated for a new use-count and its address returned.
   *       - This address is *not* necessarily aligned to the most stringent alignment #S_ALIGN_SZ; but it is
   *         aligned to #S_ALLOC_SZ.  (boost.ipc non-reference docs suggest allocate() must return a stringently
   *         aligned address; but formally that is not the case, in that boost.ipc won't barf if this isn't heeded.
   *         They probably mean that for use as a memory-algorithm for general allocation, `malloc()`-like
   *         functions like this shall return stringently aligned addresses; as otherwise processor exceptions and other
   *         errors can result when placing larger-than-N-byte structures at such addresses.)
   *
   * Last but not least, to restate the doc header: allocate(), deallocate() (as well as all `stat*()` methods)
   * shall all be called on one `*this`, never concurrently.
   *
   * ### Errors ###
   * Per standard expectations: if one runs out of #S_USE_COUNTS_CAPACITY times N bytes (a/k/a get_free_memory() is
   * insufficient to yield a successful *normal* allocation), returns null.
   *
   * @param sz
   *        See above.
   * @return An address or null.  If used properly (no `sz != N` after allocation number one),
   *         null means we've run out of pool space.
   */
  void* allocate(size_t sz);

  /**
   * Reverses an earlier allocate() call.  Caveats:
   *   - `!addr` => no-op.
   *   - `addr` equals that returned by allocate() number one (the metadata one) => no-op.  That area cannot be
   *     returned.
   *   - `addr` neither null nor pointing to currently allocated area => undefined behavior.
   *
   * @param addr
   *        See above.
   */
  void deallocate(void* addr);

  /**
   * Returns the value passed to ctor, when the pool was created.
   * @return See above.
   */
  size_t get_size() const;

  /**
   * No-op: as of this writing a `*this` will allocate up to #S_USE_COUNTS_CAPACITY times #S_ALLOC_SZ, and requires the
   * pool to be sufficiently sized (get_min_size()) from the start; and will not use additional space beyond that.
   *
   * Internally, there is a fixed-size bitmap that we do not allow to grow somehow; hence additional space does
   * not help.  That said: grow() is generally not for SHM-pools anyway but for other managed-vaddr-area types
   * (files, who knows).
   *
   * @param extra_sz
   *        It is ignored.
   */
  void grow(size_t extra_sz);

  /**
   * How many `allocate(N)` calls, divided by #S_ALLOC_SZ, would succeed now without `deallocate()` in-between,
   * until one returned null.  If allocate() has not yet been called, then assume `allocate(0)` has been
   * called (0-sized metadata area); the preceding sentence is then accurate.
   *
   * @return See above.  Starts at #S_USE_COUNTS_CAPACITY times #S_ALLOC_SZ.
   */
  size_t get_free_memory() const;

  /**
   * Convenience function that returns the metadata area as a properly-typed pointer.  (This is not a
   * standard boost.ipc memory-algorithm API obviously.)
   *
   * Returns null if any of these is true:
   *   - allocate() has not been called.
   *   - The first `allocate(n)` call was such that `n != sizeof(Metadata)` (presumable type mismatch).
   *
   * @tparam Metadata
   *         Presumably the type of the metadata area you're using.
   * @return See above.
   */
  template<typename Metadata>
  Metadata* get_metadata();

  /**
   * Returns a non-negative number that is some compile-time constant K times
   * the number of *quanta* currently in-use by `*this` for the at-most-`USE_COUNTS_CAPACITYxALLOC_SZ`-sized
   * data area.  In short: see stats_record() doc header for how to use this.  If you want to know the actual
   * meaning then read on (Details below).
   *
   * ### Details ###
   * The data area holds up-to #S_USE_COUNTS_CAPACITY use-counts (minus some space for `Metadata`, whose size
   * is established by the first allocate()).  This is divided into M = #S_USE_COUNTS_CAPACITY_QUANTUM_SZ
   * quanta.  At first, only 1 quantum is used, as a memory-use-reduction strategy.  Therefore
   * stat_quanta_active() shall return K times 1.  Once that area runs out of slots, a quantum is added; so
   * stat_quanta_active() shall return K times 2.  Once both areas run out of slots, a 3rd quantum is
   * added... etc.  Potentially stat_quanta_active() can return up-to K times M.  (As of this writing the
   * number cannot go down: only stay same or sometimes increase.)
   *
   * If called before the first allocate(), then the data area has not yet been touched; so we return zero.
   *
   * Therefore, if one saves stat_quanta_active() before allocate() (including the 1st one) and then
   * passes this to stats_record(), then:
   *   -# stats_record() shall itself obtain stat_quanta_active() and compare to the passed-in (pre-`allocate()`)
   *      one.  If they are equal, nothing has changed (see stat::Obj_db_aux_pool_stats); no-op.  Otherwise:
   *   -# The preceding allocate() causes stat_quanta_active() to go up.  Therefore stats_record()
   *      shall update `*target_stats` accordingly (again: see stat::Obj_db_aux_pool_stats).
   *
   * stats_record() therefore shall do work only when necessary; and that will occur up to at most M times
   * per underlying data area's lifetime and only via allocate().  That is rare.  The stat_quanta_active()
   * call (pre-`allocate()` and then post-`allocate()` inside stats_record) itself is each a single memory
   * load and is cheap.
   *
   * @see stats_record(): Call this post-`allocate()`.  Call stat_quanta_active() pre-`allocate()`.  Pass the
   *      the result of the latter into the former as `prev_quanta_active`.
   * @see stats_record_at_deletion(): Call this to register that the aux-pool that contains `*this` shall be
   *      removed.
   *
   * @return See above.
   */
  size_t stat_quanta_active() const;

  /**
   * Updates the various GAUGEs and HI_WMARKs in `*target_stats`, if and only if it is appropriate to do so
   * given the assumption that `*target_stats` currently holds the values when
   * `this->stat_quanta_active() == prev_quanta_active` was the case.  For most up-to-date, yet still reasonably
   * performant, results `prev_quanta_active` should be obtained no later than following the most recent
   * allocate() or the creation of `*this`; and stats_record() called after the most recent allocate().
   *
   * @see stat::Obj_db_aux_pool_stats doc header and individual stat-members' doc headers; having a grasp on these
   *      is quite helpful in understanding on this whole setup.
   *
   * @warning Since the essential values in `*target_stats` are GAUGEs, it is imperative that one
   *          call stats_record_at_deletion() when about to delete the containg SHM-pool.  Otherwise
   *          GAUGEs like Obj_db_aux_pool_stats::m_resident_sz will never go down -- not even once all arenas
   *          have shut down.
   *
   * @note You can save the return value and pass it in as `prev_quanta_active` next time you call this.
   *       However simply calling stat_quanta_active() and saving it as a stack variable ahead of each
   *       allocate() involves less long-term state for you and should be ~equally performant.
   *
   * ### Rationale ###
   * Why this dance with `prev_quanta_active`?  Why can't `*this` save it automatically?  For that matter why
   * not just pass a `target_stats` to allocate() and have it update everything automatically?  Answer: As for
   * the latter: that is certainly doable and would simplify the API (fewer methods), but allocate() has a
   * standard signature imposed by boost.ipc; calling `basic_managed_shared_memory::allocate()` et al shall
   * invoke `this->allocate()` without passing it any stat-set or `prev_quanta_active`; `*this` is officially
   * a memory-algorithm.  Granted, since Lend_tracker_pool is the only actual user of Use_count_registry, it
   * could simply bypass `basic_managed_shared_memory` allocate-API when allocating and just do something like
   * `m_pool.get_segment_manager()->get_memory_algorithm()->our_own_custom_allocate()`.  Yet we've decided, for
   * better or worse, to be a "fully" usable memory-algorithm impl (in quotes because it can still only allocate
   * one N-byte thing and then only 4-byte things, among other limitations); we're sticking to it; it might
   * pay off via reusability.  As for why not just store the pre-`allocate()` value of stat_quanta_active()
   * in allocate() -- and not need to expose stat_quanta_active() as public -- this is again doable, but it's
   * a little dirty.  Recall that, as a memory-algorithm, `*this`'s data sits inside SHM -- all of it.
   * So, yes, we could add a `size_t m_prev_quanta_active` to Header... but then we're storing in-SHM something
   * stats-related that's accessed quite rigidly only by an admin-mode Lend_tracker_pool and not by other ones +
   * not in 2+ processes.  It's not an awful crime but a bit of a layering violation.  Something like that
   * naturally belongs in Lend_tracker_pool, so we give it that task.  Given these constraints, we've tried
   * to make the rest of the API as simple as possible to use.
   *
   * @param prev_quanta_active
   *        Result of stat_quanta_active() when `*target_stats` was last accurate w/r/t to `*this`.
   *        (Note that, as the induction base, before the initial `this->allocate()` it is correct that
   *        `*this`'s contribution to `*target_stats` be zero for all GAUGEs in stat::Obj_db_aux_pool_stats.)
   * @param target_stats
   *        Stats to update (only if necessary).
   * @return stat_quanta_active() at this time.
   */
  size_t stats_record(size_t prev_quanta_active, Obj_db_aux_pool_stats* target_stats);

  /**
   * Assuming one has followed the required algorithm to update `*target_stats` so far, this method
   * (intended to be called ahead of aux-pool deletion) shall undo (subtract) `*this`'s contribution to
   * `*target_stats` for all relevant GAUGEs.
   *
   * @param target_stats
   *        See stats_record().
   * @return stat_quanta_active() at this time (and, presumably, the final value thereof).
   */
  size_t stats_record_at_deletion(Obj_db_aux_pool_stats* target_stats);

private:
  // Constants, types.

  /**
   * In the bitmap mapping the data area of size #S_USE_COUNTS_CAPACITY, this defines a *word* of bits;
   * a *word* being so chosen so that it is the largest type, such that there is a high-performance intrinsic
   * available that takes this type that can scan for the first 0 bit.
   */
  using bit_word_t = uint64_t;

  // S_PAGE_SZ and S_USE_COUNTS_CAPACITY_QUANTUM_SZ are public (see above).

  static_assert((S_PAGE_SZ % S_ALIGN_SZ) == 0,
                "Come on now... PAGE_SZ should be a multiple of the most stringent alignment (16 or w/e).");

  static_assert((((S_USE_COUNTS_CAPACITY * S_ALLOC_SZ) % S_PAGE_SZ) == 0)
                  && (S_USE_COUNTS_CAPACITY != 0)
                  && ((S_USE_COUNTS_CAPACITY % 8) == 0),
                "Please set USE_COUNTS_CAPACITYxPAGE_SZ to a positive multiple of PAGE_SZ; "
                  "and set USE_COUNTS_CAPACITY to be a multiple of 8 (bits/byte).");

  /**
   * The total size of the bitmap area which uses 1 bit ber 1 use-count slot (each N bytes wide) of the data area
   * which is #S_USE_COUNTS_CAPACITY times N bytes in size.
   */
  static constexpr size_t S_BITMAP_SZ = S_USE_COUNTS_CAPACITY / 8;
  static_assert((S_BITMAP_SZ % sizeof(bit_word_t)) == 0,
                "Please set USE_COUNTS_CAPACITY to a multiple of [8 (bits/byte) x bit_word_t (8)].");

  static_assert((S_USE_COUNTS_CAPACITY % S_USE_COUNTS_CAPACITY_QUANTUM_SZ) == 0,
                "Please set USE_COUNTS_CAPACITY_QUANTUM_SZ to USE_COUNTS_CAPACITY divided by some integer.");
  static_assert((S_USE_COUNTS_CAPACITY_QUANTUM_SZ != 0) && ((S_USE_COUNTS_CAPACITY_QUANTUM_SZ % 8) == 0),
                "Please set USE_COUNTS_CAPACITY_QUANTUM_SZ to a positive multiple of 8 (bits/byte).");

  /// The size of each bitmap section (in bytes) corresponding to 1 quantum of use-count slots (each N bytes wide).
  static constexpr size_t S_BITMAP_QUANTUM_SZ = S_USE_COUNTS_CAPACITY_QUANTUM_SZ / 8;
  // 4Ki (=1 page) bytes in bitmap section. --^
  static_assert((S_BITMAP_QUANTUM_SZ % sizeof(bit_word_t)) == 0,
                "Please set USE_COUNTS_CAPACITY_QUANTUM_SZ to a multiple of [8 (bits/byte) x bit_word_t (8)].");

  /**
   * The raw data of the bitmap: an array of *words*, each word representing [`sizeof(word)` x 8] use-count slots
   * (each N bytes wide) of the data area.  So as of this writing that's N 64-bit words each standing for 64 use-count
   * slots.  One intrinsic call can find an unused (0) bit in one word (element in this array); hence 64 potential
   * slots.
   */
  using Bitmap_words = boost::array<bit_word_t, S_BITMAP_SZ / sizeof(bit_word_t)>;

  /// This is the data in `*this`: the stuff preceding the `extra_hdr_sz`-byte gap and then the data area.
  struct Header
  {
    /// See get_size().
    const size_t m_pool_sz;

    /// See ctor and/or get_min_size().
    const size_t m_extra_hdr_sz;

    /**
     * For speed, precomputed/memorized offset from `this` (a/k/a the start of the `Header`) where
     * the data area -- up to #S_USE_COUNTS_CAPACITY times #S_ALLOC_SZ bytes long -- begins.
     */
    const size_t m_data_start_minus_this;

    /**
     * How much of of the data area is in use, in bytes; i.e., the count of 1-bits in #m_bitmap_words,
     * times #S_ALLOC_SZ; a/k/a `S_USE_COUNTS_CAPACITY * S_ALLOC_SZ - get_free_memory()`.
     */
    size_t m_allocated_sz;

    /// The size passed to the first (metadata) allocate(); or null until then.
    std::optional<size_t> m_meta_obj_sz;

    /**
     * At all times a positive multiple of `S_BITMAP_QUANTUM_SZ / sizeof(bit_word_t) / 8`, and at most
     * `S_USE_COUNTS_CAPACITY / sizeof(bit_word_t) / 8`, this equals the number of #m_bitmap_words elements that
     * represent the currently active quanta.  I.e., it is the number of bitmap words, starting from
     * the `m_bitmap_words.begin()`, through which allocate() shall search (in the bitmap).
     * Once it fails to find a single 0-bit in the entire area, this is incremented by
     * `S_BITMAP_QUANTUM_SZ / sizeof(bit_word_t) / 8` (number of words in 1 quantum).
     * If it already equals `m_bitmap_words.size()` -- i.e., `S_USE_COUNTS_CAPACITY / sizeof(bit_word_t) / 8`
     * (all the available quanta) -- then allocate() fails (no slots available).
     */
    size_t m_bitmap_words_sz;

    /**
     * In the range [0, m_bitmap_words_sz), this is the index into #m_bitmap_words where the next search
     * (in allocate()) will begin.  Simply, this is the same word that the last allocate() found the 0-bit in;
     * unless that bit was the last bit of the words; in which case it is the next word; unless that word would
     * equal #m_bitmap_words_sz; then it is 0.
     *
     * So it'll search the same word again if there are bits left at the time in it (yes, that means potentially
     * searching through some bits we already have seen to be 1), or the next word (wrapping around the
     * active quanta) if none left in that word.
     *
     * Note for context: deallocate() could occur before the next allocate(); it could open up more 0 bits in
     * the current word; so none of this is an exact science.  It is a heuristic algorithm meant to keep moving
     * forward/around so as to hopefully find 0-bits quickly each time.
     */
    size_t m_bitmap_words_next_search_idx;

    /**
     * The bitmap, where 0 represents a free slot; 1 a taken slot.  Starts all zeroes; first special `allocate()`
     * sets a small (at least 1) number of words at the start to consist of only 1-bits -- corresponding to the
     * metadata area albeit rounded up to whole words for simplicity.  These are never freed, as that `allocate()`
     * cannot be `deallocate()`d.  The rest of the bits are for the remaining (actual) `[de]allocate()` calls.
     */
    Bitmap_words m_bitmap_words;
  }; // struct Header

  // Data.

  /// See Header doc header.
  Header m_header;
}; // class Use_count_registry

// Template and constexpr implementations.

constexpr size_t Use_count_registry::get_min_size(size_t extra_hdr_sz)
{
  using flow::util::round_to_multiple;

  /* Using similar technique to what boost.ipc rbtree_best_fit uses here.
   * I *think* this *might* be overzealous, estimating *maybe* too conservatively where C++ would actually plop
   * the various parts... but if so then only a little, and it is safe.
   *
   * Do note that, as of this writing, the true size used (with all allocate() space used-up to the byte)
   * is an exact number; we cannot grow (or shrink) from there.  So get_min_size() is not just the minimal
   * size of the SHM-pool; it is also the *exact* size actually (at max) *used*: we will specifically
   * set (see our ctor, where we set m_data_start_minus_this) the start of the NxUSE_COUNTS_CAPACITY-sized
   * allocate()-used area to get_min_size(extra_hdr_size) minus NxUSE_COUNTS_CAPACITY.  Empirically speaking, at least,
   * as of this writing if one sets the SHM-pool size to `segment_manager::get_min_size() + S_ASSUMED_BASE_OFFSET`,
   * it is enough, and things work; and if one sets it to a byte less -- a boost.ipc internal assertion trips
   * due to insufficient size.
   *
   * So first C++ shall (basically) put, like, a `struct { Header ...; segment_manager sized extra_hdr_sz; };`.
   * Those should take up at worst a tight multiple of the strictest alignment which is ALIGN_SZ.
   * Next is our data area, which is USE_COUNTS_CAPACITYxALLOC_SZ bytes, as ALLOC_SZ-wide slots.  However for certain
   * reasons (see comment elsewhere for rationale) we choose to start that area on the lowest possible
   * multiple of PAGE_SZ.  Hence however much we think is conservatively required for that fake "struct" --
   * round *that* further up to PAGE_SZ... then the data area.
   *
   * ...Oh... but also... see doc header for S_ASSUMED_BASE_OFFSET, then come back here.  Long story short:
   * `*this` actually shall be placed after a short additional header (ALIGN_SZ-multiple-sized like the other
   * stuff), so include that in the calculation to leave the start of NxUSE_COUNTS_CAPACITY data area
   * PAGE_SZ-aligned versus the start of the containing SHM-pool. */

  static_assert((S_ASSUMED_BASE_OFFSET % S_ALIGN_SZ) == 0, "boost.ipc is known to align this stringently.");
  return round_to_multiple(S_ASSUMED_BASE_OFFSET
                             + round_to_multiple(sizeof(Header), S_ALIGN_SZ)
                             + round_to_multiple(extra_hdr_sz, S_ALIGN_SZ),
                           S_PAGE_SZ)
         + (S_USE_COUNTS_CAPACITY * S_ALLOC_SZ)
         /* The sum so far = actual desired (min/forever) pool size, starting from byte 0 of ASSUMED_BASE_OFFSET-header
          * and ending at last byte of our data-area.  And it's sized in such a way, if we did the rounding math
          * right, that data-area begins on a page's byte 0, relative to the pool start.  A bunch of slack likely
          * precedes this byte 0, due to the rounding-up of previous areas to the nearest PAGE_SZ.
          *
          * That's great.  However, we aren't being asked to return *that* size, but the size relative to `this`
          * (== &m_header), which sits after ASSUMED_BASE_OFFSET bytes.  Hence: */
         - S_ASSUMED_BASE_OFFSET;
  /* @todo Check an above thing empirically: see if data-area minus actual-pool-start mod PAGE_SZ, is zero.
   * assert() or unit-test it somewhere.  Just be careful to get "actual-pool-start" at the right/sufficiently
   * high level (probably not in here), so that it is convincing without having to assume some of the same arithmetic
   * we are assuming here w/r/t what area starts where versus what other address. */
}

template<typename Metadata>
Metadata* Use_count_registry::get_metadata()
{
  // Not allocate()d yet (possibly by other process), or allocate()d to apparently the wrong size: return null.
  return (m_header.m_meta_obj_sz && (sizeof(Metadata) == *m_header.m_meta_obj_sz))
           ? reinterpret_cast<Metadata*>(reinterpret_cast<uint8_t*>(this) + m_header.m_data_start_minus_this)
           : nullptr;
}

} // namespace ipc::shm::arena_lend::detail
