/*------------------------------------------------------------------
 * JumboMem memory server: Memory allocation and deallocation
 *
 * By Scott Pakin <pakin@lanl.gov>
 *------------------------------------------------------------------*/

/*
 * Copyright (C) 2010 Los Alamos National Security, LLC
 *
 * This material was produced under U.S. Government contract
 * DE-AC52-06NA25396 for Los Alamos National Laboratory (LANL), which
 * is operated by Los Alamos National Security, LLC for the
 * U.S. Department of Energy.  The U.S. Government has rights to use,
 * reproduce, and distribute this software.  NEITHER THE GOVERNMENT
 * NOR LOS ALAMOS NATIONAL SECURITY, LLC MAKES ANY WARRANTY, EXPRESS
 * OR IMPLIED, OR ASSUMES ANY LIABILITY FOR THE USE OF THIS SOFTWARE.
 * If software is modified to produce derivative works, such modified
 * software should be clearly marked so as not to confuse it with the
 * version available from LANL.
 *
 * Additionally, this program is free software; you can redistribute
 * it and/or modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; version 2.0
 * of the License.  Accordingly, this program is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 */

#define _GNU_SOURCE
#include "jumbomem.h"
#include <malloc.h>

/* Define an opaque object for by by dlmalloc. */
typedef void *mspace;

/* The following were copied from dlmalloc.c. */
#define MAX_SIZE_T (~(size_t)0)
#define MFAIL      ((void*)(MAX_SIZE_T))

/* C++ programs may allocate memory before jm_initialize_all() has a
 * chance to run.  We therefore have to ensure we're initialized
 * before every memory operation. */
#define INITIALIZE_IF_NECESSARY()               \
  do {                                          \
    if (!jm_mspace)                             \
      jm_initialize_all();                      \
  }                                             \
  while (0)

/* Define an mspace for JumboMem's exclusive use (i.e., not visible to
 * the user's program). */
static mspace jm_mspace = NULL;

/* Keep track of some statistics. */
#ifdef JM_DEBUG
uint64_t allocs_external = 0;     /* Number of allocations by the user program */
#endif

/* Declare all of our global variables en masse. */
extern JUMBOMEM_GLOBALS jm_globals;

/* Declare all of the dlmalloc functions we use. */
extern void *dlcalloc(size_t, size_t);
extern void dlfree(void *);
extern void *dlmalloc(size_t);
extern void *dlmemalign(size_t, size_t);
extern void *dlrealloc(void *, size_t);
extern void *dlvalloc(size_t);
extern void *dlpvalloc(size_t);
extern struct mallinfo dlmallinfo(void);
extern int dlmallopt(int, int);
extern int dlmalloc_trim(size_t);
extern void dlmalloc_stats(void);
extern size_t dlmalloc_usable_size(void *);
extern size_t dlmalloc_footprint(void);
extern size_t dlmalloc_max_footprint(void);
extern void **dlindependent_calloc(size_t, size_t, void **);
extern void **dlindependent_comalloc(size_t, size_t*, void **);
extern mspace create_mspace_with_base(void *base, size_t capacity, int locked);
extern void *mspace_calloc(mspace msp, size_t n_elements, size_t elem_size);
extern void mspace_free(mspace msp, void *mem);
extern void *mspace_malloc(mspace msp, size_t bytes);
extern void *mspace_memalign(mspace msp, size_t alignment, size_t bytes);
extern void *mspace_realloc(mspace msp, void* mem, size_t newsize);
extern struct mallinfo mspace_mallinfo(mspace msp);
extern int mspace_mallopt(int, int);
extern int mspace_trim(mspace msp, size_t pad);
extern void mspace_malloc_stats(mspace msp);
extern size_t mspace_footprint(mspace msp);
extern size_t mspace_max_footprint(mspace msp);
extern void **mspace_independent_calloc(mspace msp, size_t n_elements, size_t elem_size, void *chunks[]);
extern void **mspace_independent_comalloc(mspace msp, size_t n_elements, size_t sizes[], void *chunks[]);

