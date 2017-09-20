/*-----------------------------------------------------------------
 * JumboMem memory server: Not-recently-used page replacement
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

/* Define a default interval in milliseconds for clearing all
 * referenced bits */
#ifndef DEFAULT_NRU_INTERVAL
# define DEFAULT_NRU_INTERVAL 5000
#endif

/* Define the size of our PTE hash table. */
#ifndef HASH_TABLE_SIZE
# define HASH_TABLE_SIZE 1000003
#endif

/* Define a few big prime numbers for scaling random integers in case
 * RAND_MAX is small. */
#define BIGPRIME1 34359738641LL
#define BIGPRIME2 1152921504606847229LL

/* Map a page's referenced and modified bits to an NRU class. */
#define NRU_CLASS(PTE) (PTE->referenced*2 + PTE->modified)

/* Define a page-table entry structure. */
typedef struct {
  uint32_t pagenum;          /* Page number */
  int      referenced;       /* Page has been referenced */
  int      modified;         /* Page has been modified */
} PAGE_TABLE_ENTRY;

/* Define an element in a linked list of page-table entries. */
typedef struct page_bucket {
  PAGE_TABLE_ENTRY *pte;               /* Page-table entry */
  struct page_bucket *next;            /* Next element in the bucket chain */
} PAGE_BUCKET;

extern JUMBOMEM_GLOBALS jm_globals;    /* All of our global variables */
static PAGE_TABLE_ENTRY *used_pages;   /* Unordered set of pages in use */
static struct page_bucket **page_table;    /* Hash table with pointers to used_pages */
static PAGE_TABLE_ENTRY **pages_by_class;  /* Pointers to the above, sorted by NRU class */
static int sorted_by_class = 0;       /* 1=pages_by_class is sorted; 0=unsorted */
static unsigned long class_size[4];    /* Number of pages in each NRU class */
static unsigned long num_used;         /* Number of pages in all NRU classes */
static unsigned long total_pages;      /* Size of the physical address space in pages */
static int nru_readwrite;              /* 0=mark new pages read-only; 1=read/write */
static unsigned long nru_interval_ms;  /* Number of milliseconds between reference-bit clearing */
static uint64_t prev_rbit_clear_time;  /* Time at which reference bits were last cleared */
static struct page_bucket *dead_bucket = NULL;   /* Previously deleted bucket */

/* Keep track of some page-replacement statistics. */
#ifdef JM_DEBUG
static unsigned long replacement_classes[4];   /* Tally of replacements by NRU class */
#endif


/* Map a page number to [0, HASH_TABLE_SIZE-1]. */
static inline unsigned long
hash_page_number (uint32_t pagenum)
{
  return (((unsigned long)pagenum + BIGPRIME2) * BIGPRIME1) % HASH_TABLE_SIZE;
}


/* Return a PTE given a page number or NULL if the page isn't resident. */
static PAGE_TABLE_ENTRY *
find_page_by_number (uint32_t pagenum)
{
  struct page_bucket *bucket;

  for (bucket=page_table[hash_page_number(pagenum)]; bucket; bucket=bucket->next)
    if (bucket->pte->pagenum == pagenum)
      return bucket->pte;
  return NULL;
}


/* Delete a PTE given its page number. */
static void
delete_page_by_number (uint32_t pagenum)
{
  struct page_bucket *bucket;        /* Current bucket pointer */
  struct page_bucket *prev_bucket;   /* Previous bucket pointer */
  unsigned long chain_num = hash_page_number(pagenum);  /* Hash chain number */

  /* Ensure that we previously inserted the given page and that we're
   * alternating deletions and insertions as expected. */
  if (!page_table[chain_num])
    jm_abort("Internal error: Attempted to delete nonexistent page %p (empty chain)",
             jm_globals.memregion+pagenum*jm_globals.pagesize);
  if (dead_bucket)
    jm_abort("Internal error: Two page-table deletions with no intervening insertion");

  /* Perform a special case for the first element in the chain. */
  if (page_table[chain_num]->pte->pagenum == pagenum) {
    bucket = page_table[chain_num];
    page_table[chain_num] = bucket->next;
    dead_bucket = bucket;
    return;
  }

  /* Search the chain for the given page and delete the page when found. */
  for (prev_bucket=page_table[chain_num], bucket=prev_bucket->next;
       bucket;
       prev_bucket=bucket, bucket=bucket->next)
    if (bucket->pte->pagenum == pagenum) {
      prev_bucket->next = bucket->next;
      dead_bucket = bucket;
      return;
    }
  jm_abort("Internal error: Attempted to delete nonexistent page %p",
           jm_globals.memregion+pagenum*jm_globals.pagesize);
}


