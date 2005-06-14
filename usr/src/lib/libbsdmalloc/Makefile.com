#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License, Version 1.0 only
# (the "License").  You may not use this file except in compliance
# with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#
#
# Copyright 2004 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
# ident	"%Z%%M%	%I%	%E% SMI"
#

LIBRARY=	libbsdmalloc.a
VERS=		.1

OBJECTS=  \
	malloc.bsd43.o

# include library definitions
include ../../Makefile.lib

MAPFILE=	$(MAPDIR)/mapfile
SRCS=           $(OBJECTS:%.o=../common/%.c)
CLOBBERFILES +=	$(MAPFILE)

LIBS =          $(DYNLIB) $(LINTLIB)

# definitions for lint

LINTFLAGS =	-uax
LINTFLAGS64 =	-uax -Xarch=$(MACH64:sparcv9=v9)
LINTOUT=	lint.out

LINTSRC=	$(LINTLIB:%.ln=%)
ROOTLINTDIR=	$(ROOTLIBDIR)
ROOTLINT=	$(LINTSRC:%=$(ROOTLINTDIR)/%)

CLEANFILES +=	$(LINTOUT) $(LINTLIB)

CFLAGS +=	$(CCVERBOSE)
CPPFLAGS +=	-D_REENTRANT
DYNFLAGS +=     -M$(MAPFILE) $(ZINTERPOSE)
LDLIBS +=       -lc

.KEEP_STATE:

lint:
	$(LINT.c) $(SRCS) $(LDLIBS)

$(DYNLIB):      $(MAPFILE)

$(MAPFILE):
	@cd $(MAPDIR); $(MAKE) mapfile

#
# create message catalogue files
#
TEXT_DOMAIN= SUNW_OST_OSLIB
_msg:	$(MSGDOMAIN) catalog

catalog:
	sh ../makelibcmdcatalog.sh $(MSGDOMAIN)

$(MSGDOMAIN):
	$(INS.dir)

# include library targets
include ../../Makefile.targ

pics/%.o: ../common/%.c
	$(COMPILE.c) -o $@ $<
	$(POST_PROCESS_O)

# install rule for lint library target
$(ROOTLINTDIR)/%:	../common/%
	$(INS.file)

