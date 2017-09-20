/*------------------------------------------------------------------
 * JumboMem memory server: Initialization and finalization code
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

#include "jumbomem.h"

/* Use GCC constructor and destructor attributes if available. */
#if __GNUC__ >= 3 && !defined(JM_STATICLIB)
# define CONSTRUCT_ATTR __attribute__((constructor))
# define DESTRUCT_ATTR __attribute__((destructor))
#else
# define CONSTRUCT_ATTR
# define DESTRUCT_ATTR
#endif

/* Declare all of our global variables en masse. */
JUMBOMEM_GLOBALS jm_globals;

/* Define some file-local variables. */
static unsigned long local_pages;       /* JM_LOCAL_PAGES after reducing our memory usage */
#ifdef JM_DEBUG
static struct rusage initial_usage;     /* Memory usage when entering the main loop */
#endif


/* Try to convince the operating system to relinquish memory from the
 * buffer cache and other kernel stashes so that JumboMem can allocate
 * it.  The heuristic we use is to repeatedly allocate all of the free
 * memory in the system and then release it. */
static void
grab_memory (void)
{
  const int numiters = 3;  /* Number of times to allocate all of free memory */
  char *buffer[numiters];
  int i;

   for (i=0; i<numiters; i++) {
     size_t bytesavail = jm_get_available_memory_size();
     size_t j;

     buffer[i] = (char *) malloc(bytesavail);
     if (buffer[i])
       for (j=0; j<bytesavail; j+=jm_globals.ospagesize)
         buffer[i][j] = 0;
   }
   for (i=0; i<numiters; i++)
     if (buffer[i])
       free(buffer[i]);
}


/* Given the size of available memory, return the number of pages we
 * should allocate locally. */
static unsigned long
compute_local_page_count (size_t masterbytes)
{
  unsigned long max_local_pages;   /* Maximum number of pages we can cache locally */
  unsigned long max_mappings;      /* Maximum number of mappings a process can handle */
  unsigned long local_pages;       /* Page count to return */

  /* Compute the maximum number of pages to cache locally as the
   * minimum of the number of pages we can fit in memory and the
   * number of noncontiguous page mappings supported by the operating
   * system. */
  max_local_pages = masterbytes / jm_globals.pagesize;
  max_mappings = jm_get_maximum_map_count();
  if (max_mappings > 0 && max_local_pages >= max_mappings*2)
    max_local_pages = max_mappings*2 - 1;

  /* Cache the maximum number of pages unless the user instructed us
   * to do otherwise. */
  local_pages = (unsigned long) jm_getenv_nonnegative_int_or_percent("JM_LOCAL_PAGES", max_local_pages);
  if (local_pages == (unsigned long)(-1))
    /* The user didn't specify anything; use the maximum available. */
    local_pages = max_local_pages;

  /* Sanity-check the number of local pages we wound up with. */
  if (max_mappings > 0 && local_pages >= max_mappings*2)
    jm_debug_printf(2, "WARNING: %lu local pages were requested but only %lu noncontiguous page mappings are available.\n",
                    local_pages, max_mappings);
  else if (local_pages > max_local_pages)
    jm_debug_printf(2, "WARNING: %lu local pages were requested but only %lu pages seem to be available.\n",
                    local_pages, max_local_pages, max_local_pages);
  if (local_pages*jm_globals.pagesize > jm_globals.extent) {
    size_t new_local_pages = jm_globals.extent / jm_globals.pagesize;
    jm_debug_printf(3, "Cache size exceeds global address-space size; reducing local page count from %lu to %lu.\n",
                    local_pages, new_local_pages);
    local_pages = new_local_pages;
  }
  return local_pages;
}


/* Reduce jm_globals.local_pages by the number of pages that fault
 * when touching each page. */
