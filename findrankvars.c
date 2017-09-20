/* ----------------------------------------------------------------------
 * Determine which (if any) environment variables indicate that a
 * process is running on MPI rank 0, for use with the jumbomem
 * script's --rankvar option
 *
 * By Scott Pakin <pakin@lanl.gov>
 * ----------------------------------------------------------------------
 */

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
#include <string.h>
#include <stdlib.h>
#include <mpi.h>

/* Define the maximum number of bytes we'll allow in an environment
 * variable's name. */
#define MAX_ENV_VAR_LEN 65536

/* Define a type to categorize how useful an environment variable is
 * at indicating rank. */
typedef enum {
  NOT_RANK    = 0,   /* Definitely does not correspond to rank */
  GOOD_ENOUGH = 1,   /* Exists only on rank 0 */
  IS_RANK     = 2    /* Matches rank at all processes */
} RANK_TYPE;

int main (int argc, char *argv[], char *envp[])
{
  char keybuffer[MAX_ENV_VAR_LEN+1];   /* Communication buffer for keys */
  int rank;             /* Process's rank in the computation */
  int numranks;         /* Number of ranks in the computation */
  RANK_TYPE looks_like_rank;  /* How well variable indicates rank */

  /* Initialize MPI. */
  MPI_Init(&argc, &argv);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &numranks);
  if (numranks == 1) {
    fprintf(stderr, "%s: This program must be run with at least two processes (and preferably more)\n", argv[0]);
    exit(1);
  }

  /* Search for likely environment-variable candidates. */
  if (rank == 0) {
    /* The master tests each environment variable in turn. */
    char **keyvalue;      /* One key=value pair from the environment */
    int found_any = 0;    /* 1=at least one variable was found; 0=none found */
    printf("The following environment variables seem to identify rank 0:\n");
    for (keyvalue=envp; *keyvalue; keyvalue++) {
      char *key;          /* Copy of only the key from the environment */
      char *value;        /* Copy of only the value from the environment */
      char *equalptr;     /* Pointer to the first "=" in "key=value" */
      RANK_TYPE root_type;    /* The root rank's analysis of an environment variable */

      /* Parse the key=value string into a key and value. */
      key = strdup(*keyvalue);
      if (!(equalptr=strchr(key, '=')))
        /* This should never happen but we check just in case. */
        continue;
      *equalptr = '\0';
      value = equalptr + 1;

      /* Poll each slave to see if the environment variable's value
       * matches its rank when it reads the variable. */
      looks_like_rank = strcmp(value, "0") ? GOOD_ENOUGH : IS_RANK;
      strncpy(keybuffer, key, MAX_ENV_VAR_LEN);
      keybuffer[MAX_ENV_VAR_LEN] = '\0';
      MPI_Bcast(keybuffer, MAX_ENV_VAR_LEN+1, MPI_CHAR, 0, MPI_COMM_WORLD);
      root_type = looks_like_rank;
      MPI_Reduce((void *)&root_type, (void *)&looks_like_rank, 1, MPI_INT, MPI_MIN, 0, MPI_COMM_WORLD);
      switch (looks_like_rank) {
        case NOT_RANK:
          /* Variable does not indicate rank. */
          break;

        case GOOD_ENOUGH:
          /* Variable implicitly indicates rank 0. */
          printf("    %-40.40s (defined only on rank 0)\n", keybuffer);
          found_any = 1;
          break;

        case IS_RANK:
          /* Variable exactly indicates a process's rank. */
          printf("    %-40.40s (correct rank at all processes)\n", keybuffer);
          found_any = 1;
          break;

        default:
          abort();
          break;
      }
      free(key);
    }
    if (!found_any)
      printf("    [none]\n");

    /* Tell the slaves to exit. */
    keybuffer[0] = '\0';
    MPI_Bcast(keybuffer, MAX_ENV_VAR_LEN+1, MPI_CHAR, 0, MPI_COMM_WORLD);
  }
  else
    /* Slaves check each environment variable they're given. */
    while (1) {
      char *value;          /* Value of environment variable "key" */

      MPI_Bcast(keybuffer, MAX_ENV_VAR_LEN+1, MPI_CHAR, 0, MPI_COMM_WORLD);
      if (!keybuffer[0])
        break;
      looks_like_rank = NOT_RANK;        /* Assume the worst. */
      if ((value=getenv(keybuffer))) {
        if (atoi(value) == rank)
          looks_like_rank = IS_RANK;     /* The rank matches! */
        else
          looks_like_rank = NOT_RANK;    /* The rank is incorrect. */
      }
      else
        looks_like_rank = GOOD_ENOUGH;   /* Defined only on rank 0 is good enough. */
      MPI_Reduce((void *)&looks_like_rank, (void *)keybuffer, 1, MPI_INT, MPI_MIN, 0, MPI_COMM_WORLD);
    }

  /* Finish up cleanly. */
  MPI_Finalize();
  return 0;
}
