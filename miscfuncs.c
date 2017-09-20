/*------------------------------------------------------------
 * JumboMem memory server: Miscellaneous functions
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
#include <sys/time.h>
#include <time.h>

/* Define the maximum number of characters in a formatted number */
#ifndef MAX_NUMBER_WIDTH
# define MAX_NUMBER_WIDTH 128
#endif

/* Define the maximum number of concurrent calls to
 * jm_format_power_of_2() (i.e., from within the same call to
 * printf()). */
#define MAX_NUMBER_BUFFERS 4


/* Output an error message and abort the program. */
#ifdef __GNUC__
__attribute__ ((noreturn))
#endif
void jm_abort (const char *format, ...)
{
  va_list args;                      /* Variadic argument list */
  static int recursive_abort = 0;    /* 0=normal abort; 1=we're already aborting */

  jm_globals.error_exit = 1;
  JM_ENTER();
  switch (++recursive_abort) {
    case 1:
      /* First time jm_abort() is called -- give a message and exit. */
      va_start(args, format);
      fprintf(stderr, "%s: ", jm_globals.progname ? jm_globals.progname : "JumboMem");
      vfprintf(stderr, format, args);
      fprintf(stderr, "\n");
      va_end(args);
      jm_finalize_all();
      jm_set_internal_depth(0);
      break;

    case 2:
      /* Second time jm_abort() is called (shouldn't happen) --
       * finalize and exit without outputting anything. */
      va_start(args, format);
      va_end(args);
      jm_finalize_all();
      jm_set_internal_depth(0);
      break;

    case 3:
      /* Third time jm_abort() is called (really shouldn't happen) --
       * just call _exit(). */
      va_start(args, format);
      va_end(args);
      jm_set_internal_depth(0);
      break;

    case 4:
      /* We're starting to get desperate -- terminate the process. */
      kill(getpid(), SIGTERM);
      jm_set_internal_depth(0);
      break;

    default:
      /* Alright, we've had enough -- forcefully kill the process. */
      kill(getpid(), SIGKILL);
      jm_set_internal_depth(0);
      break;
  }
  _exit(1);
  exit(1);   /* Some versions of gcc look for exit() in a noreturn function. */
}


/* Parse an environment variable into a positive integer.  Return 0 if
 * the environment variable is not defined.  Fail if the environment
 * variable is defined but is not a positive integer. */
size_t
jm_getenv_positive_int (const char *envvar)
{
  char *envvar_string;    /* String value of envvar */
  char *endptr;           /* Pointer to the first non-digit in envvar */
  size_t envvar_value;    /* Numerical value of envvar */

  if ((envvar_string=getenv(envvar))) {
    envvar_value = (size_t) strtol(envvar_string, &endptr, 0);
    if (envvar_value <= 0 || *endptr != '\0')
      jm_abort("%s must be a positive integer (was \"%s\")", envvar, envvar_string);
    else
      return envvar_value;
  }
  return 0;
}


/* Parse an environment variable into a nonnegative integer.  Return
 * -1 if the environment variable is not defined.  Fail if the
 * environment variable is defined but is not a nonnegative
 * integer. */
ssize_t
jm_getenv_nonnegative_int (const char *envvar)
{
  char *envvar_string;    /* String value of envvar */
  char *endptr;           /* Pointer to the first non-digit in envvar */
  ssize_t envvar_value;   /* Numerical value of envvar */

  if ((envvar_string=getenv(envvar))) {
    envvar_value = (ssize_t) strtol(envvar_string, &endptr, 0);
    if (envvar_value < 0 || *endptr != '\0')
      jm_abort("%s must be a nonnegative integer (was \"%s\")", envvar, envvar_string);
    else
      return envvar_value;
  }
  return -1;
}


/* Parse an environment variable into a nonnegative integer or a
 * percentage of a given number.  Return -1 if the environment
 * variable is not defined.  Fail if the environment variable is
 * defined but is neither a nonnegative integer nor a valid
 * percentage. */
