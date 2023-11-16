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
   * @param logger Used for logging purposes.
   * @param borrower_repository The repository where borrower collection information is stored. The instance will
   *                            use this borrowed reference, which must be valid for the object's lifetime. This
   *                            is generally used to funnel information from multiple sessions into one, such that
   *                            it is shared instead of similar SHM pools potentially opened more than once. This
   *                            makes the utilization of offset pointers feasible in a multi-session to one process
   *                            environment.
   * @param shm_channel The channel used for transmitting shared memory pool messages. The instance will use
   *                    this borrowed reference, which must be valid for the object's lifetime.
   * @param shm_channel_error_handler Callback executed when a channel error code is emitted when using the
   *                                  shm_channel.
   * @param shm_channel_request_timeout The timeout when sending a synchronous request using the shm_channel.
   *
   * @return An instance of this class.
   */
  static std::shared_ptr<Test_shm_session> create(
    flow::log::Logger* logger,
    Borrower_shm_pool_collection_repository& borrower_repository,
    Shm_channel& shm_channel,
    flow::async::Task_asio_err&& shm_channel_error_handler,
    util::Fine_duration shm_channel_request_timeout = util::Fine_duration::max())
  {
    return std::shared_ptr<Test_shm_session>(
      new Test_shm_session(logger,
                           borrower_repository,
                           shm_channel,
                           std::move(shm_channel_error_handler),
                           shm_channel_request_timeout));
  }

  // Make public
  using Shm_session::deserialize_handle;
  using Shm_session::get_remote_process_id;
  using Shm_session::lend_shm_pool;
  using Shm_session::receive_arena;
  using Shm_session::receive_shm_pool;
  using Shm_session::receive_shm_pool_removal;
  using Shm_session::remove_lender_shm_pool;

protected:
  /**
   * Constructor.
   *
   * @param logger Used for logging purposes.
   * @param borrower_repository The repository where borrower collection information is stored. The instance will
   *                            use this borrowed reference, which must be valid for the object's lifetime. This
   *                            is generally used to funnel information from multiple sessions into one, such that
   *                            it is shared instead of similar SHM pools potentially opened more than once. This
   *                            makes the utilization of offset pointers feasible in a multi-session to one process
   *                            environment.
   * @param shm_channel The channel used for transmitting shared memory pool messages. The instance will use
   *                    this borrowed reference, which should be valid for the object's lifetime.
   * @param shm_channel_error_handler Callback executed when a channel error code is emitted when using the
   *                                  shm_channel.
   * @param shm_channel_request_timeout The timeout when sending a synchronous request using the shm_channel.
   */
  Test_shm_session(flow::log::Logger* logger,
                   Borrower_shm_pool_collection_repository& borrower_repository,
                   Shm_channel& shm_channel,
                   flow::async::Task_asio_err&& shm_channel_error_handler,
                   util::Fine_duration shm_channel_request_timeout) :
    Shm_session(logger,
                borrower_repository,
                shm_channel,
                std::move(shm_channel_error_handler),
                shm_channel_request_timeout)
  {
  }
}; // class Test_shm_session

} // ipc::session::shm::arena_lend::jemalloc::test