/* Prepare to replace hooks used by dlmalloc. */
#ifdef JM_MALLOC_HOOKS
# define MAYBE_STATIC static
# define MAYBE_CALLER , const void *caller JM_UNUSED
void (*__malloc_initialize_hook) (void) = jm_initialize_all;
#else
# define jm_internal_malloc    malloc
# define jm_internal_realloc   realloc
# define jm_internal_free      free
# define jm_internal_memalign  memalign
# define MAYBE_STATIC
# define MAYBE_CALLER
#endif


/* Call either the JumboMem-internal or JumboMem-external version of
 * free(). */
MAYBE_STATIC void
jm_internal_free (void *ptr  MAYBE_CALLER)
{
  JM_ENTER();
  jm_debug_printf(5, "%s free(%p)\n",
                  JM_INTERNAL_INVOCATION() ? "Internal" : "External", ptr);
  if (JM_INTERNAL_INVOCATION()) {
    INITIALIZE_IF_NECESSARY();
    mspace_free(jm_mspace, ptr);
  }
  else
    dlfree(ptr);
  JM_RETURN();
}


/* Call the JumboMem-internal version of malloc() without attempting
 * to acquire/release the thread mega-lock.  This is needed to
 * initialize thread-local storage. */
void *
jm_internal_malloc_no_lock (size_t size)
{
  void *result;

  INITIALIZE_IF_NECESSARY();
  jm_debug_printf(5, "Internal lock-free malloc(%lu)\n", size);
  result = mspace_malloc(jm_mspace, size);
  if ((void *)jm_globals.memregion <= result && result < (void *)jm_globals.memregion+jm_globals.extent)
    jm_abort("Internal error: Internal buffer %p is within the external range of memory", result);
  jm_debug_printf(5, "Internal lock-free malloc(%lu) ==> %p\n", size, result);
  return result;
}


/* Call either the JumboMem-internal or JumboMem-external version of
 * malloc(). */
MAYBE_STATIC void *
jm_internal_malloc (size_t size  MAYBE_CALLER)
{
  void *result;

  JM_ENTER();
  jm_debug_printf(5, "%s malloc(%lu)\n",
                  JM_INTERNAL_INVOCATION() ? "Internal" : "External", size);
  if (JM_INTERNAL_INVOCATION()) {
    INITIALIZE_IF_NECESSARY();
    result = mspace_malloc(jm_mspace, size);
    if ((void *)jm_globals.memregion <= result && result < (void *)jm_globals.memregion+jm_globals.extent)
      jm_abort("Internal error: Internal buffer %p is within the external range of memory", result);
  }
  else
    result = dlmalloc(size);
  jm_debug_printf(5, "%s malloc(%lu) ==> %p\n",
                  JM_INTERNAL_INVOCATION() ? "Internal" : "External", size, result);
  JM_RETURN(result);
}


/* Call either the JumboMem-internal or JumboMem-external version of
 * memalign(). */
MAYBE_STATIC void *
jm_internal_memalign (size_t boundary, size_t size  MAYBE_CALLER)
{
  void *result;

  JM_ENTER();
  jm_debug_printf(5, "%s memalign(%lu, %lu)\n",
                  JM_INTERNAL_INVOCATION() ? "Internal" : "External",
                  boundary, size);
  if (JM_INTERNAL_INVOCATION()) {
    INITIALIZE_IF_NECESSARY();
    result = mspace_memalign(jm_mspace, boundary, size);
    if ((void *)jm_globals.memregion <= result && result < (void *)jm_globals.memregion+jm_globals.extent)
      jm_abort("Internal error: Internal buffer %p is within the external range of memory", result);
  }
  else
    result = dlmemalign(boundary, size);
  jm_debug_printf(5, "%s memalign(%lu, %lu) ==> %p\n",
                  JM_INTERNAL_INVOCATION() ? "Internal" : "External", boundary, size, result);
  JM_RETURN(result);
}