ssize_t
jm_getenv_nonnegative_int_or_percent (const char *envvar, ssize_t base_amount)
{
  char *envvar_string;               /* String value of envvar */

  if ((envvar_string=getenv(envvar))) {
    if (strchr(envvar_string, '%')) {
      /* The user specified a percentage of base_amount to use. */
      char *endptr;    /* Pointer to the first character that's not part of a number */
      double percent;                /* Percent of maximum page count to use */

      percent = strtod(envvar_string, &endptr);
      if (*endptr != '\0' && *endptr != '%')
        jm_abort("Unable to parse \"%s\" as a percentage", envvar_string);
      if (percent < 0.0)
        jm_abort("%s must be nonnegative (was \"%s\")", envvar, envvar_string);
      return (ssize_t) (base_amount * percent / 100.0);
    }
    else
      /* The user specified an absolute number of pages to use. */
      return jm_getenv_positive_int(envvar);
  }
  return -1;
}


/* Parse an environment variable into a 0 (false) or 1 (true).  Return
 * -1 if the environment variable is not defined.  Fail if the
 * environment variable is defined but is not a valid boolean
 * value. */
int
jm_getenv_boolean (const char *envvar)
{
  char *envvar_string;    /* String value of envvar */
  const char *true_values  = "1yYtT";   /* Characters meaning "true" */
  const char *false_values = "0nNfF";   /* Characters meaning "false" */
  const char *c;

  if (!(envvar_string=getenv(envvar)))  /* String was not found. */
    return -1;
  if (envvar_string[0] == '\0')         /* Empty means "true". */
    return 1;
  for (c=true_values; *c; c++)          /* Check for "true". */
    if (envvar_string[0] == *c)
      return 1;
  for (c=false_values; *c; c++)         /* Check for "false". */
    if (envvar_string[0] == *c)
      return 0;
  jm_abort("\"%s\" is not a valid boolean value for %s\n", envvar_string, envvar);
  return -1;      /* Pacify the compiler. */
}


/* Allocate memory and abort on failure. */
void *
jm_malloc (size_t numbytes)
{
  void *buffer;         /* Buffer to return */

  buffer = malloc(numbytes);
  if (!buffer)
    jm_abort("Failed to allocate %ld bytes of memory (%s)", numbytes, jm_strerror(errno));
  return buffer;
}


/* Reallocate memory and abort on failure. */
void *
jm_realloc (void *ptr, size_t numbytes)
{
  void *buffer;         /* Buffer to return */

  buffer = realloc(ptr, numbytes);
  if (!buffer)
    jm_abort("Failed to reallocate %ld bytes of memory (%s)", numbytes, jm_strerror(errno));
  return buffer;
}


/* Allocate page-aligned memory and abort on failure. */
void *
jm_valloc (size_t numbytes)
{
  void *buffer;         /* Buffer to return */

  buffer = valloc(numbytes);
  if (!buffer)
    jm_abort("Failed to allocate %ld bytes of memory (%s)", numbytes, jm_strerror(errno));
  return buffer;
}


/* Free previously allocated memory. */
void
jm_free (void *buffer)
{
  if (buffer)
    free(buffer);
}


/* Lock addresses into RAM, but only if JM_MLOCK is true. */
int
jm_mlock (const void *addr, size_t len)
{
  static int use_mlock = -2;   /* -2=unknown; -1=false; 0=false; 1=true */

  if (use_mlock == -2)
    use_mlock = jm_getenv_boolean("JM_MLOCK");
  if (use_mlock == 1)
    return mlock(addr, len);
  errno = ENOSYS;       /* "Function not implemented" */
  return -1;
}


/* Unlock addresses from RAM, but only if JM_MLOCK is true. */
int
jm_munlock (const void *addr, size_t len)
{
  static int use_mlock = -2;   /* -2=unknown; -1=false; 0=false; 1=true */

  if (use_mlock == -2)
    use_mlock = jm_getenv_boolean("JM_MLOCK");
  if (use_mlock == 1)
    return munlock(addr, len);
  errno = ENOSYS;       /* "Function not implemented" */
  return -1;
}


/* Output a message to standard error if JM_DEBUG is greater than or
 * equal to a given number. */
