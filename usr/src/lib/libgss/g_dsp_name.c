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

/*
 *  glue routine for gss_display_name()
 *
 */

#include <mechglueP.h>
#include <stdio.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#include <string.h>

OM_uint32
gss_display_name(minor_status,
			input_name,
			output_name_buffer,
			output_name_type)

OM_uint32 *			minor_status;
const gss_name_t		input_name;
gss_buffer_t			output_name_buffer;
gss_OID *			output_name_type;

{
	OM_uint32			major_status;
	gss_union_name_t		union_name;

	if (minor_status == NULL)
		return (GSS_S_CALL_INACCESSIBLE_WRITE);
	*minor_status = 0;

	if (input_name == 0)
		return (GSS_S_CALL_INACCESSIBLE_READ | GSS_S_BAD_NAME);

	if (output_name_buffer == NULL)
		return (GSS_S_CALL_INACCESSIBLE_WRITE);

	if (output_name_type)
		*output_name_type = NULL;

	union_name = (gss_union_name_t)input_name;

	if (union_name->mech_type) {
		/*
		 * OK, we have a mechanism-specific name; let's use it!
		 */
		return (__gss_display_internal_name(minor_status,
							union_name->mech_type,
							union_name->mech_name,
							output_name_buffer,
							output_name_type));
	}

	/*
	 * copy the value of the external_name component of the union
	 * name into the output_name_buffer and point the output_name_type
	 * to the name_type component of union_name
	 */
	if (output_name_type != NULL &&
	    union_name->name_type != GSS_C_NULL_OID) {
		major_status = generic_gss_copy_oid(minor_status,
						union_name->name_type,
						output_name_type);
		if (major_status != GSS_S_COMPLETE)
			return (major_status);
	}

	if ((output_name_buffer->value =
		malloc(union_name->external_name->length + 1)) == NULL) {
		if (output_name_type && *output_name_type != GSS_C_NULL_OID) {
			(void) generic_gss_release_oid(minor_status,
						    output_name_type);
			*output_name_type = NULL;
		}
		return (GSS_S_FAILURE);
	}
	output_name_buffer->length = union_name->external_name->length;
	(void) memcpy(output_name_buffer->value,
		    union_name->external_name->value,
		union_name->external_name->length);
	((char *)output_name_buffer->value)[output_name_buffer->length] = '\0';

	return (GSS_S_COMPLETE);
}