/* Call either the JumboMem-internal or JumboMem-external version of
 * realloc(). */
MAYBE_STATIC void *
jm_internal_realloc (void *ptr, size_t size  MAYBE_CALLER)
{
  void *result;

  JM_ENTER();
  jm_debug_printf(5, "%s realloc(%p, %lu)\n",
                  JM_INTERNAL_INVOCATION() ? "Internal" : "External",
                  ptr, size);
  if (JM_INTERNAL_INVOCATION()) {
    INITIALIZE_IF_NECESSARY();
    result = mspace_realloc(jm_mspace, ptr, size);
    if ((void *)jm_globals.memregion <= result && result < (void *)jm_globals.memregion+jm_globals.extent)
      jm_abort("Internal error: Internal buffer %p is within the external range of memory", result);
  }
  else
    result = dlrealloc(ptr, size);
  jm_debug_printf(5, "%s realloc(%p, %lu) ==> %p\n",
                  JM_INTERNAL_INVOCATION() ? "Internal" : "External", ptr, size, result);
  JM_RETURN(result);
}

/* ---------------------------------------------------------------------- */

/* This is obnoxious: Some glibc functions don't call hook functions.
 * We therefore have to trap those functions and express them in terms
 * of other functions that do invoke hooks. */
#ifdef JM_MALLOC_HOOKS

void *
valloc (size_t size)
{
  JM_ENTER();
  jm_debug_printf(5, "%s valloc(%lu) -- replacing with memalign(%ld, %lu)\n",
                  JM_INTERNAL_INVOCATION() ? "Internal" : "External", size,
                  jm_globals.ospagesize, size);
  JM_RETURN(memalign(jm_globals.ospagesize, size));
}


void *
pvalloc (size_t size)
{
  size_t rounded_size = jm_globals.ospagesize*((size+jm_globals.ospagesize-1)/jm_globals.ospagesize);

  JM_ENTER();
  jm_debug_printf(5, "%s pvalloc(%lu) -- replacing with memalign(%ld, %lu)\n",
                  JM_INTERNAL_INVOCATION() ? "Internal" : "External", size,
                  jm_globals.ospagesize, rounded_size);
  JM_RETURN(memalign(jm_globals.ospagesize, rounded_size));
}


/* For now we'll ignore the remaining glibc public functions:
 *
 *   - independent_calloc
 *   - independent_comalloc
 *   - cfree
 *   - malloc_trim
 *   - malloc_usable_size
 *   - malloc_stats
 *   - mallinfo
 *   - mallopt
 */

/* Matches #ifdef JM_MALLOC_HOOKS. */
#endif

/* ---------------------------------------------------------------------- */

/* No other user-callable functions are defined here if we're simply
 * replacing the memory-allocation hooks. */
#ifndef JM_MALLOC_HOOKS


/* Call either the JumboMem-internal or JumboMem-external version of
 * calloc(). */
void *
calloc (size_t nmemb, size_t size)
{
  JM_ENTER();
  jm_debug_printf(5, "%s calloc(%lu, %lu)\n",
                  JM_INTERNAL_INVOCATION() ? "Internal" : "External", nmemb, size);
  if (JM_INTERNAL_INVOCATION()) {
    INITIALIZE_IF_NECESSARY();
    JM_RETURN(mspace_calloc(jm_mspace, nmemb, size));
  }
  else
    JM_RETURN(dlcalloc(nmemb, size));
}


/* Call either the JumboMem-internal or JumboMem-external version of
 * valloc(). */
