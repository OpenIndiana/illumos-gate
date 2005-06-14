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
 * SFMMU primitives.  These primitives should only be used by sfmmu
 * routines.
 */

#if defined(lint)
#include <sys/types.h>
#else	/* lint */
#include "assym.h"
#endif	/* lint */

#include <sys/asm_linkage.h>
#include <sys/machtrap.h>
#include <sys/machasi.h>
#include <sys/sun4asi.h>
#include <sys/pte.h>
#include <sys/mmu.h>
#include <vm/hat_sfmmu.h>
#include <vm/seg_spt.h>
#include <sys/machparam.h>
#include <sys/privregs.h>
#include <sys/scb.h>
#include <sys/intreg.h>
#include <sys/machthread.h>
#include <sys/clock.h>
#include <sys/trapstat.h>

/*
 * sfmmu related subroutines
 */

#if defined (lint)

/*
 * sfmmu related subroutines
 */

/* ARGSUSED */
void
sfmmu_ctx_steal_tl1(uint64_t sctx, uint64_t rctx)
{}

/* ARGSUSED */
void
sfmmu_raise_tsb_exception(uint64_t sctx, uint64_t rctx)
{}

/* ARGSUSED */
void
sfmmu_itlb_ld(caddr_t vaddr, int ctxnum, tte_t *tte)
{}

/* ARGSUSED */
void
sfmmu_dtlb_ld(caddr_t vaddr, int ctxnum, tte_t *tte)
{}

int
sfmmu_getctx_pri()
{ return(0); }

int
sfmmu_getctx_sec()
{ return(0); }

/* ARGSUSED */
void
sfmmu_setctx_sec(int ctx)
{}

/* ARGSUSED */
void
sfmmu_load_mmustate(sfmmu_t *sfmmup)
{
}

#else	/* lint */

/*
 * 1. If stealing ctx, flush all TLB entries whose ctx is ctx-being-stolen.
 * 2. If processor is running in the ctx-being-stolen, set the
 *    context to the resv context. That is 
 *    If processor in User-mode - pri/sec-ctx both set to ctx-being-stolen,
 *		change both pri/sec-ctx registers to resv ctx.
 *    If processor in Kernel-mode - pri-ctx is 0, sec-ctx is ctx-being-stolen,
 *		just change sec-ctx register to resv ctx. When it returns to
 *		kernel-mode, user_rtt will change pri-ctx.
 *
 * Note: For multiple page size TLB, no need to set page sizes for
 *       DEMAP context.
 *
 * %g1 = ctx being stolen (victim)
 * %g2 = invalid ctx to replace victim with
 */
	ENTRY(sfmmu_ctx_steal_tl1)
	/*
	 * Flush TLBs.
	 */
	set	MMU_PCONTEXT, %g3
	set	DEMAP_CTX_TYPE | DEMAP_PRIMARY, %g4
	ldxa	[%g3]ASI_MMU_CTX, %g5		/* get pri-ctx */
	sethi	%hi(FLUSH_ADDR), %g6
	stxa	%g1, [%g3]ASI_MMU_CTX		/* temporarily set our */
						/*   pri-ctx to victim */
	stxa	%g0, [%g4]ASI_DTLB_DEMAP	/* flush DTLB */
	stxa	%g0, [%g4]ASI_ITLB_DEMAP	/* flush ITLB */
	stxa	%g5, [%g3]ASI_MMU_CTX		/* restore original pri-ctx */
	flush	%g6				/* ensure stxa's committed */
	/* fall through to the code below */

	/*
	 * We enter here if we're just raising a TSB miss
	 * exception, without switching MMU contexts.  In
	 * this case, there is no need to flush the TLB.
	 */
	ALTENTRY(sfmmu_raise_tsb_exception)
	!
	! if (sec-ctx != victim) {
	!	return
	! } else {
	!	if (pri-ctx == victim) {
	!		write INVALID_CONTEXT to sec-ctx
	!		write INVALID_CONTEXT to pri-ctx
	!	} else {
	!		write INVALID_CONTEXT to sec-ctx
	!	}
	! }
	!
	cmp	%g1, NUM_LOCKED_CTXS
	blt,a,pn %icc, ptl1_panic		/* can't steal locked ctx */
	  mov	PTL1_BAD_CTX_STEAL, %g1
	set	CTXREG_CTX_MASK, %g6
	set	MMU_SCONTEXT, %g3
	ldxa	[%g3]ASI_MMU_CTX, %g5		/* get sec-ctx */
	and	%g5, %g6, %g5
	cmp	%g5, %g1			/* is it the victim? */
	bne,pn	%icc, 2f			/* was our sec-ctx a victim? */
	  mov	MMU_PCONTEXT, %g7
	ldxa	[%g7]ASI_MMU_CTX, %g4		/* get pri-ctx */
	and	%g4, %g6, %g4
	stxa	%g2, [%g3]ASI_MMU_CTX		/* set sec-ctx to invalid ctx */
	membar	#Sync
	cmp	%g1, %g4			/* is it the victim? */
	bne 	%icc, 2f			/* nope, no need to change it */
	  nop
	stxa	%g2, [%g7]ASI_MMU_CTX		/* set pri-ctx to invalid ctx */
	/* next instruction is retry so no membar sync */