#ifdef JM_DEBUG
void
jm_debug_printf_internal (char *filename, int lineno, int level,
                          const char *format, ...)
{
  va_list args;      /* Argument list */

  va_start (args, format);
  if (jm_globals.debuglevel >= level) {
    pid_t tid = gettid();     /* Current thread ID or -1 if not implemented */

    if (tid == -1)
      (void) fprintf(stderr, "JM_DEBUG (%s:%d [%s]): ", filename, lineno, jm_hostname());
    else
      (void) fprintf(stderr, "JM_DEBUG (%s:%d [%s:%d]): ", filename, lineno, jm_hostname(), tid);
    (void) vfprintf(stderr, format, args);
  }
  va_end (args);
}
#endif


/* Return the hostname as a static string. */
char *
jm_hostname (void)
{
  static char hostname[1025];  /* Note: Initialized to all zeroes */

  if (!hostname[0]) {
    char *firstdot;            /* Pointer to first "." character */

    if (gethostname(hostname, 1025) == -1)
      jm_abort("hostname(): %s", jm_strerror(errno));
    hostname[1024] = '\0';    /* Some systems don't null-terminate. */
    if ((firstdot=strchr(hostname, '.')))
      *firstdot = '\0';       /* Truncate at the first "." character. */
  }
  return hostname;
}


/* Read a microsecond timer and return its current value. */
uint64_t
jm_current_time (void)
{
  struct timeval now;

  if (gettimeofday(&now, NULL) == -1)
    jm_abort("gettimeofday(): %s", jm_strerror(errno));
  return 1000000*(uint64_t)now.tv_sec + (uint64_t)now.tv_usec;
}


/* Convert an error number to a string without relying on malloc()'ed
 * memory. */
const char *
jm_strerror (int errnum)
{
  if (errnum < sys_nerr)
    return sys_errlist[errnum];
  return "Unknown error";
}


/* Format a number as a string with a power-of-two suffix and "digits"
 * digits after the decimal point.  This function returns a pointer to
 * a static string.  However, for the convenience of calling
 * jm_format_power_of_2() multiple times from a single printf(), the
 * function in fact cycles through a set of static strings. */
char *
jm_format_power_of_2 (uint64_t number, int digits)
{
  const char *suffix_list = " KMGTPEZY";    /* List of possible suffixes */
  const char *suffix = suffix_list;         /* Suffix to use */
  double scaled_num = (double)number;       /* Number scaled to [0, 1023] */
  static char buffer[MAX_NUMBER_BUFFERS][MAX_NUMBER_WIDTH];   /* Formatted string to return */
  static int bufnum = -1;                   /* Index of the buffer to use */

  /* Keep dividing by 1024 until we're in the range [0, 1023]. */
  while (scaled_num >= 1024.0 && *suffix) {
    scaled_num /= 1024.0;
    suffix++;
  }

  /* Format the number and return it. */
  bufnum = (bufnum + 1) % MAX_NUMBER_BUFFERS;   /* Select the next buffer */
  if (*suffix == ' ')
    snprintf(buffer[bufnum], MAX_NUMBER_WIDTH, "%.*f", digits, scaled_num);
  else
    snprintf(buffer[bufnum], MAX_NUMBER_WIDTH, "%.*f%c", digits, scaled_num, *suffix);
  return buffer[bufnum];
}


/* Assign backing store to a region of memory. */
void
jm_assign_backing_store (char *baseaddr, size_t numbytes, int protflags)
{
  /* Map the page using mmap(). */
  JM_RECORD_CYCLE("Calling mmap()");
  if (mmap((void *)baseaddr, numbytes, protflags,
           MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED|MAP_POPULATE, 0, 0) == MAP_FAILED)
    jm_abort("Failed to assign backing store to %lu bytes of address space (%s)",
             numbytes, jm_strerror(errno));
  JM_RECORD_CYCLE("Called mmap()");

  /* Attempt to lock the page in memory using mlock(), ignoring errors. */
  JM_RECORD_CYCLE("Calling mlock()");
  if (jm_mlock ((void *)baseaddr, numbytes) == -1)
    jm_debug_printf(5, "mlock(%p, %lu) failed (%s)\n", baseaddr, numbytes, jm_strerror(errno));
  JM_RECORD_CYCLE("Called mlock()");
}


