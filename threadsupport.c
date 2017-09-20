/*------------------------------------------------------------
 * JumboMem memory server: Support for multithreaded
 * applications
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
#include <pthread.h>
#ifdef HAVE_SCHED
# define _GNU_SOURCE
# include <sched.h>
#endif
#include <sys/time.h>
#include <time.h>

/* Specify the number of blocks of thread information to allocate
 * statically. */
#define MAXSTATICBLOCKS 1

/* Define the time in milliseconds after which we give up waiting for
 * a thread to freeze. */
#ifndef JM_FREEZE_TIMEOUT
# define JM_FREEZE_TIMEOUT 1000
#endif

/* Define some per-thread information as an element in a linked list. */
typedef struct thread_info_t {
  pthread_t tid;                     /* Thread identifier (from Pthreads; not necessarily unique) */
  pid_t unique_tid;                  /* Unique thread ID (from gettid(); may be -1 */
  volatile unsigned int blocked;     /* 0=running; >0=blocked on the mega-lock */
  volatile unsigned int internal_depth;    /* 0=user mode; >0=depth of JumboMem mode */
  int cancel_handler;                /* >0=return if in the signal handler; 0=do nothing */
  int freeable;                      /* 1=struct can be free()'d; 0=cannot */
  int internal;                      /* 1=thread is internal to JumboMem; 0=user thread */
  struct thread_info_t *next;        /* Pointer to the next thread's info */
} THREAD_INFO;

/* Define a lock to serialize JumboMem thread operations. */
static pthread_mutex_t megalock = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP;  /* The lock itself */

/* Manage per-thread information. */
static THREAD_INFO *per_thread_info = NULL;   /* Linked list of information */
static pthread_key_t private_ptr_key = (pthread_key_t)(~0);   /* Key to access our thread's element of per_thread_info */

/* ---------------------------------------------------------------------- */

/* Mark a terminating thread as forever blocked. */
static void
thread_destructor (void *private)
{
  ((THREAD_INFO *)private)->blocked = ~(volatile unsigned int)0;
}


/* Initialize the pointer to thread-specific data. */
static void
create_private_key (void)
{
  if (pthread_key_create(&private_ptr_key, thread_destructor))
    jm_abort("pthread_key_create() failed");
}


/* Initialize the calling thread, given a pointer to (uninitialized)
 * thread information. */
static void
initialize_thread (void)
{
  THREAD_INFO *newinfo;        /* Information about our thread */
  static pthread_once_t key_create_control = PTHREAD_ONCE_INIT;   /* Control one-shot initialization. */
  int malloc_okay;             /* 1=safe to call malloc(); 0=unsafe */
  static pthread_mutex_t initlock = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP;  /* Allow only one thread to initialize at a time. */
#ifdef HAVE_SCHED
  cpu_set_t validcpus;         /* Set of CPUs on which we can run */
  int i;

  /* Dynamically determine the set of valid CPUs and bind the calling
   * thread to all of those. */
  CPU_ZERO(&validcpus);
  for (i=0; i<CPU_SETSIZE; i++) {
    CPU_SET(i, &validcpus);
    if (sched_setaffinity(0, sizeof(cpu_set_t), &validcpus) == -1)
      CPU_CLR(i, &validcpus);
  }
#endif

  /* Serialize allocations of thread-specific data. */
  if (pthread_mutex_lock(&initlock))
    jm_abort("Failed to acquire the thread-initialization lock");

  /* Allocate and initialize our thread-specific data. */
  malloc_okay = jm_memory_is_initialized();
  if (malloc_okay)
    newinfo = (THREAD_INFO *) jm_internal_malloc_no_lock(sizeof(THREAD_INFO));
  else {
    static THREAD_INFO static_thread_info[MAXSTATICBLOCKS];  /* Statically allocated information blocks */
    static int numstaticblocks = 0;    /* Number of statically allocated blocks in use */

    newinfo = &static_thread_info[numstaticblocks++];
    if (numstaticblocks > MAXSTATICBLOCKS)
      jm_abort("Failed to allocate %d static blocks of thread information");
  }
  memset(newinfo, 0, sizeof(THREAD_INFO));
  newinfo->freeable = malloc_okay;
  newinfo->tid = pthread_self();
  newinfo->unique_tid = gettid();
  pthread_once(&key_create_control, create_private_key);
  if (pthread_setspecific(private_ptr_key, newinfo))
    jm_abort("pthread_setspecific() failed");

  /* Insert our thread's information at the head of the
   * thread-information list.  We take the mega-lock while we do this
   * to avoid a race condition in which another thread's
   * jm_freeze_other_threads() misses seeing our thread and then our
   * thread touches data as it's being paged in, thereby resulting in
   * corrupted user data. */
  jm_enter_critical_section();
  newinfo->internal = jm_globals.is_internal;   /* Safe to read jm_globals.is_internal only when we have the lock. */
  newinfo->next = per_thread_info;
  per_thread_info = newinfo;
  jm_exit_critical_section();

  /* Allow other threads to initialize. */
  if (pthread_mutex_unlock(&initlock))
    jm_abort("Failed to release the thread-initialization lock");
}


