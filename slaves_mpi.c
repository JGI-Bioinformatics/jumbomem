/*----------------------------------------------------------------
 * JumboMem memory server: Message Passing Interface (MPI) slaves
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
#include <mpi.h>

#ifndef MAX_PENDING_FETCHES
# define MAX_PENDING_FETCHES 2
#endif
#ifndef MAX_PENDING_EVICTIONS
# define MAX_PENDING_EVICTIONS 2
#endif

/* Convert a pointer to a buffer offset to a valid memory address. */
#define OFSP2ADDR(OFS) (buffer + FROM_NETWORK(*(size_t *)(OFS)))


/* Define the internal state needed for a split-phase fetch. */
typedef struct {
  int          valid;      /* 0=available; 1=in use */
  char        *address;    /* Virtual address to fetch */
  MPI_Request  request;    /* MPI state for a nonblocking receive */
} FETCH_STATE;

/* Define the internal state needed for a split-phase evict. */
typedef struct {
  int          valid;       /* 0=available; 1=in use */
  char        *address;     /* Virtual address to evict */
  MPI_Request  requests[2]; /* MPI state for a nonblocking send of address + data */
} EVICT_STATE;

/* Define the set of commands the master can send to a slave -- and
 * one response a slave can send to the master. */
typedef enum {
  JM_MPI_TERMINATE,        /* The slave should terminate */
  JM_MPI_PUT_OFFSET,       /* Buffer offset to write to */
  JM_MPI_PUT_DATA,         /* Data to write to the buffer */
  JM_MPI_GET,              /* Buffer offset to read from */
  JM_MPI_RESPONSE          /* Data sent from slave to master */
} JM_MPI_COMMAND;

extern JUMBOMEM_GLOBALS jm_globals;   /* All of our other global variables */
static char *buffer = NULL;           /* One slave's memory buffer */
static FETCH_STATE fetch_state[MAX_PENDING_FETCHES];   /* Set of split-phase fetch state */
static EVICT_STATE evict_state[MAX_PENDING_EVICTIONS]; /* Set of split-phase eviction state */
static int rank;                      /* Our rank in the computation */
#ifdef JM_DEBUG
static struct rusage initial_usage;   /* Memory usage when entering the main loop */
#endif


