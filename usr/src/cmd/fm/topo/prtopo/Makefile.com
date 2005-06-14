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
#ident	"%Z%%M%	%I%	%E% SMI"

.KEEP_STATE:

PROG = prtopo

OBJS = $(PROG:=.o)
SRCS = $(OBJS:.o=.c)

LINTSRCS = $(OBJS:%.o=../common/%.c)
LINTFLAGS = -mnux

#
# Reset STRIPFLAG to the empty string.  esc is intentionally
# installed with symbol tables to aid compiler debugging.
#
STRIPFLAG=
CPPFLAGS += -I../common
CFLAGS += $(CTF_FLAGS)

LDFLAGS += -R/usr/lib/fm
LDLIBS += -lnvpair -L$(ROOTLIB)/fm -ltopo

ROOTPDIR = $(ROOT)/usr/lib/fm
ROOTPROG = $(ROOTPDIR)/$(PROG)

all debug: $(PROG)

install: all $(ROOTPROG)

_msg install_h:

lint:	$(LINTSRCS)
	$(LINT.c) $(LINTSRCS) $(LDLIBS)

$(PROG): $(OBJS)
	$(LINK.c) -o $@ $(OBJS) $(LDLIBS)
	$(CTFMRG)
	$(POST_PROCESS)

clean:
	$(RM) $(OBJS) core

clobber: clean
	$(RM) $(PROG)

$(ROOT)/usr/lib/fm:
	$(INS.dir)

$(ROOTPDIR): $(ROOT)/usr/lib/fm
	$(INS.dir)

$(ROOTPDIR)/%: %
	$(INS.file)

%.o: ../common/%.c
	$(COMPILE.c) $<
	$(CTFCONVO)
