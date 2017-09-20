/*-----------------------------------------------------------------
 * JumboMem memory server: Random page replacement
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

extern JUMBOMEM_GLOBALS jm_globals;     /* All of our global variables */
static char *prevpage;                  /* Address (rounded) of previous fault */
static uint32_t *used_pages;            /* Set of page numbers in use */
static unsigned long num_used;          /* Number of valid entries in the above */
static unsigned long total_pages;       /* Size of the physical address space in pages */


/* Initialize the page-replacement algorithm. */
void
jm_initialize_pagereplace (void)
{
  jm_debug_printf(2, "pagereplace_random is initializing.\n");
  prevpage = NULL;
  srandom((unsigned long)getpid() * (unsigned long)time(NULL));
  total_pages = jm_globals.local_pages;
  if (total_pages < 2)
    jm_abort("A minimum of two local pages is needed for random page-replacement to function properly");
  used_pages = (uint32_t *) jm_malloc(total_pages*sizeof(uint32_t));
  num_used = 0;
  jm_debug_printf(2, "%lu pages (%sB) can be cached locally.\n",
                  total_pages,
                  jm_format_power_of_2((uint64_t)total_pages*jm_globals.pagesize, 1));
  jm_globals.prefetch_type = PREFETCH_NONE;   /* Our jm_page_is_resident() does not currently return the values needed for prefetching. */
}


/* Say whether a page is already resident.  (It never is in this
 * page-replacement scheme.) */
int
jm_page_is_resident (char *rounded_addr JM_UNUSED, int *protflags)
{
  if (protflags)
    /* The given page faulted.  It's therefore not resident because we
     * never mark pages read-only in this page-replacement scheme. */
    return 0;
  else
    /* To support prefetching we'd need to perform a linear search
     * through the used_pages array (or use a better data structure).
     * For now, we simply disable prefetching in
     * jm_initialize_pagereplace() and return -1 here.*/
    return -1;
}


/* Given the address of a page that faulted, return the address of a
 * page to evict and the initial protection of the new page. */
void
jm_find_replacement_page (char *faulted_page, int *newprot, char **evictable_page, int *clean)
{
  size_t randnum;      /* A random offset into used_pages[] */

  /* New pages are always marked read/write and old pages are always
   * considered dirty. */
  *newprot = PROT_READ|PROT_WRITE;
  *clean = 0;

  /* Early in the run we won't need to evict anything. */
  if (num_used < total_pages) {
    /* We haven't yet filled physical memory so we don't need to evict
     * anything. */
    *evictable_page = NULL;
    used_pages[num_used++] = (uint32_t) GET_PAGE_NUMBER(faulted_page);
    jm_debug_printf(4, "%lu/%lu pages are now in use.\n", num_used, total_pages);
    return;
  }

  /* Later in the run we need to find a replacement page.  We choose a
   * page at random but exclude the most recently allocated page. */
  do {
    randnum = ((random() + BIGPRIME1) * BIGPRIME2) % num_used;
    *evictable_page = jm_globals.memregion + used_pages[randnum]*jm_globals.pagesize;
  }
  while (*evictable_page == prevpage);

  /* Keep track of the page we're about to bring in and the page we're
   * about to kick out. */
  jm_debug_printf(4, "Replacing page %lu of %lu.\n", randnum+1, total_pages);
  used_pages[randnum] = (uint32_t) GET_PAGE_NUMBER(faulted_page);
  prevpage = faulted_page;
}


/* Finalize the random algorithm. */
void
jm_finalize_pagereplace (void)
{
}
