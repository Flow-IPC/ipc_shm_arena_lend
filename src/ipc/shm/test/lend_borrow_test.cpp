/* Flow-IPC
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

#include "ipc/transport/struc/test/test_util.hpp"
#include "ipc/shm/classic/pool_arena.hpp"
#include "ipc/test/test_logger.hpp"
#include <flow/log/log.hpp>
#include <flow/util/basic_blob.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

/* These tests are generally named (Lend_borrow_test.*) and placed in a general namespace/dir (ipc::shm::test,
 * albeit as of this writing under ipc_shm_arena_lend rather than ipc_shm).  The precipitating changes that caused
 * us to write these concerned a hardening of inputs into Session::borrow_object() (which can be used generically
 * regarding of SHM-provider but internally does different things for SHM-classic versus SHM-jemalloc) and
 * SHM-jemalloc Shm_session::borrow_object() + SHM-classic Pool_arena::borrow_object() (which can be used directly
 * but on somewhat differently-accessed objects, due to the essential difference between arena-sharing SHM-providers
 * (SHM-classic being the first/reference one) and arena-lending SHM-providers (SHM-jemalloc ditto)).
 *
 * That is, since either flavor (2 flavors x 2 SHM-providers) of borrow_object() (and some incidental related items)
 * concern accepting serialized (quite small) input, which the user is expected to IPC-over using a method of their
 * choice (can be Flow-IPC -- or not), when sharing native in-SHM objects, we have hardened these to be paranoid
 * about, e.g.: weird alignments, invalid inputs (wrong size, clearly-wrong contents).  So, we needed to test these.
 * Some of the impl details differ between SHM-providers (in particular at least because what each internally must
 * encode inside the aforementioned blob-to-borrow_object() = massively different) so we test both where appropriate
 * and, e.g., just SHM-classic where appropriate.
 *
 * Naturally it was also important to at least basically exercise the positive paths, where nothing is wrong, and
 * everything is working properly.  So we have these too.  In plainer terms, we do ensure
 * the essential construct-in-SHM->lend->borrow->use-in-other-process procedure works end-to-end.
 *
 * However note that this is *not* (as of this writing at least) meant to be exhaustive testing of the lend/borrow
 * machinery end-to-end, with everything this implies.  Lend/borrow is an absolutely central aspect of Flow-IPC's
 * SHM support regardless of SHM-provider.  There are many things to exercise beyond the outer-object SHM-handle
 * blob's processing; for example the whole shm::stl::Stateless_allocator machinery that enables sharing multi-level
 * STL-compliant containers.  Also, here we don't split up into different processes (or even threads), but a realistic
 * test must do so.  Moreover, SHM-classic and SHM-jemalloc do differ in their capabilities (e.g., the
 * latter is intentionally read-only when borrowing) and in their impls.  It's a massive topic.
 * This test is not (as of this writing) meant to address all of that, though even as we write this, there is much
 * functional/unit testing like that elsewhere.  E.g., functional test transport_test (exercise mode) is
 * intense in this regard, and there are SHM-jemalloc-specific unit tests (closer in the dir structure to the
 * relevant code). */

namespace ipc::shm::test
{

namespace
{
  using flow::log::Logger;
  using session::schema::MqType;
  using session::schema::ShmType;

  ipc::test::Test_logger g_logger_obj;
  Logger* const g_logger_console = &g_logger_obj;
#if 1
  Logger* const g_logger = nullptr; // Normal: Flow-IPC objects silent.
#else
  Logger* const g_logger = &g_logger_obj; // Flip for debugging.
#endif

  /* Odd-sized POD payload.  The odd size stresses alignment machinery in the allocator and makes
   * content comparisons easy to eyeball. */
  using Payload = std::array<uint8_t, 13>;

  constexpr Payload S_CANARY{ 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                              0x99, 0xAA, 0xBB, 0xCC, 0xDD };

  /* The trailing scope-ID tag that Session_impl::lend_object() appends to its serialization blob,
   * and that Session_impl::borrow_object() strips.  The type is Session_impl::scope_id_t = uint64_t
   * but that is not a public name; tests that poke the framing therefore hardcode the size. */
  constexpr size_t S_SCOPE_ID_SZ = sizeof(uint64_t);

  using Blob = flow::util::Blob_sans_log_context;

