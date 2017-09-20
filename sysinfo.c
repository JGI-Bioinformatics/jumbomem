/*------------------------------------------------------------
 * JumboMem memory server: Provide system information using
 * system-specific mechanisms
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
#ifdef HAVE_GETTID_SYSCALL
# include <sys/syscall.h>
#endif

/* Enable the meminfo filename to be overridden at compile time. */
#ifndef MEMINFO_FILE
# define MEMINFO_FILE "/proc/meminfo"
#endif

/* Enable the max_map_count filename to be overridden at compile time. */
#ifndef MAPCOUNT_FILE
# define MAPCOUNT_FILE "/proc/sys/vm/max_map_count"
#endif

/* Specify the maximum line length to consider. */
#define MAX_LINE_LEN 1024


/* Reduce a number of bytes by either an absolute amount or a
 * percentage, aborting if the number drops below zero. */
static size_t
reserve_memory (size_t memsize, size_t absreserve, double pctreserve)
{
  /* Reduce by an absolute number of bytes. */
  if (absreserve > 0) {
    if (memsize < absreserve)
      jm_abort("Reducing %lu bytes of memory by %lu bytes would result in a negative amount of memory",
               memsize, absreserve);
    jm_debug_printf(4, "Reducing available memory reported from %lu bytes to %lu bytes.\n",
                    memsize, memsize-absreserve);
    return memsize - absreserve;
  }

  /* Reduce by a byte percentage, which may be zero. */
  if (pctreserve > 100.0)
    jm_abort("Reducing %lu bytes of memory by %.10g%% would result in a negative amount of memory",
             memsize, pctreserve);
  jm_debug_printf(4, "Reducing available memory reported from %lu bytes to %lu bytes.\n",
                  memsize, (size_t) (memsize - memsize*pctreserve/100.0));
  return (size_t) (memsize - memsize*pctreserve/100.0);
}


/* Given a list of alternating key strings and value pointers, search
 * MEMINFO_FILE for the keys and return the corresponding values (or
 * -1 if not found).  Values are automatically scaled from kilobytes
 * to bytes. */
void
jm_parse_meminfo_file (int numpairs, ...)
{
  va_list args;                 /* Argument list */
  char **keys;                  /* Array of numpairs keys */
  ssize_t **values;             /* Array of numpairs value pointers */
  FILE *meminfo;                /* Handle to MEMINFO_FILE */
  char oneline[MAX_LINE_LEN];   /* One line read from MEMINFO_FILE */
  int i;

  /* Convert the argument list to a pair of arrays. */
  keys = (char **) jm_malloc(numpairs*sizeof(char *));
  values = (ssize_t **) jm_malloc(numpairs*sizeof(ssize_t *));
  va_start(args, numpairs);
  for (i=0; i<numpairs; i++) {
    keys[i] = va_arg(args, char *);
    values[i] = va_arg(args, ssize_t *);
    *values[i] = -1;
  }
  va_end(args);

  /* Loop over each line of the file. */
  if (!(meminfo=fopen(MEMINFO_FILE, "r"))) {
    jm_debug_printf(5, "Unable to open %s (%s).\n", MEMINFO_FILE, jm_strerror(errno));
    jm_free(values);
    jm_free(keys);
    return;
  }
  while (fgets(oneline, MAX_LINE_LEN, meminfo)) {
    /* Try each key in turn. */
    for (i=0; i<numpairs; i++) {
      size_t keylen = strlen(keys[i]);
      if (!strncmp(oneline, keys[i], keylen)) {
        char *endptr;             /* Pointer to first non-digit */

        *values[i] = (size_t) strtol(oneline+keylen, &endptr, 10);
        if ((size_t)*values[i] != (size_t)(-1) && !strncmp(endptr, " kB", 3)) {
          *values[i] *= 1024;   /* Convert from kilobytes to bytes. */
          break;
        }
        else
          *values[i] = -1;
      }
    }
  }
  fclose(meminfo);
  jm_free(values);
  jm_free(keys);
  return;
}


