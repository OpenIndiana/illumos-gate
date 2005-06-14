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

#include "lint.h"
#include "thr_uberdata.h"

const char *panicstr;
ulwp_t *panic_thread;

static mutex_t assert_lock = DEFAULTMUTEX;
static ulwp_t *assert_thread = NULL;

/*
 * Called from __assert() to set panicstr and panic_thread.
 */
void
__set_panicstr(const char *msg)
{
	panicstr = msg;
	panic_thread = __curthread();
}

/*
 * Called from exit() (atexit function) to give precedence
 * to assertion failures and a core dump over _exit().
 */
void
grab_assert_lock()
{
	(void) _private_lwp_mutex_lock(&assert_lock);
}

static void
Abort(const char *msg)
{
	ulwp_t *self;
	struct sigaction act;
	sigset_t sigmask;
	lwpid_t lwpid;

	/* to help with core file debugging */
	panicstr = msg;
	if ((self = __curthread()) != NULL) {
		panic_thread = self;
		lwpid = self->ul_lwpid;
	} else {
		lwpid = __lwp_self();
	}

	/* set SIGABRT signal handler to SIG_DFL w/o grabbing any locks */
	(void) _private_memset(&act, 0, sizeof (act));
	act.sa_sigaction = SIG_DFL;
	(void) __sigaction(SIGABRT, &act, NULL);

	/* delete SIGABRT from the signal mask */
	(void) _private_sigemptyset(&sigmask);
	(void) _private_sigaddset(&sigmask, SIGABRT);
	(void) __lwp_sigmask(SIG_UNBLOCK, &sigmask, NULL);

	(void) __lwp_kill(lwpid, SIGABRT);	/* never returns */
	(void) _kill(_private_getpid(), SIGABRT); /* if it does, try harder */
	_exit(127);
}

/*
 * Write a panic message w/o grabbing any locks other than assert_lock.
 * We have no idea what locks are held at this point.
 */
void
thr_panic(const char *why)
{
	char msg[400];	/* no panic() message in the library is this long */
	ulwp_t *self;
	size_t len1, len2;

	if ((self = __curthread()) != NULL)
		enter_critical(self);
	(void) _private_lwp_mutex_lock(&assert_lock);

	(void) _private_memset(msg, 0, sizeof (msg));
	(void) strcpy(msg, "*** libc thread failure: ");
	len1 = strlen(msg);
	len2 = strlen(why);
	if (len1 + len2 >= sizeof (msg))
		len2 = sizeof (msg) - len1 - 1;
	(void) strncat(msg, why, len2);
	len1 = strlen(msg);
	if (msg[len1 - 1] != '\n')
		msg[len1++] = '\n';
	(void) _write(2, msg, len1);
	Abort(msg);
}

/*
 * Utility function for converting a long integer to a string, avoiding stdio.
 * 'base' must be one of 10 or 16
 */
void
ultos(uint64_t n, int base, char *s)
{
	char lbuf[24];		/* 64 bits fits in 16 hex digits, 20 decimal */
	char *cp = lbuf;

	do {
		*cp++ = "0123456789abcdef"[n%base];
		n /= base;
	} while (n);
	if (base == 16) {
		*s++ = '0';
		*s++ = 'x';
	}
	do {
		*s++ = *--cp;
	} while (cp > lbuf);
	*s = '\0';
}

/*
 * Report application lock usage error for mutexes and condvars.
 * Not called if _THREAD_ERROR_DETECTION=0.
 * Continue execution if _THREAD_ERROR_DETECTION=1.
 * Dump core if _THREAD_ERROR_DETECTION=2.
 */
