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

LIBRARY=	libfrupicl.a
VERS=		.1

OBJECTS=	frupicl.o

# include library definitions
include $(SRC)/lib/Makefile.lib

SRCS=		$(OBJECTS:%.o=../%.c)
CLOBBERFILES += $(LIBLINKS)

LIBS =		$(DYNLIB)

LINTFLAGS =	-mnux
LINTFLAGS64 =	$(LINTFLAGS) -Xarch=$(MACH64:sparcv9=v9)
LINTOUT=	lint.out
LINTSRC =       $(LINTLIB:%.ln=%)
ROOTLINTDIR =   $(ROOTLIBDIR)
ROOTLINT =      $(LINTSRC:%=$(ROOTLINTDIR)/%)

CLEANFILES=	$(LINTOUT)

CPPFLAGS +=	-I.. \
		-I$(SRC)/lib/libfru/include \
		-I$(SRC)/cmd/picl/plugins/sun4u/frudata \
		-I$(SRC)/lib/libpicl \
		-I$(SRC)/lib/libfruutils \
		-I$(SRC)/cmd/picl/plugins/inc
CPPFLAGS += 	-D_REENTRANT
CFLAGS +=	$(CCVERBOSE)

$(LINTLIB) :=	LINTFLAGS = -nvx -I..
$(LINTLIB) :=	LINTFLAGS64 = -nvx -Xarch=$(MACH64:sparcv9=v9) -I..

XGETFLAGS += -a
POFILE=	picl.po

.KEEP_STATE:

all : $(LIBS)
	chmod 755 $(DYNLIB)

lint :	lintcheck

%.po:	../%.c
	$(CP) $< $<.i
	$(BUILD.po)

_msg:	$(MSGDOMAIN) $(POFILE)
	$(RM) $(MSGDOMAIN)/$(POFILE)
	$(CP) $(POFILE) $(MSGDOMAIN)

$(DYNLIB):	$(MAPFILE)

$(MAPFILE):
	@cd $(MAPDIR); $(MAKE) mapfile

# include library targets
include $(SRC)/lib/Makefile.targ

pics/%.o:	../%.c
	$(COMPILE.c) -o $@ $<
	$(POST_PROCESS_O)

$(ROOTLINTDIR)/%: ../%
	$(INS.file)
