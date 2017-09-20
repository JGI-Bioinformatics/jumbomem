/*-----------------------------------------------------------------
 * JumboMem memory server: Not-recently-evicted page replacement
 *
 * By Scott Pakin <pakin@lanl.gov>
 *-----------------------------------------------------------------*/

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
#include <time.h>

/* Define a few big prime numbers for scaling random integers in case
 * RAND_MAX is small. */
#define BIGPRIME1 34359738641LL
#define BIGPRIME2 1152921504606847229LL

/* Define default parameters for the eviction queue. */
#ifndef DEFAULT_EVICT_COUNT
# define DEFAULT_EVICT_COUNT 32
#endif
#ifndef DEFAULT_RETRY_COUNT
# define DEFAULT_RETRY_COUNT 5
#endif

extern JUMBOMEM_GLOBALS jm_globals;     /* All of our global variables */
static void *page_table;                /* Set of pages in use */
static unsigned long num_used;          /* Number of valid entries in the above */
static unsigned long total_pages;       /* Size of the physical address space in pages */
static uint32_t *evicted_pages;         /* Queue of recently evicted pages */
static unsigned long evict_head = 0;    /* Pointer to the head of evicted_pages[] */
static unsigned long evict_tail = 0;    /* Pointer to the tail of evicted_pages[] */
static unsigned long evict_len;         /* Number of entries in a full queue */
static unsigned long max_retries;       /* Number of times to retry a bad page selection */

/* Initialize the page-replacement algorithm. */
void
jm_initialize_pagereplace (void)
{
  jm_debug_printf(2, "pagereplace_nre is initializing.\n");
  srandom((unsigned long)getpid() * (unsigned long)time(NULL));
  page_table = jm_create_page_table(0);
  total_pages = jm_globals.local_pages;
  if (total_pages < 2)
    jm_abort("A minimum of two local pages is needed for NRE page replacement to function properly");
  num_used = 0;
  jm_debug_printf(2, "%lu pages (%sB) can be cached locally.\n",
                  total_pages,
                  jm_format_power_of_2((uint64_t)total_pages*jm_globals.pagesize, 1));
  jm_globals.prefetch_type = PREFETCH_NONE;   /* Our jm_page_is_resident() does not currently return the values needed for prefetching. */
  if ((evict_len=jm_getenv_nonnegative_int("JM_NRE_ENTRIES")) == (unsigned long)(-1))
    evict_len = DEFAULT_EVICT_COUNT;
  if ((max_retries=jm_getenv_nonnegative_int("JM_NRE_RETRIES")) == (unsigned long)(-1))
    max_retries = DEFAULT_RETRY_COUNT;
  evicted_pages = (uint32_t *) jm_malloc(evict_len*sizeof(uint32_t));
  evicted_pages[0] = (uint32_t)(-1);
  jm_debug_printf(2, "JumboMem will keep track of the most recent %lu %s.\n",
                  evict_len, evict_len==1 ? "eviction" : "evictions");
  jm_debug_printf(2, "Poor selections of eviction candidates will be retried %lu %s.\n",
                  max_retries, max_retries==1 ? "time" : "times");
}


/* Say whether a page is already resident and, if so, what protections
 * it should have (always read/write). */
int
jm_page_is_resident (char *rounded_addr, int *protflags)
{
  if (!jm_page_table_find (page_table, rounded_addr))
    return 0;
  if (protflags)
    *protflags = PROT_READ|PROT_WRITE;
  return 1;
}


/* Given the address of a page that faulted, return the address of a
 * page to evict and the initial protection of the new page. */
void
jm_find_replacement_page (char *faulted_page, int *newprot, char **evictable_page, int *clean)
{
  size_t randnum;         /* A random offset into used_pages[] */
  unsigned long retries;  /* Number of attempts so far to find a good replacement */

  /* New pages are always marked read/write and old pages are always
   * considered dirty. */
  *newprot = PROT_READ|PROT_WRITE;
  *clean = 0;

  /* Early in the run we won't need to evict anything. */
  if (num_used < total_pages) {
    /* We haven't yet filled physical memory so we don't need to evict
     * anything. */
    *evictable_page = NULL;
    jm_page_table_insert(page_table, faulted_page, NULL);
    num_used++;
    jm_debug_printf(4, "%lu/%lu pages are now in use.\n", num_used, total_pages);
    return;
  }

  /* Later in the run we need to find a replacement page.  We choose a
   * page at random but try again if we hit a page we've recently evicted. */
  retries = 0;
  while (retries <= max_retries) {
    uint32_t pagenum;      /* Page number to evict */
    unsigned long i;

    /* Randomly select a page from the set of resident pages. */
    randnum = ((random() + BIGPRIME1) * BIGPRIME2) % num_used;
    jm_page_table_offset(page_table, randnum, &pagenum, NULL);
    *evictable_page = jm_globals.memregion + pagenum*jm_globals.pagesize;

    /* Linear search the list of recent evictions for our current selection. */
    for (i=evict_head; i!=evict_tail; i=(i+1)%evict_len)
      if (evicted_pages[i] == randnum) {
        retries++;
	break;
      }
    if (evicted_pages[i] != randnum)
      /* The current selection is a good one. */
      break;
    if (retries <= max_retries)
      jm_debug_printf(5, "The page at address %p was recently evicted.  Selecting alternate #%lu.\n",
		      *evictable_page, retries);
  }
  evicted_pages[evict_tail] = randnum;
  evict_tail = (evict_tail+1) % evict_len;
  if (evict_tail == evict_head)
    evict_head = (evict_head+1) % evict_len;

  /* Keep track of the page we're about to bring in and the page we're
   * about to kick out. */
  jm_debug_printf(4, "Replacing page %lu of %lu (address %p).\n",
		  randnum+1, total_pages, *evictable_page);
  jm_page_table_delete(page_table, *evictable_page);
  jm_page_table_insert(page_table, faulted_page, NULL);
}


/* Finalize the random algorithm. */
void
jm_finalize_pagereplace (void)
{
}
