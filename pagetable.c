/*-----------------------------------------------------------------
 * JumboMem memory server: Page-table manipulation
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

/* Define the size of our PTE hash table. */
#ifndef HASH_TABLE_SIZE
# define HASH_TABLE_SIZE 1000003
#endif

/* Define a few big prime numbers for scaling random integers in case
 * RAND_MAX is small. */
#define BIGPRIME1 34359738641LL
#define BIGPRIME2 1152921504606847229LL

/* Access the ith element of pt->used_pages. */
#define USED_PAGES(IDX) \
  (*((PAGE_TABLE_ENTRY *) ((char *)pt->used_pages + (IDX)*(pt->payload_bytes + sizeof(PAGE_TABLE_ENTRY)))))

/* Define a page-table entry structure. */
typedef struct {
  uint32_t pagenum;                    /* Page number */
} PAGE_TABLE_ENTRY;

/* Define an element in a linked list of page-table entries. */
typedef struct page_bucket {
  PAGE_TABLE_ENTRY *pte;               /* Page-table entry */
  struct page_bucket *next;            /* Next element in the bucket chain */
} PAGE_BUCKET;

/* Define a complete page table. */
typedef struct {
  PAGE_TABLE_ENTRY *used_pages;       /* Unordered set of pages in use */
  struct page_bucket **page_hash;     /* Hash table with pointers to used_pages */
  struct page_bucket *dead_bucket;    /* Previously deleted bucket */
  size_t payload_bytes;               /* Number of bytes of payload in a PAGE_TABLE_ENTRY */
  size_t num_used;                    /* Number of pages in use */
  size_t table_size;                  /* Maximum number of pages that can be used */
} PAGE_TABLE;


/* Map a page number to [0, HASH_TABLE_SIZE-1]. */
static inline unsigned long
hash_page_number (uint32_t pagenum)
{
  return (((unsigned long)pagenum + BIGPRIME2) * BIGPRIME1) % HASH_TABLE_SIZE;
}


/* Return a PTE given a page number or NULL if the page isn't resident. */
static inline PAGE_TABLE_ENTRY *
find_page_by_number (PAGE_TABLE *pt, uint32_t pagenum)
{
  struct page_bucket *bucket;

  for (bucket=pt->page_hash[hash_page_number(pagenum)]; bucket; bucket=bucket->next)
    if (bucket->pte->pagenum == pagenum)
      return bucket->pte;
  return NULL;
}


/* Delete a PTE given its page number. */
static inline void
delete_page_by_number (PAGE_TABLE *pt, uint32_t pagenum)
{
  struct page_bucket *bucket;        /* Current bucket pointer */
  struct page_bucket *prev_bucket;   /* Previous bucket pointer */
  unsigned long chain_num = hash_page_number(pagenum);  /* Hash chain number */
  struct page_bucket **page_hash = pt->page_hash;       /* Cache of the current page table */

  /* Ensure that we previously inserted the given page and that we're
   * alternating deletions and insertions as expected. */
  if (!page_hash[chain_num])
    jm_abort("Internal error: Attempted to delete nonexistent page %p (empty chain)",
             jm_globals.memregion+pagenum*jm_globals.pagesize);
  if (pt->dead_bucket)
    jm_abort("Internal error: Two page-table deletions with no intervening insertion");

  /* Perform a special case for the first element in the chain. */
  if (page_hash[chain_num]->pte->pagenum == pagenum) {
    bucket = page_hash[chain_num];
    page_hash[chain_num] = bucket->next;
    pt->dead_bucket = bucket;
    return;
  }

  /* Search the chain for the given page and delete the page when found. */
  for (prev_bucket=page_hash[chain_num], bucket=prev_bucket->next;
       bucket;
       prev_bucket=bucket, bucket=bucket->next)
    if (bucket->pte->pagenum == pagenum) {
      prev_bucket->next = bucket->next;
      pt->dead_bucket = bucket;
      return;
    }
  jm_abort("Internal error: Attempted to delete nonexistent page %p",
           jm_globals.memregion+pagenum*jm_globals.pagesize);
}


/* Insert a PTE into the page table. */
static inline void
insert_pte (PAGE_TABLE *pt, PAGE_TABLE_ENTRY *pte)
{
  struct page_bucket *bucket;                       /* New bucket */
  unsigned long chain_num = hash_page_number(pte->pagenum);  /* Hash chain number */

  if (pt->dead_bucket)
    /* Common case -- we can reuse a recently deleted bucket. */
    bucket = pt->dead_bucket;
  else
    /* Early in the program -- we haven't yet deleted any buckets. */
    bucket = (struct page_bucket *) jm_malloc(sizeof(struct page_bucket));
  bucket->pte = pte;
  bucket->next = pt->page_hash[chain_num];
  pt->page_hash[chain_num] = bucket;
  pt->dead_bucket = NULL;
}

/* ---------------------------------------------------------------------- */

/* Return a page table with a given number of bytes of data per entry
 * (plus sizeof(uint32_t) bytes of key data).  Reduce the number of
 * locally cached pages (jm_globals.local_pages) to accomodate the
 * page table. */
