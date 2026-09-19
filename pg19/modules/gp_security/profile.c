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
 * profile.c
 *	  Password profiles: what a role's password and logins must obey.
 *
 * Cloudberry keeps profiles in the shared catalog pg_profile and puts seven
 * more columns on pg_authid to say which profile a role is under and what
 * state its account is in.  An extension can add neither, and both have to be
 * readable while a session is logging in to any database -- so a profile is a
 * NOLOGIN role carrying a shared "gp_profile" security label, which is the
 * pattern "Cluster metadata without shared catalogs" settles on, and the link
 * from a role to its profile is the "gp" label key of the same name.
 *
 * A shared label is read from every database, is transactional, is dropped
 * with the role, and pg_dumpall writes it.  The limits it holds are not
 * secret: what is secret is the password history, which is why that lives in
 * a table nobody may read (see password.c).
 *
 * Cloudberry source this file is made of:
 *	  src/backend/commands/pg_profile.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/objectaddress.h"
#include "catalog/pg_authid.h"
#include "commands/seclabel.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/jsonb.h"

#include "gp_label.h"
#include "gp_security.h"

/*
 * The settings a profile label may carry: Cloudberry's pg_profile columns,
 * under the names its grammar gives them.  A table rather than an X-macro,
 * because two of the fields have different types and code generated for both
 * would have to pun one of them.
 */
typedef struct ProfileKey
{
	const char *name;
	size_t		offset;			/* into GpProfile */
	bool		is_number;
} ProfileKey;

static const ProfileKey profile_keys[] = {
	{"failed_login_attempts", offsetof(GpProfile, failed_login_attempts), true},
	{"password_lock_time", offsetof(GpProfile, password_lock_time), true},
	{"password_life_time", offsetof(GpProfile, password_life_time), true},
	{"password_grace_time", offsetof(GpProfile, password_grace_time), true},
	{"password_reuse_time", offsetof(GpProfile, password_reuse_time), true},
	{"password_reuse_max", offsetof(GpProfile, password_reuse_max), true},
	{"password_allow_hashed", offsetof(GpProfile, password_allow_hashed), true},
	{"password_verify_function", offsetof(GpProfile, password_verify_function), false},
};

#define GP_PROFILE_NKEYS	(sizeof(profile_keys) / sizeof(profile_keys[0]))

static int *
profile_int_field(GpProfile *p, const ProfileKey *k)
{
	Assert(k->is_number);
	return (int *) ((char *) p + k->offset);
}

static char **
profile_str_field(GpProfile *p, const ProfileKey *k)
{
	Assert(!k->is_number);
	return (char **) ((char *) p + k->offset);
}

/* The setting of this name, or NULL. */
static const ProfileKey *
profile_key(const char *name)
{
	for (unsigned i = 0; i < GP_PROFILE_NKEYS; i++)
	{
		if (strcmp(name, profile_keys[i].name) == 0)
			return &profile_keys[i];
	}

	return NULL;
}

static void
profile_init(GpProfile *p)
{
	for (unsigned i = 0; i < GP_PROFILE_NKEYS; i++)
	{
		if (profile_keys[i].is_number)
			*profile_int_field(p, &profile_keys[i]) = GP_PROFILE_DEFAULT;
		else
			*profile_str_field(p, &profile_keys[i]) = NULL;
	}
}

/* Anything `p` leaves at "default" comes from `fallback`. */
static void
profile_fill_defaults(GpProfile *p, const GpProfile *fallback)
{
	for (unsigned i = 0; i < GP_PROFILE_NKEYS; i++)
	{
		const ProfileKey *k = &profile_keys[i];

		if (k->is_number)
		{
			int		   *mine = profile_int_field(p, k);

			if (*mine == GP_PROFILE_DEFAULT)
				*mine = *profile_int_field(unconstify(GpProfile *, fallback), k);
		}
		else
		{
			char	  **mine = profile_str_field(p, k);

			if (*mine == NULL)
				*mine = *profile_str_field(unconstify(GpProfile *, fallback), k);
		}
	}
}

/* Take one key=value pair out of a label into `out`. */
static void
profile_set_from_json(GpProfile *out, const char *key, const JsonbValue *v)
{
	const ProfileKey *k = profile_key(key);

	if (k == NULL)
		return;					/* the check hook refuses these when set */

	if (k->is_number)
	{
		if (v->type == jbvNumeric)
			*profile_int_field(out, k) =
				DatumGetInt32(DirectFunctionCall1(numeric_int4,
												  NumericGetDatum(v->val.numeric)));
	}
	else if (v->type == jbvString)
		*profile_str_field(out, k) = pnstrdup(v->val.string.val, v->val.string.len);
}

/* ------------------------------------------------------------------------- */
/* The label                                                                 */
/* ------------------------------------------------------------------------- */

static ObjectAddress
role_address(Oid roleid)
{
	ObjectAddress addr;

	ObjectAddressSet(addr, AuthIdRelationId, roleid);
	return addr;
}

/*
 * Refuse a label the port would not understand, when it is set rather than
 * when it is read.  The limits are the user's to choose, but a key that is
 * not one of ours, or one outside the range Cloudberry accepts, is a mistake
 * worth reporting now.
 */