void *
valloc (size_t size)
{
  JM_ENTER();
  jm_debug_printf(5, "%s valloc(%lu)\n",
                  JM_INTERNAL_INVOCATION() ? "Internal" : "External", size);
  if (JM_INTERNAL_INVOCATION()) {
    /* The current version of dlmalloc (2.8.3) doesn't define
     * mspace_valloc(). */
    INITIALIZE_IF_NECESSARY();
    JM_RETURN(mspace_memalign(jm_mspace, jm_globals.ospagesize, size));
  }
  else
    JM_RETURN(dlvalloc(size));
}


/* Call either the JumboMem-internal or JumboMem-external version of
 * pvalloc(). */
void *
pvalloc (size_t size)
{
  JM_ENTER();
  jm_debug_printf(5, "%s pvalloc(%lu)\n",
                  JM_INTERNAL_INVOCATION() ? "Internal" : "External", size);
  if (JM_INTERNAL_INVOCATION()) {
    /* The current version of dlmalloc (2.8.3) doesn't define
     * mspace_valloc(). */
    INITIALIZE_IF_NECESSARY();
    JM_RETURN(mspace_memalign(jm_mspace, jm_globals.ospagesize, jm_globals.ospagesize*((size+jm_globals.ospagesize-1)/jm_globals.ospagesize)));
  }
  else
    JM_RETURN(dlpvalloc(size));
}


/* Call either the JumboMem-internal or JumboMem-external version of
 * mallinfo(). */
struct mallinfo
mallinfo (void)
{
  JM_ENTER();
  jm_debug_printf(5, "%s mallinfo()\n",
                  JM_INTERNAL_INVOCATION() ? "Internal" : "External");
  if (JM_INTERNAL_INVOCATION()) {
    INITIALIZE_IF_NECESSARY();
    JM_RETURN(mspace_mallinfo(jm_mspace));
  }
  else
    JM_RETURN(dlmallinfo());
}


/* Call either the JumboMem-internal or JumboMem-external version of
 * mallopt(). */
int
mallopt (int param_number, int value)
{
  JM_ENTER();
  jm_debug_printf(5, "%s mallopt(%d, %d)\n",
                  JM_INTERNAL_INVOCATION() ? "Internal" : "External",
                  param_number, value);
  if (JM_INTERNAL_INVOCATION()) {
    /* Note that in the current version of dlmalloc (2.8.3) the
     * mspace_mallopt() does not take an mspace argument. */
    INITIALIZE_IF_NECESSARY();
    JM_RETURN(mspace_mallopt(param_number, value));
  }
  else
    JM_RETURN(dlmallopt(param_number, value));
}


/* Call either the JumboMem-internal or JumboMem-external version of
 * malloc_trim(). */
int
malloc_trim (size_t pad)
{
  JM_ENTER();
  jm_debug_printf(5, "%s malloc_trim(%lu)\n",
                  JM_INTERNAL_INVOCATION() ? "Internal" : "External", pad);
  if (JM_INTERNAL_INVOCATION()) {
    /* Note that in the current version of dlmalloc (2.8.3) the
     * function is called mspace_trim(), not mspace_malloc_trim(). */
    INITIALIZE_IF_NECESSARY();
    JM_RETURN(mspace_trim(jm_mspace, pad));
  }
  else
    JM_RETURN(dlmalloc_trim(pad));
}


/* Call either the JumboMem-internal or JumboMem-external version of
 * malloc_stats(). */
void
malloc_stats (void)
{
  JM_ENTER();
  jm_debug_printf(5, "%s malloc_stats()\n",
                  JM_INTERNAL_INVOCATION() ? "Internal" : "External");
  if (JM_INTERNAL_INVOCATION()) {
    INITIALIZE_IF_NECESSARY();
    mspace_malloc_stats(jm_mspace);
  }
  else
    dlmalloc_stats();
}


/* Call either the JumboMem-internal or JumboMem-external version of
 * malloc_usable_size(). */
