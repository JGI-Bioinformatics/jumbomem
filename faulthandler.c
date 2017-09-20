/*------------------------------------------------------------
 * JumboMem memory server: Signal handler for page faults
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

#include "jumbomem.h"

/* A page delta of more than MAX_PAGE_DELTA is considered unpredictable. */
#ifndef MAX_PAGE_DELTA
# define MAX_PAGE_DELTA 4
#endif

/* Import all of our shared global variables */
extern JUMBOMEM_GLOBALS jm_globals;

/* Define a few more global variables to share. */
struct sigaction jm_prev_segfaulter;  /* Previous SIGSEGV handler information */
struct sigaction jm_prev_prev_segfaulter;  /* Two SIGSEGV handler informations ago */

#ifdef RTLD_NEXT
extern int (*jm_original_sigaction)(int, const struct sigaction *, struct sigaction *);
#endif
static inline void evict_end (void);

/* Define a set of page buffers for pending fetches, evictions, and
 * prefetches. */
typedef struct {
  char *address;      /* Global address to which the operation refers (NULL = no operation is pending) */
  void *state;        /* Opaque state corresponding to the operation */
  char *buffer;       /* A page-sized buffer to copy data in and out of */
  union {
    int   clean;      /* 0=page is dirty; 1=clean (evictions only) */
    int   protflags;  /* Protection flags to use once a page is fetched (fetches only) */
  } extra;
} ASYNC_INFO;

static ASYNC_INFO fetch_info;
static ASYNC_INFO evict_info;
static ASYNC_INFO prefetch_info;

/* Define various statistics to keep track of if debugging is enabled. */
#ifdef JM_DEBUG
static unsigned long min_pagefaults = 0;  /* Number of minor page faults encountered */
static unsigned long maj_pagefaults = 0;  /* Number of major page faults encountered */
static uint64_t total_fault_time = 0;     /* Total time in microseconds spent in the fault handler. */
static uint64_t min_fault_time = (uint64_t)(~0); /* Min. time in microseconds spent in the fault handler. */
static uint64_t max_fault_time = 0;       /* Max. time in microseconds spent in the fault handler. */
static unsigned long good_prefetches = 0; /* Number of prefetches whose data we used */
static unsigned long bad_prefetches = 0;  /* Number of prefetches whose data we discarded */
static unsigned long pages_sent = 0;      /* Number of pages sent to slaves */
static unsigned long pages_received = 0;  /* Number of pages received from slaves */
static unsigned long clean_evictions = 0; /* Number of pages evicted without communication */
static unsigned long page_deltas[MAX_PAGE_DELTA*2+1];   /* Tallies of deltas between faulted pages */
static unsigned long predictable_deltas = 0;   /* Number of deltas that matched the previous delta */
static unsigned long unpredictable_deltas = 0; /* Number of deltas that differed from the previous delta */
static uint64_t heartbeat_interval = (uint64_t)(-1);   /* Interval in seconds at which to output status (-1=never) */
static uint64_t first_heartbeat;          /* Time in seconds of first heartbeat */
static uint64_t last_heartbeat;           /* Time in seconds of last heartbeat */
static struct rusage usage0;              /* Resource usage at initialization time */
#endif


/* Fetch into a static buffer if extra_memcpy is set.  If extra_memcpy
 * is not set, fetch directly into the global memory region. */
static inline void
fetch_begin (char *address, int protflags)
{
  fetch_info.address = address;
  fetch_info.extra.protflags = protflags;
  fetch_info.state = jm_fetch_begin(address,
                                    jm_globals.extra_memcpy ? fetch_info.buffer : address);
}

static inline void
fetch_end (void)
{
  jm_fetch_end(fetch_info.state);
  if (jm_globals.extra_memcpy)
    memcpy((void *)fetch_info.address, (void *)fetch_info.buffer, jm_globals.pagesize);
  if (fetch_info.extra.protflags != (PROT_READ|PROT_WRITE)) {
    jm_debug_printf(4, "Changing the permissions of page %p to 0x%08X.\n",
                    fetch_info.address, fetch_info.extra.protflags);
    if (mprotect((void *)fetch_info.address, jm_globals.pagesize, fetch_info.extra.protflags) == -1)
      jm_abort("Failed to set access permissions on page %p (%s)",
               fetch_info.address, jm_strerror(errno));
  }
  fetch_info.address = NULL;
#ifdef JM_DEBUG
  pages_received++;
#endif
}