/* Return a pointer to our thread-specific data. */
static THREAD_INFO *
get_thread_specific_data (void)
{
  THREAD_INFO *private;          /* Thread-private information */

  /* Common case -- we can read the data successfully. */
  if (private_ptr_key != (pthread_key_t)(~0)
      && (private=pthread_getspecific(private_ptr_key)))
    return private;

  /* We failed to find our thread-specific data.  Is this because we
   * don't yet have any thread-specific data or because of another
   * reason?  Who knows?  We assume the former, initialize our thread,
   * and retry the operation. */
  initialize_thread();
  if (!(private=pthread_getspecific(private_ptr_key)))
    jm_abort("pthread_getspecific() failed");
  return private;
}


/* Return the current time in milliseconds. */
uint64_t
current_time_ms (void)
{
  struct timeval now;             /* Current time */

  if (gettimeofday(&now, NULL) == -1)
    jm_abort("gettimeofday() failed");
  return now.tv_sec*1000ULL + now.tv_usec/1000ULL;
}

/* ---------------------------------------------------------------------- */

/* Serialize accesses to a critical section of code. */
void
jm_enter_critical_section (void)
{
  THREAD_INFO *private;          /* Thread-private information */

  /* Acquire the thread mega-lock. */
  private = get_thread_specific_data();
  private->blocked = 1;          /* Assume atomic. */
  if (private->internal_depth == 0         /* Avoid recursive locks. */
      && pthread_mutex_lock(&megalock))
    jm_abort("Failed to acquire the thread mega-lock.");
  private->blocked = 0;
  private->internal_depth++;
}


/* Allow other threads to enter a critical section. */
void
jm_exit_critical_section (void)
{
  THREAD_INFO *private;          /* Thread-private information */

  private = get_thread_specific_data();
  private->internal_depth--;
  if (private->internal_depth == 0
      && pthread_mutex_unlock(&megalock))
    jm_abort("Failed to release the thread mega-lock.");
}


/* Return the current call depth of the mega-lock. */
unsigned int
jm_get_internal_depth (void)
{
  THREAD_INFO *private;          /* Thread-private information */

  private = get_thread_specific_data();
  return private->internal_depth;
}


/* Set the current call depth of the mega-lock (used by jm_abort()). */
void
jm_set_internal_depth (unsigned int newdepth)
{
  THREAD_INFO *private;          /* Thread-private information */

  private = get_thread_specific_data();
  private->internal_depth = newdepth;
}


/* Return 1 if we should exit the signal handler immediately, 0 if we
 * can keep going. */
int
jm_must_exit_signal_handler_now (void)
{
  THREAD_INFO *private;          /* Thread-private information */

  private = get_thread_specific_data();
  if (private->cancel_handler > 0) {
    private->cancel_handler--;
    return 1;
  }
  return 0;
}


/* Instruct all other threads (except JumboMem-internal threads) to
 * freeze execution then wait until they're all frozen before
 * returning. */
