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
 * deny.c
 *	  When in the week a role may not log in: Cloudberry's DENY clauses.
 *
 * Cloudberry keeps a role's windows in the shared catalog
 * pg_auth_time_constraint, a row each: the day and time one starts and the
 * day and time it ends, Sunday being day 0.  An extension can make no shared
 * catalog, so here they are a key of the role's "gp" label, one window after
 * another in the order they were added:
 *
 *	  deny=0 00:00:00 0 24:00:00;4 00:00:00 5 13:00:00
 *
 * which is the order Cloudberry's catalog scan finds its rows in, and so the
 * order DROP DENY's NOTICEs come in.  The label is shared, WAL-logged and
 * dumped with the role, and the segments are sent it, as every "gp" label
 * is.  Cloudberry's catalog is a view here, pg_catalog.pg_auth_time_constraint,
 * over every role's label, and a write to it with allow_system_table_mods
 * writes the label, as a write to Cloudberry's catalog writes its rows.
 *
 * A window is closed at both ends.  DENY DAY d is the whole day, d 00:00:00
 * to d 24:00:00, and DENY DAY d TIME t the instant d t alone; a window may
 * not wrap around the end of the week.  A login is refused while the time
 * the server's clock says, in the server's time zone, falls in one of the
 * role's windows.
 *
 * Cloudberry sources this file is made of:
 *	  the DENY code of src/backend/commands/user.c and of
 *	  src/backend/libpq/auth.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_shseclabel.h"
#include "commands/defrem.h"
#include "commands/trigger.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/fmgrprotos.h"
#include "utils/jsonb.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

#include "gp_label.h"
#include "gp_security.h"

/* Cloudberry's daysofweek, which its messages name the days by */
static const char *const deny_day_name[7] = {
	"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};

/* ------------------------------------------------------------------------- */
/* Windows                                                                   */
/* ------------------------------------------------------------------------- */

static int
deny_point_cmp(const GpDenyPoint *a, const GpDenyPoint *b)
{
	if (a->day != b->day)
		return a->day > b->day ? 1 : -1;
	if (a->time != b->time)
		return a->time > b->time ? 1 : -1;
	return 0;
}

/* Cloudberry's interval_contains(): both ends are in the window. */
static bool
deny_contains(const GpDenyWindow *w, const GpDenyPoint *p)
{
	return deny_point_cmp(p, &w->start) >= 0 && deny_point_cmp(p, &w->end) <= 0;
}

/* Cloudberry's interval_overlap(): the two share a moment. */
static bool
deny_overlap(const GpDenyWindow *a, const GpDenyWindow *b)
{
	return deny_point_cmp(&a->start, &b->end) <= 0 &&
		deny_point_cmp(&a->end, &b->start) >= 0;
}

static bool
deny_equal(const GpDenyWindow *a, const GpDenyWindow *b)
{
	return deny_point_cmp(&a->start, &b->start) == 0 &&
		deny_point_cmp(&a->end, &b->end) == 0;
}

static char *
deny_time_out(TimeADT t)
{
	return DatumGetCString(DirectFunctionCall1(time_out, TimeADTGetDatum(t)));
}

static TimeADT
deny_time_in(const char *s)
{
	return DatumGetTimeADT(DirectFunctionCall3(time_in, CStringGetDatum(s),
											   ObjectIdGetDatum(InvalidOid),
											   Int32GetDatum(-1)));
}

/* The moment of the week a time is, in the session's time zone. */
static void
deny_point_of(TimestampTz when, GpDenyPoint *out)
{
	struct pg_tm tm;
	fsec_t		fsec;
	int			tz;

	if (timestamp2tm(when, &tz, &tm, &fsec, NULL, NULL) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("current timestamp out of range")));

	out->day = j2day(date2j(tm.tm_year, tm.tm_mon, tm.tm_mday));
	out->time = DatumGetTimeADT(DirectFunctionCall1(timestamptz_time,
													TimestampTzGetDatum(when)));
}

/* ------------------------------------------------------------------------- */
/* The label                                                                 */
/* ------------------------------------------------------------------------- */

static void
role_address(Oid roleid, ObjectAddress *addr)
{
	ObjectAddressSet(*addr, AuthIdRelationId, roleid);
}

