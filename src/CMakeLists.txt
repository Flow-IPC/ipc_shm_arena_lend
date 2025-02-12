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

# See FlowLikeLib.cmake per below; it'll explain inside.
# It mandates the following procedure and documents details.

# We just need executables to link ipc_shm (and whatever it says it must link); and jemalloc.
# Other stuff is transitively specified by the latter, even though we do use some of that
# stuff directly too.  Oh, also, in Linux jemalloc will need -ldl (a system lib).

message(STATUS "Dependents shall need to import/link libs: ipc_shm, jemalloc, (Linux only) dl.")

set(DEP_IPC_SHM_VERSION 1.0)
if(TARGET ipc_shm)
  set(DEP_LIBS ipc_shm) # In meta-project mode it's loaded by this short name.

  # In meta-project mode we lack the benefit of find_package(Ipc*) (see the other if() path); which means
  # the transitive dependency -- capnp -- has not been communicated to us.  So we must do it ourselves to
  # be able to do capnp stuff ourselves.
  find_package(CapnProto ${CAPNP_VER} CONFIG REQUIRED)
else()
  find_package(IpcShm ${DEP_IPC_SHM_VERSION} CONFIG REQUIRED)
  # In separate-project mode it's been install(EXPORT)ed with the namespace prefix.
  set(DEP_LIBS IpcShm::ipc_shm)
endif()

# Mirror above find_package() calls.  As directed omit "REQUIRED."
set(DEP_LIBS_PKG_ARG_LISTS "IpcShm ${DEP_IPC_SHM_VERSION} CONFIG")

# However the jemalloc dependency requires some extra care.  Note we follow the approach recommended
# in FlowLikeLib.cmake doc comment.  jemalloc does not export a thing so we can use find_package(), so
# we have to follow the alternate technique of manually determining the lib-absolute-path (and shove it
# into DEP_LIBS) and include-path (and load it via target_include_directories() call).  Let's:
#
# What we want in the end: JEMALLOC_LIBRARIES (name borrowed from a thing below; you'll see) (static lib location)
# and JEMALLOC_INCLUDEDIR (ditto) (the include-path).  We'll use the following order (which comports with
# FlowLikeLib.cmake doc-comment recommendations).
#   - If both are user-set already, use those values.  (Corner case: If only one is set but not the other, then that's
#     confusing and an error.)  Else:
#   - jemalloc build *does* produce a jemalloc.pc (pkg-config), so we can use pkg_check_modules().

# First eliminate corner case.
if((JEMALLOC_LIBRARIES AND (NOT JEMALLOC_INCLUDEDIR)) OR
   ((NOT JEMALLOC_LIBRARIES) AND JEMALLOC_INCLUDEDIR))
  message(FATAL_ERROR "Must set neither (recommended) or both of: JEMALLOC_LIBRARIES and JEMALLOC_INCLUDEDIR.")
endif()
# Can now just check JEMALLOC_LIBRARIES alone for settedness.

