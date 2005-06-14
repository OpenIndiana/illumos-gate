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

#ifndef	_SYS_CPR_IMPL_H
#define	_SYS_CPR_IMPL_H

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#ifdef	__cplusplus
extern "C" {
#endif


#ifndef _ASM

#include <sys/processor.h>
#include <sys/machparam.h>
#include <sys/obpdefs.h>
#include <sys/vnode.h>
#include <sys/pte.h>

/*
 * This file contains machine dependent information for CPR
 */
#define	CPR_MACHTYPE_4U		0x3475		/* '4u' */

/*
 * Information about the pages allocated via prom_retain().
 * Increase the number of CPR_PROM_RETAIN_CNT if more
 * prom_retain() are called.
 */
#define	CPR_PROM_RETAIN_CNT	1
#define	CPR_PANICBUF		0	/* prom_retain() for panicbuf */


/*
 * For 2.7 and later releases, sun4u cprboot is an ELF64 binary and must
 * handle both ILP32 and LP64 kernels; while long and ptr sizes are fixed
 * at 64-bits for cprboot these sizes are mixed between ILP32/LP64 kernels.
 * To simplify handling of statefile data, we define fixed-size types for
 * all sun4u kernels.
 */
typedef uint64_t cpr_ptr;
typedef uint64_t cpr_ext;

struct cpr_map_info {
	cpr_ptr	virt;
	cpr_ext	phys;
	uint_t	size;
};


#define	CPR_MAX_TLB 16

struct sun4u_tlb {
	tte_t	tte;			/* tte data */
	cpr_ptr va_tag;			/* virt tag */
	int	index;			/* tlb index */
	int	tmp;			/* clear during resume */
};

typedef struct sun4u_tlb sutlb_t;


/*
 * processor info
 */
struct sun4u_cpu_info {
	dnode_t node;
	processorid_t cpu_id;
};


/*
 * This structure defines the fixed-length machine dependent data for
 * sun4u ILP32 and LP64 systems.  It is followed in the state file by
 * a variable length section of null-terminated prom forth words:
 *
 *    cpr_obp_tte_str	for translating kernel mappings, unix-tte
 *
 * The total length (fixed plus variable) of the machine-dependent
 * section is stored in cpr_machdep_desc.md_size
 *
 * WARNING: make sure all CPR_MD_* below match this structure
 */
struct cpr_sun4u_machdep {
	uint32_t ksb;			/* 0x00: kernel stack bias */
	uint16_t kpstate;		/* 0x04: kernel pstate */
	uint16_t kwstate;		/* 0x06: kernel wstate */
	cpr_ptr thrp;			/* 0x08: current thread ptr */
	cpr_ptr func;			/* 0x10: jumpback virt text addr */
	cpr_ext qsav_pc;		/* 0x18: qsav pc */
	cpr_ext qsav_sp;		/* 0x20: qsav sp */
	int	mmu_ctx_pri;		/* 0x28: primary context */
	int	mmu_ctx_sec;		/* 0x2c: secondary context */
	cpr_ptr	tmp_stack;		/* 0x30: base of data page */
	cpr_ext tmp_stacksize;		/* 0x38: leading area of data page */
	int	test_mode;		/* 0x40 */
	int	pad;			/* 0x44 */
	sutlb_t	dtte[CPR_MAX_TLB];	/* 0x48 */
	sutlb_t	itte[CPR_MAX_TLB];	/* 0x1c8 */
	struct	sun4u_cpu_info sci[NCPU]; /* 0x348 */
};
typedef struct cpr_sun4u_machdep csu_md_t;

#endif /* _ASM */


/*
 * XXX - these should be generated by a genassym,
 * but that doesn't work well for shared psm/kernel use
 */
#define	CPR_MD_KSB		0x00
#define	CPR_MD_KPSTATE		0x04
#define	CPR_MD_KWSTATE		0x06
#define	CPR_MD_THRP		0x08
#define	CPR_MD_FUNC		0x10
#define	CPR_MD_QSAV_PC		0x18
#define	CPR_MD_QSAV_SP		0x20
#define	CPR_MD_PRI		0x28
#define	CPR_MD_SEC		0x2c


#ifndef _ASM

#define	CPRBOOT		"-F cprboot"

#define	PN_TO_ADDR(pn)  ((u_longlong_t)(pn) << MMU_PAGESHIFT)
#define	ADDR_TO_PN(pa)	((pa) >> MMU_PAGESHIFT)

#define	prom_map_plat(addr, pa, size) \
	if (prom_map(addr, pa, size) == 0) { \
		errp("PROM_MAP failed: paddr=0x%lx\n", pa); \
		return (-1); \
	}

typedef	u_longlong_t	physaddr_t;

extern void i_cpr_machdep_setup(void);
extern void i_cpr_save_machdep_info(void);
extern void i_cpr_enable_intr(void);
extern void i_cpr_set_tbr(void);
extern void i_cpr_stop_intr(void);
extern void i_cpr_handle_xc(int);
extern void i_cpr_resume_setup(void *, csu_md_t *);
extern int i_cpr_write_machdep(vnode_t *);
extern int i_cpr_prom_pages(int);
extern int i_cpr_reuseinit(void);
extern int i_cpr_reusefini(void);
extern int i_cpr_check_cprinfo(void);
extern int i_cpr_reusable_supported(void);

#endif /* _ASM */

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_CPR_IMPL_H */