void
jm_freeze_other_threads (void)
{
  THREAD_INFO *threadptr;        /* Pointer to thread-specific information */
  THREAD_INFO **prev_threadptr;  /* Pointer to threadptr */
  uint64_t starting_time_ms;     /* Time in milliseconds we began waiting for threads to freeze */

  /* Tell all unblocked threads to enter the signal handler and block.
   * Remove terminated threads from the thread list as we go. */
  JM_RECORD_CYCLE("Freezing other threads");
  prev_threadptr = &per_thread_info;
  threadptr = *prev_threadptr;
  while (threadptr) {
    /* Skip our thread, all threads that are already blocked, and any
     * internal threads. */
    if (!pthread_equal(pthread_self(), threadptr->tid)
        && !threadptr->blocked
        && !threadptr->internal) {

      /* Order the thread to enter its signal handler, where it will
       * immediately block on the mega-lock. */
      jm_debug_printf(5, "Signaling thread %lu (LWP %d) to freeze\n", threadptr->tid, threadptr->unique_tid);
      if (pthread_kill(threadptr->tid, SIGSEGV) == ESRCH) {
        /* We failed to signal a thread.  It must be dead.  Hence, we
         * remove it from future consideration. */
        *prev_threadptr = threadptr->next;
        if (threadptr->freeable)
          jm_free(threadptr);
        threadptr = *prev_threadptr;
        continue;
      }
    }

    /* Proceed with the next thread. */
    prev_threadptr = &threadptr->next;
    threadptr = *prev_threadptr;
  }

  /* Wait until all other threads are blocked. */
  starting_time_ms = current_time_ms();
  for (threadptr=per_thread_info; threadptr; threadptr=threadptr->next) {
    if (!pthread_equal(pthread_self(), threadptr->tid) && !threadptr->internal)
      while (1) {
        char state;    /* Current thread state */

        /* It's safe to continue if the thread is blocked waiting for
         * the mega-lock. */
        if (threadptr->blocked)
          break;

        /* It's safe to continue if the thread is blocked in the
         * kernel.  When the thread wakes up it should immediately
         * enter its signal handler and block. */
        state = jm_get_thread_state(threadptr->unique_tid);
        if (state == 'D' || state == 'Z' || state == 'T')
          break;

        /* If the thread hasn't acknowledged our signal after a very
         * long time, take a chance and assume that it won't touch the
         * page that we're currently faulting in.  This is a risky
         * assumption but does guarantee that JumboMem won't hang when
         * a thread manages to block SIGSEGV without JumboMem's
         * knowledge (e.g., by using inline assembly language to
         * perform a syscall instruction). */
        if (current_time_ms() - starting_time_ms > JM_FREEZE_TIMEOUT) {
          jm_debug_printf(4, "Thread %lu (LWP %d) failed to freeze after %lu ms\n",
                          threadptr->tid, threadptr->unique_tid, JM_FREEZE_TIMEOUT);
          break;
        }

        /* Relinquish the CPU in hopes that the thread gets scheduled,
         * receives our signal, and blocks on the mega-lock. */
#if defined(HAVE_SCHED) && defined(_POSIX_PRIORITY_SCHEDULING)
        sched_yield();
#else
        sleep(0);
#endif
      }
  }

  /* Instruct every blocked thread to exit the signal handler once it
   * acquires the lock.  (If a thread got blocked trying to satisfy a
   * page fault, it will automatically re-enter the signal handler
   * when it retries the faulting operation.) */
  for (threadptr=per_thread_info; threadptr; threadptr=threadptr->next) {
    /* Skip our thread and any internal threads. */
    if (!pthread_equal(pthread_self(), threadptr->tid)
        && !threadptr->internal)
      threadptr->cancel_handler++;
  }
  JM_RECORD_CYCLE("Finished freezing other threads");
}


/* Initialize the current, newly created thread then invoke the user's
 * initializer.  The caller is responsible for allocating memory for
 * arg but we will free it before we return. */
void *
jm_thread_start_routine (void *arg)
{
  PTHREAD_CREATE_ARGS *jm_init_info = (PTHREAD_CREATE_ARGS *) arg;
  sigset_t segv;               /* Signal set containing only SIGSEGV */
  void *retval;                /* User function's return value */

  /* There are two functions internal to glibc
   * (__aio_create_helper_thread() and __start_helper_thread()) that
   * block all signals, spawn a thread, then restore the previous
   * signal mask.  Signal-blocking in these two functions is
   * implemented using inline assembly language so we can't easily
   * intercept that.  However, we can unblock SIGSEGV here at the
   * start of the child thread. */
  jm_enter_critical_section();
  if (sigemptyset(&segv) == -1)
    jm_abort("sigemptyset() failed");
  if (sigaddset(&segv, SIGSEGV) == -1)
    jm_abort("sigaddset() failed to add SIGSEGV");
  if (pthread_sigmask(SIG_UNBLOCK, &segv, NULL))
    jm_abort("pthread_sigmask() failed to unblock SIGSEGV");
  jm_exit_critical_section();

  /* Pass control to the user-provided function and return its return value. */
  retval = jm_init_info->start_routine(jm_init_info->arg);
  jm_free(arg);
  pthread_exit(retval);
  return NULL;    /* We should never get here. */
}
