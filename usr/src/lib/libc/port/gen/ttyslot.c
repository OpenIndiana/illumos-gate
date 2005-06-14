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
/*
 * Copyright 2004 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

/*	Copyright (c) 1988 AT&T	*/
/*	  All Rights Reserved  	*/


/*
 * Return the number of the slot in the utmp file
 * corresponding to the current user: try for file 0, 1, 2.
 * Returns -1 if slot not found.
 */

#pragma weak ttyslot = _ttyslot

#include "synonyms.h"
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utmpx.h>
#include <stdlib.h>

#ifndef TRUE
#define	TRUE 1
#define	FALSE 0
#endif

int
ttyslot(void)
{
	struct futmpx ubuf;
	char *tp, *p;
	int s;
	int ret = -1, console = FALSE;
	char ttynm[128];
	FILE	*fp;

	if ((tp = ttyname_r(0, ttynm, 128)) == NULL &&
	    (tp = ttyname_r(1, ttynm, 128)) == NULL &&
	    (tp = ttyname_r(2, ttynm, 128)) == NULL)
		return (-1);

	p = tp;
	if (strncmp(tp, "/dev/", 5) == 0)
		p += 5;

	if (strcmp(p, "console") == 0)
		console = TRUE;

	s = 0;
	if ((fp = fopen(UTMPX_FILE, "r")) == NULL)
		return (-1);
	while ((fread(&ubuf, sizeof (ubuf), 1, fp)) == 1) {
		if ((ubuf.ut_type == INIT_PROCESS ||
		    ubuf.ut_type == LOGIN_PROCESS ||
		    ubuf.ut_type == USER_PROCESS) &&
		    strncmp(p, ubuf.ut_line, sizeof (ubuf.ut_line)) == 0) {
			ret = s;
			if (!console || strncmp(ubuf.ut_host, ":0", 2) == 0)
				break;
		}
		s++;
	}
	(void) fclose(fp);
	return (ret);
}