static void
reduce_master_memory (void)
{
  struct rusage usage0, usage1;    /* Before and after resource usage */
  unsigned long newfaults;         /* Newly observed major page faults */
  size_t cached_bytes;             /* Number of bytes locally cached */
  char *buffer;                    /* Buffer for testing memory allocation */
  size_t orig_local_pages;         /* Original value of jm_globals.local_pages */
  size_t i;

  /* Ensure we can even allocate all of our pages before we try to
   * map them. */
  orig_local_pages = jm_globals.local_pages;
  do {
    cached_bytes = jm_globals.pagesize*jm_globals.local_pages;
    buffer = (char *) valloc(cached_bytes);
    if (!buffer) {
      jm_debug_printf(4, "Failed to allocate %ld bytes of memory (%s).\n",
                      cached_bytes, jm_strerror(errno));
      jm_globals.local_pages--;
    }
  }
  while (!buffer && jm_globals.local_pages >= 1);
  if (!buffer)
    /* Produce an error message and abort. */
    buffer = (char *) jm_valloc(cached_bytes);
  if (buffer)
    free(buffer);
  if (jm_globals.local_pages != orig_local_pages)
    jm_debug_printf(3, "Failed to allocate %lu pages; reducing local pages to %lu.\n",
                    orig_local_pages, jm_globals.local_pages);

  /* Temporarily map into memory as many pages as possible. */
  jm_debug_printf(3, "Determining if locally caching %lu pages (%sB) leads to major page faults...\n",
                  jm_globals.local_pages, jm_format_power_of_2(cached_bytes, 1));
  jm_assign_backing_store(jm_globals.memregion, cached_bytes, PROT_READ|PROT_WRITE);

  /* Touch every OS page once to load every page into memory. */
  for (i=0; i<cached_bytes; i+=jm_globals.ospagesize)
    jm_globals.memregion[i] = 0;

  /* "Evict" and "fetch" all locally cached JumboMem pages in hopes
   * of convincing the communication subsystem to allocate all of
   * its memory up front. */
  if (jm_globals.extra_memcpy) {
    char *comm_buffer = (char *) jm_malloc(jm_globals.pagesize);
    for (i=0; i<cached_bytes; i+=jm_globals.pagesize)
      jm_fetch_end(jm_fetch_begin(&jm_globals.memregion[i], comm_buffer));
    for (i=0; i<cached_bytes; i+=jm_globals.pagesize)
      jm_evict_end(jm_evict_begin(&jm_globals.memregion[i], comm_buffer));
    jm_free(comm_buffer);
  }
  else {
    for (i=0; i<cached_bytes; i+=jm_globals.pagesize)
      jm_fetch_end(jm_fetch_begin(&jm_globals.memregion[i], &jm_globals.memregion[i]));
    for (i=0; i<cached_bytes; i+=jm_globals.pagesize)
      jm_evict_end(jm_evict_begin(&jm_globals.memregion[i], &jm_globals.memregion[i]));
  }

  /* Touch every OS page again to determine how many pages actually fit
   * into memory. */
  getrusage(RUSAGE_SELF, &usage0);
  for (i=0; i<cached_bytes; i+=jm_globals.ospagesize)
    jm_globals.memregion[i] = 0;
  getrusage(RUSAGE_SELF, &usage1);
  newfaults = usage1.ru_majflt - usage0.ru_majflt;

  /* Unmap all pages from memory. */
  jm_remove_backing_store(jm_globals.memregion, cached_bytes);

  /* Reduce jm_globals.local_pages based on the number of page faults. */
  if (newfaults) {
    unsigned long new_page_count;   /* Reduced number of locally cacheable pages */

    jm_debug_printf(3, "The master observed %ld major page faults on %lu bytes of memory.\n",
                    newfaults, cached_bytes);
    new_page_count = jm_globals.local_pages - (newfaults*jm_globals.ospagesize+jm_globals.pagesize-1)/jm_globals.pagesize;
    jm_debug_printf(2, "Reducing the number of locally cached pages from %lu to %lu.\n",
                    jm_globals.local_pages, new_page_count);
    jm_globals.local_pages = new_page_count;
  }
  else
    jm_debug_printf(3, "No major page faults were observed.\n");
}


