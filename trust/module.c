/*
 * Copyright (C) 2012 Red Hat Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above
 *       copyright notice, this list of conditions and the
 *       following disclaimer.
 *     * Redistributions in binary form must reproduce the
 *       above copyright notice, this list of conditions and
 *       the following disclaimer in the documentation and/or
 *       other materials provided with the distribution.
 *     * The names of contributors to this software may not be
 *       used to endorse or promote products derived from this
 *       software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * Author: Stef Walter <stefw@redhat.com>
 */

#include "config.h"

#include "attrs.h"
#define P11_DEBUG_FLAG P11_DEBUG_TRUST
#include "debug.h"
#include "dict.h"
#include "library.h"
#include "module.h"
#include "pkcs11.h"
#include "session.h"
#include "token.h"

#include <assert.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define MANUFACTURER_ID         "PKCS#11 Kit                     "
#define LIBRARY_DESCRIPTION     "PKCS#11 Kit Trust Module        "
#define SLOT_DESCRIPTION        "System Certificates, Trust Anchors, and Black Lists             "
#define TOKEN_LABEL             "System Trust Anchors and Policy "
#define TOKEN_MODEL             "PKCS#11 Kit      "
#define TOKEN_SERIAL_NUMBER     "1                "

/* Arbitrary non-zero and non-one choice */
#define SYSTEM_SLOT_ID   18UL

static struct _Shared {
	p11_dict *sessions;
	p11_token *token;
	char *anchor_paths;
	char *certificate_paths;
} gl = { NULL, NULL };

/* Used during FindObjects */
typedef struct _FindObjects {
	CK_ATTRIBUTE *match;
	CK_OBJECT_HANDLE *snapshot;
	CK_ULONG iterator;
} FindObjects;

static CK_FUNCTION_LIST sys_function_list;

static void
find_objects_free (void *data)
{
	FindObjects *find = data;
	p11_attrs_free (find->match);
	free (find->snapshot);
	free (find);
}

static CK_RV
lookup_session (CK_SESSION_HANDLE handle,
                p11_session **session)
{
	p11_session *sess;

	if (!gl.sessions)
		return CKR_CRYPTOKI_NOT_INITIALIZED;

	sess = p11_dict_get (gl.sessions, &handle);
	if (!sess)
		return CKR_SESSION_HANDLE_INVALID;

	if (sess && session)
		*session = sess;
	return CKR_OK;
}

static void
parse_argument (char *arg)
{
	char *value;

	value = arg + strcspn (arg, ":=");
	if (!*value)
		value = NULL;
	else
		*(value++) = 0;

	if (strcmp (arg, "anchors") == 0) {
		free (gl.anchor_paths);
		gl.anchor_paths = value ? strdup (value) : NULL;

	} else if (strcmp (arg, "certificates") == 0) {
		free (gl.certificate_paths);
		gl.certificate_paths = value ? strdup (value) : NULL;

	} else {
		p11_message ("unrecognized module argument: %s", arg);
	}
}

static void
parse_arguments (const char *string)
{
	char quote = '\0';
	char *src, *dup, *at, *arg;

	if (!string)
		return;

	src = dup = strdup (string);
	if (!dup) {
		p11_message ("couldn't allocate memory for argument string");
		return;
	}

	arg = at = src;
	for (src = dup; *src; src++) {

		/* Matching quote */
		if (quote == *src) {
			quote = '\0';

		/* Inside of quotes */
		} else if (quote != '\0') {
			if (*src == '\\') {
				*at++ = *src++;
				if (!*src) {
					p11_message ("couldn't parse argument string: %s", string);
					goto done;
				}
				if (*src != quote)
					*at++ = '\\';
			}
			*at++ = *src;

		/* Space, not inside of quotes */
		} else if (isspace(*src)) {
			*at = 0;
			parse_argument (arg);
			arg = at;

		/* Other character outside of quotes */
		} else {
			switch (*src) {
			case '\'':
			case '"':
				quote = *src;
				break;
			case '\\':
				*at++ = *src++;
				if (!*src) {
					p11_message ("couldn't parse argument string: %s", string);
					goto done;
				}
				/* fall through */
			default:
				*at++ = *src;
				break;
			}
		}
	}


	if (at != arg) {
		*at = 0;
		parse_argument (arg);
	}

done:
	free (dup);
}

static CK_RV
sys_C_Finalize (CK_VOID_PTR reserved)
{
	CK_RV rv = CKR_OK;

	p11_debug ("in");

	/* WARNING: This function must be reentrant */

	if (reserved) {
		rv = CKR_ARGUMENTS_BAD;

	} else {
		p11_lock ();

			if (!gl.sessions) {
				rv = CKR_CRYPTOKI_NOT_INITIALIZED;

			} else {
				free (gl.certificate_paths);
				free (gl.anchor_paths);
				gl.certificate_paths = gl.anchor_paths = NULL;

				p11_dict_free (gl.sessions);
				gl.sessions = NULL;

				p11_token_free (gl.token);
				gl.token = NULL;

				rv = CKR_OK;
			}

		p11_unlock ();
	}

	p11_debug ("out: 0x%lx", rv);
	return rv;
}

