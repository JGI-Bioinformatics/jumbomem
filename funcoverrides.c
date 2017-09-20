/*------------------------------------------------------------
 * JumboMem memory server: Functions that override existing
 * (e.g., libc) functions
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
#include <limits.h>

/* If not already defined, define the maximum number of consecutive
 * successful/unsuccessful file accesses we need to observe before
 * changing the chunk size. */
#ifndef JM_MAX_CONSECUTIVE
# define JM_MAX_CONSECUTIVE 3
#endif

/* Define the maximum standard line length if not already defined. */
#ifndef LINE_MAX
# define LINE_MAX 2048
#endif


#ifdef RTLD_NEXT

/* Define pointers to all of the functions we intend to override. */
static int (*original_pthread_create)(void *, void *, void *, void *) = NULL;
static int (*original_pthread_attr_getstack)(const pthread_attr_t *,
                                             void **, size_t *) = NULL;
static int (*original_pthread_attr_setstack)(pthread_attr_t *, void *, size_t) = NULL;
static int (*original_pthread_attr_init)(pthread_attr_t *) = NULL;
static int (*original_pthread_attr_destroy)(pthread_attr_t *) = NULL;
static sighandler_t (*original_signal)(int, sighandler_t) = NULL;
static int (*original_sigaction)(int, const struct sigaction *, struct sigaction *) = NULL;
static int (*original_sigprocmask)(int, const sigset_t *, sigset_t *) = NULL;
static int (*original_pthread_sigmask)(int, const sigset_t *, sigset_t *) = NULL;
static int (*original_sigtimedwait)(const sigset_t *, siginfo_t *, const struct timespec *) = NULL;
static int (*original_sigwaitinfo)(const sigset_t *, siginfo_t *) = NULL;
static void *(*original_mmap)(void *start, size_t length, int prot, int flags, int fd, off_t offset) = NULL;
static pid_t (*original_ioctl)(int fd, int request, void *ptr) = NULL;
static int (*original_open)(const char *pathname, int flags, ...) = NULL;
static ssize_t (*original_read)(int fd, void *buf, size_t count) = NULL;
static size_t (*original_fread)(void *ptr, size_t size, size_t nmemb, FILE *stream) = NULL;
static size_t (*original_fread_unlocked)(void *ptr, size_t size, size_t nmemb, FILE *stream) = NULL;
static ssize_t (*original_write)(int fd, const void *buf, size_t count) = NULL;
static size_t (*original_fwrite)(const void *ptr, size_t size, size_t nmemb, FILE *stream) = NULL;
static size_t (*original_fwrite_unlocked)(const void *ptr, size_t size, size_t nmemb, FILE *stream) = NULL;

/* Export the original sigaction() and the original mmap(). */
int (*jm_original_sigaction)(int, const struct sigaction *, struct sigaction *) = NULL;
void *(*jm_original_mmap)(void *start, size_t length, int prot, int flags, int fd, off_t offset);

/* Import a few structures that are defined elsewhere. */
extern struct sigaction jm_prev_segfaulter;  /* Previous SIGSEGV handler information */
extern struct sigaction jm_prev_prev_segfaulter;  /* Two SIGSEGV handler informations ago */

/* Define a default stack size to use for new Pthreads. */
static size_t default_pthread_stack_size = 2UL * 1024UL * 1024UL;

/* Describe a read or write operation. */
typedef struct read_write_info_t {
  void *buffer;        /* Buffer to read into or write from */
  size_t count;        /* Number of bytes to read/read */
  ssize_t (*function)(struct read_write_info_t *info);   /* Original read/write function */
  union {
    int fd;            /* File descriptor to read/write */
    FILE *stream;      /* Stream to read/write */
  } file;
  int is_read;         /* 0=write; 1=read */
} READ_WRITE_INFO;

/* ---------------------------------------------------------------------- */