2:
	retry
	SET_SIZE(sfmmu_ctx_steal_tl1)

	ENTRY_NP(sfmmu_itlb_ld)
	rdpr	%pstate, %o3
#ifdef DEBUG
	andcc	%o3, PSTATE_IE, %g0		! If interrupts already
	bnz,pt %icc, 1f				!   disabled, panic
	  nop

	sethi	%hi(panicstr), %g1
	ldx	[%g1 + %lo(panicstr)], %g1
	tst	%g1
	bnz,pt	%icc, 1f
	  nop
	
	sethi	%hi(sfmmu_panic1), %o0
	call	panic
	 or	%o0, %lo(sfmmu_panic1), %o0
1:
#endif /* DEBUG */
	wrpr	%o3, PSTATE_IE, %pstate		! Disable interrupts
	srln	%o0, MMU_PAGESHIFT, %o0
	slln	%o0, MMU_PAGESHIFT, %o0		! Clear page offset
	or	%o0, %o1, %o0
	ldx	[%o2], %g1
	set	MMU_TAG_ACCESS, %o5
#ifdef	CHEETAHPLUS_ERRATUM_34
	!
	! If this is Cheetah or derivative and the specified TTE is locked
	! and hence to be loaded into the T16, fully-associative TLB, we
	! must avoid Cheetah+ erratum 34.  In Cheetah+ erratum 34, under
	! certain conditions an ITLB locked index 0 TTE will erroneously be
	! displaced when a new TTE is loaded via ASI_ITLB_IN.  To avoid
	! this erratum, we scan the T16 top down for an unlocked TTE and
	! explicitly load the specified TTE into that index.
	!
	GET_CPU_IMPL(%g2)
	cmp	%g2, CHEETAH_IMPL
	bl,pn	%icc, 0f
	  nop

	andcc	%g1, TTE_LCK_INT, %g0
	bz	%icc, 0f			! Lock bit is not set;
						!   load normally.
	  or	%g0, (15 << 3), %g3		! Start searching from the
						!   top down.

1:
	ldxa	[%g3]ASI_ITLB_ACCESS, %g4	! Load TTE from t16

	!
	! If this entry isn't valid, we'll choose to displace it (regardless
	! of the lock bit).
	!
	cmp	%g4, %g0
	bge	%xcc, 2f			! TTE is > 0 iff not valid
	  andcc	%g4, TTE_LCK_INT, %g0		! Check for lock bit
	bz	%icc, 2f			! If unlocked, go displace
	  nop
	sub	%g3, (1 << 3), %g3
	brgz	%g3, 1b				! Still more TLB entries
	  nop					! to search

	sethi   %hi(sfmmu_panic5), %o0          ! We searched all entries and
	call    panic                           ! found no unlocked TTE so
	  or    %o0, %lo(sfmmu_panic5), %o0     ! give up.

	