size_t
malloc_usable_size (void *mem)
{
  JM_ENTER();
  jm_debug_printf(5, "%s malloc_usable_size(%p)\n",
                  JM_INTERNAL_INVOCATION() ? "Internal" : "External", mem);
  if (JM_INTERNAL_INVOCATION()) {
    /* The current version of dlmalloc (2.8.3) doesn't define an
     * mspace_malloc_usable_size() function.  After a cursory glance
     * at dlmalloc.c it seems like it may be safe to use
     * dlmalloc_usable_size() instead. */
    INITIALIZE_IF_NECESSARY();
    JM_RETURN(dlmalloc_usable_size(mem));
  }
  else
    JM_RETURN(dlmalloc_usable_size(mem));
}


/* Call either the JumboMem-internal or JumboMem-external version of
 * malloc_footprint(). */
size_t
malloc_footprint (void)
{
  JM_ENTER();
  jm_debug_printf(5, "%s malloc_footprint()\n",
                  JM_INTERNAL_INVOCATION() ? "Internal" : "External");
  if (JM_INTERNAL_INVOCATION()) {
    /* Note that in the current version of dlmalloc (2.8.3) the
     * function is called mspace_footprint(), not
     * mspace_malloc_footprint(). */
    INITIALIZE_IF_NECESSARY();
    JM_RETURN(mspace_footprint(jm_mspace));
  }
  else
    JM_RETURN(dlmalloc_footprint());
}


/* Call either the JumboMem-internal or JumboMem-external version of
 * malloc_max_footprint(). */
size_t
malloc_max_footprint (void)
{
  JM_ENTER();
  jm_debug_printf(5, "%s malloc_max_footprint()\n",
                  JM_INTERNAL_INVOCATION() ? "Internal" : "External");
  if (JM_INTERNAL_INVOCATION()) {
    /* Note that in the current version of dlmalloc (2.8.3) the
     * function is called mspace_max_footprint(), not
     * mspace_malloc_max_footprint(). */
    INITIALIZE_IF_NECESSARY();
    JM_RETURN(mspace_max_footprint(jm_mspace));
  }
  else
    JM_RETURN(dlmalloc_max_footprint());
}


/* Call either the JumboMem-internal or JumboMem-external version of
 * independent_calloc(). */
void **
independent_calloc (size_t nmemb, size_t size, void *chunks[])
{
  JM_ENTER();
  jm_debug_printf(5, "%s independent_calloc(%lu, %lu, %p)\n",
                  JM_INTERNAL_INVOCATION() ? "Internal" : "External",
                  nmemb, size, chunks);
  if (JM_INTERNAL_INVOCATION()) {
    INITIALIZE_IF_NECESSARY();
    JM_RETURN(mspace_independent_calloc(jm_mspace, nmemb, size, chunks));
  }
  else
    JM_RETURN(dlindependent_calloc(nmemb, size, chunks));
}


/* Call either the JumboMem-internal or JumboMem-external version of
 * independent_comalloc(). */
void **
independent_comalloc (size_t nmemb, size_t sizes[], void *chunks[])
{
  JM_ENTER();
  jm_debug_printf(5, "%s independent_comalloc(%lu, %p, %p)\n",
                  JM_INTERNAL_INVOCATION() ? "Internal" : "External",
                  nmemb, sizes, chunks);
  if (JM_INTERNAL_INVOCATION()) {
    INITIALIZE_IF_NECESSARY();
    JM_RETURN(mspace_independent_comalloc(jm_mspace, nmemb, sizes, chunks));
  }
  else
    JM_RETURN(dlindependent_comalloc(nmemb, sizes, chunks));
}

/* Matches #ifndef JM_MALLOC_HOOKS. */
#endif

/* ---------------------------------------------------------------------- */

/* Return 1 if the memory-management subsystem is safe to use, 0 otherwise. */
int
jm_memory_is_initialized (void)
{
  return jm_mspace==NULL ? 0 : 1;
}


/* Return the amount of memory currently being used internally to JumboMem. */
size_t
jm_internal_memory_footprint (void)
{
  size_t internal_footprint;

  JM_ENTER();
  internal_footprint = mspace_footprint(jm_mspace);
  JM_RETURN(internal_footprint);
}