  /* Pair factory: no MQ, no native-handle transmission; only the ShmType varies.  Forwards the
   * SHM-classic pool-size cap to make_session_channel_pair(): 0 = leave default; otherwise cap
   * per-session pool at that many MiB.  Ignored unless SHM_TYPE == CLASSIC. */
  template<ShmType SHM_TYPE>
  auto make_session_pair(size_t shm_classic_pool_size_limit_mi_or_0 = 0)
  {
    return transport::struc::test::make_session_channel_pair<MqType::NONE, false, SHM_TYPE>
             (g_logger, false, shm_classic_pool_size_limit_mi_or_0);
  }

  /* Return a fresh Blob of exactly `new_size` bytes.  The first min(new_size, src.size()) bytes are
   * copied from `src`; any padding tail is zero-initialized (clear-on-alloc tag). */
  Blob make_resized_copy(const Blob& src, size_t new_size)
  {
    Blob dst{new_size, flow::util::Clear_on_alloc{}};
    const auto n = std::min(new_size, src.size());
    if (n != 0)
    {
      dst.emplace_copy(dst.begin(), util::Blob_const(src.const_data(), n));
    }
    return dst;
  }

  /* Return a fresh Blob whose logical begin() is `offset` bytes past the allocator-aligned buffer
   * start -- i.e., const_data() is misaligned by `offset` relative to natural alignment.  Content
   * matches `src` verbatim; size() == src.size(). */
  Blob make_misaligned_copy(const Blob& src, size_t offset = 1)
  {
    Blob dst;
    dst.resize(src.size() + offset);
    dst.start_past_prefix_inc(static_cast<Blob::difference_type>(offset));
    if (!src.empty())
    {
      const auto end_it = dst.emplace_copy(dst.begin(), util::Blob_const(src.const_data(), src.size()));
      EXPECT_EQ(end_it, dst.end());
    }
    return dst;
  }

  /* Return the lend/borrow engine of `sess`.  Abstracts over the SHM-classic (arena-sharing) vs
   * SHM-jemalloc (arena-lending) asymmetry: classic has no Shm_session (its single Pool_arena *is* the engine, per
   * session_shm()); jemalloc exposes a dedicated one via shm_session().  Both engines expose the
   * same lend_object()/borrow_object() surface.
   *
   * Note that this is only so straightforward for our test cases here, because *we* only use the session-scope arena
   * (sess.session_shm()).  If we needed to also lend a thing from app-scope arena (sess.app_shm()), then this would
   * have to be rewritten for the CLASSIC clause.  Indeed that is just what sess.lend/borrow_object() forwarder
   * does, and why it's useful -- so generically SHM-provider-agnostic code could (for these pre-setup arenas anyway)
   * just say sess.lend/borrow_object(), and that's that.  We do want to test these nevertheless-public APIs
   * too though.  (They're public in the first place, because while ipc::session makes setup and integration of
   * the SHM features far easier and generic that it would be otherwise, the SHM-providers *are* usable in standalone
   * fashion too.  It's a matter of layers/modules.)
   *
   * See ipc::shm doc header for a general definition of arena-sharing versus arena-lending SHM-providers. */
  template<ShmType SHM_TYPE, typename Session>
  auto* shm_engine(Session& sess)
  {
    if constexpr(SHM_TYPE == ShmType::CLASSIC)
    {
      return sess.session_shm();
    }
    else
    {
      return sess.shm_session();
    }
  }

  /* ---------- Shared tests: run on both SHM-classic and SHM-jemalloc. ---------- */

  /* Construct on the client side, lend via Session::lend_object(), borrow via
   * Session::borrow_object() on the server side, verify the borrowed view reads the same bytes the
   * lender wrote.
   *
   * For SHM-classic only, additionally write through the borrowed handle and verify the lender
   * sees the change (both sides map the same pool). */
  template<ShmType SHM_TYPE>
  void happy_session_level()
  {
    FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);
    FLOW_LOG_INFO("Session::lend_object()/borrow_object() round-trip; expect borrower reads lender's bytes.");

    auto pair = make_session_pair<SHM_TYPE>();
    auto& cli = pair.m_sessions->m_cli_session;
    auto& srv = pair.m_sessions->m_srv_session;

    auto h_cli = cli.session_shm()->template construct<Payload>(S_CANARY);
    ASSERT_NE(h_cli.get(), nullptr);

    const auto blob = cli.lend_object(h_cli);
    EXPECT_GT(blob.size(), 0u);

