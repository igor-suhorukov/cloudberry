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
 * protocol.c
 *	  CREATE PROTOCOL and its kin: the protocols of the user's, which a
 *	  location of a scheme of their own names (url_custom.c).
 *
 * Cloudberry keeps them in a catalog of its own, pg_extprotocol.  The port
 * keeps them in gp_exttable.protocol, which pg_catalog.pg_extprotocol shows,
 * one table on each node, as a catalog is: the grammar makes each statement
 * a CALL of a procedure here (gp_desugar.c), which changes the coordinator's
 * table and sends the segments the same CALL, with the OID it chose, as
 * Cloudberry dispatches the statement with its OID.  A segment reads its own
 * table as a scan of a custom location opens it.
 *
 * What PostgreSQL's catalog code does for a catalog's object -- the
 * dependencies of an external table on its protocol, of a protocol on its
 * functions and its owner, for DROP ... CASCADE and DROP OWNED BY -- is
 * not done for a row of a table: DROP PROTOCOL finds the external tables
 * that name it by their locations instead.
 *
 * Nor does pg_dump write a row of an extension's table, where Cloudberry's
 * writes CREATE PROTOCOL.  So a protocol is also a label on each of its
 * functions, of the provider "gp_protocol", which pg_dump writes with the
 * function:
 *
 *	  {"demoprot": {"trusted": true, "readfunc": "public.read_from_file",
 *	                "owner": "alice", "acl": "{alice=ar/alice,bob=r/alice}"}}
 *
 * its functions, owner and privileges by name, as a restore makes them
 * again.  Each change of a protocol writes its functions' labels again, on
 * the coordinator or one node (protocol_label_refresh); and SECURITY LABEL
 * FOR gp_protocol, a superuser's, as a restore runs it, makes each protocol
 * it names that the database has not, once every function it names is
 * there -- whichever of them the dump writes last (gp_protocol_label_check)
 * -- with a new OID, before the external tables that name it, which the
 * dump writes after the functions; and sends the segments the call that
 * makes it, as CREATE PROTOCOL sends one.
 *
 * Cloudberry sources this file is made of:
 *	  src/backend/commands/extprotocolcmds.c, catalog/pg_extprotocol.c,
 *	  and the protocol's part of catalog/aclchk.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_func.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/formatting.h"
#include "utils/lsyscache.h"
#include "commands/seclabel.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "utils/regproc.h"
#include "utils/syscache.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_exttable.h"

PG_FUNCTION_INFO_V1(gp_exttable_create_protocol);
PG_FUNCTION_INFO_V1(gp_exttable_drop_protocol);
PG_FUNCTION_INFO_V1(gp_exttable_rename_protocol);
PG_FUNCTION_INFO_V1(gp_exttable_alter_protocol_owner);
PG_FUNCTION_INFO_V1(gp_exttable_grant_protocol);
PG_FUNCTION_INFO_V1(gp_exttable_restore_protocol);

/* the label a protocol's definition is kept in, on its function */
#define PROTOCOL_LABEL_PROVIDER	"gp_protocol"

/* the privileges a protocol has, Cloudberry's ACL_ALL_RIGHTS_EXTPROTOCOL */
#define PROTOCOL_ALL_RIGHTS		(ACL_SELECT | ACL_INSERT)

/* a protocol, as its row has it */
typedef struct Protocol
{
	Oid			oid;
	char	   *name;
	Oid			readfn;
	Oid			writefn;
	Oid			validatorfn;
	Oid			owner;
	bool		trusted;
	char	   *acl_text;		/* NULL: the owner's default */
} Protocol;

typedef enum ProtocolFuncType
{
	PROTOCOL_READER,
	PROTOCOL_WRITER,
	PROTOCOL_VALIDATOR,
} ProtocolFuncType;

static const char *
func_type_to_name(ProtocolFuncType ftype)
{
	switch (ftype)
	{
		case PROTOCOL_READER:
			return "read";
		case PROTOCOL_WRITER:
			return "write";
		case PROTOCOL_VALIDATOR:
			return "validator";
	}
	return "unknown";
}

/* Is this the coordinator of a cluster, which sends the segments the call? */
static bool
dispatching(void)
{
	return !GpClusterIsSingleNode() && GpClusterBackendRole() == GP_ROLE_DISPATCH;
}

/* gp_exttable.protocol's columns */
enum
{
	Anum_protocol_oid = 1,
	Anum_protocol_ptcname,
	Anum_protocol_ptcreadfn,
	Anum_protocol_ptcwritefn,
	Anum_protocol_ptcvalidatorfn,
	Anum_protocol_ptcowner,
	Anum_protocol_ptctrusted,
	Anum_protocol_ptcacl,
};

/*
 * The protocol of this name, from this node's table, into *p: false if there
 * is none.  Read as a catalog is read, a scan of the table and no query: a
 * segment reads it where a scan of a custom location opens it, which is in
 * a slice, where a query of a function's may not read a table.
 */
static Relation
protocol_table_open(void)
{
	Oid			relid = get_relname_relid("protocol", get_namespace_oid("gp_exttable", false));

	if (!OidIsValid(relid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("protocols need the gp_exttable extension")));
	return table_open(relid, AccessShareLock);
}