if(NOT JEMALLOC_LIBRARIES)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(JEMALLOC jemalloc) # Add REQUIRED if we get rid of the fall-back (see below if not JEMALLOC_FOUND).
  if(JEMALLOC_FOUND)
    if((NOT JEMALLOC_LIBRARIES) OR (NOT JEMALLOC_INCLUDEDIR))
      message(FATAL_ERROR
                "CMake pkg-config searcher claims JEMALLOC_FOUND but has not set "
                "JEMALLOC_LIBRARIES [${JEMALLOC_LIBRARIES}] and/or JEMALLOC_INCLUDEDIR [${JEMALLOC_INCLUDEDIR}].  "
                "Do not fret: Please build a jemalloc if not yet done and try again; "
                "if it is still not found then please supply the full path to the library file (including "
                "the file itself) as the CMake knob (cache setting) JEMALLOC_LIBRARIES; and the full path "
                "to the include root (the directory containing, but not equal to, jemalloc/) as the CMake knob "
                "(cache setting) JEMALLOC_INCLUDEDIR.")
    endif()
    message(VERBOSE "CMake pkg-config searcher located jemalloc lib + include-dir (see below); "
                      "though you can override this via cache settings.")
  else()
    # Commenting this out for now:
    #   message(FATAL_ERROR
    #             "CMake pkg-config searcher could not find jemalloc: JEMALLOC_FOUND not set.  "
    #             "Do not fret: Please build a jemalloc if not yet done and try again; "
    #             "if it is still not found then please supply the full path to the library file (including "
    #             "the file itself) as the CMake knob (cache setting) JEMALLOC_LIBRARIES; and the full path "
    #             "to the include root (the directory containing, but not equal to, jemalloc/) as the CMake knob "
    #             "(cache setting) JEMALLOC_INCLUDEDIR.")

    # Here we should be just failing: either via message(FATAL_ERROR) (commented-out) or just by using `REQUIRED`
    # in the pkg_check_modules() call above.  Instead we do what FlowLikeLib.cmake doc header specifically recommends
    # against: we use an extra fallback (in the form of find_library() and find_path()).  Why the exception?  Answer:
    # We know of a case where much of jemalloc is installed (the lib, headers) but ancillary items are not,
    # namely (as relevant here) the .pc file and (as relevant when determining FLOW_IPC_JEMALLOC_PREFIX below)
    # the jemalloc-config binary.  That case is when using the current (as of 2/2025) jemalloc Conan recipe;
    # its package() routine seems to clearly just install the lib and headers but not those other items.  Possibly
    # we lack the Conan knowledge to wrangle it properly; or more likely in this case perhaps the recipe should be
    # improved; but either way: in the official GitHub pipeline (elsewhere .github/.../main.yml) we use Conan
    # to install jemalloc -- and then pkg_check_modules() fails above.  (find_program() for jemalloc-config also would
    # fail; which would cause that code below to fail; which is while a conanfile.py as of this writing specifies
    # FLOW_IPC_JEMALLOC_PREFIX manually to work-around it.)
    #
    # Perhaps the "right" approach would be to improve the Conan recipe; the problem is there, so why are we
    # compensating for it here, when Conan is only one scenario, and pkg_check_modules() works great in others?
    # So, arguably, leaving this here is a kludge due to lacking time and arguably know-how to wrangle Conan
    # into submission.  Also there is some Conan-jemalloc-related work in progress elsewhere that might make such
    # fixes moot and add to the confusion.  So, all that taken into account, we felt it would be harmless (if a bit
    # ugly) to support a gimped jemalloc install by falling back to find_library() and find_path() after all.
    #
    # TODO: Get rid of this, once the jemalloc-Conan situation is stabilized and perhaps fixed.

    message(WARNING
              "CMake pkg-config searcher could not find jemalloc.  This is known to occur in some jemalloc installs "
              "including as of this writing when installed via official jemalloc Conan recipe: jemalloc.pc is not "
              "installed (nor is jemalloc-config executable which may be relevant later in the absence of "
              "cache-setting FLOW_IPC_JEMALLOC_PREFIX).  Falling back to CMake uber-searchers find_library() and "
              "find_path().")

    block()
      set(lib_name "jemalloc")
      # Search for static lib specifically.  TODO: Is this needed really?  Being paranoid here?
      if(UNIX)
        set(lib_name "lib${lib_name}.a")
      else()
        message(FATAL_ERROR "Unsupported OS; please modify script to search for static jemalloc lib Windows-style.")
      endif()
      find_library(JEMALLOC_LIBRARIES NAMES ${lib_name})
      if(NOT JEMALLOC_LIBRARIES)
        message(FATAL_ERROR
                  "Could not find jemalloc library ([${lib_name}]) location via CMake uber-searcher "
                  "find_library().  Do not fret: Please build a jemalloc if not yet done and try again; "
                  "if it is still not found then please supply the full path to the library file (including "
                  "the file itself) as the CMake knob (cache setting) JEMALLOC_LIBRARIES; and the full path "
                  "to the include root (the directory containing, but not equal to, jemalloc/) as the CMake knob "
                  "(cache setting) JEMALLOC_INCLUDEDIR.")
      else()
        message(VERBOSE "CMake uber-searcher find_library() located jemalloc lib (see below); "
                          "though you can override this via cache setting.")

        # Without these 2 lines find_path() fails; with both it succeeds.  I (ygoldfel) don't know
        # *exactly* why it fails, but printing JEMALLOC_INCLUDEDIR at this stage showed an empty value.  So somehow
        # find_path() thinks JEMALLOC_INCLUDEDIR has already been computed and to not look "again," was my hunch.
        # So I added these 2 lines (1 of them might be enough but just to be safe...) -- because debugging this
        # subtlety didn't seem worthwhile -- and that solved it; so that's that.  TODO: Revisit.
        unset(JEMALLOC_INCLUDEDIR)
        unset(JEMALLOC_INCLUDEDIR CACHE)

        find_path(JEMALLOC_INCLUDEDIR NAMES "jemalloc/jemalloc.h") # That's a nice representative one.
        if(NOT JEMALLOC_INCLUDEDIR)
          message(FATAL_ERROR
                    "Could not find jemalloc #include root location via CMake uber-searcher "
                    "find_path().  Do not fret: Please build a jemalloc if not yet done and try again; "
                    "if it is still not found then please supply the full path to the library file (including "
                    "the file itself) as the CMake knob (cache setting) JEMALLOC_LIBRARIES; and the full path "
                    "to the include root (the directory containing, but not equal to, jemalloc/) as the CMake knob "
                    "(cache setting) JEMALLOC_INCLUDEDIR.")
        endif()
        message(VERBOSE "CMake uber-searcher find_path() located jemalloc lib (see below); "
                          "though you can override this via cache setting.")
      endif()
    endblock()
  endif()
