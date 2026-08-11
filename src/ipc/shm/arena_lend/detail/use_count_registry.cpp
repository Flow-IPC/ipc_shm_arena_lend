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
#include "ipc/shm/arena_lend/detail/use_count_registry.hpp"
#include "ipc/shm/arena_lend/arena_lend_stats.hpp"
#include <flow/util/util.hpp>
#include <flow/util/stat/stat_set.hpp>
#include <algorithm>
#include <cassert>

namespace ipc::shm::arena_lend::detail
{

Use_count_registry::Use_count_registry(size_t sz, size_t extra_hdr_sz) :
  m_header({ sz,
             extra_hdr_sz,
             /* m_data_start_minus_this: This is actually quite important.  (See its doc header for reason why
              * we are storing it instead of placing it into a function... and more.)  We can reuse get_min_size(),
              * as it assumes the layout we want. */
             get_min_size(extra_hdr_sz) - (S_USE_COUNTS_CAPACITY * S_ALLOC_SZ),
             0,
             {}, 0, 0, {} }) // Would be fine to leave them uninitialized temporarily (compiler doesn't like).
{
#if 0
  m_header.m_bitmap_words.assign(0);

  /* @todo The above `.assign(0)` is defensive, probably removable; and the benefit could be lowered RAM use.
   * Explanation: bipc is wrangling OS SHM facilities as a fairly thin
   * portability+niceness layer; in POSIX OS at least (probably Windows too but need to confirm if it becomes
   * relevant; as of this writing we're Linux-only officially) it's: ::shm_open() + ::ftruncate() => the vaddr range
   * is already zero-filled, and the pages are not yet RSS-resident (a/k/a dirty-pages).  Without .assign(0)
   * it should all still work the same.  Removing it (the potential to-do) would drop initial dirty-RAM by
   * ~S_BITMAP_SZ bytes; bitmap pages would dirty on-demand as allocate() touches them, ~proportionally to
   * m_header.m_bitmap_words_sz growing.  (So if there are 8 quanta total, we start with 1 quantum: about 0/8
   * of BITMAP_SZ will be dirty to start; then the algorithm is such that the entire quantum has to be filled
   * before adding qunatum 2 -- at that point ~1/8 of BITMAP_SZ is dirty.  If both quanta get totally used-up,
   * meaning there are that many live objects, then 2/8 of BITMAP_SZ is dirty.  Etc.)  Bottom line -- this can
   * at best save BITMAP_SZ actual-RAM; as of this writing 128Ki.  The more live objects there are -- up to
   * USE_COUNTS_CAPACITY of them -- the less is saved from the bitmap area.  This appears to be "free" and involves
   * commenting-out/deleting one line; but verify the bipc create-only path's zero-fill guarantee before
   * doing so. */
#endif
  /* m_header.m_bitmap_words is pre-zero-filled, as is required for our algorithm (all parts of the data area
   * area are available; hence all bits in bitmap = 0).
   * (We leave the historical explicit .assign(0) and @todo (now to-done) for posterity, as it includes the
   * justification/reasoning for omitting it.) */

  /* The following m_header stuff is effectively garbage until .m_meta_obj_sz (currently null) gets set by first
   * allocate(); at that point we set these to real init values:
   *   m_meta_obj_sz, m_bitmap_words_sz, m_bitmap_words_next_search_idx. */
} // Use_count_registry::Use_count_registry()

void* Use_count_registry::allocate(size_t sz)
{
  using flow::util::ceil_div;
  using std::fill_n;
  using std::find_if;

  /* Size, in words (m_bitmap_words array elements), of a bitmap section corresponding to a quantized subsection
   * of the main data area (which is S_USE_COUNTS_CAPACITY slots divided by some integer; e.g., by 32). */
  constexpr size_t BITMAP_QUANTUM_WORDS = S_BITMAP_QUANTUM_SZ / sizeof(bit_word_t);
  constexpr size_t BITMAP_WORDS_MAX_SZ = decltype(m_header.m_bitmap_words)::static_size;

  const bool is_normal_alloc = bool(m_header.m_meta_obj_sz);

  const auto data_start = reinterpret_cast<uint8_t*>(this) + m_header.m_data_start_minus_this;
  const auto bitmap_words_start = m_header.m_bitmap_words.begin();

  if (!is_normal_alloc)
  {
    // Note: This is not the fast-path.

    m_header.m_meta_obj_sz.emplace(sz); // Mark their header as allocated -- and how big it is (see get_metadata()).

    // Note that (sz == 0) is allowed.  The logic below will work (essentially no-op).

    /* sz bytes => ceil(sz / ALLOC_SZ) data-area slots => ceil(sz / ALLOC_SZ) bitmap bits
     *   => ceil(ceil(sz / ALLOC_SZ) / 8) bytes to holds ceil(sz / ALLOC_SZ) bitmap bits.  Call this K.
     * => ceil(K / sizeof(word)) words to hold K bytes.  Call that M.
     *  - Set all those bits to 1 (a/k/a set all those words to 0b111111...1).
     *  - Next search will begin at the word right after that (its LSB shall in fact be 0).
     *
     * We could set the exact bits instead, but it is annoying and not worth it; just use the entire words.
     * This will potentially waste a few bits and corresponding N-byte groups in the data area -- which is fine. */
    m_header.m_bitmap_words_next_search_idx = ceil_div(ceil_div(ceil_div(sz, S_ALLOC_SZ),
                                                                size_t(8)),
                                                       sizeof(bit_word_t));
    fill_n(bitmap_words_start,
           m_header.m_bitmap_words_next_search_idx,
           ~(bit_word_t(0)));

    /* Searches will keep going around just the bitmap section [0, BITMAP_QUANTUM_WORDS) words.
     * (When/if those run out, the range will expand to [0, 2 x BITMAP_QUANTUM_WORDS).
     * When/if those run out, [0, 3 x ...]... etc., until m_bitmap_words_sz == m_bitmap_words.size().
     * Then allocation shall fail.) */
    m_header.m_bitmap_words_sz = BITMAP_QUANTUM_WORDS;
    m_header.m_allocated_sz
      += (m_header.m_bitmap_words_next_search_idx // We actually used up this rounded amount, not `sz`.
          * sizeof(bit_word_t) * 8 * S_ALLOC_SZ);
    return data_start;
  }
  // else if (is_normal_alloc):

  if (sz != S_ALLOC_SZ)
  {
    return nullptr; // Be forgiving as promised.  @todo Maybe should assert() instead?
  }
  // else if (sz != 0):

  // This is the fast-path.  Needless to say performance is of the essence.

  /* Strategy is:
   *   - m_bitmap_words_next_search_idx is index of word where to begin search (generally we move it forward to
   *     resume search ~just past where the last one succeeded, wrapping around -- until no more available bits left,
   *     then expand the "playing field" until no more quanta left; then allocate() returns null).
   *   - Search 1 is from there until the end of the "playing field"; if no 0 bit found anywhere there, wrap around
   *     and do search 2 from start of "playing field" through just ahead of where search 1 started.  So each
   *     search (search 1 and search 2) is a find_if() through the words in that section of the bitmap.
   *   - Each find_if() does a bit search through a single word (uint64_t as of this writing) using an optimized
   *     intrinsic suited for this.  The intrinsic finds the LSB that is 0.
   * The <playing field> looks like:
   *   <[BITMAP_QUANTUM_WORDS words][BITMAP_QUANTUM_WORDS words]>[BITMAP_QUANTUM_WORDS words]...
   * In that example at some point we ran out of the 1st quantum and had expanded the <field> from
   * just the first quantum (the initial setup) to the 1st 2 quanta.  So keep doing that as needed, until
   * all the quanta are used up.
   *
   * Regarding find_if(): At least in gcc-9 STL, there's a random-accessor-iterator loop-unroll specialization
   * of find_if(), so this can actually help perf in this search, versus a manual `for` and the like. */

  unsigned int zero_bit_idx{}; // Initialize to avoid (overzealous) compiler warning in some cases.
  const auto search_start = bitmap_words_start + m_header.m_bitmap_words_next_search_idx;
  auto search_end = bitmap_words_start + m_header.m_bitmap_words_sz;
  auto bitmap_words_addr = find_if(search_start, search_end,
                                   [&](bit_word_t bits)
  {
    static_assert(sizeof(bit_word_t) == sizeof(long long),
                  "We assume that `long long` is 64-bits, as our stuff is 64-bits, and we use __builtin_ffsll()");
    return (zero_bit_idx = static_cast<unsigned int>(::__builtin_ffsll(static_cast<int64_t>(~bits)))) != 0;
  });
  if (zero_bit_idx == 0)
  {
    assert((bitmap_words_addr == search_end) && "Bug in above search?!");

    search_end = bitmap_words_start + m_header.m_bitmap_words_next_search_idx;
    bitmap_words_addr = find_if(bitmap_words_start, search_start,
                                [&](bit_word_t bits)
    {
      return (zero_bit_idx = static_cast<unsigned int>(::__builtin_ffsll(static_cast<int64_t>(~bits)))) != 0;
    });
    if (zero_bit_idx == 0)
    {
      assert((bitmap_words_addr == search_start) && "Bug in above search?!");

      if (m_header.m_bitmap_words_sz == BITMAP_WORDS_MAX_SZ)
      {
        return nullptr; // Ran out!  allocate() failed.
      }
      // else

      // We shall add a new chunk to playing field... all unallocated... hence 1st bit of 1st word = result of search.
      bitmap_words_addr = &m_header.m_bitmap_words[m_header.m_bitmap_words_sz];
      zero_bit_idx = 1;

      // Mark the wider playing field.
      m_header.m_bitmap_words_sz += BITMAP_QUANTUM_WORDS;
      assert((m_header.m_bitmap_words_sz <= BITMAP_WORDS_MAX_SZ)
             && "Did we not static_assert() that m_bitmap_words is evenly divided?");

      // ...and fall through.
    }
    // else if (zero_bit_idx != 0) [search 2]: fall through.
  } // else if (zero_bit_idx != 0) [search 1]: fall through.

  /* Search succeeded, but let's get the actual result "coordinates": which word `bitmap_words_idx` and which
   * (1-based) bit `zero_bit_idx` it is.  Err, we have the latter already (just remember it is 1-based). */
  const auto bitmap_words_idx = bitmap_words_addr - bitmap_words_start;

  // In fact mark it in the bitmap as now taken (turn the 0 into 1).
  const auto mask = bit_word_t(1) << (zero_bit_idx - 1); // (Careful; z_b_i==1 => bit 0, 2 => bit 1, etc.)
  assert(((*bitmap_words_addr & mask) == bit_word_t(0)) && "Did we mess up the above logic/use of intrinsic?");

  *bitmap_words_addr |= mask;

  // Set where to start the next search: same word (we hope the next LSB shall be 0)...
  m_header.m_bitmap_words_next_search_idx = bitmap_words_idx;
  // ...except that if, like, the LSB with the 0 (now 1!) is bit 64 of 64...
  if (zero_bit_idx == (sizeof(bit_word_t) * 8))
  {
    // ...then begin next search not in that word (since, at least right now, it is all 1s) but in the next word, and...
    m_header.m_bitmap_words_next_search_idx = (m_header.m_bitmap_words_next_search_idx + 1)
                                              % m_header.m_bitmap_words_sz; // ...wrap around, if we're on last word.
  }

  // Mark stats.
  m_header.m_allocated_sz += S_ALLOC_SZ;

  // Last but not least return the address of N-byte slot that all our bit-searching has enabled us to pinpoint.
  const auto addr = data_start
                    + (((bitmap_words_idx * sizeof(bit_word_t) * 8) + zero_bit_idx - 1)
                       * S_ALLOC_SZ);
  assert(flow::util::in_closed_range(data_start, addr, data_start + ((S_USE_COUNTS_CAPACITY - 1) * S_ALLOC_SZ))
         && "alloc() returning invalid addr: out of range.");
  return addr;
} // Use_count_registry::allocate()

void Use_count_registry::deallocate(void* void_addr)
{
  const auto addr = reinterpret_cast<uint8_t*>(void_addr);
  if (!addr)
  {
    return;
  }
  // else

  const auto data_start = reinterpret_cast<uint8_t*>(this) + m_header.m_data_start_minus_this;

  if (addr == data_start) // The first (metadata) allocate() returned this.
  {
    return; // By contract we allow this (no abort/throw/etc.) but treat it as no-op.  Rationale shown elsewhere.
  }
  // else

  // Essentially this is the reverse of the last line of allocate().

  assert((addr >= data_start) && "Invalid dealloc(addr): addr out of range (too low).");
  auto offset_from_data_start = static_cast<size_t>(addr - data_start);

  // offset_from_data_start right now is in bytes.
  assert(((offset_from_data_start % S_ALLOC_SZ) == 0)
         && "Invalid dealloc(addr): addr offset from data_start is not a multiple of ALLOC_SZ.");
  offset_from_data_start /= S_ALLOC_SZ;

  // offset_from_data_start right now is in slots.  (In the bitmap, it is 1 bit per slot).
  assert((offset_from_data_start < S_USE_COUNTS_CAPACITY) && "Invalid dealloc(addr): addr out of range (too high).");

  constexpr size_t BITS_PER_WORD = sizeof(bit_word_t) * 8;
  m_header.m_bitmap_words[offset_from_data_start / BITS_PER_WORD]
    &= (~(bit_word_t(1) << (offset_from_data_start % BITS_PER_WORD)));

  m_header.m_allocated_sz -= S_ALLOC_SZ;
} // Use_count_registry::deallocate()

size_t Use_count_registry::stat_quanta_active() const
{
  /* Take a look at allocate() + our doc header; it should make the following clear.
   * The K from our doc header -- the compile-time constant by which the # of quanta (1, 2, ...) is multiplied
   * to get m_bitmap_words_sz -- is allocate()'s BITMAP_QUANTUM_WORDS. */
  return m_header.m_meta_obj_sz ? m_header.m_bitmap_words_sz // Starts at 1xK; then 2xK, 3xK, etc.
                                : 0; // No allocate()s yet: pool was just created, not yet used.
}

size_t Use_count_registry::stats_record(size_t prev_quanta_active, Obj_db_aux_pool_stats* target_stats)
{
  using flow::util::stat::fetch_add;
  using flow::util::stat::update_hi_wmark;

  // Look at stat_quanta_active() and our doc headers; will introduce the algorithm.

  const auto quanta_active_x_bitmap_qtm_words = stat_quanta_active();
  if (quanta_active_x_bitmap_qtm_words == prev_quanta_active)
  {
    return quanta_active_x_bitmap_qtm_words; // Fast-path: No-op.  Nothing has changed since *target_stats was set.
  }
  assert((quanta_active_x_bitmap_qtm_words > prev_quanta_active)
         && "It cannot change and go down; see allocate() et al.");
  /* else: Let's do it.  Important to understand Obj_db_aux_pool_stats docs from here on.
   *
   * Slow-path, so we don't *need* to over-worry about saving every cycle; might as well though. */

  /* The divisor equals BITMAP_QUANTUM_WORDS from allocate().  Some code reuse might be nice -- maybe @todo --
   * but even without it this follows from the definition of m_bitmap_words_sz (stat_quanta_active()). */
  const auto quanta_added = static_cast<uint64_t>((quanta_active_x_bitmap_qtm_words - prev_quanta_active)
                                                  / (S_BITMAP_QUANTUM_SZ / sizeof(bit_word_t)));

  /* INC_PER_QTM_SZ is ~how much memory a fully resident (ever-touched) quantum takes:
   * the data area + the bitmap area where each bit <=> ALLOC_SZ bytes in data area.
   * Regarding the bitmap part: we no longer do the m_bitmap_words.assign(0) in our ctor (as it's pre-filled
   * to zeroes anyway), so at least roughly speaking the bitmap area is made resident in the same proportions
   * as the data area. */
  constexpr auto INC_PER_QTM_SZ = (S_USE_COUNTS_CAPACITY_QUANTUM_SZ * S_ALLOC_SZ)
                                  + S_BITMAP_QUANTUM_SZ;
  auto inc_sz = quanta_added * INC_PER_QTM_SZ;

  update_hi_wmark(&target_stats->m_use_ct_active_quanta_hi_wmark,
                  fetch_add(&target_stats->m_use_ct_active_quanta, quanta_added) + quanta_added);
  if (prev_quanta_active == 0)
  {
    /* As promised we consider the pool as non-existent until an allocate() happens; as in practice the pool
     * is created when and only when the first use-count is required; and the creation is immediately therefore
     * followed by allocate(sizeof(Metadata)) -- and for that matter the first allocate(sizeof(<use-count>)).
     * Regardless of that latter "for that matter," this means they must call stat_quanta_active() before
     * that first allocate() (returns 0) and then after the first or second allocate(); so going from 0 is
     * when to ++m_aux_pool_count (and at no other time).  There isn't really a "setup" time period when
     * there's a new ++m_aux_pool_count-contributing guy but no ++m_use_ct_active_quanta being contributed by that same
     * guy; they each contribute at the same time initially. */
    update_hi_wmark(&target_stats->m_aux_pool_count_hi_wmark,
                    fetch_add(&target_stats->m_aux_pool_count, 1) + 1);

    /* By the same logic, when going from 0 to 1+ qta is when to count the resident RAM taken by all the header
     * stuff.  After that, it's already counted.  Now let's be careful here:
     *   - m_data_start_minus_this = size of entire, full pool relative to &m_header (== this), minus the
     *     entire, full data area.  What is between &m_header and data-area?  Answer: definitely-used stuff
     *     that we should totally count as resident... including `sizeof(m_header) == sizeof(Header)` in fact...
     *   - ...except that not all of `Header m_header` is resident!  By our model all of it is, except
     *     that there is a potential hole in m_header.m_bitmap_words; the unused quanta's part of the bitmap.
     *     We already counted the *used* part in inc_sz above.  Therefore subtract it from this addition
     *     to inc_sz; the addition is to exclude the quanta.
     *   - There's also the little pre-`*this` header that technically also takes memory, S_ASSUMED_BASE_OFFSET.
     *     Including it is pedantic as hell (especially given that we're not, like, ultra-precise about which
     *     actual pages are actually resident -- among other things we assume the entire quantum is paged-in
     *     from the jump; not true).  And yet! */
    inc_sz += ((m_header.m_data_start_minus_this - S_BITMAP_SZ) + S_ASSUMED_BASE_OFFSET);

    /* These two GAUGEs are to include the max possible values for m_use_ct_active_quanta and m_resident_sz
     * respectively; meaning one can see "total of m_use_ct_active_quanta of m_use_ct_quanta in-use" and
     * "total of m_resident_sz of m_mapped_sz mapped RAM are current resident."  So, like m_aux_pool_count,
     * the entire contribution happens one time, when #-quanta goes from 0 to 1+.
     *
     * The m_use_ct_quanta value is not strictly necessary, as it could be computed from m_aux_pool_count:
     * multiply that by # of quanta, hence by MAX_QUANTA below; but we've decided to track it as-if the # of max quanta
     * per pool is not a compile-time decision; the perf impact should be negligible due to relative rarity
     * this code executing. */
    constexpr auto MAX_QUANTA = S_USE_COUNTS_CAPACITY / S_USE_COUNTS_CAPACITY_QUANTUM_SZ;
    const auto mapped_sz = get_min_size(m_header.m_extra_hdr_sz) + S_ASSUMED_BASE_OFFSET;

    static_assert(MAX_QUANTA == (S_BITMAP_SZ / S_BITMAP_QUANTUM_SZ), "Something is off somewhere....");
    assert((mapped_sz == (((MAX_QUANTA - quanta_added) * INC_PER_QTM_SZ) + inc_sz))
           && "Something is off somewhere... get_min_size()+ASSUMED_BASE_OFFSET should = all hdr+all quanta.");

    update_hi_wmark(&target_stats->m_use_ct_quanta_hi_wmark,
                    fetch_add(&target_stats->m_use_ct_quanta, MAX_QUANTA) + MAX_QUANTA);
    update_hi_wmark(&target_stats->m_mapped_sz_hi_wmark,
                    fetch_add(&target_stats->m_mapped_sz, mapped_sz) + mapped_sz);
  } // if (prev_quanta_active == 0)
  update_hi_wmark(&target_stats->m_resident_sz_hi_wmark,
                  fetch_add(&target_stats->m_resident_sz, inc_sz) + inc_sz);

  return quanta_active_x_bitmap_qtm_words;
} // Use_count_registry::stats_record()

size_t Use_count_registry::stats_record_at_deletion(Obj_db_aux_pool_stats* target_stats)
{
  using flow::util::stat::fetch_sub;

  /* See stats_record().  That should make the below resonably clear.
   * @todo A bit of code reuse w/r/t stats_record() w/r/t some of this math wouldn't hurt. */

  const auto quanta_active_x_bitmap_qtm_words = stat_quanta_active();

  if (quanta_active_x_bitmap_qtm_words == 0)
  {
    return 0; // Apparently they've never allocate()d, so nothing was counted.
  }
  // else
  const auto quanta_active = quanta_active_x_bitmap_qtm_words / (S_BITMAP_QUANTUM_SZ / sizeof(bit_word_t));
  constexpr auto MAX_QUANTA = S_USE_COUNTS_CAPACITY / S_USE_COUNTS_CAPACITY_QUANTUM_SZ;

  const auto mapped_sz = get_min_size(m_header.m_extra_hdr_sz) + S_ASSUMED_BASE_OFFSET;

  fetch_sub(&target_stats->m_aux_pool_count, 1);
  fetch_sub(&target_stats->m_use_ct_quanta, MAX_QUANTA);
  fetch_sub(&target_stats->m_mapped_sz, mapped_sz);
  fetch_sub(&target_stats->m_use_ct_active_quanta, quanta_active);
  fetch_sub(&target_stats->m_resident_sz,
            // resident_sz should be (mapped_sz - <unused quanta> x <1 quantum's contribution>)).
            mapped_sz - ((MAX_QUANTA - quanta_active)
                         * ((S_USE_COUNTS_CAPACITY_QUANTUM_SZ * S_ALLOC_SZ) + S_BITMAP_QUANTUM_SZ)));

  return quanta_active_x_bitmap_qtm_words;
} // Use_count_registry::stats_record_at_deletion()

void Use_count_registry::grow(size_t)
{
  // As advertised: no-op (rationale shown elsewhere).
}

size_t Use_count_registry::get_size() const
{
  return m_header.m_pool_sz;
}

size_t Use_count_registry::get_free_memory() const
{
  return (S_USE_COUNTS_CAPACITY * S_ALLOC_SZ) - m_header.m_allocated_sz;
}

} // namespace ipc::shm::arena_lend::detail