/* A label's value as windows, oldest first. */
static List *
deny_parse(const char *value)
{
	List	   *windows = NIL;
	char	   *copy = pstrdup(value);
	char	   *save = NULL;

	for (char *item = strtok_r(copy, ";", &save); item != NULL;
		 item = strtok_r(NULL, ";", &save))
	{
		GpDenyWindow *w = palloc(sizeof(GpDenyWindow));
		int			sday;
		int			eday;
		char		stime[64];
		char		etime[64];

		if (sscanf(item, "%d %63s %d %63s", &sday, stime, &eday, etime) != 4)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("malformed DENY window \"%s\" in a \"%s\" label",
							item, GP_LABEL_PROVIDER)));
		w->start.day = sday;
		w->start.time = deny_time_in(stime);
		w->end.day = eday;
		w->end.time = deny_time_in(etime);
		windows = lappend(windows, w);
	}
	return windows;
}

List *
GpDenyRead(Oid roleid)
{
	ObjectAddress addr;
	char	   *value;

	role_address(roleid, &addr);
	value = GpLabelGet(&addr, GP_LABEL_deny);
	if (value == NULL)
		return NIL;
	return deny_parse(value);
}

void
GpDenyWrite(Oid roleid, List *windows)
{
	ObjectAddress addr;
	StringInfoData buf;

	role_address(roleid, &addr);
	if (windows == NIL)
	{
		GpLabelSet(&addr, GP_LABEL_deny, NULL);
		return;
	}

	initStringInfo(&buf);
	foreach_ptr(GpDenyWindow, w, windows)
	{
		if (buf.len > 0)
			appendStringInfoChar(&buf, ';');
		appendStringInfo(&buf, "%d %s %d %s",
						 w->start.day, deny_time_out(w->start.time),
						 w->end.day, deny_time_out(w->end.time));
	}
	GpLabelSet(&addr, GP_LABEL_deny, buf.data);
}

/* ------------------------------------------------------------------------- */
/* The clauses                                                               */
/* ------------------------------------------------------------------------- */

/*
 * Cloudberry's ExtractAuthInterpretDay(): a number from 0 to 6, or the name
 * of a day in English, in any case.
 */
static int16
deny_day(const JsonbValue *v)
{
	if (v != NULL && v->type == jbvNumeric)
	{
		int32		day = DatumGetInt32(DirectFunctionCall1(numeric_int4,
															NumericGetDatum(v->val.numeric)));

		if (day < 0 || day > 6)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("numeric day of week must be between 0 and 6")));
		return (int16) day;
	}
	if (v != NULL && v->type == jbvString)
	{
		char	   *name = pnstrdup(v->val.string.val, v->val.string.len);

		for (int16 d = 0; d < 7; d++)
		{
			if (pg_strcasecmp(name, deny_day_name[d]) == 0)
				return d;
		}
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("invalid weekday name \"%s\"", name),
				 errhint("Day of week must be one of 'Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'.")));
	}
	elog(ERROR, "gp_security: a DENY clause carried no day");
	return 0;					/* keep the compiler quiet */
}

/* One carried point's day and time; the time as given, or NULL. */
static void
deny_carried_point(JsonbContainer *point, JsonbValue *day, char **time)
{
	JsonbValue	buf;
	JsonbValue *v;

	v = getKeyJsonValueFromContainer(point, "day", strlen("day"), day);
	if (v == NULL)
		elog(ERROR, "gp_security: a DENY clause carried no day");

	v = getKeyJsonValueFromContainer(point, "time", strlen("time"), &buf);
	*time = (v != NULL && v->type == jbvString) ?
		pnstrdup(v->val.string.val, v->val.string.len) : NULL;
}

/*
 * The window a clause the rewrite carried says, as Cloudberry's
 * ExtractAuthIntervalClause() reads one: the start's day and time, then the
 * end's, a point being both, and a missing time the start or the end of the
 * day.  The time is read as time's input reads it, errors and all.
 */
