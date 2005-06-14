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

#include <strings.h>
#include <errno.h>
#include <cryptoutil.h>
#include <unistd.h> /* for pid_t */
#include <pthread.h>
#include <security/cryptoki.h>
#include "softGlobal.h"
#include "softSession.h"
#include "softObject.h"
#include "softKeystore.h"
#include "softKeystoreUtil.h"

#pragma fini(softtoken_fini)

static struct CK_FUNCTION_LIST functionList = {
	{ 2, 11 },	/* version */
	C_Initialize,
	C_Finalize,
	C_GetInfo,
	C_GetFunctionList,
	C_GetSlotList,
	C_GetSlotInfo,
	C_GetTokenInfo,
	C_GetMechanismList,
	C_GetMechanismInfo,
	C_InitToken,
	C_InitPIN,
	C_SetPIN,
	C_OpenSession,
	C_CloseSession,
	C_CloseAllSessions,
	C_GetSessionInfo,
	C_GetOperationState,
	C_SetOperationState,
	C_Login,
	C_Logout,
	C_CreateObject,
	C_CopyObject,
	C_DestroyObject,
	C_GetObjectSize,
	C_GetAttributeValue,
	C_SetAttributeValue,
	C_FindObjectsInit,
	C_FindObjects,
	C_FindObjectsFinal,
	C_EncryptInit,
	C_Encrypt,
	C_EncryptUpdate,
	C_EncryptFinal,
	C_DecryptInit,
	C_Decrypt,
	C_DecryptUpdate,
	C_DecryptFinal,
	C_DigestInit,
	C_Digest,
	C_DigestUpdate,
	C_DigestKey,
	C_DigestFinal,
	C_SignInit,
	C_Sign,
	C_SignUpdate,
	C_SignFinal,
	C_SignRecoverInit,
	C_SignRecover,
	C_VerifyInit,
	C_Verify,
	C_VerifyUpdate,
	C_VerifyFinal,
	C_VerifyRecoverInit,
	C_VerifyRecover,
	C_DigestEncryptUpdate,
	C_DecryptDigestUpdate,
	C_SignEncryptUpdate,
	C_DecryptVerifyUpdate,
	C_GenerateKey,
	C_GenerateKeyPair,
	C_WrapKey,
	C_UnwrapKey,
	C_DeriveKey,
	C_SeedRandom,
	C_GenerateRandom,
	C_GetFunctionStatus,
	C_CancelFunction,
	C_WaitForSlotEvent
};

boolean_t softtoken_initialized = B_FALSE;

static pid_t softtoken_pid = 0;

/* This mutex protects soft_session_list, all_sessions_closing */
pthread_mutex_t soft_sessionlist_mutex;
soft_session_t *soft_session_list = NULL;

int all_sessions_closing = 0;

int soft_urandom_fd = -1;
int soft_random_fd = -1;

slot_t soft_slot;
obj_to_be_freed_list_t obj_delay_freed;
ses_to_be_freed_list_t ses_delay_freed;

/* protects softtoken_initialized and access to C_Initialize/C_Finalize */
pthread_mutex_t soft_giant_mutex = PTHREAD_MUTEX_INITIALIZER;

static CK_RV finalize_common(CK_VOID_PTR pReserved);
static void softtoken_fini();