/* Split large reads into chunks and prefault the pages in each chunk. */
static ssize_t
read_or_write (READ_WRITE_INFO *info)
{
  size_t bytesdone = 0;            /* Number of bytes read/written so far */
  size_t unsuccessful_bytes = 0;   /* Smallest buffer size that was read/written unsuccessfully */
  size_t successful_bytes = 0;     /* Largest buffer size that was read/written successfully */
  size_t max_successful_bytes = 0;     /* Largest value of successful_bytes we've seen */
  unsigned int consec_successes = 0;   /* Number of consecutive successes */
  unsigned int consec_failures = 0;    /* Number of consecutive failures */
  const char *baseaddr = (const char *) info->buffer;
  size_t totalcount = info->count;

  /* Invoke the original read or write call if we don't yet know the
   * page size or the number of pages we can cache locally.  Also,
   * invoke the original read or write call if all or part of the
   * buffer is not managed by JumboMem. */
  if (!jm_globals.pagesize
      || baseaddr < jm_globals.memregion
      || baseaddr+totalcount >= jm_globals.memregion+jm_globals.extent)
    return (*info->function)(info);
  if (!successful_bytes) {
    successful_bytes = jm_globals.ospagesize;
    max_successful_bytes = jm_globals.ospagesize;
    unsuccessful_bytes = 2*jm_globals.local_pages*jm_globals.pagesize - successful_bytes;
  }

  /* Repeatedly read/write one chunk at a time. */
#ifdef JM_DEBUG
  jm_debug_printf(5, "%s %lu bytes of data one chunk at a time.\n",
                  info->is_read ? "Reading" : "Writing", totalcount);
#endif
  while (bytesdone < totalcount) {
    size_t bytesremaining = totalcount - bytesdone;   /* Number of bytes left to read/write */
    ssize_t newbytes;              /* Number of bytes just read/written */

    /* Determine the size of the largest chunk we expect to be able to
     * read/write in a single function call. */
    if (consec_successes == JM_MAX_CONSECUTIVE) {
      successful_bytes = info->count;
      consec_successes = 0;
    }
    else if (consec_failures == JM_MAX_CONSECUTIVE) {
      unsuccessful_bytes = info->count;
      consec_failures = 0;
      if (unsuccessful_bytes <= jm_globals.ospagesize)
        /* Give up if we can't read even a single page. */
        break;
      if (unsuccessful_bytes == successful_bytes) {
        /* What used to be successful is now unsuccessful.  Reset the
         * binary search and hope for the best. */
        successful_bytes = jm_globals.ospagesize;
        unsuccessful_bytes = 2*max_successful_bytes - successful_bytes;
      }
    }
    info->count = (successful_bytes+unsuccessful_bytes) / 2;
    if (info->count > bytesremaining)
      info->count = bytesremaining;
    jm_debug_printf(5, "Trying count of (%lu+%lu)/2 = %lu bytes.\n",
                    successful_bytes, unsuccessful_bytes, info->count);

    /* Touch every page to force it into local memory. */
    info->buffer = (void *) ((const char *)baseaddr + bytesdone);
    jm_touch_memory_region(info->buffer, info->count);

    /* Read/write as much as we can and keep track of our success/failure. */
    newbytes = (*info->function)(info);
    if (newbytes < 1) {
      /* The read or write failed. */
      consec_failures++;
      consec_successes = 0;
      jm_debug_printf(5, "Failure #%u at %p (%s).\n",
                      consec_failures, info->buffer, jm_strerror(errno));
      continue;
    }
    else {
      /* The read or write succeeded. */
      consec_successes++;
      consec_failures = 0;
      if (max_successful_bytes < info->count)
        max_successful_bytes = info->count;
    }
#ifdef JM_DEBUG
    jm_debug_printf(5, "%s %lu of %lu bytes = %.1f%% (block size = %lu bytes).\n",
                    info->is_read ? "Read" : "Wrote", bytesdone+newbytes,
                    totalcount, (bytesdone+newbytes)*100.0/totalcount, info->count);
#endif

    /* Keep track of what we've read/written so far. */
    bytesdone += newbytes;
#ifdef JM_DEBUG
    jm_debug_printf(5, "%s %lu of %lu bytes.\n",
                    info->is_read ? "Read" : "Wrote", bytesdone, totalcount);
#endif
  }

  /* Return the total number of bytes we read/wrote. */
#ifdef JM_DEBUG
  jm_debug_printf(5, "%s is exiting with %lu of %lu read (%s).\n",
                  info->is_read ? "Read" : "Write", bytesdone, totalcount, jm_strerror(errno));
#endif
  return (ssize_t)bytesdone;
}