#ifdef JM_DEBUG
static void
additional_diagnostics (void)
{
  if (jm_globals.debuglevel >= 1) {
    extern char **environ;   /* The program's environment */
    char **envvar;           /* A single key=value string */
    int foundvar = 0;        /* 1=found at least one JM_* variable */

    jm_debug_printf(1, "JumboMem environment variables encountered:\n");
    for (envvar=environ; *envvar; envvar++)
      if (!strncmp(*envvar, "JM_", 3) && strncmp(*envvar, "JM_EXPECTED_RANK=", 17)) {
        jm_debug_printf(1, "   %s\n", *envvar);
        foundvar = 1;
      }
    if (!foundvar)
      jm_debug_printf(1, "   [none]\n");
    jm_debug_printf(2, "Global memory size: %lu bytes (%sB)\n",
                    jm_globals.extent,
                    jm_format_power_of_2((uint64_t)jm_globals.extent, 1));
    jm_debug_printf(2, "Prefetching is %s.\n",
                    jm_globals.prefetch_type==PREFETCH_NONE ? "disabled" : "enabled");
    jm_debug_printf(2, "Asynchronous eviction is %s.\n",
                    jm_globals.async_evict ? "enabled" : "disabled");
    jm_debug_printf(2, "Copy in/copy out is %s.\n",
                    jm_globals.extra_memcpy ? "enabled" : "disabled");
    jm_debug_printf(2, "JumboMem page size: %ld bytes; OS page size: %d bytes\n",
                    jm_globals.pagesize, jm_globals.ospagesize);
    jm_debug_printf(2, "Using %u slaves.\n", jm_globals.numslaves);
#ifdef JM_DIST_BLOCK
    jm_debug_printf(2, "Pages are distributed to slaves in block fashion.\n");
#else
    jm_debug_printf(2, "Pages are distributed to slaves in round-robin fashion.\n");
#endif
#ifdef JM_MALLOC_HOOKS
    jm_debug_printf(2, "malloc() hooks are enabled.\n");
#else
    jm_debug_printf(2, "malloc() hooks are disabled.\n");
#endif
  }
}
#endif


/* Determine a range of addresses that define our global address
 * space.  Set jm_globals.memregion and jm_globals.endaddress
 * accordingly.  We assume that jm_globals.extent is already properly
 * defined. */
