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

namespace ipc::shm::arena_lend::detail
{

/**
 * Pretends to be a void object such that references can be used. In particular, it is useful for templating
 * with void types.
 */
struct Void_holder
{
  /// Intentionally empty
}; // struct Void_holder

/**
 * A generic mechanism for getting the reference for a type. In particular, it supports the void type.
 *
 * @tparam T The type.
 */
template <typename T>
struct Add_reference
{
  /// The reference type.
  using m_type = T&;
}; // struct Add_reference

/**
 * Specialization for a reference type.
 *
 * @tparam T The type.
 */
template<typename T>
struct Add_reference<T&>
{
  /// The reference type.
  using m_type = T&;
}; // struct Add_reference<T&>

/**
 * Specialization for void type.
 */
template<>
struct Add_reference<void>
{
  using m_type = Void_holder&;
}; // struct Add_reference<void>

/**
 * Specialization for const void type.
 */
template<>
struct Add_reference<const void>
{
  using m_type = const Void_holder&;
}; // struct Add_reference<const void>

} // namespace ipc::shm::arena_lend::detail
