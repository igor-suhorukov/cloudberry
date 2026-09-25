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
 * option.c
 *	  The validator of gp_exttable_fdw's options, and who may make an
 *	  external table of which protocol.
 *
 * A role's right to make a readable or writable gpfdist table, or a readable
 * http one, is Cloudberry's CREATE ROLE ... CREATEEXTTABLE (pg_authid's
 * rolcreaterextgpfd and the rest).  The port keeps it in the role's
 * gp_exttable label (exttable_ddl.c): "gpfdist:r", "gpfdist:w", "http:r",
 * comma-separated.
 *
 * Cloudberry sources this file is made of:
 *	  gpcontrib/gp_exttable_fdw/option.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/reloptions.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_foreign_table.h"
#include "commands/defrem.h"
#include "commands/seclabel.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/regproc.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_exttable.h"

PG_FUNCTION_INFO_V1(gp_exttable_permission_check);

static void is_valid_rejectlimit(const char *reject_limit_type, int32 reject_limit);

/* The rights a label names, of the three there are. */
#define EXT_RIGHT_GPFDIST_R		0x01
#define EXT_RIGHT_GPFDIST_W		0x02
#define EXT_RIGHT_HTTP_R		0x04

static const struct
{
	const char *name;
	int			right;
}			ext_rights[] = {
	{"gpfdist:r", EXT_RIGHT_GPFDIST_R},
	{"gpfdist:w", EXT_RIGHT_GPFDIST_W},
	{"http:r", EXT_RIGHT_HTTP_R},
};

static int
label_rights(const char *label)
{
	int			rights = 0;
	char	   *copy;
	char	   *tok;
	char	   *save;

	if (label == NULL)
		return 0;
	copy = pstrdup(label);
	for (tok = strtok_r(copy, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save))
		for (int i = 0; i < lengthof(ext_rights); i++)
			if (strcmp(tok, ext_rights[i].name) == 0)
				rights |= ext_rights[i].right;
	pfree(copy);
	return rights;
}

/* Does the role's label grant it this protocol's right, r or w? */
bool		ExtRoleMay(Oid roleid, const char *protocol, char mode);

bool
ExtRoleMay(Oid roleid, const char *protocol, char mode)
{
	ObjectAddress addr;
	char		want[32];

	ObjectAddressSet(addr, AuthIdRelationId, roleid);
	snprintf(want, sizeof(want), "%s:%c", protocol, mode);
	return (label_rights(GetSecurityLabel(&addr, "gp_exttable")) &
			label_rights(want)) != 0;
}

/* The label provider: a comma-separated list of rights, and nothing else. */
static void
gp_exttable_label_check(const ObjectAddress *object, const char *seclabel)
{
	char	   *copy;
	char	   *tok;
	char	   *save;

	if (object->classId != AuthIdRelationId)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("gp_exttable labels a role alone")));
	if (seclabel == NULL)
		return;
	copy = pstrdup(seclabel);
	for (tok = strtok_r(copy, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save))
	{
		bool		known = false;

		for (int i = 0; i < lengthof(ext_rights); i++)
			known |= strcmp(tok, ext_rights[i].name) == 0;
		if (!known)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid gp_exttable label \"%s\"", seclabel)));
	}
}

void
ExtRegisterLabelProvider(void)
{
	register_label_provider("gp_exttable", gp_exttable_label_check);
}

/*
 * One [NO]CREATEEXTTABLE's key-value list, as Cloudberry's
 * TransformExttabAuthClause() completes and checks it: the right it names.
 */
