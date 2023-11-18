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

#pragma once

#include "ipc/common.hpp"
#include <flow/util/util.hpp>
#include <mutex>

/**
 * Segregated private stuff for ipc::shm::arena_lend.
 * @todo We don't really do `detail` sub-namespaces anywhere else, so maybe don't do it here or do do it all over?
 */
namespace ipc::shm::arena_lend::detail
{

#if defined(__GNUC__) && !defined(__clang__)
#  define IPC_SHM_ARENA_LEND_DETAIL_GCC_COMPILER 1
#else
#  define IPC_SHM_ARENA_LEND_DETAIL_GCC_COMPILER 0
#endif

/* gcc is pretty paranoid about some bit-field paths below and gives some *very* cryptic warnings
 * that amount to maybe-uninitialized.  The code appears solid, so let's bypass it temporarily. */
#if IPC_SHM_ARENA_LEND_DETAIL_GCC_COMPILER
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

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
 * We treat multi-byte things as numbers, not individual bytes; so for example `uint64_t n = 0x0001020304050607ull` is
 * stored in memory as `07 06 05 04 03 02 01 00`; but in code we might do `n & 0xff` to isolate the LS-byte and indeed
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
  // Types.

  /**
   * Pool offset type.  Outside of Shm_pool_offset_ptr_data no entity can request a pool larger than what
   * this can index.
   *
   * ### Impl: Why 32-bit width chosen for `pool_offset_t` ###
   * Per Shm_pool_offset_ptr_data doc header impl discussion, it must fit into #m_rep_t in addition to 1
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
   *   - If unsigned: it will add an overflowed 32-bit positive 0xffffffff (~2 billion) to the `uint64_t` base and
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

  /**
   * Pool ID type.  Outside of Shm_pool_offset_ptr_data you must use generate_pool_id() to generate new ones.
   *
   * ### Impl: Why `uint32_t` was chosen ###
   * Per Shm_pool_offset_ptr_data doc header impl discussion, it must fit into #m_rep_t in addition to 1
   * selector bit and #pool_offset_t.  We've chosen 32 LSB of 64-bit #rep_t to be #pool_offset_t; so that
   * leaves 31 bits; 32 bits is the smallest type available to hold a 31-bit unsigned number.
   */
  using pool_id_t = uint32_t;

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
    // Constants.

    /**
     * # of bits in #m_pool_offset (determines ceiling on size of pool; but note #pool_offset_t signedness-or-not
     * is a factor).
     */
    static constexpr unsigned int S_N_POOL_OFFSET_BITS = 32;

    // Data.  In reverse bit order as @noted below!

    /**
     * If and only if #m_selector_offset_else_raw: The offset within pool IDed by #m_pool_id.
     *
     * @note If #pool_offset_t is signed, then this *can* be negative!  Please read that alias's doc header.
     */
    volatile pool_offset_t m_pool_offset : S_N_POOL_OFFSET_BITS;

    /// If and only if #m_selector_offset_else_raw: The pool ID.
    volatile pool_id_t m_pool_id : (sizeof(rep_t) * 8) - S_N_POOL_OFFSET_BITS - 1;

    /**
     * Either 0 (raw or null pointer; do not use the other bit members) or 1 (offset pointer).
     *
     * @note With gcc/clang/x64-64 this is listed *last*, because it is the *most* significant datum.
     *       The fields are thus in reverse order of bit order.
     */
    volatile rep_t m_selector_offset_else_raw : 1;
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
    volatile rep_t m_val_bits : S_N_VAL_BITS;

    /**
     * If `!m_selector_offset_else_raw`, and the entire value is not all-zero-bits:
     * #m_ext_sign_msb repeated.
     */
    volatile rep_t m_ext_sign_bits_except_msb : ((sizeof(rep_t) * 8) - S_N_VAL_BITS - 1 - 1);

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
    volatile rep_t m_ext_sign_msb : 1;