/* Remove backing store from a region of memory.  We don't explicitly
 * munlock() the region because that ought to be implied by
 * munmap()'ing it.  (At least, I _hope_ that's the case.) */
void
jm_remove_backing_store (char *baseaddr, size_t numbytes)
{
  JM_RECORD_CYCLE("Calling munmap()");
  if (munmap((void *)baseaddr, numbytes) == -1)
    jm_abort("Failed to remove backing store from %ld bytes of address space (%s)",
             numbytes, jm_strerror(errno));
  JM_RECORD_CYCLE("Called munmap()");
}


/* Touch a range of addresses to fault them into the local cache.
 * This function should not be called while the fault handler is
 * active. */
void
jm_touch_memory_region (const char *baseaddr, size_t numbytes)
{
  const char *bufptr;       /* Pointer into baseaddr */
  const unsigned long miniters = 3;  /* Minimum number of times to walk the region */
  int valid_test;           /* 1=jm_page_is_resident() works; 0=always returns -1 */
  int keepgoing;            /* 1=do another iteration; 0=stop */
  unsigned long nonresident;  /* Number of pages that needed to be faulted in */
  unsigned long prev_nonresident;  /* Previous value of nonresident */
  unsigned long i;

  /* Do nothing if any part of the address range is out of the
   * JumboMem memory region. */
  if (baseaddr < jm_globals.memregion
      || baseaddr+numbytes >= jm_globals.memregion+jm_globals.extent)
    return;

  /* There's no point in touching more memory than can be cached locally. */
  if (numbytes > jm_globals.local_pages*jm_globals.pagesize)
    numbytes = jm_globals.local_pages*jm_globals.pagesize;

  /* Repeatedly walk the region in an attempt to overcome randomness
   * in the page-replacement policy that may evict pages we'd prefer
   * to keep. */
  baseaddr = (const char *) (((uintptr_t)baseaddr/jm_globals.pagesize)*jm_globals.pagesize);  /* Round down to a page boundary. */
  valid_test = jm_page_is_resident((char *)baseaddr, NULL) != -1;
  nonresident = numbytes;   /* Assume all pages are nonresident. */
  for (i=0, keepgoing=1; keepgoing; i++) {
    const char *lastaddr;
    lastaddr = baseaddr + (numbytes/jm_globals.pagesize+1)*jm_globals.pagesize;
    if (lastaddr >= jm_globals.memregion + jm_globals.extent)
      lastaddr -= jm_globals.pagesize;
    prev_nonresident = nonresident;
    nonresident = 0;

    /* Walk the region in reverse order so that the beginning part of
     * the buffer is the most likely to be resident in the local
     * cache. */
    for (bufptr=lastaddr;
         bufptr>=(const char *)baseaddr;
         bufptr-=jm_globals.pagesize) {
      if (!jm_page_is_resident((char *)bufptr, NULL))
        nonresident++;
      jm_globals.dummy += *bufptr;
    }

    /* Determine if we should stop or perform another iteration. */
    if (valid_test) {
      if (nonresident == 0)
        /* Exit if we successfully made a pass with no page faults. */
        keepgoing = 0;
      else if (nonresident < prev_nonresident)
        /* Do another iteration as long as we're making progress. */
        keepgoing = 1;
      else
        /* Do another iteration if we haven't yet done the minimum. */
        keepgoing = (i < miniters);
    }
    else
      /* We don't have a valid count of nonresident pages so we always
       * perform exactly the minimum number of iterations. */
      keepgoing = (i < miniters);
  }

#ifdef JM_DEBUG
  if (nonresident == 0)
    jm_debug_printf(5, "All pages are resident after %lu %s.\n",
                    i, i==1 ? "iteration" : "iterations");
  else
    jm_debug_printf(5, "Some page may still not be resident after %lu %s.\n",
                    i, i==1 ? "iteration" : "iterations");
#endif
}