    /* Normally `blob` would be transmitted over (any) IPC between processes here; we aren't testing
     * that and can cheat by literally using the same one. */

    auto h_srv = srv.template borrow_object<Payload>(blob);
    ASSERT_NE(h_srv.get(), nullptr);
    EXPECT_EQ(*h_srv, S_CANARY);

    if constexpr(SHM_TYPE == ShmType::CLASSIC)
    {
      /* SHM-classic: borrower and lender map the same pool, so a write via the borrowed handle is
       * visible to the lender.  (SHM-jemalloc borrowers do not share that write view.) */
      FLOW_LOG_INFO("SHM-classic: write via borrower; expect lender sees it.");
      Payload altered{};
      altered.fill(0xEE);
      *h_srv = altered;
      EXPECT_EQ(*h_cli, altered);
    }
  }

  /* Same round-trip as happy_session_level(), but via the shm-session-level API (no scope-ID tag).
   * For SHM-classic that means Pool_arena::lend_object() / Pool_arena::borrow_object(); for
   * SHM-jemalloc it means Shm_session::lend_object() / Shm_session::borrow_object(). */
  template<ShmType SHM_TYPE>
  void happy_shm_session_level()
  {
    FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);
    FLOW_LOG_INFO("Shm-session-level lend_object()/borrow_object() round-trip.");

    auto pair = make_session_pair<SHM_TYPE>();
    auto& cli = pair.m_sessions->m_cli_session;
    auto& srv = pair.m_sessions->m_srv_session;

    auto h_cli = cli.session_shm()->template construct<Payload>(S_CANARY);
    ASSERT_NE(h_cli.get(), nullptr);

