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

#include "ipc/transport/struc/shm/serializer.hpp"

namespace ipc::transport::struc::shm::arena_lend::jemalloc
{

// Types.

/**
 * Convenience alias: Use this when constructing a struc::Channel with tag
 * `Channel_base::S_SERIALIZE_VIA_*_SHM` and `Session` = `shm::arena_lend::jemalloc::*_session`.
 *
 * See Channel_base::Serialize_via_session_shm, Channel_base::Serialize_via_app_shm doc headers
 * for when/how to use.
 *
 * Tip: `Sync_io_obj` member alias will get you the `sync_io`-pattern counterpart.
 *
 * @internal
 * Unable to put it in ..._fwd.hpp, because it relies on nested class inside incomplete type.
 */
template<typename Channel_obj, typename Message_body>
using Channel
  = struc::Channel<Channel_obj, Message_body, Builder::Config, Reader::Config>;

} // namespace ipc::transport::struc::shm::arena_lend::jemalloc