/* A row of the table, into *p. */
static void
protocol_from_tuple(HeapTuple tup, TupleDesc desc, Protocol *p)
{
	bool		isnull;
	Datum		d;

	p->name = pstrdup(NameStr(*DatumGetName(heap_getattr(tup, Anum_protocol_ptcname, desc, &isnull))));
	p->oid = DatumGetObjectId(heap_getattr(tup, Anum_protocol_oid, desc, &isnull));
	d = heap_getattr(tup, Anum_protocol_ptcreadfn, desc, &isnull);
	p->readfn = isnull ? InvalidOid : DatumGetObjectId(d);
	d = heap_getattr(tup, Anum_protocol_ptcwritefn, desc, &isnull);
	p->writefn = isnull ? InvalidOid : DatumGetObjectId(d);
	d = heap_getattr(tup, Anum_protocol_ptcvalidatorfn, desc, &isnull);
	p->validatorfn = isnull ? InvalidOid : DatumGetObjectId(d);
	p->owner = DatumGetObjectId(heap_getattr(tup, Anum_protocol_ptcowner, desc, &isnull));
	p->trusted = DatumGetBool(heap_getattr(tup, Anum_protocol_ptctrusted, desc, &isnull));
	d = heap_getattr(tup, Anum_protocol_ptcacl, desc, &isnull);
	p->acl_text = isnull ? NULL :
		DatumGetCString(OidFunctionCall1(F_ARRAY_OUT, d));
}

static bool
protocol_get(const char *name, Protocol *p)
{
	Relation	rel = protocol_table_open();
	TableScanDesc scan;
	HeapTuple	tup;
	bool		found = false;

	scan = table_beginscan_catalog(rel, 0, NULL);
	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		bool		isnull;
		Datum		d = heap_getattr(tup, Anum_protocol_ptcname, RelationGetDescr(rel), &isnull);

		if (isnull || strcmp(NameStr(*DatumGetName(d)), name) != 0)
			continue;
		protocol_from_tuple(tup, RelationGetDescr(rel), p);
		found = true;
		break;
	}
	table_endscan(scan);
	table_close(rel, AccessShareLock);
	return found;
}

/* The function of a protocol, as url_custom.c opens a location of it. */
Oid
ExtProtocolFunction(const char *name, bool iswritable, bool *exists)
{
	Protocol	p;

	*exists = protocol_get(name, &p);
	if (!*exists)
		return InvalidOid;
	return iswritable ? p.writefn : p.readfn;
}

static void
protocol_must_exist(const char *name, Protocol *p)
{
	if (!protocol_get(name, p))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("protocol \"%s\" does not exist", name)));
}

static void protocol_label_refresh(Oid fnoid);

/* The labels of a protocol's functions, each of which carries it. */
static void
protocol_labels_refresh(const Protocol *p)
{
	protocol_label_refresh(p->readfn);
	if (p->writefn != p->readfn)
		protocol_label_refresh(p->writefn);
	if (p->validatorfn != p->readfn && p->validatorfn != p->writefn)
		protocol_label_refresh(p->validatorfn);
}

/* A function by its schema and name, as the label keeps it. */
static char *
protocol_function_name(Oid fnoid)
{
	return quote_qualified_identifier(get_namespace_name(get_func_namespace(fnoid)),
									  get_func_name(fnoid));
}

static void
protocol_label_member(StringInfo buf, const char *key, const char *value, bool *first)
{
	if (value == NULL)
		return;
	appendStringInfoString(buf, *first ? "" : ", ");
	escape_json(buf, key);
	appendStringInfoString(buf, ": ");
	escape_json(buf, value);
	*first = false;
}

/*
 * The "gp_protocol" label of a function again, from the protocols of this
 * node's table it is a function of: none, and it has none.  On the
 * coordinator or one node, whose labels pg_dump reads; a segment's table
 * changes by the coordinator's calls.
 */
static void
protocol_label_refresh(Oid fnoid)
{
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	StringInfoData buf;
	bool		any = false;
	ObjectAddress addr;

	if (!OidIsValid(fnoid) || GpClusterBackendRole() == GP_ROLE_EXECUTE ||
		!SearchSysCacheExists1(PROCOID, ObjectIdGetDatum(fnoid)))
		return;

	initStringInfo(&buf);
	appendStringInfoChar(&buf, '{');
	rel = protocol_table_open();
	scan = table_beginscan_catalog(rel, 0, NULL);
	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Protocol	p;
		bool		first;

		protocol_from_tuple(tup, RelationGetDescr(rel), &p);
		if (p.readfn != fnoid && p.writefn != fnoid && p.validatorfn != fnoid)
			continue;
		appendStringInfoString(&buf, any ? ", " : "");
		escape_json(&buf, p.name);
		appendStringInfoString(&buf, ": {");
		appendStringInfo(&buf, "\"trusted\": %s", p.trusted ? "true" : "false");
		first = false;
		protocol_label_member(&buf, "readfunc",
							  OidIsValid(p.readfn) ? protocol_function_name(p.readfn) : NULL, &first);
		protocol_label_member(&buf, "writefunc",
							  OidIsValid(p.writefn) ? protocol_function_name(p.writefn) : NULL, &first);
		protocol_label_member(&buf, "validatorfunc",
							  OidIsValid(p.validatorfn) ? protocol_function_name(p.validatorfn) : NULL, &first);
		protocol_label_member(&buf, "owner", GetUserNameFromId(p.owner, false), &first);
		protocol_label_member(&buf, "acl", p.acl_text, &first);
		appendStringInfoChar(&buf, '}');
		any = true;
	}
	table_endscan(scan);
	table_close(rel, AccessShareLock);
	appendStringInfoChar(&buf, '}');

	ObjectAddressSet(addr, ProcedureRelationId, fnoid);
	SetSecurityLabel(&addr, PROTOCOL_LABEL_PROVIDER, any ? buf.data : NULL);
}

