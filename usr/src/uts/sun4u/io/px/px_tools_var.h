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

#ifndef _SYS_PX_TOOLS_VAR_H
#define	_SYS_PX_TOOLS_VAR_H

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * Functions exported from the px_tools.c module.
 */
int px_dev_reg_ops(dev_info_t *dip, void *arg, int cmd, int mode, px_t *px_p);
int px_bus_reg_ops(dev_info_t *dip, void *arg, int cmd, int mode);
int px_intr_admn(dev_info_t *dip, void *arg, int cmd, int mode, px_t *px_p);

/*
 * PCI Space definitions.
 */
#define	PCI_CONFIG_RANGE_BANK	(PCI_REG_ADDR_G(PCI_ADDR_CONFIG))
#define	PCI_IO_RANGE_BANK	(PCI_REG_ADDR_G(PCI_ADDR_IO))
#define	PCI_MEM_RANGE_BANK	(PCI_REG_ADDR_G(PCI_ADDR_MEM32))
#define	PCI_MEM64_RANGE_BANK	(PCI_REG_ADDR_G(PCI_ADDR_MEM64))

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_PX_TOOLS_VAR_H */