/* Invoke original_read(). */
static ssize_t
do_read (READ_WRITE_INFO *info)
{
  errno = 0;
  return (*original_read)(info->file.fd, (void *)info->buffer, info->count);
}


/* Invoke original_fread(). */
static ssize_t
do_fread (READ_WRITE_INFO *info)
{
  clearerr(info->file.stream);
  return (ssize_t) (*original_fread)((void *)info->buffer, 1, info->count, info->file.stream);
}


/* Invoke original_fread_unlocked(). */
static ssize_t
do_fread_unlocked (READ_WRITE_INFO *info)
{
  clearerr(info->file.stream);
  return (ssize_t) (*original_fread_unlocked)((void *)info->buffer, 1, info->count, info->file.stream);
}


/* Invoke original_write(). */
static ssize_t
do_write (READ_WRITE_INFO *info)
{
  errno = 0;
  return (*original_write)(info->file.fd, info->buffer, info->count);
}


/* Invoke original_fwrite(). */
static ssize_t
do_fwrite (READ_WRITE_INFO *info)
{
  clearerr(info->file.stream);
  return (ssize_t) (*original_fwrite)((const void *)info->buffer, 1, info->count, info->file.stream);
}


/* Invoke original_fwrite_unlocked(). */
static ssize_t
do_fwrite_unlocked (READ_WRITE_INFO *info)
{
  clearerr(info->file.stream);
  return (ssize_t) (*original_fwrite_unlocked)((const void *)info->buffer, 1, info->count, info->file.stream);
}

/* ---------------------------------------------------------------------- */

/* Keep track of all threads we create.  Also, invoke a JumboMem
 * wrapper function before invoking the caller-provided function. */