    /**
     * Either 0 (raw or null pointer; if all other bits are also 0, then do not use the other bit members) or 1
     * (do not use the other bit members).  When converting to a canonical `void*`: leave the rest of the
     * bits alone; but set this one to equal #m_ext_sign_msg (the next LSB).
     *
     * @note With gcc/clang/x64-64 this is listed *last*, because it is the *most* significant datum.
     *       The fields are thus in reverse order of bit order.
     */
    volatile rep_t m_selector_offset_else_raw : 1;
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

  /// The SHM-object (pool) mapped by #s_pool_id_shm_region_or_none.  Default-cted until initialization.
  static bipc::shared_memory_object s_pool_id_shm_obj_or_none;

  /**
   * Initialized no more than once (in this process) by generate_pool_id(), a handle to tiny SHM region
   * storing (only) an `atomic<rep_t>` used to generate unique pool IDs.  Default-cted until initialization.
   */
  static bipc::mapped_region s_pool_id_shm_region_or_none;
}; // class Shm_pool_offset_ptr_data_base

/**
 * Implementation core of Shm_pool_offset_ptr.  It provides the latter's essential capabilities while leaving out
 * the interface-y necessities of a standard fancy-pointer type such as the concept of the pointed type, `rebind`,
 * and so on.  One can think of us implementing the core of `uint8_t*` only (conceptually speaking);
 * as opposed to a `T*` parameterized on `T`.
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
 *         (a/k/a *handle*).  Returning null implies the input args are corrupt/wrong.
 *       - `static void from_address(const void*, pool_id_t& pool_id, pool_offset_t& pool_offset)`: The opposite.
 *         Sets `pool_id = 0` (a special invalid ID), if the input `void*` is not in a SHM-pool.
 *   - Alternatively, for the case where a datum is located outside any SHM pool (perhaps on the stack),
 *     so in particular `from_address()` would yield `pool_id == 0`, it instead stores a raw vaddr.
 *     - Depending on the compile-time situation this alternative may be disallowed (`CAN_STORE_RAW_PTR`
 *       template parameter is `false`).  In particular in the shm::arena_lend design that is the case on the borrower
 *       side (when interpreting a data structure created by another process and transmitted to -- borrowed
 *       by -- this one).
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
 * @note The key members, most importantly `m_rep`, are declared `volatile`.  This is subtly important.
 *       Without any `volatile` at least gcc-9, at least with -O3, saw fit to optimize in such a way as
 *       to sometimes cause `.increment()` to either no-op or not run at all (it was not immediately clear which).
 *       What was clear was that a pointer `+=` or `+` would have no effect.
 *       Adding nothing more than `std::cout` debug statements inside various branches of
 *       `.increment()` made it "snap out of it" and do what the code logically wants.  In other words
 *       the optimizer apparently believed certain assignment code had no side effect, perhaps due to the
 *       aforementioned aliasing technique being used.  However adding `volatile` made it "understand"
 *       and behave as expected.  This can be seen as an argument in favor of the bit-shift approach
 *       and against type-punning.  That said for now we stick with the latter -- while emphasizing the
 *       importance of exhaustive unit-testing (which we do have).
 *
 * @todo Internally to Shm_pool_offset_ptr_data and its base Shm_pool_offset_ptr_data_base, consider
 * using native `union` and/or `bit_cast` (in lieu of `reinterpret_cast*`), to aid in the bit-field-type-punning
 * approach.  (1) So a `union` would contain both the integer and the 2 bit-field interpretation `struct`s
 * `Offset_ptr_rep`, `Raw_ptr_rep` (with the latter 2 still having an identical MSB member, the selector).
 * Then no `reinterpret_cast` would be necessary.  Most likely this would be a stylistic change only --
 * effectively it should result in similar or identical machine code generated.  The pros would be a (very subjectively)
 * slightly clearer impl and, perhaps, the avoidance of certain type-punning warnings (for which as of this writing,
 * for gcc, we currently use a `#pragma`) -- though the exact same type-punning logic would really be in-place.
 * The con is that this adds yet another instance of using what is, technically (according to the standard),
 * undefined behavior; as officially only the last-assigned `union` member has a defined value, a rule we absolutely
 * do and must break.  So this could go either way.  (2) One could also, or additionally, use `std::bit_cast`;
 * however this would not be functionally equivalent, as this operation copies underlying bytes and cannot
 * modify values in-place; it could affect performance.  Also `bit_cast` is C++20; as of this writing we are on
 * C++17.
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
 * @tparam CAN_STORE_RAW_PTR
 *         Whether a `*this` is allowed to represent a vaddr that is neither null nor belonging to any SHM-pool
 *         registered in the global Repository_type at the time of construction of a `*this` from a `void*`.
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
  static constexpr bool S_RAW_OK = CAN_STORE_RAW_PTR;

  // Constructors/destructor.

  /// Construct with `nullptr`.
  Shm_pool_offset_ptr_data();

  /**
   * Construct from vaddr.  If #S_RAW_OK is `false`, and `p` is neither `nullptr` nor belongs to #Repository, then
   * acts as if `p` is `nullptr` after all.
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
   * @param src
   *        Source object.
   */
  Shm_pool_offset_ptr_data(const Shm_pool_offset_ptr_data<Repository_type, !S_RAW_OK>& src);

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
   * If `S_RAW_OK == false`:
   *   - Recall that internally a *handle* is stored: pool ID, offset into that pool.
   *     - If this pool does not exist, we return `nullptr`.  Implications:
   *       - If one dereferences it, behavior is obviously undefined.  This is desirable and likely similar to
   *         what would occur with dereferencing a corrupt or (obviously) null regular pointer.
   *       - If one compares to it such as the `.end()` scenario below, they're likely to get some behavior
   *         they don't expect -- but do they expect, if the system/they allowed the pool to get unmapped
   *         while running algorithms on related data?  Not our problem.
   *     - If this pool does exist, but the offset is out of bounds, we will return the out-of-bounds
   *       address based on the simple `base + offset` formula.  If offset is negative, we'll return pre-pool
   *       vaddr; if offset is positive and equals or exceeds pool size, we'll return the post-pool vaddr.
   *       Reason: See #pool_offset_t doc header.
   *
   * If `S_RAW_OK == true`:
   *   - If `is_raw()`: See above `!S_RAW_OK` case.  Same deal here.
   *   - Else: We return the stored address (in canonical form).
   *
   * In short: get() will *not* yield `nullptr` for various out-of-bounds situations.  `bool()` conversion
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
  explicit operator bool() const;

  /**
   * Increments `*this` by a number of bytes (which can be positive, negative, or zero).  As explained in
   * get() and #pool_offset_t doc headers this is maximally permissive, including when essentially nonsensical
   * get() return value might result.  Summary of edge cases including the aforementioned ones but limited to them:
   *   - If `!*this` legitimately (we are null due to being so assigned, not due to the pool referred-to within
   *     becoming invalid), this will:
   *     - (if #S_RAW_OK is `true`) act similarly to native `+=` (become numerically equal to the bits in `bytes`);
   *     - (else) no-op.  So don't do that.  Really, though, it would be ill-advised to do it with a raw pointer too.
   *   - If `*this` is a legit offset pointer, meaning it refers to an existing pool, and incrementing the stored
   *     offset by `bytes` (which might make it smaller) places `*this` before or past the pool boundary: We do so.
   *     Naturally dereferencing get() would yield undefined behavior; but for example `increment(-bytes)` would
   *     get `*this` back to its original state which might be just fine.
   *   - If arithmetic overflow occurs:
   *     - This function will not invoke undefined behavior (crash or similar).
   *     - get() will not either.
   *     - However no guarantees() are made as to the value get() would return numerically.  Informally speaking an
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

  // Friends.

  /// Friend of this class.
  template<typename Repository_type2, bool CAN_STORE_RAW_PTR2>
  friend std::ostream& operator<<(std::ostream& os,
                                  Shm_pool_offset_ptr_data<Repository_type2, CAN_STORE_RAW_PTR2> val);

  /**
   * The raw bits.  See our class doc header and #Base internals.  The preferred method of interpreting it is:
   *   -# Check against simply being equal to 0 (all bits).  If so => not-a-pointer (null).
   *   -# Cast `&m_rep` to `Base::Offset_ptr_rep*`; check Offset_ptr_rep::m_selector_offset_else_raw; if
   *      1 then: Offset_ptr_rep::m_pool_id specifies the pool in global #Repository singleton;
   *      Offset_ptr_rep::m_pool_offset is the offset off its base vaddr.  Else:
   *   -# All but the MSB of #m_rep = the same bits in the canonical result get() must return (the raw
   *      `void*`).  To compute the full canonical pointer (namely in get()):
   *      - The 2nd MSB (which happens to equal the following extended-sign bits) is in
   *        `(Raw_ptr_rep*)(&m_rep)->m_ext_sign_msb`.
   *      - get() must copy #m_rep but before turning this copy ensure its MSB equals `m_ext_sign_msb`.
   *        In other words if and only if Raw_ptr_rep::m_ext_sign_msb is 1, the MSB in the copy shall be set to 1.
   *
   * If `!S_RAW_OK`, then (corruption/undefined behavior aside) in step 2 `m_selector_offset_else_raw == 1` always.
   */
  volatile rep_t m_rep;
}; // class Shm_pool_offset_ptr_data