static CK_RV
sys_C_Initialize (CK_VOID_PTR init_args)
{
	CK_C_INITIALIZE_ARGS *args = NULL;
	int supplied_ok;
	CK_RV rv;

	p11_library_init_once ();

	/* WARNING: This function must be reentrant */

	p11_debug ("in");

	p11_lock ();

		rv = CKR_OK;

		/* pReserved must be NULL */
		args = init_args;

		/* ALL supplied function pointers need to have the value either NULL or non-NULL. */
		supplied_ok = (args->CreateMutex == NULL && args->DestroyMutex == NULL &&
		               args->LockMutex == NULL && args->UnlockMutex == NULL) ||
		              (args->CreateMutex != NULL && args->DestroyMutex != NULL &&
		               args->LockMutex != NULL && args->UnlockMutex != NULL);
		if (!supplied_ok) {
			p11_message ("invalid set of mutex calls supplied");
			rv = CKR_ARGUMENTS_BAD;
		}

		/*
		 * When the CKF_OS_LOCKING_OK flag isn't set return an error.
		 * We must be able to use our pthread functionality.
		 */
		if (!(args->flags & CKF_OS_LOCKING_OK)) {
			p11_message ("can't do without os locking");
			rv = CKR_CANT_LOCK;
		}

		/*
		 * We support setting the socket path and other arguments from from the
		 * pReserved pointer, similar to how NSS PKCS#11 components are initialized.
		 */
		if (rv == CKR_OK) {
			if (args->pReserved)
				parse_arguments ((const char*)args->pReserved);

			gl.sessions = p11_dict_new (p11_dict_ulongptr_hash,
			                            p11_dict_ulongptr_equal,
			                            NULL, p11_session_free);

			gl.token = p11_token_new (gl.anchor_paths ? gl.anchor_paths : SYSTEM_ANCHORS,
			                          gl.certificate_paths ? gl.certificate_paths : SYSTEM_CERTIFICATES);

			if (gl.sessions == NULL || gl.token == NULL) {
				warn_if_reached ();
				rv = CKR_GENERAL_ERROR;
			}
		}

	p11_unlock ();

	if (rv != CKR_OK)
		sys_C_Finalize (NULL);

	p11_debug ("out: 0x%lx", rv);
	return rv;
}

static CK_RV
sys_C_GetInfo (CK_INFO_PTR info)
{
	CK_RV rv = CKR_OK;

	p11_library_init_once ();

	p11_debug ("in");

	return_val_if_fail (info != NULL, CKR_ARGUMENTS_BAD);

	p11_lock ();

		if (!gl.sessions)
			rv = CKR_CRYPTOKI_NOT_INITIALIZED;

	p11_unlock ();

	if (rv == CKR_OK) {
		memset (info, 0, sizeof (*info));
		info->cryptokiVersion.major = CRYPTOKI_VERSION_MAJOR;
		info->cryptokiVersion.minor = CRYPTOKI_VERSION_MINOR;
		info->libraryVersion.major = PACKAGE_MAJOR;
		info->libraryVersion.minor = PACKAGE_MINOR;
		info->flags = 0;
		strncpy ((char*)info->manufacturerID, MANUFACTURER_ID, 32);
		strncpy ((char*)info->libraryDescription, LIBRARY_DESCRIPTION, 32);
	}

	p11_debug ("out: 0x%lx", rv);

	return rv;
}

static CK_RV
sys_C_GetFunctionList (CK_FUNCTION_LIST_PTR_PTR list)
{
	/* Can be called before C_Initialize */
	return_val_if_fail (list != NULL, CKR_ARGUMENTS_BAD);

	*list = &sys_function_list;
	return CKR_OK;
}

static CK_RV
sys_C_GetSlotList (CK_BBOOL token_present,
                   CK_SLOT_ID_PTR slot_list,
                   CK_ULONG_PTR count)
{
	CK_RV rv = CKR_OK;

	return_val_if_fail (count != NULL, CKR_ARGUMENTS_BAD);

	p11_debug ("in");

	p11_lock ();

		if (!gl.sessions)
			rv = CKR_CRYPTOKI_NOT_INITIALIZED;

	p11_unlock ();

	if (rv != CKR_OK) {
		/* already failed */

	} else if (!slot_list) {
		*count = 1;
		rv = CKR_OK;

	} else if (*count < 1) {
		*count = 1;
		rv = CKR_BUFFER_TOO_SMALL;

	} else {
		slot_list[0] = SYSTEM_SLOT_ID;
		*count = 1;
		rv = CKR_OK;
	}

	p11_debug ("out: 0x%lx", rv);

	return rv;
}

static CK_RV
sys_C_GetSlotInfo (CK_SLOT_ID id,
                   CK_SLOT_INFO_PTR info)
{
	CK_RV rv = CKR_OK;

	return_val_if_fail (id == SYSTEM_SLOT_ID, CKR_SLOT_ID_INVALID);
	return_val_if_fail (info != NULL, CKR_ARGUMENTS_BAD);

	p11_debug ("in");

	p11_lock ();

		if (!gl.sessions)
			rv = CKR_CRYPTOKI_NOT_INITIALIZED;

	p11_unlock ();

	if (rv == CKR_OK) {
		memset (info, 0, sizeof (*info));
		info->firmwareVersion.major = 0;
		info->firmwareVersion.minor = 0;
		info->hardwareVersion.major = 0;
		info->hardwareVersion.minor = 0;
		info->flags = CKF_TOKEN_PRESENT;
		strncpy ((char*)info->manufacturerID, MANUFACTURER_ID, 32);
		strncpy ((char*)info->slotDescription, SLOT_DESCRIPTION, 64);
	}

	p11_debug ("out: 0x%lx", rv);

	return rv;
}

