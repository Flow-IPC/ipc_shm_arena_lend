# Installing the software

## Installation

An exported `ipc_shm_arena_lend` consists of C++ header files installed under "ipc/..." in the
include-root; and a library such as `libipc_shm_arena_lend.a`.
Certain items are also exported for people who use CMake to build their own
projects; we make it particularly easy to use `ipc_shm_arena_lend` proper in that case
(`find_package(IpcShmArenaLend)`).  However documentation is generated monolithically for all of Flow-IPC;
not for `ipc_shm_arena_lend` separately.

The basic prerequisites for *building* the above:

  - Linux;
  - a C++ compiler with C++ 17 support;
  - Boost headers (plus certain libraries) install;
  - {fmt} install;
  - dependency headers and library (from within this overall project) install(s); in this case those of:
    `flow`, `ipc_core`, `ipc_transport_structured`, `ipc_session`, `ipc_shm`;
  - CMake;
  - Cap'n Proto (a/k/a capnp);
  - jemalloc.

The basic prerequisites for *using* the above:

  - Linux, C++ compiler, Boost, {fmt}, above-listed dependency lib(s), capnp, jemalloc (but CMake is not required);
    plus:
  - your source code `#include`ing any exported `ipc/` headers must be itself built in C++ 17 mode;
  - any executable using the `ipc_*` libraries must be linked with certain Boost and ubiquitous
    system libraries.

We intentionally omit version numbers and even specific compiler types in the above description; the CMake run
should help you with that.

