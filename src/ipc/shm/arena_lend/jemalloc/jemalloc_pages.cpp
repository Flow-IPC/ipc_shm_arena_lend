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

// echan - This file is taken from src/pages.c from jemalloc source, namely version 5.2.1. (See more notes below.)

#include "ipc/shm/arena_lend/jemalloc/jemalloc_pages.hpp"
#include <flow/common.hpp>

// echan - Added for compilation purposes
#include <cstddef>
#include <cstdint>
#include <cassert>
#include <sys/mman.h>
#include <cstdarg>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>

// echan - Build warnings from jemalloc internal functions
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

using std::size_t;

namespace ipc::shm::arena_lend::jemalloc
{

/* ygoldfel - My understanding is, indeed, echan needed to make some additions for SHM support
 * in a jemalloc-compatible way and thus, as he noted above, leveraged existing existing
 * *internal* jemalloc source code. Hence such code was placed here and then somewhat modified (as little
 * as possible). One complication was that, to compile/work, these pages.c snippets also needed
 * certain items from also-internal jemalloc header files: ones residing in include/internal/<...>.h. To be clear:
 * these are not like STL/Boost .../detail/... headers which are not supposed to be #include<>d *directly* by user
 * but physically *can* be (and indirectly are, via non-`detail` ones). These jemalloc/internal/<...>.h
 * are in fact not installed into the include-path when one `make install`s
 * jemalloc. So there was a choice of either shipping entire such files inside this SHM-jemalloc project;
 * or trying to replicate just the necessary parts. The latter was somewhat doable: it is
 * just a handful of straightforward macros/constants -- assuming Linux, which we do for now -- the definition
 * of which is unlikely to change. Unfortunately a malloc_read_fd() helper had to be replicated, but it is
 * relatively straightforward too.
 *
 * When making changes: Be very careful. Just because it compiles does *not* mean it works correctly; e.g.,
 * if a #define like JEMALLOC_USE_SYSCALL is not defined when it must be, incorrect code might execute.
 * Fortunately we have detailed unit tests that should catch issues. That said, when making changes, it is
 * important to go over the existing jemalloc code without assuming uncritically that if it builds, then it works.
 * That is especially the case if we add support for more OS/environments (as of this writing it is only Linux).
 * @todo Longer-term we'd write this code more "our way," while using jemalloc's insights as to the necessary
 * algorithms involved.
 *
 * The following are the headers/stuff a small subset of which we then replicate just below. */

#if 0

#  define JEMALLOC_PAGES_C_

#  include <jemalloc/jemalloc.h>
#  include "jemalloc/internal/jemalloc_preamble.h"
#  include <jemalloc/internal/jemalloc_internal_macros.h>
#  include <jemalloc/internal/jemalloc_internal_defs.h>
#  include <jemalloc/internal/jemalloc_internal_types.h>

/* ygoldfel - echan had this being compiled in this spot, but the relevant code was `#if 0`d out (search below), so
 * we can `#if 0` this out too for the time being. */
#  ifdef JEMALLOC_DEFINE_MADVISE_FREE
#    define JEMALLOC_MADV_FREE 8
#  endif

#  include "jemalloc/internal/pages.h"
#  include "jemalloc/internal/jemalloc_internal_includes.h"
#  include "jemalloc/internal/assert.h"
#  include "jemalloc/internal/malloc_io.h"

// ygoldfel - echan had this being compiled in this spot, but it is a BSD thing, so let's just keep it tight.
#  ifdef JEMALLOC_SYSCTL_VM_OVERCOMMIT
#    include <sys/sysctl.h>
#    ifdef __FreeBSD__
#      include <vm/vm_param.h>
#    endif
#  endif

#endif // #if 0

#ifndef FLOW_OS_LINUX
#  error "We are doing things here which, even if they built OK in non-Linux OS, we'd need to carefully test first."
#endif
// Now we assume Linux which might matter at least for the next few lines.

/* ygoldfel - This is the replication of the needed subset of the above. These are copy/pasted (except tabs vs.
 * spaces/etc.) from jemalloc except where noted. */

#define PAGE \
  ((size_t)(1U << LG_PAGE))
#define PAGE_MASK \
  ((size_t)(PAGE - 1))
#define PAGE_ADDR2BASE(a)\
  ((void *)((uintptr_t)(a) & ~PAGE_MASK))
#define PAGE_CEILING(s)\
  (((s) + PAGE_MASK) & ~PAGE_MASK)

#define ALIGNMENT_ADDR2BASE(a, alignment) \
  ((void *)((uintptr_t)(a) & ((~(alignment)) + 1)))
#define ALIGNMENT_ADDR2OFFSET(a, alignment) \
  ((size_t)((uintptr_t)(a) & (alignment - 1)))
#define ALIGNMENT_CEILING(s, alignment) \
  (((s) + (alignment - 1)) & ((~(alignment)) + 1))

// yoldfel - These are correct to be defined in Linux.
#define JEMALLOC_PROC_SYS_VM_OVERCOMMIT_MEMORY
#define JEMALLOC_USE_SYSCALL

/* ygoldfel - That leaves LG_PAGE. Substance-wise: if the page size is X, which is always a 1 bit followed by
 * N zeroes, then LG_PAGE is the index of the 1-bit (0-based). Commonly that's 12 (1 << 12 == 4096).
 * Technically jemalloc has this set at compile-time by splicing it into #define in a generated .h after finding it
 * programmatically in a similar way (during configure step) to how we are about to do it. We could do something
 * like that, but finding it before main() at run-time, and referring to the `const` in the code below, is hardly
 * a major perf loss. Nor do we need it set before main().
 * @todo Revisit. It can after all be precomputed and spliced-in. It's just annoying build-wise.
 *
 * I choose the type as `int` only because a "naked" literal (`#define 12` in jemalloc-configure-generated code)
 * is `int` in C/C++. */
static const auto LG_PAGE = int(::ffsl(::sysconf(_SC_PAGESIZE))) - 1;

// ygoldfel - Now the aforementioned helper: copy/pasted from jemalloc/internal/malloc_io.h.
static inline ssize_t
malloc_read_fd(int fd, void *buf, size_t count) {
#if defined(JEMALLOC_USE_SYSCALL) && defined(SYS_read)
        long result = syscall(SYS_read, fd, buf, count);
#else
        ssize_t result = read(fd, buf,
#ifdef _WIN32
            (unsigned int)
#endif
            count);
#endif
        return (ssize_t)result;
}

// ygoldfel - pages.c is reproduced now, with changes where marked.

/******************************************************************************/
/* Data. */

/* Actual operating system page size, detected during bootstrap, <= PAGE. */
static size_t os_page;

#ifndef _WIN32
#  define PAGES_PROT_COMMIT (PROT_READ | PROT_WRITE)
#  define PAGES_PROT_DECOMMIT (PROT_NONE)
static int mmap_flags;
#endif
static bool os_overcommits;

// echan - Commented out - Disable automatic huge page usage and lazy purge is unsupported for shm
#if 0
const char *thp_mode_names[] = {
  "default",
  "always",
  "never",
  "not supported"
};
thp_mode_t opt_thp = THP_MODE_DEFAULT;
thp_mode_t init_system_thp_mode;

/* Runtime support for lazy purge. Irrelevant when !pages_can_purge_lazy. */
static bool pages_can_purge_lazy_runtime = true;
#endif // #if 0

/******************************************************************************/
/*
 * Function prototypes for static functions that are referenced prior to
 * definition.
 */

static void os_pages_unmap(void *addr, size_t size);

/******************************************************************************/

/**
 * Returns the mapping flags to use when memory mapping.
 *
 * @param fd The file descriptor used when mapping
 *
 * @return Ditto
 */
static int determine_mmap_flags(int fd)
{
  int flags = (fd != -1) ? MAP_SHARED : mmap_flags;
#ifdef MAP_NORESERVE
  if (os_overcommits)
  {
    flags |= MAP_NORESERVE;
  }
#endif

  return flags;
}

// echan - Added fd parameter
static void *
os_pages_map(void *addr, size_t size, size_t alignment, bool *commit, int fd) {
  assert(ALIGNMENT_ADDR2BASE(addr, os_page) == addr);
  assert(ALIGNMENT_CEILING(size, os_page) == size);
  assert(size != 0);

  if (os_overcommits) {
    *commit = true;
  }

  void *ret;
#ifdef _WIN32
  /*
   * If VirtualAlloc can't allocate at the given address when one is
   * given, it fails and returns NULL.
   */
  ret = VirtualAlloc(addr, size, MEM_RESERVE | (*commit ? MEM_COMMIT : 0),
      PAGE_READWRITE);
#else
  /*
   * We don't use MAP_FIXED here, because it can cause the *replacement*
   * of existing mappings, and we only want to create new mappings.
   */
  {
    int prot = *commit ? PAGES_PROT_COMMIT : PAGES_PROT_DECOMMIT;

    // echan - Added fd parameter
    ret = mmap(addr, size, prot, determine_mmap_flags(fd), fd, 0);
  }
  assert(ret != NULL);

  if (ret == MAP_FAILED) {
    ret = NULL;
  } else if (addr != NULL && ret != addr) {
    /*
     * We succeeded in mapping memory, but not in the right place.
     */
    os_pages_unmap(ret, size);
    ret = NULL;
  }
#endif
  assert(ret == NULL || (addr == NULL && ret != addr) || (addr != NULL &&
      ret == addr));
  return ret;
}

static void *
os_pages_trim(void *addr, size_t alloc_size, size_t leadsize, size_t size,
              bool *commit) {
  void *ret = (void *)((uintptr_t)addr + leadsize);

  assert(alloc_size >= leadsize + size);
#ifdef _WIN32
  os_pages_unmap(addr, alloc_size);
  void *new_addr = os_pages_map(ret, size, PAGE, commit);
  if (new_addr == ret) {
    return ret;
  }
  if (new_addr != NULL) {
    os_pages_unmap(new_addr, size);
  }
  return NULL;
#else
  size_t trailsize = alloc_size - leadsize - size;

  if (leadsize != 0) {
    os_pages_unmap(addr, leadsize);
  }
  if (trailsize != 0) {
    os_pages_unmap((void *)((uintptr_t)ret + size), trailsize);
  }
  return ret;
#endif
}

static void
os_pages_unmap(void *addr, size_t size) {
  assert(ALIGNMENT_ADDR2BASE(addr, os_page) == addr);
  assert(ALIGNMENT_CEILING(size, os_page) == size);

#ifdef _WIN32
  if (VirtualFree(addr, 0, MEM_RELEASE) == 0)
#else
  if (munmap(addr, size) == -1)
#endif
  {
    // echan - Commented out - Private functions
#if 0
    char buf[BUFERROR_BUF];

    buferror(get_errno(), buf, sizeof(buf));
    malloc_printf("<jemalloc>: Error in "
#ifdef _WIN32
        "VirtualFree"
#else
        "munmap"
#endif
        "(): %s\n", buf);
    if (opt_abort) {
      abort();
    }
#endif // #if 0
    assert(false && "Could not unmap");
  }
}

// echan - Added fd parameter
static void *
pages_map_slow(size_t size, size_t alignment, bool *commit, int fd) {
  size_t alloc_size = size + alignment - os_page;
  /* Beware size_t wrap-around. */
  if (alloc_size < size) {
    return NULL;
  }

  void *ret;
  do {
    // echan - Added fd param
    // echan - We cannot overallocate to a memory mapped file, so use heap memory first
    void *pages = os_pages_map(NULL, alloc_size, alignment, commit, -1);
    if (pages == NULL) {
      return NULL;
    }
    size_t leadsize = ALIGNMENT_CEILING((uintptr_t)pages, alignment)
        - (uintptr_t)pages;
    ret = os_pages_trim(pages, alloc_size, leadsize, size, commit);
  } while (ret == NULL);

  // echan - Added section - Map over with memory mapped file at the same location
  if (fd != -1)
  {
    int prot = *commit ? PAGES_PROT_COMMIT : PAGES_PROT_DECOMMIT;
    void* ret2 = mmap(ret, size, prot, (determine_mmap_flags(fd) | MAP_FIXED), fd, 0);
    if (ret2 == MAP_FAILED)
    {
      ret = NULL;
    }
    else if (ret2 != ret)
    {
      // We succeeded in mapping memory, but not in the right place. This is likely close to an assertion.
      os_pages_unmap(ret2, size);
      ret = NULL;
    }
  }

  assert(ret != NULL);
  assert(PAGE_ADDR2BASE(ret) == ret);
  return ret;
}

// echan - Added fd parameter
void *
pages_map(void *addr, size_t size, size_t alignment, bool *commit, int fd) {
  assert(alignment >= PAGE);
  assert(ALIGNMENT_ADDR2BASE(addr, alignment) == addr);

#if defined(__FreeBSD__) && defined(MAP_EXCL)
  /*
   * FreeBSD has mechanisms both to mmap at specific address without
   * touching existing mappings, and to mmap with specific alignment.
   */
  {
    if (os_overcommits) {
      *commit = true;
    }

    int prot = *commit ? PAGES_PROT_COMMIT : PAGES_PROT_DECOMMIT;
    int flags = mmap_flags;

    if (addr != NULL) {
      flags |= MAP_FIXED | MAP_EXCL;
    } else {
      unsigned alignment_bits = ffs_zu(alignment);
      assert(alignment_bits > 1);
      flags |= MAP_ALIGNED(alignment_bits - 1);
    }

    void *ret = mmap(addr, size, prot, flags, -1, 0);
    if (ret == MAP_FAILED) {
      ret = NULL;
    }

    return ret;
  }
#endif
  /*
   * Ideally, there would be a way to specify alignment to mmap() (like
   * NetBSD has), but in the absence of such a feature, we have to work
   * hard to efficiently create aligned mappings.  The reliable, but
   * slow method is to create a mapping that is over-sized, then trim the
   * excess.  However, that always results in one or two calls to
   * os_pages_unmap(), and it can leave holes in the process's virtual
   * memory map if memory grows downward.
   *
   * Optimistically try mapping precisely the right amount before falling
   * back to the slow method, with the expectation that the optimistic
   * approach works most of the time.
   */

  void *ret = os_pages_map(addr, size, os_page, commit, fd);
  if (ret == NULL || ret == addr) {
    return ret;
  }
  assert(addr == NULL);
  if (ALIGNMENT_ADDR2OFFSET(ret, alignment) != 0) {
    os_pages_unmap(ret, size);
    return pages_map_slow(size, alignment, commit, fd);
  }

  assert(PAGE_ADDR2BASE(ret) == ret);
  return ret;
}

void
pages_unmap(void *addr, size_t size) {
  assert(PAGE_ADDR2BASE(addr) == addr);
  assert(PAGE_CEILING(size) == size);

  os_pages_unmap(addr, size);
}

static bool
pages_commit_impl(void *addr, size_t size, bool commit) {
  assert(PAGE_ADDR2BASE(addr) == addr);
  assert(PAGE_CEILING(size) == size);

  if (os_overcommits) {
    return true;
  }

#ifdef _WIN32
  return (commit ? (addr != VirtualAlloc(addr, size, MEM_COMMIT,
      PAGE_READWRITE)) : (!VirtualFree(addr, size, MEM_DECOMMIT)));
#else
  {
                int prot = commit ? PAGES_PROT_COMMIT : PAGES_PROT_DECOMMIT;
    void *result = mmap(addr, size, prot, mmap_flags | MAP_FIXED,
        -1, 0);
    if (result == MAP_FAILED) {
      return true;
    }
    if (result != addr) {
      /*
       * We succeeded in mapping memory, but not in the right
       * place.
       */
      os_pages_unmap(result, size);
      return true;
    }
    return false;
  }
#endif
}

bool
pages_commit(void *addr, size_t size) {
         return pages_commit_impl(addr, size, true);
}

bool
pages_decommit(void *addr, size_t size) {
         return pages_commit_impl(addr, size, false);
}

#if 0
// echan - Cannot use MADV_FREE for shared memory pages, so lazy purge is unsupported
bool
pages_purge_lazy(void *addr, size_t size) {
  assert(ALIGNMENT_ADDR2BASE(addr, os_page) == addr);
  assert(PAGE_CEILING(size) == size);

  if (!pages_can_purge_lazy) {
    return true;
  }
  if (!pages_can_purge_lazy_runtime) {
    /*
     * Built with lazy purge enabled, but detected it was not
     * supported on the current system.
     */
    return true;
  }

#ifdef _WIN32
  VirtualAlloc(addr, size, MEM_RESET, PAGE_READWRITE);
  return false;
#elif defined(JEMALLOC_PURGE_MADVISE_FREE)
  return (madvise(addr, size,
#  ifdef MADV_FREE
      MADV_FREE
#  else
      JEMALLOC_MADV_FREE
#  endif
      ) != 0);
#elif defined(JEMALLOC_PURGE_MADVISE_DONTNEED) && \
    !defined(JEMALLOC_PURGE_MADVISE_DONTNEED_ZEROS)
  return (madvise(addr, size, MADV_DONTNEED) != 0);
#else
  not_reached();
#endif
}

// echan - madvise is ineffective for shared memory pages
bool
pages_purge_forced(void *addr, size_t size) {
  assert(PAGE_ADDR2BASE(addr) == addr);
  assert(PAGE_CEILING(size) == size);

  if (!pages_can_purge_forced) {
    return true;
  }

#if defined(JEMALLOC_PURGE_MADVISE_DONTNEED) && \
    defined(JEMALLOC_PURGE_MADVISE_DONTNEED_ZEROS)
  return (madvise(addr, size, MADV_DONTNEED) != 0);
#elif defined(JEMALLOC_MAPS_COALESCE)
  /* Try to overlay a new demand-zeroed mapping. */
  return pages_commit(addr, size);
#else
  not_reached();
#endif

  return false;
}

// echan - Commented out - Disable automatic huge page usage
static bool
pages_huge_impl(void *addr, size_t size, bool aligned) {
  if (aligned) {
    assert(HUGEPAGE_ADDR2BASE(addr) == addr);
    assert(HUGEPAGE_CEILING(size) == size);
  }
#ifdef JEMALLOC_HAVE_MADVISE_HUGE
  return (madvise(addr, size, MADV_HUGEPAGE) != 0);
#else
  return true;
#endif
}

bool
pages_huge(void *addr, size_t size) {
  return pages_huge_impl(addr, size, true);
}

static bool
pages_huge_unaligned(void *addr, size_t size) {
  return pages_huge_impl(addr, size, false);
}

static bool
pages_nohuge_impl(void *addr, size_t size, bool aligned) {
  if (aligned) {
    assert(HUGEPAGE_ADDR2BASE(addr) == addr);
    assert(HUGEPAGE_CEILING(size) == size);
  }

#ifdef JEMALLOC_HAVE_MADVISE_HUGE
  return (madvise(addr, size, MADV_NOHUGEPAGE) != 0);
#else
  return false;
#endif
}

bool
pages_nohuge(void *addr, size_t size) {
  return pages_nohuge_impl(addr, size, true);
}

static bool
pages_nohuge_unaligned(void *addr, size_t size) {
  return pages_nohuge_impl(addr, size, false);
}

bool
pages_dontdump(void *addr, size_t size) {
  assert(PAGE_ADDR2BASE(addr) == addr);
  assert(PAGE_CEILING(size) == size);
#ifdef JEMALLOC_MADVISE_DONTDUMP
  return madvise(addr, size, MADV_DONTDUMP) != 0;
#else
  return false;
#endif
}

bool
pages_dodump(void *addr, size_t size) {
  assert(PAGE_ADDR2BASE(addr) == addr);
  assert(PAGE_CEILING(size) == size);
#ifdef JEMALLOC_MADVISE_DONTDUMP
  return madvise(addr, size, MADV_DODUMP) != 0;
#else
  return false;
#endif
}
#endif // #if 0


static size_t
os_page_detect(void) {
#ifdef _WIN32
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return si.dwPageSize;
#elif defined(__FreeBSD__)
  /*
   * This returns the value obtained from
   * the auxv vector, avoiding a syscall.
   */
  return getpagesize();
#else
  long result = sysconf(_SC_PAGESIZE);
  if (result == -1) {
    return LG_PAGE;
  }
  return (size_t)result;
#endif
}

#ifdef JEMALLOC_SYSCTL_VM_OVERCOMMIT
static bool
os_overcommits_sysctl(void) {
  int vm_overcommit;
  size_t sz;

  sz = sizeof(vm_overcommit);
#if defined(__FreeBSD__) && defined(VM_OVERCOMMIT)
  int mib[2];

  mib[0] = CTL_VM;
  mib[1] = VM_OVERCOMMIT;
  if (sysctl(mib, 2, &vm_overcommit, &sz, NULL, 0) != 0) {
    return false; /* Error. */
  }
#else
  if (sysctlbyname("vm.overcommit", &vm_overcommit, &sz, NULL, 0) != 0) {
    return false; /* Error. */
  }
#endif

  return ((vm_overcommit & 0x3) == 0);
}
#endif

#ifdef JEMALLOC_PROC_SYS_VM_OVERCOMMIT_MEMORY
/*
 * Use syscall(2) rather than {open,read,close}(2) when possible to avoid
 * reentry during bootstrapping if another library has interposed system call
 * wrappers.
 */
static bool
os_overcommits_proc(void) {
  int fd;
  char buf[1];

#if defined(JEMALLOC_USE_SYSCALL) && defined(SYS_open)
  #if defined(O_CLOEXEC)
    fd = (int)syscall(SYS_open, "/proc/sys/vm/overcommit_memory", O_RDONLY |
      O_CLOEXEC);
  #else
    fd = (int)syscall(SYS_open, "/proc/sys/vm/overcommit_memory", O_RDONLY);
    if (fd != -1) {
      fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
    }
  #endif
#elif defined(JEMALLOC_USE_SYSCALL) && defined(SYS_openat)
  #if defined(O_CLOEXEC)
    fd = (int)syscall(SYS_openat,
      AT_FDCWD, "/proc/sys/vm/overcommit_memory", O_RDONLY | O_CLOEXEC);
  #else
    fd = (int)syscall(SYS_openat,
      AT_FDCWD, "/proc/sys/vm/overcommit_memory", O_RDONLY);
    if (fd != -1) {
      fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
    }
  #endif
#else
  #if defined(O_CLOEXEC)
    fd = open("/proc/sys/vm/overcommit_memory", O_RDONLY | O_CLOEXEC);
  #else
    fd = open("/proc/sys/vm/overcommit_memory", O_RDONLY);
    if (fd != -1) {
      fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
    }
  #endif
#endif

  if (fd == -1) {
    return false; /* Error. */
  }

  ssize_t nread = malloc_read_fd(fd, &buf, sizeof(buf));
#if defined(JEMALLOC_USE_SYSCALL) && defined(SYS_close)
  syscall(SYS_close, fd);
#else
  close(fd);
#endif

  if (nread < 1) {
    return false; /* Error. */
  }
  /*
   * /proc/sys/vm/overcommit_memory meanings:
   * 0: Heuristic overcommit.
   * 1: Always overcommit.
   * 2: Never overcommit.
   */
  return (buf[0] == '0' || buf[0] == '1');
}
#endif

// echan - Commented out - Disable automatic huge page usage
#if 0
void
pages_set_thp_state (void *ptr, size_t size) {
  if (opt_thp == thp_mode_default || opt_thp == init_system_thp_mode) {
    return;
  }
  assert(opt_thp != thp_mode_not_supported &&
      init_system_thp_mode != thp_mode_not_supported);

  if (opt_thp == thp_mode_always
      && init_system_thp_mode != thp_mode_never) {
    assert(init_system_thp_mode == thp_mode_default);
    pages_huge_unaligned(ptr, size);
  } else if (opt_thp == thp_mode_never) {
    assert(init_system_thp_mode == thp_mode_default ||
        init_system_thp_mode == thp_mode_always);
    pages_nohuge_unaligned(ptr, size);
  }
}

static void
init_thp_state(void) {
  if (!have_madvise_huge) {
    if (metadata_thp_enabled() && opt_abort) {
      malloc_write("<jemalloc>: no MADV_HUGEPAGE support\n");
      abort();
    }
    goto label_error;
  }

  static const char sys_state_madvise[] = "always [madvise] never\n";
  static const char sys_state_always[] = "[always] madvise never\n";
  static const char sys_state_never[] = "always madvise [never]\n";
  char buf[sizeof(sys_state_madvise)];

#if defined(JEMALLOC_USE_SYSCALL) && defined(SYS_open)
  int fd = (int)syscall(SYS_open,
      "/sys/kernel/mm/transparent_hugepage/enabled", O_RDONLY);
#else
  int fd = open("/sys/kernel/mm/transparent_hugepage/enabled", O_RDONLY);
#endif
  if (fd == -1) {
    goto label_error;
  }

  ssize_t nread = malloc_read_fd(fd, &buf, sizeof(buf));
#if defined(JEMALLOC_USE_SYSCALL) && defined(SYS_close)
  syscall(SYS_close, fd);
#else
  close(fd);
#endif

  if (nread < 0) {
    goto label_error;
  }

  if (strncmp(buf, sys_state_madvise, (size_t)nread) == 0) {
    init_system_thp_mode = thp_mode_default;
  } else if (strncmp(buf, sys_state_always, (size_t)nread) == 0) {
    init_system_thp_mode = thp_mode_always;
  } else if (strncmp(buf, sys_state_never, (size_t)nread) == 0) {
    init_system_thp_mode = thp_mode_never;
  } else {
    goto label_error;
  }
  return;
label_error:
  opt_thp = init_system_thp_mode = thp_mode_not_supported;
}
#endif // #if 0

// echan - Added static initialization
__attribute__((constructor))
bool
pages_boot(void) {
  os_page = os_page_detect();

  /* ygoldfel - Had to remove the `os_page_detect() <= PAGE` check; it does not quite work due to order of
   * static initialization; however in Linux (which, for now at least, we actively assume) jemalloc's configure
   * uses this same method to determine LG_PAGE and therefore PAGE; and we echo that in setting
   * LG_PAGE in its static initializer. It's a bit of an ugly house of cards, but I am following the idea of
   * changing jemalloc's (slightly modified) code as little as possible. Perhaps: @todo Longer-term we'd write
   * this code more "our way," while using jemalloc's insights as to the necessary algorithms involved. */
#if 0
  if (os_page > PAGE) {
    malloc_write("<jemalloc>: Unsupported system page size\n");
    if (opt_abort) {
      abort();
    }
    return true;
  }
#endif // #if 0

#ifndef _WIN32
  mmap_flags = MAP_PRIVATE | MAP_ANON;
#endif

#ifdef JEMALLOC_SYSCTL_VM_OVERCOMMIT
  os_overcommits = os_overcommits_sysctl();
#elif defined(JEMALLOC_PROC_SYS_VM_OVERCOMMIT_MEMORY)
  os_overcommits = os_overcommits_proc();
#  ifdef MAP_NORESERVE
  if (os_overcommits) {
    mmap_flags |= MAP_NORESERVE;
  }
#  endif
#else
  os_overcommits = false;
#endif

  // echan - Commented out - Disable automatic huge page usage
#if 0
  init_thp_state();

#ifdef __FreeBSD__
  /*
   * FreeBSD doesn't need the check; madvise(2) is known to work.
   */
#else
  /* Detect lazy purge runtime support. */
  if (pages_can_purge_lazy) {
    bool committed = false;
    void *madv_free_page = os_pages_map(NULL, PAGE, PAGE, &committed);
    if (madv_free_page == NULL) {
      return true;
    }
    assert(pages_can_purge_lazy_runtime);
    if (pages_purge_lazy(madv_free_page, PAGE)) {
      pages_can_purge_lazy_runtime = false;
    }
    os_pages_unmap(madv_free_page, PAGE);
  }
#endif

#endif // #if 0
  return false;
}

// Static method
size_t Jemalloc_pages::get_page_size()
{
  return os_page;
}

// Static method
size_t Jemalloc_pages::get_page_mask()
{
  return PAGE_MASK;
}

// Static method
void* Jemalloc_pages::map(void* address, size_t size, size_t alignment, bool& commit, int fd)
{
  if ((alignment == PAGE) || (address != nullptr))
  {
    return pages_map(address, size, alignment, &commit, fd);
  }
  else
  {
    // This assertion is already within pages_map()
    assert(alignment >= PAGE);
    return pages_map_slow(size, alignment, &commit, fd);
  }
}

// Static method
void Jemalloc_pages::unmap(void* address, size_t size)
{
  pages_unmap(address, size);
}

// Static method
bool Jemalloc_pages::commit(void* address, size_t size)
{
  assert(PAGE_ADDR2BASE(address) == address);
  assert(PAGE_CEILING(size) == size);

  if (os_overcommits)
  {
    return false;
  }

  // echan - Using mprotect instead of mmap to avoid destructiveness in case of bugs
  return (mprotect(address, size, PAGES_PROT_COMMIT) == 0);
}

// Static method
bool Jemalloc_pages::commit_original(void* address, size_t size)
{
  return !pages_commit(address, size);
}

// Static method
bool Jemalloc_pages::decommit(void* address, int fd, size_t file_offset, size_t size, bool force)
{
  assert(PAGE_ADDR2BASE(address) == address);
  assert(fd != -1);
  assert(PAGE_CEILING(file_offset) == file_offset);
  assert(PAGE_CEILING(size) == size);

  if (!force && os_overcommits)
  {
    return false;
  }

  // Order is important here as once we mark the pages as non-writeable, we cannot return false as the caller
  // may think state has not changed
  return (purge_forced(fd, file_offset, size) && (mprotect(address, size, PAGES_PROT_DECOMMIT) == 0));
}

// Static method
bool Jemalloc_pages::decommit_original(void* address, size_t size)
{
  return !pages_decommit(address, size);
}

// Static method
bool Jemalloc_pages::purge_forced(int fd, size_t file_offset, size_t size)
{
  assert(fd != -1);
  assert(PAGE_CEILING(file_offset) == file_offset);
  assert(PAGE_CEILING(size) == size);

  return (fallocate(fd, (FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE), file_offset, size) == 0);
}

// Static method
void Jemalloc_pages::set_os_overcommit_memory(bool overcommit)
{
  os_overcommits = overcommit;
}

// Static method
bool Jemalloc_pages::get_os_overcommit_memory()
{
  return os_overcommits;
}

} // namespace ipc::shm::arena_lend::jemalloc

#pragma GCC diagnostic pop // ignored "-Wunused-parameter"
