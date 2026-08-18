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
#include "ipc/shm/arena_lend/detail/arena_lend_fwd.hpp"
#include <flow/util/util.hpp>
#include <boost/interprocess/sync/named_mutex.hpp>

/// @cond
// -^- Doxygen, please ignore the following.  It's undef-ed later anyway.

// 1 if and only if the compiler is gcc proper (not clang, which also defines `__GNUC__`); keys pragmas below.
#if defined(__GNUC__) && !defined(__clang__)
#  define IPC_SHM_ARENA_LEND_DETAIL_GCC_COMPILER 1
#else
#  define IPC_SHM_ARENA_LEND_DETAIL_GCC_COMPILER 0
#endif

// -v- Doxygen, please stop ignoring.
/// @endcond

/* gcc (gcc-9 at least) is pretty paranoid about some bit-field paths below and gives some *very* cryptic warnings
 * that amount to maybe-uninitialized.  The code appears solid, so let's bypass it temporarily. */
#if IPC_SHM_ARENA_LEND_DETAIL_GCC_COMPILER
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

/// Segregated private stuff for ipc::shm::arena_lend.
namespace ipc::shm::arena_lend::detail
{

// Types.

/**
 * Parameterization-independent aspects of Shm_pool_offset_ptr_data segregated in non-template base.  In terms of
 * public API the user should care about:
 *   - The proper type aliases for pool ID, pool offset.
 *   - Pool ID generation: generate_pool_id().
 *
 * However you should read the Shm_pool_offset_ptr_data doc header first.
 *
 * ### Bit order, endianness ###
 * Terminology: MSB = most-significant (higher) bits; LSB = least-significant (lower) bits.  So in a 64-bit
 * unsigned number, the MSB is called bit 63, while the LSB is bit 0.
 *
 * In all the surrounding code endianness is intentionally not a factor:
 * We treat multi-byte things as numbers, not individual bytes; so, e.g., `uint64_t n = 0x0001'0203'0405'0607ull` is
 * stored in memory as `07 06 05 04 03 02 01 00`; but in code we might do `n & 0xFF` to isolate the LS-byte and indeed
 * get `0x07`.  (Or we might use a bit-field for the same purpose, but disregard that here.)  We do *not* in this
 * code do something like `reinterpret_cast<uint8_t*>(&n)[7]` (which would be
 * wrong; in this likely-little-endian system `[0]` would be correct); it would be an unnecessary layer of
 * non-portability, and it would make the code more difficult to understand anyway.
 *
 * As for the bit-field angle (which we asked you to disregard momentarily above):
 * Okay, so actually endianness is indirectly a factor after all.  As discussed elsewhere we do low-level
 * pointer-tagging which assumes x86-64 architecture; and we use bit fields; as a result, at least in gcc/clang,
 * bit fields are placed in the resulting composite number in the *reverse* order in which they're declared
 * in a given `struct`.  So for example, if we have `struct { uint64_t m_half2 : 32; uint64_t m_half1 : 32; } bits`,
 * and `auto& num = *(reinterpret_cast<uint64_t*>(&bits))`, and we execute `bits.m_half1 = 0; bits.m_half2 = 1`, then
 * `num = 1`.  Note the reversed order inside the `struct` bit-field declaration.  Presumably gcc/clang does it this
 * way due to little-endianness of x86-64.
 *
 * ### Internals ###
 * These will make sense upon reading the impl section of the Shm_pool_offset_ptr_data class doc header.
 */
class Shm_pool_offset_ptr_data_base
{
public:
  // Constants.

  /**
   * # of bits in a pool offset (determines ceiling on size of pool; but note #pool_offset_t signedness-or-not
   * is a factor).  It is public (within `detail` though), as certain calculations involving pool max sizes and such
   * might need to use this value.
   */
  static constexpr unsigned int S_N_POOL_OFFSET_BITS = 32;

  // Types.

  /**
   * Pool offset type.  Outside of Shm_pool_offset_ptr_data no entity can request a pool larger than what
   * this can index.
   *
   * @note Maintenance/context: The width and signedness of Shm_pool_offset_ptr_data_base::pool_offset_t
   *       determines, as of this writing, the value to which SHM-jemalloc elsewhere sets the
   *       jemalloc per-arena option `retain_grow_limit`, having to do with limiting the growth of vaddr areas.
   *       Calling your attention to this, as it is a subtle effect that might affect something unexpectedly.
   *
   * ### Impl: Why 32-bit width chosen for `pool_offset_t` ###
   * Per Shm_pool_offset_ptr_data doc header impl discussion, it must fit into #rep_t in addition to 1
   * selector bit and #pool_id_t.  We choose the full 32 LSB (of 64-bit #rep_t) to be #pool_offset_t; allowing
   * for comfy and round maximally-2-or-4GiB-sized pools.  32 LSB also synergizes with #diff_t.
   *
   * ### Impl: Why signed type chosen for `pool_offset_t` ###
   * That's a subtle discussion.  (To get it out of the way: it limits pool size to 2GiB instead of 4GiB; this
   * is acceptable (assuming 4GiB is acceptable, which it is).)  However it adds entropy/reasoning complexity to
   * make it signed; so what is gained for that trouble?  Answer: Consider `decrement(1)`.  If `m_pool_offset >= 1`,
   * then all is cool; it becomes zero; nice.  Now suppose Shm_pool_offset_ptr_data::m_pool_offset is zero
   * and consider `get()` which forwards to `Repository::to_address()` which will act based on what we decide here
   * type-wise (#pool_offset_t is propagated as the offset-type alias all over the code).  `to_address()`, having
   * looked up the pool base vaddr `base` (internally a `uint64_t` thing), will now do the following depending
   * on our decision.
   *   - If unsigned: it will add an overflowed 32-bit positive 0xFFFF'FFFF (~4 billion) to the `uint64_t` base and
   *     return that; and we will return that in turn from `get()`.  That is it'll return some arbitrary location
   *     in vaddr space.
   *   - If signed: it will subtract 1 and return that.  That is it will return a somewhat less arbitrary location
   *     just ahead of the pool's start.
   *
   * Now, which is better?  The answer is subjective, but we can make a pretty good case for the 2nd one.
   * Firstly refer to the explanation in `get()` doc header for why `Repository_type::to_address()`, and therefore
   * `get()`, would ever return an out-of-pool vaddr (spoiler alert: something like a `vector<uint8_t>::end()`
   * iterator -- as an example -- may well be used for comparison -- not dereferencing! -- in a legit algorithm,
   * and such a buffer might happen to reside at the tail of a pool).  The question is, could that legitimately
   * be a use case in the opposite end of a pool/buffer?  Well... yes; a buffer might be at the start of a pool;
   * and one might let a Shm_pool_offset_ptr_data get past its `begin()` in the reverse direction; for example
   * something like: `for (...; p >= x.begin(); --p)`.  (Think about an integer counter instead:
   * `for (int i = ...; i >= 0; --i)`; if it were `unsigned int` instead then `i >= 0` would *always* be true;
   * hence why in a reverse-direction such loop it's normal to use a signed type.  We're following similar logic.)
   * Granted, such code would not be exactly prudent on the user's part, and certainly one can avoid such situations
   * by changing their code a bit; but it is nevertheless conceivable.
   *
   * So that is why it is signed.  However it is not a slam-dunk.  Vaguely speaking signed indices and signed integers
   * in a context like this can add pain.
   */
  using pool_offset_t = int32_t;

  /// Pool ID type alias.  Please see detail::pool_id_t doc header for discussion as to the chosen bit width.
  using pool_id_t = detail::pool_id_t;

  /**
   * Analogous to `difference_type` in STL containers (but uses our naming conventions).
   *
   * ### Impl: Why `int32_t` was chosen ###
   * Firstly `boost::offset_ptr` defaults to `int`; and we specifically require x86-64 architecture
   * (see class Shm_pool_offset_ptr_data doc header); so `int` would be `int32_t`.  Secondly, as of this
   * writing: for offset pointers, we've chosen the 32 LSB as the offset; and for raw pointers x86-64
   * mandates 48 LSB as storing the vaddr.  The lesser is 32; so there you go.  (Note that it's normal
   * to use the signed version of the size-type; e.g., `ptr_diff_t` is signed version of `size_t` normally;
   * overflow can of course occur, still, but this is seen as the nature of the business we're in so to speak.)
   */
  using diff_t = int32_t;

  // Methods.

  /**
   * Returns a heretofore-unused pool ID so as to be used to identify (in a Shm_pool_offset_ptr_data::Repository)
   * a pool being created at this time.  Note that one shall not use this, necessarily, for every insertion into
   * such a table: the pool ID is cross-process, so a borrowing process will use the pool ID that was
   * `generate_pool_id()`ed by the owner process (hence both the pool name and pool ID shall be IPC-transmitted
   * during the borrowing process).
   *
   * ### Design / what is "heretofore-unused"? ###
   * Formally: one just uses generate_pool_id() when a new SHM-pool is being created and added to the `Repository`.
   * (It might be convenient for other reasons to include the ID in the pool name; but I digress.)  Internally,
   * though, what actually happens and why?
   *
   * (Firstly this is all about kernel-persistence, meaning once the machine restarts, it's a new day.)
   * The decision here is to use a 31-bit ID -- hence ~2 billion unique IDs -- across *all* processes on the
   * machine, regardless of application, split, or anything else; in that sense it's different from all other
   * cross-process namespaces in ::ipc as of this writing, which at least segregate things by owner application.
   * In this case we don't have a string namespace to work with, though (unlike util::Shared_name).  We could
   * alternatively split up the 2 billion-wide space among (e.g.) splits.  The downside of this is complexity
   * of API/code; and limiting the number of pools each (say) split can generate across its lifetime.
   * (Trying to register/unregister IDs is also too complicated.)  The bottom line is: 2 billion pools,
   * by all applications, between machine boots should be sufficient; for example we consider PID to be a
   * unique-enough-across-all-time (between boots) process ID in other parts of the system.  (Granted there are
   * potentially many pools per process, but still, ballpark, this reasoning is okay.)
   *
   * Moreover we will wrap-around having reached the max 31-bit number, back to 1 (0 is special and shall not
   * be used).  If by some incredible miracle we actually do reach this overflow condition, the chances that
   * that the wrapped-around-to-processes are still around/relevant = virtually nil.  So even that should work.
   *
   * @return See above.
   */
  static pool_id_t generate_pool_id();

protected:
  // Types.

#if !(((defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__)) || (defined(_MSC_VER) && defined(_M_X64)))
  static_assert(false, "This pointer-tagging impl relies on the target architecture being x86-64 a/k/a AMD64.");
#endif

#if (!defined(__GNUC__)) && (!defined(__clang__))
  static_assert(false, "The bit-field code below is tested only with gcc and clang.");
#endif