static CK_RV
sys_C_GetTokenInfo (CK_SLOT_ID id,
                    CK_TOKEN_INFO_PTR info)
{
	CK_RV rv = CKR_OK;

	return_val_if_fail (id == SYSTEM_SLOT_ID, CKR_SLOT_ID_INVALID);
	return_val_if_fail (info != NULL, CKR_ARGUMENTS_BAD);

	p11_debug ("in");

	p11_lock ();

		if (!gl.sessions)
			rv = CKR_CRYPTOKI_NOT_INITIALIZED;

	p11_unlock ();

	if (rv == CKR_OK) {
		memset (info, 0, sizeof (*info));
		info->firmwareVersion.major = 0;
		info->firmwareVersion.minor = 0;
		info->hardwareVersion.major = 0;
		info->hardwareVersion.minor = 0;
		info->flags = CKF_TOKEN_INITIALIZED | CKF_WRITE_PROTECTED;
		strncpy ((char*)info->manufacturerID, MANUFACTURER_ID, 32);
		strncpy ((char*)info->label, TOKEN_LABEL, 32);
		strncpy ((char*)info->model, TOKEN_MODEL, 16);
		strncpy ((char*)info->serialNumber, TOKEN_SERIAL_NUMBER, 16);
		info->ulMaxSessionCount = CK_EFFECTIVELY_INFINITE;
		info->ulSessionCount = CK_UNAVAILABLE_INFORMATION;
		info->ulMaxRwSessionCount = 0;
		info->ulRwSessionCount = CK_UNAVAILABLE_INFORMATION;
		info->ulMaxPinLen = 0;
		info->ulMinPinLen = 0;
		info->ulTotalPublicMemory = CK_UNAVAILABLE_INFORMATION;
		info->ulFreePublicMemory = CK_UNAVAILABLE_INFORMATION;
		info->ulTotalPrivateMemory = CK_UNAVAILABLE_INFORMATION;
		info->ulFreePrivateMemory = CK_UNAVAILABLE_INFORMATION;
	}

	p11_debug ("out: 0x%lx", rv);

	return rv;
}

static CK_RV
sys_C_GetMechanismList (CK_SLOT_ID id,
                        CK_MECHANISM_TYPE_PTR mechanism_list,
                        CK_ULONG_PTR count)
{
	CK_RV rv = CKR_OK;

	return_val_if_fail (count != NULL, CKR_ARGUMENTS_BAD);

	p11_debug ("in");

	*count = 0;

	p11_debug ("out: 0x%lx", rv);
	return rv;
}

static CK_RV
sys_C_GetMechanismInfo (CK_SLOT_ID id,
                        CK_MECHANISM_TYPE type,
                        CK_MECHANISM_INFO_PTR info)
{
	return_val_if_fail (id == SYSTEM_SLOT_ID, CKR_SLOT_ID_INVALID);
	return_val_if_fail (info != NULL, CKR_ARGUMENTS_BAD);
	return_val_if_reached (CKR_MECHANISM_INVALID);
}

static CK_RV
sys_C_InitToken (CK_SLOT_ID id,
                 CK_UTF8CHAR_PTR pin,
                 CK_ULONG pin_len,
                 CK_UTF8CHAR_PTR label)
{
	return_val_if_fail (id == SYSTEM_SLOT_ID, CKR_SLOT_ID_INVALID);
	return_val_if_reached (CKR_TOKEN_WRITE_PROTECTED);
}

static CK_RV
sys_C_WaitForSlotEvent (CK_FLAGS flags,
                        CK_SLOT_ID_PTR slot,
                        CK_VOID_PTR reserved)
{
	p11_debug ("not supported");
	return CKR_FUNCTION_NOT_SUPPORTED;
}

static CK_RV
sys_C_OpenSession (CK_SLOT_ID id,
                   CK_FLAGS flags,
                   CK_VOID_PTR user_data,
                   CK_NOTIFY callback,
                   CK_SESSION_HANDLE_PTR handle)
{
	p11_session *session;
	CK_RV rv = CKR_OK;

	return_val_if_fail (id == SYSTEM_SLOT_ID, CKR_SLOT_ID_INVALID);
	return_val_if_fail (handle != NULL, CKR_ARGUMENTS_BAD);

	p11_debug ("in");

	p11_lock ();

		if (!gl.sessions) {
			rv = CKR_CRYPTOKI_NOT_INITIALIZED;

		} else if (!(flags & CKF_SERIAL_SESSION)) {
			rv = CKR_SESSION_PARALLEL_NOT_SUPPORTED;

		} else if (flags & CKF_RW_SESSION) {
			rv = CKR_TOKEN_WRITE_PROTECTED;

		} else {
			session = p11_session_new (gl.token);
			if (p11_dict_set (gl.sessions, &session->handle, session)) {
				rv = CKR_OK;
				*handle = session->handle;
				p11_debug ("session: %lu", *handle);
			} else {
				warn_if_reached ();
				rv = CKR_GENERAL_ERROR;
			}
		}

	p11_unlock ();

	p11_debug ("out: 0x%lx", rv);

	return rv;
}

static CK_RV
sys_C_CloseSession (CK_SESSION_HANDLE handle)
{
	CK_RV rv = CKR_OK;

	p11_debug ("in");

	p11_lock ();

		if (!gl.sessions) {
			rv = CKR_CRYPTOKI_NOT_INITIALIZED;

		} else if (p11_dict_remove (gl.sessions, &handle)) {
			rv = CKR_OK;

		} else {
			rv = CKR_SESSION_HANDLE_INVALID;
		}

	p11_unlock ();

	p11_debug ("out: 0x%lx", rv);

	return rv;
}

static CK_RV
sys_C_CloseAllSessions (CK_SLOT_ID id)
{
	CK_RV rv = CKR_OK;

	return_val_if_fail (id == SYSTEM_SLOT_ID, CKR_SLOT_ID_INVALID);

	p11_debug ("in");

	p11_lock ();

		if (!gl.sessions) {
			rv = CKR_CRYPTOKI_NOT_INITIALIZED;

		} else {
			p11_dict_clear (gl.sessions);
			rv = CKR_OK;
		}

	p11_unlock ();

	p11_debug ("out: 0x%lx", rv);

	return rv;
}