2:
	!
	! We have found an unlocked or non-valid entry; we'll explicitly load
	! our locked entry here.
	!
	sethi	%hi(FLUSH_ADDR), %o1		! Flush addr doesn't matter
	stxa	%o0, [%o5]ASI_IMMU
	stxa	%g1, [%g3]ASI_ITLB_ACCESS
	flush	%o1				! Flush required for I-MMU
	ba	3f				! Delay slot of ba is empty
	nop					!   per Erratum 64

0:
#endif	/* CHEETAHPLUS_ERRATUM_34 */
	sethi	%hi(FLUSH_ADDR), %o1		! Flush addr doesn't matter
	stxa	%o0, [%o5]ASI_IMMU
	stxa	%g1, [%g0]ASI_ITLB_IN
	flush	%o1				! Flush required for I-MMU
3:
	retl
	  wrpr	%g0, %o3, %pstate		! Enable interrupts
	SET_SIZE(sfmmu_itlb_ld)

	/*
	 * Load an entry into the DTLB.
	 *
	 * Special handling is required for locked entries since there
	 * are some TLB slots that are reserved for the kernel but not
	 * always held locked.  We want to avoid loading locked TTEs
	 * into those slots since they could be displaced.
	 */
	ENTRY_NP(sfmmu_dtlb_ld)
	rdpr	%pstate, %o3
#ifdef DEBUG
	andcc	%o3, PSTATE_IE, %g0		! if interrupts already
	bnz,pt	%icc, 1f			! disabled, panic
	  nop

	sethi	%hi(panicstr), %g1
	ldx	[%g1 + %lo(panicstr)], %g1
	tst	%g1
	bnz,pt	%icc, 1f
	  nop

	sethi	%hi(sfmmu_panic1), %o0
	call	panic
	 or	%o0, %lo(sfmmu_panic1), %o0
1:
#endif /* DEBUG */
	wrpr	%o3, PSTATE_IE, %pstate		! disable interrupts
	srln	%o0, MMU_PAGESHIFT, %o0
	slln	%o0, MMU_PAGESHIFT, %o0		! clear page offset
	or	%o0, %o1, %o0			! or in ctx to form tagacc
	ldx	[%o2], %g1
	sethi	%hi(ctx_pgsz_array), %o2	! Check for T8s
	ldn	[%o2 + %lo(ctx_pgsz_array)], %o2
	brz	%o2, 1f
	set	MMU_TAG_ACCESS, %o5
	ldub	[%o2 + %o1], %o2		! Cheetah+: set up tag access
	sll	%o2, TAGACCEXT_SHIFT, %o2	! extension register so entry
	set	MMU_TAG_ACCESS_EXT, %o4		! can go into T8 if unlocked
	stxa	%o2,[%o4]ASI_DMMU
	membar	#Sync
1:
	andcc	%g1, TTE_LCK_INT, %g0		! Locked entries require
	bnz,pn	%icc, 2f			! special handling
	  sethi	%hi(dtlb_resv_ttenum), %g3
	stxa	%o0,[%o5]ASI_DMMU		! Load unlocked TTE
	stxa	%g1,[%g0]ASI_DTLB_IN		! via DTLB_IN
	membar	#Sync
	retl
	  wrpr	%g0, %o3, %pstate		! enable interrupts
2:
	ld	[%g3 + %lo(dtlb_resv_ttenum)], %g3
	sll	%g3, 3, %g3			! First reserved idx in TLB 0
	sub	%g3, (1 << 3), %g3		! Decrement idx
3:
	ldxa	[%g3]ASI_DTLB_ACCESS, %g4	! Load TTE from TLB 0
	!
	! If this entry isn't valid, we'll choose to displace it (regardless
	! of the lock bit).
	!
	brgez,pn %g4, 4f			! TTE is > 0 iff not valid
	  nop
	andcc	%g4, TTE_LCK_INT, %g0		! Check for lock bit
	bz,pn	%icc, 4f			! If unlocked, go displace
	  nop
	sub	%g3, (1 << 3), %g3		! Decrement idx
	brgez	%g3, 3b			
	  nop
	sethi	%hi(sfmmu_panic5), %o0		! We searched all entries and
	call	panic				! found no unlocked TTE so
	  or	%o0, %lo(sfmmu_panic5), %o0	! give up.