// Free functions.

/**
 * Prints string representation of the given `Shm_pool_offset_ptr_data` to the given `ostream`.
 *
 * @relatesalso Shm_pool_offset_ptr_data
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Repository_type, bool CAN_STORE_RAW_PTR>
std::ostream& operator<<(std::ostream& os,
                         Shm_pool_offset_ptr_data<Repository_type, CAN_STORE_RAW_PTR> val);


// Template implementations.

template<typename Repository_type, bool CAN_STORE_RAW_PTR>
Shm_pool_offset_ptr_data<Repository_type, CAN_STORE_RAW_PTR>::Shm_pool_offset_ptr_data() :
  m_rep(0)
{
  // Not-a-pointer: All bits are zero.  (See class doc header for explanation.)
}

template<typename Repository_type, bool CAN_STORE_RAW_PTR>
Shm_pool_offset_ptr_data<Repository_type, CAN_STORE_RAW_PTR>::Shm_pool_offset_ptr_data(const void* p)
{
  // m_rep is uninitialized garbage.

  if (!p)
  {
    m_rep = 0;
    return;
  }
  // else

  pool_id_t pool_id_or_0;
  pool_offset_t pool_offset;
  Repository::from_address(p, pool_id_or_0, pool_offset); // @todo Would compiler let us target m_pool_* directly?

  if (pool_id_or_0 == 0)
  {
    if constexpr(S_RAW_OK)
    {
      /* This is slightly subtle; if one revisits the class doc header, one sees that, in order for
       * raw-pointer get() to at most merely need to flip one bit and otherwise return the result as-is, we
       * store as many bits of the canonical form as possible (all but the MSB); and only set the MSB to the
       * required selector value (0). */
      m_rep = reinterpret_cast<rep_t>(p);

