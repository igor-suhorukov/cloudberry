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
#include "nodes/value.h"
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
 * The check SECURITY LABEL makes before it labels a role, made here: what
 * this module writes about a role are labels, so it asks the same question
 * -- CREATEROLE and the ADMIN option on the role, or superuser for a
 * superuser.  It used to ask object_ownercheck, which knows no owner of a
 * role, and answered every user but a superuser with an internal error.
 */
static void
security_check_role(Oid roleid)
{
	ObjectAddress addr;

	ObjectAddressSet(addr, AuthIdRelationId, roleid);
	check_object_ownership(GetUserId(), OBJECT_ROLE, addr,
						   (Node *) makeString(GetUserNameFromId(roleid, false)),
						   NULL);
}

/* Put a role under a profile, or take it from under one with NULL. */
static void
security_assign_profile(Oid roleid, const char *profile)
{
	ObjectAddress addr;

	ObjectAddressSet(addr, AuthIdRelationId, roleid);

	if (profile != NULL)
	{
		Oid			profileid = get_role_oid(profile, true);

		if (!OidIsValid(profileid) || !GpProfileIsProfileRole(profileid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("profile \"%s\" does not exist", profile)));
	}

	GpLabelSet(&addr, GP_LABEL_profile, profile);
}

/*
 * Lock an account, or unlock it.  The lock is the label and nothing else:
 * rolcanlogin keeps saying what an administrator said, so the two never have
 * to be told apart.
 */
static void
security_lock_role(Oid roleid, bool lock)
{
	ObjectAddress addr;
	GpLoginState st;

	ObjectAddressSet(addr, AuthIdRelationId, roleid);
	GpLoginStateGet(roleid, &st);

	if (lock)
	{
		GpLabelSet(&addr, GP_LABEL_locked_until, "forever");
		st.locked_until = DT_NOEND;
	}
	else
	{
		GpLabelSet(&addr, GP_LABEL_locked_until, NULL);
		GpLabelSet(&addr, GP_LABEL_failed_logins, NULL);
		st.locked_until = 0;
		st.failed_logins = 0;
	}

	GpLoginStateSet(roleid, &st);
}

/*
 * What O26 carried to an ALTER USER for this module (gp_desugar.c,
 * GpAttachCarriers): Cloudberry's role options PROFILE p and ACCOUNT LOCK and
 * UNLOCK, and the port's NOPROFILE, as DefElems in the "gp" namespace --
 * gp.profile = 'p' or DEFAULT, gp.account = 'lock' or 'unlock'.  PostgreSQL's
 * grammar never puts a namespace on a role option, so these are all ours.
 */
static bool
is_role_carrier(DefElem *def)
{
	return def->defnamespace != NULL && strcmp(def->defnamespace, "gp") == 0;
}

static bool
has_role_carriers(List *options)
{
	ListCell   *lc;

	foreach(lc, options)
	{
		if (is_role_carrier((DefElem *) lfirst(lc)))
			return true;
	}
	return false;
}

/* Take them out, before ALTER ROLE would refuse them. */
static List *
take_role_carriers(List **options)
{
	List	   *taken = NIL;
	ListCell   *lc;

	foreach(lc, *options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (!is_role_carrier(def))
			continue;
		taken = lappend(taken, def);
		*options = foreach_delete_current(*options, lc);
	}
	return taken;
}

/*
 * ALTER USER u PROFILE p, and the rest.  The profile is checked, and the role
 * checked for as SECURITY LABEL would check it, before the statement runs;
 * ALTER ROLE with nothing left in it then runs as it would, and answers ALTER
 * ROLE, as Cloudberry's does.
 */
static void
apply_role_carriers(Oid roleid, List *carried)
{
	ListCell   *lc;

	foreach(lc, carried)
	{
		DefElem    *def = (DefElem *) lfirst(lc);
		const char *value = def->arg != NULL ? strVal(def->arg) : NULL;

		if (strcmp(def->defname, "profile") == 0)
			security_assign_profile(roleid, value);
		else if (strcmp(def->defname, "account") == 0)
			security_lock_role(roleid, value != NULL && strcmp(value, "lock") == 0);
		else
			elog(ERROR, "gp_security: unrecognized role option \"%s\"", def->defname);
	}
}

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
	List	   *carried = NIL;
	Oid			roleid = InvalidOid;

	if (IsA(pstmt->utilityStmt, AlterRoleStmt) &&
		has_role_carriers(((AlterRoleStmt *) pstmt->utilityStmt)->options))
	{
		AlterRoleStmt *stmt;
		ListCell   *lc;

		/* A read-only tree belongs to a cached plan that may be run again. */
		if (readOnlyTree)
		{
			pstmt = copyObject(pstmt);
			readOnlyTree = false;
		}
		stmt = (AlterRoleStmt *) pstmt->utilityStmt;
		carried = take_role_carriers(&stmt->options);

		roleid = get_rolespec_oid(stmt->role, false);
		security_check_role(roleid);

		/* a profile that is not one is refused before anything is done */
		foreach(lc, carried)
		{
			DefElem    *def = (DefElem *) lfirst(lc);

			if (strcmp(def->defname, "profile") == 0 && def->arg != NULL)
			{
				Oid			profileid = get_role_oid(strVal(def->arg), true);

				if (!OidIsValid(profileid) || !GpProfileIsProfileRole(profileid))
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("profile \"%s\" does not exist", strVal(def->arg))));
			}
		}
	}

	if (gp_enable_password_profile)
		rolename = GpPasswordRoleOfStmt(pstmt->utilityStmt);

	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);

	if (carried != NIL)
	{
		CommandCounterIncrement();
		apply_role_carriers(roleid, carried);
	}

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
 * gp_security.assign_profile(role name, profile name)
 *
 * What Cloudberry writes as ALTER USER ... PROFILE p; a NULL profile is its
 * ALTER USER ... NOPROFILE.
 */
Datum
gp_security_assign_profile(PG_FUNCTION_ARGS)
{
	Oid			roleid = get_role_oid(NameStr(*PG_GETARG_NAME(0)), false);

	security_check_role(roleid);
	security_assign_profile(roleid,
							PG_ARGISNULL(1) ? NULL : NameStr(*PG_GETARG_NAME(1)));
	PG_RETURN_VOID();
}

/*
 * gp_security.lock_role(name) and unlock_role(name)
 *
 * Cloudberry's ALTER USER ... ACCOUNT LOCK and ACCOUNT UNLOCK.
 */
Datum
gp_security_lock_role(PG_FUNCTION_ARGS)
{
	Oid			roleid = get_role_oid(NameStr(*PG_GETARG_NAME(0)), false);

	security_check_role(roleid);
	security_lock_role(roleid, true);
	PG_RETURN_VOID();
}

Datum
gp_security_unlock_role(PG_FUNCTION_ARGS)
{
	Oid			roleid = get_role_oid(NameStr(*PG_GETARG_NAME(0)), false);

	security_check_role(roleid);
	security_lock_role(roleid, false);
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

	/*
	 * O26 carries a role's PROFILE and ACCOUNT LOCK to its ALTER USER for the
	 * hook below, and asks this first (cb_module.h).
	 */
	*find_rendezvous_variable(CB_SECURITY_RENDEZVOUS) = (void *) &gp_enable_password_profile;

	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = gp_security_ProcessUtility;
}
