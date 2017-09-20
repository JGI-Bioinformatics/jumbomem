###################################
# Makefile wrapper for SCons	  #
# By Scott Pakin <pakin@lanl.gov> #
###################################

# ----------------------------------------------------------------------
#
#                            *** NOTE ***
#
# JumboMem builds using SCons (http://www.scons.org/), not make.
#
# This file is just a crude wrapper script so people who don't read
# instructions may still be able to build JumboMem with "make" and
# install it with "make install".
#
# As you're clearly the type who reads instructions (albeit possibly
# after "make" failed), here's what you need to know:
#
#   * To customize the build process (to change compiler flags,
#     libraries, etc.), edit custom.py.  Run "scons custom.py" (or
#     "make custom.py" if you prefer) to produce a template custom.py
#     file that can be used as a starting point.
# 
#   * Run "scons" (or "make") to compile JumboMem.
# 
#   * Run "scons install" (or "make install") to install JumboMem.
# 
#   * Run "scons -c" (or "make clean") to delete all object files and
#     start over from a clean system.  Run "scons -c install" (or "make
#     uninstall") to additionally uninstall JumboMem.
#
# ----------------------------------------------------------------------
#
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
#
# ----------------------------------------------------------------------


# The following are for the benefit of package-management systems.
prefix = /usr/local
DESTDIR =

###########################################################################

all: complain_scons custom.py
	scons -Q

clean: complain_scons
	scons -Q -c

install: complain_scons custom.py
	scons -Q install DESTDIR=$(DESTDIR) PREFIX=$(PREFIX)

uninstall: complain_scons custom.py
	scons -Q -c install DESTDIR=$(DESTDIR) PREFIX=$(PREFIX)

dist: complain_scons
	scons -Q dist

###########################################################################

complain_scons:
	@echo "*********************************************************"
	@echo "*** JumboMem builds using SCons, not make.            ***"
	@if scons -v > /dev/null 2>&1 ; then \
	  echo "*** For your convenience I'll run scons now but you   ***" ; \
	  echo "*** should really run it directly.                    ***" ; \
	  echo "*********************************************************" ; \
	  echo "" ; \
	else \
	  echo "*** Please install SCons -- it can be downloaded      ***" ; \
	  echo "*** from http://www.scons.org/ -- and use that        ***" ; \
	  echo "*** instead of make.                                  ***" ; \
	  echo "*********************************************************" ; \
	  false ; \
	fi

custom.py:
	scons custom.py

###########################################################################
