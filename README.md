# Flow-IPC Sub-project -- SHM-jemalloc -- Commercial-grade jemalloc memory manager turbocharges your zero-copy work

This project is a sub-project of the larger Flow-IPC meta-project.  Please see
a similar `README.md` for Flow-IPC, first.  You can most likely find it either in the parent
directory to this one; or else in a sibling GitHub repository named `ipc.git`.

A more grounded description of the various sub-projects of Flow-IPC, including this one, can be found
in `./src/ipc/common.hpp` off the directory containing the present README.  Look for
`Distributed sub-components (libraries)` in a large C++ comment.

Took a look at those?  Still interested in `ipc_shm` as an independent entity?  Then read on.
Before you do though: it is, typically, both easier and more functional to simply treat Flow-IPC as a whole.
To do so it is sufficient to never have to delve into topics discussed in this README.  In particular
the Flow-IPC generated documentation guided Manual + Reference are monolithic and cover all the
sub-projects together, including this one.

Still interested?  Then read on.

`ipc_shm_arena_lend` depends on `ipc_shm` (and all its dependencies; i.e. `ipc_session`, `ipc_transport_structured`,
`ipc_core`, `flow`).  It provides `ipc::transport::struc::shm::arena_lend`, `ipc::shm::arena_lend`, and
`ipc::session::shm::arena_lend`.

`ipc_shm_arena_lend` (a/k/a **SHM-jemalloc**) adds an alternative **SHM-jemalloc SHM provider** to the one from
its immediate dependency, `ipc_shm`, which provides the **SHM-classic SHM provider**.
For most users, by changing the characters `classic` to `arena_lend::jemalloc` in a couple locations in
your code, one will simply gain the properties of SHM-jemalloc.

  - Constructing (allocating) objects in shared memory (whether directly or indirectly, such as when
    creating `ipc::transport::struc::Msg_out`s for sending via `struc::Channel`), SHM-jemalloc will use
    the commercial-grade memory allocation provider, jemalloc (which powers Meta, FreeBSD, and many other
    huge things).  You get thread caching, fragmentation avoidance -- all that good stuff that regular-heap
    `malloc()` and `new` gets you.  By contrast SHM-classic (from `ipc_shm`) uses a reasonable but basic
    Boost-supplied allocation algorithm with few to no such perfomance-oriented features.  (One can replace
    that algorithm, but it's not easy to do better: that's why things like jemalloc and tcmalloc exist and
    are no joke.)
  - The owner-segregated (a/k/a "arena-lending," hence the name) design of SHM-jemalloc enables a greater
    degree of safety which can matter a great deal in server situations with sensitive customer data.
    In the case of SHM-classic, both processes are reading and writing in the same SHM segment (**SHM pool**);
    if one crashes (among other things), both processes are "poisoned" w/r/t access to any data therein.
    Not so with SHM-jemalloc.  (This is a hand-wavy description; rest assured the real documentation gets into
    all the desired details.  Take a look at the guided Manual.  See Documentation below.)

## Documentation

See Flow-IPC meta-project's `README.md` Documentation section.  `ipc_shm_arena_lend` lacks its own generated docs.
However, it contributes to the aforementioned monolithic documentation through its many comments which can
(of course) be found directly in its code (`./src/ipc/...`).  (The monolithic generated documentation scans
these comments using Doxygen, combined with its siblings' comments... and so on.)

## Obtaining the source code

- As a tarball/zip: The [project web site](https://flow-ipc.github.io) links to individual releases with notes, docs,
  download links.  We are included in a subdirectory off the Flow-IPC root.
- For Git access:
  - `git clone --recurse-submodules git@github.com:Flow-IPC/ipc.git`; or
  - `git clone git@github.com:Flow-IPC/ipc_shm_arena_lend.git`

## Installation

See [INSTALL](./INSTALL.md) guide.

## Contributing

See [CONTRIBUTING](./CONTRIBUTING.md) guide.