/* Insert a PTE into the page table. */
static void
insert_pte (PAGE_TABLE_ENTRY *pte)
{
  struct page_bucket *bucket;                       /* New bucket */
  unsigned long chain_num = hash_page_number(pte->pagenum);  /* Hash chain number */

  if (dead_bucket)
    /* Common case -- we can reuse a recently deleted bucket. */
    bucket = dead_bucket;
  else
    /* Early in the program -- we haven't yet deleted any buckets. */
    bucket = (struct page_bucket *) jm_malloc(sizeof(struct page_bucket));
  bucket->pte = pte;
  bucket->next = page_table[chain_num];
  page_table[chain_num] = bucket;
  dead_bucket = NULL;
}


/* Re-sort the pages_by_class array. */
static void
sort_pages_by_class (void)
{
  unsigned long class_offset[4];    /* Indexes into pages_by_class by class */
  unsigned long i;

  /* Do nothing if the list is already sorted. */
  if (sorted_by_class)
    return;

  /* Recalculate the number of entries in each class. */
  class_size[0] = 0;
  class_size[1] = 0;
  class_size[2] = 0;
  class_size[3] = 0;
  for (i=0; i<num_used; i++)
    class_size[NRU_CLASS(pages_by_class[i])]++;

  /* Calculate an initial offset for each class. */
  class_offset[0] = 0;
  class_offset[1] = class_size[0];
  class_offset[2] = class_size[1] + class_offset[1];
  class_offset[3] = class_size[2] + class_offset[2];

  /* Sort pages_by_class using a bucket sort. */
  for (i=0; i<num_used; i++) {
    PAGE_TABLE_ENTRY *onepage = &used_pages[i];   /* Page to place */
    pages_by_class[class_offset[NRU_CLASS(onepage)]++] = onepage;
  }
  sorted_by_class = 1;
}


/* Clear all reference bits every nru_interval_ms milliseconds. */
static void
maybe_clear_reference_bits (void)
{
  uint64_t now = jm_current_time()/1000;
  unsigned long i;

  if (now - prev_rbit_clear_time < nru_interval_ms)
    return;
  jm_debug_printf(4, "Resetting all NRU reference bits.\n");
  for (i=0; i<num_used; i++)
    used_pages[i].referenced = 0;
  sorted_by_class = 0;
  prev_rbit_clear_time = now;
}


/* Initialize the NRU algorithm. */
void
jm_initialize_pagereplace (void)
{
  long new_total_pages;   /* Signed version of total_pages */

  jm_debug_printf(2, "pagereplace_nru is initializing.\n");
  srandom((unsigned long)getpid() * (unsigned long)time(NULL));

  /* Unless the user explicitly specified a page count, reduce the
   * number of locally cacheable pages to make room for our data
   * structures. */
  if (getenv("JM_LOCAL_PAGES"))
    new_total_pages = jm_globals.local_pages;
  else {
    new_total_pages = jm_globals.local_pages * jm_globals.pagesize;
    new_total_pages -= HASH_TABLE_SIZE * sizeof(struct page_bucket *);
    new_total_pages /= (long) (jm_globals.pagesize+sizeof(struct page_bucket)+sizeof(PAGE_TABLE_ENTRY *)+sizeof(PAGE_TABLE_ENTRY));
    jm_debug_printf(3, "Reducing the number of locally cacheable pages from %lu to %ld to accommodate NRU data.\n",
		    jm_globals.local_pages, new_total_pages);
  }
  if (new_total_pages < 1)
    jm_abort("A minimum of one local page is needed for NRU page-replacement to function properly");
  total_pages = new_total_pages;
  jm_globals.local_pages = total_pages;

  used_pages = (PAGE_TABLE_ENTRY *) jm_malloc(total_pages*sizeof(PAGE_TABLE_ENTRY));
  page_table = (struct page_bucket **) jm_malloc(HASH_TABLE_SIZE*sizeof(struct page_bucket *));
  memset((void *)page_table, 0, HASH_TABLE_SIZE*sizeof(struct page_bucket *));
  pages_by_class = (PAGE_TABLE_ENTRY **) jm_malloc(total_pages*sizeof(PAGE_TABLE_ENTRY *));
  class_size[0] = 0;
  class_size[1] = 0;
  class_size[2] = 0;
  class_size[3] = 0;
#ifdef JM_DEBUG
  replacement_classes[0] = 0;
  replacement_classes[1] = 0;
  replacement_classes[2] = 0;
  replacement_classes[3] = 0;
#endif
  num_used = 0;
  if ((nru_readwrite=jm_getenv_boolean("JM_NRU_RW")) == -1)
    nru_readwrite = 1;
  if (!(nru_interval_ms=jm_getenv_positive_int("JM_NRU_INTERVAL")))
    nru_interval_ms = DEFAULT_NRU_INTERVAL;
  prev_rbit_clear_time = jm_current_time() / 1000;   /* Convert to milliseconds. */
  jm_debug_printf(2, "NRU reference bits will be cleared every %lu milliseconds.\n",
                  nru_interval_ms);
  jm_debug_printf(2, "Newly loaded pages will be marked %s.\n",
                  nru_readwrite ? "read/write" : "read-only");
  jm_debug_printf(2, "%lu pages (%sB) can be cached locally.\n",
                  total_pages,
                  jm_format_power_of_2((uint64_t)total_pages*jm_globals.pagesize, 1));
}