4:
	stxa	%o0,[%o5]ASI_DMMU		! Setup tag access
	stxa	%g1,[%g3]ASI_DTLB_ACCESS	! Displace entry at idx
	membar	#Sync
	retl
	  wrpr	%g0, %o3, %pstate		! enable interrupts
	SET_SIZE(sfmmu_dtlb_ld)

	ENTRY_NP(sfmmu_getctx_pri)
	set	MMU_PCONTEXT, %o0
	retl
	  ldxa	[%o0]ASI_MMU_CTX, %o0
	SET_SIZE(sfmmu_getctx_pri)

	ENTRY_NP(sfmmu_getctx_sec)
	set	MMU_SCONTEXT, %o0
	set	CTXREG_CTX_MASK, %o1
	ldxa	[%o0]ASI_MMU_CTX, %o0
	retl
	and	%o0, %o1, %o0
	SET_SIZE(sfmmu_getctx_sec)

	/*
	 * Set the secondary context register for this process.
	 * %o0 = context number for this process.
	 */
	ENTRY_NP(sfmmu_setctx_sec)
	/*
	 * From resume we call sfmmu_setctx_sec with interrupts disabled.
	 * But we can also get called from C with interrupts enabled. So,
	 * we need to check first. Also, resume saves state in %o3 and %o5
	 * so we can't use those registers here.
	 */

	/* If interrupts are not disabled, then disable them */
	rdpr	%pstate, %g1
	btst	PSTATE_IE, %g1
	bnz,a,pt %icc, 1f
	wrpr	%g1, PSTATE_IE, %pstate		/* disable interrupts */
1:
	mov	MMU_SCONTEXT, %o1
	sethi	%hi(ctx_pgsz_array), %g2
	ldn	[%g2 + %lo(ctx_pgsz_array)], %g2
	brz	%g2, 2f
	nop
	ldub	[%g2 + %o0], %g2
	sll	%g2, CTXREG_EXT_SHIFT, %g2
	or	%g2, %o0, %o0
2:
	sethi	%hi(FLUSH_ADDR), %o4
	stxa	%o0, [%o1]ASI_MMU_CTX		/* set 2nd context reg. */
	flush	%o4

	btst	PSTATE_IE, %g1
	bnz,a,pt %icc, 1f
	wrpr	%g0, %g1, %pstate		/* enable interrupts */
1:	retl
	nop
	SET_SIZE(sfmmu_setctx_sec)

	/*
	 * set ktsb_phys to 1 if the processor supports ASI_QUAD_LDD_PHYS.
	 * returns the detection value in %o0.
	 */
	ENTRY_NP(sfmmu_setup_4lp)
	GET_CPU_IMPL(%o0);
	cmp	%o0, CHEETAH_PLUS_IMPL
	blt,a,pt %icc, 4f
	  clr	%o1
	set	ktsb_phys, %o2
	mov	1, %o1
	st	%o1, [%o2]
4:	retl
	mov	%o1, %o0
	SET_SIZE(sfmmu_setup_4lp)


	/*
	 * Called to load MMU registers and tsbmiss area
	 * for the active process.  This function should
	 * only be called from TL=0.
	 *
	 * %o0 - hat pointer
	 */
	ENTRY_NP(sfmmu_load_mmustate)
	/*
	 * From resume we call sfmmu_load_mmustate with interrupts disabled.
	 * But we can also get called from C with interrupts enabled. So,
	 * we need to check first. Also, resume saves state in %o5 and we
	 * can't use this register here.
	 */

	sethi	%hi(ksfmmup), %o3
	ldx	[%o3 + %lo(ksfmmup)], %o3
	cmp	%o3, %o0
	be,pn	%xcc, 3f			! if kernel as, do nothing
	  nop

	/* If interrupts are not disabled, then disable them */
	rdpr	%pstate, %g1
	btst	PSTATE_IE, %g1
	bnz,a,pt %icc, 1f
	wrpr	%g1, PSTATE_IE, %pstate		! disable interrupts