int
pthread_create (void *thread, void *attr, void *start_routine, void *arg)
{
  int retval;                         /* Function return value */
  PTHREAD_CREATE_ARGS *caller_args;   /* Arguments provided by the caller */
  void *stackaddr;                    /* Pthread stack address (not used) */
  size_t stacksize;                   /* Pthread stack size */
  int freeattr = 0;                   /* 0=no freeing; 1=free attr when done; 2=destroy and free */

  /* Package up the arguments to pthread_create() so we can pass them
   * to our own start routine, jm_thread_start_routine(). */
  caller_args = (PTHREAD_CREATE_ARGS *) jm_malloc(sizeof(PTHREAD_CREATE_ARGS));
  caller_args->start_routine = start_routine;
  caller_args->arg = arg;
  caller_args->threadstack = NULL;    /* May be overwritten below. */

  /* Ensure that a stack is specified.  Otherwise, the user's
   * pthread_join() may add externally allocated memory to a free
   * list.  A JumboMem-internal pthread_create() may then pick up
   * externally allocated memory and try to free it, leading to
   * hard-to-track memory corruption. */
  if (!attr) {
    attr = jm_malloc(sizeof(pthread_attr_t));
    (*original_pthread_attr_init)(attr);
    freeattr = 2;
  }
  if ((*original_pthread_attr_getstack)(attr, &stackaddr, &stacksize) != 0)
    jm_abort("Failed to retrieve the Pthread stack size from a given attribute");
  if (stacksize == 0) {
    /* FIXME: The thread-stack memory is never freed.  The challenge
     * is that the memory has to remain valid until
     * jm_thread_start_routine() exits, which means there's not really
     * a good place to inject a jm_free().  (Additional bookkeeping is
     * needed but is probably not worth the effort.) */
    pthread_attr_t *stacked_attr;    /* Same as attr but with a stack specified */
    stacked_attr = jm_malloc(sizeof(pthread_attr_t));
    *stacked_attr = *(pthread_attr_t *)attr;
    attr = stacked_attr;
    freeattr = 1;
    jm_enter_critical_section();  /* Stack needs to be allocated internally.  Why? */
    caller_args->threadstack = jm_valloc(default_pthread_stack_size);
    jm_exit_critical_section();
    if ((*original_pthread_attr_setstack)(attr,
                                          caller_args->threadstack,
                                          default_pthread_stack_size) != 0)
      jm_abort("Failed to set the Pthread stack size");
  }

  /* Invoke jm_thread_start_routine() as a wrapper for the
   * user-provided start_routine(). */
  retval = (*original_pthread_create)(thread, attr, jm_thread_start_routine, (void *)caller_args);

  /* Free any memory we allocated (except stack memory). */
  if (freeattr >= 1) {
    if (freeattr == 2
        && (*original_pthread_attr_destroy)(attr) != 0)
      jm_abort("Failed to destroy a Pthread attribute");
    jm_free(attr);
  }
  return retval;
}


/* Ensure that user-invoked mmap() calls don't allocate in the middle
 * of the JumboMem memory region. */
void *
mmap (void *start, size_t length, int prot, int flags, int fd, off_t offset)
{
  return jm_mmap(start, length, prot, flags, fd, offset);
}


/* Prevent libc's signal() function from replacing JumboMem's SIGSEGV
 * handler. */
sighandler_t
signal (int signum, sighandler_t handler)
{
  /* Pass through every signal but SIGSEGV.  Pass through SIGSEGV only
   * during JumboMem initialization. */
  JM_ENTER();
  if (signum != SIGSEGV || JM_INTERNAL_INVOCATION())
    JM_RETURN((*original_signal)(signum, handler));

  /* Pretend to execute signal(). */
  jm_prev_prev_segfaulter.sa_handler = jm_prev_segfaulter.sa_handler;
  jm_prev_segfaulter.sa_handler = handler;
  JM_RETURN(jm_prev_prev_segfaulter.sa_handler);
}


/* Prevent libc's sigaction() function from replacing JumboMem's
 * SIGSEGV handler. */
int
sigaction (int signum, const struct sigaction *act, struct sigaction *oldact)
{
  JM_ENTER();

  /* If this is an internal invocation, let it pass through unmodified. */
  if (JM_INTERNAL_INVOCATION())
    JM_RETURN((*original_sigaction)(signum, act, oldact));

  /* If this call does not attempt to modify SIGSEGV, let it pass
   * through but only after stripping out SIGSEGV from the set of
   * masked signals. */
  if (signum != SIGSEGV) {
    if (act) {
      struct sigaction newact = *act;   /* Modifiable copy of act */

      (void) sigdelset(&newact.sa_mask, SIGSEGV);
      JM_RETURN((*original_sigaction)(signum, &newact, oldact));
    }
    else
      /* No signals are being masked. */
      JM_RETURN((*original_sigaction)(signum, act, oldact));
  }

  /* If this call does attempt to modify SIGSEGV, then only pretend to
   * execute sigaction(SIGSEGV, ...). */
  if (oldact)
    *oldact = jm_prev_segfaulter;
  if (act) {
    jm_prev_prev_segfaulter = jm_prev_segfaulter;
    jm_prev_segfaulter = *act;
  }
  JM_RETURN(0);
}


/* Prevent sigprocmask() from blocking SIGSEGV unless called from
 * within JumboMem. */
