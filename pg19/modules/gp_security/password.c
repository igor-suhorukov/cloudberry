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
 * password.c
 *	  What a new password must satisfy, and what is remembered about old ones.
 *
 * check_password_hook sees a password before it is stored, which is where
 * Cloudberry's PASSWORD_VERIFY_FUNCTION, PASSWORD_ALLOW_HASHED and the two
 * reuse rules belong.  The rest happens after the statement: the verifier
 * that was actually stored is copied into the history, and the role is given
 * the VALID UNTIL that PASSWORD_LIFE_TIME asks for.
 *
 * The history is the one piece of this module that may not be a security
 * label.  A label is readable by everyone (pg_seclabels is a plain view), and
 * an old password verifier is worth attacking offline, so it goes to a table
 * that is revoked from PUBLIC -- which is what decision 9 says to do with
 * secrets and history.  That table lives in one database, so a password
 * change for a role whose profile has reuse rules is refused anywhere else,
 * rather than quietly recording nothing.
 *
 * Cloudberry sources this file is made of:
 *	  the profile code of src/backend/commands/user.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_authid.h"
#include "commands/user.h"
#include "executor/spi.h"
#include "libpq/crypt.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/timestamp.h"

#include "gp_security.h"

static check_password_hook_type prev_check_password = NULL;

/*
 * What the hook learned about the password now being set, for the pass that
 * runs after the statement.  One statement sets at most one password.
 */
static bool password_pending = false;
static bool password_pending_validuntil_given = false;

/* ------------------------------------------------------------------------- */
/* Where the history lives                                                   */
/* ------------------------------------------------------------------------- */

static bool
history_is_here(void)
{
	char	   *here = get_database_name(MyDatabaseId);

	return here != NULL && gp_security_database != NULL &&
		strcmp(here, gp_security_database) == 0;
}

/*
 * A profile with reuse rules needs the history, and the history is in one
 * database.  Say so rather than record nothing.
 */
static void
history_require_here(const char *username)
{
	if (history_is_here())
		return;

	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot change the password of role \"%s\" in database \"%s\"",
					username, get_database_name(MyDatabaseId)),
			 errdetail("Its profile limits password reuse, and the password history is kept in database \"%s\", which is what \"gp.security_database\" names.",
					   gp_security_database),
			 errhint("Change it while connected to \"%s\".", gp_security_database)));
}

/*
 * Is the new password one of the old ones the profile still remembers?
 *
 * PASSWORD_REUSE_TIME is how long a password is remembered, and
 * PASSWORD_REUSE_MAX how many changes must come between two uses of the same
 * one.  A password is refused if either rule still covers it.
 */
