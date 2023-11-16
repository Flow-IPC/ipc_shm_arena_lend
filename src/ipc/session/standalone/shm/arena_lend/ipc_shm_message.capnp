# Flow-IPC: SHM-jemalloc
# Copyright (c) 2023 Akamai Technologies, Inc.; and other contributors.
# Each commit is copyright by its respective author or author's employer.
#
# Licensed under the MIT License:
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

@0xefb985df434a6f6b;

using Cxx = import "/capnp/c++.capnp";

$Cxx.namespace("ipc::session::shm::arena_lend::schema");

using PoolId = UInt32; # Should match detail::Shm_pool_offset_ptr_data_base::pool_id_t.
using PoolSize = UInt32; # Should match detail::Shm_pool_offset_ptr_data_base::pool_offset_t.

# Operational communication messages between a lender and borrower in a shared memory system.
struct IpcShmMessage
{
  union
  {
    lendArena @0 :LendArena;
    lendPool @1 :LendPool;
    removePool @2 :RemovePool;
    returnObject @3 :ReturnObject;
    response @4 :Response;
  }

  # Sent by a lender to lend a shared memory pool collection (arena) to a borrower. This is performed in
  # a request-response manner due to potential ordering conflicts.
  struct LendArena
  {
    collectionId @0 :UInt32;
  }

  # Sent by a lender to lend a shared memory pool within a collection to a borrower. The collection should have
  # already been borrowed. This is performed in a request-response manner due to potential ordering conflicts.
  struct LendPool
  {
    collectionId @0 :UInt32;
    poolId @1 :PoolId;
    poolName @2 :Text;
    poolSize @3 :PoolSize;
  }

  # Sent by a lender to deregister a shared memory pool within a collection from a borrower. The collection and
  # pool should have already been borrowed.
  struct RemovePool
  {
    collectionId @0 :UInt32;
    poolId @1 :PoolId;
  }

  # Sent by a borrower to return an object that was previously borrowed.
  struct ReturnObject
  {
    collectionId @0 :UInt32;
    poolId @1 :PoolId;
    poolOffset @2 :PoolSize;
  }

  # Sent by a borrower to indicate whether a request was completed successfully. This includes lend arena and
  # lend pool.
  struct Response
  {
    success @0 :Bool;
  }
}