int
sigprocmask (int how, const sigset_t *set, sigset_t *oldset)
{
  JM_ENTER();
  if (!JM_INTERNAL_INVOCATION() && set) {
    sigset_t newset = *set;

    (void) sigdelset(&newset, SIGSEGV);
    JM_RETURN((*original_sigprocmask)(how, &newset, oldset));
  }
  else
    JM_RETURN((*original_sigprocmask)(how, set, oldset));
}


/* Prevent pthread_sigmask() from blocking SIGSEGV unless called from
 * within JumboMem. */
int
pthread_sigmask (int how, const sigset_t *set, sigset_t *oldset)
{
  JM_ENTER();
  if (!JM_INTERNAL_INVOCATION() && set) {
    sigset_t newset = *set;

    (void) sigdelset(&newset, SIGSEGV);
    JM_RETURN((*original_pthread_sigmask)(how, &newset, oldset));
  }
  else
    JM_RETURN((*original_pthread_sigmask)(how, set, oldset));
}


/* Don't let sigtimedwait() steal JumboMem's SIGSEGVs. */
int
sigtimedwait (const sigset_t *set, siginfo_t *info,
              const struct timespec *timeout)
{
  JM_ENTER();
  if (!JM_INTERNAL_INVOCATION() && set) {
    sigset_t newset = *set;

    (void) sigdelset(&newset, SIGSEGV);
    JM_RETURN((*original_sigtimedwait)(&newset, info, timeout));
  }
  else
    JM_RETURN((*original_sigtimedwait)(set, info, timeout));
}


/* Don't let sigwaitinfo() steal JumboMem's SIGSEGVs. */
int
sigwaitinfo (const sigset_t *set, siginfo_t *info)
{
  JM_ENTER();
  if (!JM_INTERNAL_INVOCATION() && set) {
    sigset_t newset = *set;

    (void) sigdelset(&newset, SIGSEGV);
    JM_RETURN((*original_sigwaitinfo)(&newset, info));
  }
  else
    JM_RETURN((*original_sigwaitinfo)(set, info));
}


/* Touch the page pointed to by ioctl()'s pointer argument in hopes of
 * avoiding an EFAULT return code (or worse). */
int
ioctl (int fd, int request, void *ptr)
{
  JM_ENTER();
  jm_touch_memory_region(ptr, jm_globals.pagesize);
  JM_RETURN((*original_ioctl)(fd, request, ptr));
}


/* Open a file with the special case that /proc/meminfo is faked. */
int
open (const char *pathname, int flags, ...)
{
  FILE *real_meminfo;       /* Handle to the real /proc/meminfo file */
  FILE *fake_meminfo;       /* Handle to a fake /proc/meminfo file */
  char oneline[LINE_MAX+1]; /* One line read from real_meminfo */
  uint64_t memtotal = 0;    /* Total RAM */
  uint64_t memfree = 0;     /* Available RAM */

  /* Common case -- fall back to the original open() call. */
  JM_ENTER();
  if (JM_INTERNAL_INVOCATION() || strcmp(pathname, "/proc/meminfo")) {
    if (flags & O_CREAT) {
      va_list args;           /* Optional function arguments */
      mode_t mode;            /* File open mode */

      va_start(args, flags);
      mode = va_arg(args, mode_t);
      va_end(args);
      JM_RETURN((*original_open)(pathname, flags, mode));
    }
    else
      JM_RETURN((*original_open)(pathname, flags));
  }

  /* Fabricate a /proc/meminfo file. */
  if (!(fake_meminfo=tmpfile()))
    JM_RETURN(-1);
  if (!(real_meminfo=fopen("/proc/meminfo", "r")))
    JM_RETURN(-1);
  while (fgets(oneline, LINE_MAX+1, real_meminfo)) {
    if (sscanf(oneline, "MemTotal: %" PRIu64 " kB", &memtotal) == 1)
      fprintf(fake_meminfo, "MemTotal:     %8" PRIu64" kB\n",
              jm_globals.extent/1024);
    else
      if (sscanf(oneline, "MemFree: %" PRIu64 " kB", &memfree) == 1)
        fprintf(fake_meminfo, "MemFree:      %8" PRIu64 " kB\n",
                (jm_globals.extent - (memtotal-memfree))/1024);
      else
        fprintf(fake_meminfo, "%s", oneline);
  }
  if (ferror(real_meminfo))
    JM_RETURN(-1);
  fclose(real_meminfo);
  if (fseek(fake_meminfo, 0L, SEEK_SET) == -1)
    JM_RETURN(-1);
  JM_RETURN(fileno(fake_meminfo));
}


