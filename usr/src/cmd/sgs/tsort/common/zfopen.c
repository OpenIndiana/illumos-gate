/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*	Copyright (c) 1988 AT&T	*/
/*	  All Rights Reserved  	*/


#pragma ident	"%Z%%M%	%I%	%E% SMI"

/*
 *	fopen(3S) with error checking
 */

#include	"errmsg.h"
#include	<stdio.h>

FILE *
zfopen(severity, path, type)
int	severity;
char	*path;
char	*type;
{
	register FILE	*fp;	/* file pointer */

	if ((fp = fopen(path, type)) == NULL) {
		char	*mode;

		if (type[1] == '+')
			mode = "updating";
		else
			switch (type[0]) {
			case 'r':
				mode = "reading";
				break;
			case 'w':
				mode = "writing";
				break;
			case 'a':
				mode = "appending";
				break;
			default:
				mode = type;
			}
		_errmsg("UXzfopen1", severity,
			"Cannot open file \"%s\" for %s.",
			path, mode);
	}
	return (fp);
}