endif() # Else if (JEMALLOC_LIBRARIES): Cool, both that and JEMALLOC_INCLUDEDIR are set by user.

message(STATUS "jemalloc lib location: [${JEMALLOC_LIBRARIES}].")
message(STATUS "jemalloc header location: [${JEMALLOC_INCLUDEDIR}].")
# (^-- Don't forget to target_include_directories() it later.)  Can/must add this now per FlowLikeLib.cmake doc cmnt.
list(APPEND DEP_LIBS ${JEMALLOC_LIBRARIES})

if(LINUX)
  list(APPEND DEP_LIBS dl)
endif()

# There is one more thing however.  See the source file ipc/shm/arena_lend/jemalloc/detail/jemalloc.hpp
# which specifies that IPC_SHM_ARENA_LEND_JEMALLOC_API_PREFIX is a macro that must be defined (to blank or otherwise)
# for any file `#include`ing that header (but we'll just assume all our `.cpp`s).  This value, it explains,
# must be the prefix (potentially none) that's part of every jemalloc public API function's name which is
# determine at jemalloc build time.  What to do depends on what the user, who's supplying the dependencies which
# includes jemalloc, decided.  (Blank would have the key effect of also replacing malloc()/free(); while non-blank
# means the user is relying on the arguably more-advanced mode in which jemalloc APIs are specifically used as
# opposed to merely using is at a malloc() replacer.  However we don't care why; just what.)
#
# We could require the user to supply the je-prefix explicitly via knob (cache setting); but ideally it can be
# found automatically.  Indeed: jemalloc builds/exports typically a binary called jemalloc-config which can output
# this (and other compile-time) info.  But where is that program?  In *nix it is seemingly usually in ../bin
# off the lib location (which is the dir part of JEMALLOC_LIBRARIES supposedly); but that doesn't sound reliable
# (what if the lib is not directly in /lib but like some platform-based subdir or something? etc.).
# Anyway!  That's what find_program() is for.  If find_program() doesn't work, they can give the location by
# setting knob manually.  Lastly jemalloc-config can be bypassed by simply supplying the je-prefix via yet
# another knob.