void
lock_error(const mutex_t *mp, const char *who, void *cv, const char *msg)
{
	/* take a snapshot of the mutex before it changes (we hope!) */
	mutex_t mcopy = *mp;
	char buf[800];
	uberdata_t *udp;
	ulwp_t *self;
	lwpid_t lwpid;
	pid_t pid;

	/* avoid recursion deadlock */
	if ((self = __curthread()) != NULL) {
		if (assert_thread == self)
			_exit(127);
		enter_critical(self);
		(void) _private_lwp_mutex_lock(&assert_lock);
		assert_thread = self;
		lwpid = self->ul_lwpid;
		udp = self->ul_uberdata;
		pid = udp->pid;
	} else {
		self = NULL;
		(void) _private_lwp_mutex_lock(&assert_lock);
		lwpid = __lwp_self();
		udp = &__uberdata;
		pid = _private_getpid();
	}

	(void) strcpy(buf,
	    "\n*** _THREAD_ERROR_DETECTION: lock usage error detected ***\n");
	(void) strcat(buf, who);
	(void) strcat(buf, "(");
	if (cv != NULL) {
		ultos((uint64_t)(uintptr_t)cv, 16, buf + strlen(buf));
		(void) strcat(buf, ", ");
	}
	ultos((uint64_t)(uintptr_t)mp, 16, buf + strlen(buf));
	(void) strcat(buf, ")");
	if (msg != NULL) {
		(void) strcat(buf, ": ");
		(void) strcat(buf, msg);
	} else if (!mutex_is_held(&mcopy)) {
		(void) strcat(buf, ": calling thread does not own the lock");
	} else if (mcopy.mutex_rcount) {
		(void) strcat(buf, ": mutex rcount = ");
		ultos((uint64_t)mcopy.mutex_rcount, 10, buf + strlen(buf));
	} else {
		(void) strcat(buf, ": calling thread already owns the lock");
	}
	(void) strcat(buf, "\ncalling thread is ");
	ultos((uint64_t)(uintptr_t)self, 16, buf + strlen(buf));
	(void) strcat(buf, " thread-id ");
	ultos((uint64_t)lwpid, 10, buf + strlen(buf));
	if (msg != NULL || mutex_is_held(&mcopy))
		/* EMPTY */;
	else if (mcopy.mutex_lockw == 0)
		(void) strcat(buf, "\nthe lock is unowned");
	else if (!(mcopy.mutex_type & (USYNC_PROCESS|USYNC_PROCESS_ROBUST))) {
		(void) strcat(buf, "\nthe lock owner is ");
		ultos((uint64_t)mcopy.mutex_owner, 16, buf + strlen(buf));
	} else {
		(void) strcat(buf, " in process ");
		ultos((uint64_t)pid, 10, buf + strlen(buf));
		(void) strcat(buf, "\nthe lock owner is ");
		ultos((uint64_t)mcopy.mutex_owner, 16, buf + strlen(buf));
		(void) strcat(buf, " in process ");
		ultos((uint64_t)mcopy.mutex_ownerpid, 10, buf + strlen(buf));
	}
	(void) strcat(buf, "\n\n");
	(void) _write(2, buf, strlen(buf));
	if (udp->uberflags.uf_thread_error_detection >= 2)
		Abort(buf);
	assert_thread = NULL;
	(void) _private_lwp_mutex_unlock(&assert_lock);
	if (self != NULL)
		exit_critical(self);
}

/*
 * Report application lock usage error for rwlocks.
 * Not called if _THREAD_ERROR_DETECTION=0.
 * Continue execution if _THREAD_ERROR_DETECTION=1.
 * Dump core if _THREAD_ERROR_DETECTION=2.
 */
void
rwlock_error(const rwlock_t *rp, const char *who, const char *msg)
{
	/* take a snapshot of the rwlock before it changes (we hope) */
	rwlock_t rcopy = *rp;
	char buf[800];
	uberdata_t *udp;
	ulwp_t *self;
	lwpid_t lwpid;
	pid_t pid;

	/* avoid recursion deadlock */
	if ((self = __curthread()) != NULL) {
		if (assert_thread == self)
			_exit(127);
		enter_critical(self);
		(void) _private_lwp_mutex_lock(&assert_lock);
		assert_thread = self;
		lwpid = self->ul_lwpid;
		udp = self->ul_uberdata;
		pid = udp->pid;
	} else {
		self = NULL;
		(void) _private_lwp_mutex_lock(&assert_lock);
		lwpid = __lwp_self();
		udp = &__uberdata;
		pid = _private_getpid();
	}

	(void) strcpy(buf,
	    "\n*** _THREAD_ERROR_DETECTION: lock usage error detected ***\n");
	(void) strcat(buf, who);
	(void) strcat(buf, "(");
	ultos((uint64_t)(uintptr_t)rp, 16, buf + strlen(buf));
	(void) strcat(buf, "): ");
	(void) strcat(buf, msg);
	(void) strcat(buf, "\ncalling thread is ");
	ultos((uint64_t)(uintptr_t)self, 16, buf + strlen(buf));
	(void) strcat(buf, " thread-id ");
	ultos((uint64_t)lwpid, 10, buf + strlen(buf));
	if (rcopy.rwlock_type & USYNC_PROCESS) {
		uint32_t *rwstate = (uint32_t *)&rcopy.rwlock_readers;

		(void) strcat(buf, " in process ");
		ultos((uint64_t)pid, 10, buf + strlen(buf));

		if (*rwstate & URW_WRITE_LOCKED) {
			(void) strcat(buf, "\nthe lock writer owner is ");
			ultos((uint64_t)rcopy.rwlock_owner, 16,
			    buf + strlen(buf));
			(void) strcat(buf, " in process ");
			ultos((uint64_t)rcopy.rwlock_ownerpid, 10,
			    buf + strlen(buf));
		} else if (*rwstate & URW_READERS_MASK) {
			(void) strcat(buf, "\nthe lock is owned by ");
			ultos((uint64_t)(*rwstate & URW_READERS_MASK), 10,
			    buf + strlen(buf));
			(void) strcat(buf, " readers");
		} else
			(void) strcat(buf, "\nthe lock is unowned");

		if (*rwstate & URW_HAS_WAITERS) {
			(void) strcat(buf, "\nand appears to have waiters");
			if (*rwstate & URW_WRITE_WANTED)
				(void) strcat(buf,
				    " (including at least one writer)");
		}
	} else if (rcopy.rwlock_readers < 0) {
		(void) strcat(buf, "\nthe lock writer owner is ");
		ultos((uint64_t)rcopy.rwlock_mowner, 16, buf + strlen(buf));
	} else if (rcopy.rwlock_readers > 0) {
		(void) strcat(buf, "\nthe lock is owned by ");
		ultos((uint64_t)rcopy.rwlock_readers, 10, buf + strlen(buf));
		(void) strcat(buf, "readers");
	} else {
		(void) strcat(buf, "\nthe lock is unowned");
	}
	(void) strcat(buf, "\n\n");
	(void) _write(2, buf, strlen(buf));
	if (udp->uberflags.uf_thread_error_detection >= 2)
		Abort(buf);
	assert_thread = NULL;
	(void) _private_lwp_mutex_unlock(&assert_lock);
	if (self != NULL)
		exit_critical(self);
}

