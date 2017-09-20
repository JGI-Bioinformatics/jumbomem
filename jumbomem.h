/*------------------------------------------------------------
 * JumboMem memory server: Common header file
 *
 * By Scott Pakin <pakin@lanl.gov>
 *------------------------------------------------------------*/

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
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <limits.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/resource.h>

/* Specify that a function argument is expected to be unused. */
#ifdef __GNUC__
# define JM_UNUSED __attribute__((unused))
#else
# define JM_UNUSED
#endif

/* Define macros for converting a global address to a page number,
 * slave number, and slave byte offset. */
#define GET_PAGE_NUMBER(ADDR) ((uintptr_t)((ADDR)-jm_globals.memregion)/jm_globals.pagesize)
#ifdef JM_DIST_BLOCK
/* Distribute pages among slaves in a block fashion (i.e., fill one
 * slave's memory before using any of the next slave's memory). */
# define GET_SLAVE_NUM(ADDR) ((uintptr_t)((ADDR)-jm_globals.memregion)/jm_globals.slavebytes)
# define GET_SLAVE_OFFSET(ADDR) ((uintptr_t)((ADDR)-jm_globals.memregion)%jm_globals.slavebytes)
#else
/* Distribute pages among slaves in a round-robin fashion (i.e.,
 * adjacent pages go to adjacent slaves). */
# define GET_SLAVE_NUM(ADDR) (GET_PAGE_NUMBER(ADDR)%jm_globals.numslaves)
# define GET_SLAVE_OFFSET(ADDR) ((GET_PAGE_NUMBER(ADDR)/jm_globals.numslaves)*jm_globals.pagesize)
#endif

/* Define macros for normalizing a size_t's byte order in a
 * heterogeneous system.  Currently, the follow macro block is
 * somewhat GNU-specific. */
#ifdef JM_HETEROGENEOUS
  /* Heterogeneous case */
# include <netinet/in.h>
# if !defined(__BYTE_ORDER) || !defined(__BIG_ENDIAN)
#  error Unable to compare __BYTE_ORDER to __BIG_ENDIAN; is this a GNU system?
# endif
# if __BYTE_ORDER == __BIG_ENDIAN
   /* Big endian side of a heterogeneous system -- no need to do anything */
#  define TO_NETWORK(x)   (x)
#  define FROM_NETWORK(x) (x)
# else
  /* Little endian side of a heterogeneous system -- must swap bytes */
#  ifdef ntohll
    /* Maybe someday GNU will define these. */
#   define TO_NETWORK(x)   (htonll(x))
#   define FROM_NETWORK(x) (ntohll(x))
#  else
    /* For now, we can use bswap_64() from byteswap.h to swap bytes. */
#   include <byteswap.h>
#   define TO_NETWORK(x)   (bswap_64(x))
#   define FROM_NETWORK(x) (bswap_64(x))
#  endif
# endif
#else
  /* Homogeneous case */
# define TO_NETWORK(x)   (x)
# define FROM_NETWORK(x) (x)
#endif

/* Define macros for recording that we've entered or exited a JumboMem
 * function. */
#define JM_ENTER() jm_enter_critical_section()
#define JM_RETURN(RETVAL)                       \
  do {                                          \
    jm_exit_critical_section();                 \
    return RETVAL;                              \
  }                                             \
  while (0)
#define JM_INTERNAL_INVOCATION() (jm_globals.is_internal || jm_get_internal_depth() > 1)

/* Define MAP_POPULATE if not defined in sys/mman.h. */
#ifndef MAP_POPULATE
# define MAP_POPULATE 0
#endif

/* Encapsulate the arguments to pthread_create(). */
typedef struct {
  void *(*start_routine)(void *);      /* Initial thread function */
  void *arg;                           /* Argument to the above */
  void *threadstack;                   /* Stack for the thread to use; NULL=user-allocated */
} PTHREAD_CREATE_ARGS;

/* We can use one of the following techniques to determine the next
 * page to prefetch. */
typedef enum {
  PREFETCH_NONE,           /* Don't prefetch any pages. */
  PREFETCH_NEXT,           /* Always prefetch the (static) next page. */
  PREFETCH_DELTA           /* Prefetch the same page distance as previously. */
} JUMBOMEM_PREFETCH;

/* Put all of our global variables in a single structure to avoid
 * namespace pollution. */