/*
 * A change of this node's table, with its arguments.  The table is the
 * extension's, which no user writes but through these procedures, whose
 * callers' rights are checked here: the change is made as the bootstrap
 * superuser, as PostgreSQL's foreign keys check a row as the table's owner.
 */
static void
protocol_exec(const char *sql, int nargs, Oid *argtypes, Datum *values,
			  const char *nulls)
{
	Oid			save_userid;
	int			save_sec_context;

	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID,
						   save_sec_context | SECURITY_LOCAL_USERID_CHANGE |
						   SECURITY_RESTRICTED_OPERATION);
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	if (SPI_execute_with_args(sql, nargs, argtypes, values, nulls, false, 0) < 0)
		elog(ERROR, "could not change gp_exttable.protocol");
	SPI_finish();
	SetUserIdAndSecContext(save_userid, save_sec_context);
	CommandCounterIncrement();
}

/* A role as GRANT and OWNER TO name it: a name, or CURRENT_USER and its kin. */
static Oid
role_oid(const char *name)
{
	if (strcmp(name, "current_user") == 0 || strcmp(name, "current_role") == 0)
		return GetUserId();
	if (strcmp(name, "session_user") == 0)
		return GetSessionUserId();
	return get_role_oid(name, false);
}

/* The owner's own, or a superuser: may they change the protocol? */
static void
protocol_check_owner(const Protocol *p)
{
	if (!superuser() && !has_privs_of_role(GetUserId(), p->owner))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be owner of protocol %s", p->name)));
}

/*
 * A protocol's function, checked as Cloudberry's ValidateProtocolFunction()
 * checks it: none of arguments, returning an integer (void for the
 * validator), neither a set nor IMMUTABLE, and one its creator may call.
 */
static Oid
validate_protocol_function(List *fnName, ProtocolFuncType fntype)
{
	Oid			fnOid;
	bool		retset;
	Oid		   *true_oid_array;
	Oid			actual_rettype;
	Oid			desired_rettype = (fntype == PROTOCOL_VALIDATOR) ? VOIDOID : INT4OID;
	FuncDetailCode fdresult;
	AclResult	aclresult;
	Oid			inputTypes[1] = {InvalidOid};
	int			nvargs;
	Oid			vatype;
	int			fgc_flags;

	fdresult = func_get_detail(fnName, NIL, NIL, 0, inputTypes, false, false,
							   false, &fgc_flags, &fnOid, &actual_rettype, &retset,
							   &nvargs, &vatype, &true_oid_array, NULL);

	if (fdresult != FUNCDETAIL_NORMAL || !OidIsValid(fnOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(fnName, 0, NIL, inputTypes))));
	if (OidIsValid(vatype))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("Invalid protocol function"),
				 errdetail("Protocol functions cannot be variadic.")));
	if (retset)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("Invalid protocol function"),
				 errdetail("Protocol functions cannot return sets.")));
	if (actual_rettype != desired_rettype)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("%s protocol function %s must return %s",
						func_type_to_name(fntype),
						func_signature_string(fnName, 0, NIL, inputTypes),
						(fntype == PROTOCOL_VALIDATOR ? "void" : "an integer"))));
	if (func_volatile(fnOid) == PROVOLATILE_IMMUTABLE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("%s protocol function %s is declared IMMUTABLE",
						func_type_to_name(fntype),
						func_signature_string(fnName, 0, NIL, inputTypes)),
				 errhint("PROTOCOL functions must be declared STABLE or VOLATILE")));

	aclresult = object_aclcheck(ProcedureRelationId, fnOid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION, get_func_name(fnOid));

	return fnOid;
}

static Datum
oid_or_null(Oid oid, char *null)
{
	*null = OidIsValid(oid) ? ' ' : 'n';
	return ObjectIdGetDatum(oid);
}

/*
 * CALL gp_exttable.create_protocol(name, trusted, definition, oid): CREATE
 * [TRUSTED] PROTOCOL name (readfunc = ..., writefunc = ..., validatorfunc =
 * ...), the definition as the grammar gives it, name and value in turn.
 * The OID is the coordinator's, on a segment; 0 on the coordinator.
 */