    const auto blob = shm_engine<SHM_TYPE>(cli)->lend_object(h_cli);
    EXPECT_GT(blob.size(), 0u);
    auto h_srv = shm_engine<SHM_TYPE>(srv)->template borrow_object<Payload>(blob);
    ASSERT_NE(h_srv.get(), nullptr);
    EXPECT_EQ(*h_srv, S_CANARY);
  }

  /* Shrink the shm-session-level serialization below the engine's fixed expected size.  Both
   * engines require an exact size match at this layer; the borrow must fail cleanly (null handle,
   * no crash). */
  template<ShmType SHM_TYPE>
  void sabotage_shm_level_too_small()
  {
    FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);
    FLOW_LOG_INFO("Shm-session-level borrow_object() on truncated blob; expect null.");

    auto pair = make_session_pair<SHM_TYPE>();
    auto& cli = pair.m_sessions->m_cli_session;
    auto& srv = pair.m_sessions->m_srv_session;

    auto h_cli = cli.session_shm()->template construct<Payload>(S_CANARY);
    ASSERT_NE(h_cli.get(), nullptr);

    const auto blob = shm_engine<SHM_TYPE>(cli)->lend_object(h_cli);
    ASSERT_GT(blob.size(), 0u);
    const auto truncated = make_resized_copy(blob, blob.size() - 1);
    auto h_srv = shm_engine<SHM_TYPE>(srv)->template borrow_object<Payload>(truncated);
    EXPECT_EQ(h_srv.get(), nullptr);
  }

  /* Pad the shm-session-level serialization past the engine's fixed expected size.  Same
   * expectation as too_small(): exact match is required, extra tail bytes must cause a clean null
   * return. */
  template<ShmType SHM_TYPE>
  void sabotage_shm_level_too_large()
  {
    FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);
    FLOW_LOG_INFO("Shm-session-level borrow_object() on padded blob; expect null.");

    auto pair = make_session_pair<SHM_TYPE>();
    auto& cli = pair.m_sessions->m_cli_session;
    auto& srv = pair.m_sessions->m_srv_session;

    auto h_cli = cli.session_shm()->template construct<Payload>(S_CANARY);
    ASSERT_NE(h_cli.get(), nullptr);

    const auto blob = shm_engine<SHM_TYPE>(cli)->lend_object(h_cli);
    ASSERT_GT(blob.size(), 0u);
    const auto padded = make_resized_copy(blob, blob.size() + 4);
    auto h_srv = shm_engine<SHM_TYPE>(srv)->template borrow_object<Payload>(padded);
    EXPECT_EQ(h_srv.get(), nullptr);
  }

  /* Present a misaligned (by 1 byte) copy of the shm-session-level serialization.  The hardening's
   * whole point: borrow_object() must memcpy out of the buffer, so a misaligned source address
   * must not cause UB (signed/unsigned wrap, unaligned load in strict-alignment env).  Size is
   * unchanged, content is unchanged, so the borrow should succeed. */
  template<ShmType SHM_TYPE>
  void sabotage_shm_level_misaligned()
  {
    FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);
    FLOW_LOG_INFO("Shm-session-level borrow_object() on misaligned blob; expect success (memcpy hardening).");

    auto pair = make_session_pair<SHM_TYPE>();
    auto& cli = pair.m_sessions->m_cli_session;
    auto& srv = pair.m_sessions->m_srv_session;

    auto h_cli = cli.session_shm()->template construct<Payload>(S_CANARY);
    ASSERT_NE(h_cli.get(), nullptr);

    const auto blob = shm_engine<SHM_TYPE>(cli)->lend_object(h_cli);
    ASSERT_GT(blob.size(), 0u);
    const auto shifted = make_misaligned_copy(blob, 1);
    ASSERT_EQ(shifted.size(), blob.size());
    auto h_srv = shm_engine<SHM_TYPE>(srv)->template borrow_object<Payload>(shifted);
    ASSERT_NE(h_srv.get(), nullptr);
    EXPECT_EQ(*h_srv, S_CANARY);
  }

  /* ---------- SHM-classic-only tests: Session_impl::borrow_object() + Pool_arena::borrow_object() edge cases. ---------- */

  /* Session_impl::borrow_object() must refuse a blob too small to carry the scope-ID tail
   * (blob.size() < sizeof(scope_id_t)).  Historically there was a size-underflow here that would
   * access past the buffer start. */
  void classic_sabotage_si_bo_blob_below_scope_id_sz()
  {
    FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);
    FLOW_LOG_INFO("Session::borrow_object() on blob smaller than scope-ID tail; expect null (no underflow).");

    auto pair = make_session_pair<ShmType::CLASSIC>();
    auto& srv = pair.m_sessions->m_srv_session;

    /* A few bytes: smaller than sizeof(scope_id_t) == sizeof(uint64_t). */
    Blob tiny{3, flow::util::Clear_on_alloc{}};
    auto h = srv.template borrow_object<Payload>(tiny);
    EXPECT_EQ(h.get(), nullptr);
  }

  /* Session_impl::borrow_object() -> Pool_arena::borrow_object() rejects a payload-size mismatch.
   * The session layer strips the scope-ID tail then forwards to Pool_arena::borrow_object(), which
   * requires an exact-size match on the remainder.  Pad garbage bytes *before* the original blob
   * (prefix slide) so the post-strip Pool_arena payload is oversized. */
  void classic_sabotage_si_bo_wrong_payload_size()
  {
    FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);
    FLOW_LOG_INFO("Session::borrow_object() on prefix-slid blob (post-strip payload oversized); expect null.");

    auto pair = make_session_pair<ShmType::CLASSIC>();
    auto& cli = pair.m_sessions->m_cli_session;
    auto& srv = pair.m_sessions->m_srv_session;

    auto h_cli = cli.session_shm()->template construct<Payload>(S_CANARY);
    const auto blob = cli.lend_object(h_cli);
    ASSERT_GT(blob.size(), S_SCOPE_ID_SZ);

    constexpr size_t PAD = 4;
    Blob bad{blob.size() + PAD, flow::util::Clear_on_alloc{}};
    bad.emplace_copy(bad.begin() + PAD, util::Blob_const(blob.const_data(), blob.size()));

    auto h_srv = srv.template borrow_object<Payload>(bad);
    EXPECT_EQ(h_srv.get(), nullptr);
  }

  /* Session_impl::borrow_object() must read the trailing scope_id via memcpy since its position
   * in the blob is not necessarily aligned.  A blob whose start address is itself shifted by one
   * byte is extra insurance: both the Pool_arena payload read and the scope_id read are off an
   * unaligned base. */
  void classic_sabotage_si_bo_misaligned()
  {
    FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);
    FLOW_LOG_INFO("Session::borrow_object() on misaligned blob; expect success (memcpy scope-ID).");

    auto pair = make_session_pair<ShmType::CLASSIC>();
    auto& cli = pair.m_sessions->m_cli_session;
    auto& srv = pair.m_sessions->m_srv_session;

    auto h_cli = cli.session_shm()->template construct<Payload>(S_CANARY);
    const auto blob = cli.lend_object(h_cli);
    const auto shifted = make_misaligned_copy(blob, 1);
    ASSERT_EQ(shifted.size(), blob.size());

    auto h_srv = srv.template borrow_object<Payload>(shifted);
    ASSERT_NE(h_srv.get(), nullptr);
    EXPECT_EQ(*h_srv, S_CANARY);
  }

  /* Session_impl::borrow_object() rejects an unknown scope_id.  Overwrite the trailing scope_id
   * bytes with a value matching neither S_SCOPE_ID_SESSION nor S_SCOPE_ID_APP. */
  void classic_sabotage_si_bo_bogus_scope_id()
  {
    FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);
    FLOW_LOG_INFO("Session::borrow_object() with bogus scope_id; expect null.");

    auto pair = make_session_pair<ShmType::CLASSIC>();
    auto& cli = pair.m_sessions->m_cli_session;
    auto& srv = pair.m_sessions->m_srv_session;

    auto h_cli = cli.session_shm()->template construct<Payload>(S_CANARY);
    const auto blob = cli.lend_object(h_cli);
    ASSERT_GT(blob.size(), S_SCOPE_ID_SZ);

    Blob bad = make_resized_copy(blob, blob.size());
    const uint64_t bogus = 0xDEAD'BEEF'DEAD'BEEFULL;
    std::memcpy(bad.data() + (bad.size() - S_SCOPE_ID_SZ), &bogus, S_SCOPE_ID_SZ);

    auto h_srv = srv.template borrow_object<Payload>(bad);
    EXPECT_EQ(h_srv.get(), nullptr);
  }

  /* Pool_arena::borrow_object() -> is_obj_in_arena() [-> is_addr_in-arena()]: mainstream out-of-pool rejection.
   * This is a core safety check wherein the encoded offset would point the outer (construct()ed) object
   * outside the Pool_arena pool.  Negative offsets are allowed for other reasons/in other contexts, but here it should
   * fail.  Naturally positive offsets are allowed in all contexts, but an offset beyond the pool here should
   * fail.
   *
   * Note: borrow_object()'s bounds check is is-addr-wrap-safe by construction (uses subtraction,
   * not addition).  We do not attempt to exercise the wrap path directly: synthesizing an input
   * that would actually wrap a ptrdiff_t on x86-64 is not meaningfully reachable from real
   * borrower behavior, and a naive-vs-safe implementation would both reject the obviously-out-of-pool
   * offsets used here.  Hence we forego trying to test against the counterfactual (what would happen if
   * the in-pool checks were written the non-wrap-safe way?) -- as black box (not a very big one at that) it's tough
   * to modulate it for that.  However it is important that the normal/mainstream cases didn't get broken; which we
   * test by exercising lend/borrow all over this and surrounding tests (both the valid- and invalid-input cases). */
  void classic_sabotage_pa_bo_addr_out_of_pool()
  {
    FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);
    FLOW_LOG_INFO("Pool_arena::borrow_object() with offset out of pool (below then above); expect null.");

    /* Cap the session-scope pool small so that the above-pool offset below is plainly past its
     * end.  (make_session_pair()'s default ~GiBs pool would swallow any "just use a big number"
     * above-pool offset we could pick.)  The below-pool subcase uses offset = -1 and is
     * insensitive to pool size. */
    constexpr size_t POOL_LIMIT_MI = 4; // Small, yet generous for session internals.
    auto pair = make_session_pair<ShmType::CLASSIC>(POOL_LIMIT_MI);
    auto& cli = pair.m_sessions->m_cli_session;
    auto& srv = pair.m_sessions->m_srv_session;

    auto h_cli = cli.session_shm()->template construct<Payload>(S_CANARY);
    auto blob = cli.session_shm()->lend_object(h_cli);
    ASSERT_EQ(blob.size(), sizeof(ptrdiff_t));

    // Below pool: pool_base + (-1) = pool_base - 1.
    {
      const ptrdiff_t bogus_offset = -1;
      std::memcpy(blob.data(), &bogus_offset, sizeof(bogus_offset));
      auto h_srv = srv.session_shm()->template borrow_object<Payload>(blob);
      EXPECT_EQ(h_srv.get(), nullptr);
    }

    // Above pool: 2x the cap, comfortably past pool end.
    {
      constexpr ptrdiff_t bogus_offset = ptrdiff_t{POOL_LIMIT_MI * 2} * 1024 * 1024;
      std::memcpy(blob.data(), &bogus_offset, sizeof(bogus_offset));
      auto h_srv = srv.session_shm()->template borrow_object<Payload>(blob);
      EXPECT_EQ(h_srv.get(), nullptr);
    }
  }

  /* If the thing tested in classic_sabotage_pa_bo_addr_out_of_pool() passes, then Pool_arena::borrow_object()
   * also ensures that the would-be object (plus internally-kept ref-count for GC) does not *end* past the pool
   * boundary.  So we similarly test that.
   *
   * Mechanically this is somewhat different, even though it sounds similar: The end of a T is determined by
   * its start (provided inside the SHM-handle-encoding blob, really as a single offset) plus sizeof(T); and T
   * is provided at compile-time by the borrower-side application.  Hence we can modulate T itself.
   *
   * We stand up our own Pool_arena of known small size: Pool_arena exposes no size-accessor as of this writing, and
   * the session-factory pool is sized by ipc::session internally (albeit it's possible to override).  In any
   * case it seemed crisper/more direct in this case to create our own manually (which is a formally allowed practice).
   * (Incidentally, with SHM-jemalloc doing that -- without ipc::session doing it for us -- is quite a bit more
   * strenuous coding-wise.)
   *
   * Use offset = 0 (start-addr = pool_base, in-pool); pick T whose sizeof > pool_size, so the end-check must reject.
   * @todo Could be more exhaustive by trying to check for the (internally kept) ref-count ending up just past the
   * pool + various such near-boundary corner cases. */
  void classic_sabotage_pa_bo_obj_tail_past_pool_end()
  {
    FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);
    FLOW_LOG_INFO("Pool_arena::borrow_object<Big>() with start in-pool but tail past end; expect null.");

    const auto pool_name = util::Shared_name::ct("lend_borrow_test_tail_past_pool_end");

    /* Fire post-cleanup even if an ASSERT trips below.  Also do a defensive pre-cleanup
     * in case a prior run of this test left the named pool behind. */
    struct Pool_remover
    {
      util::Shared_name m_name;
      ~Pool_remover() { flow::Error_code dummy; classic::Pool_arena::remove_persistent(g_logger, m_name, &dummy); }
    };
    { flow::Error_code dummy; classic::Pool_arena::remove_persistent(nullptr /* quiet */, pool_name, &dummy); }
    Pool_remover remover{pool_name};

    constexpr size_t POOL_SIZE = 4096;
    classic::Pool_arena arena{g_logger, pool_name, util::CREATE_ONLY, POOL_SIZE};

    /* Sanity-check the lend encoding: Pool_arena::lend_object() must produce a blob of exactly
     * sizeof(ptrdiff_t) (the offset from pool base).  If that ever changes, the crafted offset-0
     * blob below would fail borrow_object() for an unrelated size-mismatch reason, silently
     * hiding what this test is actually checking.  The construct/lend pair is discarded when
     * the inner scope ends. */
    {
      auto h = arena.template construct<Payload>(S_CANARY);
      ASSERT_NE(h.get(), nullptr);
      const auto sanity_blob = arena.lend_object(h);
      ASSERT_EQ(sanity_blob.size(), sizeof(ptrdiff_t));
    }

    /* offset = 0 -> start-addr = pool_base (trivially in-pool); sizeof(Big) > POOL_SIZE so
     * is_obj_in_arena()'s end-check must reject. */
    using Big = std::array<uint8_t, POOL_SIZE * 2>;
    static_assert(sizeof(Big) > POOL_SIZE);
    Blob blob{sizeof(ptrdiff_t), flow::util::Clear_on_alloc{}};
    auto h_bogus = arena.template borrow_object<Big>(blob);
    EXPECT_EQ(h_bogus.get(), nullptr);
  }

  /* Pool_arena::is_handle_in_arena() accepts a handle that genuinely belongs to the arena.
   * (Exercises the same is_addr_in_arena() helper that borrow_object() uses.) */
  void classic_test_is_handle_in_arena_positive()
  {
    FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);
    FLOW_LOG_INFO("Pool_arena::is_handle_in_arena() on in-arena handle; expect true.");

    auto pair = make_session_pair<ShmType::CLASSIC>();
    auto& cli = pair.m_sessions->m_cli_session;

    auto h = cli.session_shm()->template construct<Payload>(S_CANARY);
    ASSERT_NE(h.get(), nullptr);
    EXPECT_TRUE(cli.session_shm()->is_handle_in_arena(h));
  }

  /* is_handle_in_arena() rejects a handle from a DIFFERENT arena (app_shm vs session_shm).  Both
   * arenas exist in this session; addresses cannot coincide.  Verifies the bounds check rejects
   * when the address is real but outside the pool. */
  void classic_test_is_handle_in_arena_negative()
  {
    FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);
    FLOW_LOG_INFO("Pool_arena::is_handle_in_arena() on handle from a different arena; expect false.");

    auto pair = make_session_pair<ShmType::CLASSIC>();
    auto& srv = pair.m_sessions->m_srv_session;

    auto h_app = srv.app_shm()->template construct<Payload>(S_CANARY);
    ASSERT_NE(h_app.get(), nullptr);
    EXPECT_FALSE(srv.session_shm()->is_handle_in_arena(h_app));
    EXPECT_TRUE(srv.app_shm()->is_handle_in_arena(h_app));
  }

  /* is_handle_in_arena() must short-circuit to false when the Pool_arena has no attached pool
   * (m_pool == nullptr).  Stand up a Pool_arena on the stack in Open_only mode with a bogus pool
   * name: attachment fails, m_pool stays null.  Then exercise is_handle_in_arena() on a real
   * handle from a separate, functional arena -- must return false (and not dereference the null
   * m_pool). */
  void classic_test_is_handle_in_arena_null_pool()
  {
    FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);
    FLOW_LOG_INFO("Pool_arena::is_handle_in_arena() on unattached (null-pool) Pool_arena; expect false.");

    auto pair = make_session_pair<ShmType::CLASSIC>();
    auto& cli = pair.m_sessions->m_cli_session;

    auto h = cli.session_shm()->template construct<Payload>(S_CANARY);
    ASSERT_NE(h.get(), nullptr);

    flow::Error_code ec;
    classic::Pool_arena detached{g_logger, util::Shared_name::ct("cool_nonexistent_bogus_pool_xyz"),
                                 util::OPEN_ONLY, false, &ec};
    // `ec` indicates open failure; that's the point of this test.  Whatever its value, proceed.
    EXPECT_FALSE(detached.is_handle_in_arena(h));
  }

  /* One TEST per (template test body, SHM flavor). */