/* Say whether a page is already resident and return new protection
 * flags if it is.  If protflags is NULL, we make no changes to our
 * internal state. */
int
jm_page_is_resident (char *rounded_addr, int *protflags)
{
  PAGE_TABLE_ENTRY *pte;      /* Page-table entry found */

  /* Clear all reference bits if we haven't recently. */
  maybe_clear_reference_bits();

  /* Search for the given address. */
  if (!(pte=find_page_by_number((uint32_t) GET_PAGE_NUMBER(rounded_addr))))
    return 0;

  /* Alter the NRU class now that the page is modified. */
  if (protflags) {
    pte->referenced = 1;
    pte->modified = 1;
    sorted_by_class = 0;
    *protflags = PROT_READ|PROT_WRITE;
  }
  return 1;
}


/* Given the address of a page that faulted, return the address of a
 * page to evict and the initial protections of the new page. */
void
jm_find_replacement_page (char *faulted_page, int *newprot, char **evictable_page, int *clean)
{
  int class;     /* NRU class from which to evict a page */
  PAGE_TABLE_ENTRY *pframe;   /* Page frame to evict and replace */

  /* Clear all reference bits if we haven't recently. */
  maybe_clear_reference_bits();

  /* If the page table isn't full we don't need to evict anything. */
  if (num_used < total_pages) {
    pframe = &used_pages[num_used];
    pages_by_class[num_used] = pframe;
    num_used++;
    *evictable_page = NULL;
    jm_debug_printf(4, "%lu/%lu pages are now in use.\n", num_used, total_pages);
  }
  else {
    /* Randomly select a page to evict from the smallest-numbered
     * nonempty NRU class. */
    long int random_offset;      /* Random offset into pages_by_class */

    /* Find the smallest-numbered nonempty class. */
    for (class=0; class<4 && class_size[class]==0; class++)
      ;

    /* Because pages_by_class is probably nearly sorted, for speed we
     * simply select a random page from what should be the desired
     * class.  Only if the page has the wrong class do we sort the
     * array and try again. */
    random_offset = ((random() + BIGPRIME1) * BIGPRIME2) % class_size[class];
    pframe = pages_by_class[random_offset];
    if (NRU_CLASS(pframe) != class) {
      sort_pages_by_class();
      pframe = pages_by_class[random_offset];
    }

    /* Map the page frame number to a byte offset into the memory region. */
    *evictable_page = jm_globals.memregion + jm_globals.pagesize*pframe->pagenum;
    *clean = !pframe->modified;
    delete_page_by_number(pframe->pagenum);
#ifdef JM_DEBUG
    replacement_classes[class]++;
    jm_debug_printf(4, "Replacing page %lu of %lu (a class %d page).\n",
                    pframe->pagenum+1, total_pages, class);
#endif
  }

  /* Keep track of the newly faulted page. */
  pframe->pagenum = (uint32_t) GET_PAGE_NUMBER(faulted_page);
  pframe->referenced = 1;
  insert_pte(pframe);
  sorted_by_class = 0;
  if (nru_readwrite) {
    *newprot = PROT_READ|PROT_WRITE;
    pframe->modified = 1;
  }
  else {
    *newprot = PROT_READ;
    pframe->modified = 0;
  }
}


/* Finalize the NRU algorithm. */
void
jm_finalize_pagereplace (void)
{
  /* Output some statistics on normal termination. */
#ifdef JM_DEBUG
  if (!jm_globals.error_exit) {
    jm_debug_printf(2, "Evictions by NRU class:\n");
    jm_debug_printf(2, "   Class 0 (unreferenced, unmodified): %lu\n", replacement_classes[0]);
    jm_debug_printf(2, "   Class 1 (unreferenced, modified):   %lu\n", replacement_classes[1]);
    jm_debug_printf(2, "   Class 2 (referenced, unmodified):   %lu\n", replacement_classes[2]);
    jm_debug_printf(2, "   Class 3 (referenced, modified):     %lu\n", replacement_classes[3]);
  }
#endif
}