Datum
gp_exttable_create_protocol(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	bool		trusted = PG_GETARG_BOOL(1);
	ArrayType  *defarr = PG_GETARG_ARRAYTYPE_P(2);
	Oid			oid = PG_ARGISNULL(3) ? InvalidOid : PG_GETARG_OID(3);
	Datum	   *defs;
	int			ndefs;
	List	   *readfuncName = NIL;
	List	   *writefuncName = NIL;
	List	   *validatorfuncName = NIL;
	Oid			readfn = InvalidOid;
	Oid			writefn = InvalidOid;
	Oid			validatorfn = InvalidOid;
	Protocol	existing;
	Oid			argtypes[7] = {OIDOID, TEXTOID, OIDOID, OIDOID, OIDOID, OIDOID, BOOLOID};
	Datum		values[7];
	char		nulls[8] = "       ";

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to create an external protocol")));

	deconstruct_array_builtin(defarr, TEXTOID, &defs, NULL, &ndefs);
	for (int i = 0; i + 1 < ndefs; i += 2)
	{
		char	   *attr = TextDatumGetCString(defs[i]);
		List	   *fn = stringToQualifiedNameList(TextDatumGetCString(defs[i + 1]), NULL);

		if (pg_strcasecmp(attr, "readfunc") == 0)
			readfuncName = fn;
		else if (pg_strcasecmp(attr, "writefunc") == 0)
			writefuncName = fn;
		else if (pg_strcasecmp(attr, "validatorfunc") == 0)
			validatorfuncName = fn;
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("protocol attribute \"%s\" not recognized", attr)));
	}
	if (readfuncName == NIL && writefuncName == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("protocol must be specify at least a readfunc or a writefunc")));

	/* the built-in schemes, which no protocol of the user's may be */
	if (pg_strcasecmp(name, "file") == 0 || pg_strcasecmp(name, "http") == 0 ||
		pg_strcasecmp(name, "gpfdist") == 0 || pg_strcasecmp(name, "gpfdists") == 0)
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("protocol \"%s\" already exists", name),
				 errhint("pick a different protocol name")));
	if (protocol_get(name, &existing))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("protocol \"%s\" already exists", name)));

	if (readfuncName)
		readfn = validate_protocol_function(readfuncName, PROTOCOL_READER);
	if (writefuncName)
		writefn = validate_protocol_function(writefuncName, PROTOCOL_WRITER);
	if (validatorfuncName)
		validatorfn = validate_protocol_function(validatorfuncName, PROTOCOL_VALIDATOR);

	if (!OidIsValid(oid))
		oid = GetNewObjectId();
	values[0] = ObjectIdGetDatum(oid);
	values[1] = CStringGetTextDatum(name);
	values[2] = oid_or_null(readfn, &nulls[2]);
	values[3] = oid_or_null(writefn, &nulls[3]);
	values[4] = oid_or_null(validatorfn, &nulls[4]);
	values[5] = ObjectIdGetDatum(GetUserId());
	values[6] = BoolGetDatum(trusted);
	protocol_exec("INSERT INTO gp_exttable.protocol"
				  " (oid, ptcname, ptcreadfn, ptcwritefn, ptcvalidatorfn, ptcowner, ptctrusted)"
				  " VALUES ($1, $2::name, $3, $4, $5, $6, $7)",
				  7, argtypes, values, nulls);
	protocol_label_refresh(readfn);
	if (writefn != readfn)
		protocol_label_refresh(writefn);
	if (validatorfn != readfn && validatorfn != writefn)
		protocol_label_refresh(validatorfn);

	if (dispatching())
		GpDispatchCommand(psprintf("CALL gp_exttable.create_protocol(%s, %s, %s, %u)",
								   quote_literal_cstr(name), trusted ? "true" : "false",
								   quote_literal_cstr(DatumGetCString(OidFunctionCall1(F_ARRAY_OUT,
																							  PointerGetDatum(defarr)))),
								   oid));
	PG_RETURN_VOID();
}

/*
 * The external tables of a protocol: foreign tables of gp_exttable_server
 * whose locations are of its scheme.
 */
static List *
protocol_tables(const char *name)
{
	Oid			argtypes[1] = {TEXTOID};
	Datum		values[1];
	List	   *relids = NIL;
	MemoryContext caller = CurrentMemoryContext;

	values[0] = CStringGetTextDatum(psprintf("location_uris=%s://%%", name));
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	if (SPI_execute_with_args("SELECT ft.ftrelid FROM pg_catalog.pg_foreign_table ft"
							  " JOIN pg_catalog.pg_foreign_server s ON s.oid = ft.ftserver"
							  " WHERE s.srvname = 'gp_exttable_server'"
							  " AND EXISTS (SELECT 1 FROM unnest(ft.ftoptions) o WHERE o LIKE $1)",
							  1, argtypes, values, NULL, true, 0) != SPI_OK_SELECT)
		elog(ERROR, "could not read pg_foreign_table");
	for (uint64 i = 0; i < SPI_processed; i++)
	{
		bool		isnull;
		MemoryContext old = MemoryContextSwitchTo(caller);

		/* in the caller's memory, which outlasts SPI's */
		relids = lappend_oid(relids,
							 DatumGetObjectId(SPI_getbinval(SPI_tuptable->vals[i],
															SPI_tuptable->tupdesc,
															1, &isnull)));
		MemoryContextSwitchTo(old);
	}
	SPI_finish();
	return relids;
}

/*
 * CALL gp_exttable.drop_protocol(names, if_exists, cascade): DROP PROTOCOL
 * [IF EXISTS] name, ... [CASCADE | RESTRICT].  The external tables that
 * read or write through one go with it under CASCADE, and keep it
 * otherwise, as its dependencies would in Cloudberry.
 */
