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
 * Copyright (c) 1999-2001 by Sun Microsystems, Inc.
 * All rights reserved.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

/*
 * file: mipagentsnmp_trap.c
 *
 * This file contains the SNMP trap routines.
 */

#include <sys/types.h>
#include <netinet/in.h>

#include <impl.h>
#include <node.h>

#include "snmp_stub.h"

#ifndef lint
struct CallbackItem genCallItem[10];
int genNumCallItem = 0;
int genNumTrapElem = 0;
int genTrapTableMap[10];
struct TrapHndlCxt genTrapBucket[10];
struct TrapAnyEnterpriseInfo genTrapAnyEnterpriseInfo[10];
#endif