# Utility: Find jemalloc-config; invoke it; parse result; set je-prefix.
function(jemalloc_config_run)
  message(CHECK_START "(Determining jemalloc API-name prefix if any.)")
  list(APPEND CMAKE_MESSAGE_INDENT "- ")

  if(NOT JEMALLOC_CONFIG_BIN)
    find_program(JEMALLOC_CONFIG_BIN NAMES "jemalloc-config") # In Windows it should find jemalloc-config.exe, etc.
    if(NOT JEMALLOC_CONFIG_BIN)
      message(FATAL_ERROR
                "Could not find jemalloc-config binary executable (want it to auto-determine je-prefix).  "
                "Do not fret: Please build a jemalloc if not yet done and try again; "
                "if it is still not found then please supply the full path as the CMake knob "
                "(cache setting) JEMALLOC_CONFIG_BIN; and if for whatever reason this program is just "
                "not available, then you can instead manually supply the je-prefix via CMake "
                "knob (cache setting) FLOW_IPC_JEMALLOC_PREFIX (note: special value \"none\" for a blank prefix).")
    endif()
    message(VERBOSE "CMake uber-searcher find_program() located jemalloc-config binary (see below); "
                      "though you can override this via cache setting.")
  endif()
  message(STATUS "jemalloc-config binary path: [${JEMALLOC_CONFIG_BIN}].")

  execute_process(COMMAND ${JEMALLOC_CONFIG_BIN} --config
                  OUTPUT_VARIABLE output
                  OUTPUT_STRIP_TRAILING_WHITESPACE
                  COMMAND_ERROR_IS_FATAL ANY)

  # Possibilities:
  #   - $output might contain (among other tokens) this token:
  #       --with-jemalloc-prefix=
  #     Means: empty prefix.  Not actually sure this form is possible, but defensively assume it is.
  #   - Else $output might contain (among other tokens) this token:
  #       --with-jemalloc-prefix=(1 or more non-space chars)
  #     Means: non-empty prefix, namely the thing after =.  Certainly this form is possible/essential.
  #   - Else $output might contain *no* token starting with
  #       --with-jemalloc-prefix=
  #     Means: again, empty prefix.  Certainly this form is possible (we have seen it, namely when jemalloc's
  #     `configure` was invoked with no --with-jemalloc-prefix= arg).
  string(REGEX MATCH "--with-jemalloc-prefix=[^ ]*" FLOW_IPC_JEMALLOC_PREFIX ${output})
  # Now FLOW_IPC_JEMALLOC_PREFIX is one of:
  #   - "--with-jemalloc-prefix="
  #     Means FLOW_IPC_JEMALLOC_PREFIX should be "".  Hence should erase "--with-jemalloc-prefix=" from start.
  #   - "--with-jemalloc-prefix=(1 or more non-space chars)""
  #     Means FLOW_IPC_JEMALLOC_PREFIX should be (the part after =).
  #     Hence should erase "--with-jemalloc-prefix=" from start.
  #   - ""
  #     Means (same as 1st situation).  Hence should do nothing.
  # Therefore erasing "--with-jemalloc-prefix=" from start, if it is there (else doing nothing) = a correct way
  # to get what we want.
  string(REGEX REPLACE "^--with-jemalloc-prefix=" "" FLOW_IPC_JEMALLOC_PREFIX "${FLOW_IPC_JEMALLOC_PREFIX}")

  # Lastly follow the convention for this cache setting.
  if("${FLOW_IPC_JEMALLOC_PREFIX}" STREQUAL "")
    set(FLOW_IPC_JEMALLOC_PREFIX "${NONE}")
  endif()

  # "Fun" quirk of CMake: a cache setting can't be straightforwardly modified from within a function.
  # Have to do this.
  set(FLOW_IPC_JEMALLOC_PREFIX ${FLOW_IPC_JEMALLOC_PREFIX} CACHE STRING ${FLOW_IPC_JEMALLOC_PREFIX_DOC} FORCE)

  message(VERBOSE "jemalloc je-prefix auto-determined via jemalloc-config execution (see below); "
                    "though you can override this via cache setting.")

  list(POP_BACK CMAKE_MESSAGE_INDENT)
  message(CHECK_PASS "(Done; found via jemalloc-config execution; prefix is [${FLOW_IPC_JEMALLOC_PREFIX}].)")
endfunction()

function(post_include_setup)
  message(CHECK_START "(Library [${PROJ}] + headers/etc.: code-gen/install targets: additional setup.)")
  list(APPEND CMAKE_MESSAGE_INDENT "- ")

  if(NOT JEMALLOC_INCLUDEDIR)
    message(FATAL_ERROR "Something went wrong -- we should have determined JEMALLOC_INCLUDEDIR earlier.  Bug?")
  endif()

  # Note: PUBLIC in case some of our headers #include jemalloc stuff (which as of this writing is the case).
  target_include_directories(${PROJ} PUBLIC ${JEMALLOC_INCLUDEDIR})

  set(NONE "none")
  set(FLOW_IPC_JEMALLOC_PREFIX_DOC
      "jemalloc API-name prefix -- special value \"${NONE}\" for empty such prefix (how your jemalloc was configured).")
  set(FLOW_IPC_JEMALLOC_PREFIX CACHE STRING ${FLOW_IPC_JEMALLOC_PREFIX_DOC})
  if(NOT FLOW_IPC_JEMALLOC_PREFIX)
    jemalloc_config_run() # It has to succeed; or script aborts.
  endif()
  message(STATUS "Shall compile with -DIPC_SHM_ARENA_LEND_JEMALLOC_API_PREFIX=[${FLOW_IPC_JEMALLOC_PREFIX}].")

  # PRIVATE, as we need only apply it to some .cpp files of ours; no exported headers require it as of this writing.
  # Note: that could change (function templates, constexpr functions, explicitly-inlined functions) in which
  # case (as detail/jemalloc.hpp also notes) we cannot use the compiler-define technique and will need to generate
  # and export a header file.  An #error will make that clear though; until then let's do the easy thing.
  block()
    set(pfx ${FLOW_IPC_JEMALLOC_PREFIX})
    if(pfx STREQUAL ${NONE}) # The special value is b/c an actual empty value is already taken for other purposes.
      set(pfx "")
    endif()
    target_compile_definitions(${PROJ} PUBLIC "IPC_SHM_ARENA_LEND_JEMALLOC_API_PREFIX=${pfx}")
  endblock()

  list(POP_BACK CMAKE_MESSAGE_INDENT)
  message(CHECK_PASS "(Done.)")