/*
 * Report a thread usage error.
 * Not called if _THREAD_ERROR_DETECTION=0.
 * Writes message and continues execution if _THREAD_ERROR_DETECTION=1.
 * Writes message and dumps core if _THREAD_ERROR_DETECTION=2.
 */
void
thread_error(const char *msg)
{
	char buf[800];
	uberdata_t *udp;
	ulwp_t *self;
	lwpid_t lwpid;

	/* avoid recursion deadlock */
	if ((self = __curthread()) != NULL) {
		if (assert_thread == self)
			_exit(127);
		enter_critical(self);
		(void) _private_lwp_mutex_lock(&assert_lock);
		assert_thread = self;
		lwpid = self->ul_lwpid;
		udp = self->ul_uberdata;
	} else {
		self = NULL;
		(void) _private_lwp_mutex_lock(&assert_lock);
		lwpid = __lwp_self();
		udp = &__uberdata;
	}

	(void) strcpy(buf, "\n*** _THREAD_ERROR_DETECTION: "
		"thread usage error detected ***\n*** ");
	(void) strcat(buf, msg);

	(void) strcat(buf, "\n*** calling thread is ");
	ultos((uint64_t)(uintptr_t)self, 16, buf + strlen(buf));
	(void) strcat(buf, " thread-id ");
	ultos((uint64_t)lwpid, 10, buf + strlen(buf));
	(void) strcat(buf, "\n\n");
	(void) _write(2, buf, strlen(buf));
	if (udp->uberflags.uf_thread_error_detection >= 2)
		Abort(buf);
	assert_thread = NULL;
	(void) _private_lwp_mutex_unlock(&assert_lock);
	if (self != NULL)
		exit_critical(self);
}

/*
 * We use __assfail() because the libc __assert() calls
 * gettext() which calls malloc() which grabs a mutex.
 * We do everything without calling standard i/o.
 * _assfail() is an exported function, __assfail() is private to libc.
 */
#pragma weak _assfail = __assfail
void
__assfail(const char *assertion, const char *filename, int line_num)
{
	char buf[800];	/* no assert() message in the library is this long */
	ulwp_t *self;
	lwpid_t lwpid;

	/* avoid recursion deadlock */
	if ((self = __curthread()) != NULL) {
		if (assert_thread == self)
			_exit(127);
		enter_critical(self);
		(void) _private_lwp_mutex_lock(&assert_lock);
		assert_thread = self;
		lwpid = self->ul_lwpid;
	} else {
		self = NULL;
		(void) _private_lwp_mutex_lock(&assert_lock);
		lwpid = __lwp_self();
	}

	(void) strcpy(buf, "assertion failed for thread ");
	ultos((uint64_t)(uintptr_t)self, 16, buf + strlen(buf));
	(void) strcat(buf, ", thread-id ");
	ultos((uint64_t)lwpid, 10, buf + strlen(buf));
	(void) strcat(buf, ": ");
	(void) strcat(buf, assertion);
	(void) strcat(buf, ", file ");
	(void) strcat(buf, filename);
	(void) strcat(buf, ", line ");
	ultos((uint64_t)line_num, 10, buf + strlen(buf));
	(void) strcat(buf, "\n");
	(void) _write(2, buf, strlen(buf));
	/*
	 * We could replace the call to Abort() with the following code
	 * if we want just to issue a warning message and not die.
	 *	assert_thread = NULL;
	 *	_private_lwp_mutex_unlock(&assert_lock);
	 *	if (self != NULL)
	 *		exit_critical(self);
	 */
	Abort(buf);
}