typedef struct {
  size_t  pagesize;        /* JumboMem logical page size */
  size_t  ospagesize;      /* Operating system page size */
  char   *memregion;       /* Entire memory region under our control */
  char   *endaddress;      /* Pointer past last word of memregion passed to dlmalloc */
  size_t  extent;          /* Total number of bytes in memregion */
  unsigned int numslaves;  /* Number of slave processes */
  size_t  slavebytes;      /* Number of bytes managed by each slave */
  unsigned long local_pages;   /* Number of JumboMem pages we can cache at the master */
  char   *progname;        /* Name of this program (argv[0]) */
  JUMBOMEM_PREFETCH prefetch_type;  /* Prefetching technique to utilize */
  int     async_evict;     /* 0=evict pages synchronously; 1=asynchronously */
  int     extra_memcpy;    /* 0=send/receive directly; 1=copy data in and out of message buffers */
  int     debuglevel;      /* Debug level (larger = more verbose output) */
  int     is_internal;     /* 0=within either JumboMem or user code; >0=definitely within JumboMem */
  int     error_exit;      /* 0=normal termination; 1=jm_abort() was called */
  volatile uint64_t dummy; /* Dummy variable for preventing compiler optimizations */
#ifdef JM_PROFILE_SIZE
  uint64_t timings[JM_PROFILE_SIZE];      /* Readings of the cycle counter */
  const char *timedesc[JM_PROFILE_SIZE];  /* Description of each reading */
  volatile int numtimings;                /* Number of entries in the above */
#endif
} JUMBOMEM_GLOBALS;
extern JUMBOMEM_GLOBALS jm_globals;

/* Read the cycle counter and associate a textual description with it. */
#if defined(JM_PROFILE_SIZE) && defined(__GNUC__)
  /* The following definition is x86-64 (and gcc) specific.  Do not
   * define JM_PROFILE_SIZE unless you're targeting an x86-64 CPU. */
# define JM_RECORD_CYCLE(DESC)                                            \
  if (jm_globals.numtimings < JM_PROFILE_SIZE) {                          \
    uint32_t lo, hi;                                                      \
    asm volatile("rdtsc" : "=a" (lo), "=d" (hi));                         \
    jm_globals.timings[jm_globals.numtimings] = ((uint64_t)hi <<32) | lo; \
    jm_globals.timedesc[jm_globals.numtimings] = DESC;                    \
    jm_globals.numtimings++;                                              \
  }
#else
# define JM_RECORD_CYCLE(DESC)
#endif


/* Initialize/finalize all of JumboMem. */
extern void jm_initialize_all(void);
extern void jm_finalize_all(void);

/* Initialize various JumboMem modules. */
extern void jm_initialize_memory(void);
extern void jm_initialize_overrides(void);
extern void jm_initialize_pagereplace(void);
extern void jm_initialize_signal_handler(void);
extern void jm_initialize_slaves(void);

/* Finalize various JumboMem modules. */
extern void jm_finalize_memory(void);
extern void jm_finalize_pagereplace(void);
extern void jm_finalize_signal_handler(void);
extern void jm_finalize_slaves(void);

/* Asynchronously fetch a page from a slave or evict a page to a slave. */
extern void *jm_fetch_begin(char *fetch_addr, char *fetch_page);
extern void jm_fetch_end(void *opaque_state);
extern void *jm_evict_begin(char *evict_addr, char *evict_page);
extern void jm_evict_end(void *opaque_state);

/* Say whether a page is already resident and, if so, what protections
 * it should have (always read/write). */
extern int jm_page_is_resident(char *rounded_addr, int *protflags);

/* Given the address of a page that faulted, return the address of a
 * page to evict and the initial protection of the new page. */
extern void jm_find_replacement_page (char *faulted_page, int *newprot, char **evictable_page, int *clean);

/* Output an error message and abort the program. */
extern void jm_abort(const char *format, ...);

/* Parse an environment variable into a positive integer.  Return 0 if
 * the environment variable is not defined.  Fail if the environment
 * variable is defined but is not a positive integer. */
extern size_t jm_getenv_positive_int(const char *envvar);

/* Parse an environment variable into a nonnegative integer.  Return
 * -1 if the environment variable is not defined.  Fail if the
 * environment variable is defined but is not a nonnegative
 * integer. */
extern ssize_t jm_getenv_nonnegative_int(const char *envvar);

/* Parse an environment variable into a nonnegative integer or a
 * percentage of a given number.  Return -1 if the environment
 * variable is not defined.  Fail if the environment variable is
 * defined but is neither a nonnegative integer nor a valid
 * percentage. */
extern ssize_t jm_getenv_nonnegative_int_or_percent(const char *envvar, ssize_t base_amount);

/* Parse an environment variable into a 0 (false) or 1 (true).  Return
 * -1 if the environment variable is not defined.  Fail if the
 * environment variable is defined but is not a valid boolean
 * value. */
extern int jm_getenv_boolean(const char *envvar);

/* Allocate and free memory using the original malloc(), valloc(),
 * realloc(), and free() calls. */
extern void *jm_malloc(size_t size);
extern void *jm_valloc(size_t size);
extern void *jm_realloc(void *ptr, size_t size);
extern void jm_free(void *ptr);
extern void *jm_internal_malloc_no_lock(size_t size);