1:
	/*
	 * We need to set up the TSB base register, tsbmiss
	 * area, and load locked TTE(s) for the TSB.
	 */
	ldx	[%o0 + SFMMU_TSB], %o1		! %o1 = first tsbinfo
	ldx	[%o1 + TSBINFO_NEXTPTR], %g2	! %g2 = second tsbinfo
	brz,pt	%g2, 4f
	  nop
	/*
	 * We have a second TSB for this process, so we need to 
	 * encode data for both the first and second TSB in our single
	 * TSB base register.  See hat_sfmmu.h for details on what bits
	 * correspond to which TSB.
	 * We also need to load a locked TTE into the TLB for the second TSB
	 * in this case.
	 */
	MAKE_TSBREG_SECTSB(%o2, %o1, %g2, %o3, %o4, %g3, sfmmu_tsb_2nd)
	! %o2 = tsbreg
	sethi	%hi(utsb4m_dtlb_ttenum), %o3
	sethi	%hi(utsb4m_vabase), %o4
	ld	[%o3 + %lo(utsb4m_dtlb_ttenum)], %o3
	ldx	[%o4 + %lo(utsb4m_vabase)], %o4	! %o4 = TLB tag for sec TSB
	sll	%o3, DTACC_SHIFT, %o3		! %o3 = sec TSB TLB index
	RESV_OFFSET(%g2, %o4, %g3, sfmmu_tsb_2nd)	! or-in bits of TSB VA
	LOAD_TSBTTE(%g2, %o3, %o4, %g3)		! load sec TSB locked TTE
	sethi	%hi(utsb_vabase), %g3
	ldx	[%g3 + %lo(utsb_vabase)], %g3	! %g3 = TLB tag for first TSB
	ba,pt	%xcc, 5f
	  nop

4:	sethi	%hi(utsb_vabase), %g3
	ldx	[%g3 + %lo(utsb_vabase)], %g3	! %g3 = TLB tag for first TSB
	MAKE_TSBREG(%o2, %o1, %g3, %o3, %o4, sfmmu_tsb_1st)	! %o2 = tsbreg

5:	LOAD_TSBREG(%o2, %o3, %o4)		! write TSB base register

	/*
	 * Load the TTE for the first TSB at the appropriate location in
	 * the TLB
	 */
	sethi	%hi(utsb_dtlb_ttenum), %o2
	ld	[%o2 + %lo(utsb_dtlb_ttenum)], %o2
	sll	%o2, DTACC_SHIFT, %o2		! %o1 = first TSB TLB index
	RESV_OFFSET(%o1, %g3, %o3, sfmmu_tsb_1st)	! or-in bits of TSB VA
	LOAD_TSBTTE(%o1, %o2, %g3, %o4)		! load first TSB locked TTE

6:	ldx	[%o0 + SFMMU_ISMBLKPA], %o1	! copy members of sfmmu
	CPU_TSBMISS_AREA(%o2, %o3)		! we need to access from
	stx	%o1, [%o2 + TSBMISS_ISMBLKPA]	! sfmmu_tsb_miss into the
	lduh	[%o0 + SFMMU_FLAGS], %o3	! per-CPU tsbmiss area.
	stx	%o0, [%o2 + TSBMISS_UHATID]
	stuh	%o3, [%o2 + TSBMISS_HATFLAGS]

	btst	PSTATE_IE, %g1
	bnz,a,pt %icc, 3f
	wrpr	%g0, %g1, %pstate		! enable interrupts
3:	retl
	nop
	SET_SIZE(sfmmu_load_mmustate)

#endif /* lint */

#if defined (lint)
/*
 * Invalidate all of the entries within the tsb, by setting the inv bit
 * in the tte_tag field of each tsbe.
 *
 * We take advantage of the fact TSBs are page aligned and a multiple of
 * PAGESIZE to use block stores.
 *
 * See TSB_LOCK_ENTRY and the miss handlers for how this works in practice
 * (in short, we set all bits in the upper word of the tag, and we give the
 * invalid bit precedence over other tag bits in both places).
 */
/* ARGSUSED */
void
sfmmu_inv_tsb_fast(caddr_t tsb_base, uint_t tsb_bytes)
{}

#else /* lint */