void
GpDenyWindowFromClause(const char *carried, GpDenyWindow *out)
{
	Jsonb	   *jb = DatumGetJsonbP(DirectFunctionCall1(jsonb_in,
														CStringGetDatum(carried)));
	JsonbIterator *it = JsonbIteratorInit(&jb->root);
	JsonbIteratorToken tok;
	JsonbValue	v;
	JsonbContainer *points[2] = {NULL, NULL};
	int			npoints = 0;
	JsonbValue	day;
	char	   *time;

	while ((tok = JsonbIteratorNext(&it, &v, true)) != WJB_DONE)
	{
		if (tok == WJB_ELEM && v.type == jbvBinary && npoints < 2)
			points[npoints++] = v.val.binary.data;
	}
	if (npoints == 0)
		elog(ERROR, "gp_security: a DENY clause carried no point");
	if (npoints == 1)
		points[1] = points[0];

	deny_carried_point(points[0], &day, &time);
	out->start.day = deny_day(&day);
	out->start.time = deny_time_in(time != NULL ? time : "00:00:00");
	deny_carried_point(points[1], &day, &time);
	out->end.day = deny_day(&day);
	out->end.time = deny_time_in(time != NULL ? time : "24:00:00");

	if (deny_point_cmp(&out->start, &out->end) > 0)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("time interval must not wrap around")));
}

/* The statement's own SUPERUSER or NOSUPERUSER, if it has one. */
static bool
stmt_superuser(List *options, bool *given)
{
	bool		super = false;

	*given = false;
	foreach_node(DefElem, def, options)
	{
		if (def->defnamespace == NULL && strcmp(def->defname, "superuser") == 0)
		{
			*given = true;
			super = defGetBoolean(def);
		}
	}
	return super;
}

static bool
role_is_superuser(Oid roleid)
{
	HeapTuple	tuple = SearchSysCache1(AUTHOID, ObjectIdGetDatum(roleid));
	bool		super;

	if (!HeapTupleIsValid(tuple))
		return false;
	super = ((Form_pg_authid) GETSTRUCT(tuple))->rolsuper;
	ReleaseSysCache(tuple);
	return super;
}

/*
 * What a CREATE or ALTER ROLE carried of DENY and DROP DENY, read and checked
 * before the statement runs, in Cloudberry's order: each clause's window as
 * it comes, then a superuser's refused -- the role the statement leaves,
 * which its own SUPERUSER or NOSUPERUSER says where it has one -- then the
 * two kinds refused together.  roleid is the role an ALTER names.
 */
void
GpDenyCheck(Node *stmt, Oid roleid, List *carried, List **add, List **drop)
{
	bool		creating = IsA(stmt, CreateRoleStmt);
	List	   *options = creating ? ((CreateRoleStmt *) stmt)->options :
		((AlterRoleStmt *) stmt)->options;
	bool		given;
	bool		super;

	*add = NIL;
	*drop = NIL;
	foreach_node(DefElem, def, carried)
	{
		GpDenyWindow *w;

		if (strcmp(def->defname, "deny") != 0 && strcmp(def->defname, "drop_deny") != 0)
			continue;
		w = palloc(sizeof(GpDenyWindow));
		GpDenyWindowFromClause(strVal(def->arg), w);
		if (strcmp(def->defname, "deny") == 0)
			*add = lappend(*add, w);
		else
			*drop = lappend(*drop, w);
	}

	super = stmt_superuser(options, &given);
	if (!creating && !given)
		super = role_is_superuser(roleid);
	if (super && *add != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 creating ?
				 errmsg("cannot create superuser with DENY rules") :
				 errmsg("cannot alter superuser with DENY rules")));

	if (*add != NIL && *drop != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("conflicting or redundant options"),
				 errhint("DENY and DROP DENY cannot be used in the same ALTER ROLE statement.")));
}

/*
 * And after the statement: the windows added, or dropped -- each that meets
 * one DROP DENY names, with a NOTICE, and an error when none does -- and, as
 * Cloudberry's ALTER ROLE does, every window of a role the statement left a
 * superuser taken away, whatever the statement said.  Called for every ALTER
 * ROLE, carrying anything or not.
 */
