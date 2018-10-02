//
// fuzzalloc
// A memory allocator for fuzzing
//
// Author: Adrian Herrera
//

#include <errno.h>    // for errno, EINVAL, ENOMEM
#include <stddef.h>   // for ptrdiff_t
#include <stdint.h>   // for uintptr_t
#include <stdlib.h>   // for abort, getenv
#include <string.h>   // for memcpy, memset
#include <sys/mman.h> // for mmap
#include <unistd.h>   // for getpagesize

#include "debug.h"
#include "malloc_internal.h"

//===-- Global variables --------------------------------------------------===//

/// Maps malloc/calloc/realloc def site tags (inserted during compilation) to
/// whether they have been mapped or not
static uint8_t mapped_def_sites[FUZZALLOC_TAG_MAX + 1];

/// Page size determined at runtime by `getpagesize`
static int page_size = 0;

/// Constant determined on first allocation. Size of an mspace, determined from
/// the `MSPACE_SIZE_ENV_VAR` environment variable
static size_t mspace_size = 0;

/// Constant determined on first allocation. Distance between the `mmap`-ed
/// memory and and the start of the `mspace` (which has some overhead associated
/// with it)
static ptrdiff_t mspace_overhead = -1;

#if defined(FUZZALLOC_USE_LOCKS)
static pthread_mutex_t malloc_global_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

//===-- Private helper functions ------------------------------------------===//

static inline uintptr_t align(uintptr_t n, size_t alignment) {
  return (n + alignment - 1) & -alignment;
}

static size_t init_mspace_size(void) {
  size_t psize = MSPACE_DEFAULT_SIZE;

  char *mspace_size_str = getenv(MSPACE_SIZE_ENV_VAR);
  if (mspace_size_str) {
    char *endptr;
    psize = strtoul(mspace_size_str, &endptr, 0);
    if (psize == 0 || *endptr != '\0' || mspace_size_str == endptr) {
      DEBUG_MSG("unable to read %s environment variable: %s\n",
                MSPACE_SIZE_ENV_VAR, mspace_size_str);
      psize = MSPACE_DEFAULT_SIZE;
    }
  } else {
    DEBUG_MSG("%s not set. Using default mspace size\n", MSPACE_SIZE_ENV_VAR);
  }

  // Ensure the mspace size so that it is correctly aligned
  assert(page_size);
  return align(psize, page_size);
}