endfunction()

# (See above if wondering why we're not auto-searching for these.  There's a good reason.)
# Ideally keep this in `find -s` order.  Don't forget to exclude test/.
set(SRCS
    ipc/session/detail/shm/arena_lend/jemalloc/session_shared_name.cpp
    ipc/session/shm/arena_lend/jemalloc/error.cpp
    ipc/session/standalone/shm/arena_lend/borrower_collection.cpp
    ipc/session/standalone/shm/arena_lend/borrower_shm_pool_collection_repository.cpp
    ipc/session/standalone/shm/arena_lend/jemalloc/shm_session.cpp
    ipc/session/standalone/shm/arena_lend/lender_collection.cpp
    ipc/session/standalone/shm/arena_lend/shm_session_data.cpp
    ipc/shm/arena_lend/borrower_shm_pool_collection.cpp
    ipc/shm/arena_lend/borrower_shm_pool_repository.cpp
    ipc/shm/arena_lend/detail/shm_pool_offset_ptr_data.cpp
    ipc/shm/arena_lend/divisible_shm_pool.cpp
    ipc/shm/arena_lend/jemalloc/ipc_arena.cpp
    ipc/shm/arena_lend/jemalloc/jemalloc_pages.cpp
    ipc/shm/arena_lend/jemalloc/memory_manager.cpp
    ipc/shm/arena_lend/jemalloc/shm_pool_collection.cpp
    ipc/shm/arena_lend/memory_manager.cpp
    ipc/shm/arena_lend/owner_shm_pool_collection.cpp
    ipc/shm/arena_lend/owner_shm_pool_listener_for_repository.cpp
    ipc/shm/arena_lend/shm_pool.cpp
    ipc/shm/arena_lend/shm_pool_collection.cpp)
set(CAPNP_SCHEMAS
    ipc/session/standalone/shm/arena_lend/ipc_shm_message.capnp)