/* Redefine mmap() to prevent programs from mapping memory into the
 * middle of JumboMem's controlled region. */
void *
jm_mmap (void *start, size_t length, int prot, int flags, int fd, off_t offset)
{
  static void *(*original_mmap)(void *start, size_t length, int prot, int flags, int fd, off_t offset) = NULL;

  /* The first time we're called, acquire a pointer to libc's mmap(). */
  JM_ENTER();
  if (!original_mmap) {
#ifdef RTLD_NEXT
    original_mmap = (void *(*)(void *, size_t, int, int, int, off_t)) dlsym(RTLD_NEXT, "mmap");
#else
    original_mmap = mmap;
    jm_debug_printf(2, "WARNING: JumboMem is unable to intercept mmap() calls; programs that use mmap() may crash JumboMem.\n");
#endif
    if (!original_mmap)
      jm_abort("Failed to find mmap() (%s)", dlerror());
  }

  /* Prevent allocations to our memory region. */
  if (start || JM_INTERNAL_INVOCATION())
    /* Do nothing special if we were given a starting address or if
     * mmap() was called from within JumboMem. */
    JM_RETURN((*original_mmap)(start, length, prot, flags, fd, offset));
  else {
    void *address;      /* Mapped address returned by mmap() */
    void *dataend;      /* End of the user's data segment */

    /* If space is available, try to allocate memory *below* the
     * JumboMem-controlled memory region. */
    dataend = (void *) ((((uintptr_t)sbrk(0) - 1) / jm_globals.ospagesize + 1) * jm_globals.ospagesize);
    if (dataend+length < (void *)jm_globals.memregion) {
      /* Increment the end of the data segment so we don't try to
       * mmap() the same addresses on a subsequent call. */
      if (brk(dataend+length) == 0) {
	address = (*original_mmap)(dataend, length, prot, flags|MAP_FIXED, fd, offset);
	if (!(address == MAP_FAILED
	      || ((void *)jm_globals.memregion <= address && address < (void *)jm_globals.memregion+jm_globals.extent))) {
	  JM_RETURN(address);
	}
	jm_debug_printf(4, "Failed to mmap() memory at address %p; retrying elsewhere\n", dataend);
      }
      else
        jm_debug_printf(4, "Failed to set the end of the data segment to %p with brk() (%s); retrying mmap() elsewhere\n",
			dataend+length, jm_strerror(errno));
    }

    /* Specify a desired map address lying just past the
     * JumboMem-controlled memory region.  Abort if mmap() returns an
     * address within the JumboMem-controlled memory region. */
    address = (*original_mmap)((void *)(jm_globals.memregion+jm_globals.extent),
                               length, prot, flags, fd, offset);
    if (address == MAP_FAILED)
      jm_abort("mmap() failed to allocate %lu bytes at or above address %p (%s)",
               length, jm_globals.memregion+jm_globals.extent, jm_strerror(errno));
    if ((void *)jm_globals.memregion <= address && address < (void *)jm_globals.memregion+jm_globals.extent)
      jm_abort("Failed to prevent mmap() from allocating %lu bytes within [%p, %p]",
               length, jm_globals.memregion, jm_globals.memregion+jm_globals.extent);
    JM_RETURN(address);
  }
}


