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
 * gp_security.c
 *	  Password profiles, account locking and login windows.
 *
 * Cloudberry has two shared catalogs and seven pg_authid columns for this,
 * and a pair of postmaster children to write them.  Here a profile is a
 * NOLOGIN role carrying a shared "gp_profile" label (profile.c), the state of
 * an account is shared memory with a "gp" label as its durable copy
 * (login.c), and what a new password must satisfy is checked in
 * check_password_hook (password.c).  A background worker stands in for the
 * login monitor, for the reason Cloudberry has one: a backend whose login
 * just failed cannot commit anything.
 *
 * Cloudberry sources this module is made of:
 *	  src/backend/commands/pg_profile.c, postmaster/loginmonitor.c,
 *	  and the profile code of user.c, auth.c and postinit.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_authid.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/timestamp.h"

#include "cb_module.h"
#include "gp_core_api.h"
#include "gp_label.h"
#include "gp_security.h"

PG_MODULE_MAGIC_EXT(
					.name = "gp_security",
					.version = GP_VERSION
);

bool		gp_enable_password_profile = false;
char	   *gp_security_database = NULL;

static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/* ------------------------------------------------------------------------- */
/* Hooks                                                                     */
/* ------------------------------------------------------------------------- */

/*
 * A password is checked before the statement runs and remembered after it, so
 * that what goes into the history is the verifier that was really stored.
 */
static void
gp_security_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
						   bool readOnlyTree, ProcessUtilityContext context,
						   ParamListInfo params, QueryEnvironment *queryEnv,
						   DestReceiver *dest, QueryCompletion *qc)
{
	char	   *rolename = NULL;

	if (gp_enable_password_profile)
		rolename = GpPasswordRoleOfStmt(pstmt->utilityStmt);

	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);

	if (rolename != NULL)
	{
		CommandCounterIncrement();
		GpPasswordRecorded(rolename);
	}
}

/* ------------------------------------------------------------------------- */
/* SQL                                                                       */
/* ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(gp_security_assign_profile);
PG_FUNCTION_INFO_V1(gp_security_lock_role);
PG_FUNCTION_INFO_V1(gp_security_unlock_role);
PG_FUNCTION_INFO_V1(gp_security_role_locked_until);
PG_FUNCTION_INFO_V1(gp_security_role_failed_logins);

/*
 * The check SECURITY LABEL makes for a role, made here: these functions write
 * the same labels, so they ask the same question.
 */