static void
gp_profile_check(const ObjectAddress *object, const char *seclabel)
{
	Jsonb	   *jb;
	JsonbIterator *it;
	JsonbIteratorToken tok;
	JsonbValue	v;
	const ProfileKey *k = NULL;
	char	   *key = NULL;

	if (seclabel == NULL)
		return;

	if (object->classId != AuthIdRelationId)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("a \"%s\" security label belongs on a role",
						GP_PROFILE_PROVIDER)));

	jb = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(seclabel)));

	if (!JB_ROOT_IS_OBJECT(jb))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a \"%s\" security label must be a JSON object",
						GP_PROFILE_PROVIDER),
				 errhint("Use %s.create_profile() rather than writing one by hand.",
						 GP_SECURITY_SCHEMA)));

	it = JsonbIteratorInit(&jb->root);
	while ((tok = JsonbIteratorNext(&it, &v, true)) != WJB_DONE)
	{
		if (tok == WJB_KEY)
		{
			key = pnstrdup(v.val.string.val, v.val.string.len);
			k = profile_key(key);
			if (k == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("unrecognized profile setting \"%s\"", key)));
			continue;
		}

		if (tok != WJB_VALUE || k == NULL)
			continue;

		if (k->is_number)
		{
			int			n;

			if (v.type != jbvNumeric)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("profile setting \"%s\" must be a number", key)));

			n = DatumGetInt32(DirectFunctionCall1(numeric_int4,
												  NumericGetDatum(v.val.numeric)));

			if (n < GP_PROFILE_UNLIMITED || n > GP_PROFILE_MAX_VALID)
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("profile setting \"%s\" must be between %d and %d",
								key, GP_PROFILE_UNLIMITED, GP_PROFILE_MAX_VALID)));
		}
		else if (v.type != jbvString)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("profile setting \"%s\" must be a name", key)));

		k = NULL;
	}
}

void
GpProfileRegisterProvider(void)
{
	register_label_provider(GP_PROFILE_PROVIDER, gp_profile_check);
}

/* ------------------------------------------------------------------------- */
/* Reading a profile                                                         */
/* ------------------------------------------------------------------------- */

bool
GpProfileIsProfileRole(Oid roleid)
{
	ObjectAddress addr = role_address(roleid);

	return GetSecurityLabel(&addr, GP_PROFILE_PROVIDER) != NULL;
}

/* Fill `out` from a profile role's label.  False when there is no such role. */
bool
GpProfileRead(const char *profile, GpProfile *out)
{
	Oid			roleid = get_role_oid(profile, true);
	ObjectAddress addr;
	char	   *label;
	Jsonb	   *jb;
	JsonbIterator *it;
	JsonbIteratorToken tok;
	JsonbValue	v;
	char	   *key = NULL;

	profile_init(out);

	if (!OidIsValid(roleid))
		return false;

	addr = role_address(roleid);
	label = GetSecurityLabel(&addr, GP_PROFILE_PROVIDER);
	if (label == NULL)
		return false;

	jb = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(label)));
	it = JsonbIteratorInit(&jb->root);

	while ((tok = JsonbIteratorNext(&it, &v, true)) != WJB_DONE)
	{
		if (tok == WJB_KEY)
		{
			key = pnstrdup(v.val.string.val, v.val.string.len);
			continue;
		}

		if (tok == WJB_VALUE && key != NULL)
		{
			profile_set_from_json(out, key, &v);
			key = NULL;
		}
	}

	return true;
}

char *
GpProfileNameForRole(Oid roleid)
{
	ObjectAddress addr = role_address(roleid);

	return GpLabelGet(&addr, GP_LABEL_profile);
}

bool
GpProfileForRole(Oid roleid, GpProfile *out)
{
	char	   *name;
	GpProfile	fallback;

	profile_init(out);

	if (!gp_enable_password_profile)
		return false;

	name = GpProfileNameForRole(roleid);
	if (name == NULL)
		return false;

	if (!GpProfileRead(name, out))
	{
		/*
		 * The profile role is gone but the role still points at it.  Refusing
		 * every login would be worse than falling back to the default, which
		 * is what a role with no profile of its own gets anyway.
		 */
		ereport(WARNING,
				(errmsg("role \"%s\" is under profile \"%s\", which does not exist",
						GetUserNameFromId(roleid, true), name)));
	}

	/* Anything the profile leaves at "default" comes from the default profile. */
	if (GpProfileRead(GP_DEFAULT_PROFILE, &fallback))
		profile_fill_defaults(out, &fallback);

	return true;
}

/* ------------------------------------------------------------------------- */
/* SQL                                                                       */
/* ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(gp_security_profile_setting);
PG_FUNCTION_INFO_V1(gp_security_role_profile);

/*
 * gp_security.profile_setting(profile name, setting text) -> integer
 *
 * One of a profile's numeric settings, after the default profile has filled
 * in whatever it left alone -- which is what a role under it will actually be
 * held to.
 */
Datum
gp_security_profile_setting(PG_FUNCTION_ARGS)
{
	Name		profile = PG_GETARG_NAME(0);
	char	   *setting = text_to_cstring(PG_GETARG_TEXT_PP(1));
	const ProfileKey *k = profile_key(setting);
	GpProfile	p;
	GpProfile	fallback;

	if (k == NULL || !k->is_number)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unrecognized profile setting \"%s\"", setting)));

	if (!GpProfileRead(NameStr(*profile), &p))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("profile \"%s\" does not exist", NameStr(*profile))));

	if (GpProfileRead(GP_DEFAULT_PROFILE, &fallback))
		profile_fill_defaults(&p, &fallback);

	PG_RETURN_INT32(*profile_int_field(&p, k));
}

/*
 * gp_security.role_profile(role name) -> name
 *
 * Which profile a role is under; what Cloudberry reads from
 * pg_authid.rolprofile.
 */
Datum
gp_security_role_profile(PG_FUNCTION_ARGS)
{
	Name		rolename = PG_GETARG_NAME(0);
	Oid			roleid = get_role_oid(NameStr(*rolename), true);
	char	   *profile;

	if (!OidIsValid(roleid))
		PG_RETURN_NULL();

	profile = GpProfileNameForRole(roleid);
	if (profile == NULL)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(DirectFunctionCall1(namein, CStringGetDatum(profile)));
}