/* Repeatedly process commands we receive from the network. */
static void
slave_event_loop (void)
{
  int pagesize = (int) jm_globals.pagesize;  /* Cache of the global page size */
  char *recvbuf;              /* One page to send/receive */
  MPI_Status status;          /* A message's MPI receive status */
  MPI_Request request;        /* Handle to an MPI asynchronous-receive */
  char *next_touch = buffer;  /* Next word of memory to touch */

  /* Receive and process messages until we're told to stop. */
  recvbuf = (char *) jm_valloc(pagesize);
  do {
    /* While we wait for a mesasge to arrive we touch each page in
     * turn in hopes of discouraging the operating system from
     * reclaiming some of the pages we haven't accessed recently. */
    MPI_Irecv((void *)recvbuf, pagesize, MPI_BYTE, 0, MPI_ANY_TAG, MPI_COMM_WORLD, &request);
    while (1) {
      int recv_complete;      /* 1=a mesasge arrived; 0=still waiting */

      MPI_Test(&request, &recv_complete, &status);
      if (recv_complete)
        break;
      *(volatile int *)next_touch;   /* Touch the current page. */
      next_touch += jm_globals.ospagesize;
      if (next_touch >= buffer+jm_globals.slavebytes)
        next_touch = buffer;
    }

    /* Decide based on the message tag what to do next. */
    switch (status.MPI_TAG) {
      case JM_MPI_PUT_OFFSET:
        /* The master is telling us where it'll next write to. */
        MPI_Recv(jm_globals.extra_memcpy ? (void *)recvbuf : OFSP2ADDR(recvbuf),
                 pagesize, MPI_BYTE, 0, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        if (status.MPI_TAG != JM_MPI_PUT_DATA
            && status.MPI_TAG != JM_MPI_TERMINATE)
          jm_abort("Expected MPI tag %d but received MPI tag %d",
                   JM_MPI_PUT_DATA, status.MPI_TAG);
        if (jm_globals.extra_memcpy)
          memcpy((void *)(OFSP2ADDR(recvbuf)), (void *)recvbuf, pagesize);
	jm_debug_printf(5, "Processed a JM_MPI_PUT_OFFSET of address %p.\n",
			jm_globals.extra_memcpy ? (void *)recvbuf : OFSP2ADDR(recvbuf));
        break;

      case JM_MPI_PUT_DATA:
        /* The master is giving us data to write to our buffer.  This
         * case *should* have been handled by the JM_MPI_PUT_OFFSET
         * case above. */
        jm_abort("Received MPI tag %d at an unexpected time", JM_MPI_PUT_DATA);
        break;

      case JM_MPI_GET:
        /* The master wants a page from us. */
	jm_debug_printf(5, "Processing a JM_MPI_GET of address %p.\n",
			jm_globals.extra_memcpy ? (void *)recvbuf : OFSP2ADDR(recvbuf));
        if (jm_globals.extra_memcpy) {
          memcpy((void *)recvbuf, (void *)(OFSP2ADDR(recvbuf)), pagesize);
          MPI_Rsend(recvbuf, pagesize, MPI_BYTE, 0, JM_MPI_RESPONSE, MPI_COMM_WORLD);
        }
        else
          MPI_Rsend(OFSP2ADDR(recvbuf), pagesize, MPI_BYTE, 0, JM_MPI_RESPONSE, MPI_COMM_WORLD);
        break;

      case JM_MPI_TERMINATE:
        /* Break out of the loop. */
        break;

      default:
        /* We received an unrecognized command. */
        jm_abort("Unrecognized MPI tag %d", status.MPI_TAG);
        break;
    }
  }
  while (status.MPI_TAG != JM_MPI_TERMINATE);

  /* The master instructed us to terminate.*/
#ifdef JM_DEBUG
  if (jm_globals.debuglevel >= 3) {
    struct rusage usage;   /* Information to report about our memory usage */

    getrusage(RUSAGE_SELF, &usage);
    jm_debug_printf(3, "Slave #%d is terminating with %ld major faults, %ld minor faults, and %ld swaps.\n",
                    rank,
                    usage.ru_majflt - initial_usage.ru_majflt,
                    usage.ru_minflt - initial_usage.ru_minflt,
                    usage.ru_nswap  - initial_usage.ru_nswap);
  }
#endif
  MPI_Finalize();
  _exit(0);
}


/* Initialize MPI.  Only rank 0 returns to the caller. */
void
jm_initialize_slaves (void)
{
  int dummy_argc = 1;                  /* Fake argc for MPI_Init() */
  char **dummy_argv;                   /* Fake argv for MPI_Init() */
  char *dummy_argv_data[] = {"jumbomem", NULL};   /* Contents of the above */
  unsigned long min_memory;            /* Minimum free memory on any rank */

  /* Common initialization */
  if (jm_globals.debuglevel >= 3) {
    /* Ideally, only rank 0 should output the initialization message. */
    char *rankstr = getenv("JM_EXPECTED_RANK");  /* Computed by the wrapper script */
    if (!rankstr || atoi(rankstr) == 0)
      jm_debug_printf(3, "slaves_mpi is initializing.\n");
  }
  dummy_argv = dummy_argv_data;
  jm_exit_critical_section();         /* Enable MPI_Init() to spawn threads. */
  jm_globals.is_internal = 1;         /* MPI_Init() threads should use internal malloc() and friends. */
  MPI_Init(&dummy_argc, &dummy_argv);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  if (rank == 0)
    jm_debug_printf(2, "The master task is running on %s.\n", jm_hostname());
  else
    jm_debug_printf(3, "Slave #%d is running on %s.\n", rank, jm_hostname());

  /* Ensure that the master and slaves agree upon the logical page
   * size to use. */
  MPI_Bcast((void *)&jm_globals.pagesize, 1, MPI_UNSIGNED_LONG, 0, MPI_COMM_WORLD);

  /* Determine the minimum amount of memory that any slave can manage. */
  if (rank == 0)
    jm_globals.slavebytes = (size_t)(-1);    /* The master's memory is independent of the slaves'. */
  else {
    /* Allocate as much memory as we can. */
    do {
      buffer = (char *) valloc(jm_globals.slavebytes);
      if (!buffer) {
        jm_debug_printf(4, "Failed to allocate %ld bytes of memory (%s).\n",
                        jm_globals.slavebytes, jm_strerror(errno));
        jm_globals.slavebytes -= jm_globals.pagesize;
      }
    }
    while (!buffer && jm_globals.slavebytes > jm_globals.pagesize);
    if (!buffer)
      /* Produce an error message and abort. */
      buffer = (char *) jm_valloc(jm_globals.slavebytes);
    jm_debug_printf(3, "Slave #%d can use at most %lu bytes of memory.\n",
                    rank, jm_globals.slavebytes);
  }
  MPI_Allreduce((void *)&jm_globals.slavebytes, (void *)&min_memory, 1, MPI_UNSIGNED_LONG, MPI_MIN, MPI_COMM_WORLD);
  jm_globals.slavebytes = min_memory;
  if (jm_globals.slavebytes == (size_t)(-1)) {
    /* There must not be any slaves. */
    jm_globals.numslaves = 0;
    jm_globals.is_internal = 0;
    return;
  }

  /* Reduce jm_globals.slavebytes by the number of bytes that fault
   * when touching each page. */
  if (jm_getenv_boolean("JM_REDUCEMEM") == 1) {
    if (rank == 0)
      jm_debug_printf(3, "Determining if using %lu bytes/slave leads to major page faults...\n",
                      jm_globals.slavebytes);
    else {
      struct rusage usage0, usage1;   /* Before and after resource usage */
      long int newfaults;             /* Newly observed major page faults */
      size_t i;

      /* Touch every page once to load every page into memory. */
      for (i=0; i<jm_globals.slavebytes; i+=jm_globals.ospagesize)
        buffer[i] = 0;

      /* Touch every page again to determine how many pages actually
       * fit into memory. */
      getrusage(RUSAGE_SELF, &usage0);
      for (i=0; i<jm_globals.slavebytes; i+=jm_globals.ospagesize)
        buffer[i] = 0;
      getrusage(RUSAGE_SELF, &usage1);
      newfaults = usage1.ru_majflt - usage0.ru_majflt;

      /* Reduce jm_globals.slavebytes accordingly. */
      if (newfaults) {
        jm_debug_printf(3, "Slave #%d saw %ld major page faults on %lu bytes of memory.\n",
                        rank, newfaults, jm_globals.slavebytes);
        jm_globals.slavebytes -= newfaults*jm_globals.ospagesize;
      }
    }
    MPI_Allreduce((void *)&jm_globals.slavebytes, (void *)&min_memory, 1, MPI_UNSIGNED_LONG, MPI_MIN, MPI_COMM_WORLD);
    if (rank == 0) {
      if (jm_globals.slavebytes != min_memory)
        jm_debug_printf(2, "Reducing per-slave memory from %lu bytes to %lu bytes.\n",
                        jm_globals.slavebytes, min_memory);
      else
        jm_debug_printf(3, "No slave observed any major page faults.\n");
    }
    jm_globals.slavebytes = min_memory;
  }

  /* Perform more initialization specific to either the master or slaves. */
  if (rank == 0) {
    int i;

    /* We're the master -- determine the number of slaves we're managing. */
    MPI_Comm_size(MPI_COMM_WORLD, (int *)&jm_globals.numslaves);
    jm_globals.numslaves--;              /* Rank 0 isn't a slave. */
    for (i=0; i<MAX_PENDING_FETCHES; i++)
      fetch_state[i].valid = 0;
    jm_globals.is_internal = 0;
    jm_enter_critical_section();    /* Re-take the lock because jm_initialize_all() will release it. */
    return;
  }
  else {
    /* We're a slave -- lock our buffer into memory then enter our
     * command loop and never return. */
    jm_globals.is_internal = 1;
    if (jm_mlock((void *)buffer, jm_globals.slavebytes) == -1)
      jm_debug_printf(5, "mlock(%p, %lu) failed (%s)\n",
		      buffer, jm_globals.slavebytes, jm_strerror(errno));
#ifdef JM_DEBUG
    if (jm_globals.debuglevel >= 3)
      getrusage(RUSAGE_SELF, &initial_usage);
#endif
    slave_event_loop();
  }
}


/* Start evicting a given page. */
void *
jm_evict_begin (char *evict_addr, char *evict_buffer)
{
  static size_t put_offset;  /* Slave buffer offset to which to put a page */
  int put_slave;             /* Slave to which to put a page */
  EVICT_STATE *state;        /* Current state for the asynchronous operation */
  int i;

  /* Announce what we're about to do. */
  jm_debug_printf(4, "Evicting the page at address %p.\n", evict_addr);

  /* Acquire new internal state. */
  for (i=0; i<MAX_PENDING_EVICTIONS; i++) {
    state = &evict_state[i];
    if (!state->valid) {
      state->valid = 1;
      state->address = evict_addr;
      break;
    }
  }
  if (i == MAX_PENDING_EVICTIONS)
    jm_abort("Too many evictions (%ld) are concurrently outstanding", MAX_PENDING_EVICTIONS+1);

  /* Begin the page eviction. */
  put_slave = (int)GET_SLAVE_NUM(evict_addr);
  put_offset = TO_NETWORK(GET_SLAVE_OFFSET(evict_addr));
  MPI_Isend((void *)&put_offset, sizeof(size_t), MPI_BYTE, put_slave+1,
            JM_MPI_PUT_OFFSET, MPI_COMM_WORLD, &state->requests[0]);
  MPI_Isend((void *)evict_buffer, (int)jm_globals.pagesize, MPI_BYTE, put_slave+1,
            JM_MPI_PUT_DATA, MPI_COMM_WORLD, &state->requests[1]);

  /* Return a pointer to our fetch state. */
  return (void *) state;
}


/* Finish evicting a given page. */
void
jm_evict_end (void *stateobj)
{
  EVICT_STATE *state = (EVICT_STATE *) stateobj;

  /* Announce what we're about to do. */
  jm_debug_printf(4, "Completing the eviction of the page at address %p.\n", state->address);

  /* Block until the page is completely evicted. */
  MPI_Waitall(2, state->requests, MPI_STATUSES_IGNORE);
  state->valid = 0;

  /* Announce what we're about to do. */
  jm_debug_printf(4, "Finished evicting the page at address %p.\n", state->address);
}


/* Start fetching a given page. */
void *
jm_fetch_begin (char *fetch_addr, char *fetch_buffer)
{
  static size_t get_offset;  /* Slave buffer offset from which to get a page */
  int get_slave;             /* Slave from which to get a page */
  FETCH_STATE *state;        /* Current state for the asynchronous operation */
  int i;

  /* Announce what we're about to do. */
  jm_debug_printf(4, "Fetching the page at address %p.\n", fetch_addr);

  /* Acquire new internal state. */
  for (i=0; i<MAX_PENDING_FETCHES; i++) {
    state = &fetch_state[i];
    if (!state->valid) {
      state->valid = 1;
      state->address = fetch_addr;
      break;
    }
  }
  if (i == MAX_PENDING_FETCHES)
    jm_abort("Too many fetches (%ld) are concurrently outstanding", MAX_PENDING_FETCHES+1);

  /* Fetch the given page from a slave. */
  get_slave = (int)GET_SLAVE_NUM(fetch_addr);
  MPI_Irecv((void *)fetch_buffer, (int)jm_globals.pagesize, MPI_BYTE, get_slave+1,
            JM_MPI_RESPONSE, MPI_COMM_WORLD, &state->request);
  get_offset = TO_NETWORK(GET_SLAVE_OFFSET(fetch_addr));
  MPI_Send((void *)&get_offset, sizeof(size_t), MPI_BYTE, get_slave+1,
           JM_MPI_GET, MPI_COMM_WORLD);

  /* Return a pointer to our fetch state. */
  return (void *) state;
}


/* Finish fetching a given page. */
void
jm_fetch_end (void *stateobj)
{
  FETCH_STATE *state = (FETCH_STATE *) stateobj;

  /* Announce what we're about to do. */
  jm_debug_printf(4, "Waiting for the page at address %p.\n", state->address);

  /* Block until the fetched page arrives. */
  MPI_Wait(&state->request, MPI_STATUS_IGNORE);
  state->valid = 0;

  /* Announce what we just did. */
  jm_debug_printf(4, "Finished waiting for the page at address %p.\n", state->address);
}


/* Shut down cleanly. */
void
jm_finalize_slaves (void)
{
  unsigned int i;

  /* Send each slave a shutdown message. */
  for (i=0; i<jm_globals.numslaves; i++)
    MPI_Send("", 0, MPI_BYTE, i+1, JM_MPI_TERMINATE, MPI_COMM_WORLD);

  /* Shut down MPI. */
  jm_globals.is_internal = 1;   /* All MPI_Finalize() memory allocation should use internal routines. */
  jm_exit_critical_section();
  MPI_Finalize();
  jm_enter_critical_section();
  jm_globals.is_internal = 0;
}
