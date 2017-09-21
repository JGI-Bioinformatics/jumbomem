################################################
# Build the JumboMem memory server using SCons #
# By Scott Pakin <pakin@lanl.gov>              #
################################################

# ----------------------------------------------------------------------
# Copyright (C) 2010 Los Alamos National Security, LLC
#
# This file is part of JumboMem.
#
# This material was produced under U.S. Government contract
# DE-AC52-06NA25396 for Los Alamos National Laboratory (LANL), which is
# operated by Los Alamos National Security, LLC for the U.S. Department
# of Energy.  The U.S. Government has rights to use, reproduce, and
# distribute this software.  NEITHER THE GOVERNMENT NOR LOS ALAMOS
# NATIONAL SECURITY, LLC MAKES ANY WARRANTY, EXPRESS OR IMPLIED, OR
# ASSUMES ANY LIABILITY FOR THE USE OF THIS SOFTWARE.  If software is
# modified to produce derivative works, such modified software should be
# clearly marked so as not to confuse it with the version available
# from LANL.
#
# Additionally, this program is free software; you can redistribute it
# and/or modify it under the terms of the GNU General Public License as
# published by the Free Software Foundation; version 2.0 of the License.
# Accordingly, this program is distributed in the hope that it will be
# useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
# ----------------------------------------------------------------------

import os
import sys
import string
import re
import SCons.Script.Main

# Specify the current version of JumboMem.
jm_version = "2.1"

# Define a basic environment to use.
env = Environment(ENV = os.environ)

if os.environ.has_key("PREFIX"):
    env["PREFIX"] = os.environ["PREFIX"]
if os.environ.has_key("CC"):
    env["CC"] = os.environ["CC"]

# Define a function to copy the main environment.
def copy_env():
    "Copy the main environment."
    global env
    try:
        newenv = env.Clone()
    except AttributeError:
        newenv = env.Copy()
    return newenv

# Define a function to determine if size_t is a 64-bit value.  If not,
# then this is probably a 32-bit build and JumboMem will probably
# break horribly.
def Check64Bits(context):
    "Determine if size_t is a 64-bit value."
    context.Message("Checking for 64-bit support... ")
    result = context.TryRun('''
#include <sys/types.h>
int main (void)
{
  if (sizeof(size_t) < 8)
    return 1;
  else
    return 0;
}
''', ".c")
    context.Result(result[0])
    return result[0]

# Find the Linux system-call number associated with gettid().
def FindGettidSyscall(context):
    "Find gettid()'s system-call number."
    context.Message("Checking for a definition of __NR_gettid... ")
    have_NR_gettid = context.TryCompile('''
#include "sys/syscall.h"
int gettid_call = __NR_gettid;
''', ".c")
    context.Result(have_NR_gettid)
    return have_NR_gettid

# Define a function to replace various parameters in the
# wrapper-script template with their final values.
def CustomizeWrapperScript(target, source, env):
    "Replace wrapper-script parameters with actual values."
    # Copy the input file to the output file.
    infile = open(str(source[0]))
    outfile = open(str(target[0]), "w")
    oneline = infile.readline()
    while oneline:
        oneline = string.replace(oneline, "JM_VERSION=unknown",
                                 'JM_VERSION="%s"' % jm_version)
        oneline = string.replace(oneline, 'launchtemplate=""',
                                 "launchtemplate='%s'" % env["LAUNCHCMD"])
        oneline = string.replace(oneline, 'JM_RANKVAR=""',
                                 "JM_RANKVAR=%s" % env["RANKVAR"])
        if env["STATICLIB"]:
            oneline = string.replace(oneline, "staticlib=no", "staticlib=yes")
        oneline = string.replace(oneline, "LD_PRELOAD=",
                                 'LD_PRELOAD="%s"' % os.path.join(env["PREFIX"], "lib", str(library[0])))
        outfile.write(oneline)
        oneline = infile.readline()
    infile.close()
    outfile.close()

# Define a function to replace various parameters in the
# man page with their final values.
def CustomizeManPage(target, source, env):
    "Replace man-page parameters with actual values."
    # Copy the input file to the output file.
    infile = open(str(source[0]))
    outfile = open(str(target[0]), "w")
    oneline = infile.readline()
    while oneline:
        oneline = string.replace(oneline, "@VERSION@", "v%s" % jm_version)
        outfile.write(oneline)
        oneline = infile.readline()
    infile.close()
    outfile.close()