static Oid
security_role_to_change(const char *rolename)
{
	Oid			roleid = get_role_oid(rolename, false);

	if (!object_ownercheck(AuthIdRelationId, roleid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_ROLE, rolename);

	return roleid;
}

/*
 * gp_security.assign_profile(role name, profile name)
 *
 * What Cloudberry writes as ALTER USER ... PROFILE p; a NULL profile is its
 * ALTER USER ... NOPROFILE.
 */
Datum
gp_security_assign_profile(PG_FUNCTION_ARGS)
{
	Name		rolename = PG_GETARG_NAME(0);
	Oid			roleid = security_role_to_change(NameStr(*rolename));
	ObjectAddress addr;

	ObjectAddressSet(addr, AuthIdRelationId, roleid);

	if (PG_ARGISNULL(1))
	{
		GpLabelSet(&addr, GP_LABEL_profile, NULL);
		PG_RETURN_VOID();
	}
	else
	{
		Name		profile = PG_GETARG_NAME(1);
		Oid			profileid = get_role_oid(NameStr(*profile), true);

		if (!OidIsValid(profileid) || !GpProfileIsProfileRole(profileid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("profile \"%s\" does not exist", NameStr(*profile))));

		GpLabelSet(&addr, GP_LABEL_profile, NameStr(*profile));
	}

	PG_RETURN_VOID();
}

/*
 * gp_security.lock_role(name) and unlock_role(name)
 *
 * Cloudberry's ALTER USER ... ACCOUNT LOCK and ACCOUNT UNLOCK.  The lock is
 * the label and nothing else: rolcanlogin keeps saying what an administrator
 * said, so the two never have to be told apart.
 */
Datum
gp_security_lock_role(PG_FUNCTION_ARGS)
{
	Name		rolename = PG_GETARG_NAME(0);
	Oid			roleid = security_role_to_change(NameStr(*rolename));
	ObjectAddress addr;
	GpLoginState st;

	ObjectAddressSet(addr, AuthIdRelationId, roleid);
	GpLabelSet(&addr, GP_LABEL_locked_until, "forever");

	GpLoginStateGet(roleid, &st);
	st.locked_until = DT_NOEND;
	GpLoginStateSet(roleid, &st);

	PG_RETURN_VOID();
}

Datum
gp_security_unlock_role(PG_FUNCTION_ARGS)
{
	Name		rolename = PG_GETARG_NAME(0);
	Oid			roleid = security_role_to_change(NameStr(*rolename));
	ObjectAddress addr;
	GpLoginState st;

	ObjectAddressSet(addr, AuthIdRelationId, roleid);
	GpLabelSet(&addr, GP_LABEL_locked_until, NULL);
	GpLabelSet(&addr, GP_LABEL_failed_logins, NULL);

	GpLoginStateGet(roleid, &st);
	st.locked_until = 0;
	st.failed_logins = 0;
	GpLoginStateSet(roleid, &st);

	PG_RETURN_VOID();
}

/*
 * gp_security.role_locked_until(name) -> timestamptz
 *
 * NULL when the role is not locked; infinity when it is locked until someone
 * unlocks it.  This is Cloudberry's pg_authid.rollockdate next to
 * rolaccountstatus.
 */
Datum
gp_security_role_locked_until(PG_FUNCTION_ARGS)
{
	Name		rolename = PG_GETARG_NAME(0);
	Oid			roleid = get_role_oid(NameStr(*rolename), true);
	GpLoginState st;

	if (!OidIsValid(roleid))
		PG_RETURN_NULL();

	GpLoginStateGet(roleid, &st);

	if (st.locked_until == 0)
		PG_RETURN_NULL();

	PG_RETURN_TIMESTAMPTZ(st.locked_until);
}

/*
 * gp_security.role_failed_logins(name) -> integer
 *
 * Cloudberry's pg_authid.rolfailedlogins.
 */
Datum
gp_security_role_failed_logins(PG_FUNCTION_ARGS)
{
	Name		rolename = PG_GETARG_NAME(0);
	Oid			roleid = get_role_oid(NameStr(*rolename), true);
	GpLoginState st;

	if (!OidIsValid(roleid))
		PG_RETURN_NULL();

	GpLoginStateGet(roleid, &st);
	PG_RETURN_INT32(st.failed_logins);
}

/* ------------------------------------------------------------------------- */

void
_PG_init(void)
{
	/*
	 * This module registers a background worker and installs the
	 * authentication hook, and neither can be done after the postmaster has
	 * started.  It also registers a security label provider, which every
	 * backend that reads such a label has to have registered.
	 */
	CB_REQUIRE_PRELOAD("gp_security");
	CB_REQUIRE_CORE("gp_security");

	DefineCustomBoolVariable("gp.enable_password_profile",
							 "Hold roles to the password profile they are under.",
							 "With this off, profiles can be defined and assigned "
							 "but nothing is enforced.  Cloudberry calls this "
							 "enable_password_profile.",
							 &gp_enable_password_profile,
							 false,
							 PGC_POSTMASTER,
							 0,
							 NULL, NULL, NULL);

	DefineCustomStringVariable("gp.security_database",
							   "Database holding the password history.",
							   "An old password verifier is a secret, so it cannot "
							   "go in a security label, which everyone may read. "
							   "It goes in a table in this database instead.",
							   &gp_security_database,
							   "postgres",
							   PGC_POSTMASTER,
							   0,
							   NULL, NULL, NULL);

	GpProfileRegisterProvider();
	GpLoginInstallHook();
	GpPasswordInstallHook();
	GpSecurityRegisterWorker();

	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = gp_security_ProcessUtility;
}