#if 0 // Can be interesting to temporarily enable to ensure our assumptions about canonical form seem to hold.
      // Perf: Enable only when perf does not matter.
      {
#  ifndef NDEBUG
        const auto sign_bits = m_rep >> (Raw_ptr_rep::S_N_VAL_BITS - 1); // Shift by 47 bits = 0... then 17 ex-MSB.
#  endif
        assert(((sign_bits == rep_t(0)) // First 17 bits were 0s.  Or:
                ||
                // First 17 bits were 1s.  (1111...1 + 1 = 10000...0 = 1 then 17 0s, which is 1 << 17.)
                ((sign_bits + rep_t(1)) == (rep_t(1) << ((sizeof(rep_t) * 8) - Raw_ptr_rep::S_N_VAL_BITS + 1))))
               && "A non-canonical raw pointer encountered.");
      }
#endif

      // Set MSB to 0.  Operate on bit-field for clarity and probably speed (see class doc header for discussion).
      const auto raw_ptr_rep = reinterpret_cast<volatile Raw_ptr_rep*>(&m_rep);
      raw_ptr_rep->m_selector_offset_else_raw = rep_t(0);
    } // if constexpr(S_RAW_OK)
    else // if constexpr(!S_RAW_OK)
    {
      // Just leave it as null and pray for happiness (as promised).
      m_rep = 0;
    }
  } // if (!pool_id_or_0 == 0)
  else // if (pool_id_or_0 != 0): Found in a SHM pool.
  {
    // Operate on bit-field for clarity and probably speed (see class doc header for discussion).
    const auto offset_ptr_rep = reinterpret_cast<volatile Offset_ptr_rep*>(&m_rep);

    offset_ptr_rep->m_selector_offset_else_raw = rep_t(1);
    offset_ptr_rep->m_pool_id = pool_id_or_0;
    offset_ptr_rep->m_pool_offset = pool_offset;
  }
} // Shm_pool_offset_ptr_data::Shm_pool_offset_ptr_data(const void* p)