static CK_RV
sys_C_GetFunctionStatus (CK_SESSION_HANDLE handle)
{
	return CKR_SESSION_PARALLEL_NOT_SUPPORTED;
}

static CK_RV
sys_C_CancelFunction (CK_SESSION_HANDLE handle)
{
	return CKR_SESSION_PARALLEL_NOT_SUPPORTED;
}

static CK_RV
sys_C_GetSessionInfo (CK_SESSION_HANDLE handle,
                      CK_SESSION_INFO_PTR info)
{
	CK_RV rv;

	return_val_if_fail (info != NULL, CKR_ARGUMENTS_BAD);

	p11_debug ("in");

	p11_lock ();

		rv = lookup_session (handle, NULL);

	p11_unlock ();

	if (rv == CKR_OK) {
		info->flags = CKF_SERIAL_SESSION;
		info->slotID = SYSTEM_SLOT_ID;
		info->state = CKS_RO_PUBLIC_SESSION;
		info->ulDeviceError = 0;
	}

	p11_debug ("out: 0x%lx", rv);

	return rv;
}

static CK_RV
sys_C_InitPIN (CK_SESSION_HANDLE handle,
               CK_UTF8CHAR_PTR pin,
               CK_ULONG pin_len)
{
	return_val_if_reached (CKR_TOKEN_WRITE_PROTECTED);
}

static CK_RV
sys_C_SetPIN (CK_SESSION_HANDLE handle,
              CK_UTF8CHAR_PTR old_pin,
              CK_ULONG old_pin_len,
              CK_UTF8CHAR_PTR new_pin,
              CK_ULONG new_pin_len)
{
	return_val_if_reached (CKR_TOKEN_WRITE_PROTECTED);
}

static CK_RV
sys_C_GetOperationState (CK_SESSION_HANDLE handle,
                         CK_BYTE_PTR operation_state,
                         CK_ULONG_PTR operation_state_len)
{
	p11_debug ("not supported");
	return CKR_FUNCTION_NOT_SUPPORTED;
}

static CK_RV
sys_C_SetOperationState (CK_SESSION_HANDLE handle,
                         CK_BYTE_PTR operation_state,
                         CK_ULONG operation_state_len,
                         CK_OBJECT_HANDLE encryption_key,
                         CK_OBJECT_HANDLE authentication_key)
{
	p11_debug ("not supported");
	return CKR_FUNCTION_NOT_SUPPORTED;
}

static CK_RV
sys_C_Login (CK_SESSION_HANDLE handle,
             CK_USER_TYPE user_type,
             CK_UTF8CHAR_PTR pin,
             CK_ULONG pin_len)
{
	return_val_if_reached (CKR_FUNCTION_NOT_SUPPORTED);
}

static CK_RV
sys_C_Logout (CK_SESSION_HANDLE handle)
{
	return_val_if_reached (CKR_FUNCTION_NOT_SUPPORTED);
}

static CK_RV
sys_C_CreateObject (CK_SESSION_HANDLE handle,
                    CK_ATTRIBUTE_PTR template,
                    CK_ULONG count,
                    CK_OBJECT_HANDLE_PTR new_object)
{
	CK_ATTRIBUTE *attrs;
	p11_session *session;
	CK_BBOOL token;
	CK_RV rv;

	return_val_if_fail (new_object != NULL, CKR_ARGUMENTS_BAD);

	p11_debug ("in");

	p11_lock ();

		rv = lookup_session (handle, &session);
		if (rv == CKR_OK) {
			if (p11_attrs_findn_bool (template, count, CKA_TOKEN, &token) && token)
				rv = CKR_TOKEN_WRITE_PROTECTED;
		}

		if (rv == CKR_OK) {
			attrs = p11_attrs_buildn (NULL, template, count);
			rv = p11_session_add_object (session, attrs, new_object);
		}

	p11_unlock ();

	p11_debug ("out: 0x%lx", rv);

	return rv;
}

static CK_RV
sys_C_CopyObject (CK_SESSION_HANDLE handle,
                  CK_OBJECT_HANDLE object,
                  CK_ATTRIBUTE_PTR template,
                  CK_ULONG count,
                  CK_OBJECT_HANDLE_PTR new_object)
{
	CK_BBOOL vfalse = CK_FALSE;
	CK_ATTRIBUTE token = { CKA_TOKEN, &vfalse, sizeof (vfalse) };
	p11_session *session;
	CK_ATTRIBUTE *original;
	CK_ATTRIBUTE *attrs;
	CK_BBOOL val;
	CK_RV rv;

	return_val_if_fail (new_object != NULL, CKR_ARGUMENTS_BAD);

	p11_debug ("in");

	p11_lock ();

		rv = lookup_session (handle, &session);
		if (rv == CKR_OK) {
			original = p11_session_get_object (session, object, NULL);
			if (original == NULL)
				rv = CKR_OBJECT_HANDLE_INVALID;
		}

		if (rv == CKR_OK) {
			if (p11_attrs_findn_bool (template, count, CKA_TOKEN, &val) && val)
				rv = CKR_TOKEN_WRITE_PROTECTED;
		}

		if (rv == CKR_OK) {
			attrs = p11_attrs_dup (original);
			attrs = p11_attrs_buildn (attrs, template, count);
			attrs = p11_attrs_build (attrs, &token, NULL);
			rv = p11_session_add_object (session, attrs, new_object);
		}

	p11_unlock ();

	p11_debug ("out: 0x%lx", rv);

	return rv;
}

