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

#include "ipc/session/standalone/shm/arena_lend/jemalloc/shm_session.hpp"

namespace ipc::session::shm::arena_lend::jemalloc::test
{

/**
 * Wrapper around Shm_session to provide access to protected methods.
 */
class Test_shm_session :
  public Shm_session
{
public:
  /**
   * Creates an instance of this class. The motivation for utilizing a shared pointer is that borrowed object
   * handles refer back to the instance that generated the handle.
   *
   * Mirrors Shm_session::create(); see its doc header for the arg semantics.  In particular `shm_channel`
   * must be a fresh (never-started) channel; it is subsumed (owned and started) by the created instance.
   *
   * @param logger Used for logging purposes.
   * @param shm_channel The channel used for transmitting shared memory pool messages.  Subsumed; see above.
   * @param session_token The (non-nil) session token, equal on both sides; e.g., `Session::session_token()`.
   * @param shm_channel_error_handler Callback executed when a channel error code is emitted when using the
   *                                  shm_channel.
   *
   * @return An instance of this class.
   */
  static std::shared_ptr<Test_shm_session> create(
    flow::log::Logger* logger,
    Shm_channel&& shm_channel,
    const transport::struc::Session_token& session_token,
    flow::async::Task_asio_err&& shm_channel_error_handler)
  {
    return std::shared_ptr<Test_shm_session>(
      new Test_shm_session(logger,
                           std::move(shm_channel),
                           session_token,
                           std::move(shm_channel_error_handler)));
  }

  // Make public
  using Shm_session::get_remote_process_id;
  using Shm_session::set_disconnected;
  using Shm_session::Shm_object_handle;

protected:
  /**
   * Constructor. See create() for the arg semantics.
   *
   * @param logger See create().
   * @param shm_channel See create().
   * @param session_token See create().
   * @param shm_channel_error_handler See create().
   */
  Test_shm_session(flow::log::Logger* logger,
                   Shm_channel&& shm_channel,
                   const transport::struc::Session_token& session_token,
                   flow::async::Task_asio_err&& shm_channel_error_handler) :
    Shm_session(logger,
                std::move(shm_channel),
                session_token,
                std::move(shm_channel_error_handler))
  {
  }
}; // class Test_shm_session

} // ipc::session::shm::arena_lend::jemalloc::test
