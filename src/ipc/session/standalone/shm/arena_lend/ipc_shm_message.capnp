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

using PoolId = UInt32; # Should match detail::pool_id_t.
using PoolSize = UInt32; # Should match detail::pool_offset_t.

struct IpcShmMessage
{
  # Operational communication messages between a lender and borrower in an arena-lending shared memory system.

  union
  {
    lendArena @0 :LendArena;
    lendPool @1 :LendPool;
    response @3 :Response;

    removePool @2 :Void;
    # Was RemovePool; removed for now. See @todo in Shm_session re. restoring it.
  }

  struct LendArena
  {
    # Sent by a lender to lend a shared memory pool collection (arena) to a borrower.  This is performed in
    # a request-response manner (`Response` ack required before proceeding) due to potential ordering conflicts.

    collectionId @0 :UInt32; # Ordinal (1, 2, ...) collection (arena) ID, unique given an owner process (PID).
    poolNameBase @1 :Text;
    # Root of a SHM-pool's file-system name (add globally unique pool ID => get full name).
    # Borrower must be able to compute that full name in a few different contexts.  (For such a computation
    # it needs: pool ID; and owner ID (PID) + collection ID, to look up this poolNameBase where
    # borrower stores it.  As of this writing: Pool ID comes from LendPool below (for actual buffer-storing
    # pools) or on a per-constructed/lent-object basis via the SHM-handle serialization (for the
    # Lend_tracker_pool aux pools).
  }

  struct LendPool
  {
    # Sent by a lender to lend a shared memory pool within a collection to a borrower.  The collection (arena)
    # should have already been borrowed.  This is performed in a request-response manner (`Response` ack required
    # before proceeding) due to potential ordering conflicts.  Specifically we do LendPool request/response like so:
    # (1) user calls SHM-allocate API, (2) memory manager (let's say jemalloc) is asked to in fact do that,
    # (3) memory manager determines a new pool (vaddr area; extent) is required, (4) it tells Flow-IPC internals
    # via hook that it needs this, so we create/map SHM-pool, do the request-response, and give control back
    # to memory manager (which completes the malloc()-y op then returns through Flow-IPC back into end user-land).

    collectionId @0 :UInt32;
    poolId @1 :PoolId; # Globally unique (until reboot).
    poolSize @2 :PoolSize;
    # Among other things, we at least need this to know how much vaddr space to memory-map (mmap() in Linux et al)
    # upon opening SHM-pool via computed pool name.  See comment on poolNameBase above about that computation.
  }

  # Ack for LendArena, LendPool.
  struct Response
  {
    success @0 :Bool;
  }
}