void
GpDenyApply(Oid roleid, const char *rolename, bool creating,
			List *add, List *drop)
{
	List	   *windows = creating ? NIL : GpDenyRead(roleid);
	List	   *kept = NIL;
	bool		dropped = false;

	if (!creating && windows != NIL && role_is_superuser(roleid))
	{
		GpDenyWrite(roleid, NIL);
		windows = NIL;
	}

	if (add != NIL)
	{
		GpDenyWrite(roleid, list_concat(windows, add));
		return;
	}
	if (drop == NIL)
		return;

	foreach_ptr(GpDenyWindow, w, windows)
	{
		bool		meets = false;

		foreach_ptr(GpDenyWindow, d, drop)
		{
			if (deny_overlap(w, d))
			{
				meets = true;
				break;
			}
		}
		if (!meets)
		{
			kept = lappend(kept, w);
			continue;
		}
		ereport(NOTICE,
				(errmsg("dropping DENY rule for \"%s\" between %s %s and %s %s",
						rolename,
						deny_day_name[w->start.day], deny_time_out(w->start.time),
						deny_day_name[w->end.day], deny_time_out(w->end.time))));
		dropped = true;
	}
	if (!dropped)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("cannot find matching DENY rules for \"%s\"", rolename)));
	GpDenyWrite(roleid, kept);
}

/* ------------------------------------------------------------------------- */
/* Logging in                                                                */
/* ------------------------------------------------------------------------- */

/*
 * May this role log in at this time?  Cloudberry's
 * check_auth_time_constraints_internal(): no, if the time falls in one of its
 * windows; and a superuser that has any is warned about, since none should
 * be given one -- only a write to the catalog can give one.
 */
bool
GpDenyAllows(Oid roleid, TimestampTz when)
{
	List	   *windows = GpDenyRead(roleid);
	GpDenyPoint now;
	bool		allowed = true;

	if (windows == NIL)
		return true;

	deny_point_of(when, &now);
	foreach_ptr(GpDenyWindow, w, windows)
	{
		if (deny_contains(w, &now))
		{
			allowed = false;
			break;
		}
	}

	if (role_is_superuser(roleid))
		ereport(WARNING,
				(errmsg("time constraints added on superuser role")));
	return allowed;
}

/*
 * The same, by the role's name, for a caller that has no other way in: the
 * regress.so functions of Cloudberry's tests (cb_regress.c), which find it by
 * name.  A role that does not exist may, as in Cloudberry: refusing it is
 * another step's.
 */
PGDLLEXPORT bool
GpDenyRoleAllowed(const char *rolename, TimestampTz when)
{
	Oid			roleid = get_role_oid(rolename, true);

	if (!OidIsValid(roleid))
		return true;
	return GpDenyAllows(roleid, when);
}

/* The time a login is checked at: gp.auth_time_override's, or the clock's. */
TimestampTz
GpDenyNow(void)
{
	if (gp_auth_time_override != NULL && gp_auth_time_override[0] != '\0')
		return DatumGetTimestampTz(DirectFunctionCall3(timestamptz_in,
													   CStringGetDatum(gp_auth_time_override),
													   ObjectIdGetDatum(InvalidOid),
													   Int32GetDatum(-1)));
	return GetCurrentTimestamp();
}

/* ------------------------------------------------------------------------- */
/* SQL                                                                       */
/* ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(gp_security_check_auth_time_constraints);
PG_FUNCTION_INFO_V1(gp_security_auth_time_constraints);
PG_FUNCTION_INFO_V1(gp_security_auth_time_constraint_write);

/*
 * gp_security.check_auth_time_constraints(rolename name, at timestamptz)
 *
 * Whether the role may log in at that time; what Cloudberry's tests ask
 * through their regress.so.
 */
Datum
gp_security_check_auth_time_constraints(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(GpDenyRoleAllowed(NameStr(*PG_GETARG_NAME(0)),
									 PG_GETARG_TIMESTAMPTZ(1)));
}

/*
 * gp_security.auth_time_constraints(): every role's windows, the rows of
 * Cloudberry's pg_auth_time_constraint -- each role's the newest first, as
 * Cloudberry's catalog gives them to the join its tests read them with.
 */