template<typename Repository_type, bool CAN_STORE_RAW_PTR>
Shm_pool_offset_ptr_data<Repository_type, CAN_STORE_RAW_PTR>::Shm_pool_offset_ptr_data
  (const Shm_pool_offset_ptr_data&) = default;

template<typename Repository_type, bool CAN_STORE_RAW_PTR>
Shm_pool_offset_ptr_data<Repository_type, CAN_STORE_RAW_PTR>::Shm_pool_offset_ptr_data
  (const Shm_pool_offset_ptr_data<Repository_type, !S_RAW_OK>& src) :

  Shm_pool_offset_ptr_data(src.get()) // @todo Can we do something more performant?
{
#if 0 // Avoid the perf hit even from an assert().  Could enable when debugging perhaps.
  /* We shouldn't be converting from a raw-allowing, raw pointer to an offset-only pointer, as a
   * successful conversion wouldn't be expected and there isn't a use case to do this. -echan */
  if constexpr(!S_RAW_OK)
  {
    assert(((!src) || (!src.is_raw())) && "Conversion from raw pointer to offset-only pointer type.");
  }
#endif
}

template<typename Repository_type, bool CAN_STORE_RAW_PTR>
Shm_pool_offset_ptr_data<Repository_type, CAN_STORE_RAW_PTR>&
  Shm_pool_offset_ptr_data<Repository_type, CAN_STORE_RAW_PTR>::operator=(const Shm_pool_offset_ptr_data&)
    = default;

