/*
 * Copyright 2004 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

/*
 * Copyright (C) 1998 by the FundsXpress, INC.
 * 
 * All rights reserved.
 * 
 * Export of this software from the United States of America may require
 * a specific license from the United States Government.  It is the
 * responsibility of any person or organization contemplating export to
 * obtain such a license before exporting.
 * 
 * WITHIN THAT CONSTRAINT, permission to use, copy, modify, and
 * distribute this software and its documentation for any purpose and
 * without fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright notice and
 * this permission notice appear in supporting documentation, and that
 * the name of FundsXpress. not be used in advertising or publicity pertaining
 * to distribution of the software without specific, written prior
 * permission.  FundsXpress makes no representations about the suitability of
 * this software for any purpose.  It is provided "as is" without express
 * or implied warranty.
 * 
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

#include <k5-int.h>
#include <hash_provider.h>

static void
k5_sha1_hash_size(size_t *output)
{
    KRB5_LOG0(KRB5_INFO, "k5_sha1_hash_size() start");
    *output = SHS_DIGESTSIZE;
}

static void
k5_sha1_block_size(size_t *output)
{
    KRB5_LOG0(KRB5_INFO, "k5_sha1_block_size() start");
    *output = SHS_DATASIZE;
}

static krb5_error_code
k5_sha1_hash(krb5_context context,
	unsigned int icount, krb5_const krb5_data *input,
	krb5_data *output)
{
    CK_MECHANISM mechanism;

    mechanism.mechanism = CKM_SHA_1;
    mechanism.pParameter = NULL_PTR;
    mechanism.ulParameterLen = 0;

    return(k5_ef_hash(context, &mechanism, icount, input, output));

    return(0);
}

const struct krb5_hash_provider krb5_hash_sha1 = {
    k5_sha1_hash_size,
    k5_sha1_block_size,
    k5_sha1_hash
};