/* Evict from a static buffer if extra_memcpy is set.  If extra_memcpy
 * is not set, evict directly from the global memory region. */
static inline void
evict_begin (char *address, int clean)
{
  evict_info.address = address;
  evict_info.extra.clean = clean;
  if (!clean) {
    if (jm_globals.extra_memcpy) {
      memcpy((void *)evict_info.buffer, (void *)address, jm_globals.pagesize);
      evict_info.state = jm_evict_begin(address, evict_info.buffer);
    }
    else
      evict_info.state = jm_evict_begin(address, address);
  }
  if (jm_globals.async_evict) {
    /* If we're evicting asynchronously we need to revoke write access
     * to the page to avoid dropping data that gets written while the
     * page is being evicted. */
    if (mprotect(address, jm_globals.pagesize, PROT_READ) == -1)
      jm_abort("Failed to revoke write access to page %p: %s",
               address, jm_strerror(errno));
  }
  else
    /* If we're evicting synchronously, perform the evict_end()
     * immediately. */
    evict_end();
}

static inline void
evict_end (void)
{
  if (!evict_info.extra.clean)
    jm_evict_end(evict_info.state);
  jm_remove_backing_store(evict_info.address, jm_globals.pagesize);
  evict_info.address = NULL;
#ifdef JM_DEBUG
  if (evict_info.extra.clean)
    clean_evictions++;
  else
    pages_sent++;
#endif
}


/* Prefetches always involve a memcpy() because the target isn't yet
 * mapped.  Also, the calling code manually sets and clears
 * prefetch_info.address. */
static inline void
prefetch_begin (char *fetch_addr, char *fetch_page)
{
  prefetch_info.state = jm_fetch_begin(fetch_addr, fetch_page);
}

static inline void
prefetch_end (void)
{
  jm_fetch_end(prefetch_info.state);
#ifdef JM_DEBUG
  pages_received++;
#endif
}


/* Prefetch the page after a given page. */
static void
start_prefetch (char *rounded_addr)
{
  size_t pagesize = jm_globals.pagesize;  /* Cache of the JumboMem page size */

  /* Select a candidate page to prefetch. */
  switch (jm_globals.prefetch_type) {
    /* Prefetch the page following the one that faulted. */
    case PREFETCH_NEXT:
      prefetch_info.address = rounded_addr + pagesize;
      break;

    /* Prefetch the page following the one that faulted. */
    case PREFETCH_DELTA:
      {
        static char *prev_fault_addr = NULL;   /* Previously faulted address */

        prefetch_info.address = rounded_addr + (rounded_addr-prev_fault_addr);
        prev_fault_addr = rounded_addr;
      }
      break;

    /* We should never get here. */
    default:
      jm_abort("Internal error: Unknown prefetch type %d\n", jm_globals.prefetch_type);
      break;
  }

  /* Cancel the prefetch if the address is invalid or the page is
   * already resident. */
  if (prefetch_info.address < jm_globals.memregion
      || prefetch_info.address >= jm_globals.memregion+jm_globals.extent
      || jm_page_is_resident(prefetch_info.address, NULL))
    prefetch_info.address = NULL;
  else
    prefetch_begin(prefetch_info.address, prefetch_info.buffer);
}


/* Complete and process a pending prefetch.  Attempt to overlap an
 * eviction with the prefetch. */