set(HDRS
    ipc/session/detail/shm/arena_lend/jemalloc/client_session_impl.hpp
    ipc/session/detail/shm/arena_lend/jemalloc/jemalloc_fwd.hpp
    ipc/session/detail/shm/arena_lend/jemalloc/server_session_impl.hpp
    ipc/session/detail/shm/arena_lend/jemalloc/session_impl.hpp
    ipc/session/shm/arena_lend/jemalloc/client_session.hpp
    ipc/session/shm/arena_lend/jemalloc/error.hpp
    ipc/session/shm/arena_lend/jemalloc/jemalloc.hpp
    ipc/session/shm/arena_lend/jemalloc/jemalloc_fwd.hpp
    ipc/session/shm/arena_lend/jemalloc/server_session.hpp
    ipc/session/shm/arena_lend/jemalloc/session.hpp
    ipc/session/shm/arena_lend/jemalloc/session_server.hpp
    ipc/session/standalone/shm/arena_lend/arena_lend_fwd.hpp
    ipc/session/standalone/shm/arena_lend/borrower_collection.hpp
    ipc/session/standalone/shm/arena_lend/borrower_shm_pool_collection_repository.hpp
    ipc/session/standalone/shm/arena_lend/jemalloc/shm_session.hpp
    ipc/session/standalone/shm/arena_lend/lender_collection.hpp
    ipc/session/standalone/shm/arena_lend/shm_session_data.hpp
    ipc/shm/arena_lend/arena_lend_fwd.hpp
    ipc/shm/arena_lend/borrower_allocator_arena.hpp
    ipc/shm/arena_lend/borrower_shm_pool_collection.hpp
    ipc/shm/arena_lend/borrower_shm_pool_listener.hpp
    ipc/shm/arena_lend/borrower_shm_pool_repository.hpp
    ipc/shm/arena_lend/detail/add_reference.hpp
    ipc/shm/arena_lend/detail/shm_pool_offset_ptr_data.hpp
    ipc/shm/arena_lend/detail/shm_pool_use_tracker.hpp
    ipc/shm/arena_lend/divisible_shm_pool.hpp
    ipc/shm/arena_lend/jemalloc/detail/jemalloc.hpp
    ipc/shm/arena_lend/jemalloc/ipc_arena.hpp
    ipc/shm/arena_lend/jemalloc/jemalloc.hpp
    ipc/shm/arena_lend/jemalloc/jemalloc_fwd.hpp
    ipc/shm/arena_lend/jemalloc/jemalloc_pages.hpp
    ipc/shm/arena_lend/jemalloc/memory_manager.hpp
    ipc/shm/arena_lend/jemalloc/shm_pool_collection.hpp
    ipc/shm/arena_lend/memory_manager.hpp
    ipc/shm/arena_lend/owner_shm_pool_collection.hpp
    ipc/shm/arena_lend/owner_shm_pool_listener.hpp
    ipc/shm/arena_lend/owner_shm_pool_listener_for_repository.hpp
    ipc/shm/arena_lend/shm_pool.hpp
    ipc/shm/arena_lend/shm_pool_collection.hpp
    ipc/shm/arena_lend/shm_pool_holder.hpp
    ipc/shm/arena_lend/shm_pool_offset_ptr.hpp
    ipc/shm/arena_lend/shm_pool_repository.hpp
    ipc/shm/arena_lend/shm_pool_repository_singleton.hpp
    ipc/transport/struc/shm/arena_lend/jemalloc/jemalloc.hpp
    ipc/transport/struc/shm/arena_lend/jemalloc/jemalloc_fwd.hpp
    ${CAPNP_SCHEMAS}) # Export these like any regular headers...

# ...but also, at the proper time, generate .c++ from them (add to SRCS) and .h also from them (add to HDRS).
# Reminder: this is a supported optional hook of FlowLikeLib.cmake.
function(generate_custom_srcs)
  if(TARGET ipc_transport_structured)
    # Please see explanation in ipc_session's src/CMakeLists.txt for why we do the following.
    set(CAPNPC_IMPORT_DIRS
        ${FLOW_LIKE_META_ROOT_ipc_transport_structured}/src
        ${FLOW_LIKE_META_ROOT_ipc_session}/src
        ${FLOW_LIKE_META_ROOT_ipc_shm}/src)

    message(VERBOSE "We are a subdir of a meta-project.  Manually added the following sibling paths to the capnp "
                      "import paths: [${CAPNPC_IMPORT_DIRS}].")
  endif()

  capnp_generate_cpp(capnp_generated_srcs capnp_generated_hdrs ${CAPNP_SCHEMAS})

  list(APPEND SRCS ${capnp_generated_srcs})
  list(APPEND HDRS ${capnp_generated_hdrs})
  set(SRCS ${SRCS} PARENT_SCOPE)
  set(HDRS ${HDRS} PARENT_SCOPE)

  message(STATUS "Install target: Exports capnp schemas [${CAPNP_SCHEMAS}].")
  message(STATUS "Install target: Exports capnp schema-generated headers [${capnp_generated_hdrs}].")
  message(STATUS "capnp-generated sources: [${capnp_generated_srcs}].")
endfunction()

include("${FLOW_LIKE_TOOLS}/FlowLikeLib.cmake")

# See above.
post_include_setup()

# Side discussion: The 3 (as of this writing) cache settings set/checked by
# find_*()/pkg_check_modules()/etc. above work fine; but really it'd
# be nice to provide docstrings for them (for example I did so for FLOW_IPC_JEMALLOC_PREFIX which participates in no
# find_*()/etc.).  Sure, it's just for ccmake/etc. users, but it's a nice thing to do.  CMake 3.26.3 at least won't
# let me: the `set(BLAH CACHE STRING "docstring")` is okay in and of itself, but it prevents find_*()/etc. from doing
# anything, even when ${BLAH} is empty -- I guess if it's defined (even if blank) it skips it?  Bizarre.
# I (ygoldfel) tried pretty hard to work around it, but it was just unwieldy for such a silly thing; so I decided
# the default docstrings for these ("Path to a library.", etc.) were not too bad, as long as we provide clear
# error messages and otherwise document all this elsewhere.  TODO: Revisit.  Maybe a later CMake fixed it.