static CK_RV
sys_C_DestroyObject (CK_SESSION_HANDLE handle,
                     CK_OBJECT_HANDLE object)
{
	p11_session *session;
	CK_RV rv;

	p11_debug ("in");

	p11_lock ();

		rv = lookup_session (handle, &session);
		if (rv == CKR_OK)
			rv = p11_session_del_object (session, object);

	p11_unlock ();

	p11_debug ("out: 0x%lx", rv);

	return rv;
}

static CK_RV
sys_C_GetObjectSize (CK_SESSION_HANDLE handle,
                     CK_OBJECT_HANDLE object,
                     CK_ULONG_PTR size)
{
	p11_session *session;
	CK_RV rv;

	return_val_if_fail (size != NULL, CKR_ARGUMENTS_BAD);

	p11_debug ("in");

	p11_lock ();

		rv = lookup_session (handle, &session);
		if (rv == CKR_OK) {
			if (p11_session_get_object (session, object, NULL)) {
				*size = CK_UNAVAILABLE_INFORMATION;
				rv = CKR_OK;
			} else {
				rv = CKR_OBJECT_HANDLE_INVALID;
			}
		}

	p11_unlock ();

	p11_debug ("out: 0x%lx", rv);

	return rv;
}

static CK_RV
sys_C_GetAttributeValue (CK_SESSION_HANDLE handle,
                         CK_OBJECT_HANDLE object,
                         CK_ATTRIBUTE_PTR template,
                         CK_ULONG count)
{
	CK_ATTRIBUTE *attrs;
	CK_ATTRIBUTE *result;
	CK_ATTRIBUTE *attr;
	p11_session *session;
	CK_ULONG i;
	CK_RV rv;

	p11_debug ("in");

	p11_lock ();

		rv = lookup_session (handle, &session);
		if (rv == CKR_OK) {
			attrs = p11_session_get_object (session, object, NULL);
			if (attrs == NULL)
				rv = CKR_OBJECT_HANDLE_INVALID;
		}

		if (rv == CKR_OK) {
			for (i = 0; i < count; i++) {
				result = template + i;
				attr = p11_attrs_find (attrs, result->type);
				if (!attr) {
					result->ulValueLen = (CK_ULONG)-1;
					rv = CKR_ATTRIBUTE_TYPE_INVALID;
					continue;
				}

				if (!result->pValue) {
					result->ulValueLen = attr->ulValueLen;
					continue;
				}

				if (result->ulValueLen >= attr->ulValueLen) {
					memcpy (result->pValue, attr->pValue, attr->ulValueLen);
					result->ulValueLen = attr->ulValueLen;
					continue;
				}

				result->ulValueLen = (CK_ULONG)-1;
				rv = CKR_BUFFER_TOO_SMALL;
			}
		}

	p11_unlock ();

	p11_debug ("out: 0x%lx", rv);

	return rv;
}

static CK_RV
sys_C_SetAttributeValue (CK_SESSION_HANDLE handle,
                         CK_OBJECT_HANDLE object,
                         CK_ATTRIBUTE_PTR template,
                         CK_ULONG count)
{
	p11_session *session;
	CK_RV rv;

	p11_debug ("in");

	p11_lock ();

		rv = lookup_session (handle, &session);
		if (rv == CKR_OK)
			rv = p11_session_set_object (session, object, template, count);

	p11_unlock ();

	p11_debug ("out: 0x%lx", rv);

	return rv;
}

static CK_RV
sys_C_FindObjectsInit (CK_SESSION_HANDLE handle,
                       CK_ATTRIBUTE_PTR template,
                       CK_ULONG count)
{
	CK_OBJECT_HANDLE *handle_ptr;
	CK_BBOOL want_token_objects;
	CK_BBOOL want_session_objects;
	CK_BBOOL token;
	p11_dict *objects;
	FindObjects *find;
	p11_session *session;
	p11_dictiter iter;
	CK_ULONG i;
	CK_RV rv;

	p11_debug ("in");

	p11_lock ();

		/* Are we searching for token objects? */
		if (p11_attrs_findn_bool (template, count, CKA_TOKEN, &token)) {
			want_token_objects = token;
			want_session_objects = !token;
		} else {
			want_token_objects = CK_TRUE;
			want_session_objects = CK_TRUE;
		}

		rv = lookup_session (handle, &session);

		/* Refresh from disk if this session hasn't yet */
		if (rv == CKR_OK && want_token_objects && !session->loaded) {
			session->loaded = CK_TRUE;
			p11_token_load (gl.token);
		}

		if (rv == CKR_OK) {
			objects = p11_token_objects (gl.token);

			find = calloc (1, sizeof (FindObjects));
			warn_if_fail (find != NULL);

			/* Make a copy of what we're matching */
			if (find) {
				find->match = p11_attrs_buildn (NULL, template, count);
				warn_if_fail (find->match != NULL);

				/* Build a session snapshot of all objects */
				find->iterator = 0;
				count = p11_dict_size (objects) + p11_dict_size (session->objects) + 1;
				find->snapshot = calloc (count, sizeof (CK_OBJECT_HANDLE));
				warn_if_fail (find->snapshot != NULL);
			}

			if (find && find->snapshot) {
				i = 0;

				if (want_token_objects) {
					p11_dict_iterate (objects, &iter);
					for ( ; p11_dict_next (&iter, (void *)&handle_ptr, NULL); i++) {
						assert (i < count);
						find->snapshot[i] = *handle_ptr;
					}
				}

				if (want_session_objects) {
					p11_dict_iterate (session->objects, &iter);
					for ( ; p11_dict_next (&iter, (void *)&handle_ptr, NULL); i++) {
						assert (i < count);
						find->snapshot[i] = *handle_ptr;
					}
				}

				assert (i < count);
				assert (find->snapshot[i] == 0UL);
			}

			if (!find || !find->snapshot || !find->match)
				rv = CKR_HOST_MEMORY;
			else
				p11_session_set_operation (session, find_objects_free, find);
		}

	p11_unlock ();

	p11_debug ("out: 0x%lx", rv);

	return rv;
}