/* Split large reads into chunks and prefault the pages in each chunk. */
ssize_t
read (int fd, void *buffer, size_t count)
{
  READ_WRITE_INFO read_info;    /* Description of a read() operation */

  read_info.file.fd = fd;
  read_info.buffer = buffer;
  read_info.count = count;
  read_info.function = do_read;
  read_info.is_read = 1;
  return read_or_write(&read_info);
}


/* Split large stdio reads into chunks and prefault the pages in each chunk. */
size_t
fread (void *buffer, size_t size, size_t nmemb, FILE *stream)
{
  READ_WRITE_INFO fread_info;    /* Description of an fread() operation */

  fread_info.file.stream = stream;
  fread_info.buffer = buffer;
  fread_info.count = size * nmemb;
  fread_info.function = do_fread;
  fread_info.is_read = 1;
  return read_or_write(&fread_info) / size;
}


/* Split large, unlocked stdio reads into chunks and prefault the
 * pages in each chunk. */
#undef fread_unlocked
size_t
fread_unlocked (void *buffer, size_t size, size_t nmemb, FILE *stream)
{
  READ_WRITE_INFO fread_unlocked_info;    /* Description of an fread_unlocked() operation */

  fread_unlocked_info.file.stream = stream;
  fread_unlocked_info.buffer = buffer;
  fread_unlocked_info.count = size * nmemb;
  fread_unlocked_info.function = do_fread_unlocked;
  fread_unlocked_info.is_read = 1;
  return read_or_write(&fread_unlocked_info) / size;
}


/* Split large writes into chunks and prefault the pages in each chunk. */
#undef fwrite_unlocked
ssize_t
write (int fd, const void *buffer, size_t count)
{
  READ_WRITE_INFO write_info;    /* Description of a write() operation */

  write_info.file.fd = fd;
  write_info.buffer = (void *) buffer;
  write_info.count = count;
  write_info.function = do_write;
  write_info.is_read = 0;
  return read_or_write(&write_info);
}


/* Split large stdio writes into chunks and prefault the pages in each
 * chunk. */
size_t
fwrite (const void *buffer, size_t size, size_t nmemb, FILE *stream)
{
  READ_WRITE_INFO fwrite_info;    /* Description of an fwrite() operation */

  fwrite_info.file.stream = stream;
  fwrite_info.buffer = (void *) buffer;
  fwrite_info.count = size * nmemb;
  fwrite_info.function = do_fwrite;
  fwrite_info.is_read = 0;
  return read_or_write(&fwrite_info) / size;
}


/* Split large, unlocked stdio writes into chunks and prefault the
 * pages in each chunk. */
size_t
fwrite_unlocked (const void *buffer, size_t size, size_t nmemb, FILE *stream)
{
  READ_WRITE_INFO fwrite_unlocked_info;    /* Description of an fwrite_unlocked() operation */

  fwrite_unlocked_info.file.stream = stream;
  fwrite_unlocked_info.buffer = (void *) buffer;
  fwrite_unlocked_info.count = size * nmemb;
  fwrite_unlocked_info.function = do_fwrite_unlocked;
  fwrite_unlocked_info.is_read = 0;
  return read_or_write(&fwrite_unlocked_info) / size;
}


