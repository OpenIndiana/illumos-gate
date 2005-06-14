#!/bin/ksh -p
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
# Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
#ident	"%Z%%M%	%I%	%E% SMI"
#
# Builds bfu archives. If no arguments, uses the environment variables
# already set (by bldenv). One argument specifies an environment file
# like nightly or bldenv uses.

USAGE='Usage: $0 [ <env_file> ]'

if [ $# -gt 1 ]; then
	echo $USAGE >&2
	exit 1
fi

if [ $# -eq 1 ]; then
	if [ -z "$OPTHOME" ]; then
		OPTHOME=/opt
		export OPTHOME
	fi
	#
	#       Setup environmental variables
	#
	if [ -f $1 ]; then
		if [[ $1 = */* ]]; then
			. $1
		else
			. ./$1
		fi
	else
		if [ -f $OPTHOME/onbld/env/$1 ]; then
			. $OPTHOME/onbld/env/$1
		else
			echo "Cannot find env file as either $1 \c" >&2
			echo "or $OPTHOME/onbld/env/$1" >&2
			exit 1
		fi
	fi
fi

if [ -z "$ROOT" -o ! -d "$ROOT" ]; then
	echo '$ROOT must be set to a valid proto directory.' >&2
	exit 1
fi

if [ "$o_FLAG" != "y" ] && [ -z "$SRC" -o ! -d "$SRC" -o -z "$MACH" ]; then
	echo '$SRC and $MACH should be set to get archive permissions right.' \
	    >&2
fi

if [ -z "$CPIODIR" ]; then
	# $CPIODIR may not exist though, so no test for it
	echo '$CPIODIR must be set to a valid proto directory.' >&2
	exit 1
fi

zflag=""
archivetype=""
if [ -n "${NIGHTLY_OPTIONS}" ]; then
	zflag=`echo ${NIGHTLY_OPTIONS} | grep z`
	if [ -n "$zflag" ]; then
		zflag="-z"
		archivetype="compressed "
	fi
fi

export I_REALLY_WANT_TO_RUN_MKBFU=YES

echo "Making ${archivetype}archives from $ROOT in $CPIODIR."
if [ "$o_FLAG" != "y" -a -n "$MACH" -a -n "$SRC" -a -d "$SRC/pkgdefs" ]; then
	pkg=$SRC/pkgdefs
	bpkg=$SRC/realmode/pkgdefs
	exc=etc/exception_list_$MACH
	if [ "$X_FLAG" = "y" ]; then
		ipkg=$IA32_IHV_WS/usr/src/pkgdefs
		bpkgargs="-e $bpkg/$exc -e $ipkg/$exc $bpkg $ipkg"
	else
		bpkgargs=""
	fi
# boot/solaris/boot.bin is in $SRC/pkgdefs/SUNWcar.i, rather than in
# $SRC/realmode/pkgdefs, so the boot build needs to read both sources of
# packaging.
# usr/lib/fs/ufs/mboot and a few friends are in $SRC/realmode/pkgdefs,
# so the regular build also needs this packaging data.
	mkbfu	-f "cpiotranslate -e $pkg/$exc $bpkgargs $pkg" \
		$zflag $ROOT $CPIODIR
else
	mkbfu $zflag $ROOT $CPIODIR
fi

if [ -n "$MACH" -a -n "$SRC" -a -d "$SRC/pkgdefs" ]; then
	mkacr $SRC $MACH $CPIODIR
else
	cat >&2 <<'EOF'
$SRC and $MACH need to be set to create a conflict resolution archive.
EOF
fi

