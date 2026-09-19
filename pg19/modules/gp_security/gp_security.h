/*-------------------------------------------------------------------------
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 * gp_security.h
 *	  What the parts of the password-profile code share.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_SECURITY_H
#define GP_SECURITY_H

#include "postgres.h"

#include "datatype/timestamp.h"
#include "nodes/parsenodes.h"

#define GP_SECURITY_SCHEMA		"gp_security"
#define GP_PROFILE_PROVIDER		"gp_profile"

/* The profile a role with none of its own is under.  Cloudberry's pg_default;
 * a role may not be named pg_*, so the port's is gp_default. */
#define GP_DEFAULT_PROFILE		"gp_default"

/*
 * Cloudberry's PROFILE_DEFAULT and PROFILE_UNLIMITED
 * (src/include/catalog/pg_profile.h:72-74), and the same ceiling on a limit.
 */
#define GP_PROFILE_DEFAULT		(-1)
#define GP_PROFILE_UNLIMITED	(-2)
#define GP_PROFILE_MAX_VALID	9999

/*
 * What a profile says.  Every field but the verify function is a count or a
 * number of days, with the two negative values above for "take the default
 * profile's" and "no limit".
 */
typedef struct GpProfile
{
	int			failed_login_attempts;
	int			password_lock_time;	/* days */
	int			password_life_time;	/* days */
	int			password_grace_time;	/* days */
	int			password_reuse_time;	/* days */
	int			password_reuse_max;	/* password changes */
	int			password_allow_hashed;	/* 1 yes, 0 no */
	char	   *password_verify_function;	/* qualified name, or NULL */
} GpProfile;

/* GUCs, all named gp.* so that a leftover one in a file is a placeholder. */
extern PGDLLIMPORT bool gp_enable_password_profile;
extern PGDLLIMPORT char *gp_security_database;

/* profile.c */

/* Registered during preload. */
extern void GpProfileRegisterProvider(void);

/* Is this role a profile definition rather than an ordinary role? */
extern bool GpProfileIsProfileRole(Oid roleid);

/*
 * The profile a role is under, with every "default" filled in from the
 * default profile.  Returns false when the role has no profile, which is when
 * none of this applies to it.
 */
extern bool GpProfileForRole(Oid roleid, GpProfile *out);

/* The name of the profile a role is under, or NULL. */
extern char *GpProfileNameForRole(Oid roleid);

/* Read one profile's own settings, before defaults are filled in. */
extern bool GpProfileRead(const char *profile, GpProfile *out);

/* login.c */

/* The live state of a role, which the label is the durable copy of. */
typedef struct GpLoginState
{
	int			failed_logins;
	TimestampTz locked_until;	/* 0: not locked; DT_NOEND: until unlocked */
} GpLoginState;

extern void GpLoginStateGet(Oid roleid, GpLoginState *out);
extern void GpLoginStateSet(Oid roleid, const GpLoginState *state);

/* Installed during preload. */
extern void GpLoginInstallHook(void);

/* Registered during preload; persists what the hook decides. */
extern void GpSecurityRegisterWorker(void);

/* Ask the worker to write a role's state out. */
extern void GpSecurityWakeWorker(Oid roleid);

/* password.c */

/* Installed during preload. */
extern void GpPasswordInstallHook(void);

/*
 * A password was accepted for this role: remember it, and give the role the
 * VALID UNTIL its profile's password_life_time asks for.  Called after the
 * statement that set it.
 */
extern void GpPasswordRecorded(const char *rolename);

/* Which roles a CREATE/ALTER ROLE statement gave a password to. */
extern char *GpPasswordRoleOfStmt(Node *parsetree);

#endif							/* GP_SECURITY_H */