CK_RV
C_Initialize(CK_VOID_PTR pInitArgs)
{

	int initialize_pid;
	boolean_t supplied_ok;
	CK_RV rv;

	/*
	 * Get lock to insure only one thread enters this
	 * function at a time.
	 */
	(void) pthread_mutex_lock(&soft_giant_mutex);

	initialize_pid = getpid();

	if (softtoken_initialized) {
		if (initialize_pid == softtoken_pid) {
			/*
			 * This process has called C_Initialize already
			 */
			(void) pthread_mutex_unlock(&soft_giant_mutex);
			return (CKR_CRYPTOKI_ALREADY_INITIALIZED);
		} else {
			/*
			 * A fork has happened and the child is
			 * reinitializing.  Do a finalize_common to close
			 * out any state from the parent, and then
			 * continue on.
			 */
			(void) finalize_common(NULL);
		}
	}

	if (pInitArgs != NULL) {
		CK_C_INITIALIZE_ARGS *initargs1 =
		    (CK_C_INITIALIZE_ARGS *) pInitArgs;

		/* pReserved must be NULL */
		if (initargs1->pReserved != NULL) {
			(void) pthread_mutex_unlock(&soft_giant_mutex);
			return (CKR_ARGUMENTS_BAD);
		}

		/*
		 * ALL supplied function pointers need to have the value
		 * either NULL or non-NULL.
		 */
		supplied_ok = (initargs1->CreateMutex == NULL &&
		    initargs1->DestroyMutex == NULL &&
		    initargs1->LockMutex == NULL &&
		    initargs1->UnlockMutex == NULL) ||
		    (initargs1->CreateMutex != NULL &&
		    initargs1->DestroyMutex != NULL &&
		    initargs1->LockMutex != NULL &&
		    initargs1->UnlockMutex != NULL);

		if (!supplied_ok) {
			(void) pthread_mutex_unlock(&soft_giant_mutex);
			return (CKR_ARGUMENTS_BAD);
		}

		/*
		 * When the CKF_OS_LOCKING_OK flag isn't set and mutex
		 * function pointers are supplied by an application,
		 * return an error.  We must be able to use our own primitives.
		 */
		if (!(initargs1->flags & CKF_OS_LOCKING_OK) &&
		    (initargs1->CreateMutex != NULL)) {
			(void) pthread_mutex_unlock(&soft_giant_mutex);
			return (CKR_CANT_LOCK);
		}
	}

	/* Initialize the session list lock */
	if (pthread_mutex_init(&soft_sessionlist_mutex, NULL) != 0) {
		(void) pthread_mutex_unlock(&soft_giant_mutex);
		return (CKR_CANT_LOCK);
	}

	softtoken_initialized = B_TRUE;
	softtoken_pid = initialize_pid;

	/*
	 * token object related initialization
	 */
	soft_slot.authenticated = 0;
	soft_slot.userpin_change_needed = 0;
	soft_slot.token_object_list = NULL;

	if ((rv = soft_init_token_session()) != CKR_OK) {
		(void) pthread_mutex_unlock(&soft_giant_mutex);
		return (rv);
	}

	/* Initialize the slot lock */
	if (pthread_mutex_init(&soft_slot.slot_mutex, NULL) != 0) {
		(void) soft_destroy_token_session();
		(void) pthread_mutex_unlock(&soft_giant_mutex);
		return (CKR_CANT_LOCK);
	}

	/* Get keystore version because it might not be 1 at this time */
	if (soft_keystore_get_version(&soft_slot.ks_version, B_FALSE) !=
	    0) {
		soft_token_present = B_FALSE;
	}

	if (soft_token_present) {
		/* Load all the public token objects from keystore */
		if ((rv = soft_get_token_objects_from_keystore(
		    PUB_TOKENOBJS)) != CKR_OK) {
			(void) pthread_mutex_destroy(&soft_slot.slot_mutex);
			(void) soft_destroy_token_session();
			(void) pthread_mutex_unlock(&soft_giant_mutex);
			return (rv);
		} else {
			/*
			 * Invalidate public token objects until the
			 * C_OpenSession is called.
			 */
			soft_validate_token_objects(B_FALSE);
		}
	}

	(void) pthread_mutex_unlock(&soft_giant_mutex);

	/* Initialize the object_to_be_freed list */
	(void) pthread_mutex_init(&obj_delay_freed.obj_to_be_free_mutex, NULL);
	obj_delay_freed.count = 0;
	obj_delay_freed.first = NULL;
	obj_delay_freed.last = NULL;

	(void) pthread_mutex_init(&ses_delay_freed.ses_to_be_free_mutex, NULL);
	ses_delay_freed.count = 0;
	ses_delay_freed.first = NULL;
	ses_delay_freed.last = NULL;
	return (CKR_OK);

}

/*
 * C_Finalize is a wrapper around finalize_common. The
 * soft_giant_mutex should be locked by C_Finalize().
 */
CK_RV
C_Finalize(CK_VOID_PTR pReserved)
{

	CK_RV rv;

	(void) pthread_mutex_lock(&soft_giant_mutex);

	rv = finalize_common(pReserved);

	(void) pthread_mutex_unlock(&soft_giant_mutex);

	return (rv);

}

/*
 * finalize_common() does the work for C_Finalize.  soft_giant_mutex
 * must be held before calling this function.
 */