static void
locate_global_address_space (void)
{
  void *startaddr;          /* Starting address to request */
  int retries_allowed = 1;  /* 1=retry with an arbitrary address on failure; 0=abort on failure */

  /* Determine the address at which JumboMem should allocate its
   * memory region.  By default, we allocate it just beyond the end of
   * the data segment.  The hope is that this may enable some
   * not-quite 64-bit clean programs to fit their key data structures
   * beneath the 4GB boundary, where they may happen to work.  If the
   * user specified a specific address or increment from the default,
   * honor that request and abort if we can't. */
  startaddr = (void *) ((((uintptr_t)sbrk(0)-1)/jm_globals.pagesize + 1) * jm_globals.pagesize);
  do {
    char *baseaddrstr;   /* User-specified base address or increment as a string */
    long int baseaddr;   /* Numeric equivalent of the above */
    char *endptr;        /* Pointer to the character past the parsed integer */

    /* Use the default address if JM_BASEADDR is not specified. */
    baseaddrstr = getenv("JM_BASEADDR");
    if (!baseaddrstr)
      break;

    /* Parse JM_BASEADDR as a signed integer. */
    retries_allowed = 0;
    while (isspace(*baseaddrstr))
      baseaddrstr++;
    baseaddr = strtol(baseaddrstr, &endptr, 0);
    if (*endptr != '\0')
      jm_abort("JM_BASEADDR requires an integer value (was \"%s\")", getenv("JM_BASEADDR"));

    /* Process JM_BASEADDR differently based on its initial character. */
    if (*baseaddrstr == '+' || *baseaddrstr == '-')
      /* JM_BASEADDR begins with "+" or "-" -- increment startaddr by
       * that amount.  ("-" will probably always cause mmap() to fail,
       * but that's the user's problem.) */ {
      jm_debug_printf(4, "Attempting to allocate address space at location %p + %p = %p\n",
		      startaddr, baseaddr, (void *) ((uintptr_t)startaddr + baseaddr));
      startaddr = (void *) ((uintptr_t)startaddr + baseaddr);
    }
    else
      /* JM_BASEADDR begins with another character -- treat it as an
       * absolute address. */
      startaddr = (void *) (uintptr_t) baseaddr;
  }
  while (0);

  /* Try to allocate our address space just beyond the end of our data
   * segment.  The hope is that this may enable some not-quite 64-bit
   * clean programs to fit their key data structures beneath the 4GB
   * boundary, where they may happen to work. */
  if ((jm_globals.memregion=mmap(startaddr, jm_globals.pagesize, PROT_NONE,
                                 MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,
                                 0, 0)) == MAP_FAILED) {
    /* Okay, that didn't work.  Try again if we're allowed to. */
    if (retries_allowed) {
      /* Let the operating system allocate address space wherever it
       * wants to. */
      jm_debug_printf(4, "Failed to map %ld bytes of address space at address %p (%s); trying elsewhere\n",
		      jm_globals.pagesize, startaddr, jm_strerror(errno));
      if ((jm_globals.memregion=mmap(NULL, jm_globals.pagesize,
				     PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS,
				     0, 0)) == MAP_FAILED)
	jm_abort("Failed to map %ld bytes of address space (%s)",
		 jm_globals.pagesize, jm_strerror(errno));
    }
    else
      /* We're not allowed to retry the operation so we have to abort
       * JumboMem. */
      jm_abort("Failed to map %ld bytes of address space at address %p (%s)",
	       jm_globals.pagesize, startaddr, jm_strerror(errno));
  }

  /* Store and report information about our address range. */
  if ((uintptr_t)jm_globals.memregion % jm_globals.pagesize != 0)
    jm_globals.memregion += jm_globals.pagesize - ((uintptr_t)jm_globals.memregion % jm_globals.pagesize);
  jm_globals.endaddress = jm_globals.memregion;
  jm_debug_printf(3, "Global address space = [%p, %p].\n",
                  jm_globals.memregion, jm_globals.memregion+jm_globals.extent);
}

/* ---------------------------------------------------------------------- */