template<typename Repository_type, bool CAN_STORE_RAW_PTR>
void* Shm_pool_offset_ptr_data<Repository_type, CAN_STORE_RAW_PTR>::get() const
{
  // See the void* ctor and/or the class doc header; it should explain the following pretty well.  Keeping cmnts light.

  // We suspect this is common enough to where doing this before any bit/bit-sequence access = overall optimization.
  if (m_rep == rep_t(0))
  {
    return nullptr;
  }
  // else

  // Operate on bit-field for clarity and probably speed (see class doc header for discussion).
  const auto offset_ptr_rep = reinterpret_cast<const volatile Offset_ptr_rep*>(&m_rep);

  if constexpr(S_RAW_OK)
  {
    if (offset_ptr_rep->m_selector_offset_else_raw == rep_t(0))
    {
      auto result_rep = m_rep;
      const auto raw_ptr_result_rep = reinterpret_cast<volatile Raw_ptr_rep*>(&result_rep);
      if (raw_ptr_result_rep->m_ext_sign_msb != rep_t(0))
      {
        raw_ptr_result_rep->m_selector_offset_else_raw = rep_t(1); // Abusing this a bit... but it's not a crime.
      }
      return reinterpret_cast<void*>(result_rep);
    } // if (!offset_ptr_rep->m_selector_offset_else_raw)
    // else if (offset_ptr_rep->m_selector_offset_else_raw): Fall through.
  } // if constexpr(S_RAW_OK)
  else // if constexpr(!S_RAW_OK)
  {
#if 0 // Avoid the perf hit even from an assert().  Could enable when debugging perhaps.
    assert((offset_ptr_rep->m_selector_offset_else_raw == rep_t(1))
           && "How'd we reach such a state in 1st place?  Constructor shouldn't; increment() should not either.");
#endif
  }

  return Repository::to_address(offset_ptr_rep->m_pool_id, offset_ptr_rep->m_pool_offset);
} // Shm_pool_offset_ptr_data::get()

template<typename Repository_type, bool CAN_STORE_RAW_PTR>
bool Shm_pool_offset_ptr_data<Repository_type, CAN_STORE_RAW_PTR>::is_raw() const
{
  if constexpr(S_RAW_OK)
  {
    // Suppose MSB = 0; if all other bits = 0 then null; if at least one is 1 then raw.  Otherwise neither.
    const auto offset_ptr_rep = reinterpret_cast<const volatile Offset_ptr_rep*>(&m_rep);
    return (offset_ptr_rep->m_selector_offset_else_raw == pool_offset_t(0))
             && (m_rep != rep_t(0));
  }
  else
  {
    // Cannot be raw.
    return false;
  }
}

template<typename Repository_type, bool CAN_STORE_RAW_PTR>
Shm_pool_offset_ptr_data<Repository_type, CAN_STORE_RAW_PTR>::operator bool() const
{
  return m_rep != rep_t(0);
}

