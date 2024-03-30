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

/* Note: This header, like all detail/ headers, by convention shall not be `#include`d by user code
 * directly.  A rare exception, if it can be called that, is that nearby unit-test code *can* include it as of this
 * writing, if it wants to leverage IPC_SHM_ARENA_LEND_JEMALLOC_API() to force fun jemalloc behaviors for
 * test purposes. */

// Macros.

#ifndef IPC_SHM_ARENA_LEND_JEMALLOC_API_PREFIX
static_assert(false, "IPC_SHM_ARENA_LEND_JEMALLOC_API_PREFIX macro must be defined, if even to a blank value.  "
                       "Build script error?");
#endif

/**
 * Helper macro to enable IPC_SHM_ARENA_LEND_JEMALLOC_API() impl to work.  Users of this header may ignore.
 * @param ARG_1
 *        First thing.
 * @param ARG_2
 *        Second thing.
 * @return Replaced with the two concatenated.
 */
#define IPC_SHM_ARENA_LEND_JEMALLOC_API_PREFIX_CONCAT(ARG_1, ARG_2) \
  IPC_SHM_ARENA_LEND_JEMALLOC_API_PREFIX_CONCAT_IMPL(ARG_1, ARG_2)

/**
 * Helper macro to enable IPC_SHM_ARENA_LEND_JEMALLOC_API() impl to work.  Users of this header may ignore.
 * @param ARG_1
 *        First thing.
 * @param ARG_2
 *        Second thing.
 * @return Replaced with the two concatenated.
 */
#define IPC_SHM_ARENA_LEND_JEMALLOC_API_PREFIX_CONCAT_IMPL(ARG_1, ARG_2) \
  ARG_1 ## ARG_2

/**
 * To be used when invoking jemalloc public API functions, this evaluates to the given argument (which shall be
 * a function name) pre-pended with the value of macro `IPC_SHM_ARENA_LEND_JEMALLOC_API_PREFIX`.
 * For example `IPC_SHM_ARENA_LEND_JEMALLOC_API(mallocx)` might resolve to simply `mallocx` or `je_mallocx`
 * depending on the value of `IPC_SHM_ARENA_LEND_JEMALLOC_API_PREFIX`.
 *
 * ### Implementation notes ###
 * It can be head-breaking to understand why one needs those 2 levels of macros; in fact for me (ygoldfel) the
 * comprehension arrives and leaves at will; but I assure you using only 1 intermediate macro that simply `##`es
 * the args together is not enough (let alone none).
 *
 * @param ARG_api_name
 *        A jemalloc API sans prefix (e.g., `malloc`, `mallocx`).
 * @return It is replaced with the arg pre-pended by the prefix.
 *
 * @note As of this writing this is used only by .cpp files internal to Flow-IPC; and unit-test files near those;
 *       it is not used by headers.  Therefore as of this writing it is sufficient to set
 *       `IPC_SHM_ARENA_LEND_JEMALLOC_API_PREFIX` value when
 *       building the library and unit tests -- that is by Flow-IPC's internal build scripts -- via
 *       compiler command-line macro-defines.  If headers begin using jemalloc APIs -- in templates, `constexpr`s,
 *       and/or perhaps inlined functions -- then we would need to dynamically generate and export a header
 *       containing ``IPC_SHM_ARENA_LEND_JEMALLOC_API_PREFIX` definition (e.g., using a .hpp.in file turned into
 *       .hpp file by the build script).  We would then `#include` that dynamically-generated header
 *       above this macro's definition in its containing header file (detail/jemalloc.hpp as of this writing).
 */
#define IPC_SHM_ARENA_LEND_JEMALLOC_API(ARG_api_name) \
  IPC_SHM_ARENA_LEND_JEMALLOC_API_PREFIX_CONCAT(IPC_SHM_ARENA_LEND_JEMALLOC_API_PREFIX, ARG_api_name)
