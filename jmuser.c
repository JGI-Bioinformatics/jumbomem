/*---------------------------------------------------------------
 * JumboMem memory server: User interface to JumboMem internals
 *
 * By Scott Pakin <pakin@lanl.gov>
 *---------------------------------------------------------------*/

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

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <dlfcn.h>
#include <sys/types.h>

/* Define a macro that tries to initialize JumboMem and returns RETVAL
 * on failure. */
#define RETURN_IF_NO_JM(RETVAL)                 \
  do {                                          \
    if (!selfhandle) {                          \
      initialize_jmu();                         \
      if (!selfhandle)                          \
        return RETVAL;                          \
    }                                           \
  }                                             \
  while (0)


/* Define a handle to our program and its shared objects (including
 * JumboMem, we hope). */
static void *selfhandle = NULL;

/* Serialize accesses to a critical section of code. */
static void (*jm_enter_critical_section)(void) = NULL;

/* Allow other threads to enter a critical section. */
static void (*jm_exit_critical_section)(void) = NULL;


/* Initialize the interface to JumboMem's internals.  On failure,
 * selfhandle will still be NULL. */
static void
initialize_jmu (void)
{
  if (!(selfhandle=dlopen(NULL, RTLD_LAZY|RTLD_LOCAL)))
    return;
  jm_enter_critical_section = dlsym(selfhandle, "jm_enter_critical_section");
  jm_exit_critical_section  = dlsym(selfhandle, "jm_exit_critical_section");
  if (!jm_enter_critical_section || !jm_exit_critical_section) {
    dlclose(selfhandle);
    selfhandle = NULL;
  }
}

/* ---------------------------------------------------------------------- */

/* Invoke malloc() as if it were called internally by JumboMem. */
void *
jmu_malloc (size_t numbytes)
{
  void *retval;

  RETURN_IF_NO_JM(NULL);
  jm_enter_critical_section();
  retval = malloc(numbytes);
  jm_exit_critical_section();
  return retval;
}


/* Invoke realloc() as if it were called internally by JumboMem. */
void *
jmu_realloc (void *ptr, size_t numbytes)
{
  void *retval;

  RETURN_IF_NO_JM(NULL);
  jm_enter_critical_section();
  retval = realloc(ptr, numbytes);
  jm_exit_critical_section();
  return retval;
}


/* Invoke free() as if it were called internally by JumboMem. */
void
jmu_free (void *ptr)
{
  RETURN_IF_NO_JM();
  jm_enter_critical_section();
  free(ptr);
  jm_exit_critical_section();
}
