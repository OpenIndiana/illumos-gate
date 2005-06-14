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
 * Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

/*
 * PX Control Block object
 */
#include <sys/types.h>
#include <sys/kmem.h>
#include <sys/async.h>
#include <sys/sunddi.h>
#include <sys/ddi_impldefs.h>
#include <sys/machsystm.h>
#include "px_obj.h"

/*LINTLIBRARY*/

int
px_cb_attach(px_t *px_p)
{
	sysino_t sysino;
	px_fault_t *fault_p = &px_p->px_cb_fault;
	caddr_t	csr = (caddr_t)px_p->px_address[PX_REG_XBC];
	px_cb_t *cb_p;

	/* if fail to get sysino, no need to proceed */
	if (px_lib_intr_devino_to_sysino(px_p->px_dip,
	    px_p->px_inos[PX_FAULT_XBC], &sysino) != DDI_SUCCESS)
		return (DDI_FAILURE);

	/*
	 * allocating JBC common block
	 */
	if ((cb_p = (px_cb_t *)px_lib_get_cb(csr)) == NULL) {
		cb_p = kmem_zalloc(sizeof (px_cb_t), KM_SLEEP);
		cb_p->xbc_px_p = px_p;
		mutex_init(&cb_p->xbc_intr_lock, NULL, MUTEX_DRIVER, NULL);
		cb_p->xbc_attachcnt++;
		px_p->px_cb_p = cb_p;
		px_lib_set_cb(csr, (uint64_t)cb_p);
	} else {
		cb_p->xbc_attachcnt++;
		px_p->px_cb_p = cb_p;
		return (DDI_SUCCESS);
	}

	/*
	 * initialize XBC fault data structure
	 * this happens on first attaching instance on sun4u,
	 * and every instance on sun4v.
	 */
	fault_p->px_fh_dip = px_p->px_dip;	/* ??? */
	fault_p->px_fh_sysino = sysino;
	fault_p->px_fh_lst = NULL;
	mutex_init(&fault_p->px_fh_lock, NULL, MUTEX_DRIVER, NULL);

	/*
	 * Register JBC Error interrupt.
	 */
	px_err_add_fh(fault_p, PX_ERR_JBC,
		(caddr_t)px_p->px_address[PX_REG_XBC]);

	/* activate JBC fault interrupt if not yet activated */
	return (px_err_add_intr(px_p,
	    &px_p->px_cb_fault, PX_FAULT_XBC));
}

void
px_cb_detach(px_t *px_p)
{
	px_cb_t *cb_p = px_p->px_cb_p;
	caddr_t	csr = (caddr_t)px_p->px_address[PX_REG_XBC];

	cb_p->xbc_attachcnt--;
	if (cb_p->xbc_attachcnt > 0) {
		px_p->px_cb_p = NULL;
		return;
	}

	px_err_rem_intr(px_p, PX_FAULT_XBC);
	px_err_rem(&px_p->px_cb_fault, PX_FAULT_XBC);
	mutex_destroy(&cb_p->xbc_intr_lock);
	px_lib_set_cb(csr, 0ull);
	px_p->px_cb_p = NULL;
	kmem_free(cb_p, sizeof (px_cb_t));
}

int
px_cb_intr(dev_info_t *dip, px_fh_t *fh_p)
{
	uint32_t offset = px_fhd_tbl[fh_p->fh_err_id].fhd_st;
	uint64_t stat = fh_p->fh_stat;
	if (stat)
		LOG(DBG_ERR_INTR, dip, "[%x]=%16llx cb stat\n", offset, stat);
	return (stat ? DDI_INTR_CLAIMED : DDI_INTR_UNCLAIMED);
}
