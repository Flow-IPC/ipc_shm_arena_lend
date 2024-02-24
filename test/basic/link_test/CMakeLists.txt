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

# This is very similar to ipc_shm's link_test.  Keeping comments light.

find_package(CapnProto CONFIG REQUIRED)

if(TARGET ipc_transport_structured)
  set(CAPNPC_IMPORT_DIRS ${FLOW_LIKE_META_ROOT_ipc_transport_structured}/src)
endif()
capnp_generate_cpp(capnp_generated_srcs capnp_generated_hdrs_ignored "schema.capnp")

function(handle_binary name_sh)
  set(name "ipc_shm_arena_lend_link_test_${name_sh}.exec") # Must match common.cpp constant values.
  add_executable(${name} common.cpp "main_${name_sh}.cpp" ${capnp_generated_srcs})

  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

  common_set_target_properties(${name})

  # Link good ol' libipc_shm_arena_lend.
  target_link_libraries(${name} PRIVATE ${PROJ})

  install(TARGETS ${name}
          RUNTIME DESTINATION bin)

  message(STATUS "Recommended: [cd ${CMAKE_INSTALL_PREFIX}/bin && "
                   "./${name}].")
  message(STATUS "Run srv program first in 1 terminal, then cli in another, as same user, from that dir.")
endfunction()

handle_binary(srv)
handle_binary(cli)