static void
complete_prefetch (char *rounded_addr, int protflags, char *evictable_page, int clean)
{
  /* See if we prefetched the current page. */
  if (prefetch_info.address) {
    /* We prefetched *something*.  Was it the page we wanted? */
    prefetch_end();
    if (prefetch_info.address == rounded_addr) {
      /* Yes!  Evict an old page and copy in the prefetched page. */
      if (evictable_page)
        evict_begin(evictable_page, clean);
      memcpy((void *)rounded_addr, (void *)prefetch_info.buffer, jm_globals.pagesize);
#ifdef JM_DEBUG
      good_prefetches++;
#endif

      /* Set the final permissions on the prefetched page. */
      if (protflags != (PROT_READ|PROT_WRITE)) {
        jm_debug_printf(4, "Changing the permissions of prefetched page %p to 0x%08X.\n",
                        rounded_addr, protflags);
        if (mprotect((void *)rounded_addr, jm_globals.pagesize, protflags) == -1)
          jm_abort("Failed to set access permissions on page %p (%s)",
                   rounded_addr, jm_strerror(errno));
      }
    }
    else {
      /* No.  Evict an old page, discard the prefetched page and fetch
       * the correct new page from a remote server. */
      fetch_begin(rounded_addr, protflags);
      if (evictable_page)
        evict_begin(evictable_page, clean);
      fetch_end();
#ifdef JM_DEBUG
      bad_prefetches++;
#endif
    }
  }
  else {
    /* We had no prefetches pending -- evict an old page and fetch the
     * new page from a remote server. */
    fetch_begin(rounded_addr, protflags);
    if (evictable_page)
      evict_begin(evictable_page, clean);
    fetch_end();
  }
}

/* ---------------------------------------------------------------------- */

/* Convert segmentation faults to remote paging operations. */
void
jm_signal_handler (int signum, siginfo_t *siginfo, void *notused JM_UNUSED)
{
  size_t pagesize;       /* Cache of the JumboMem page size */
  char *rounded_addr;    /* Faulted address rounded down to a page boundary */
  char *evictable_page;  /* Page to evict from memory */
  int   clean;           /* 0=evictable page is dirty; 1=clean */
  int   protflags;       /* Protection flags for mmap() or mprotect() */
  static char *fault_address = NULL;  /* Address that faulted */
#ifdef JM_DEBUG
  uint64_t starttime;    /* Time at which we began replacing pages */
  uint64_t stoptime;     /* Time at which we finished replacing pages */
#endif

  /* When multiple threads enter the signal handler simultaneously, we
   * need to ensure that only one thread services the fault. */
  JM_ENTER();
  if (jm_must_exit_signal_handler_now())
    JM_RETURN();

  /* Determine if we need to get involved. */
  JM_RECORD_CYCLE("Entered the fault handler");
  pagesize = jm_globals.pagesize;
  rounded_addr = (char *) (pagesize*((uintptr_t)siginfo->si_addr/pagesize));
  if (rounded_addr < jm_globals.memregion
      || rounded_addr >= jm_globals.memregion+jm_globals.extent) {
    /* This must be a "real" segmentation fault. */
    jm_debug_printf(4, "Unknown address %p faulted.\n", siginfo->si_addr);
#ifdef RTLD_NEXT
    if (jm_original_sigaction(signum, &jm_prev_segfaulter, NULL) == -1)
      jm_abort("Failed to restore the SIGSEGV handler (%s)", jm_strerror(errno));
#else
    if (sigaction(signum, &jm_prev_segfaulter, NULL) == -1)
      jm_abort("Failed to restore the SIGSEGV handler (%s)", jm_strerror(errno));
#endif
    JM_RETURN();
  }

  /* We do need to get involved.  However, we abort if we're already
   * involved in servicing another fault. */
  if (fault_address)
    jm_abort("Faulted on address %p while processing the fault on address %p",
             siginfo->si_addr, fault_address);
  fault_address = siginfo->si_addr;
  jm_debug_printf(4, "Address %p faulted.\n", siginfo->si_addr);

  /* Freeze all other threads before doing any page operations.
   * (Although no other thread can be in this critical section, we
   * need to ensure that no other thread can access a page whose data
   * has not yet arrived.) */
  jm_freeze_other_threads();

  /* If the page is already resident, change the permissions and
   * return.  Note that we don't maintain timing statistics for
   * permission alterations. */
  if (jm_page_is_resident(rounded_addr, &protflags)) {
    if (mprotect(rounded_addr, pagesize, protflags) == -1)
      jm_abort("Failed to set the protection flags for the page at address %p",
               rounded_addr);
#ifdef JM_DEBUG
    min_pagefaults++;
#endif
    fault_address = NULL;
    JM_RETURN();
  }

  /* Keep track of the number of page faults and the time we spent
   * processing them. */
#ifdef JM_DEBUG
  maj_pagefaults++;
  starttime = jm_current_time();
#endif

  /* Wait for the previous eviction (if any) to complete. */
  if (evict_info.address)
    evict_end();

  /* Evict one page and bring in another. */
  JM_RECORD_CYCLE("Finding a replacement page");
  jm_find_replacement_page(rounded_addr, &protflags, &evictable_page, &clean);
  JM_RECORD_CYCLE("Found a replacement page");
  jm_assign_backing_store(rounded_addr, pagesize, PROT_READ|PROT_WRITE);
  if (jm_globals.prefetch_type != PREFETCH_NONE) {
    /* Prefetching is enabled -- see if we've already prefetched the
     * page and fetch it if we haven't.  In either case, prefetch the
     * next page. */
    complete_prefetch(rounded_addr, protflags, evictable_page, clean);
    start_prefetch(rounded_addr);
  }
  else {
    /* Prefetching is disabled -- fetch the page from a remote server. */
    JM_RECORD_CYCLE("Fetching a replacement page");
    fetch_begin(rounded_addr, protflags);
    if (evictable_page) {
      JM_RECORD_CYCLE("Evicting an old page");
      evict_begin(evictable_page, clean);
      JM_RECORD_CYCLE("Evicted an old page");
    }
    fetch_end();
    JM_RECORD_CYCLE("Fetched a replacement page");
  }

  /* Maintain statistics of the time spent processing faults. */
#ifdef JM_DEBUG
  stoptime = jm_current_time();
  total_fault_time += stoptime - starttime;
  if (min_fault_time > stoptime - starttime)
    min_fault_time = stoptime - starttime;
  if (max_fault_time < stoptime - starttime)
    max_fault_time = stoptime - starttime;
#endif

  /* Output heartbeat information if requested. */
#ifdef JM_DEBUG
  if (stoptime/1000000-last_heartbeat > heartbeat_interval) {
    struct rusage usage1;

    last_heartbeat = stoptime/1000000;
    getrusage(RUSAGE_SELF, &usage1);
    jm_debug_printf(1, "Major faults after %lu seconds: %lu OS, %lu JumboMem\n",
                    last_heartbeat - first_heartbeat,
                    usage1.ru_majflt - usage0.ru_majflt,
                    maj_pagefaults);
  }
#endif

  /* Keep track of how predictable the page faults are. */
#ifdef JM_DEBUG
  {
    static char *prev_fault_addr = NULL;   /* Previously faulted address */
    long int delta;                        /* Delta in pages to the current fault */
    static long int prev_delta = 0;        /* Previous delta value */

    delta = (rounded_addr-prev_fault_addr) / pagesize;
    if (-MAX_PAGE_DELTA <= delta && delta <= MAX_PAGE_DELTA)
      page_deltas[delta+MAX_PAGE_DELTA]++; /* Predictable */
    else
      page_deltas[MAX_PAGE_DELTA]++;       /* Unpredictable */
    if (delta == prev_delta)
      predictable_deltas++;
    else
      unpredictable_deltas++;
    prev_fault_addr = rounded_addr;
    prev_delta = delta;
  }
#endif

  /* Exit the fault handler. */
  fault_address = NULL;
  JM_RECORD_CYCLE("Exiting the fault handler");
  JM_RETURN();
}