  /// The unsigned integer type used to store Shm_pool_offset_ptr_data pointer payloads.
  using rep_t = uint64_t;
  static_assert(sizeof(void*) == sizeof(rep_t), "x86-64 should have 64-bit pointers.");

  /**
   * A bit-field overlaid over any #rep_t, this represents a pointer with the selector MSB indicating
   * a pool-and-offset-bearing non-null payload.  Alternatively, if #m_selector_offset_else_raw is 0,
   * then the remaining bits in the bit-field shall be ignored; a `*this` holds either null or a raw-pointer
   * bearing payload.
   */
  struct Offset_ptr_rep
  {
    // Data.  In reverse bit order as @noted below!

    /**
     * If and only if #m_selector_offset_else_raw: The offset within pool IDed by #m_pool_id.
     *
     * @note If #pool_offset_t is signed, then this *can* be negative!  Please read that alias's doc header.
     */
    pool_offset_t m_pool_offset : S_N_POOL_OFFSET_BITS;

    /// If and only if #m_selector_offset_else_raw: The pool ID.
    pool_id_t m_pool_id : (sizeof(rep_t) * 8) - S_N_POOL_OFFSET_BITS - 1;

    /**
     * Either 0 (raw or null pointer; do not use the other bit members) or 1 (offset pointer).
     *
     * @note With gcc/clang/x64-64 this is listed *last*, because it is the *most* significant datum.
     *       The fields are thus in reverse order of bit order.
     */
    rep_t m_selector_offset_else_raw : 1;
  }; // struct Offset_ptr_rep
  static_assert(sizeof(Offset_ptr_rep) == sizeof(rep_t),
                "Expecting bit-field to be packed tightly; not guaranteed by standard but in practice is.");

  /**
   * A bit-field overlaid over any #rep_t, this represents a pointer with the selector MSB indicating
   * a ra-pointer-bearing non-null payload.  Alternatively, if #m_selector_offset_else_raw is 1,
   * then the remaining bits in the bit-field shall be ignored; a `*this` holds either null or an offset-pointer
   * bearing payload.
   */
  struct Raw_ptr_rep
  {
    // Constants.

    /// # of bits in #m_val_bits.
    static constexpr size_t S_N_VAL_BITS = 48;

    // Data.  In reverse bit order as @noted below!

    /**
     * If `!m_selector_offset_else_raw`, and the entire value is not all-zero-bits:
     * #m_ext_sign_msb repeated yet again, followed by 47 more bits; altogether the 48 bits represent
     * the vaddr itself, sans all the bits to the left which are sign-extension bits that complete the
     * canonical form (except #m_selector_offset_else_raw which we use as metadata in our pointer-tagging
     * scheme).
     */
    rep_t m_val_bits : S_N_VAL_BITS;

    /**
     * If `!m_selector_offset_else_raw`, and the entire value is not all-zero-bits:
     * #m_ext_sign_msb repeated.
     */
    rep_t m_ext_sign_bits_except_msb : ((sizeof(rep_t) * 8) - S_N_VAL_BITS - 1 - 1);

    /**
     * If `!m_selector_offset_else_raw`, and the entire value is not all-zero-bits: The most
     * significant, after the leading bit taken up by #m_selector_offset_else_raw, sign-extension bit
     * in this raw pointer.  In a properly constructed pointer:
     *   - If this is 0, then the next (64 - 1 - 48) = 15 bits shall also be 0.  Those 3 terms are respectively:
     *     bit count of #rep_t (formally `sizeof(rep_t) * 8`); 1 bit taken up by #m_selector_offset_else_raw
     *     pointer-tagging bit; then the 48 LSB storing the actual vaddr minus the canonicalizing padding
     *     (a/k/a *sign extension*).
     *   - If this is 1, then the next 15 bits shall also be 1.
     */
    rep_t m_ext_sign_msb : 1;

    /**
     * Either 0 (raw or null pointer; if all other bits are also 0, then do not use the other bit members) or 1
     * (do not use the other bit members).  When converting to a canonical `void*`: leave the rest of the
     * bits alone; but set this one to equal #m_ext_sign_msb (the next LSB).
     *
     * @note With gcc/clang/x64-64 this is listed *last*, because it is the *most* significant datum.
     *       The fields are thus in reverse order of bit order.
     */
    rep_t m_selector_offset_else_raw : 1;
  }; // struct Raw_ptr_rep
  static_assert(sizeof(Raw_ptr_rep) == sizeof(rep_t),
                "Expecting bit-field to be packed tightly; not guaranteed by standard but in practice is.");
  /* @todo It'd be nice to write a static_assert() directly ensuring that members of a bit field in this arch
   * appear in the opposite order of declaration (LSB declared first).
   * This is easily doable with (constexpr) std::bit_cast (available in C++20), but as of this writing we're on
   * C++17.
   *   static_assert(std::bit_cast<rep_t>(Offset_ptr_rep{ 0, 0, 1 }) == (rep_t(1) << 63), "..."); // Etc.
   * An assert() is easy enough to write; but so far my (ygoldfel) attempts to write a compile-time type-punning
   * expression have been fruitless, so a static_assert() has not been forthcoming so far.  It's not a *huge*
   * deal, as we do ensure the architecture is specifically so-and-so at compile-time; but it'd be a little
   * nicer to instead assert aspects of the architecture we care about as opposed to its identity.
   * Judge an architecture by the content of its character!  At least we did do so regarding the bits getting
   * properly packed into a rep_t. */

private:
  // Data.

  /// Used with `std::call_once()` to ensure #s_pool_id_shm_region_or_none is initialized no more than once.
  static std::once_flag s_pool_id_shm_region_init_flag;

  /**
   * The SHM-object (pool) mapped by #s_pool_id_shm_region_or_none.  Default-cted until initialization.
   *
   * Stored so that the pool does not disappear (if all handles close, then the pool is removed by system).
   */
  static bipc::shared_memory_object s_pool_id_shm_obj_or_none;

  /**
   * Initialized no more than once (in this process) by generate_pool_id(), a handle to tiny SHM region
   * storing (only) a `pool_id_t` used to generate unique pool IDs.  Default-cted until initialization.
   *
   * The pointee pool has the same name across the entire system.
   *
   * Stored for performance, so that generate_pool_id() need not map this each time but only once.
   */
  static bipc::mapped_region s_pool_id_shm_region_or_none;