template<typename Repository_type, bool CAN_STORE_RAW_PTR>
void Shm_pool_offset_ptr_data<Repository_type, CAN_STORE_RAW_PTR>::increment(diff_t bytes) noexcept
{
  static_assert(std::is_signed_v<diff_t>, "If diff_t is unsigned, we cannot really decrement pointers.");

  // Operate on bit-field for clarity and probably speed (see class doc header for discussion).
  const auto offset_ptr_rep = reinterpret_cast<volatile Offset_ptr_rep*>(&m_rep);

  if constexpr(S_RAW_OK)
  {
    if (offset_ptr_rep->m_selector_offset_else_raw == 0)
    {
      /* Subtlety: m_rep may be 0, meaning we are not holding a raw pointer per se (by our classification)
       * but rather represent nullptr.  However: this is allowed with native pointers (dereferencing the
       * result is another matter); for instance `uint8_t* x = 0; x += 4;` will make it hold address 0x4.
       * So we let it happen.
       *
       * Subtlety: One wonders if doing += or -= ops on native pointers keeps canonical-form rules in mind;
       * so if one overflow the lower 48 bits, perhaps x86-64 arch will avoid touching the nearby
       * extended-sign bits.  Experimentation, backed by docs, shows that is not the case: It simply does
       * the += or -= op on the underlying uint64_t.  Of course dereferencing the result is going to blow up
       * (processor exception => SEGV-type-thing), but that's beside the point.  So we do the same thing.
       *
       * Related subtlety: Our += is identical to native pointer +=; the only possible difference occurs in that
       * we might flip ->m_selector_offset_else_raw to 1, if they're doing an op on the outside edges of the
       * range.  This would turn us into an offset pointer; so a subsequent get() (which is not a deref yet!)
       * would behave unpredictably; it could return nullptr, or it could accidentally encode a real pool ID
       * and thus yield some real-ish address instead of the right thing (which, granted, itself is something
       * unreal).  A native "get()" in that state would return the "right thing."  So we have to choices
       * for what do after the `m_rep +=`.  1, we could force-clear the MSB (shift left, shift right).
       * This would yield, possibly, the "right thing" in subsequent get() (which flips the MSB if the
       * 2nd MSB is 1, which would probably be 1 in this scenario -- though not necessarily depending on
       * magnitude of `bytes`).  Else, 2, we could do nothing.  This would hit the aforementioned
       * non-deterministic behavior, wherein get() would take us to be an offset pointer.  One can make
       * a case for either.  I (ygoldfel) ultimately decided to do (2).  The motivation: in the by-far-mainstream
       * case, where no one is doing anything funky, it has higher performance.  The defense:
       * Performing overflowing pointer arithmetic is not disallowed (meaning it shouldn't lead to undefined
       * behavior) in and of itself; and reading a resulting pointer value afterwards shouldn't lead  to
       * any exception or crash; but counting on any particular numeric value -- honestly I haven't tried to
       * absolutely confirm this formally in terms of the standard but just intuitively let's be real here --
       * is not in the cards.  Our impl doesn't crash anything here, or in subsequent get(), but any code
       * counting on some kind of particular resulting address after overflowing a pointer... shouldn't.  Nor
       * is it useful in any conceivable to me way.  So for performance's sake this is defensible. */
      m_rep += bytes;
      return;
    } // if (raw_ptr_rep->m_selector_offset_else_raw == 0)
    // else: Fall through:
  } // if constexpr(S_RAW_OK)
  else
  {
    if (m_rep == rep_t(0)) // Recall that !S_RAW_OK.
    {
      /* As advertised remain null.  We have no choice; we cannot express this any other way.  We could assert(),
       * but again we promised to behave -- ill-advised or not we stick to the promise. */
      return;
    }
    // else

#if 0 // Avoid the perf hit even from an assert().  Could enable when debugging perhaps.
    assert((offset_ptr_rep->m_selector_offset_else_raw == rep_t(1))
           && "How'd we reach such a state in 1st place?  Constructor shouldn't; increment() should not either.");
#endif
  }

  offset_ptr_rep->m_pool_offset += bytes; // Remember this may be negative (or 0).
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
  using Types = Shm_pool_offset_ptr_data<Repository_type, CAN_STORE_RAW_PTR>;
  using rep_t = typename Types::rep_t;
  using Offset_ptr_rep = typename Types::Offset_ptr_rep;
  using Raw_ptr_rep = typename Types::Raw_ptr_rep;
  using boost::io::ios_all_saver;

  if (val.m_rep == rep_t(0))
  {
    return os << "null";
  }
  // else
  const auto offset_ptr_rep = reinterpret_cast<volatile Offset_ptr_rep*>(&val.m_rep);

  if constexpr(CAN_STORE_RAW_PTR)
  {
    if (!offset_ptr_rep->m_selector_offset_else_raw)
    {
      ios_all_saver saver(os); // Revert std::hex/etc. soon.
      const auto raw_ptr_rep = reinterpret_cast<const volatile Raw_ptr_rep*>(&val.m_rep);
      return os << "ext_sign_bit[" << raw_ptr_rep->m_ext_sign_msb << "]... val_bits[0x"
                << std::hex << raw_ptr_rep->m_val_bits << "]@" << val.get();
      // @todo Slight perf impact: val.get() has some redundancy to above selector bit check.
    }
    // else: Fall through:
  }
  return os << "pool_id[" << offset_ptr_rep->m_pool_id << "]+[" << offset_ptr_rep->m_pool_offset << "]@" << val.get();
} // operator<<(ostream, Shm_pool_offset_ptr_data)

#if IPC_SHM_ARENA_LEND_DETAIL_GCC_COMPILER
#  pragma GCC diagnostic pop // See above.
#endif

#undef IPC_SHM_ARENA_LEND_DETAIL_GCC_COMPILER

} // namespace ipc::shm::arena_lend::detail