#define	VIS_BLOCKSIZE	64

	ENTRY(sfmmu_inv_tsb_fast)

	! Get space for aligned block of saved fp regs.
	save	%sp, -SA(MINFRAME + 2*VIS_BLOCKSIZE), %sp

	! kpreempt_disable();
	ldsb	[THREAD_REG + T_PREEMPT], %l3
	inc	%l3
	stb	%l3, [THREAD_REG + T_PREEMPT]

	! See if fpu was in use.  If it was, we need to save off the
	! floating point registers to the stack.
	rd	%fprs, %l0			! %l0 = cached copy of fprs
	btst	FPRS_FEF, %l0
	bz,pt	%icc, 4f
	  nop

	! save in-use fpregs on stack
	membar	#Sync				! make sure tranx to fp regs
						! have completed
	add	%fp, STACK_BIAS - 65, %l1	! get stack frame for fp regs
	and	%l1, -VIS_BLOCKSIZE, %l1	! block align frame
	stda	%d0, [%l1]ASI_BLK_P		! %l1 = addr of saved fp regs

	! enable fp
4:	membar	#StoreStore|#StoreLoad|#LoadStore
	wr	%g0, FPRS_FEF, %fprs
	wr	%g0, ASI_BLK_P, %asi

	! load up FP registers with invalid TSB tag.
	fone	%d0			! ones in tag
	fzero	%d2			! zeros in TTE
	fone	%d4			! ones in tag
	fzero	%d6			! zeros in TTE
	fone	%d8			! ones in tag
	fzero	%d10			! zeros in TTE
	fone	%d12			! ones in tag
	fzero	%d14			! zeros in TTE
	ba,pt	%xcc, .sfmmu_inv_doblock
	  mov	(4*VIS_BLOCKSIZE), %i4	! we do 4 stda's each loop below

.sfmmu_inv_blkstart:
      ! stda	%d0, [%i0+192]%asi  ! in dly slot of branch that got us here
	stda	%d0, [%i0+128]%asi
	stda	%d0, [%i0+64]%asi
	stda	%d0, [%i0]%asi

	add	%i0, %i4, %i0
	sub	%i1, %i4, %i1

.sfmmu_inv_doblock:
	cmp	%i1, (4*VIS_BLOCKSIZE)	! check for completion
	bgeu,a	%icc, .sfmmu_inv_blkstart
	  stda	%d0, [%i0+192]%asi

.sfmmu_inv_finish:
	membar	#Sync
	btst	FPRS_FEF, %l0		! saved from above
	bz,a	.sfmmu_inv_finished
	  wr	%l0, 0, %fprs		! restore fprs

	! restore fpregs from stack
	ldda    [%l1]ASI_BLK_P, %d0
	membar	#Sync
	wr	%l0, 0, %fprs		! restore fprs

.sfmmu_inv_finished:
	! kpreempt_enable();
	ldsb	[THREAD_REG + T_PREEMPT], %l3
	dec	%l3
	stb	%l3, [THREAD_REG + T_PREEMPT]
	ret
	restore
	SET_SIZE(sfmmu_inv_tsb_fast)

#endif /* lint */

#if defined(lint)

/*
 * Prefetch "struct tsbe" while walking TSBs.
 * prefetch 7 cache lines ahead of where we are at now.
 * #n_reads is being used since #one_read only applies to
 * floating point reads, and we are not doing floating point
 * reads.  However, this has the negative side effect of polluting
 * the ecache.
 * The 448 comes from (7 * 64) which is how far ahead of our current
 * address, we want to prefetch.
 */
/*ARGSUSED*/
void
prefetch_tsbe_read(struct tsbe *tsbep)
{}

/* Prefetch the tsbe that we are about to write */
/*ARGSUSED*/
void
prefetch_tsbe_write(struct tsbe *tsbep)
{}

#else /* lint */

	ENTRY(prefetch_tsbe_read)
	retl
	prefetch	[%o0+448], #n_reads
	SET_SIZE(prefetch_tsbe_read)

	ENTRY(prefetch_tsbe_write)
	retl
	prefetch	[%o0], #n_writes
	SET_SIZE(prefetch_tsbe_write)
#endif /* lint */


#ifndef lint
#endif	/* lint */

