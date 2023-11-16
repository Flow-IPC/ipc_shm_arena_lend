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

@0xd72938424129d2e2;

using Cxx = import "/capnp/c++.capnp";

$Cxx.namespace("ipc::session::shm::arena_lend::jemalloc::test");

# The message types for our unit tests.
struct TestMessage
{
  union
  {
    requestType @0 :RequestType;
    responseObject @1 :TestObjectMessage;
    responseType @2 :ResponseType;
  }

  # Types of requests sent from client to server. Note that these are asynchronous requests.
  enum RequestType
  {
    start @0;
    received @1;
    finish @2;
  }

  # Types of responses sent from server to client.
  enum ResponseType
  {
    cleanup @0;
  }
}

# A message containing a serialized object and a shared memory pool to validate.
struct TestObjectMessage
{
  # The message description.
  description @0 :Text;
  # The object type.
  type @1 :ObjectType;
  # The object in serialized form.
  serializedObject @2 :Data;
  # The shared memory pool id to validate that it has been borrowed.
  shmPoolIdToCheck @3 :UInt32;

  # The domain of object types.
  enum ObjectType
  {
    # An array of characters stored in shared memory.
    charArray @0;
    # A container of characters stored in shared memory.
    vectorChar @1;
    # A container of characters stored in shared memory.
    string @2;
    # A container of fixed sized structures stored in shared memory.
    list @3;
  }
}