static CK_RV
sys_C_FindObjects (CK_SESSION_HANDLE handle,
                   CK_OBJECT_HANDLE_PTR objects,
                   CK_ULONG max_count,
                   CK_ULONG_PTR count)
{
	CK_OBJECT_HANDLE object;
	CK_ATTRIBUTE *attrs;
	FindObjects *find = NULL;
	p11_session *session;
	CK_ULONG matched;
	CK_RV rv;

	return_val_if_fail (count != NULL, CKR_ARGUMENTS_BAD);

	p11_debug ("in");

	p11_lock ();

		rv = lookup_session (handle, &session);
		if (rv == CKR_OK) {
			if (session->cleanup != find_objects_free)
				rv = CKR_OPERATION_NOT_INITIALIZED;
			find = session->operation;
		}

		if (rv == CKR_OK) {
			matched = 0;
			while (matched < max_count) {
				object = find->snapshot[find->iterator];
				if (!object)
					break;

				find->iterator++;

				attrs = p11_session_get_object (session, object, NULL);
				if (attrs == NULL)
					continue;

				if (p11_attrs_match (attrs, find->match)) {
					objects[matched] = object;
					matched++;
				}
			}

			*count = matched;
		}

	p11_unlock ();

	p11_debug ("out: 0x%lx", rv);

	return rv;
}

static CK_RV
sys_C_FindObjectsFinal (CK_SESSION_HANDLE handle)
{
	p11_session *session;
	CK_RV rv;

	p11_debug ("in");

	p11_lock ();

		rv = lookup_session (handle, &session);
		if (rv == CKR_OK) {
			if (session->cleanup != find_objects_free)
				rv = CKR_OPERATION_NOT_INITIALIZED;
			else
				p11_session_set_operation (session, NULL, NULL);
		}

	p11_unlock ();

	p11_debug ("out: 0x%lx", rv);

	return rv;
}

static CK_RV
sys_C_EncryptInit (CK_SESSION_HANDLE handle,
                   CK_MECHANISM_PTR mechanism,
                   CK_OBJECT_HANDLE key)
{
	return_val_if_reached (CKR_MECHANISM_INVALID);
}

static CK_RV
sys_C_Encrypt (CK_SESSION_HANDLE handle,
               CK_BYTE_PTR data,
               CK_ULONG data_len,
               CK_BYTE_PTR encrypted_data,
               CK_ULONG_PTR encrypted_data_len)
{
	return_val_if_reached (CKR_OPERATION_NOT_INITIALIZED);
}

static CK_RV
sys_C_EncryptUpdate (CK_SESSION_HANDLE handle,
                     CK_BYTE_PTR part,
                     CK_ULONG part_len,
                     CK_BYTE_PTR encrypted_part,
                     CK_ULONG_PTR encrypted_part_len)
{
	return_val_if_reached (CKR_OPERATION_NOT_INITIALIZED);
}

static CK_RV
sys_C_EncryptFinal (CK_SESSION_HANDLE handle,
                    CK_BYTE_PTR last_part,
                    CK_ULONG_PTR last_part_len)
{
	return_val_if_reached (CKR_OPERATION_NOT_INITIALIZED);
}

static CK_RV
sys_C_DecryptInit (CK_SESSION_HANDLE handle,
                   CK_MECHANISM_PTR mechanism,
                   CK_OBJECT_HANDLE key)
{
	return_val_if_reached (CKR_MECHANISM_INVALID);
}

static CK_RV
sys_C_Decrypt (CK_SESSION_HANDLE handle,
               CK_BYTE_PTR enc_data,
               CK_ULONG enc_data_len,
               CK_BYTE_PTR data,
               CK_ULONG_PTR data_len)
{
	return_val_if_reached (CKR_OPERATION_NOT_INITIALIZED);
}

static CK_RV
sys_C_DecryptUpdate (CK_SESSION_HANDLE handle,
                     CK_BYTE_PTR enc_part,
                     CK_ULONG enc_part_len,
                     CK_BYTE_PTR part,
                     CK_ULONG_PTR part_len)
{
	return_val_if_reached (CKR_OPERATION_NOT_INITIALIZED);
}

static CK_RV
sys_C_DecryptFinal (CK_SESSION_HANDLE handle,
                    CK_BYTE_PTR last_part,
                    CK_ULONG_PTR last_part_len)
{
	return_val_if_reached (CKR_OPERATION_NOT_INITIALIZED);
}

static CK_RV
sys_C_DigestInit (CK_SESSION_HANDLE handle,
                  CK_MECHANISM_PTR mechanism)
{
	return_val_if_reached (CKR_MECHANISM_INVALID);
}

static CK_RV
sys_C_Digest (CK_SESSION_HANDLE handle,
              CK_BYTE_PTR data,
              CK_ULONG data_len,
              CK_BYTE_PTR digest,
              CK_ULONG_PTR digest_len)
{
	return_val_if_reached (CKR_OPERATION_NOT_INITIALIZED);
}

static CK_RV
sys_C_DigestUpdate (CK_SESSION_HANDLE handle,
                    CK_BYTE_PTR part,
                    CK_ULONG part_len)
{
	return_val_if_reached (CKR_OPERATION_NOT_INITIALIZED);
}