/* Allocate more address space (called from the dl*() routines). */
void *
jm_morecore (long int increment)
{
  char *prev_endaddress;     /* Ending address before we increment it */

  /* Keep track of the number of allocations and the number of bytes
   * allocated. */
  JM_ENTER();
#ifdef JM_DEBUG
  if (increment > 0)
    allocs_external++;
#endif

  /* Return memory we had previously mmap()'ed.  Fail if we didn't
   * mmap() enough memory. */
  if (jm_globals.endaddress+increment < jm_globals.memregion
      || jm_globals.endaddress+increment > jm_globals.memregion+jm_globals.extent
      || increment < 0) {
    jm_debug_printf(3, "Failed to allocate %ld bytes of JumboMem memory.\n", increment);
    JM_RETURN(MFAIL);
  }
  prev_endaddress = jm_globals.endaddress;
  jm_globals.endaddress += increment;
  if (increment > 1024)
    jm_debug_printf(3, "Allocated %ld bytes (%sB) of JumboMem memory at address %p for a total of %sB.\n",
                    increment,
                    jm_format_power_of_2((uint64_t)increment, 1),
                    prev_endaddress,
                    jm_format_power_of_2((uint64_t)jm_globals.endaddress
                                         -(uint64_t)jm_globals.memregion,
                                         1));
  else if (increment != 0)
    jm_debug_printf(3, "Allocated %ld bytes of JumboMem memory at address %p for a total of %sB.\n",
                    increment, prev_endaddress,
                    jm_format_power_of_2((uint64_t)jm_globals.endaddress
                                         -(uint64_t)jm_globals.memregion,
                                         1));
  JM_RETURN(prev_endaddress);
}


/* Initialize the memory-allocation routines. */
void
jm_initialize_memory (void)
{
  /* Statically allocate the initial space for a dlmalloc mspace so as
   * to avoid recursive jm_mmap()/jm_initialize_memory() loops. */
  static size_t initial_mspace_block[1024];

  JM_ENTER();
  if (!(jm_mspace=create_mspace_with_base((void *)initial_mspace_block,
                                          1024*sizeof(size_t), 0)))
    jm_abort("Failed to create a dlmalloc mspace");
#ifdef JM_MALLOC_HOOKS
  __malloc_hook = jm_internal_malloc;
  __realloc_hook = jm_internal_realloc;
  __free_hook = jm_internal_free;
  __memalign_hook = jm_internal_memalign;
#endif

  /* malloc() calls mmap() which calls dlsym() which calls malloc().
   * By invoking malloc() now we can ensure (at least, make it likely)
   * that dlsym() has enough memory in the initial mspace block to
   * initialize mmap(). */
  jm_free(jm_malloc(16));
  JM_RETURN();
}


/* Finalize the memory-allocation routines. */
void
jm_finalize_memory (void)
{
  /* Report our memory usage on a successful exit. */
  JM_ENTER();
#ifdef JM_DEBUG
  if (jm_globals.debuglevel >= 2 && !jm_globals.error_exit) {
    size_t bytes_external = dlmalloc_max_footprint();  /* Bytes allocated by the user program */
    size_t bytes_internal = mspace_max_footprint(jm_mspace);  /* Bytes allocated by JumboMem */

    if (bytes_internal < 1024)
      jm_debug_printf(2, "JumboMem and its libraries allocated a total of %lu bytes for the master task.\n",
                      bytes_internal);
    else
      jm_debug_printf(2, "JumboMem and its libraries allocated a total of %lu bytes (%sB) for the master task.\n",
                      bytes_internal, jm_format_power_of_2(bytes_internal, 1));
    if (bytes_external < 1024)
      jm_debug_printf(2, "The user program allocated a total of %lu bytes in %lu calls to morecore().\n",
                      bytes_external, allocs_external);
    else
      jm_debug_printf(2, "The user program allocated a total of %lu bytes (%sB) in %lu calls to morecore().\n",
                      bytes_external, jm_format_power_of_2(bytes_external, 1), allocs_external);
    if (jm_globals.extent)
      jm_debug_printf(2, "Address space utilization: %.1f%% of %sB\n",
                      (100.0*bytes_external)/jm_globals.extent,
                      jm_format_power_of_2((uint64_t)jm_globals.extent, 1));
    else
      jm_debug_printf(2, "Address space utilization: 0.0%% of 0.0B\n");
  }
#endif
  JM_RETURN();
}