#define SHM_TEST(name) \
  TEST(Lend_borrow_test, name##_ShmClassic) { name<ShmType::CLASSIC>(); } \
  TEST(Lend_borrow_test, name##_ShmJemalloc) { name<ShmType::JEMALLOC>(); }
} // namespace (anon)

SHM_TEST(happy_session_level)
SHM_TEST(happy_shm_session_level)
SHM_TEST(sabotage_shm_level_too_small)
SHM_TEST(sabotage_shm_level_too_large)
SHM_TEST(sabotage_shm_level_misaligned)

TEST(Lend_borrow_test, classic_sabotage_si_bo_blob_below_scope_id_sz) { classic_sabotage_si_bo_blob_below_scope_id_sz(); }
TEST(Lend_borrow_test, classic_sabotage_si_bo_wrong_payload_size) { classic_sabotage_si_bo_wrong_payload_size(); }
TEST(Lend_borrow_test, classic_sabotage_si_bo_misaligned) { classic_sabotage_si_bo_misaligned(); }
TEST(Lend_borrow_test, classic_sabotage_si_bo_bogus_scope_id) { classic_sabotage_si_bo_bogus_scope_id(); }
TEST(Lend_borrow_test, classic_sabotage_pa_bo_addr_out_of_pool) { classic_sabotage_pa_bo_addr_out_of_pool(); }
TEST(Lend_borrow_test, classic_sabotage_pa_bo_obj_tail_past_pool_end) { classic_sabotage_pa_bo_obj_tail_past_pool_end(); }
TEST(Lend_borrow_test, classic_test_is_handle_in_arena_positive) { classic_test_is_handle_in_arena_positive(); }
TEST(Lend_borrow_test, classic_test_is_handle_in_arena_negative) { classic_test_is_handle_in_arena_negative(); }
TEST(Lend_borrow_test, classic_test_is_handle_in_arena_null_pool) { classic_test_is_handle_in_arena_null_pool(); }

#undef SHM_TEST

} // namespace ipc::shm::test