static CK_RV
sys_C_DigestKey (CK_SESSION_HANDLE handle,
                 CK_OBJECT_HANDLE key)
{
	return_val_if_reached (CKR_OPERATION_NOT_INITIALIZED);
}

static CK_RV
sys_C_DigestFinal (CK_SESSION_HANDLE handle,
                   CK_BYTE_PTR digest,
                   CK_ULONG_PTR digest_len)
{
	return_val_if_reached (CKR_OPERATION_NOT_INITIALIZED);
}

static CK_RV
sys_C_SignInit (CK_SESSION_HANDLE handle,
                CK_MECHANISM_PTR mechanism,
                CK_OBJECT_HANDLE key)
{
	return_val_if_reached (CKR_MECHANISM_INVALID);
}

static CK_RV
sys_C_Sign (CK_SESSION_HANDLE handle,
            CK_BYTE_PTR data,
            CK_ULONG data_len,
            CK_BYTE_PTR signature,
            CK_ULONG_PTR signature_len)
{
	return_val_if_reached (CKR_OPERATION_NOT_INITIALIZED);
}

static CK_RV
sys_C_SignUpdate (CK_SESSION_HANDLE handle,
                  CK_BYTE_PTR part,
                  CK_ULONG part_len)
{
	return_val_if_reached (CKR_OPERATION_NOT_INITIALIZED);
}

static CK_RV
sys_C_SignFinal (CK_SESSION_HANDLE handle,
                 CK_BYTE_PTR signature,
                 CK_ULONG_PTR signature_len)
{
	return_val_if_reached (CKR_OPERATION_NOT_INITIALIZED);
}

static CK_RV
sys_C_SignRecoverInit (CK_SESSION_HANDLE handle,
                       CK_MECHANISM_PTR mechanism,
                       CK_OBJECT_HANDLE key)
{
	return_val_if_reached (CKR_MECHANISM_INVALID);
}

static CK_RV
sys_C_SignRecover (CK_SESSION_HANDLE handle,
                   CK_BYTE_PTR data,
                   CK_ULONG data_len,
                   CK_BYTE_PTR signature,
                   CK_ULONG_PTR signature_len)
{
	return_val_if_reached (CKR_OPERATION_NOT_INITIALIZED);
}

static CK_RV
sys_C_VerifyInit (CK_SESSION_HANDLE handle,
                  CK_MECHANISM_PTR mechanism,
                  CK_OBJECT_HANDLE key)
{
	return_val_if_reached (CKR_MECHANISM_INVALID);
}

static CK_RV
sys_C_Verify (CK_SESSION_HANDLE handle,
              CK_BYTE_PTR data,
              CK_ULONG data_len,
              CK_BYTE_PTR signature,
              CK_ULONG signature_len)
{
	return_val_if_reached (CKR_OPERATION_NOT_INITIALIZED);
}

static CK_RV
sys_C_VerifyUpdate (CK_SESSION_HANDLE handle,
                    CK_BYTE_PTR part,
                    CK_ULONG part_len)
{
	return_val_if_reached (CKR_OPERATION_NOT_INITIALIZED);
}

static CK_RV
sys_C_VerifyFinal (CK_SESSION_HANDLE handle,
                   CK_BYTE_PTR signature,
                   CK_ULONG signature_len)
{
	return_val_if_reached (CKR_OPERATION_NOT_INITIALIZED);
}

static CK_RV
sys_C_VerifyRecoverInit (CK_SESSION_HANDLE handle,
                         CK_MECHANISM_PTR mechanism,
                         CK_OBJECT_HANDLE key)
{
	return_val_if_reached (CKR_MECHANISM_INVALID);
}

static CK_RV
sys_C_VerifyRecover (CK_SESSION_HANDLE handle,
                     CK_BYTE_PTR signature,
                     CK_ULONG signature_len,
                     CK_BYTE_PTR data,
                     CK_ULONG_PTR data_len)
{
	return_val_if_reached (CKR_OPERATION_NOT_INITIALIZED);
}

static CK_RV
sys_C_DigestEncryptUpdate (CK_SESSION_HANDLE handle,
                           CK_BYTE_PTR part,
                           CK_ULONG part_len,
                           CK_BYTE_PTR enc_part,
                           CK_ULONG_PTR enc_part_len)
{
	return_val_if_reached (CKR_OPERATION_NOT_INITIALIZED);
}

static CK_RV
sys_C_DecryptDigestUpdate (CK_SESSION_HANDLE handle,
                           CK_BYTE_PTR enc_part,
                           CK_ULONG enc_part_len,
                           CK_BYTE_PTR part,
                           CK_ULONG_PTR part_len)
{
	return_val_if_reached (CKR_OPERATION_NOT_INITIALIZED);
}

static CK_RV
sys_C_SignEncryptUpdate (CK_SESSION_HANDLE handle,
                         CK_BYTE_PTR part,
                         CK_ULONG part_len,
                         CK_BYTE_PTR enc_part,
                         CK_ULONG_PTR enc_part_len)
{
	return_val_if_reached (CKR_OPERATION_NOT_INITIALIZED);
}

static CK_RV
sys_C_DecryptVerifyUpdate (CK_SESSION_HANDLE handle,
                           CK_BYTE_PTR enc_part,
                           CK_ULONG enc_part_len,
                           CK_BYTE_PTR part,
                           CK_ULONG_PTR part_len)
{
	return_val_if_reached (CKR_OPERATION_NOT_INITIALIZED);
}

static CK_RV
sys_C_GenerateKey (CK_SESSION_HANDLE handle,
                   CK_MECHANISM_PTR mechanism,
                   CK_ATTRIBUTE_PTR template,
                   CK_ULONG count,
                   CK_OBJECT_HANDLE_PTR key)
{
	return_val_if_reached (CKR_MECHANISM_INVALID);
}