static CK_RV
finalize_common(CK_VOID_PTR pReserved) {

	CK_RV rv = CKR_OK;
	struct object *delay_free_obj, *tmpo;
	struct session *delay_free_ses, *tmps;

	if (!softtoken_initialized) {
		return (CKR_CRYPTOKI_NOT_INITIALIZED);
	}

	/* Check to see if pReseved is NULL */
	if (pReserved != NULL) {
		return (CKR_ARGUMENTS_BAD);
	}

	(void) pthread_mutex_lock(&soft_sessionlist_mutex);
	/*
	 * Set all_sessions_closing flag so any access to any
	 * existing sessions will be rejected.
	 */
	all_sessions_closing = 1;
	(void) pthread_mutex_unlock(&soft_sessionlist_mutex);

	/* Delete all the sessions and release the allocated resources */
	rv = soft_delete_all_sessions();

	(void) pthread_mutex_lock(&soft_sessionlist_mutex);
	/* Reset all_sessions_closing flag. */
	all_sessions_closing = 0;
	(void) pthread_mutex_unlock(&soft_sessionlist_mutex);

	softtoken_initialized = B_FALSE;
	softtoken_pid = 0;

	if (soft_urandom_fd > 0) {
		(void) close(soft_urandom_fd);
		soft_urandom_fd = -1;
	}

	if (soft_random_fd > 0) {
		(void) close(soft_random_fd);
		soft_random_fd = -1;
	}

	/* Destroy the session list lock here */
	(void) pthread_mutex_destroy(&soft_sessionlist_mutex);

	/*
	 * Destroy token object related stuffs
	 * 1. Clean up the token object list
	 * 2. Destroy slot mutex
	 * 3. Destroy mutex in token_session
	 */
	soft_delete_all_in_core_token_objects(ALL_TOKEN);
	(void) pthread_mutex_destroy(&soft_slot.slot_mutex);
	(void) soft_destroy_token_session();

	/*
	 * free all entries in the delay_freed list
	 */
	delay_free_obj = obj_delay_freed.first;
	while (delay_free_obj != NULL) {
		tmpo = delay_free_obj->next;
		free(delay_free_obj);
		delay_free_obj = tmpo;
	}
	(void) pthread_mutex_destroy(&obj_delay_freed.obj_to_be_free_mutex);

	delay_free_ses = ses_delay_freed.first;
	while (delay_free_ses != NULL) {
		tmps = delay_free_ses->next;
		free(delay_free_ses);
		delay_free_ses = tmps;
	}
	(void) pthread_mutex_destroy(&ses_delay_freed.ses_to_be_free_mutex);

	return (rv);
}

/*
 * softtoken_fini() function required to make sure complete cleanup
 * is done if softtoken is ever unloaded without a C_Finalize() call.
 */
static void
softtoken_fini()
{
	(void) pthread_mutex_lock(&soft_giant_mutex);

	/* if we're not initilized, do not attempt to finalize */
	if (!softtoken_initialized) {
		(void) pthread_mutex_unlock(&soft_giant_mutex);
		return;
	}

	(void) finalize_common(NULL_PTR);

	(void) pthread_mutex_unlock(&soft_giant_mutex);
}

CK_RV
C_GetInfo(CK_INFO_PTR pInfo)
{
	if (!softtoken_initialized)
		return (CKR_CRYPTOKI_NOT_INITIALIZED);

	if (pInfo == NULL) {
		return (CKR_ARGUMENTS_BAD);
	}

	/* Provide general information in the provided buffer */
	pInfo->cryptokiVersion.major = CRYPTOKI_VERSION_MAJOR;
	pInfo->cryptokiVersion.minor = CRYPTOKI_VERSION_MINOR;
	(void) strncpy((char *)pInfo->manufacturerID,
	    SOFT_MANUFACTURER_ID, 32);
	pInfo->flags = 0;
	(void) strncpy((char *)pInfo->libraryDescription,
	    LIBRARY_DESCRIPTION, 32);
	pInfo->libraryVersion.major = LIBRARY_VERSION_MAJOR;
	pInfo->libraryVersion.major = LIBRARY_VERSION_MINOR;

	return (CKR_OK);
}

CK_RV
C_GetFunctionList(CK_FUNCTION_LIST_PTR_PTR ppFunctionList)
{
	if (ppFunctionList == NULL) {
		return (CKR_ARGUMENTS_BAD);
	}

	*ppFunctionList = &functionList;

	return (CKR_OK);
}

/*
 * PKCS#11 states that C_GetFunctionStatus should always return
 * CKR_FUNCTION_NOT_PARALLEL
 */
/*ARGSUSED*/
CK_RV
C_GetFunctionStatus(CK_SESSION_HANDLE hSession)
{
	return (CKR_FUNCTION_NOT_PARALLEL);
}

/*
 * PKCS#11 states that C_CancelFunction should always return
 * CKR_FUNCTION_NOT_PARALLEL
 */
/*ARGSUSED*/
CK_RV
C_CancelFunction(CK_SESSION_HANDLE hSession)
{
	return (CKR_FUNCTION_NOT_PARALLEL);
}

/*
 * Perform a write that can handle EINTR.
 */
int
looping_write(int fd, void *buf, int len)
{
	char *p = buf;
	int cc, len2 = 0;

	if (len == 0)
		return (0);

	do {
		cc = write(fd, p, len);
		if (cc < 0) {
			if (errno == EINTR)
				continue;
			return (cc);
		} else if (cc == 0) {
			return (len2);
		} else {
			p += cc;
			len2 += cc;
			len -= cc;
		}
	} while (len > 0);
	return (len2);
}

/*
 * Perform a read that can handle EINTR.
 */
int
looping_read(int fd, void *buf, int len)
{
	char *p = buf;
	int cc, len2 = 0;

	do {
		cc = read(fd, p, len);
		if (cc < 0) {
			if (errno == EINTR)
				continue;
			return (cc);	/* errno is already set */
		} else if (cc == 0) {
			return (len2);
		} else {
			p += cc;
			len2 += cc;
			len -= cc;
		}
	} while (len > 0);
	return (len2);
}