Datum
gp_exttable_drop_protocol(PG_FUNCTION_ARGS)
{
	ArrayType  *namearr = PG_GETARG_ARRAYTYPE_P(0);
	bool		if_exists = PG_GETARG_BOOL(1);
	bool		cascade = PG_GETARG_BOOL(2);
	Datum	   *names;
	int			nnames;

	deconstruct_array_builtin(namearr, TEXTOID, &names, NULL, &nnames);
	for (int i = 0; i < nnames; i++)
	{
		char	   *name = TextDatumGetCString(names[i]);
		Protocol	p;
		List	   *tables;
		Oid			argtypes[1] = {OIDOID};
		Datum		values[1];

		if (!protocol_get(name, &p))
		{
			if (!if_exists)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("protocol \"%s\" does not exist", name)));
			/* said once, by the coordinator, not by each segment too */
			if (GpClusterBackendRole() != GP_ROLE_EXECUTE)
				ereport(NOTICE,
						(errmsg("protocol \"%s\" does not exist, skipping", name)));
			continue;
		}
		protocol_check_owner(&p);

		tables = protocol_tables(name);
		if (tables != NIL && !cascade)
			ereport(ERROR,
					(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
					 errmsg("cannot drop protocol %s because other objects depend on it", name),
					 errdetail("external table %s depends on protocol %s",
							   get_rel_name(linitial_oid(tables)), name),
					 errhint("Use DROP ... CASCADE to drop the dependent objects too.")));
		foreach_oid(relid, tables)
		{
			ObjectAddress addr;

			if (GpClusterBackendRole() != GP_ROLE_EXECUTE)
				ereport(NOTICE,
						(errmsg("drop cascades to external table %s", get_rel_name(relid))));
			ObjectAddressSet(addr, RelationRelationId, relid);
			performDeletion(&addr, DROP_CASCADE, 0);
		}

		values[0] = ObjectIdGetDatum(p.oid);
		protocol_exec("DELETE FROM gp_exttable.protocol WHERE oid = $1",
					  1, argtypes, values, NULL);
		protocol_labels_refresh(&p);
	}

	if (dispatching())
		GpDispatchCommand(psprintf("CALL gp_exttable.drop_protocol(%s, %s, %s)",
								   quote_literal_cstr(DatumGetCString(OidFunctionCall1(F_ARRAY_OUT,
																							  PointerGetDatum(namearr)))),
								   if_exists ? "true" : "false",
								   cascade ? "true" : "false"));
	PG_RETURN_VOID();
}

/* CALL gp_exttable.rename_protocol(name, newname): ALTER PROTOCOL ... RENAME TO */
Datum
gp_exttable_rename_protocol(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *newname = text_to_cstring(PG_GETARG_TEXT_PP(1));
	Protocol	p;
	Protocol	other;
	Oid			argtypes[2] = {OIDOID, TEXTOID};
	Datum		values[2];

	protocol_must_exist(name, &p);
	protocol_check_owner(&p);
	if (protocol_get(newname, &other))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("protocol \"%s\" already exists", newname)));

	values[0] = ObjectIdGetDatum(p.oid);
	values[1] = CStringGetTextDatum(newname);
	protocol_exec("UPDATE gp_exttable.protocol SET ptcname = $2::name WHERE oid = $1",
				  2, argtypes, values, NULL);
	protocol_labels_refresh(&p);

	if (dispatching())
		GpDispatchCommand(psprintf("CALL gp_exttable.rename_protocol(%s, %s)",
								   quote_literal_cstr(name), quote_literal_cstr(newname)));
	PG_RETURN_VOID();
}

/*
 * CALL gp_exttable.alter_protocol_owner(name, owner): ALTER PROTOCOL ...
 * OWNER TO.  An untrusted protocol is a superuser's to use, and is given to
 * no one else, as Cloudberry refuses it.
 */
Datum
gp_exttable_alter_protocol_owner(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *rolename = text_to_cstring(PG_GETARG_TEXT_PP(1));
	Oid			newowner = role_oid(rolename);
	Protocol	p;
	Oid			argtypes[2] = {OIDOID, OIDOID};
	Datum		values[2];

	protocol_must_exist(name, &p);
	if (p.owner != newowner)
	{
		protocol_check_owner(&p);
		if (!p.trusted && !superuser_arg(newowner))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("untrusted protocol \"%s\" can't be owned by non superuser",
							name)));
		if (!superuser())
			check_can_set_role(GetUserId(), newowner);
		values[0] = ObjectIdGetDatum(p.oid);
		values[1] = ObjectIdGetDatum(newowner);
		protocol_exec("UPDATE gp_exttable.protocol SET ptcowner = $2 WHERE oid = $1",
					  2, argtypes, values, NULL);
		protocol_labels_refresh(&p);
	}

	if (dispatching())
		GpDispatchCommand(psprintf("CALL gp_exttable.alter_protocol_owner(%s, %s)",
								   quote_literal_cstr(name), quote_literal_cstr(rolename)));
	PG_RETURN_VOID();
}

/* A protocol's ACL, or the owner's default where it has none. */
static Acl *
protocol_acl(const Protocol *p)
{
	AclItem		item;

	if (p->acl_text != NULL)
		return DatumGetAclPCopy(OidFunctionCall3(F_ARRAY_IN,
													CStringGetDatum(p->acl_text),
													ObjectIdGetDatum(ACLITEMOID),
													Int32GetDatum(-1)));
	item.ai_grantee = p->owner;
	item.ai_grantor = p->owner;
	ACLITEM_SET_PRIVS_GOPTIONS(item, PROTOCOL_ALL_RIGHTS, ACL_NO_RIGHTS);
	return aclupdate(make_empty_acl(), &item, ACL_MODECHG_ADD, p->owner, DROP_RESTRICT);
}