static CK_RV
sys_C_GenerateKeyPair (CK_SESSION_HANDLE handle,
                       CK_MECHANISM_PTR mechanism,
                       CK_ATTRIBUTE_PTR pub_template,
                       CK_ULONG pub_count,
                       CK_ATTRIBUTE_PTR priv_template,
                       CK_ULONG priv_count,
                       CK_OBJECT_HANDLE_PTR pub_key,
                       CK_OBJECT_HANDLE_PTR priv_key)
{
	return_val_if_reached (CKR_MECHANISM_INVALID);
}

static CK_RV
sys_C_WrapKey (CK_SESSION_HANDLE handle,
               CK_MECHANISM_PTR mechanism,
               CK_OBJECT_HANDLE wrapping_key,
               CK_OBJECT_HANDLE key,
               CK_BYTE_PTR wrapped_key,
               CK_ULONG_PTR wrapped_key_len)
{
	return_val_if_reached (CKR_MECHANISM_INVALID);
}

static CK_RV
sys_C_UnwrapKey (CK_SESSION_HANDLE handle,
                 CK_MECHANISM_PTR mechanism,
                 CK_OBJECT_HANDLE unwrapping_key,
                 CK_BYTE_PTR wrapped_key,
                 CK_ULONG wrapped_key_len,
                 CK_ATTRIBUTE_PTR template,
                 CK_ULONG count,
                 CK_OBJECT_HANDLE_PTR key)
{
	return_val_if_reached (CKR_MECHANISM_INVALID);
}

static CK_RV
sys_C_DeriveKey (CK_SESSION_HANDLE handle,
                 CK_MECHANISM_PTR mechanism,
                 CK_OBJECT_HANDLE base_key,
                 CK_ATTRIBUTE_PTR template,
                 CK_ULONG count,
                 CK_OBJECT_HANDLE_PTR key)
{
	return_val_if_reached (CKR_MECHANISM_INVALID);
}

static CK_RV
sys_C_SeedRandom (CK_SESSION_HANDLE handle,
                  CK_BYTE_PTR seed,
                  CK_ULONG seed_len)
{
	return_val_if_reached (CKR_RANDOM_NO_RNG);
}

static CK_RV
sys_C_GenerateRandom (CK_SESSION_HANDLE handle,
                      CK_BYTE_PTR random_data,
                      CK_ULONG random_len)
{
	return_val_if_reached (CKR_RANDOM_NO_RNG);
}

/* --------------------------------------------------------------------
 * MODULE ENTRY POINT
 */

static CK_FUNCTION_LIST sys_function_list = {
	{ CRYPTOKI_VERSION_MAJOR, CRYPTOKI_VERSION_MINOR },  /* version */
	sys_C_Initialize,
	sys_C_Finalize,
	sys_C_GetInfo,
	sys_C_GetFunctionList,
	sys_C_GetSlotList,
	sys_C_GetSlotInfo,
	sys_C_GetTokenInfo,
	sys_C_GetMechanismList,
	sys_C_GetMechanismInfo,
	sys_C_InitToken,
	sys_C_InitPIN,
	sys_C_SetPIN,
	sys_C_OpenSession,
	sys_C_CloseSession,
	sys_C_CloseAllSessions,
	sys_C_GetSessionInfo,
	sys_C_GetOperationState,
	sys_C_SetOperationState,
	sys_C_Login,
	sys_C_Logout,
	sys_C_CreateObject,
	sys_C_CopyObject,
	sys_C_DestroyObject,
	sys_C_GetObjectSize,
	sys_C_GetAttributeValue,
	sys_C_SetAttributeValue,
	sys_C_FindObjectsInit,
	sys_C_FindObjects,
	sys_C_FindObjectsFinal,
	sys_C_EncryptInit,
	sys_C_Encrypt,
	sys_C_EncryptUpdate,
	sys_C_EncryptFinal,
	sys_C_DecryptInit,
	sys_C_Decrypt,
	sys_C_DecryptUpdate,
	sys_C_DecryptFinal,
	sys_C_DigestInit,
	sys_C_Digest,
	sys_C_DigestUpdate,
	sys_C_DigestKey,
	sys_C_DigestFinal,
	sys_C_SignInit,
	sys_C_Sign,
	sys_C_SignUpdate,
	sys_C_SignFinal,
	sys_C_SignRecoverInit,
	sys_C_SignRecover,
	sys_C_VerifyInit,
	sys_C_Verify,
	sys_C_VerifyUpdate,
	sys_C_VerifyFinal,
	sys_C_VerifyRecoverInit,
	sys_C_VerifyRecover,
	sys_C_DigestEncryptUpdate,
	sys_C_DecryptDigestUpdate,
	sys_C_SignEncryptUpdate,
	sys_C_DecryptVerifyUpdate,
	sys_C_GenerateKey,
	sys_C_GenerateKeyPair,
	sys_C_WrapKey,
	sys_C_UnwrapKey,
	sys_C_DeriveKey,
	sys_C_SeedRandom,
	sys_C_GenerateRandom,
	sys_C_GetFunctionStatus,
	sys_C_CancelFunction,
	sys_C_WaitForSlotEvent
};

#ifdef OS_WIN32
__declspec(dllexport)
#endif

CK_RV
C_GetFunctionList (CK_FUNCTION_LIST_PTR_PTR list)
{
	return sys_C_GetFunctionList (list);
}

CK_ULONG
p11_module_next_id (void)
{
	static CK_ULONG unique = 0x10;
	return (unique)++;
}