To build `ipc_shm_arena_lend`:

  1. Ensure a Boost install is available.  If you don't have one, please install the latest version at
     [boost.org](https://boost.org).  If you do have one, try using that one (our build will complain if insufficient).
     (From this point on, that's the recommended tactic to use when deciding on the version number for any given
     prerequisite.  E.g., same deal with CMake in step 2.)
  2. Ensure a {fmt} install is available (available at [{fmt} web site](https://fmt.dev/) if needed).
  3. Ensure a CMake install is available (available at [CMake web site](https://cmake.org/download/) if needed).
  4. Ensure a capnp install is available (available at [Cap'n Proto web site](https://capnproto.org/) if needed).
  5. Ensure a jemalloc install is available (available at [jemalloc web site](https://jemalloc.net/) if needed).
     - If you are already using jemalloc in your under-development executable(s), great.  We will work
       whether you're using it to replace default `malloc()`/`free()` and (orthogonally) `new`/`delete`; or
       not.
     - If you are *not* already using jemalloc: When building jemalloc, during its `configure` step, you
       have 2 choices to make.
       - Whether to replace default `malloc()`/`free()` in your application(s).  This is entirely orthogonal
         to Flow-IPC's operation per se; rather it will affect general heap use in your application.
         Most likely converting your heap provider to jemalloc is a separate mission versus using Flow-IPC;
         so your answer will then be no, you don't want to replace `malloc()` and `free()`.  In that case,
         when building jemalloc, use its `configure` feature wherein one supplies an API-name prefix to that
         script.
         - We recommend the prefix: `je_`.  (Then the API will include `je_malloc()`, `je_free()`, and others.)
         - If you do (now, or later) intend to replace the default `malloc()`/`free()` with jemalloc's, then
           do not supply any prefix to `configure`.
       - Whether to replace `new`/`delete` (though the default impls may forward to `malloc()`/`free()`; in which
         case even if you do *not* replace them, the choice in the previous bullet will still have effect).
         This is a binary decision: Most likely, again, you don't want this replacement quite yet;
         so tell jemalloc's `configure` that via particular command-line flag.  If you do, then tell `configure`
         *that*.
     - Flow-IPC will automatically build in the way compatible with the way you've built jemalloc.
       (Our CMake script(s) will, internally, use `jemalloc_config` program to determine the chosen API-name
       prefix.)
  6. Use CMake `cmake` (command-line tool) or `ccmake` (interactive text-UI tool) to configure and generate
     a build system (namely a GNU-make `Makefile` and friends).  Details on using CMake are outside our scope here;
     but the basics are as follows.  CMake is very flexible and powerful; we've tried not to mess with that principle
     in our build script(s).
     1. Choose a tool.  `ccmake` will allow you to interactively configure aspects of the build system, including
        showing docs for various knobs our CMakeLists.txt (and friends) have made availale.  `cmake` will do so without
        asking questions; you'll need to provide all required inputs on the command line.  Let's assume `cmake` below,
        but you can use whichever makes sense for you.
     2. Choose a working *build directory*, somewhere outside the present `ipc` distribution.  Let's call this
        `$BUILD`: please `mkdir -p $BUILD`.  Below we'll refer to the directory containing the present `README.md` file
        as `$SRC`.
     3. Configure/generate the build system.  The basic command line:
        `cd $BUILD && cmake -DCMAKE_INSTALL_PREFIX=... -DCMAKE_BUILD_TYPE=... $SRC`,
        where `$CMAKE_INSTALL_PREFIX/{include|lib|...}` will be the export location for headers/library/goodies;
        `CMAKE_BUILD_TYPE={Release|RelWithDebInfo|RelMinSize|Debug|}` specifies build config.
        More options are available -- `CMAKE_*` for CMake ones; `CFG_*` for Flow-IPC ones -- and can be
        viewed with `ccmake` or by glancing at `$BUILD/CMakeCache.txt` after running `cmake` or `ccmake`.
        - Regarding `CMAKE_BUILD_TYPE`, you can use the empty "" type to supply
          your own compile/link flags, such as if your organization or product has a standard set suitable for your
          situation.  With the non-blank types we'll take CMake's sensible defaults -- which you can override
          as well.  (See CMake docs; and/or a shortcut is checking out `$BUILD/CMakeCache.txt`.)
        - This is the likeliest stage at which CMake would detect lacking dependencies.  See CMake docs for
          how to tweak its robust dependency-searching behavior; but generally if it's not in a global system
          location, or not in the `CMAKE_INSTALL_PREFIX` (export) location itself, then you can provide more
          search locations by adding a semicolon-separated list thereof via `-DCMAKE_PREFIX_PATH=...`.
        - Alternatively most things' locations can be individually specified via `..._DIR` settings.
     4. Build using the build system generated in the preceding step:  In `$BUILD` run `make`.
     5. Install (export):  In `$BUILD` run `make install`.

To use `ipc_shm_arena_lend`:

  - `#include` the relevant exported header(s).
  - Link the exported library (such as `libipc_shm_arena_lend.a`) and the required other libraries to
    your executable(s).
    - If using CMake to build such executable(s):
      1. Simply use `find_package(IpcShmArenaLend)` to find it.
      2. Then use `target_link_libraries(... IpcShmArenaLend::ipc_shm_arena_lend)` on your target
         to ensure all necessary libraries are linked.
         (This will include the libraries themselves and the dependency libraries it needs to avoid undefined-reference
         errors when linking.  Details on such things can be found in CMake documentation; and/or you may use
         our CMake script(s) for inspiration; after all we do build all the libraries and a `*_link_test.exec`
         executable.)
    - Otherwise specify it manually based on your build system of choice (if any).  To wit, in order:
      - Link against `libipc_shm_arena_lend.a`, `libipc_shm.a`, `libipc_session.a`, `libipc_transport_structured.a`,
        `libipc_core.a`, and `libflow.a`.
      - Link against Boost libraries mentioned in a `flow/.../CMakeLists.txt` line (search `flow` dependency for it):
        `set(BOOST_LIBS ...)`.
      - Link against the {fmt} library, `libfmt`.
      - Link against the system pthreads library, `librt`, and `libdl`.
  - Read the documentation to learn how to use Flow-IPC's (and/or Flow's) various features.