/* Initialize all of JumboMem. */
void CONSTRUCT_ATTR
jm_initialize_all (void)
{
  char *prefetch_string;           /* String describing the prefetch type */
  size_t slavebytes;               /* Rounded per-slave memory size */
  size_t masterbytes;              /* Maximum number of bytes we can cache locally */
  static int already_called = 0;   /* 0=first invocation; 1=further invocation */

  /* Do nothing if we were already initialized by one of the functions
   * in allocate.c or if we're currently initializing and were invoked
   * recursively by one of the functions in allocate.c. */
  JM_ENTER();
  if (already_called)
    JM_RETURN();
  already_called = 1;

  /* Prevent fork() and friends from fighting for our JumboMem memory. */
  unsetenv("LD_PRELOAD");

  /* Start malloc() et al. running. */
  jm_initialize_overrides();
  jm_initialize_memory();

  /* Announce that we've started doing something. */
#ifdef JM_DEBUG
  memset((void *)&initial_usage, 0, sizeof(struct rusage));
#endif
  memset((void *)&jm_globals, 0, sizeof(JUMBOMEM_GLOBALS));
  jm_globals.progname = "jumbomem";
  jm_globals.debuglevel = jm_getenv_nonnegative_int("JM_DEBUG");
  if (jm_globals.debuglevel >= 1) {
    /* Ideally, only rank 0 should output the initialization message. */
    char *rankstr = getenv("JM_EXPECTED_RANK");  /* Computed by the wrapper script */
    if (!rankstr || atoi(rankstr) == 0)
      jm_debug_printf(1, "JumboMem is initializing.\n");
  }

  /* Determine the (logical) page size to use. */
  jm_globals.ospagesize = jm_get_page_size();
  if ((jm_globals.pagesize=jm_getenv_positive_int("JM_PAGESIZE"))) {
    if (jm_globals.pagesize % jm_globals.ospagesize != 0)
      jm_abort("JM_PAGESIZE must be a multiple of the OS page size (%ld bytes)", jm_globals.ospagesize);
  }
  else if (!(jm_globals.pagesize=jm_get_minimum_jm_page_size())) {
    jm_debug_printf(2, "WARNING: JumboMem is unable to determine the minimum page size; setting JM_PAGESIZE is strongly recommended.\n");
    jm_globals.pagesize = jm_globals.ospagesize;
  }

  /* Determine if we should prefetch new pages, asynchronously evict
   * old pages, and/or copy data in and out of message buffers. */
  prefetch_string = getenv("JM_PREFETCH");
  if (!prefetch_string)
    jm_globals.prefetch_type = PREFETCH_NONE;
  else {
    typedef struct {
      JUMBOMEM_PREFETCH  prefetch_type;    /* Symbolic prefetch type */
      const char        *prefetch_string;  /* Textual prefetch type */
    } PREFETCH_ARG;
    PREFETCH_ARG prefetches[] = {
      {PREFETCH_NONE,  "none"},
      {PREFETCH_NEXT,  "next"},
      {PREFETCH_DELTA, "delta"}};
    int i;

    for (i=sizeof(prefetches)/sizeof(PREFETCH_ARG)-1; i>=0; i--)
      if (!strcmp(prefetch_string, prefetches[i].prefetch_string)) {
        jm_globals.prefetch_type = prefetches[i].prefetch_type;
        break;
      }
    if (i < 0)
      jm_abort("Unrecognized value \"%s\" for JM_PREFETCH", prefetch_string);
  }
  if ((jm_globals.async_evict=jm_getenv_boolean("JM_ASYNCEVICT")) == -1)
    jm_globals.async_evict = 0;
  if ((jm_globals.extra_memcpy=jm_getenv_boolean("JM_MEMCPY")) == -1)
    jm_globals.extra_memcpy = 0;

  /* Spawn a bunch of slaves. */
  grab_memory();
  if (!(jm_globals.slavebytes=jm_getenv_positive_int("JM_SLAVEMEM")))
    jm_globals.slavebytes = jm_get_available_memory_size();
  slavebytes = jm_globals.slavebytes;
  jm_initialize_slaves();

  /* Disable JumboMem if no slaves were provided. */
  if (jm_globals.numslaves < 1) {
    jm_debug_printf(1, "JumboMem requires at least one slave; allocating all memory locally.\n");
    jm_globals.extent = slavebytes;
    locate_global_address_space();
    jm_assign_backing_store(jm_globals.memregion, jm_globals.extent, PROT_READ|PROT_WRITE);
    jm_debug_printf(2, "Locally allocated %lu bytes (%sB) of memory.\n",
                    jm_globals.extent,
                    jm_format_power_of_2((uint64_t)jm_globals.extent, 1));
    JM_RETURN();
  }

  /* Round down per-slave memory usage to the nearest full JumboMem page. */
  slavebytes = (jm_globals.slavebytes/jm_globals.pagesize) * jm_globals.pagesize;
  if (jm_globals.slavebytes != slavebytes) {
    jm_debug_printf(3, "Rounding down slave memory from %lu bytes to %lu*%lu=%lu bytes.\n",
                    jm_globals.slavebytes, slavebytes/jm_globals.pagesize,
                    jm_globals.pagesize, slavebytes);
    jm_globals.slavebytes = slavebytes;
  }

  /* Allocate global address space. */
  jm_globals.extent = jm_globals.slavebytes * jm_globals.numslaves;
  jm_debug_printf(3, "%lu bytes/slave * %u slaves = %lu total bytes (%sB).\n",
                  jm_globals.slavebytes, jm_globals.numslaves, jm_globals.extent,
                  jm_format_power_of_2((uint64_t)jm_globals.extent, 1));
  locate_global_address_space();

  /* Start running the page-replacement algorithm. */
  if (!(masterbytes=jm_getenv_positive_int("JM_MASTERMEM")))
    masterbytes = jm_get_available_memory_size();
  jm_debug_printf(3, "The master can use at most %lu bytes of memory.\n", masterbytes);
  jm_globals.local_pages = compute_local_page_count(masterbytes);
  if (jm_getenv_boolean("JM_REDUCEMEM") == 1 && !getenv("JM_LOCAL_PAGES"))
    reduce_master_memory();
  local_pages = jm_globals.local_pages;   /* The page-replacement code might alter the number of locally cached pages. */
  jm_initialize_pagereplace();

  /* Output some additional diagnostics. */
#ifdef JM_DEBUG
  additional_diagnostics();
#endif

  /* Install a signal handler for segmentation faults within our
   * managed memory region. */
  jm_initialize_signal_handler();

  /* Begin using the global address space. */
#ifdef JM_DEBUG
  if (jm_globals.debuglevel >= 2)
    getrusage(RUSAGE_SELF, &initial_usage);
#endif
  jm_debug_printf(2, "JumboMem is running.\n");
  JM_RETURN();
}