void *
jm_create_page_table (size_t valuebytes)
{
  long new_total_pages;   /* Signed version of jm_globals.local_pages */
  size_t pte_bytes = sizeof(uint32_t) + valuebytes;   /* Bytes per PAGE_TABLE_ENTRY */
  PAGE_TABLE *pt;         /* Page table to allocate and return */

  /* Unless the user explicitly specified a page count, reduce the
   * number of locally cacheable pages to make room for our data
   * structures. */
  if (getenv("JM_LOCAL_PAGES"))
    new_total_pages = jm_globals.local_pages;
  else {
    new_total_pages = jm_globals.local_pages * jm_globals.pagesize;
    new_total_pages -= HASH_TABLE_SIZE * sizeof(struct page_bucket *);
    new_total_pages /= (long) (jm_globals.pagesize+sizeof(struct page_bucket)+sizeof(PAGE_TABLE_ENTRY *)+pte_bytes);
    jm_debug_printf(3, "Reducing the number of locally cacheable pages from %lu to %ld to accommodate a page table.\n",
		    jm_globals.local_pages, new_total_pages);
  }
  if (new_total_pages < 1)
    jm_abort("Too little memory is available to cache locally even one remote page");
  jm_globals.local_pages = new_total_pages;

  /* Allocate and return a page table. */
  pt = (PAGE_TABLE *) jm_malloc(sizeof(PAGE_TABLE));
  pt->num_used = 0;
  pt->table_size = new_total_pages;
  pt->used_pages = (PAGE_TABLE_ENTRY *) jm_malloc(new_total_pages*pte_bytes);
  pt->page_hash = (struct page_bucket **) jm_malloc(HASH_TABLE_SIZE*sizeof(struct page_bucket *));
  memset((void *)pt->page_hash, 0, HASH_TABLE_SIZE*sizeof(struct page_bucket *));
  pt->dead_bucket = NULL;
  pt->payload_bytes = valuebytes;
  return (void *) pt;
}


/* Insert a page into the page table.  The caller must ensure that
 * this function is called no more than jm_globals.local_pages
 * times. */
void
jm_page_table_insert (void *pt_obj, char *address, void *extradata)
{
  PAGE_TABLE *pt = (PAGE_TABLE *)pt_obj;  /* The page table proper */
  PAGE_TABLE_ENTRY *pte;                  /* A single page-table entry */

  /* Allocate a page-table entry or abort if no PTEs are free. */
  if (pt->num_used == pt->table_size)
    jm_abort("A page table with %lu entries overflowed", pt->table_size);
  if (pt->dead_bucket)
    /* Common case: Recycle the previously detached PTE. */
    pte = pt->dead_bucket->pte;
  else
    /* Early in the program: Point to the next available PTE. */
    pte = &USED_PAGES(pt->num_used);
  pte->pagenum = GET_PAGE_NUMBER(address);
  if (extradata)
    memcpy(pte+1, extradata, pt->payload_bytes);

  /* Insert the PTE into the given bucket. */
  insert_pte(pt, pte);
  pt->num_used++;
}


/* Delete a page from the page table. */
void
jm_page_table_delete (void *pt_obj, char *address)
{
  PAGE_TABLE *pt = (PAGE_TABLE *)pt_obj;  /* The page table proper */

  delete_page_by_number(pt, GET_PAGE_NUMBER(address));
  pt->num_used--;
}


/* Given a page's address, return a pointer to that page's payload
 * data or NULL if the page isn't resident. */
void *
jm_page_table_find (void *pt_obj, char *address)
{
  PAGE_TABLE_ENTRY *pte;        /* One page-table entry */

  pte = find_page_by_number((PAGE_TABLE *)pt_obj, GET_PAGE_NUMBER(address));
  if (pte)
    return (void *) (pte+1);    /* Return a pointer to the payload data. */
  else
    return NULL;
}


/* Map an index in [0, num_used-1] to a page number and the associated
 * payload.  Abort if the index is out of range. */
void
jm_page_table_offset (void *pt_obj, uint32_t index, uint32_t *pagenum, void **extradata)
{
  PAGE_TABLE *pt = (PAGE_TABLE *) pt_obj;  /* The page table proper */
  PAGE_TABLE_ENTRY *pte;                   /* The specified page-table entry */

  if (index >= pt->num_used)
    jm_abort("Page-table offset %u is out-of-bounds", index);
  pte = &USED_PAGES(index);
  *pagenum = pte->pagenum;
  if (extradata)
    *extradata = pte + 1;
}


/* Free all of the data in a page table. */
void
jm_page_table_free (void *pt_obj)
{
  PAGE_TABLE *pt = (PAGE_TABLE *) pt_obj;       /* The page table proper */
  unsigned int i;

  for (i=0; i<HASH_TABLE_SIZE; i++)
    if (pt->page_hash[i])
      jm_free(pt->page_hash[i]);
  jm_free(pt->page_hash);
  jm_free(pt->used_pages);
  jm_free(pt);
}