/*
 * CALL gp_exttable.grant_protocol(is_grant, privileges, names, grantees,
 * grant_option, cascade): GRANT SELECT | INSERT | ALL ON PROTOCOL ... TO
 * ..., and REVOKE, as Cloudberry's ExecGrant_ExtProtocol() carries them out.
 */
Datum
gp_exttable_grant_protocol(PG_FUNCTION_ARGS)
{
	bool		is_grant = PG_GETARG_BOOL(0);
	ArrayType  *privarr = PG_GETARG_ARRAYTYPE_P(1);
	ArrayType  *namearr = PG_GETARG_ARRAYTYPE_P(2);
	ArrayType  *granteearr = PG_GETARG_ARRAYTYPE_P(3);
	bool		grant_option = PG_GETARG_BOOL(4);
	bool		cascade = PG_GETARG_BOOL(5);
	Datum	   *privs;
	Datum	   *names;
	Datum	   *grantees;
	int			nprivs;
	int			nnames;
	int			ngrantees;
	AclMode		privileges = ACL_NO_RIGHTS;
	bool		all_privs = false;
	List	   *granteeids = NIL;

	deconstruct_array_builtin(privarr, TEXTOID, &privs, NULL, &nprivs);
	deconstruct_array_builtin(namearr, TEXTOID, &names, NULL, &nnames);
	deconstruct_array_builtin(granteearr, TEXTOID, &grantees, NULL, &ngrantees);

	for (int i = 0; i < nprivs; i++)
	{
		char	   *priv = TextDatumGetCString(privs[i]);

		if (pg_strcasecmp(priv, "all") == 0)
		{
			privileges |= PROTOCOL_ALL_RIGHTS;
			all_privs = true;
		}
		else if (pg_strcasecmp(priv, "select") == 0)
			privileges |= ACL_SELECT;
		else if (pg_strcasecmp(priv, "insert") == 0)
			privileges |= ACL_INSERT;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_GRANT_OPERATION),
					 errmsg("invalid privilege type %s for external protocol",
							asc_toupper(priv, strlen(priv)))));
	}
	for (int i = 0; i < ngrantees; i++)
	{
		char	   *grantee = TextDatumGetCString(grantees[i]);

		if (pg_strcasecmp(grantee, "public") == 0)
			granteeids = lappend_oid(granteeids, ACL_ID_PUBLIC);
		else
			granteeids = lappend_oid(granteeids, role_oid(grantee));
	}

	for (int i = 0; i < nnames; i++)
	{
		char	   *name = TextDatumGetCString(names[i]);
		Protocol	p;
		Acl		   *acl;
		Oid			grantorId;
		AclMode		avail_goptions;
		AclMode		this_privileges;
		Oid			argtypes[2] = {OIDOID, TEXTOID};
		Datum		values[2];

		protocol_must_exist(name, &p);
		if (!p.trusted)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("protocol \"%s\" is not trusted", name),
					 errhint("Only superusers may use untrusted protocols.")));

		acl = protocol_acl(&p);
		select_best_grantor(NULL, privileges, acl, p.owner, &grantorId,
							&avail_goptions);

		/*
		 * What the grantor may grant, as PostgreSQL's
		 * restrict_and_check_grant() allows it: none at all, without even a
		 * privilege to grant, is refused.
		 */
		if (avail_goptions == ACL_NO_RIGHTS &&
			aclmask(acl, grantorId, p.owner,
					PROTOCOL_ALL_RIGHTS | ACL_GRANT_OPTION_FOR(PROTOCOL_ALL_RIGHTS),
					ACLMASK_ANY) == ACL_NO_RIGHTS)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied for external protocol %s", name)));
		this_privileges = privileges & ACL_OPTION_TO_PRIVS(avail_goptions);
		if (this_privileges == ACL_NO_RIGHTS)
			ereport(WARNING,
					(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_GRANTED),
					 is_grant ? errmsg("no privileges were granted for \"%s\"", name) :
					 errmsg("no privileges could be revoked for \"%s\"", name)));
		else if (!all_privs && this_privileges != privileges)
			ereport(WARNING,
					(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_GRANTED),
					 is_grant ? errmsg("not all privileges were granted for \"%s\"", name) :
					 errmsg("not all privileges could be revoked for \"%s\"", name)));

		foreach_oid(grantee, granteeids)
		{
			AclItem		item;

			item.ai_grantee = grantee;
			item.ai_grantor = grantorId;
			ACLITEM_SET_PRIVS_GOPTIONS(item,
									   (is_grant || !grant_option) ? this_privileges : ACL_NO_RIGHTS,
									   (!is_grant || grant_option) ? this_privileges : ACL_NO_RIGHTS);
			acl = aclupdate(acl, &item, is_grant ? ACL_MODECHG_ADD : ACL_MODECHG_DEL,
							p.owner, cascade ? DROP_CASCADE : DROP_RESTRICT);
		}

		values[0] = ObjectIdGetDatum(p.oid);
		values[1] = DirectFunctionCall1(textin,
										OidFunctionCall1(F_ARRAY_OUT, PointerGetDatum(acl)));
		protocol_exec("UPDATE gp_exttable.protocol SET ptcacl = $2::aclitem[] WHERE oid = $1",
					  2, argtypes, values, NULL);
		protocol_labels_refresh(&p);
	}

	if (dispatching())
	{
		GpDispatchCommand(psprintf("CALL gp_exttable.grant_protocol(%s, %s, %s, %s, %s, %s)",
								   is_grant ? "true" : "false",
								   quote_literal_cstr(DatumGetCString(OidFunctionCall1(F_ARRAY_OUT, PointerGetDatum(privarr)))),
								   quote_literal_cstr(DatumGetCString(OidFunctionCall1(F_ARRAY_OUT, PointerGetDatum(namearr)))),
								   quote_literal_cstr(DatumGetCString(OidFunctionCall1(F_ARRAY_OUT, PointerGetDatum(granteearr)))),
								   grant_option ? "true" : "false",
								   cascade ? "true" : "false"));
	}
	PG_RETURN_VOID();
}