static void
history_check(const char *username, const char *newpass, PasswordType type,
			  const GpProfile *profile)
{
	Oid			argtypes[2] = {TEXTOID, INT4OID};
	Datum		values[2];
	StringInfoData sql;
	uint64		i;
	bool		reused = false;

	if (profile->password_reuse_time <= 0 && profile->password_reuse_max <= 0)
		return;

	history_require_here(username);

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	/*
	 * Everything either rule still covers: the newest `reuse_max` verifiers,
	 * and anything set less than `reuse_time` days ago.
	 */
	initStringInfo(&sql);
	appendStringInfoString(&sql,
						   "SELECT verifier FROM " GP_SECURITY_SCHEMA ".password_history"
						   " WHERE rolname = $1 AND (false");
	if (profile->password_reuse_max > 0)
		appendStringInfo(&sql,
						 " OR set_at >= (SELECT min(set_at) FROM ("
						 "     SELECT set_at FROM " GP_SECURITY_SCHEMA ".password_history"
						 "      WHERE rolname = $1 ORDER BY set_at DESC LIMIT %d) recent)",
						 profile->password_reuse_max);
	if (profile->password_reuse_time > 0)
		appendStringInfoString(&sql,
							   " OR set_at > now() - ($2 || ' days')::interval");
	appendStringInfoString(&sql, ")");

	values[0] = CStringGetTextDatum(username);
	values[1] = Int32GetDatum(profile->password_reuse_time > 0
							  ? profile->password_reuse_time : 0);

	if (SPI_execute_with_args(sql.data, 2, argtypes, values, NULL, true, 0) != SPI_OK_SELECT)
		elog(ERROR, "gp_security: could not read the password history");

	for (i = 0; i < SPI_processed && !reused; i++)
	{
		bool		isnull;
		Datum		d = SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc,
									  1, &isnull);
		char	   *old;

		if (isnull)
			continue;
		old = TextDatumGetCString(d);

		if (type == PASSWORD_TYPE_PLAINTEXT)
		{
			const char *logdetail = NULL;

			/*
			 * plain_crypt_verify answers the same question the server asks at
			 * login: does this plaintext produce that verifier?
			 */
			if (plain_crypt_verify(username, old, newpass, &logdetail) == STATUS_OK)
				reused = true;
		}
		else if (strcmp(old, newpass) == 0)
		{
			/*
			 * An already-hashed password can only be compared as it stands.
			 * The same plaintext hashed again would have another salt, which
			 * is what PASSWORD_ALLOW_HASHED is for.
			 */
			reused = true;
		}
	}

	SPI_finish();

	if (reused)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PASSWORD),
				 errmsg("password for role \"%s\" may not be reused yet", username),
				 errdetail("Its profile keeps the last %d password(s), for %d day(s).",
						   profile->password_reuse_max, profile->password_reuse_time)));
}

/* ------------------------------------------------------------------------- */
/* The hook                                                                  */
/* ------------------------------------------------------------------------- */

static void
gp_security_check_password(const char *username, const char *shadow_pass,
						   PasswordType password_type, Datum validuntil_time,
						   bool validuntil_null)
{
	Oid			roleid;
	GpProfile	profile;

	if (prev_check_password)
		prev_check_password(username, shadow_pass, password_type,
							validuntil_time, validuntil_null);

	password_pending = false;

	if (!gp_enable_password_profile)
		return;

	roleid = get_role_oid(username, true);

	/*
	 * CREATE ROLE reaches here before the role exists, so a new role is held
	 * to the default profile until it is given one of its own.
	 */
	if (OidIsValid(roleid))
	{
		if (!GpProfileForRole(roleid, &profile))
			return;
	}
	else if (!GpProfileRead(GP_DEFAULT_PROFILE, &profile))
		return;

	/* PASSWORD_ALLOW_HASHED */
	if (profile.password_allow_hashed == 0 &&
		password_type != PASSWORD_TYPE_PLAINTEXT)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PASSWORD),
				 errmsg("password for role \"%s\" must be given in plain text",
						username),
				 errdetail("Its profile does not allow an already-hashed password."),
				 errhint("Without the plain text the server cannot tell whether the password obeys the profile's other rules.")));

	/* PASSWORD_VERIFY_FUNCTION */
	if (profile.password_verify_function != NULL)
	{
		Oid			argtypes[2] = {TEXTOID, TEXTOID};
		Datum		values[2];
		StringInfoData sql;

		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "SPI_connect failed");

		initStringInfo(&sql);
		appendStringInfo(&sql, "SELECT %s($1, $2)",
						 profile.password_verify_function);

		values[0] = CStringGetTextDatum(username);
		values[1] = CStringGetTextDatum(shadow_pass);

		if (SPI_execute_with_args(sql.data, 2, argtypes, values, NULL, true, 1)
			!= SPI_OK_SELECT)
			elog(ERROR, "gp_security: password verify function \"%s\" failed",
				 profile.password_verify_function);

		SPI_finish();
	}

	/* PASSWORD_REUSE_TIME and PASSWORD_REUSE_MAX */
	history_check(username, shadow_pass, password_type, &profile);

	password_pending = true;
	password_pending_validuntil_given = !validuntil_null;
}

void
GpPasswordInstallHook(void)
{
	prev_check_password = check_password_hook;
	check_password_hook = gp_security_check_password;
}

