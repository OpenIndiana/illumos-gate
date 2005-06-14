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

#include <sys/param.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/vfs.h>
#include <sys/vnode.h>
#include <sys/swap.h>
#include <sys/file.h>
#include <sys/proc.h>
#include <sys/var.h>
#include <sys/uadmin.h>
#include <sys/signal.h>
#include <sys/time.h>
#include <vm/seg_kmem.h>
#include <sys/modctl.h>
#include <sys/callb.h>
#include <sys/dumphdr.h>
#include <sys/debug.h>
#include <sys/ftrace.h>
#include <sys/cmn_err.h>
#include <sys/panic.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/policy.h>
#include <sys/zone.h>

/*
 * Administrivia system call.  We provide this in two flavors: one for calling
 * from the system call path (uadmin), and the other for calling from elsewhere
 * within the kernel (kadmin).  Callers must beware that certain uadmin cmd
 * values (specifically A_SWAPCTL) are only supported by uadmin and not kadmin.
 */

extern ksema_t fsflush_sema;
kmutex_t ualock;


/*
 * Kill all user processes in said zone.  A special argument of ALL_ZONES is
 * passed in when the system as a whole is shutting down.  The lack of per-zone
 * process lists is likely to make the following a performance bottleneck on a
 * system with many zones.
 */
void
killall(zoneid_t zoneid)
{
	proc_t *p;

	ASSERT(zoneid != GLOBAL_ZONEID);
	/*
	 * Kill all processes except kernel daemons and ourself.
	 * Make a first pass to stop all processes so they won't
	 * be trying to restart children as we kill them.
	 */
	mutex_enter(&pidlock);
	for (p = practive; p != NULL; p = p->p_next) {
		if ((zoneid == ALL_ZONES || p->p_zone->zone_id == zoneid) &&
		    p->p_exec != NULLVP &&	/* kernel daemons */
		    p->p_as != &kas &&
		    p->p_stat != SZOMB) {
			mutex_enter(&p->p_lock);
			p->p_flag |= SNOWAIT;
			sigtoproc(p, NULL, SIGSTOP);
			mutex_exit(&p->p_lock);
		}
	}
	p = practive;
	while (p != NULL) {
		if ((zoneid == ALL_ZONES || p->p_zone->zone_id == zoneid) &&
		    p->p_exec != NULLVP &&	/* kernel daemons */
		    p->p_as != &kas &&
		    p->p_stat != SIDL &&
		    p->p_stat != SZOMB) {
			mutex_enter(&p->p_lock);
			if (sigismember(&p->p_sig, SIGKILL)) {
				mutex_exit(&p->p_lock);
				p = p->p_next;
			} else {
				sigtoproc(p, NULL, SIGKILL);
				mutex_exit(&p->p_lock);
				(void) cv_timedwait(&p->p_srwchan_cv,
					    &pidlock, lbolt + hz);
				p = practive;
			}
		} else {
			p = p->p_next;
		}
	}
	mutex_exit(&pidlock);
}