/* Lock/unlock addresses into/from RAM, but only if JM_MLOCK is true. */
extern int jm_mlock(const void *addr, size_t len);
extern int jm_munlock(const void *addr, size_t len);

/* Output a message to stderr if the debug level is at least the given
 * level.  If JM_DEBUG is set, miscfuncs.c defines a jm_debug_printf()
 * function.  If not, we define jm_debug_printf() here to be an empty
 * macro. */
#ifdef JM_DEBUG
extern void jm_debug_printf_internal(char *filename, int lineno, int level,
                                     const char *format, ...);
# define jm_debug_printf(...) jm_debug_printf_internal(__FILE__, __LINE__, __VA_ARGS__)
#else
# define jm_debug_printf(...)
#endif

/* Return the hostname as a static string. */
extern char *jm_hostname(void);

/* Read a microsecond timer and return its current value. */
extern uint64_t jm_current_time(void);

/* Convert an error number to a string without relying on malloc()'ed
 * memory. */
extern const char *jm_strerror(int errnum);

/* Format a number as a string with a power-of-two suffix and "digits"
 * digits after the decimal point.  This function returns a pointer to
 * a static string.  However, for the convenience of calling
 * jm_format_power_of_2() multiple times from a single printf(), the
 * function in fact cycles through a set of static strings. */
extern char *jm_format_power_of_2(uint64_t number, int digits);

/* Assign or remove memory backing store. */
extern void jm_assign_backing_store(char *baseaddr, size_t numbytes, int protflags);
extern void jm_remove_backing_store(char *baseaddr, size_t numbytes);

/* Touch a range of addresses to fault them into the local cache.
 * This function should not be called while the fault handler is
 * active. */
extern void jm_touch_memory_region(const char *baseaddr, size_t numbytes);

/* Return 1 if the memory-management subsystem is safe to use, 0 otherwise. */
extern int jm_memory_is_initialized(void);

/* Serialize accesses to a critical section of code. */
extern void jm_enter_critical_section(void);

/* Allow other threads to enter a critical section. */
extern void jm_exit_critical_section(void);

/* Return the current call depth of the mega-lock. */
extern unsigned int jm_get_internal_depth(void);

/* Set the current call depth of the mega-lock (used by jm_abort()). */
extern void jm_set_internal_depth(unsigned int newdepth);

/* Return 1 if we should exit the signal handler immediately. */
extern int jm_must_exit_signal_handler_now(void);

/* Instruct all other threads to freeze execution and wait until
 * they're all frozen before returning. */
extern void jm_freeze_other_threads(void);

/* Initialize the current thread then invoke the user's initializer.
 * The caller is responsible for allocating memory for arg but we will
 * free it before we return. */
extern void *jm_thread_start_routine(void *arg);

/* Return a page table with a given number of bytes of data per entry
 * (plus sizeof(uint32_t) bytes of key data).  Reduce the number of
 * locally cached pages (jm_globals.local_pages) to accomodate the
 * page table. */
extern void *jm_create_page_table(size_t valuebytes);

/* Insert a page into the page table.  The caller must ensure that
 * this function is called no more than jm_globals.local_pages
 * times. */
extern void jm_page_table_insert(void *pt_obj, char *address, void *extradata);

/* Delete a page from the page table. */
extern void jm_page_table_delete(void *pt_obj, char *address);

/* Return a pointer to a page's payload data or NULL if the page isn't
 * resident. */
extern void *jm_page_table_find(void *pt_obj, char *address);

/* Map an index in [0, num_used-1] to a page number and the associated
 * payload.  Abort if the index is out of range. */
extern void jm_page_table_offset(void *pt_obj, uint32_t index, uint32_t *pagenum, void **extradata);

/* Free all of the data in a page table. */
extern void jm_page_table_free(void *pt_obj);

/* Return the caller's thread (LWP) ID.  This is defined by JumboMem
 * if not provided by the C library. */
extern pid_t gettid(void);

/* Given a thread (LWP) ID, return the thread's current run state or
 * '?' if the state could not be determined. */
extern char jm_get_thread_state(pid_t tid);

/* Parse the kernel meminfo file to return the amount of free memory
 * we can use. */
extern size_t jm_get_available_memory_size(void);

/* Return the maximum number of mappings available to a process or
 * zero if indeterminate. */
extern unsigned long jm_get_maximum_map_count(void);

/* Return the minimum page size that's safe for JumboMem to use (i.e.,
 * that can't cause mmap() to run out of mappings).  Return 0 if we're
 * unable to determine the minimum page size. */
extern size_t jm_get_minimum_jm_page_size(void);

/* Return the physical page size. */
extern size_t jm_get_page_size(void);

/* Redefine mmap() to prevent programs from mapping memory into the
 * middle of JumboMem's controlled region. */
extern void *jm_mmap (void *start, size_t length, int prot, int flags, int fd, off_t offset);