static int
exttab_auth_right(const char *spec)
{
	char	   *type = NULL;
	char	   *protocol = NULL;
	char	   *copy = pstrdup(spec);
	char	   *tok;
	char	   *save;
	int			n = 0;
	static const char *const keys[] = {"type", "protocol"};
	static const char *const vals[] = {"readable", "writable", "gpfdist", "gpfdists", "http"};

	for (tok = strtok_r(copy, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save))
	{
		char	   *eq = strchr(tok, '=');
		char	   *key;
		char	   *val;
		bool		ok = false;

		if (eq == NULL)
			continue;
		*eq = '\0';
		key = tok;
		val = eq + 1;
		if (++n > 2)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid [NO]CREATEEXTTABLE specification. too many values")));
		for (int i = 0; i < lengthof(keys); i++)
			ok |= pg_strcasecmp(key, keys[i]) == 0;
		if (!ok)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid [NO]CREATEEXTTABLE option \"%s\"", key)));
		ok = false;
		for (int i = 0; i < lengthof(vals); i++)
			ok |= pg_strcasecmp(val, vals[i]) == 0;
		if (!ok)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid [NO]CREATEEXTTABLE option \"%s\"", val)));
		if (pg_strcasecmp(key, "type") == 0)
		{
			if (type != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("redundant option for \"%s\"", key)));
			if (pg_strcasecmp(val, "readable") != 0 && pg_strcasecmp(val, "writable") != 0)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("invalid %s value \"%s\"", key, val)));
			type = val;
		}
		else
		{
			if (protocol != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("redundant option for \"%s\"", key)));
			if (pg_strcasecmp(val, "gpfdist") != 0 && pg_strcasecmp(val, "gpfdists") != 0 &&
				pg_strcasecmp(val, "http") != 0)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("invalid %s value \"%s\"", key, val)));
			protocol = val;
		}
	}
	if (type == NULL)
		type = "readable";
	if (protocol == NULL)
		protocol = "gpfdist";

	if (pg_strcasecmp(protocol, "http") == 0)
		return pg_strcasecmp(type, "readable") == 0 ? EXT_RIGHT_HTTP_R : -1;
	return pg_strcasecmp(type, "readable") == 0 ? EXT_RIGHT_GPFDIST_R : EXT_RIGHT_GPFDIST_W;
}

/*
 * A role's CREATEEXTTABLE and NOCREATEEXTTABLE, as gp_sql's hook hands them
 * over once CREATE or ALTER ROLE has run: kept in the role's label, which
 * starts empty for a new role, as Cloudberry's SetCreateExtTableForRole()
 * sets its pg_authid columns.  The coordinator's alone: a segment takes the
 * tables the coordinator let the role make.
 */
PGDLLEXPORT void GpExtTableSetRoleAuth(Oid roleid, List *auth, bool is_create);

void
GpExtTableSetRoleAuth(Oid roleid, List *auth, bool is_create)
{
	ObjectAddress addr;
	int			rights;
	int			allowed = 0;
	bool		conflict = false;
	StringInfoData label;

	ObjectAddressSet(addr, AuthIdRelationId, roleid);
	rights = is_create ? 0 : label_rights(GetSecurityLabel(&addr, "gp_exttable"));

	/* the allowed first, then the disallowed, as Cloudberry takes them */
	foreach_node(DefElem, def, auth)
	{
		int			right;

		if (strcmp(def->defname, "exttabauth") != 0)
			continue;
		right = exttab_auth_right(defGetString(def));
		if (right < 0)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid CREATEEXTTABLE specification. writable http external tables do not exist")));
		rights |= right;
		allowed |= right;
	}
	foreach_node(DefElem, def, auth)
	{
		int			right;

		if (strcmp(def->defname, "exttabnoauth") != 0)
			continue;
		right = exttab_auth_right(defGetString(def));
		if (right < 0)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid NOCREATEEXTTABLE specification. writable http external tables do not exist")));
		if (allowed & right)
			conflict = true;
		rights &= ~right;
	}
	if (conflict)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("conflicting specifications in CREATEEXTTABLE and NOCREATEEXTTABLE")));

	initStringInfo(&label);
	for (int i = 0; i < lengthof(ext_rights); i++)
		if (rights & ext_rights[i].right)
			appendStringInfo(&label, "%s%s", label.len > 0 ? "," : "", ext_rights[i].name);
	SetSecurityLabel(&addr, "gp_exttable", label.len > 0 ? label.data : NULL);
}