# Define a function to create a .tar.gz file of the JumboMem sources.
def CreateTarFile(target, source, env):
    "Copy all of the original (as opposed to generated) sources to a subdirectory."
    env.Execute(Mkdir(jm_versioned_name))
    for src in source:
        env.Execute(Copy(jm_versioned_name, str(src)))
    tarcmd = env.subst("${TAR}")
    target_name = str(target[0])
    env.Execute("%s -czf %s %s" % (tarcmd, target_name, jm_versioned_name))
    env.Execute(Delete(jm_versioned_name))

# Define a function to create a custom.py template.
def CreateCustomPy(target, source, env):
    "Generate a template custom.py file."
    target_name = str(target[0])
    custompy = open(target_name, "w")

    # Output some boilerplate text.
    custompy.write("%s\n" % '''\
####################################
# PARAMETERS FOR BUILDING JUMBOMEM #
####################################

# ----------------------------------------------------------- #
# Modify this file as necessary for your system then rerun    #
# "scons".  Note that this file is a Python script so the     #
# rules of Python syntax apply (cf. http://docs.python.org/). #
# ----------------------------------------------------------- #
''')

    # Define a function that outputs key:value pairs, using a default
    # value if the actual value is uninteresting.
    def showvariables(key_defvalue_list):
        "Output key:value pairs given a list of <key>:<default value> tuples."
        for key, defvalue in key_defvalue_list:
            try:
                value = env[key]
                if value == "" or value == []:
                    value = defvalue
            except KeyError:
                value = defvalue
            custompy.write("%s = %s\n" % (key, repr(value)))

    # Output variables used for compiling.
    custompy.write("# Compiling\n")
    showvariables([("CC", env["CC"]),
                   ("SHCC", env["CC"]),
                   ("CPPPATH", ["/usr/local/include"]),
                   ("#CPPDEFINES", ["FOO=BAR", "BAZ", "QUUX"]),
                   ("CFLAGS", "-O2 -g -Wall")])
    custompy.write("\n")

    # Output variables used for linking.
    custompy.write("# Linking\n")
    showvariables([("SHLINKFLAGS", ["$LINKFLAGS", "-shared"]),
                   ("LIBPATH", ["/usr/local/lib"]),
                   ("LIBS", ["mpi"])])
    custompy.write("\n")

    # Output variables used for installing.
    custompy.write("# Installing\n")
    showvariables([("PREFIX", env["PREFIX"])])
    custompy.write("\n")

    # Output variables used for running.
    custompy.write("# Running\n")
    showvariables([("#LAUNCHCMD", "mpirun -np $nodes")])
    showvariables([("#LAUNCHCMD", "prun -N $nodes")])
    showvariables([("#LAUNCHCMD", "srun -q -N $nodes -A mpirun -np $nodes -bynode --mca btl_openib_use_eager_rdma 0 --mca btl_openib_eager_limit 128 --mca btl_openib_max_send_size 12288 --mca btl self,sm,openib")])
    showvariables([("#RANKVAR", "MPIRUN_RANK")])
    showvariables([("#RANKVAR", "RMS_RANK")])
    custompy.close()

###########################################################################

# Perform a few initial checks.
malloc_hooks = 0
if not env.GetOption("clean"):
    config = Configure(env,
                       custom_tests = {"Check64Bits"       : Check64Bits,
                                       "FindGettidSyscall" : FindGettidSyscall})

    # Determine if this is a 64-bit build environment.  Abort if it isn't.
    if not config.Check64Bits():
        print "JumboMem requires a 64-bit build environment."
        Exit(1)

    # Determine if the C library's malloc() is derived from dlmalloc
    # (as is the case in glibc).  If so, we replace only the
    # memory-allocation hooks instead of replacing all of the
    # memory-allocation functions in their entirety.
    if config.CheckFunc("__malloc_hook"):
        malloc_hooks = 1

    # Ensure that we have pthread.h and a pthread library.
    if not config.CheckLibWithHeader("pthread", "pthread.h", "C"):
        print "JumboMem requires POSIX threads (Pthreads)."
        Exit(1)

    # See if we have sched.h and the sched_setaffinity() function.
    if config.CheckCHeader("sched.h") and config.CheckFunc("sched_setaffinity"):
        config.env.Prepend(CPPDEFINES=["HAVE_SCHED"])

    # See if we have gettid() or, if not, if we can invoke it via the
    # Linux syscall() interface.
    if config.CheckFunc("gettid"):
        config.env.Prepend(CPPDEFINES=["HAVE_GETTID"])
    elif config.CheckCHeader("sys/syscall.h") and config.CheckFunc("syscall") and config.FindGettidSyscall():
        config.env.Prepend(CPPDEFINES=["HAVE_GETTID_SYSCALL"])
    else:
        print "WARNING: This build of JumboMem will not be completely thread-safe."

    env = config.Finish()