Datum
gp_security_auth_time_constraints(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	rel;
	SysScanDesc scan;
	HeapTuple	tuple;
	List	   *roles = NIL;

	InitMaterializedSRF(fcinfo, 0);

	/* the roles that have a "gp" label, which a window is a key of */
	rel = table_open(SharedSecLabelRelationId, AccessShareLock);
	scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_shseclabel l = (Form_pg_shseclabel) GETSTRUCT(tuple);
		bool		isnull;
		Datum		provider = heap_getattr(tuple, Anum_pg_shseclabel_provider,
											RelationGetDescr(rel), &isnull);

		if (l->classoid != AuthIdRelationId || isnull ||
			strcmp(TextDatumGetCString(provider), GP_LABEL_PROVIDER) != 0)
			continue;
		roles = lappend_oid(roles, l->objoid);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	foreach_oid(roleid, roles)
	{
		List	   *windows = GpDenyRead(roleid);

		for (int i = list_length(windows) - 1; i >= 0; i--)
		{
			GpDenyWindow *w = list_nth(windows, i);
			Datum		values[5];
			bool		nulls[5] = {false, false, false, false, false};

			values[0] = ObjectIdGetDatum(roleid);
			values[1] = Int16GetDatum(w->start.day);
			values[2] = TimeADTGetDatum(w->start.time);
			values[3] = Int16GetDatum(w->end.day);
			values[4] = TimeADTGetDatum(w->end.time);
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		}
	}
	return (Datum) 0;
}

/* A row of the view, as a role and its window; every column is needed. */
static Oid
deny_row(TriggerData *trigdata, HeapTuple tuple, GpDenyWindow *w)
{
	TupleDesc	desc = RelationGetDescr(trigdata->tg_relation);
	static const char *const column[5] = {
		"authid", "start_day", "start_time", "end_day", "end_time"
	};
	Datum		v[5];

	for (int i = 0; i < 5; i++)
	{
		bool		isnull;

		v[i] = heap_getattr(tuple, i + 1, desc, &isnull);
		if (isnull)
			ereport(ERROR,
					(errcode(ERRCODE_NOT_NULL_VIOLATION),
					 errmsg("null value in column \"%s\" of relation \"%s\" violates not-null constraint",
							column[i], RelationGetRelationName(trigdata->tg_relation))));
	}
	w->start.day = DatumGetInt16(v[1]);
	w->start.time = DatumGetTimeADT(v[2]);
	w->end.day = DatumGetInt16(v[3]);
	w->end.time = DatumGetTimeADT(v[4]);
	return DatumGetObjectId(v[0]);
}

/* A role's window taken away: the first that is this one.  Was it there? */
static bool
deny_remove(Oid roleid, const GpDenyWindow *w)
{
	List	   *windows = GpDenyRead(roleid);

	foreach_ptr(GpDenyWindow, have, windows)
	{
		if (deny_equal(have, w))
		{
			windows = foreach_delete_current(windows, have);
			GpDenyWrite(roleid, windows);
			return true;
		}
	}
	return false;
}

/*
 * gp_security.auth_time_constraint_write(), INSTEAD OF each row written to
 * pg_catalog.pg_auth_time_constraint: the row's window added to its role's
 * label, taken from it, or both, as a write to Cloudberry's catalog adds and
 * takes its rows -- the rows as written, since Cloudberry checks nothing a
 * catalog write says.  The statement trigger beside it has refused the write
 * unless allow_system_table_mods is on.
 */
Datum
gp_security_auth_time_constraint_write(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	GpDenyWindow oldw;
	GpDenyWindow neww;
	Oid			oldrole;
	Oid			newrole;

	if (!CALLED_AS_TRIGGER(fcinfo))
		elog(ERROR, "gp_security_auth_time_constraint_write: not called by the trigger manager");

	if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
	{
		newrole = deny_row(trigdata, trigdata->tg_trigtuple, &neww);
		if (!SearchSysCacheExists1(AUTHOID, ObjectIdGetDatum(newrole)))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("role with OID %u does not exist", newrole)));
		GpDenyWrite(newrole, lappend(GpDenyRead(newrole), &neww));
		return PointerGetDatum(trigdata->tg_trigtuple);
	}

	oldrole = deny_row(trigdata, trigdata->tg_trigtuple, &oldw);
	if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
		return PointerGetDatum(deny_remove(oldrole, &oldw) ?
							   trigdata->tg_trigtuple : NULL);

	newrole = deny_row(trigdata, trigdata->tg_newtuple, &neww);
	if (!SearchSysCacheExists1(AUTHOID, ObjectIdGetDatum(newrole)))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("role with OID %u does not exist", newrole)));
	if (!deny_remove(oldrole, &oldw))
		return PointerGetDatum(NULL);
	GpDenyWrite(newrole, lappend(GpDenyRead(newrole), &neww));
	return PointerGetDatum(trigdata->tg_newtuple);
}
