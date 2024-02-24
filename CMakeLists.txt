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

cmake_minimum_required(VERSION 3.26.3) # See FlowLikeCodeGenerate.cmake for details.
# See that guy; it'll explain inside.  It mandates the following procedure and documents details.

set(PROJ "ipc_shm_arena_lend")

message(CHECK_START "(Project [${PROJ}] root CMake script executing.)")
list(APPEND CMAKE_MESSAGE_INDENT "- ")

set(PROJ_CAMEL "IpcShmArenaLend")
# TODO: Get it from nearest `VERSION` file perhaps.
set(PROJ_VERSION "1.0.0")
set(PROJ_HUMAN "Flow-IPC (SHM-jemalloc)")

set(OS_SUPPORT_MSG "Only Linux is supported for now.  For ipc_shm_arena_lend: There is much low-level code; "
                     "much testing and possibly significant coding is needed for macOS/BSD support; "
                     "probably more so for Windows.")
# Subtext: This thing is doing hairy jemalloc things at least.  Who knows what's needed?  That's not to say it's
# incredibly hard either.
# (That's after ensuring ipc_shm is supported in the OS in question first.)

if(FLOW_LIKE_META_ROOT)
  message(VERBOSE
            "FLOW_LIKE_META_ROOT set indicating we are a subdir of a meta-project.  Dependencies should be pre-loaded.")
  set(FLOW_LIKE_TOOLS "${FLOW_LIKE_META_ROOT_flow}/tools/cmake")
else()
  message(VERBOSE
            "FLOW_LIKE_META_ROOT not set; this indicates we are being distributed separately as opposed to "
              "in the meta-project.  We will load dependencies via find_package() from an external install-dir.")

  find_package(Flow 1.0 CONFIG REQUIRED)
  # That should have found (somewhere)/lib/cmake/Flow/FlowConfig.cmake based on the "Flow" token we gave it
  # and saved its location into ${Flow_DIR} (standard CMake technique); and we need
  # (somewhere)/share/flow/cmake/FlowLikeCodeGenerate.cmake; hence:
  set(FLOW_LIKE_TOOLS "${Flow_DIR}/../../../share/flow/cmake")
endif()

include("${FLOW_LIKE_TOOLS}/FlowLikeCodeGenerate.cmake")

# Tip: Most likely you are next interested in ./src/CMakeLists.txt and then perhaps ./test/{basic|suite}/CMakeLists.txt.

list(POP_BACK CMAKE_MESSAGE_INDENT)
message(CHECK_PASS "(Done, success.)")
