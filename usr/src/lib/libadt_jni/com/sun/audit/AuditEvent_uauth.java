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
 * Copyright (c) 2001-2002 by Sun Microsystems, Inc.
 * All rights reserved.
 *
 * ident	"%Z%%M%	%I%	%E% SMI"
 */
package com.sun.audit;

// audit event:  AUE_uauth = 6199

public class AuditEvent_uauth extends AuditEvent {

	private native void putEvent(byte[]session, 
	    int status, int ret_val,
	    String	auth_used,
	    String	objectname);

	public AuditEvent_uauth(AuditSession session)
		throws Exception {
		super(session);
	}


	private String auth_used_val = "";	// required
	public void auth_used(String setTo) {
		auth_used_val = setTo;
	}

	private String objectname_val = "";	// required
	public void objectname(String setTo) {
		objectname_val = setTo;
	}

	public void putEvent(int status, int ret_val) {
		byte[]	session = super.sh.getSession();

		if ((super.sh.AuditIsOn) && (super.sh.ValidSession))
			putEvent(session, status, ret_val,
			    auth_used_val,
			    objectname_val);
	}
}