Datum
gp_exttable_permission_check(PG_FUNCTION_ARGS)
{
	List	   *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			catalog = PG_GETARG_OID(1);
	bool		is_writable = false;
	bool		is_superuser = superuser();
	List	   *location_list = NIL;
	char	   *reject_limit_type = "r";
	int32		reject_limit = -1;
	bool		formattype_found = false;
	bool		locationuris_found = false;
	bool		command_found = false;
	bool		rejectlimit_found = false;

	foreach_node(DefElem, def, options_list)
	{
		if (pg_strcasecmp(def->defname, "is_writable") == 0)
			is_writable = defGetBoolean(def);
		else if (pg_strcasecmp(def->defname, "command") == 0)
		{
			command_found = true;
			if (!is_superuser)
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("must be superuser to create an EXECUTE external web table")));
		}
		else if (pg_strcasecmp(def->defname, "location_uris") == 0)
		{
			location_list = TokenizeLocationUris(defGetString(def));
			locationuris_found = true;
		}
		else if (pg_strcasecmp(def->defname, "format_type") == 0)
		{
			char	   *format = (char *) defGetString(def);

			if (pg_strcasecmp(format, "t") != 0 && pg_strcasecmp(format, "c") != 0 &&
				pg_strcasecmp(format, "b") != 0)
				ereport(ERROR,
						(errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
						 errmsg("format_type must be [t | c | b], t(text), c(csv), b(custom)")));
			formattype_found = true;
		}
		else if (pg_strcasecmp(def->defname, "reject_limit_type") == 0)
		{
			reject_limit_type = (char *) defGetString(def);
			if (pg_strcasecmp(reject_limit_type, "r") != 0 &&
				pg_strcasecmp(reject_limit_type, "p") != 0)
				ereport(ERROR,
						(errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
						 errmsg("reject_limit_type must be [r | p], r(ROW) or p(PERCENT)")));
		}
		else if (pg_strcasecmp(def->defname, "reject_limit") == 0)
		{
			reject_limit = atoi((char *) defGetString(def));
			rejectlimit_found = true;
		}
		else if (pg_strcasecmp(def->defname, "encoding") == 0)
		{
			char	   *encoding = (char *) defGetString(def);

			if (!PG_VALID_ENCODING(atoi(encoding)))
				ereport(ERROR,
						(errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
						 errmsg("%s is not a valid encoding code", encoding)));
		}
	}

	/* the wrapper and its server take no options; a table must say these */
	if (catalog != ForeignTableRelationId)
		PG_RETURN_VOID();

	if (!formattype_found)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("must specify format_type option([t | c | b], t(text), c(csv), b(custom))")));

	if (locationuris_found && command_found)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("location_uris and command options conflict with each other")));

	if (!locationuris_found && !command_found)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("must specify one of location_uris and command option")));

	/* a segment takes what the coordinator checked */
	if (!is_superuser && GpClusterBackendRole() != GP_ROLE_EXECUTE && location_list != NIL)
		(void) is_valid_locationuris(location_list, is_writable);

	if (rejectlimit_found)
	{
		if (is_writable)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("single row error handling may not be used with a writable external table")));
		is_valid_rejectlimit(reject_limit_type, reject_limit);
	}

	PG_RETURN_VOID();
}

/*
 * May a role that is no superuser make an external table of these locations?
 * Never of files; of gpfdist, gpfdists and http as its rights say; of a
 * protocol of the user's as its privileges on the protocol say.
 */
bool
is_valid_locationuris(List *location_list, bool is_writable)
{
	char	   *uri_str = pstrdup(strVal(linitial(location_list)));
	Uri		   *uri = ParseExternalTableUri(uri_str);
	Oid			roleid = GetUserId();

	if (uri->protocol == URI_FILE)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to create an external table with a file protocol")));
	else if ((uri->protocol == URI_GPFDIST || uri->protocol == URI_GPFDISTS) && is_writable)
	{
		if (!ExtRoleMay(roleid, "gpfdist", 'w'))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied: no privilege to create a writable gpfdist(s) external table")));
	}
	else if (uri->protocol == URI_GPFDIST || uri->protocol == URI_GPFDISTS)
	{
		if (!ExtRoleMay(roleid, "gpfdist", 'r'))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied: no privilege to create a readable gpfdist(s) external table")));
	}
	else if (uri->protocol == URI_HTTP && !is_writable)
	{
		if (!ExtRoleMay(roleid, "http", 'r'))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied: no privilege to create an http external table")));
	}
	else if (uri->protocol == URI_CUSTOM)
		ExtProtocolCheckUse(uri->customprotocol, is_writable);
	else
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("internal error in DefineExternalRelation"),
				 errdetail("Protocol is %d, writable is %d.",
						   uri->protocol, is_writable)));

	FreeExternalTableUri(uri);
	pfree(uri_str);
	return true;
}

static void
is_valid_rejectlimit(const char *reject_limit_type, int32 reject_limit)
{
	if (pg_strcasecmp(reject_limit_type, "r") == 0)
	{
		if (reject_limit < 2)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("segment reject limit in ROWS must be 2 or larger (got %d)",
							reject_limit)));
	}
	else if (pg_strcasecmp(reject_limit_type, "p") == 0)
	{
		if (reject_limit < 1 || reject_limit > 100)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("segment reject limit in PERCENT must be between 1 and 100 (got %d)",
							reject_limit)));
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
				 errmsg("reject_limit_type must be [r | p], r(ROW) or p(PERCENT)")));
}