/* Initialize the signal handler. */
void
jm_initialize_signal_handler (void)
{
  struct sigaction segfaulter;     /* Installation information for our SIGSEGV handler */
  unsigned long pagesize = jm_globals.pagesize;   /* Cache of the JumboMem page size */
  size_t localbytes = jm_globals.local_pages*pagesize;  /* Number of bytes we can cache locally */
  unsigned long i;

  /* Initialize the fault-predictability statistics and the heartbeat
   * counter. */
#ifdef JM_DEBUG
  for (i=0; i<MAX_PAGE_DELTA*2+1; i++)
    page_deltas[i] = 0;
  heartbeat_interval = (uint64_t) jm_getenv_nonnegative_int("JM_HEARTBEAT");
  first_heartbeat = last_heartbeat = jm_current_time() / 1000000;
  getrusage(RUSAGE_SELF, &usage0);
#endif

  /* Allocate memory for various page copies. */
  if (jm_globals.prefetch_type != PREFETCH_NONE)
    prefetch_info.buffer = (char *) jm_valloc(pagesize);
  if (jm_globals.extra_memcpy) {
    evict_info.buffer = (char *) jm_valloc(pagesize);
    fetch_info.buffer = (char *) jm_valloc(pagesize);
  }
  prefetch_info.address = NULL;
  evict_info.address = NULL;
  fetch_info.address = NULL;

  /* Initialize (without talking to the slaves) as many pages as we
   * can cache locally. */
  for (i=0; i<localbytes; i+=pagesize) {
    int protflags;          /* Initial page-protection flags */
    char *evictable_page;   /* Page to evict (had better be NULL here) */
    int clean;              /* 1=evictable page is clean; 0=dirty */

    jm_find_replacement_page(&jm_globals.memregion[i], &protflags, &evictable_page, &clean);
    if (evictable_page)
      jm_abort("The page at address %p was evicted prematurely\n");
    if (i == 0)
      /* Map the entire region at once. */
      jm_assign_backing_store(jm_globals.memregion, localbytes, protflags);
  }

  /* Install a signal handler for segmentation faults within our
   * managed memory region. */
  memset((void *)&segfaulter, 0, sizeof(struct sigaction));
  segfaulter.sa_sigaction = jm_signal_handler;
  segfaulter.sa_flags = SA_RESTART|SA_SIGINFO|SA_NODEFER;
  if (sigaction(SIGSEGV, &segfaulter, &jm_prev_segfaulter) == -1)
    jm_abort("Failed to install a SIGSEGV handler (%s)", jm_strerror(errno));
  jm_prev_prev_segfaulter = jm_prev_segfaulter;
}