# Let the user customize the build from the command line.
opts = Variables("custom.py")
opts.Add(PathVariable("PREFIX",
                      "Directory prefix beneath which to install JumboMem",
                      env["PREFIX"],
                      PathVariable.PathAccept))
opts.Add("LAUNCHCMD",
         'Template for launching a JumboMem job (with "$nodes" automatically replaced)',
         "")
opts.Add("RANKVAR",
         "Environment variable holding each process's rank in the computation",
         "")
opts.Add(PathVariable("DESTDIR",
                      "Staging directory for JumboMem installation",
                      "",
                      PathVariable.PathAccept))
opts.Add(EnumVariable("PAGEREPLACE",
                      "Page-replacement algorithm",
                      "nre",
                      allowed_values=("nre", "nru", "random", "fifo"),
                      ignorecase=2))
opts.Add(EnumVariable("PAGEALLOCATE",
                      "Page-allocation algorithm",
                      "rr",
                      allowed_values=("rr","block"),
                      ignorecase=2))
opts.Add(EnumVariable("SLAVETYPE",
                      "Slave communication protocol",
                      "mpi",
                      allowed_values=("mpi","shmem"),
                      ignorecase=2))
opts.Add("LIBBASE",
         "Base name of the target library",
         "jumbomem")
opts.Add(BoolVariable("STATICLIB",
                      "Build a static library instead of a shared library",
                      0))
opts.Add(BoolVariable("MALLOCHOOKS",
                      "Hook, instead of replace, malloc() et al.",
                      malloc_hooks))
opts.Add(BoolVariable("DEBUG",
                      "Include debugging code (with verbosity selected at run time)",
                      1))
opts.Update(env)
Help(opts.GenerateHelpText(env))

# Allow arbitrary other key=value pairs to be specified on the command
# line or in the custom.py file.
for key, value in ARGUMENTS.items():
    if key not in opts.keys():
        env[key] = value
        opts.Add(key, "Custom option")
try:
    import custom
    for key, value in custom.__dict__.items():
        if key[0] != "_":
            env[key] = value
            if key not in opts.keys():
                opts.Add(key, "Custom option")
except ImportError:
    pass

# Allow multiple libraries to be specified on the command line.
# (SCons v0.96.94.D001 has no inherent support for this.)
try:
    env["LIBS"] = string.split(env["LIBS"])
except KeyError:
    pass
except AttributeError:
    pass

# Pass additional arguments to all C files.
if env["STATICLIB"]:
    env.Prepend(CPPDEFINES=["JM_STATICLIB"])
if env["DEBUG"]:
    env.Prepend(CPPDEFINES=["JM_DEBUG"])
if env["PAGEALLOCATE"] == "block":
    env.Prepend(CPPDEFINES=["JM_DIST_BLOCK"])
if env["MALLOCHOOKS"]:
    env.Prepend(CPPDEFINES=["JM_MALLOC_HOOKS"])

# Ensure that we have mpi.h if we're building the MPI slave.
if not env.GetOption("clean") and env["SLAVETYPE"] == "mpi":
    config = Configure(env)
    if not config.CheckCHeader("mpi.h"):
        print 'ERROR: mpi.h is required when SLAVETYPE is set to "mpi"'
        #Exit(1)
    env = config.Finish()

# Map a filename to a custom environment.
custom_env = {}

# Pass the JumboMem version to initialize.c.
custom_env["initialize.c"] = copy_env()
custom_env["initialize.c"].Prepend(CPPDEFINES=["JM_VERSION='\"%s\"'" % jm_version])

# Build two versions of the memory-allocation routines: dlmalloc() et
# al. for use by the user's program and mspace_malloc() et al. for
# JumboMem's internal use.
Command("mspace-malloc.c", "dlmalloc.c", Copy("$TARGET", "$SOURCE"))
custom_env["dlmalloc.c"] = copy_env()
custom_env["dlmalloc.c"].Prepend(CPPDEFINES=[
    "mmap=jm_mmap",
    "CORRUPTION_ERROR_ACTION(M)=jm_abort(\\\"Memory corruption detected (external)\\\")",
    "USAGE_ERROR_ACTION(M,P)=jm_abort(\\\"Invalid free() or realloc() of external address %p\\\", P)",
    "USE_DL_PREFIX=1",
    "HAVE_MORECORE=1",
    "MORECORE=jm_morecore",
    "MORECORE_CONTIGUOUS=0",
    "MORECORE_CANNOT_TRIM=1",
    "HAVE_MMAP=0",
    "HAVE_MREMAP=0"])
