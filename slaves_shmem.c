/*----------------------------------------------------------------
 * JumboMem memory server: SHMEM (put/get) slaves
 *
 * By Scott Pakin <pakin@lanl.gov>
 *----------------------------------------------------------------*/

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
#include <shmem.h>
#ifdef JM_DEBUG
# include <sys/resource.h>
#endif

extern JUMBOMEM_GLOBALS jm_globals;   /* All of our other global variables */
static char *buffer = NULL;           /* One slave's memory buffer */
static char **buffer_addr;            /* Array of each slave's memory buffer address */


/* Initialize SHMEM.  Only rank 0 returns to the caller. */
void
jm_initialize_slaves (void)
{
  int rank;                            /* Our rank in the computation */
  int numranks;                        /* The total number of ranks */
  long min_memory;                     /* Minimum free memory on any rank */
  long workarray[_SHMEM_REDUCE_MIN_WRKDATA_SIZE];  /* Needed by SHMEM */
  long syncarray[_SHMEM_REDUCE_SYNC_SIZE];         /* Needed by SHMEM */
  int i;

  /* Common initialization */
  jm_debug_printf(3, "slaves_shmem is initializing.\n");
  shmem_init();
  rank = shmem_my_pe();
  numranks = shmem_n_pes();
  if (rank == 0)
    jm_debug_printf(3, "The master task can use at most %ld bytes of memory.\n",
                    jm_globals.slavebytes);
  else
    jm_debug_printf(3, "Slave #%d can use at most %ld bytes of memory.\n",
                    rank, jm_globals.slavebytes);
  jm_globals.numslaves = numranks - 1;
  for (i=0; i<_SHMEM_REDUCE_SYNC_SIZE; i++)
    syncarray[i] = _SHMEM_SYNC_VALUE;
  shmem_long_min_to_all(&min_memory, (long *)&jm_globals.slavebytes, 1, 0, 0,
                        numranks, workarray, syncarray);
  jm_globals.slavebytes = min_memory;
  if (rank > 0)
    buffer = (char *) jm_malloc(jm_globals.slavebytes);
  buffer_addr = (char **) jm_malloc(numranks*sizeof(char *));
  for (i=0; i<_SHMEM_REDUCE_SYNC_SIZE; i++)
    syncarray[i] = _SHMEM_SYNC_VALUE;
  shmem_fcollect64((void *)buffer_addr, &buffer, 1, 0, 0, numranks, syncarray);

  /* Slaves simply spin until the program exits. */
  if (rank > 0)
    while (1)
      ;
}


/* Start evicting a given page. */
void *
jm_evict_begin (char *evict_addr, char *evict_buffer)
{
  size_t put_offset;     /* Slave buffer offset to which to put a page */
  int put_slave;         /* Slave to which to put a page */
  void *put_handle;      /* SHMEM nonblocking put handle */

  /* Announce what we're about to do. */
  jm_debug_printf(4, "Evicting the page at address %p.\n", evict_addr);

  /* Begin the page eviction. */
  put_slave = (int)GET_SLAVE_NUM(evict_addr);
  put_offset = GET_SLAVE_OFFSET(evict_addr);
  shmem_putmem_nb((void *)(buffer_addr[put_slave+1]+put_offset), (void *)evict_buffer,
                  jm_globals.pagesize, put_slave+1, &put_handle);
  return put_handle;
}


/* Finish evicting a given page. */
void
jm_evict_end (void *stateobj)
{
  /* Announce what we're about to do. */
  jm_debug_printf(4, "Completing a page eviction.\n");

  /* Complete the Put operation. */
  shmem_wait_nb(stateobj);
}


/* Start fetching a given page. */
void *
jm_fetch_begin (char *fetch_addr, char *fetch_buffer)
{
  size_t get_offset;     /* Slave buffer offset to which to get a page */
  int get_slave;         /* Slave to which to get a page */
  void *get_handle;      /* SHMEM nonblocking get handle */

  /* Announce what we're about to do. */
  jm_debug_printf(4, "Fetching the page at address %p.\n", fetch_addr);

  /* Begin the page fetch. */
  get_slave = (int)GET_SLAVE_NUM(fetch_addr);
  get_offset = GET_SLAVE_OFFSET(fetch_addr);
  shmem_getmem_nb((void *)fetch_buffer, (void *)(buffer_addr[get_slave+1]+get_offset),
                  jm_globals.pagesize, get_slave+1, &get_handle);
  return get_handle;
}


/* Finish fetching a given page. */
void
jm_fetch_end (void *stateobj)
{
  /* Announce what we're about to do. */
  jm_debug_printf(4, "Waiting for a page to arrive.\n");

  /* Complete the Get operation. */
  shmem_wait_nb(stateobj);
}


/* Shut down cleanly. */
void
jm_finalize_slaves (void)
{
  globalexit(0);
}