/* Finalize all of JumboMem. */
DESTRUCT_ATTR
void
jm_finalize_all (void)
{
  static int finalizing = 0;     /* 1=we're in the process of finalizing; 0=first call */

  /* Avoid recursive finalization. */
  JM_ENTER();
  if (finalizing == 0) {
    finalizing = 1;

    /* On a normal exit alert the user if we saw any major page faults. */
#ifdef JM_DEBUG
    if (jm_globals.debuglevel >= 2 && !jm_globals.error_exit) {
      struct rusage usage;     /* Information to report about our memory usage */

      getrusage(RUSAGE_SELF, &usage);
      jm_debug_printf(2, "The master task is terminating with %ld major faults, %ld minor faults, and %ld swaps.\n",
                      usage.ru_majflt - initial_usage.ru_majflt,
                      usage.ru_minflt - initial_usage.ru_minflt,
                      usage.ru_nswap  - initial_usage.ru_nswap);
      if (jm_getenv_boolean("JM_REDUCEMEM") == 1)
        jm_debug_printf(2, "Result of JM_REDUCEMEM=%s: JM_LOCAL_PAGES=%lu JM_SLAVEMEM=%lu\n",
                        getenv("JM_REDUCEMEM"), local_pages, jm_globals.slavebytes);
    }
#endif

    /* Tell all of our modules to shut down cleanly. */
    jm_finalize_signal_handler();
    jm_finalize_pagereplace();
    jm_finalize_memory();
    jm_finalize_slaves();       /* Does not necessarily return. */

    /* Announce that the program is exiting. */
    jm_debug_printf(1, "JumboMem is %s.\n",
                    jm_globals.error_exit
                    ? "terminating with an error status"
                    : "exiting normally");
  }
  JM_RETURN();
}


/* Use the older _init() and _fini() functions to initialize the
 * library if we can't declare GCC constructors and destructors. */
#if __GNUC__ < 3 && !defined(JM_STATICLIB)
void
_init(void)
{
  jm_initialize_all();
}

void
_fini(void)
{
  jm_finalize_all();
}
#endif