/* Return the physical page size. */
size_t
jm_get_page_size (void)
{
  static size_t pagesize = 0;      /* Page size in bytes */

  /* First attempt: Return the previously observed page size. */
  if (pagesize)
    return pagesize;

  /* Second attempt: Use sysconf() to determine the page size. */
#ifdef _SC_PAGESIZE
  pagesize = (size_t) sysconf(_SC_PAGESIZE);
#elif defined(_SC_PAGE_SIZE)
  pagesize = (size_t) sysconf(_SC_PAGE_SIZE);
#endif
  if (pagesize == (size_t)(-1L))
    pagesize = 0;
  else
    return pagesize;

  /* Third attempt: Use getpagesize() to determine the page size. */
  return getpagesize();
}


/* Parse the kernel meminfo file to return the amount of free memory
 * we can use. */
size_t
jm_get_available_memory_size (void)
{
  size_t physmem_size = 0;        /* Size of physical memory in bytes */
  ssize_t memfree;                /* MemFree key from MEMINFO_FILE */
  ssize_t buffers;                /* Buffers key from MEMINFO_FILE */
  ssize_t cached;                 /* Cached key from MEMINFO_FILE */
  static size_t reservemem = 0;   /* Bytes of memory to skim off the result */
  static double reservemem_pct = 0.0;  /* Percentage of memory to skim off the result */
  static int have_reservemem = 0; /* 1=reservemem and reservemem_pct are value; 0=invalid */

  /* Set reservemem or reservemem_pct if we haven't already done so. */
  if (!have_reservemem) {
    char *reservemem_str = getenv("JM_RESERVEMEM");   /* String specifying memory to reserve */

    if (reservemem_str) {
      if (strchr(reservemem_str, '%')) {
        /* The user specified a percentage of available memory to reserve. */
        char *endptr;    /* Pointer to the first character that's not part of a number */
        reservemem_pct = strtod(reservemem_str, &endptr);
        if (*endptr != '\0' && *endptr != '%')
          jm_abort("Unable to parse \"%s\" as a percentage", reservemem_str);
        if (reservemem_pct < 0.0)
          jm_abort("JM_RESERVEMEM must be nonnegative (was \"%s\")", reservemem_str);
      }
      else
        /* The user specified an absolute amount of memory to reserve. */
        reservemem = (size_t) jm_getenv_nonnegative_int("JM_RESERVEMEM");
    }
    have_reservemem = 1;
  }

  /* First attempt: Read /proc/meminfo (or whatever MEMINFO_FILE is
   * defined as) and return MemFree+Buffers+Cached as an estimate of
   * free memory. */
  jm_parse_meminfo_file(3,
                        "MemFree:", &memfree,
                        "Buffers:", &buffers,
                        "Cached:",  &cached);

  if (memfree>=0 && buffers>=0 && cached>=0) {
    physmem_size = (size_t) (memfree + buffers + cached);
    return reserve_memory(physmem_size, reservemem, reservemem_pct);
  }

  /* Second attempt: Use sysconf() to read the number of available
   * pages of physical memory and multiply that by the page size. */
#ifdef _SC_AVPHYS_PAGES
  physmem_size = (size_t) sysconf(_SC_AVPHYS_PAGES);
  if (physmem_size == (size_t)(-1L))
    physmem_size = 0;
  else {
    physmem_size *= jm_get_page_size();
    return reserve_memory(physmem_size, reservemem, reservemem_pct);
  }
#endif

  /* Third attempt: Tell the user we need him to specify explicitly
   * the amount of available memory. */
  jm_abort("Failed to determine the available physical memory; JM_SLAVEMEM and either JM_MASTERMEM or JM_LOCAL_PAGES need to be set explicitly");
  return 0;    /* Prevent the compiler from complaining. */
}


/* Return the maximum number of mappings available to a process or
 * zero if indeterminate. */
unsigned long
jm_get_maximum_map_count (void)
{
  FILE *map_count_file;         /* File listing the maximum number of memory maps */
  unsigned long max_map_count = 0;  /* Maximum number of memory maps per process */

  if (!(map_count_file=fopen(MAPCOUNT_FILE, "r")))
    return 0;
  (void) fscanf(map_count_file, "%lu", &max_map_count);
  fclose(map_count_file);
  return max_map_count;
}


/* Return the minimum page size that's safe for JumboMem to use (i.e.,
 * that can't cause mmap() to run out of mappings).  Return 0 if we're
 * unable to determine the minimum page size. */