/* Complete any pending operations and restore the original SIGSEGV handler. */
void
jm_finalize_signal_handler (void)
{
  if (jm_globals.prefetch_type != PREFETCH_NONE && prefetch_info.address)
    prefetch_end();
  if (evict_info.address)
    evict_end();
  if (fetch_info.address)
    fetch_end();

  /* Report some final statistics on a successful exit. */
#ifdef JM_DEBUG
  if (!jm_globals.error_exit) {
    int i;

    jm_debug_printf(2, "Total number of JumboMem page faults: %lu major, %lu minor\n",
                    maj_pagefaults, min_pagefaults);
    if (maj_pagefaults > 0)
      jm_debug_printf(2, "JumboMem major-fault handling time (min/mean/max usecs): %llu %llu %llu\n",
                      min_fault_time, total_fault_time/maj_pagefaults, max_fault_time);
    if (total_fault_time > 0)
      jm_debug_printf(2, "Mean JumboMem major-fault handling rate: %.1f MB/s\n",
                      1e6*jm_globals.pagesize*(pages_sent+pages_received)/(total_fault_time*1048576.0));
    if (jm_globals.prefetch_type != PREFETCH_NONE)
      jm_debug_printf(2, "Useful prefetches: %lu; wasted prefetches: %lu\n",
                      good_prefetches, bad_prefetches);
    jm_debug_printf(2, "Evictions of clean pages: %lu; evictions of dirty pages: %lu\n",
                    clean_evictions, pages_sent);
    jm_debug_printf(2, "Total communication: %lu pages sent and %lu pages received\n",
                    pages_sent, pages_received);
    jm_debug_printf(2, "Fault deltas:\n");
    jm_debug_printf(2, "   +/- 1 page:  %lu faults\n",
                    page_deltas[MAX_PAGE_DELTA+1] + page_deltas[MAX_PAGE_DELTA-1]);
    for (i=2; i<=MAX_PAGE_DELTA; i++)
      jm_debug_printf(2, "   +/- %d pages: %lu faults\n",
                      i, page_deltas[MAX_PAGE_DELTA+i] + page_deltas[MAX_PAGE_DELTA-i]);
    jm_debug_printf(2, "   +/- other:   %lu faults\n",
                    page_deltas[MAX_PAGE_DELTA]);
    if (predictable_deltas + unpredictable_deltas != 0)
      jm_debug_printf(2, "Trivially predictable fault deltas: %.1f%%\n",
                      100.0*predictable_deltas/(double)(predictable_deltas+unpredictable_deltas));
  }
#endif

#ifdef RTLD_NEXT
  if (jm_original_sigaction(SIGSEGV, &jm_prev_segfaulter, NULL) == -1)
    jm_abort("Failed to restore the SIGSEGV handler (%s)", jm_strerror(errno));
#endif
}