/*
 * May the user make an external table of this protocol -- readable, or
 * writable?  Its owner may, and a superuser; anyone else by the privilege
 * the protocol's ACL gives them, SELECT or INSERT, as Cloudberry checks it
 * (gp_exttable_fdw's option.c, pg_extprotocol_aclcheck()).
 */
void
ExtProtocolCheckUse(const char *name, bool iswritable)
{
	Protocol	p;
	AclMode		mode = iswritable ? ACL_INSERT : ACL_SELECT;

	protocol_must_exist(name, &p);
	if (superuser() || has_privs_of_role(GetUserId(), p.owner))
		return;
	if (!p.trusted)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for external protocol %s", name)));
	if (aclmask(protocol_acl(&p), GetUserId(), p.owner, mode, ACLMASK_ANY) == ACL_NO_RIGHTS)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for external protocol %s", name)));
}

/* The protocol's OID, or InvalidOid. */
Oid
ExtProtocolOid(const char *name, bool missing_ok)
{
	Protocol	p;

	if (protocol_get(name, &p))
		return p.oid;
	if (!missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("protocol \"%s\" does not exist", name)));
	return InvalidOid;
}

/*
 * A protocol as its label has it (gp_protocol_label_check), made in this
 * node's table: its functions by name, checked as CREATE PROTOCOL checks
 * them, its owner and privileges by name, and the OID given, or a new one.
 * On the coordinator, the segments are sent the call that makes it there,
 * with the OID.
 */
static void
protocol_restore(const char *name, bool trusted, const char *readfunc,
				 const char *writefunc, const char *validatorfunc,
				 const char *owner, const char *acl, Oid oid)
{
	Protocol	existing;
	Oid			readfn = InvalidOid;
	Oid			writefn = InvalidOid;
	Oid			validatorfn = InvalidOid;
	Oid			argtypes[8] = {OIDOID, TEXTOID, OIDOID, OIDOID, OIDOID, OIDOID, BOOLOID, TEXTOID};
	Datum		values[8];
	char		nulls[9] = "        ";

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to create an external protocol")));
	if (protocol_get(name, &existing))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("protocol \"%s\" already exists", name)));
	if (readfunc != NULL)
		readfn = validate_protocol_function(stringToQualifiedNameList(readfunc, NULL),
											PROTOCOL_READER);
	if (writefunc != NULL)
		writefn = validate_protocol_function(stringToQualifiedNameList(writefunc, NULL),
											 PROTOCOL_WRITER);
	if (validatorfunc != NULL)
		validatorfn = validate_protocol_function(stringToQualifiedNameList(validatorfunc, NULL),
												 PROTOCOL_VALIDATOR);
	if (!OidIsValid(readfn) && !OidIsValid(writefn))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("protocol must be specify at least a readfunc or a writefunc")));

	if (!OidIsValid(oid))
		oid = GetNewObjectId();
	values[0] = ObjectIdGetDatum(oid);
	values[1] = CStringGetTextDatum(name);
	values[2] = oid_or_null(readfn, &nulls[2]);
	values[3] = oid_or_null(writefn, &nulls[3]);
	values[4] = oid_or_null(validatorfn, &nulls[4]);
	values[5] = ObjectIdGetDatum(get_role_oid(owner, false));
	values[6] = BoolGetDatum(trusted);
	values[7] = acl != NULL ? CStringGetTextDatum(acl) : (Datum) 0;
	nulls[7] = acl != NULL ? ' ' : 'n';
	protocol_exec("INSERT INTO gp_exttable.protocol"
				  " (oid, ptcname, ptcreadfn, ptcwritefn, ptcvalidatorfn, ptcowner, ptctrusted, ptcacl)"
				  " VALUES ($1, $2::name, $3, $4, $5, $6, $7, $8::aclitem[])",
				  8, argtypes, values, nulls);

	if (dispatching())
	{
#define LITERAL_OR_NULL(x) ((x) != NULL ? quote_literal_cstr(x) : "NULL")
		GpDispatchCommand(psprintf("CALL gp_exttable.restore_protocol(%s, %s, %s, %s, %s, %s, %s, %u)",
								   quote_literal_cstr(name), trusted ? "true" : "false",
								   LITERAL_OR_NULL(readfunc), LITERAL_OR_NULL(writefunc),
								   LITERAL_OR_NULL(validatorfunc),
								   quote_literal_cstr(owner), LITERAL_OR_NULL(acl), oid));
#undef LITERAL_OR_NULL
	}
}