custom_env["mspace-malloc.c"] = copy_env()
custom_env["mspace-malloc.c"].Prepend(CPPDEFINES=[
    "mmap=jm_mmap",
    "CORRUPTION_ERROR_ACTION(M)=jm_abort(\\\"Memory corruption detected (internal)\\\")",
    "USAGE_ERROR_ACTION(M,P)=jm_abort(\\\"Invalid free() or realloc() of internal address %p\\\", P)",
    "MORECORE_CONTIGUOUS=0",
    "ONLY_MSPACES=1"])

# Specify how to build the library (either shared or static).
libsources = [
    "allocate.c",
    "dlmalloc.c",
    "mspace-malloc.c",
    "initialize.c",
    "faulthandler.c",
    "miscfuncs.c",
    "funcoverrides.c",
    "sysinfo.c",
    "threadsupport.c",
    "pagetable.c",
    "pagereplace_%s.c" % env["PAGEREPLACE"],
    "slaves_%s.c" % env["SLAVETYPE"]]
env.Append(LIBS=["dl", "pthread"])

# Build either a shared or a static library.
libobjects = []
for src in libsources:
    if env["STATICLIB"]:
        # Build a static library (for debugging).
        try:
            custom_env[src].Object(src)
        except KeyError:
            env.Object(src)
        libobjects.append(os.path.splitext(src)[0] + env["OBJSUFFIX"])
    else:
        # Build a shared library (common case).
        try:
            custom_env[src].SharedObject(src)
        except KeyError:
            env.SharedObject(src)
        libobjects.append(os.path.splitext(src)[0] + env["SHOBJSUFFIX"])
if env["STATICLIB"]:
    library = env.StaticLibrary(target=env["LIBBASE"], source=libobjects)
else:
    library = env.SharedLibrary(target=env["LIBBASE"], source=libobjects)

# Build a library of user-callable JumboMem functions.
jmuser_lib = env.StaticLibrary(target="jmuser", source="jmuser.c")

###########################################################################

# Customize the JumboMem wrapper script and man page.
wrapper_script = env.Command("jumbomem",
                             ["jumbomem.in", "SConstruct",
                              env.Value(jm_version),
                              env.Value(env["LAUNCHCMD"]),
                              env.Value(env["RANKVAR"]),
                              env.Value(env["STATICLIB"]),
                              env.Value(env["PREFIX"])],
                             [CustomizeWrapperScript,
                              Chmod("$TARGET", 0755)])
man_page = env.Command("jumbomem.1",
                       ["jumbomem.1.in",
                        env.Value(jm_version)],
                       [CustomizeManPage])

# Build the findrankvars helper program.
findrankvars = env.Program("findrankvars.c")

# Install the libraries, wrapper script, helper program, man page, and
# header file when requested.
full_prefix = env["DESTDIR"] + env["PREFIX"]
libdir = os.path.join(full_prefix, "lib")
env.Install(libdir, [library, jmuser_lib])
bindir = os.path.join(full_prefix, "bin")
env.Install(bindir, [wrapper_script, findrankvars])
mandir = os.path.join(full_prefix, "share/man/man1")
env.Install(mandir, [man_page])
includedir = os.path.join(full_prefix, "include")
env.Install(includedir, "jmuser.h")
env.Alias("install", full_prefix)

# Create a tar archive if requested.
jm_versioned_name = "jumbomem-%s" % jm_version
tarfile = jm_versioned_name + ".tar.gz"
original_sources = [
    "README",
    "INSTALL",
    "COPYING",
    "Makefile",
    "SConstruct",
    "allocate.c",
    "dlmalloc.c",
    "faulthandler.c",
    "findrankvars.c",
    "initialize.c",
    "jumbomem.1.in",
    "jumbomem.h",
    "jumbomem.in",
    "miscfuncs.c",
    "funcoverrides.c",
    "pagetable.c",
    "pagereplace_fifo.c",
    "pagereplace_nru.c",
    "pagereplace_nre.c",
    "pagereplace_random.c",
    "slaves_mpi.c",
    "slaves_shmem.c",
    "sysinfo.c",
    "threadsupport.c",
    "jmuser.c",
    "jmuser.h",
    "testjm.c"]
if ("dist" in COMMAND_LINE_TARGETS
    or tarfile in COMMAND_LINE_TARGETS
    or env.GetOption("clean")):
    Command(tarfile, original_sources, CreateTarFile)
    Alias("dist", tarfile)

# Create a custom.py template if requested.
custompy = env.Command("custom.py", None, CreateCustomPy)
#env.NoClean(custompy)