  /**
   * Initialized no more than once (in this process) by generate_pool_id(), a handle to a named-mutex
   * protecting the state of the pool possibly-pointed-to by #s_pool_id_shm_region_or_none.  More formally
   * the state of that pool (when this mutex is unlocked) is one of:
   *
   *   - It does not exist.
   *   - It exists; is sized for the stored `pool_id_t`; and the bits therein contain an ID value, such that
   *     `++`ing yields a valid (non-zero) ID.
   *
   * Therefore one must lock this before opening or creating the pool; and if creation was indeed necessary,
   * then before unlocking one must size it; and most importantly initialize it to a pre-valid ID (perhaps 0 or
   * 1).  Not doing the latter breaks the invariant: after pool creation the contents of the pool = incoherent;
   * a `++` on this zero-looking-value can and does cause undefined behavior.
   *
   * The pointee mutex has the same name across the entire system.
   *
   * Stored for performance, so that generate_pool_id() need not open this each time but only once.
   */
  static std::optional<boost::interprocess::named_mutex> s_pool_id_mutex_or_none;
}; // class Shm_pool_offset_ptr_data_base

/**
 * Implementation core of Shm_pool_offset_ptr.  It provides the latter's essential capabilities while leaving out
 * the interface-y necessities of a standard fancy-pointer type such as the concept of the pointed type, `rebind`,
 * and so on.  One can think of us implementing the core of `uint8_t*` only (conceptually speaking);
 * as opposed to a `T*` parameterized on `T`.
 *
 * The requirements for `Repository::to_address()` and `Repository::from_address()` are below, so look for that.
 *
 * Implementation design
 * ---------------------
 * ### Pointer tagging scheme ###
 * The goal is to provide a fancy-pointer type capable of pointing into the SHM-pool system as per
 * shm::arena_lend design, wherein:
 *   - A pointer may be null (not-a-pointer).  Otherwise:
 *   - Each pointer is conceptually composed of 2 data: pool ID (that specifies a particular pool in a global
 *     table; such a pool has a base vaddr valid in this process); and a pool offset (within that
 *     pool, off the aforementioned base vaddr, in bytes).  The global table is `Repository_type` template
 *     parameter and contains:
 *       - `static void* to_address(pool_id_t pool_id, pool_offset_t pool_offset)`: Get vaddr from the 2 pointer data
 *         (a/k/a *handle*).  If `pool_id` is not a known pool, undefined behavior (UB).  (We do not assume
 *         it will return null, and your impl may be a bit faster if it skips the check and lets the UB freak flag fly.)
 *         - (As of this writing `Borrower_shm_pool_collection_repository` and `Owner_shm_pool_repository` both
 *           yield UB.)
 *         - `pool_offset` may be negative or exceed pool size; this is *not* UB: `to_address()` shall yield
 *           the out-of-pool-bounds vaddr.
 *       - `static void from_address(const void*, pool_id_t& pool_id, pool_offset_t& pool_offset)`: The opposite.
 *         - If #S_CAN_STORE_RAW_PTR template parameter is `true`:
 *           - Sets `pool_id = 0` (a special invalid ID), if the input `void*` is not in a SHM-pool.
 *         - Otherwise:
 *           - Allowed to yield UB, if the input `void*` is not in a SHM-pool.
 *             Then our subsequent get() also yields UB.
 *             - (As of this writing `Borrower_shm_pool_collection_repository` yields UB.)
 *           - Allowed to set `pool_id = 0` instead as well.  Then our get() yields null to try to contain UB entropy.
 *   - Alternatively, for the case where a datum is located outside any SHM pool (perhaps on the stack),
 *     so in particular `from_address()` would yield `pool_id == 0`, it instead stores a raw vaddr.
 *     - Depending on the compile-time situation this alternative may be disallowed (#S_CAN_STORE_RAW_PTR
 *       template parameter is `false`).  In particular in the shm::arena_lend design that is the case on the borrower
 *       side (when interpreting a data structure created by another process and transmitted to -- borrowed
 *       by -- this one).  (As of this writing `Borrower_shm_pool_collection_repository` is this way in particular.)
 *     - We call this (in this context) a *raw pointer*; whereas otherwise it is an *offset pointer*.
 *       - (Note that `boost::offset_ptr`, while serving a similar role to our offset pointer, is not the same
 *         thing; by using the offset-versus-`this` technique it can represent both in-SHM and raw pointers
 *         without any dichotomy as to its internal representation.  This is great, but it doesn't work
 *         for our case: We have more than 1 SHM pool, and if pointer in pool 1 needs to point into pool 2,
 *         and pool 1 and pool 2 base vaddrs are not equidistant to each other in process 1 versus process 2,
 *         then the `boost::offset_ptr` fails.)
 *   - In all cases its `sizeof` must be as small as possible; in fact our explicit goal is to have it equal
 *     `sizeof(void*)` (it obviously cannot be even smaller).  Then `*this`es can be copied around as quickly
 *     as regular `T*` pointers.  (Consider, e.g, a complex STL-compliant container of containers of....)
 *
 * The design of such a thing would be straightforward if not for the latter requirement.  Consider that, when
 * `CAN_STORE_RAW_PTR == true`, the fancy-pointer must be able to store all `8 * sizeof(void*)` bits of a
 * raw (non-SHM) address; *and* it needs at least 1 bit to specify *whether* it is a raw pointer or
 * an offset pointer.  So that is already more than `8 * size(void*)` bits.  (Due to alignment, and so on,
 * the amount of extra space used will be much greater still than the extra bit, even if that extra bit alone were
 * somehow okay.)
 *
 * @note For the rest of this discussion, which is by necessity low-level and not perfectly portable, let us
 *       assume the compilation-target system is x86-64 a/k/a AMD64.  (We `static_assert()` on this.)
 *
 * In x86-64 there are 64 bits in a raw pointer.  And if they were all potentially used, then we'd be screwed
 * as shown above.  In reality, however, only the 48 LSB bits store the actual vaddr: bits 47, 46, ..., 0
 * (good for 256TiB of addressable space).  The remaining 16 bits (bits 63, 62, ..., 48) are called
 * the *sign extension* and must equal bit 47; this is the *canonical form* pointers must be stored in, or else
 * the processor will explode on dereference (SEGV or similar).  So, as long as our get() returns a pointer value
 * in this form, internally we can use those 16 high bits to store additional information a/k/a metadata.
 * This is called *pointer tagging*.
 *
 * In our case we have only 1 bit (pun intended) of metadata: "is this an offset pointer or raw pointer?".
 * We shall use the most significant bit (bit 63).  Call it the *selector* bit:
 *   - The special value `nullptr` is represented as all bits = 0.  One can think of this as
 *     the 3rd type of storable pointer: not-a-pointer.  It's important that no value in either of the other
 *     forms (below) will also result in all bits = 0.  So if not null then:
 *   - Selector bit = 0 => raw pointer.
 *     - Bits 47 through 0 = actual vaddr value.  (One of these at least must be 1; otherwise it'd be null.)
 *     - Bits 62 through 48 = sign extension = bit 47 repeated.  (Hence get(), in this case, need only:
 *       copy `m_rep`; in the copy flip bit 63 to 1, if and only if bit 47 is 1;
 *       and return the copy.  Quite efficient.)
 *   - Selector bit = 1 => offset pointer.
 *     - Now we have 63 bits to store the pool ID and pool offset.  We can make various decisions here about
 *       how to split up that real estate; see Shm_pool_offset_ptr_data_base for the decisions made.
 *       - However: The pool ID must never be zero.  (Offset certainly can.)  This is a reserved value which
 *         `Repository_type::from_pointer()` can efficiently use to indicate not-found-in-any-SHM-pool condition.
 *
 * ### Bit fields and type-punning versus shift/AND/OR/NOT ###
 * In the functions of this class we need to access individual bits and/or bit sequences (interpreted as numbers
 * at times; e.g., the selector bit is MSB = bit 63; pool ID would be bits 62 through 32, for 31 bits total).
 * When accessing or modifying part of `m_rep` we have two choices more or less:
 *   - Use shifting `<<` `>>` and OR `|` and AND `&`, with perhaps a splash of NOT `~`.
 *   - Use bit fields combined with type-punning (accessing a value of one type by treating that memory location
 *     as value(s) of other type(s)).
 *     - (The type-punning itself in turn can be done a couple ways: Via native `union` (not `std::variant` which
 *       introduces more stored data!); or via `reinterpret_cast<>`ing between pointer types.
 *       The latter, in our direct experience, is inferior: One must `pragma`-away warnings; and after that one
 *       must use `volatile` to prevent (at least gcc-9 `-O3`) from causing wrong behavior due to optimizer not
 *       understanding when and how `*this` has been modified; but `volatile` then potentially subverts the
 *       optimizer's actual job of speeding things up.  The `union` way is also slightly more readable and slightly
 *       more concise.  The only disadvantage of `union`ing (here): this adds yet another instance of using what is,
 *       technically (according to the standard), undefined behavior; as officially only the last-assigned `union`
 *       member has a defined value, a rule we absolutely do and must break.  However, (1) we already rely
 *       on architecture-specific behavior, which is much riskier anyway (in for a penny, in for a pound); and (2)
 *       this type-punning technique is quite wide-spread and understood to be in practice correct in any
 *       conceivable environment (in practice).)
 *       - (One could also, or additionally, use `std::bit_cast`;
 *         however this would not be functionally equivalent, as this operation copies underlying bytes and cannot
 *         modify values in-place; it could affect performance.  Also `bit_cast` is C++20; as of this writing we are on
 *         C++17.  So we'd need to access gcc built-ins instead, until we're at C++20.)
 *
 * The aspects of bit field approach:
 *   - Bit field code is easier to understand (once declared at least). (Pro)
 *   - Bit fields are very not-portable across architectures and even compilers within a given architecture:
 *     standard specifically says their packing, order in memory, and byte-straddling behavior are unspecified.
 *     (Con)
 *     - (However, clang+gcc + x64-64 has a well defined behavior.)
 *   - Bit fields are faster or not-slower, according to the Internet (and apparently with clang even more so). (Pro)
 *     - (Performance here is quite important, as pointer-to-address and address-to-pointer are potentially very
 *       common pointer-involving operations, in aggregate even more frequent than dereferencing.)
 *
 * Ultimately we decided, for now at least, that since this code is already built around a particular architecture,
 * non-portability is much less of a concern that normal.  On the other hand performance and clarity are extremely
 * and rather important (respectively).  So we went with bit fields.
 *
 * @todo Consider extending Shm_pool_offset_ptr impl, currently non-portably limited to x86-64 architecture
 *       (and gcc/clang compilers), to other modern architecture(s) (perhaps ARM and other mobile-related arch?).
 *
 * @todo Examine empirical performance impact of bit-field approach in Shm_pool_offset_ptr impl versus alternative
 *       bit-arithmetic approach (shift, AND, OR, NOT).  I.e., benchmark it.
 * @todo Examine portability details of bit-field approach in Shm_pool_offset_ptr impl.
 *
 * @tparam Repository_type
 *         The shared memory pool repository type that can turn an offset-pointer *handle* (as represented by
 *         pool ID + pool offset pair) into the vaddr per the present shm::arena_lend-compliant process (a `void*`);
 *         and vice versa.  It shall have the 2 `static` APIs shown above, `to_address()` and `from_address()`.
 *         See details above regarding their expected behaviors.
 * @tparam CAN_STORE_RAW_PTR
 *         Whether a `*this` is allowed to represent a vaddr that is neither null nor belonging to any SHM-pool
 *         registered in the global `Repository_type` at the time of construction of a `*this` from a `void*`.
 *         If `true` in that event, then a raw pointer shall be stored; if `false`, then null is stored (but
 *         unless user code checks for this rather odd possibility, in practice subsequent undefined behavior
 *         is likely).
 */
template<typename Repository_type, bool CAN_STORE_RAW_PTR>
class Shm_pool_offset_ptr_data :
  public Shm_pool_offset_ptr_data_base
{
public:
  // Types.

  /// Our base type.
  using Base = Shm_pool_offset_ptr_data_base;

  /// Short-hand for template parameter `Repository_type`; may be useful for generic programming.
  using Repository = Repository_type;

  /// Short-hand for template parameter `CAN_STORE_RAW_PTR`; may be useful for generic programming.
  static constexpr bool S_CAN_STORE_RAW_PTR = CAN_STORE_RAW_PTR;

  // Constructors/destructor.

  /// Construct with `nullptr`.
  Shm_pool_offset_ptr_data();

  /**
   * Construct from vaddr.
   *
   * Corner case: If #S_CAN_STORE_RAW_PTR is `false`, and `p` is neither `nullptr` nor belongs to
   * #Repository, then formally behavior is undefined.  Practically:
   *   - If `Repository::from_address()` yields `pool_id = 0` in this situation, then we shall act as if
   *     `p == nullptr` (we become null).
   *   - If it yields UB in this situation, then we yield UB also.
   *
   * @param p
   *        See above.
   */
  Shm_pool_offset_ptr_data(const void* p);

  /**
   * Copy constructor.
   *
   * @param src
   *        Source object.
   */
  Shm_pool_offset_ptr_data(const Shm_pool_offset_ptr_data& src);

  /**
   * Copy constructor from object whose type is the opposite w/r/t whether it can store a raw pointer.
   *
   * Corner case: If #S_CAN_STORE_RAW_PTR is `false`, and `src.is_raw() == true`, then
   * this ctor shall yield `this->to_bool() == false`.  Or in regular words: if our type is such that we
   * can only represent in-SHM addresses, and `src` stores a raw address, then we cannot (safely) represent
   * the address in `src` and will represent null instead.  (We are not aware of a use-case for such a
   * conversion to be successful and prefer to deterministically result in null.)
   *
   * @param src
   *        Source object.
   */
  Shm_pool_offset_ptr_data(const Shm_pool_offset_ptr_data<Repository_type, !S_CAN_STORE_RAW_PTR>& src);

  // Methods.

  /**
   * Assignment.
   *
   * @param src
   *        Source object.
   * @return `*this`.
   */
  Shm_pool_offset_ptr_data& operator=(const Shm_pool_offset_ptr_data& src);

  /**
   * Returns whether the offset pointer holds a non-null, raw pointer; as opposed to an offset pointer or null.
   *
   * @return See above.
   */
  bool is_raw() const;

  /**
   * Retrieves the pointer to the object, if there is one stored; otherwise `nullptr`.
   *
   * ### Corner cases ###
   * This class acts in particular ways depending on what representation is currently stored, and what the value is;
   * one can get to odd situations via increment() in particular.  First we describe what `*this`
   * will do formally; then we will discuss how this might affect the public-facing containing fancy-pointer type.
   *
   * If null is actually stored, as via default ctor, ctor from `nullptr`, or assignment of either, then we
   * return `nullptr` as required.  Now assume this is not the case.
   *
   * If `S_CAN_STORE_RAW_PTR == false`:
   *   - Recall that internally a *handle* is stored: pool ID, offset into that pool.
   *     - If this pool does not exist, *and* `Repository::to_address()` detects this: we return `nullptr`.
   *       Implications:
   *       - If one dereferences it, behavior is obviously undefined.  This is desirable and likely similar to
   *         what would occur with dereferencing a corrupt or (obviously) null regular pointer.
   *       - If one compares to it such as the `.end()` scenario below, they're likely to get some behavior
   *         they don't expect -- but do they expect, if the system/they allowed the pool to get unmapped
   *         while running algorithms on related data?  Not our problem.
   *       - Even though we do this, formally it is UB.  We are just trying to contain the entropy somewhat.
   *     - If this pool does not exist, *and* `Repository::to_address()` yields UB as a result:
   *       we too yield UB.
   *     - If this pool does exist, but the offset is out of bounds, we will return the out-of-bounds
   *       address based on the simple `base + offset` formula.  If offset is negative, we'll return pre-pool
   *       vaddr; if offset is positive and equals or exceeds pool size, we'll return the post-pool vaddr.
   *       Reason: See #pool_offset_t doc header.
   *
   * If `S_CAN_STORE_RAW_PTR == true`:
   *   - If `is_raw()`: See above `!S_CAN_STORE_RAW_PTR` case.  Same deal here.
   *   - Else: We return the stored address (in canonical form).
   *
   * In short: get() will *not* yield `nullptr` for various out-of-bounds situations.  to_bool() conversion
   * will act consistently with this.
   *
   * @return See above.
   */
  void* get() const;

  /**
   * Returns `false` if and only if `get() == nullptr`.  Please see get() doc header for notes regarding
   * when `nullptr` is, and is not, returned (this may be subtler than one might assume).
   *
   * @return See above.
   */
  bool to_bool() const;

  /**
   * Returns `true` if and only if `get() == other.get()`, albeit with certain acceptable exceptions and a
   * better perf profile in practice.  The purpose is to provide a faster implemention of `==` and `!=` for
   * Shm_pool_offset_ptr.  (Flip the result of equals() to get `!=`.)
   *
   * Formal contract is as noted above; and the exceptions were we might not return `.get() == other.get()` are
   * as follows.  Assume `*this` is P1, `other` is P2.
   *   - If P1 and P2 are non-null (`.to_bool() == true`) and non-raw (`.is_raw() == false`; always the case
   *     if #S_CAN_STORE_RAW_PTR is `false`); *and*
   *   - P1's pool is different from P2's pool; *and*
   *   - the offset for P1 or P2 or each of them points to outside the aforementioned pool
   *     (hence offset is either negative or past pool's size); *then*:
   *   - we shall return `false` (not-equal), even though in reality `get() == other.get()` is possible.
   *   - We consider this a pathological case worth the perf gains realized.
   *
   * @param other
   *        Thing against which to compare.
   * @return See above.
   */
  bool equals(Shm_pool_offset_ptr_data other) const;

  /**
   * Returns `true` if and only if `get() < other.get()`.  The purpose is to provide a faster implemention of
   * `<` and `>=` for Shm_pool_offset_ptr.  (Flip the result of less_than() to get `>=`.)
   *
   * @param other
   *        Thing against which to compare.
   * @return `get() < other.get()`.
   */
  bool less_than(Shm_pool_offset_ptr_data other) const;

  /**
   * Returns `true` if and only if `get() > other.get()`.  The purpose is to provide a faster implemention of
   * `>` and `<=` for Shm_pool_offset_ptr.  (Flip the result of greater_than() to get `<=`.)
   *
   * @param other
   *        Thing against which to compare.
   * @return `get() > other.get()`.
   */
  bool greater_than(Shm_pool_offset_ptr_data other) const;

  /**
   * Increments `*this` by a number of bytes (which can be positive, negative, or zero).  As explained in
   * get() and #pool_offset_t doc headers this is maximally permissive, including when essentially nonsensical
   * get() return value might result.  Summary of edge cases including the aforementioned ones but not limited to them:
   *   - If `!*this` legitimately (we are null due to being so assigned, not due to the pool referred-to within
   *     becoming invalid), this will:
   *     - (if #S_CAN_STORE_RAW_PTR is `true`) act similarly to native `+=` (become numerically = the bits in `bytes`);
   *     - (else) no-op.  So don't do that.  Really, though, it would be ill-advised to do it with a raw pointer too.
   *   - If `*this` is a legit offset pointer, meaning it refers to an existing pool, and incrementing the stored
   *     offset by `bytes` (which might make it smaller) places `*this` before or past the pool boundary: We do so.
   *     Naturally dereferencing get() would yield undefined behavior; but for example `increment(-bytes)` would
   *     get `*this` back to its original state which might be just fine.
   *   - If arithmetic overflow occurs:
   *     - This function will not invoke undefined behavior (crash or similar).
   *     - get() will not either.
   *     - However no guarantees are made as to the value get() would return numerically.  Informally speaking an
   *       attempt is made to hew as close to native pointer behavior as possible, depending on is_raw(), but
   *       it is a best effort only.  Informally it is generally ill-advised to rely on any particular behavior
   *       at that point.
   *
   * @param bytes
   *        See above.
   */
  void increment(diff_t bytes) noexcept;

private:
  // Types.

  /// Short-hand from base.
  using rep_t = Base::rep_t;
  /// Short-hand from base.
  using Offset_ptr_rep = Base::Offset_ptr_rep;
  /// Short-hand from base.
  using Raw_ptr_rep = Base::Raw_ptr_rep;
  /// Type of `m_rep`.  @see #m_rep.
  using Representation = union
  {
    /// The full raw bits.
    rep_t m_rep;
    /// The bits viewed in the offset-ptr form.
    Offset_ptr_rep m_offset_ptr_rep;
    /// The bits viewed in the raw-ptr form.
    Raw_ptr_rep m_raw_ptr_rep;
  };

  // Methods.

  /**
   * Helper, surely inlined with any decent optimizer, that returns what get() would return if
   * #m_rep equalled the supplied value and its MSB were 0 but at least one other bit were 1.  In plainer language,
   * we know it's not null, and its MSB indicates a raw pointer, then this returns the valid (canonical)
   * pointer represented.
   *
   * Must not be compiled unless #S_CAN_STORE_RAW_PTR is `true`.
   *
   * @param rep
   *        Would-be #m_rep; does not equal zero; but MSB but be zero; or UB results.
   * @return What get() would return givem `m_rep == rep`.
   */
  static void* get_as_raw(Representation rep);

  // Friends.

  /// Friend of this class (cross-`CAN_STORE_RAW_PTR` copy ctor needs access to `m_rep`).
  friend class Shm_pool_offset_ptr_data<Repository_type, !S_CAN_STORE_RAW_PTR>;

  /// Friend of this class.
  template<typename Repository_type2, bool CAN_STORE_RAW_PTR2>
  friend std::ostream& operator<<(std::ostream& os,
                                  Shm_pool_offset_ptr_data<Repository_type2, CAN_STORE_RAW_PTR2> val);

  /**
   * The raw bits.  See our class doc header and #Base internals.  The preferred method of interpreting it is:
   *   -# Check against simply being equal to 0 (all bits).  If so => not-a-pointer (null).
   *   -# Access `m_rep.m_offset_ptr_rep.m_selector_offset_else_raw`; if
   *      1 then: `Offset_ptr_rep::m_pool_id` specifies the pool in global #Repository singleton;
   *      `Offset_ptr_rep::m_pool_offset` is the offset off its base vaddr.  Else:
   *   -# All but the MSB of #m_rep = the same bits in the canonical result get() must return (the raw
   *      `void*`).  To compute the full canonical pointer (namely in get()):
   *      - The 2nd MSB (which happens to equal the following extended-sign bits) is in
   *        `m_rep.m_raw_ptr_rep.m_ext_sign_msb`.
   *      - get() must copy `m_rep.m_rep` but before returning this copy ensure its MSB equals `m_ext_sign_msb`.
   *        In other words if and only if `Raw_ptr_rep::m_ext_sign_msb` is 1, the MSB in the copy shall be set to 1.
   *
   * If `!S_CAN_STORE_RAW_PTR`, then (corruption/undefined behavior aside) in step 2 `m_selector_offset_else_raw == 1`
   * always.
   */
  Representation m_rep;
}; // class Shm_pool_offset_ptr_data

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Repository_type, bool CAN_STORE_RAW_PTR>
Shm_pool_offset_ptr_data<Repository_type, CAN_STORE_RAW_PTR>::Shm_pool_offset_ptr_data() :
  m_rep{ .m_rep = 0 }
{
  // Not-a-pointer: All bits are zero.  (See class doc header for explanation.)
}

template<typename Repository_type, bool CAN_STORE_RAW_PTR>
Shm_pool_offset_ptr_data<Repository_type, CAN_STORE_RAW_PTR>::Shm_pool_offset_ptr_data(const void* p)
{
  // m_rep is uninitialized garbage.

  if (!p)
  {
    m_rep.m_rep = 0;
    return;
  }
  // else

  pool_id_t pool_id_or_0;
  pool_offset_t pool_offset;
  Repository::from_address(p, pool_id_or_0, pool_offset); // @todo Would compiler let us target m_pool_* directly?

  if (pool_id_or_0 == 0)
  {
    if constexpr(S_CAN_STORE_RAW_PTR)
    {
      /* This is slightly subtle; if one revisits the class doc header, one sees that, in order for
       * raw-pointer get() to at most merely need to flip one bit and otherwise return the result as-is, we
       * store as many bits of the canonical form as possible (all but the MSB); and only set the MSB to the
       * required selector value (0). */
      m_rep.m_rep = reinterpret_cast<rep_t>(p);

#if 0 // Can be interesting to temporarily enable to ensure our assumptions about canonical form seem to hold.
      // Perf: Enable only when perf does not matter.
      {
#  ifndef NDEBUG
        // Shift by 47 bits = 0... then 17 previously MSB.
        const auto sign_bits = m_rep.m_rep >> (Raw_ptr_rep::S_N_VAL_BITS - 1);
#  endif
        assert(((sign_bits == rep_t(0)) // First 17 bits were 0s.  Or:
                ||
                // First 17 bits were 1s.  (1111...1 + 1 = 10000...0 = 1 then 17 0s, which is 1 << 17.)
                ((sign_bits + rep_t(1)) == (rep_t(1) << ((sizeof(rep_t) * 8) - Raw_ptr_rep::S_N_VAL_BITS + 1))))
               && "A non-canonical raw pointer encountered.");
      }
#endif

      // Set MSB to 0.  Operate on bit-field for clarity and probably speed (see class doc header for discussion).
      m_rep.m_raw_ptr_rep.m_selector_offset_else_raw = rep_t(0);
    } // if constexpr(S_CAN_STORE_RAW_PTR)
    else // if constexpr(!S_CAN_STORE_RAW_PTR)
    {
      /* Just leave it as null and pray for happiness (as promised).
       * Reminder: Repository::from_address() is allowed to behave this way, in which case we do this;
       * and it is allowed to yield UB; then we yield UB. */
      m_rep.m_rep = 0;
    }
  } // if (!pool_id_or_0 == 0)
  else // if (pool_id_or_0 != 0): Found in a SHM pool.
  {
    // Operate on bit-field for clarity and probably speed (see class doc header for discussion).
    m_rep.m_offset_ptr_rep.m_selector_offset_else_raw = rep_t(1);
    m_rep.m_offset_ptr_rep.m_pool_id = pool_id_or_0;
    m_rep.m_offset_ptr_rep.m_pool_offset = pool_offset;
  }
} // Shm_pool_offset_ptr_data::Shm_pool_offset_ptr_data(const void* p)

template<typename Repository_type, bool CAN_STORE_RAW_PTR>
Shm_pool_offset_ptr_data<Repository_type, CAN_STORE_RAW_PTR>::Shm_pool_offset_ptr_data
  (const Shm_pool_offset_ptr_data&) = default;

template<typename Repository_type, bool CAN_STORE_RAW_PTR>
Shm_pool_offset_ptr_data<Repository_type, CAN_STORE_RAW_PTR>::Shm_pool_offset_ptr_data
  (const Shm_pool_offset_ptr_data<Repository_type, !S_CAN_STORE_RAW_PTR>& src)
{
  /* The bit encoding for null (all zeros) and offset pointers (MSB=1, pool_id, pool_offset) is identical
   * regardless of CAN_STORE_RAW_PTR.  So we can copy the bits directly for those cases, avoiding the double
   * lookup that `Shm_pool_offset_ptr_data(src.get())` would incur (resolve to vaddr via to_address(), then
   * from_address() back to pool_id/offset).
   *
   * The only case needing special handling: src is a raw pointer (non-null, MSB=0), and we are offset-only
   * (!S_CAN_STORE_RAW_PTR).  See below. */

  if constexpr(S_CAN_STORE_RAW_PTR)
  {
    // offset-only -> raw-allowed: src can only be null or offset (never raw).  Both are valid as-is.
    m_rep.m_rep = src.m_rep.m_rep;
  }
  else
  {
    // raw-allowed -> offset-only: src can be null, offset, or raw.
    if (src.is_raw())
    {
      /* src is raw (non-null, selector=0).  We cannot represent it.  We shouldn't be converting from a
       * raw-allowing, raw pointer to an offset-only pointer, as a successful conversion wouldn't be expected,
       * and there isn't a use case to do this, that we know of anyway.
       *
       * Since we cannot represent it, we store null as advertised.  (This is consistent with how the void* ctor
       * handles not-in-a-pool addresses, if that could even be detected, when !S_CAN_STORE_RAW_PTR.)
       *
       * (One option -- which would mean changing our contract -- would be to essentially delegate to
       * the void*-taking ctor, giving it src.get().  That is try Repository::from_address(); if it yields
       * pool_id=0 then save null anyway; but otherwise yay, we found our representation as an offset-ptr after all.
       * However that only makes our contract weirdly non-deterministic.  This raises a question: why does
       * the void*-taking ctor do what it does then?  Answer: It is different, because it has only a raw ptr
       * to begin with; it *must* do from_address() in the first place; from_address() is an outside force;
       * if it happens to not explode, then we might as well also not-explode while formally promising and delivering
       * UB.  Here, on the other hand, we don't have to call any such thing: We have `src` already as a
       * Shm_pool_offset_ptr_data, same class template as *this; and src.is_raw() is a misuse of the API.) */
      m_rep.m_rep = 0;
    }
    else
    {
      m_rep.m_rep = src.m_rep.m_rep; // null or offset pointer: bits are valid as-is.
    }
  }
} // Shm_pool_offset_ptr_data::Shm_pool_offset_ptr_data(cross-type copy ctor)

template<typename Repository_type, bool CAN_STORE_RAW_PTR>
Shm_pool_offset_ptr_data<Repository_type, CAN_STORE_RAW_PTR>&
  Shm_pool_offset_ptr_data<Repository_type, CAN_STORE_RAW_PTR>::operator=(const Shm_pool_offset_ptr_data&)
    = default;

template<typename Repository_type, bool CAN_STORE_RAW_PTR>
void* Shm_pool_offset_ptr_data<Repository_type, CAN_STORE_RAW_PTR>::get_as_raw(Representation rep) // Static.
{
  // See the void* ctor and/or the class doc header; it should explain the following pretty well.  Keeping cmnts light.

  static_assert(S_CAN_STORE_RAW_PTR,
                "This helper method is meaningless, unless it is possible to store raw vaddrs.  "
                  "Granted, we are static, so it would still work algorithmically speaking; but "
                  "this way protects from trying to call us in unqualified form for the wrong type at least.");

#if 0 // Avoid the perf hit even from an assert().  Could enable when debugging perhaps.
  assert((rep.m_offset_ptr_rep.m_selector_offset_else_raw == rep_t(0))
         && "Precondition to helper method = the selector bit <=> raw.");
#endif

  if (rep.m_raw_ptr_rep.m_ext_sign_msb != rep_t(0))
  {
    rep.m_raw_ptr_rep.m_selector_offset_else_raw = rep_t(1);
  }
  return reinterpret_cast<void*>(rep.m_rep);
}

template<typename Repository_type, bool CAN_STORE_RAW_PTR>
void* Shm_pool_offset_ptr_data<Repository_type, CAN_STORE_RAW_PTR>::get() const
{
  // See the void* ctor and/or the class doc header; it should explain the following pretty well.  Keeping cmnts light.

  // We suspect this is common enough to where doing this before any bit/bit-sequence access = overall optimization.
  if (m_rep.m_rep == rep_t(0))
  {
    return nullptr;
  }
  // else

  // Operate on bit-field for clarity and probably speed (see class doc header for discussion).
  if constexpr(S_CAN_STORE_RAW_PTR)
  {
    if (m_rep.m_offset_ptr_rep.m_selector_offset_else_raw == rep_t(0))
    {
      return get_as_raw(m_rep);
    } // if (m_rep.m_offset_ptr_rep.m_selector_offset_else_raw == rep_t(0))
    // else if (m_rep.m_offset_ptr_rep.m_selector_offset_else_raw != rep_t(0)): Fall through.
  } // if constexpr(S_CAN_STORE_RAW_PTR)
  else // if constexpr(!S_CAN_STORE_RAW_PTR)
  {
#if 0 // Avoid the perf hit even from an assert().  Could enable when debugging perhaps.
    assert((m_rep.m_offset_ptr_rep.m_selector_offset_else_raw == rep_t(1))
           && "How'd we reach such a state in 1st place?  Constructor shouldn't; increment() should not either.");
#endif
  }

  // Per contract: if this yields null (pool not found), we return null.  If it yields UB, we yield UB.
  return Repository::to_address(m_rep.m_offset_ptr_rep.m_pool_id, m_rep.m_offset_ptr_rep.m_pool_offset);
} // Shm_pool_offset_ptr_data::get()

template<typename Repository_type, bool CAN_STORE_RAW_PTR>
bool Shm_pool_offset_ptr_data<Repository_type, CAN_STORE_RAW_PTR>::is_raw() const
{
  if constexpr(S_CAN_STORE_RAW_PTR)
  {
    // Suppose MSB = 0; if all other bits = 0 then null; if at least one is 1 then raw.  Otherwise neither.
    return (m_rep.m_offset_ptr_rep.m_selector_offset_else_raw == pool_offset_t(0))
             && (m_rep.m_rep != rep_t(0));
  }
  else
  {
    return false; // Cannot be raw.
  }
}

template<typename Repository_type, bool CAN_STORE_RAW_PTR>
bool Shm_pool_offset_ptr_data<Repository_type, CAN_STORE_RAW_PTR>::to_bool() const
{
  return m_rep.m_rep != rep_t(0);
}

template<typename Repository_type, bool CAN_STORE_RAW_PTR>
bool Shm_pool_offset_ptr_data<Repository_type, CAN_STORE_RAW_PTR>::equals(Shm_pool_offset_ptr_data other) const
{
  constexpr bool NO_RAW = !S_CAN_STORE_RAW_PTR;
  constexpr bool RESULT_EQ = true; // Harder to have a brain-glitch this way, we've found.
  constexpr bool RESULT_NE = false;

  /* Consult doc header first, then come back here.  So: we could just `return get() == other.get()`, and we only
   * do not, because we want higher perf than that.  However, as we reflected in doc header (which, to be fair,
   * in this case is worded how it is *because of* the reasoning in this impl, less so the reverse), our gold
   * standard for correctness is indeed `get() == other.get()`, *but* we are allowed to make exceptions when
   * doing so (1) would lead to higher performance and (2) is defensible in practical terms (so, something like
   * "in this logic branch we might not return `get == other.get()`, but it is OK because <practical reason>").
   * So we'll do that kind of reasoning below.
   *
   * Since get() behavior matters for the "gold standard" of our behavior, we may at points rely on its exact
   * impl; and that's fine; but remember one thing in particular: if we are not null (to_bool() == true), and
   * !is_raw(), then `get() == Repository::to_address(m_pool_id, m_pool_offset)`, which in turn very specifically
   * is:
   *   - lookup base-vaddr by m_pool_id in (conceptual, actual impls may vary for perf reasons) map
   *       [not found => undefined behavior (UB) => by our contract we don't care]
   *   - add base-vaddr + m_pool_offset; return result
   *       [result being before base-vaddr or past pool-end => *not* UB => we must handle it]
   *
   * The key question: How would we do better perf-wise than `get() == other.get()`?  After all if we can't,
   * then might as well just return that.  Answer: If x is not null and !x.is_raw() (incidentally always the case
   * if NO_RAW), then x.get() involves a map lookup of some kind.  Depending on Repository impl (and they do
   * quite vary; as of this writing Borrower_shm_pool_collection_repository and Owner_shm_pool_repository are
   * the two possibilities in Flow-IPC proper, and certainly their impls are quite different), at a minimum
   * some map lookup must occur.  Conceivably there can also be locking.  In any case, the minimum case of a lock-free
   * hash-map lookup *times two* may not sound "that bad," but consider that generally people are used to
   * raw pointers, and with raw pointers there is *no* extra calculation; .get() would just yield the numeric
   * value of the pointer.  A map-lookup is much slower.  Moreover we have seen production high-load environments
   * in which .get()-due-to-ptr-equality-comparisons showed up as a processor hot spot.  (In that particular case
   * it was due to a `for(a:b)` loop, `b` being an in-SHM vector; the comparison to b.end() meant many lookups.)
   * So it's serious business.
   *
   * If we can avoid .get() -- or more precisely Repository::to_address() firstly and get_as_raw() as a distant
   * second priority -- relying on only a few comparisons instead, then great.
   *
   * Spoiler alert: In the final analysis (by algorithm inspection, not necessarily empirically), the below
   *   - reduces to a comparison of integers if !CAN_STORE_RAW_PTR; otherwise:
   *   - reduces to same if they're indeed numerically equal (surely not uncommon); otherwise:
   *   - reduces to a handful of 0/bit/equality comparisons in most other situations, by inducing a
   *     likely-safe exception to the "gold standard";
   *   - worst-case, in an atypical situation, is similar to `.get() == other.get()`. */

  const auto& other_rep = other.m_rep;

  if (m_rep.m_rep == other_rep.m_rep)
  {
    return RESULT_EQ; // Bitwise equal => get()s would be obviously also equal.
  }
  // else

  if constexpr(NO_RAW)
  {
    return RESULT_NE;
    /* Let's prove that is correct.  Analyzing possibilities:
     *   - If we are null (!to_bool()), then other must have equal value for EQ, but it is different, so NE is right.
     *   - Same but flipped.
     *   - Hence neither is null; and since NO_RAW, m_pool_id/offset and other.m_pool_id/offset are all
     *     valid and in-use values.  So:
     *   - If m_pool/offset and other.m_pool_id/offset *were* pairwise equal, then it would result in EQ.
     *     But `m_rep != other_rep`, so that's not the case.  So, result must be NE, unless...
     *   - ...there is some way for m_pool/offset and other.m_pool_id/offset to *not* be pairwise equal, yet
     *     for `get() == other.get()`.  Is there?  Let's contemplate the possibilities.  Assume
     *     they're not pairwise equal, and result is EQ: `get() == other.get()`.
     *   - If `m_pool_id == other.m_pool_id`, then EQ can only result if `m_pool_offset == other.m_pool_offset`;
     *     but we've established non-pairwise-equality.  So `m_pool_id != other.m_pool_id`.
     *   - If m_offset and other.m_pool_offset are both within-pool-bounds (not negative, not past respective
     *     pool sizes), then get() and other.get() are within separate pools.  Pools do not overlap.
     *     Therefore one, or both, of `m_pool_offset`s are outside their respective pool bounds.
     *   - The answer is yes.  E.g., say m_pool_offset is 0, while other.m_pool_offset is the distance, or
     *     negative distance (depending on whether our pool or `other` pool comes first by vaddr value),
     *     between our base-vaddr and `other` base-vaddr -- even if that's billions of bytes apart.  In that
     *     case `get() == other.get()` -- EQ -- but we shall return NE.
     *
     * We consider this to be a reasonable exception to the "gold standard" (`get() == other.get()`) and thus
     * in our contract pointed it out.  Why?  Answer: It is a pathological case.  In practice it's difficult to
     * conceive of a situation where this would naturally come about without trying to make it happen.  It isn't
     * 100% inconceivable, but we document it and live with the unlikely bad consequences.  It is worth
     * not having to do get() here.  (Though, at least if would've already correctly returned EQ on bitwise
     * equality.) */
  } // if constexpr(NO_RAW)
  else // if constexpr(CAN_STORE_RAW_PTR)
  {
    /* The below could be written as follows, but we unwind (in admittedly relatively ugly fashion) ~everything to
     * do as little recalculating as possible, making it all very explicit.  Overkill?  Possibly?  Maintenance risk?
     * A bit, probably.  All in all though might be worthwhile given how paranoid about perf we are trying to be. */
#if 0
    return ((!to_bool()) || (!other.to_bool()) || (is_raw() == other.is_raw()))
             ? RESULT_NE;
             : (get() == other.get());
#else // The actual code then:
    if (m_rep.m_rep == rep_t(0)) { return RESULT_NE; } // `other` is not null.
    if (other_rep.m_rep == rep_t(0)) { return RESULT_NE; } // *this is not null.
    // else if (neither is null):

    const bool is_raw = m_rep.m_offset_ptr_rep.m_selector_offset_else_raw == pool_offset_t(0);
    if (is_raw
        ==
        (other_rep.m_offset_ptr_rep.m_selector_offset_else_raw == pool_offset_t(0)))
    {
      return RESULT_NE;
    }
    // else if (neither is null) && (is_raw != other.is_raw())

    return (is_raw ? (get_as_raw(m_rep)
                      ==
                      Repository::to_address(other_rep.m_offset_ptr_rep.m_pool_id,
                                             other_rep.m_offset_ptr_rep.m_pool_offset))
                   : (get_as_raw(other_rep) // !is_raw => other.is_raw()
                      ==
                      Repository::to_address(m_rep.m_offset_ptr_rep.m_pool_id,
                                             m_rep.m_offset_ptr_rep.m_pool_offset)))
             ? RESULT_EQ : RESULT_NE;
#endif

    /* Let's prove that is correct.  Please refer to the `if 0`ed nicely-readable version which is equivalent to
     * the longer-but-hopefully-a-bit fast one just above.  Analyzing possibilities:
     *
     *   - If we are null, or `other` is null: See bullet points 1-2 in the proof cmnt in the NO_RAW case above.  Same.
     *     So assume neither is null.  Hence we are raw or an offset-ptr, as it `other` one of the two.
     *   - If `is_raw() == other.is_raw()`:
     *     - If !is_raw(): See the proof above for the NO_RAW case (starting with bullet point 3).  Same applies.
     *     - If is_raw(): Then it's just a regular raw vaddr comparison (except MSB is unconditionally 0 for both,
     *       but it carries no info, as it's part of the canonial form's filler bits); certainly if they're numerically
     *       unequal then NE is correct.  (This is stronger than the preceding bullet: There is no pathological way
     *       for NE to be technically wrong.)
     *   - So assume is_raw() and !other.is_raw() (the reverse could be the case, but by symmetry the same
     *     reasoning below would apply equally well), and both are non-null.
     *
     * And in that case we use the (slow) "gold standard": `get() == other.get()`.  (It is unwound a bit to avoid
     * redundant computing: since is_raw, we can directly use the get_as_raw() part of get(); and since
     * !other.is_raw() and non-null, we can directly use the to_address() part of other.get().)
     *
     * That is correct by definition.  QED.
     *
     * ...but maybe we can do something faster and still okay?  Well, is_raw(), so get() is just
     * equal to reinterpret_cast<void*>(m_rep) + potentially flip of MSB (get_as_raw()) -- quick enough;
     * but there's simply no way to deal with `other` other than calling to_address(other.m_pool_id/offset)
     * which involves one of the lookups we were trying to avoid.  If we don't want to do that, then:
     * Our only other option is the ol' `return NE`.  Would that be okay in that any
     * false NE would be pathological?  Answer: Kind of.  It's somewhat similar to the pathological case
     * outlined in the NO_RAW comment but 50% less so: One of the sides is a raw address, but the other one
     * represents the same vaddr but in a pool.  Like maybe they created an offset-pointer to a pool-free
     * location and then allocated to create a pool at that "suspected" place and are now checking.  It's "thin";
     * less thin than the NO_RAW scenario though.
     *
     * Here's our reasoning to play it safe: We just don't think the scenario of comparing an owner-side
     * (CAN_STORE_RAW_PTR) pointer to not-in-SHM versus a pointer to in-SHM is likely enough to need to
     * perf-optimize (at which point just doing the definitely-correct thing wins).  Copying bytes between the
     * two, yes, totally plausible.  Why compare them though?  E.g., loop-comparing against .end() would be in
     * the same container so probably all in-SHM or all on stack/in heap.  Or a list<> comparing node vs another
     * node... same deal.
     *
     * Plus at least it's only one to_address() (map lookup) as opposed to two.
     *
     * @todo Revisit; gather some stats, perhaps under load with diverse production code bases. */
  } // else if constexpr(CAN_STORE_RAW_PTR)
} // Shm_pool_offset_ptr_data::equals()

template<typename Repository_type, bool CAN_STORE_RAW_PTR>
bool Shm_pool_offset_ptr_data<Repository_type, CAN_STORE_RAW_PTR>::less_than(Shm_pool_offset_ptr_data other) const
{
  constexpr bool RESULT_LT = true; // Harder to have a brain-glitch this way, we've found.
  constexpr bool RESULT_GE = false;

  /* We could just `return get() < other.get()`, and we only do not, because we want
   * higher perf than that.  However, as we reflected in doc header, our gold
   * standard for correctness is indeed `get() == other.get()`, *but* we are allowed to make exceptions when
   * doing so (1) would lead to higher performance and (2) is defensible in practical terms.
   * So we'll do that kind of reasoning below.  However: it turns out that, unlike in equals(), we are able
   * to avoid any exceptions for the sake of perf here.
   *
   * Since get() behavior matters for the "gold standard" of our behavior, we may at points rely on its exact
   * impl; and that's fine; but remember one thing in particular: if we are not null (to_bool() == true), and
   * !is_raw(), then `get() == Repository::to_address(m_pool_id, m_pool_offset)`, which in turn very specifically
   * is:
   *   - lookup base-vaddr by m_pool_id in (conceptual, actual impls may vary for perf reasons) map
   *       [not found => undefined behavior (UB) => by our contract we don't care]
   *   - add base-vaddr + m_pool_offset; return result
   *       [result being before base-vaddr or past pool-end => *not* UB => we must handle it]
   *
   * The key question: How would we do better perf-wise than `get() == other.get()`?  After all if we can't,
   * then might as well just return that.  Answer: <similar to what we wrote in equals() in analogous spot>.
   *
   * If we can avoid .get() -- or more precisely Repository::to_address() firstly and get_as_raw() as a distant
   * second priority -- relying on only a few comparisons instead, then great.
   *
   * Spoiler alert: In the final analysis (by algorithm inspection, not necessarily empirically), the below is
   * pretty similar perf-wise to just `return get() < other.get()` except for one likely very-common situation:
   * both sides non-null, non-raw (100% of the time if !CAN_STORE_RAW_PTR, most of the time otherwise), and
   * in the same SHM-pool.  In that very-common situation it reduces to a few 0 checks and a comparison of
   * two (in x86-64, 32-bit) integers. */

  const auto& other_rep = other.m_rep;
  const bool other_is_null = other_rep.m_rep == rep_t(0);

  /* First handle/eliminate situations where we are null (!to_bool()) and/or `other` is null.  Granted
   * `<` comparisons between null and <anything> are probably fairly rare, so it'd be nice to not check for that
   * until we have to (all else being equal); but logically it seems hard to avoid doing it first.  At least
   * a check-for-zero should be quite quick. */

  if (m_rep.m_rep == rep_t(0))
  {
    return other_is_null ? RESULT_GE : RESULT_LT; // null >= null / null < non-null.
  }
  // else
  if (other_is_null) // && *this is not null
  {
    return RESULT_GE; // non-null >= null.
  }
  // else neither is null:

  /* Next handle/eliminate situations where one or both is a raw pointer (MSB indicates rawness; the other bits
   * contain actual vaddr being encoded; MSB in vaddr would be copy of 2nd MSB and carries no real info).
   * Happily !CAN_STORE_RAW_PTR means by definition neither one is raw, so that block can be skipped. */

  if constexpr(S_CAN_STORE_RAW_PTR)
  {
    const bool is_raw = m_rep.m_offset_ptr_rep.m_selector_offset_else_raw == pool_offset_t(0);
    if (is_raw != (other_rep.m_offset_ptr_rep.m_selector_offset_else_raw == pool_offset_t(0)))
    {
      /* One is raw; the other is not.  No choice but to essentially execute .get() for both sides and compare.
       * However we can skip some redundancies based on information we do have and reduce .get() on one side
       * to get_as_raw() (possible bit flip but that's it) and the "slow" lookup on the other side. */
      return (is_raw ? (get_as_raw(m_rep)
                        <
                        Repository::to_address(other_rep.m_offset_ptr_rep.m_pool_id,
                                               other_rep.m_offset_ptr_rep.m_pool_offset))
                     : (get_as_raw(other_rep) // !is_raw => other.is_raw()
                        >
                        Repository::to_address(m_rep.m_offset_ptr_rep.m_pool_id,
                                               m_rep.m_offset_ptr_rep.m_pool_offset)))
               ? RESULT_LT : RESULT_GE;
    }
    // else if (is_raw == other.is_raw()):
    if (is_raw)
    {
      /* Both are raw.  That's promising, in that it is tempting to just return `m_rep.m_rep < other_rep.m_rep`
       * (compare the bits' numeric values).  Unfortunately it is possible that the numeric of value of actual
       * vaddr get() (by now, get_as_raw()) flips the MSB of .m_rep.  If that were true for neither or both sides:
       * no problem; we could still just use `m_rep.m_rep < other_rep.m_rep`.  It can be true for exactly one side
       * too though.  Hence we need to do the bit-flip, if indeed 2nd MSB indicates this, for each side first
       * and then compare.  It should be quick. */
      return (get_as_raw(m_rep) < get_as_raw(other_rep)) ? RESULT_LT : RESULT_GE;
    }
    // else if both are non-null and non-raw (both are offset pointers).  Fall through:
  }
  else // if constexpr(!CAN_STORE_RAW_PTR)
  {
#if 0 // Avoid the perf hit even from an assert().  Could enable when debugging perhaps.
    assert((!is_raw()) && "Cannot be raw if !CAN_STORE_RAW_PTR.");
    assert((!other.is_raw()) && "`other` cannot be raw if !CAN_STORE_RAW_PTR.");
#endif
  }

  // To recap: both are non-null and non-raw (both are offset pointers).

  if (m_rep.m_offset_ptr_rep.m_pool_id == other_rep.m_offset_ptr_rep.m_pool_id)
  {
    /* This is probably quite common (certainly not universal though): things being compared tend to live near each
     * other, within the same vaddr area (extent, pool).  So in many cases the whole thing amounts to:
     * two comparisons against zero, [if CAN_STORE_RAW_PTR: 2ish bit computations,] and `integer1 < integer2`. */
    return (m_rep.m_offset_ptr_rep.m_pool_offset < other_rep.m_offset_ptr_rep.m_pool_offset)
             ? RESULT_LT : RESULT_GE;
  }
  // else

  return (Repository::to_address(m_rep.m_offset_ptr_rep.m_pool_id, m_rep.m_offset_ptr_rep.m_pool_offset)
          <
          Repository::to_address(other_rep.m_offset_ptr_rep.m_pool_id, other_rep.m_offset_ptr_rep.m_pool_offset))
           ? RESULT_LT : RESULT_GE;
} // Shm_pool_offset_ptr_data::less_than()

template<typename Repository_type, bool CAN_STORE_RAW_PTR>
bool
  Shm_pool_offset_ptr_data<Repository_type, CAN_STORE_RAW_PTR>::greater_than(Shm_pool_offset_ptr_data other) const
{
  constexpr bool RESULT_GT = true; // Harder to have a brain-glitch this way, we've found.
  constexpr bool RESULT_LE = false;

  /* This impl is similar to less_than(), so please grok that guy; then come back here.  Keeping comments light.
   *
   * Code reuse (beyond get_as_raw() reuse) is not impossible (maybe a template param for whether we'll do
   * less-than or greater-than), but seems the resulting code would just be harder to understand, and the
   * gains for easier maintenance/reduced bugginess would be marginal. */

  const auto& other_rep = other.m_rep;
  const bool other_is_null = other_rep.m_rep == rep_t(0);

  if (m_rep.m_rep == rep_t(0))
  {
    return RESULT_LE; // null <= null / null <= non-null.
  }
  // else
  if (other_is_null) // && *this is not null
  {
    return RESULT_GT; // non-null > null.
  }
  // else neither is null:

  if constexpr(S_CAN_STORE_RAW_PTR)
  {
    const bool is_raw = m_rep.m_offset_ptr_rep.m_selector_offset_else_raw == pool_offset_t(0);
    if (is_raw != (other_rep.m_offset_ptr_rep.m_selector_offset_else_raw == pool_offset_t(0)))
    {
      return (is_raw ? (get_as_raw(m_rep)
                        >
                        Repository::to_address(other_rep.m_offset_ptr_rep.m_pool_id,
                                               other_rep.m_offset_ptr_rep.m_pool_offset))
                     : (get_as_raw(other_rep) // !is_raw => other.is_raw()
                        <
                        Repository::to_address(m_rep.m_offset_ptr_rep.m_pool_id,
                                               m_rep.m_offset_ptr_rep.m_pool_offset)))
               ? RESULT_GT : RESULT_LE;
    }
    // else if (is_raw == other.is_raw()):
    if (is_raw)
    {
      return (get_as_raw(m_rep) > get_as_raw(other_rep)) ? RESULT_GT : RESULT_LE;
    }
    // else Fall through:
  }
  else // if constexpr(!CAN_STORE_RAW_PTR)
  {
#if 0 // Avoid the perf hit even from an assert().  Could enable when debugging perhaps.
    assert((!is_raw()) && "Cannot be raw if !CAN_STORE_RAW_PTR.");
    assert((!other.is_raw()) && "`other` cannot be raw if !CAN_STORE_RAW_PTR.");
#endif
  }

  // To recap: both are non-null and non-raw (both are offset pointers).

  if (m_rep.m_offset_ptr_rep.m_pool_id == other_rep.m_offset_ptr_rep.m_pool_id)
  {
    return (m_rep.m_offset_ptr_rep.m_pool_offset > other_rep.m_offset_ptr_rep.m_pool_offset)
             ? RESULT_GT : RESULT_LE;
  }
  // else

  return (Repository::to_address(m_rep.m_offset_ptr_rep.m_pool_id, m_rep.m_offset_ptr_rep.m_pool_offset)
          >
          Repository::to_address(other_rep.m_offset_ptr_rep.m_pool_id, other_rep.m_offset_ptr_rep.m_pool_offset))
           ? RESULT_GT : RESULT_LE;
} // Shm_pool_offset_ptr_data::greater_than()

template<typename Repository_type, bool CAN_STORE_RAW_PTR>
void Shm_pool_offset_ptr_data<Repository_type, CAN_STORE_RAW_PTR>::increment(diff_t bytes) noexcept
{
  static_assert(std::is_signed_v<diff_t>, "If diff_t is unsigned, we cannot really decrement pointers.");

  // Operate on bit-field for clarity and probably speed (see class doc header for discussion).
  if constexpr(S_CAN_STORE_RAW_PTR)
  {
    if (m_rep.m_offset_ptr_rep.m_selector_offset_else_raw == 0)
    {
      /* Subtlety: m_rep may be 0, meaning we are not holding a raw pointer per se (by our classification)
       * but rather represent nullptr.  However: this is allowed with native pointers (dereferencing the
       * result is another matter); for instance `uint8_t* x = 0; x += 4;` will make it hold address 0x4.
       * So we let it happen.
       *
       * Subtlety: One wonders if doing += or -= ops on native pointers keeps canonical-form rules in mind;
       * so if one overflows the lower 48 bits, perhaps x86-64 arch will avoid touching the nearby
       * extended-sign bits.  Experimentation, backed by docs, shows that is not the case: It simply does
       * the += or -= op on the underlying uint64_t.  Of course dereferencing the result is going to blow up
       * (processor exception => SEGV-type-thing), but that's beside the point.  So we do the same thing.
       *
       * Related subtlety: Our += is identical to native pointer +=; the only possible difference occurs in that
       * we might flip ->m_selector_offset_else_raw to 1, if they're doing an op on the outside edges of the
       * range.  This would turn us into an offset pointer; so a subsequent get() (which is not a deref yet!)
       * would behave unpredictably; it could return nullptr, or it could accidentally encode a real pool ID
       * and thus yield some real-ish address instead of the right thing (which, granted, itself is something
       * unreal).  A native `get()` in that state would return the "right thing."  So we have two choices
       * for what do after the `m_rep +=`.  1, we could force-clear the MSB (shift left, shift right).
       * This would yield, possibly, the "right thing" in subsequent get() (which flips the MSB if the
       * 2nd MSB is 1, which would probably be 1 in this scenario -- though not necessarily depending on
       * magnitude of `bytes`).  Else, 2, we could do nothing.  This would hit the aforementioned
       * non-deterministic behavior, wherein get() would take us to be an offset pointer.  One can make
       * a case for either.  I (ygoldfel) ultimately decided to do (2).  The motivation: in the by-far-mainstream
       * case, where no one is doing anything funky, it has higher performance.  The defense:
       * Performing overflowing pointer arithmetic is not disallowed (meaning it shouldn't lead to undefined
       * behavior) in and of itself; and reading a resulting pointer value afterwards shouldn't lead to
       * any exception or crash; but counting on any particular numeric value -- honestly I haven't tried to
       * absolutely confirm this formally in terms of the standard but just intuitively let's be real here --
       * is not in the cards.  Our impl doesn't crash anything here, or in subsequent get(), but any code
       * counting on some kind of particular resulting address after overflowing a pointer... shouldn't.  Nor
       * is it useful in any conceivable to me way.  So for performance's sake this is defensible. */
      m_rep.m_rep += bytes;
      return;
    } // if (raw_ptr_rep->m_selector_offset_else_raw == 0)
    // else: Fall through:
  } // if constexpr(S_CAN_STORE_RAW_PTR)
  else
  {
    if (m_rep.m_rep == rep_t(0)) // Recall that !S_CAN_STORE_RAW_PTR.
    {
      /* As advertised remain null.  We have no choice; we cannot express this any other way.  We could assert(),
       * but again we promised to behave -- ill-advised or not we stick to the promise. */
      return;
    }
    // else

#if 0 // Avoid the perf hit even from an assert().  Could enable when debugging perhaps.
    assert((m_rep.m_offset_ptr_rep.m_selector_offset_else_raw == rep_t(1))
           && "How'd we reach such a state in 1st place?  Constructor shouldn't; increment() should not either.");
#endif
  }

  m_rep.m_offset_ptr_rep.m_pool_offset += bytes; // Remember this may be negative (or 0).
  /* - This might have overflowed; similarly to the above "defense" we can state that this will cause to crash
   *   here or in get(), and no guarantees can be made or should be expected as to the numeric value of get()
   *   after such an operation.
   * - It might have gone out of bounds of a pool.  That is it might be negative; or positive but exceeding
   *   the pool size; this is discussed in #pool_offset_t doc header.  The conclusion based on that explanation:
   *   We should indeed let it happen.
   * - It might be entirely Kosher; so yay then. */
} // Shm_pool_offset_ptr_data::increment()

template<typename Repository_type, bool CAN_STORE_RAW_PTR>
std::ostream& operator<<(std::ostream& os,
                         Shm_pool_offset_ptr_data<Repository_type, CAN_STORE_RAW_PTR> val)
{
  using rep_t = typename Shm_pool_offset_ptr_data<Repository_type, CAN_STORE_RAW_PTR>::rep_t;
  using boost::io::ios_all_saver;

  if (val.m_rep.m_rep == rep_t(0))
  {
    return os << "null";
  }
  // else

  if constexpr(CAN_STORE_RAW_PTR)
  {
    if (!val.m_rep.m_offset_ptr_rep.m_selector_offset_else_raw)
    {
      ios_all_saver saver(os); // Revert std::hex/etc. soon.
      return os << "ext_sign_bit[" << val.m_rep.m_raw_ptr_rep.m_ext_sign_msb << "]... val_bits[0x"
                << std::hex << val.m_rep.m_raw_ptr_rep.m_val_bits << "]@" << val.get();
      // @todo Slight perf impact: val.get() has some redundancy to above selector bit check.
    }
    // else: Fall through:
  }
  return os << "pool_id[" << val.m_rep.m_offset_ptr_rep.m_pool_id
            << "]+[" << val.m_rep.m_offset_ptr_rep.m_pool_offset << "]@" << val.get();
} // operator<<(ostream, Shm_pool_offset_ptr_data)

} // namespace ipc::shm::arena_lend::detail

#if IPC_SHM_ARENA_LEND_DETAIL_GCC_COMPILER
#  pragma GCC diagnostic pop // See above.
#endif

#undef IPC_SHM_ARENA_LEND_DETAIL_GCC_COMPILER