static char *
text_arg_or_null(FunctionCallInfo fcinfo, int n)
{
	return PG_ARGISNULL(n) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(n));
}

/*
 * CALL gp_exttable.restore_protocol(name, trusted, readfunc, writefunc,
 * validatorfunc, owner, acl, oid): what a gp_protocol label restored on the
 * coordinator sends a segment, with the coordinator's OID.
 */
Datum
gp_exttable_restore_protocol(PG_FUNCTION_ARGS)
{
	if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(5))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("a protocol's name, trust and owner must not be null")));
	protocol_restore(text_to_cstring(PG_GETARG_TEXT_PP(0)), PG_GETARG_BOOL(1),
					 text_arg_or_null(fcinfo, 2), text_arg_or_null(fcinfo, 3),
					 text_arg_or_null(fcinfo, 4), text_to_cstring(PG_GETARG_TEXT_PP(5)),
					 text_arg_or_null(fcinfo, 6),
					 PG_ARGISNULL(7) ? InvalidOid : PG_GETARG_OID(7));
	PG_RETURN_VOID();
}

/* A member of a label's entry, a string, or NULL where it has none. */
static char *
label_string(JsonbContainer *entry, const char *protocol, const char *key)
{
	JsonbValue	buf;
	JsonbValue *v = getKeyJsonValueFromContainer(entry, key, strlen(key), &buf);

	if (v == NULL || v->type == jbvNull)
		return NULL;
	if (v->type != jbvString)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" of protocol \"%s\" must be a string", key, protocol)));
	return pnstrdup(v->val.string.val, v->val.string.len);
}

/* Is there a function of no arguments of this name?  NULL: none is asked for. */
static bool
function_is_there(const char *qualified)
{
	if (qualified == NULL)
		return true;
	return OidIsValid(LookupFuncName(stringToQualifiedNameList(qualified, NULL),
									 0, NULL, true));
}

/*
 * The relabel check hook of "gp_protocol": a superuser's, on a function; an
 * object of protocols by name, each {"trusted": bool, "readfunc",
 * "writefunc", "validatorfunc", "owner", "acl": text}.  Each protocol it
 * names that this database has not is made, as a restore writes the label;
 * one it has is left as it is.  A segment makes none: the coordinator sends
 * it the call.
 */
static void
gp_protocol_label_check(const ObjectAddress *object, const char *seclabel)
{
	Jsonb	   *jb;
	JsonbIterator *it;
	JsonbIteratorToken tok;
	JsonbValue	v;

	if (object->classId != ProcedureRelationId)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a \"%s\" security label goes on a function",
						PROTOCOL_LABEL_PROVIDER)));
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to create an external protocol")));
	if (seclabel == NULL)
		return;

	jb = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(seclabel)));
	if (!JB_ROOT_IS_OBJECT(jb))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a \"%s\" security label must be a JSON object",
						PROTOCOL_LABEL_PROVIDER)));

	it = JsonbIteratorInit(&jb->root);
	(void) JsonbIteratorNext(&it, &v, true);	/* the object's start */
	while ((tok = JsonbIteratorNext(&it, &v, true)) != WJB_DONE)
	{
		char	   *name;
		JsonbContainer *entry;
		JsonbValue	buf;
		JsonbValue *trusted;
		char	   *owner;
		char	   *readfunc;
		char	   *writefunc;
		char	   *validatorfunc;
		Protocol	existing;

		if (tok != WJB_KEY)
			continue;
		name = pnstrdup(v.val.string.val, v.val.string.len);
		tok = JsonbIteratorNext(&it, &v, true);
		if (tok != WJB_VALUE || v.type != jbvBinary ||
			!JsonContainerIsObject(v.val.binary.data))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("protocol \"%s\" must be given an object", name)));
		entry = v.val.binary.data;
		trusted = getKeyJsonValueFromContainer(entry, "trusted", strlen("trusted"), &buf);
		if (trusted == NULL || trusted->type != jbvBool)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("\"trusted\" of protocol \"%s\" must be true or false", name)));
		owner = label_string(entry, name, "owner");
		if (owner == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("protocol \"%s\" must be given its owner", name)));

		if (GpClusterBackendRole() == GP_ROLE_EXECUTE || protocol_get(name, &existing))
			continue;
		readfunc = label_string(entry, name, "readfunc");
		writefunc = label_string(entry, name, "writefunc");
		validatorfunc = label_string(entry, name, "validatorfunc");
		/* made with the label of the last of its functions a restore makes */
		if (!function_is_there(readfunc) || !function_is_there(writefunc) ||
			!function_is_there(validatorfunc))
			continue;
		protocol_restore(name, trusted->val.boolean, readfunc, writefunc,
						 validatorfunc, owner, label_string(entry, name, "acl"),
						 InvalidOid);
	}
}

void
ExtProtocolRegisterLabelProvider(void)
{
	register_label_provider(PROTOCOL_LABEL_PROVIDER, gp_protocol_label_check);
}