/* ------------------------------------------------------------------------- */
/* After the statement                                                       */
/* ------------------------------------------------------------------------- */

/*
 * The role a CREATE/ALTER ROLE statement gave a password to, or NULL.  Asked
 * before the statement runs, so that the name is read from the parse tree
 * rather than guessed afterwards.
 */
char *
GpPasswordRoleOfStmt(Node *parsetree)
{
	List	   *options;
	const char *rolename;
	ListCell   *lc;

	if (IsA(parsetree, CreateRoleStmt))
	{
		CreateRoleStmt *stmt = (CreateRoleStmt *) parsetree;

		options = stmt->options;
		rolename = stmt->role;
	}
	else if (IsA(parsetree, AlterRoleStmt))
	{
		AlterRoleStmt *stmt = (AlterRoleStmt *) parsetree;

		options = stmt->options;
		rolename = stmt->role->rolename;
		if (rolename == NULL)	/* CURRENT_USER and friends */
			rolename = GetUserNameFromId(GetUserId(), false);
	}
	else
		return NULL;

	foreach(lc, options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "password") == 0 && def->arg != NULL)
			return pstrdup(rolename);
	}

	return NULL;
}

/*
 * The password was accepted and stored.  Remember the verifier, and give the
 * role the VALID UNTIL its profile asks for.
 */
void
GpPasswordRecorded(const char *rolename)
{
	Oid			roleid;
	GpProfile	profile;
	const char *logdetail = NULL;
	char	   *verifier;

	if (!gp_enable_password_profile || !password_pending)
		return;

	password_pending = false;

	roleid = get_role_oid(rolename, true);
	if (!OidIsValid(roleid) || !GpProfileForRole(roleid, &profile))
		return;

	/*
	 * What was actually stored, rather than what the statement said: the
	 * server may have hashed it, and the history has to hold the form a login
	 * will be checked against.
	 */
	verifier = get_role_password(rolename, &logdetail);

	if (verifier != NULL &&
		(profile.password_reuse_time > 0 || profile.password_reuse_max > 0))
	{
		Oid			argtypes[2] = {TEXTOID, TEXTOID};
		Datum		values[2];

		history_require_here(rolename);

		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "SPI_connect failed");

		values[0] = CStringGetTextDatum(rolename);
		values[1] = CStringGetTextDatum(verifier);

		if (SPI_execute_with_args("INSERT INTO " GP_SECURITY_SCHEMA ".password_history"
								  "       (rolname, verifier, set_at)"
								  " VALUES ($1, $2, now())",
								  2, argtypes, values, NULL, false, 0) != SPI_OK_INSERT)
			elog(ERROR, "gp_security: could not record a password");

		SPI_finish();
	}

	/*
	 * PASSWORD_LIFE_TIME, and PASSWORD_GRACE_TIME after it.  PostgreSQL has
	 * one cutoff rather than Oracle's two, so the grace period is inside the
	 * valid window; how long before it a warning is given is
	 * password_expiration_warning_threshold, which is server-wide rather than
	 * per profile.
	 *
	 * A statement that set VALID UNTIL itself is left alone: it said what it
	 * wanted.
	 */
	if (profile.password_life_time > 0 && !password_pending_validuntil_given)
	{
		int			days = profile.password_life_time;
		StringInfoData sql;

		if (profile.password_grace_time > 0)
			days += profile.password_grace_time;

		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "SPI_connect failed");

		initStringInfo(&sql);
		appendStringInfo(&sql, "ALTER ROLE %s VALID UNTIL %s",
						 quote_identifier(rolename),
						 quote_literal_cstr(timestamptz_to_str(
							 TimestampTzPlusMilliseconds(GetCurrentTimestamp(),
														 (int64) days * 86400 * 1000))));

		if (SPI_execute(sql.data, false, 0) != SPI_OK_UTILITY)
			elog(ERROR, "gp_security: could not set VALID UNTIL for role \"%s\"",
				 rolename);

		SPI_finish();
	}
}
