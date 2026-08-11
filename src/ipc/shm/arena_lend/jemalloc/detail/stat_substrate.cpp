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
#include "ipc/shm/arena_lend/jemalloc/detail/stat_substrate.hpp"
#include <flow/util/util.hpp>
#include <vector>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cassert>
#include <flow/util/stat/histo.hpp>

namespace ipc::shm::arena_lend::jemalloc::detail::stat
{

// Implementations.

bool config_stats_enabled()
{
  /* config.stats is a compile-time constant within a given jemalloc build, so read it once and cache.
   * Note: the config.* paths are available regardless of build flags (jemalloc ctl.c); a read failure here
   * would thus be unexpected, but we conservatively treat any such failure as "stats unavailable." */
  static const bool s_enabled = ([]() -> bool
  {
    bool enabled = false;
    return mallctl_read("config.stats", &enabled) && enabled;
  })();
  return s_enabled;
}

size_t page_size()
{
  static const size_t s_page_size = ([]() -> size_t
  {
    size_t page = 0;
    mallctl_read("arenas.page", &page);
    return page;
  })();
  return s_page_size;
}

void epoch_refresh()
{
  /* Writing any value to "epoch" advances jemalloc's stats snapshot; the read-back (old epoch) is ignored.
   * We pass the value as the new-value argument; the old-value out-args are left null. */
  uint64_t epoch = 1;
  IPC_SHM_ARENA_LEND_JEMALLOC_API(mallctl)("epoch", nullptr, nullptr, &epoch, sizeof(epoch));
}

bool mallctl_name_to_mib(const char* name, size_t* mib, size_t* mib_len)
{
  return IPC_SHM_ARENA_LEND_JEMALLOC_API(mallctlnametomib)(name, mib, mib_len) == 0;
}

void cumulative_alloc_dealloc_sz(arena_id_t arena_id, uint64_t* alloc_sz_ptr, uint64_t* dealloc_sz_ptr,
                                 flow::util::stat::Histogram_counter* alloc_count_by_size_ptr)
{
  using flow::util::ostream_op_string;
  using flow::util::stat::Histogram_counter;
  using value_t = Histogram_counter::value_t;
  using std::vector;
  using std::array;
  using std::copy_n;
  using uint = unsigned int;

  assert(alloc_sz_ptr && dealloc_sz_ptr && alloc_count_by_size_ptr);
  auto& alloc_sz = *alloc_sz_ptr;
  auto& dealloc_sz = *dealloc_sz_ptr;
  auto& histo = *alloc_count_by_size_ptr;

  /* Init-once (process-wide constants): per-size-class byte-sizes + the resolved MIBs for the per-arena
   * per-size-class cumulative counters.  jemalloc's size classes (and their sizes) are fixed for the process;
   * only the arena/class indices vary per read -- mutated into copies of the cached MIBs below.  Small classes
   * are "bins"; large classes are "lextents"; their byte-sizes are contiguous and strictly increasing across the
   * join (bins end ~14 KiB, lextents start ~16 KiB).  We cache their concatenation (m_all_szs) -- used both for
   * the cumulative-bytes math and to derive the histogram's request-range bucket bounds (m_histo_lo_bounds +
   * m_histo_last_bucket_sz; see the by-size histogram member docs in arena_lend_stats.hpp).  (Max MIB depth for
   * these = 6, e.g. stats.arenas.i.bins.j.nmalloc.) */
  struct Tables
  {
    vector<value_t> m_bin_szs; // small (bin) size-class byte-sizes.
    vector<value_t> m_lext_szs; // large (lextent) size-class byte-sizes.
    vector<value_t> m_all_szs; // m_bin_szs plus m_lext_szs: per-class byte-sizes for the cumulative-bytes math.
    vector<value_t> m_histo_lo_bounds; // Request-range histogram bucket lower-bounds: [1, s0+1, s1+1, ...].
    value_t m_histo_last_bucket_sz = 1; // Last histogram bucket's width (spans up to the top class size).
    array<size_t, 6> m_bin_nmalloc_mib, m_bin_ndalloc_mib;
    array<size_t, 6> m_lext_nmalloc_mib, m_lext_ndalloc_mib;
    size_t m_bin_mib_len = 6, m_lext_mib_len = 6;
  };
  static const Tables s_tables = ([]() -> Tables
  {
    Tables tables;

    uint n_bins = 0;
    mallctl_read("arenas.nbins", &n_bins);
    tables.m_bin_szs.resize(n_bins);
    for (uint idx = 0; idx != n_bins; ++idx)
    {
      mallctl_read(ostream_op_string("arenas.bin.", idx, ".size").c_str(), &tables.m_bin_szs[idx]);
    }

    uint n_lext = 0;
    mallctl_read("arenas.nlextents", &n_lext);
    tables.m_lext_szs.resize(n_lext);
    for (uint idx = 0; idx != n_lext; ++idx)
    {
      mallctl_read(ostream_op_string("arenas.lextent.", idx, ".size").c_str(), &tables.m_lext_szs[idx]);
    }

    tables.m_all_szs.reserve(tables.m_bin_szs.size() + tables.m_lext_szs.size());
    tables.m_all_szs.insert(tables.m_all_szs.end(), tables.m_bin_szs.begin(), tables.m_bin_szs.end());
    tables.m_all_szs.insert(tables.m_all_szs.end(), tables.m_lext_szs.begin(), tables.m_lext_szs.end());

    /* Histogram bucket bounds = request ranges (The Law): bucket i counts allocations whose requested size N
     * rounded up to size-class i, i.e. N in (prev-class, this-class].  So the lower bound of bucket i = (prev
     * class size) + 1: [1, s0+1, s1+1, ..., s_{n-2}+1]; the last bucket spans up to the top class size. */
    const auto n_all = tables.m_all_szs.size();
    tables.m_histo_lo_bounds.resize(n_all);
    tables.m_histo_lo_bounds[0] = 1;
    for (size_t idx = 1; idx != n_all; ++idx)
    {
      tables.m_histo_lo_bounds[idx] = (tables.m_all_szs[idx - 1] + 1);
    }
    tables.m_histo_last_bucket_sz = (tables.m_all_szs[n_all - 1] - tables.m_all_szs[n_all - 2]);

    /* Resolve the MIB templates (placeholder indices 0).  Per the jemalloc MIB convention the arena index lives
     * at slot [2] and the size-class index at slot [4]; we mutate those per read below. */
    mallctl_name_to_mib("stats.arenas.0.bins.0.nmalloc", tables.m_bin_nmalloc_mib.data(), &tables.m_bin_mib_len);
    auto len = size_t(6);
    mallctl_name_to_mib("stats.arenas.0.bins.0.ndalloc", tables.m_bin_ndalloc_mib.data(), &len);
    mallctl_name_to_mib("stats.arenas.0.lextents.0.nmalloc", tables.m_lext_nmalloc_mib.data(), &tables.m_lext_mib_len);
    len = 6;
    mallctl_name_to_mib("stats.arenas.0.lextents.0.ndalloc", tables.m_lext_ndalloc_mib.data(), &len);

    return tables;
  })(); // static const Tables s_tables =

  alloc_sz = dealloc_sz = 0;
  // The histogram: fresh, zero-count, with the constant request-range bounds (see by-size histogram docs).
  histo = Histogram_counter{s_tables.m_histo_lo_bounds, s_tables.m_histo_last_bucket_sz};

  /* For each size-class idx: sum (count x size-class-byte-size) into the scalar totals, with mib[2] = arena and
   * mib[4] = class index; AND overwrite the matching `histo` bucket from the nmalloc count.  The histogram's
   * buckets are [bins | lextents] in order, so bins occupy [0, nbins) and lextents [nbins, nbins + nlext) --
   * hence the per-walk bucket_offset (0 for bins, nbins for lextents). */
  const auto walk = [&](const auto& nmalloc_mib, const auto& ndalloc_mib,
                        size_t mib_len, const vector<value_t>& szs, size_t bucket_offset)
  {
    array<size_t, 6> mib_n;
    array<size_t, 6> mib_d;
    copy_n(nmalloc_mib.data(), mib_len, mib_n.data());
    copy_n(ndalloc_mib.data(), mib_len, mib_d.data());
    mib_n[2] = mib_d[2] = arena_id;
    for (size_t idx = 0; idx != szs.size(); ++idx)
    {
      mib_n[4] = mib_d[4] = idx;
      uint64_t count_n = 0;
      uint64_t count_d = 0;
      mallctl_read_mib(mib_n.data(), mib_len, &count_n);
      mallctl_read_mib(mib_d.data(), mib_len, &count_d);
      const auto sz = uint64_t(szs[idx]);
      alloc_sz += (count_n * sz);
      dealloc_sz += (count_d * sz);
      histo.overwrite_count_for_bucket(bucket_offset + idx, count_n);
    }
  }; // const auto walk =

  walk(s_tables.m_bin_nmalloc_mib, s_tables.m_bin_ndalloc_mib,
       s_tables.m_bin_mib_len, s_tables.m_bin_szs, 0);
  walk(s_tables.m_lext_nmalloc_mib, s_tables.m_lext_ndalloc_mib,
       s_tables.m_lext_mib_len, s_tables.m_lext_szs, s_tables.m_bin_szs.size());
} // cumulative_alloc_dealloc_sz()

} // namespace ipc::shm::arena_lend::jemalloc::detail::stat
