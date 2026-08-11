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

#include "ipc/shm/arena_lend/owner_shm_pool_collection.hpp"

namespace ipc::shm::arena_lend::detail
{

/**
 * This is the `friend` facade of Owner_shm_pool_collection that exposes non-public APIs hidden from public
 * user by providing public access to them; this is used internally by various Flow-IPC components
 * (Thread_lcl_obj_db_admin, Shm_session, etc.) that hold a pointer to an Owner_shm_pool_collection subclass
 * and need access to a small privileged interface not intended for external users.
 *
 * @tparam Base_t
 *         The type of object whose specific non-public API to expose; an Owner_shm_pool_collection or subclass thereof.
 */
template<typename Base_t>
struct Owner_spc_impl
{
  // Types.

  /// Short-hand for wrapped class.
  using Base = Base_t;

  // Data.

  /// Direct-initializable wrapped object.  Access `public` API through this reference; non-public API via `*this`.
  Base& m_base;

  // Methods.

  /**
   * See Owner_shm_pool_collection::generate_shm_object_name().
   * @param shm_pool_id
   *        See Owner_shm_pool_collection counterpart.
   * @return See Owner_shm_pool_collection counterpart.
   */
  Shared_name generate_shm_object_name(Owner_shm_pool_collection::pool_id_t shm_pool_id) const;

  /**
   * See Owner_shm_pool_collection::get_pool_name_base().
   * @return See Owner_shm_pool_collection counterpart.
   */
  const Shared_name& get_pool_name_base() const;

  /// See eponymous method in #Base.
  void this_thread_ensure_tcache_exists() const;
}; // struct Owner_spc_impl

// Template implementations.

template<typename Base_t>
Shared_name Owner_spc_impl<Base_t>::generate_shm_object_name(Owner_shm_pool_collection::pool_id_t shm_pool_id) const
{
  return m_base.generate_shm_object_name(shm_pool_id);
}

template<typename Base_t>
const Shared_name& Owner_spc_impl<Base_t>::get_pool_name_base() const
{
  return m_base.get_pool_name_base();
}

template<typename Base_t>
void Owner_spc_impl<Base_t>::this_thread_ensure_tcache_exists() const
{
  m_base.this_thread_ensure_tcache_exists();
}

} // namespace ipc::shm::arena_lend::detail
