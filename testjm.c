/*------------------------------------------------------------------
 * Simple test of the JumboMem memory server
 *
 * By Scott Pakin <pakin@lanl.gov>
 *------------------------------------------------------------------*/

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
#include <unistd.h>
#include <inttypes.h>
#include <pthread.h>

/* Define some global variables. */
size_t numwords;       /* Number or words to allocate */
int *array;            /* Array of words */
int correct_sum;       /* Expected sum of the array's contents */


/* Sum the array. */
void *sum_array (void *arg)
{
  int sum;               /* Sum of the array's contents */
  uintptr_t tid = 1 + (uintptr_t) arg;
  size_t i;

  printf("Summing the array on thread %lu ... ", tid);
  fflush(stdout);
  for (i=0, sum=0; (size_t)i<numwords; i++)
    sum += array[i];
  printf("done.\n");
  if (sum != correct_sum) {
    printf("FAILURE: Expected %d; saw %d on thread %lu\n", correct_sum, sum, tid);
    return (void *)1;
  }
  printf("SUCCESS by thread %lu!\n", tid);
  return NULL;
}


int main (int argc, char *argv[])
{
  long numthreads;                 /* Number of threads to allocate */
  long numbytes;                   /* Number of bytes to allocate */
  pthread_t *thread_ids = NULL;    /* List of all of our child threads */
  uintptr_t exitcode;              /* Status code to return from main() */
  int i;

  /* Parse the command line. */
  if (argc < 2 || argc > 3) {
    fprintf(stderr, "Usage: %s <gibibytes> [<threads>]\n", argv[0]);
    exit(1);
  }
  numbytes = atol(argv[1]);
  if (numbytes <= 0) {
    fprintf(stderr, "%s: The number of gibibytes must be positive\n", argv[0]);
    exit(1);
  }
  numbytes *= 1073741824;
  numwords = (size_t)numbytes / sizeof(int);
  if (argc > 2) {
    numthreads = atol(argv[2]);
    if (numthreads <= 0) {
      fprintf(stderr, "%s: The number of threads must be positive\n", argv[0]);
      exit(1);
    }
  }
  else
    numthreads = 1;

  /* Allocate memory. */
  printf("Allocating %lu bytes of memory ... ", numbytes);
  fflush(stdout);
  if (!(array = (int *) malloc(numbytes))) {
    printf("failed.\n");
    fflush(stdout);
    perror("malloc");
    exit(1);
  }
  printf("done.\n");

  /* Initialize the array. */
  printf("Writing %lu %lu-byte words into an array ... ", numwords, sizeof(int));
  fflush(stdout);
  for (i=0, correct_sum=0; (size_t)i<numwords; i++) {
    array[i] = i + 1;
    correct_sum += i + 1;
  }
  printf("done.\n");

  /* Spawn zero or more array-summing threads. */
  if (numthreads > 1) {
    if (!(thread_ids=malloc((numthreads-1)*sizeof(pthread_t)))) {
      perror("malloc");
      exit(1);
    }
    for (i=0; i<numthreads-1; i++)
      if (pthread_create(&thread_ids[i], NULL, sum_array, (void *)(uintptr_t)(i+1))) {
	fprintf(stderr, "%s: Failed to create thread %d\n", argv[0], i+1);
	exit(1);
      }
  }

  /* Sum the array locally. */
  exitcode = (uintptr_t) sum_array(NULL);

  /* Wait for each child thread to sum the array. */
  for (i=0; i<numthreads-1; i++) {
    void *retval;      /* Child's return value */

    if (pthread_join(thread_ids[i], &retval)) {
      fprintf(stderr, "%s: Failed to join thread %d\n", argv[0], i+1);
      exit(1);
    }
    exitcode |= (uintptr_t) retval;
  }

  /* Deallocate memory and exit. */
  printf("Freeing %lu bytes of memory ... ", numbytes);
  fflush(stdout);
  free(array);
  if (numthreads > 1)
    free(thread_ids);
  printf("done.\n");
  return exitcode;
}