static mspace create_fuzzalloc_mspace(tag_t def_site_tag) {
  // Memory address is too low
  assert(def_site_tag != 0);

  // This should only happen once
  if (__builtin_expect(page_size == 0, FALSE)) {
    page_size = getpagesize();
    DEBUG_MSG("using page size %d bytes\n", page_size);
  }

  // This should also only happen once
  //
  // XXX When used with ASan and this is first called, environ does not seem
  // to have been initialized yet, so we'll always use the default mspace size
  if (__builtin_expect(mspace_size == 0, FALSE)) {
    mspace_size = init_mspace_size();
    assert(mspace_size <= MSPACE_ALIGNMENT);
    DEBUG_MSG("using mspace size %lu bytes\n", mspace_size);
  }

  // This def site has not been used before. Create a new mspace for this site
  DEBUG_MSG("creating new mspace\n");

  // mmap the requested amount of memory at an address such that the upper bits
  // of the mmap-ed memory match the def site tag
  void *mmap_base_addr = GET_MSPACE(def_site_tag);

  DEBUG_MSG("mmap-ing %lu bytes of memory at %p...\n", mspace_size,
            mmap_base_addr);
  void *mmap_base = mmap(mmap_base_addr, mspace_size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  if (mmap_base == (void *)(-1)) {
    DEBUG_MSG("mmap failed: %s\n", strerror(errno));
    abort();
  }
  DEBUG_MSG("mmap base at %p\n", mmap_base);

  // Create the mspace on the mmap-ed memory
  DEBUG_MSG("creating mspace with base %p (size %lu bytes)\n", mmap_base,
            mspace_size);
#if defined(FUZZALLOC_USE_LOCKS)
  mspace space = create_mspace_with_base(mmap_base, mspace_size, TRUE);
#else
  mspace space = create_mspace_with_base(mmap_base, mspace_size, FALSE);
#endif
  if (!space) {
    DEBUG_MSG("create_mspace_with_base failed at base %p (size %lu bytes)\n",
              mmap_base, mspace_size);
    abort();
  }

  // Setting the mspace overhead should only ever happen once
  if (mspace_overhead == -1) {
    mspace_overhead = space - mmap_base;
    DEBUG_MSG("mspace overhead is %lu bytes\n", mspace_overhead);
  }

  // This is the first memory allocation for this def site, so save the mspace
  // tag into the mspace map (and likewise the def site tag into the def site
  // map)
  DEBUG_MSG("mspace (size %lu bytes) created for def site %#x\n", mspace_size,
            def_site_tag);
  mapped_def_sites[def_site_tag] = TRUE;

  return space;
}

//===-- tagged malloc interface -------------------------------------------===//

void *__tagged_malloc(tag_t def_site_tag, size_t size) {
  DEBUG_MSG("__tagged_malloc(%#x, %lu) called from %p\n", def_site_tag, size,
            __builtin_return_address(0));

  mspace space;

  // Need to ensure that no-one else can update the mapped def sites while we
  // are doing our own mapping
  ACQUIRE_MALLOC_GLOBAL_LOCK();

  if (mapped_def_sites[def_site_tag] == FALSE) {
    space = create_fuzzalloc_mspace(def_site_tag);

    // Release the global lock - we've updated the def site map
    RELEASE_MALLOC_GLOBAL_LOCK();
  } else {
    // Don't need the global lock anymore - the mspace lock will take care of it
    RELEASE_MALLOC_GLOBAL_LOCK();

    assert(mspace_overhead >= 0);
    space = GET_MSPACE(def_site_tag) + mspace_overhead;
  }

  // Note that this doesn't look at previously-allocated memory in this mspace
  // (because that would be too expensive)
  if (__builtin_expect(size > mspace_size, FALSE)) {
    DEBUG_MSG("malloc size (%lu bytes) larger than mspace size (%lu bytes)\n",
              size, mspace_size);
    errno = ENOMEM;

    return NULL;
  }

  void *mem = mspace_malloc(space, size);
  DEBUG_MSG("mspace_malloc(%p, %lu) returned %p\n", space, size, mem);
  assert(mem > space && mem < (space + mspace_size));

  return mem;
}

void *__tagged_calloc(tag_t def_site_tag, size_t nmemb, size_t size) {
  DEBUG_MSG("__tagged_calloc(%#x, %lu, %lu) called from %p\n", def_site_tag,
            nmemb, size, __builtin_return_address(0));

  mspace space;

  // Need to ensure that no-one else can update the mapped def sites while we
  // are doing our own mapping
  ACQUIRE_MALLOC_GLOBAL_LOCK();

  if (mapped_def_sites[def_site_tag] == FALSE) {
    space = create_fuzzalloc_mspace(def_site_tag);

    // Release the global lock - we've updated the def site map
    RELEASE_MALLOC_GLOBAL_LOCK();
  } else {
    // Don't need the global lock anymore - the mspace lock will take care of it
    RELEASE_MALLOC_GLOBAL_LOCK();

    assert(mspace_overhead >= 0);
    space = GET_MSPACE(def_site_tag) + mspace_overhead;
  }

  // Need to check this to avoid a division-by-zero
  if (__builtin_expect(size > 0, TRUE)) {
    // Note that this doesn't look at previously-allocated memory in this mspace
    // (because that would be too expensive)
    if (__builtin_expect(nmemb > mspace_size / size, FALSE)) {
      DEBUG_MSG("calloc size (%lu bytes) larger than mspace size (%lu bytes)\n",
                nmemb * size, mspace_size);
      errno = ENOMEM;

      return NULL;
    }
  }

  void *mem = mspace_calloc(space, nmemb, size);
  DEBUG_MSG("mspace_calloc(%p, %lu, %lu) returned %p\n", space, nmemb, size,
            mem);
  assert(mem > space && mem < (space + mspace_size));

  return mem;
}

void *__tagged_realloc(tag_t def_site_tag, void *ptr, size_t size) {
  DEBUG_MSG("__tagged_realloc(%#x, %p, %lu) called from %p\n", def_site_tag,
            ptr, size, __builtin_return_address(0));

  mspace space;

  if (!ptr) {
    // We may be allocating a new memory region. Need to ensure that no-one else
    // can update the mapped def sites while we are doing our own mapping
    ACQUIRE_MALLOC_GLOBAL_LOCK();

    if (mapped_def_sites[def_site_tag] == FALSE) {
      space = create_fuzzalloc_mspace(def_site_tag);

      // Release the global lock - we've updated the def site map
      RELEASE_MALLOC_GLOBAL_LOCK();
    } else {
      // Don't need the global lock anymore - the mspace lock will take care of
      // it
      RELEASE_MALLOC_GLOBAL_LOCK();

      assert(mspace_overhead >= 0);
      space = GET_MSPACE(def_site_tag) + mspace_overhead;
    }
  } else {
    // We are resizing an existing memory region, so reuse the def site tag and
    // mspace of the existing pointer
    def_site_tag = GET_DEF_SITE_TAG(ptr);

    assert(mspace_overhead >= 0);
    space = GET_MSPACE(def_site_tag) + mspace_overhead;
  }

  // Note that this doesn't look at previously-allocated memory in this mspace
  // (because that would be too expensive)
  if (__builtin_expect(size > mspace_size, FALSE)) {
    DEBUG_MSG("realloc size (%lu bytes) larger than mspace size (%lu bytes)\n",
              size, mspace_size);
    errno = ENOMEM;

    return NULL;
  }

  void *mem = mspace_realloc(space, ptr, size);
  DEBUG_MSG("mspace_realloc(%p, %p, %lu) returned %p\n", space, ptr, size, mem);
  assert(mem > space && mem < (space + mspace_size));

  return mem;
}

//===-- malloc interface --------------------------------------------------===//

void *malloc(size_t size) {
  return __tagged_malloc(FUZZALLOC_DEFAULT_TAG, size);
}

void *calloc(size_t nmemb, size_t size) {
  return __tagged_calloc(FUZZALLOC_DEFAULT_TAG, nmemb, size);
}

void *realloc(void *ptr, size_t size) {
  return __tagged_realloc(FUZZALLOC_DEFAULT_TAG, ptr, size);
}

void free(void *ptr) {
  DEBUG_MSG("free(%p) called from %p\n", ptr, __builtin_return_address(0));

  if (!ptr) {
    return;
  }

  tag_t def_site_tag = GET_DEF_SITE_TAG(ptr);

  assert(mspace_overhead >= 0);
  mspace space = GET_MSPACE(def_site_tag) + mspace_overhead;

  DEBUG_MSG("mspace_free(%p, %p)\n", space, ptr);
  mspace_free(space, ptr);

  // TODO destroy mspace?
}