/* Implement calloc() in terms of malloc().  This shouldn't be
 * necessary but without it JumboMem was crashing when launching GNU
 * Octave. */
#ifdef JM_MALLOC_HOOKS
void *
calloc (size_t nmemb, size_t size)
{
  void *buffer;                  /* Allocated and cleared buffer to return */

  buffer = malloc(nmemb*size);
  if (buffer)
    memset(buffer, 0, nmemb*size);
  return buffer;
}
#endif


/* Matches #if RTLD_NEXT */
#endif

/* ---------------------------------------------------------------------- */

#ifdef RTLD_NEXT
/* Abort when a function is invoked but the override function failed
 * to find the original function.  If only C supported closures we'd
 * be able to output the name of the failed function as well. */
static void
function_not_found (void)
{
  jm_abort("An overridable function was called but the overriding function couldn't be found.");
}


/* Look up a function and return a pointer to an abort function if the
 * given function wasn't found.  We don't abort immediately because
 * the function in question might never be called. */
static void *
lookup_function (const char *funcname)
{
  void *function = dlsym(RTLD_NEXT, funcname);
  if (function)
    return function;
  else
    return (void *) function_not_found;
}
#endif


/* Initialize all of our function overrides. */
void
jm_initialize_overrides (void)
{
  struct rlimit stacklimits;     /* Hard and soft limits on stack size */

  /* Store a pointer to each function we intend to replace plus some
   * Pthread functions that may or may not be linked in. */
#ifdef RTLD_NEXT
  original_pthread_create = lookup_function("pthread_create");
  original_pthread_attr_init = lookup_function("pthread_attr_init");
  original_pthread_attr_destroy = lookup_function("pthread_attr_destroy");
  original_pthread_attr_getstack = lookup_function("pthread_attr_getstack");
  original_pthread_attr_setstack = lookup_function("pthread_attr_setstack");
  original_signal = lookup_function("signal");
  original_sigaction = lookup_function("sigaction");
  original_sigprocmask = lookup_function("sigprocmask");
  original_pthread_sigmask = lookup_function("pthread_sigmask");
  original_sigtimedwait = lookup_function("sigtimedwait");
  original_sigwaitinfo = lookup_function("sigwaitinfo");
  original_ioctl = lookup_function("ioctl");
  jm_original_sigaction = original_sigaction;
  original_mmap = lookup_function("mmap");
  jm_original_mmap = original_mmap;
  original_open = lookup_function("open");
  original_read = lookup_function("read");
  original_fread = lookup_function("fread");
  original_fread_unlocked = lookup_function("fread_unlocked");
  original_write = lookup_function("write");
  original_fwrite = lookup_function("fwrite");
  original_fwrite_unlocked = lookup_function("fwrite_unlocked");
#else
  jm_debug_printf(2, "WARNING: JumboMem is unable to intercept existing functions; many programs will fail.\n");
#endif

  /* Determine the default stack size for use with pthread_create(). */
  if (getrlimit(RLIMIT_STACK, &stacklimits) == -1) {
    jm_debug_printf(5, "WARNING: Failed to determine the limits on stack size (%s); using a default size for Pthreads\n",
                    jm_strerror(errno));
    stacklimits.rlim_cur = (rlim_t) default_pthread_stack_size;
  }
  else {
    if (stacklimits.rlim_cur == RLIM_INFINITY) {
      jm_debug_printf(5, "WARNING: Unlimited stack size; using a default size for Pthreads\n");
      stacklimits.rlim_cur = (rlim_t) default_pthread_stack_size;
    }
  }
  jm_debug_printf(5, "Setting the default Pthread stack size to %lu bytes\n",
                  (unsigned long) stacklimits.rlim_cur);
  default_pthread_stack_size = (size_t) stacklimits.rlim_cur;
}