int
kadmin(int cmd, int fcn, void *mdep, cred_t *credp)
{
	int error = 0;
	int locked = 0;
	char *buf;
	size_t buflen = 0;

	/*
	 * We might be called directly by the kernel's fault-handling code, so
	 * we can't assert that the caller is in the global zone.
	 */

	/*
	 * Make sure that cmd is one of the valid <sys/uadmin.h> command codes
	 * and that we have appropriate privileges for this action.
	 */
	switch (cmd) {
	case A_FTRACE:
	case A_SHUTDOWN:
	case A_REBOOT:
	case A_REMOUNT:
	case A_FREEZE:
	case A_DUMP:
		if (secpolicy_sys_config(credp, B_FALSE) != 0)
			return (EPERM);
		break;

	default:
		return (EINVAL);
	}

	/*
	 * Serialize these operations on ualock.  If it is held, just return
	 * as if successful since the system will soon reset or remount.
	 */
	if (cmd == A_SHUTDOWN || cmd == A_REBOOT || cmd == A_REMOUNT) {
		if (!mutex_tryenter(&ualock))
			return (0);
		locked = 1;
	}

	switch (cmd) {
	case A_SHUTDOWN:
	{
		proc_t *p = ttoproc(curthread);

		/*
		 * Release (almost) all of our own resources if we are called
		 * from a user context, however if we are calling kadmin() from
		 * a kernel context then we do not release these resources.
		 */
		if (ttoproc(curthread) != &p0) {
			if ((error = exitlwps(0)) != 0)
				return (error);
			mutex_enter(&p->p_lock);
			p->p_flag |= SNOWAIT;
			sigfillset(&p->p_ignore);
			curthread->t_lwp->lwp_cursig = 0;
			curthread->t_lwp->lwp_extsig = 0;
			if (p->p_exec) {
				vnode_t *exec_vp = p->p_exec;
				p->p_exec = NULLVP;
				mutex_exit(&p->p_lock);
				VN_RELE(exec_vp);
			} else {
				mutex_exit(&p->p_lock);
			}

			pollcleanup();
			closeall(P_FINFO(curproc));
			relvm();

		} else {
			/*
			 * Reset t_cred if not set because much of the
			 * filesystem code depends on CRED() being valid.
			 */
			if (curthread->t_cred == NULL)
				curthread->t_cred = kcred;
		}

		/*
		 * Communcate that init shouldn't be restarted.
		 */
		zone_shutdown_global();

		killall(ALL_ZONES);
		/*
		 * If we are calling kadmin() from a kernel context then we
		 * do not release these resources.
		 */
		if (ttoproc(curthread) != &p0) {
			VN_RELE(u.u_cdir);
			if (u.u_rdir)
				VN_RELE(u.u_rdir);
			if (u.u_cwd)
				refstr_rele(u.u_cwd);

			u.u_cdir = rootdir;
			u.u_rdir = NULL;
			u.u_cwd = NULL;
		}

		/*
		 * Allow the reboot/halt/poweroff code a chance to do
		 * anything it needs to whilst we still have filesystems
		 * mounted, like loading any modules necessary for later
		 * performing the actual poweroff.
		 */
		if ((mdep != NULL) && (*(char *)mdep == '/')) {
			buf = i_convert_boot_device_name(mdep, NULL, &buflen);
			mdpreboot(cmd, fcn, buf);
		} else
			mdpreboot(cmd, fcn, mdep);

		/*
		 * Allow fsflush to finish running and then prevent it
		 * from ever running again so that vfs_unmountall() and
		 * vfs_syncall() can acquire the vfs locks they need.
		 */
		sema_p(&fsflush_sema);
		(void) callb_execute_class(CB_CL_UADMIN_PRE_VFS, NULL);

		vfs_unmountall();
		(void) VFS_MOUNTROOT(rootvfs, ROOT_UNMOUNT);
		vfs_syncall();

		(void) callb_execute_class(CB_CL_UADMIN_POST_VFS, NULL);
		dump_ereports();
		dump_messages();

		/* FALLTHROUGH */
	}

	case A_REBOOT:
		if ((mdep != NULL) && (*(char *)mdep == '/')) {
			buf = i_convert_boot_device_name(mdep, NULL, &buflen);
			mdboot(cmd, fcn, buf);
		} else
			mdboot(cmd, fcn, mdep);
		/* no return expected */
		break;

	case A_REMOUNT:
		(void) VFS_MOUNTROOT(rootvfs, ROOT_REMOUNT);
		break;

	case A_FREEZE:
	{
		/* XXX: declare in some header file */
		extern int cpr(int);

		if (modload("misc", "cpr") == -1)
			return (ENOTSUP);
		error = cpr(fcn);
		break;
	}

	case A_FTRACE:
	{
		switch (fcn) {
		case AD_FTRACE_START:
			(void) FTRACE_START();
			break;
		case AD_FTRACE_STOP:
			(void) FTRACE_STOP();
			break;
		default:
			error = EINVAL;
		}
		break;
	}

	case A_DUMP:
	{
		if (fcn == AD_NOSYNC) {
			in_sync = 1;
			break;
		}

		panic_bootfcn = fcn;
		panic_forced = 1;

		if ((mdep != NULL) && (*(char *)mdep == '/')) {
			panic_bootstr = i_convert_boot_device_name(mdep,
					    NULL, &buflen);
		} else
			panic_bootstr = mdep;

		panic("forced crash dump initiated at user request");
		/*NOTREACHED*/
	}

	default:
		error = EINVAL;
	}

	if (locked)
		mutex_exit(&ualock);

	return (error);
}

int
uadmin(int cmd, int fcn, uintptr_t mdep)
{
	int error = 0, rv = 0;
	size_t nbytes = 0;
	char buf[257];
	cred_t *credp = CRED();

	/*
	 * The swapctl system call doesn't have its own entry point: it uses
	 * uadmin as a wrapper so we just call it directly from here.
	 */
	if (cmd == A_SWAPCTL) {
		if (get_udatamodel() == DATAMODEL_NATIVE)
			error = swapctl(fcn, (void *)mdep, &rv);
#if defined(_SYSCALL32_IMPL)
		else
			error = swapctl32(fcn, (void *)mdep, &rv);
#endif /* _SYSCALL32_IMPL */
		return (error ? set_errno(error) : rv);
	}

	/*
	 * Handle zones.
	 */
	if (getzoneid() != GLOBAL_ZONEID) {
		error = zone_uadmin(cmd, fcn, credp);
		return (error ? set_errno(error) : 0);
	}

	/*
	 * Certain subcommands intepret a non-NULL mdep value as a pointer to
	 * a boot string.  Attempt to copy it in now, or reset mdep to NULL.
	 */
	if (cmd == A_SHUTDOWN || cmd == A_REBOOT || cmd == A_DUMP) {
		if (mdep != NULL && copyinstr((const char *)mdep, buf,
		    sizeof (buf) - 1, &nbytes) == 0) {
			buf[nbytes] = '\0';
			mdep = (uintptr_t)buf;
		} else
			mdep = NULL;
	}

	if ((error = kadmin(cmd, fcn, (void *)mdep, credp)) != 0)
		return (set_errno(error));

	return (0);
}
