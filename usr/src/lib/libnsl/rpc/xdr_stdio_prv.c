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
 * Copyright 2003 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"
/*
 * xdr_stdio_prv.c, XDR implementation on special standard i/o file.
 *
 * To avoid the file descriptor limitation in stdio, we implement
 * a private version of the same routines from xdr_stdio.c  using
 * modified FILE structure. ( __NSL_FILE )
 * This set of routines implements a XDR on a special stdio stream.
 * XDR_ENCODE serializes onto the stream, XDR_DECODE de-serializes
 * from the stream.
 */

#include "rpc_mt.h"
#include <rpc/types.h>
#include <stdio.h>
#include <rpc/xdr.h>
#include <sys/types.h>
#include <rpc/trace.h>
#include <inttypes.h>
#include "nsl_stdio_prv.h"

static struct xdr_ops *__nsl_xdrstdio_ops(void);

/*
 * Initialize a stdio xdr stream.
 * Sets the xdr stream handle xdrs for use on the stream file.
 * Operation flag is set to op.
 */
void
__nsl_xdrstdio_create(XDR *xdrs, __NSL_FILE *file, enum xdr_op op)
{
	trace1(TR_xdrstdio_create, 0);
	xdrs->x_op = op;
	xdrs->x_ops = __nsl_xdrstdio_ops();
	xdrs->x_private = (caddr_t)file;
	xdrs->x_handy = 0;
	xdrs->x_base = 0;
	trace1(TR_xdrstdio_create, 1);
}

/*
 * Destroy a stdio xdr stream.
 * Cleans up the xdr stream handle xdrs previously set up by xdrstdio_create.
 */
static void
__nsl_xdrstdio_destroy(XDR *xdrs)
{
	trace1(TR_xdrstdio_destroy, 0);
	(void) __nsl_fflush((__NSL_FILE *)xdrs->x_private);
	/* xx should we close the file ?? */
	trace1(TR_xdrstdio_destroy, 1);
}


static bool_t
__nsl_xdrstdio_getint32(XDR *xdrs, int32_t *lp)
{
	trace1(TR_xdrstdio_getint32, 0);
	if (__nsl_fread((caddr_t)lp, sizeof (int32_t), 1,
			(__NSL_FILE *)xdrs->x_private) != 1) {
		trace1(TR_xdrstdio_getint32, 1);
		return (FALSE);
	}
	*lp = ntohl(*lp);
	trace1(TR_xdrstdio_getint32, 1);
	return (TRUE);
}

static bool_t
__nsl_xdrstdio_putint32(XDR *xdrs, int32_t *lp)
{

	int32_t mycopy = htonl(*lp);
	lp = &mycopy;

	trace1(TR_xdrstdio_putint32, 0);
	if (__nsl_fwrite((caddr_t)lp, sizeof (int32_t), 1,
			(__NSL_FILE *)xdrs->x_private) != 1) {
		trace1(TR_xdrstdio_putint32, 1);
		return (FALSE);
	}
	trace1(TR_xdrstdio_putint32, 1);
	return (TRUE);
}

static bool_t
__nsl_xdrstdio_getlong(xdrs, lp)
	XDR *xdrs;
	register long *lp;
{
	int32_t i;

	if (!__nsl_xdrstdio_getint32(xdrs, &i))
		return (FALSE);

	*lp = (long)i;

	return (TRUE);
}

static bool_t
__nsl_xdrstdio_putlong(xdrs, lp)
	XDR *xdrs;
	long *lp;
{
	int32_t i;

#if defined(_LP64)
	if ((*lp > INT32_MAX) || (*lp < INT32_MIN)) {
		return (FALSE);
	}
#endif

	i = (int32_t)*lp;

	return (__nsl_xdrstdio_putint32(xdrs, &i));
}

static bool_t
__nsl_xdrstdio_getbytes(XDR *xdrs, caddr_t addr, int len)
{
	trace2(TR_xdrstdio_getbytes, 0, len);
	if ((len != 0) &&
		(__nsl_fread(addr, (int)len, 1,
			(__NSL_FILE *)xdrs->x_private) != 1)) {
		trace1(TR_xdrstdio_getbytes, 1);
		return (FALSE);
	}
	trace1(TR_xdrstdio_getbytes, 1);
	return (TRUE);
}

static bool_t
__nsl_xdrstdio_putbytes(XDR *xdrs, caddr_t addr, int len)
{
	trace2(TR_xdrstdio_putbytes, 0, len);
	if ((len != 0) &&
		(__nsl_fwrite(addr, (int)len, 1,
			(__NSL_FILE *)xdrs->x_private) != 1)) {
		trace1(TR_xdrstdio_putbytes, 1);
		return (FALSE);
	}
	trace1(TR_xdrstdio_putbytes, 1);
	return (TRUE);
}

static uint_t
__nsl_xdrstdio_getpos(XDR *xdrs)
{
	uint_t dummy1;

	trace1(TR_xdrstdio_getpos, 0);
	dummy1 = (uint_t)__nsl_ftell((__NSL_FILE *)xdrs->x_private);
	trace1(TR_xdrstdio_getpos, 1);
	return (dummy1);
}

static bool_t
__nsl_xdrstdio_setpos(XDR *xdrs, uint_t pos)
{
	bool_t dummy2;

	trace2(TR_xdrstdio_setpos, 0, pos);
	dummy2 = (__nsl_fseek((__NSL_FILE *)xdrs->x_private,
			(int)pos, 0) < 0) ? FALSE : TRUE;
	trace1(TR_xdrstdio_setpos, 1);
	return (dummy2);
}

static rpc_inline_t *
__nsl_xdrstdio_inline(XDR *xdrs, int len)
{

	/*
	 * Must do some work to implement this: must insure
	 * enough data in the underlying stdio buffer,
	 * that the buffer is aligned so that we can indirect through a
	 * long *, and stuff this pointer in xdrs->x_buf.  Doing
	 * a fread or fwrite to a scratch buffer would defeat
	 * most of the gains to be had here and require storage
	 * management on this buffer, so we don't do this.
	 */
	trace2(TR_xdrstdio_inline, 0, len);
	trace2(TR_xdrstdio_inline, 1, len);
	return (NULL);
}

static bool_t
__nsl_xdrstdio_control(XDR *xdrs, int request, void *info)
{
	switch (request) {

	default:
		return (FALSE);
	}
}

static struct xdr_ops *
__nsl_xdrstdio_ops()
{
	static struct xdr_ops ops;
	extern mutex_t	ops_lock;

/* VARIABLES PROTECTED BY ops_lock: ops */

	trace1(TR_xdrstdio_ops, 0);
	mutex_lock(&ops_lock);
	if (ops.x_getlong == NULL) {
		ops.x_getlong = __nsl_xdrstdio_getlong;
		ops.x_putlong = __nsl_xdrstdio_putlong;
		ops.x_getbytes = __nsl_xdrstdio_getbytes;
		ops.x_putbytes = __nsl_xdrstdio_putbytes;
		ops.x_getpostn = __nsl_xdrstdio_getpos;
		ops.x_setpostn = __nsl_xdrstdio_setpos;
		ops.x_inline = __nsl_xdrstdio_inline;
		ops.x_destroy = __nsl_xdrstdio_destroy;
		ops.x_control = __nsl_xdrstdio_control;
#if defined(_LP64)
		ops.x_getint32 = __nsl_xdrstdio_getint32;
		ops.x_putint32 = __nsl_xdrstdio_putint32;
#endif
	}
	mutex_unlock(&ops_lock);
	trace1(TR_xdrstdio_ops, 1);
	return (&ops);
}