size_t
jm_get_minimum_jm_page_size (void)
{
  unsigned long max_map_count = 0;  /* Maximum number of memory maps per process */
  size_t physmem;               /* Available physical memory */
  static size_t min_page_size = 0;  /* Minimum "safe" JumboMem page size */

  /* First attempt: See if we've already been called. */
  if (min_page_size)
    return min_page_size;

  /* Second attempt: Read the contents of MAPCOUNT_FILE (a single
   * ASCII integer). */
  max_map_count = jm_get_maximum_map_count();
  if (max_map_count < 1)
    /* Give up.  The caller will use the OS page size as a -- probably
     * incorrect -- estimate. */
    return 0;

  /* Recommend as the minimum page size the total address space
   * divided by the number of maps, rounded up to the nearest multiple
   * of the OS page size.
   *
   * The worst case is alternating mapped and unmapped pages.  For
   * example, if we had a total of sixteen 4096-byte pages of memory
   * (for a total of 65536 bytes) and the ability to hold seven
   * mappings at once, then 8192-byte JumboMem pages may fail because
   * the OS needs to maintain eight mappings in the worst case:
   *
   *  1   2   3   4   5   6   7   8
   * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   * |X|X| | |X|X| | |X|X| | |X|X| | |
   * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   *
   * Our formula tells us that 65536/7 rounded up to the next multiple
   * of 4096 is 12288, which requires only five mappings in the worst
   * case (although the last OS page can't be used by JumboMem):
   *
   *  1     2     3     4     5
   * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   * |X|X|X| | | |X|X|X| | | |X|X|X| |
   * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   */
  physmem = jm_get_available_memory_size();
  min_page_size = ((physmem/max_map_count+jm_globals.ospagesize-1) / jm_globals.ospagesize) * jm_globals.ospagesize;
  if (min_page_size < jm_globals.ospagesize)
    min_page_size = jm_globals.ospagesize;
  return min_page_size;
}


/* Implement gettid(), which returns the caller's thread (LWP) ID. */
#ifndef HAVE_GETTID
pid_t
gettid (void)
{
# ifdef HAVE_GETTID_SYSCALL
  return (pid_t) syscall(__NR_gettid);
# else
  return (pid_t) (-1);
# endif
}
#endif


/* Given a thread (LWP) ID, return the thread's current run state or
 * '?' if the state could not be determined.  Valid values on Linux
 * (as of 2.6.18) include the following:
 *
 *   - R (running)
 *   - S (sleeping; interruptible)
 *   - D (disk sleep; uninterruptable)
 *   - Z (zombie)
 *   - T (traced or stopped on a signal)
 *   - W (paging)
 */
char jm_get_thread_state (pid_t tid)
{
  char statfilename[35];      /* The string "/proc/<tid>/stat" for some tid */
  int statfile;               /* Handle to /proc/<tid>/stat */
  char tidstate = '?';        /* Run state of the given thread */
  static char *statdata = NULL;   /* Pointer to the stat file's data */
  ssize_t bytesread;          /* Number of bytes read from the stat file */
  char *lastparen;            /* Pointer to the last ")" in the stat file */
  const size_t maxstatbytes = /* Maximum size of a stat file in bytes */
    1                             /* 1 char */
    + NAME_MAX + 2                /* 1 string (parenthesized process name) */
    +  7 * 20                     /* 7 signed longs */
    +  8 * 11                     /* 8 signed ints */
    + 24 * 20                     /* 24 unsigned longs */
    + 40                          /* 40 spaces separating 41 fields */
    + 256;                        /* Fudge factor for future growth */

  /* Open /proc/<tid>/stat. */
  if (tid == -1)
    goto have_state;
  sprintf(statfilename, "/proc/%d/stat", (int)tid);
  if ((statfile=open(statfilename, O_RDONLY)) == -1)
    goto have_state;

  /* Allocate memory if necessary. */
  if (!statdata)
    if (!(statdata = (char *) malloc(maxstatbytes + 1)))
      goto close_file;

  /* Read the entire file into memory. */
  if ((bytesread=read(statfile, statdata, maxstatbytes)) == -1)
    goto close_file;
  statdata[bytesread] = '\0';

  /* Search backwards for the file's final ")". */
  if (!(lastparen=strrchr(statdata, ')')))
    goto close_file;
  if (lastparen[1] && lastparen[2])  /* We expect a space and a status code. */
    tidstate = lastparen[2];

  /* Undo various operations and exit. */
 close_file:
  (void) close(statfile);
 have_state:
  return tidstate;
}
