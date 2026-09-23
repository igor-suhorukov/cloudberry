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
 * partition.c
 *	  The partitions Cloudberry's classic partition clauses describe.
 *
 * O26 carries a classic clause to gp_sql as one option of its statement
 * (pg19/grammar/gp_partition.h): CREATE TABLE's PARTITION BY ... (...) as
 * gp.partition_by in its WITH list, and each of ALTER TABLE's partition
 * commands as SET (gp.partition_cmd = ...).  gp_sql's ProcessUtility hook
 * takes the option out, lets PostgreSQL run what is left -- the partitioned
 * table itself, or the rest of the ALTER -- and then calls here, where the
 * clause is read again and done: each partition it describes is a CREATE
 * TABLE ... PARTITION OF run through ProcessUtility, so that M2's dispatch
 * sees each one, and each command becomes the statements of PostgreSQL's that
 * Cloudberry turned it into -- DROP TABLE, TRUNCATE, DETACH and ATTACH
 * PARTITION, a rename.
 *
 * What this keeps of Cloudberry is its semantics: the partitions' names
 * (<parent>_<level>_prt_<name or number>, the default partition numbered
 * first), EVERY's arithmetic with the key type's + operator, START EXCLUSIVE
 * and END INCLUSIVE turned into PostgreSQL's inclusive-exclusive bounds, a
 * missing START or END taken from the neighbouring partition, the templates
 * that give a new partition its subpartitions, and the messages.  What it
 * changes is where things are kept: a SUBPARTITION TEMPLATE is in the "gp"
 * label of the hierarchy's root, where Cloudberry has gp_partition_template;
 * and a partition's bound reaches PostgreSQL as text PostgreSQL's parser
 * reads, because PostgreSQL 19's parse analysis takes no Const in a raw
 * bound, which Cloudberry's does (its transformExprRecurse has a T_Const
 * case PostgreSQL does not).
 *
 * Cloudberry sources this file is made of:
 *	  src/backend/parser/parse_partition_gp.c
 *	  src/backend/commands/tablecmds_gp.c
 *	  src/backend/catalog/gp_partition_template.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/partition.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_partitioned_table.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/params.h"
#include "optimizer/optimizer.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_node.h"
#include "parser/parse_oper.h"
#include "parser/parse_utilcmd.h"
#include "partitioning/partbounds.h"
#include "partitioning/partdesc.h"
#include "storage/lmgr.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/datum.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/partcache.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

#include "gp_grammar_int.h"
#include "gp_label.h"
#include "gp_partition.h"
#include "gp_sql.h"

/* gp.max_partition_level: Cloudberry's gp_max_partition_level */
int			gp_max_partition_level = 0;

/*
 * The key of a level of partitions, and the partitions each of its members is
 * to be given: a SUBPARTITION BY, with its template if it has one.  The chain
 * goes down a level at a time.
 */
typedef struct PartLevel
{
	PartitionSpec *spec;		/* the key, as PostgreSQL's node */
	GpPartParser *tp;			/* the text the template is in */
	GpPartDef  *template_def;	/* NULL if it has none */
	int			location;		/* SUBPARTITION BY, in the user's text */
	struct PartLevel *next;
} PartLevel;

/*
 * One partition to be made, with its name chosen before any is made, as
 * Cloudberry chooses them.  A range bound is kept as Consts until the
 * partitions without a START or an END have one; a list bound is what the
 * user wrote.
 */
typedef struct PartChild
{
	char	   *relname;
	bool		is_default;
	char		strategy;
	List	   *listdatums;		/* LIST: raw expressions */
	List	   *lower;			/* RANGE: Consts, or a ColumnRef minvalue */
	List	   *upper;			/* RANGE: Consts, or a ColumnRef maxvalue */
	int			location;		/* its element, for an error about it */
	List	   *options;
	char	   *tablespace;
	char	   *accessmethod;
	GpPartParser *subp;			/* its own partitions: in this text, */
	GpPartDef  *subdef;			/* this list */
	PartLevel  *sublevel;		/* with this key */
} PartChild;

/* A partitioned table whose partitions are still to be made. */
typedef struct PartTodo
{
	Oid			relid;
	GpPartParser *p;
	GpPartDef  *def;
	PartLevel  *sub;
	bool		alter;			/* ALTER TABLE ... ADD PARTITION */
} PartTodo;

static List *make_children(Relation parentrel, GpPartParser *p, GpPartDef *def,
						   PartLevel *sub, bool alter, const char *queryString);
static void create_all(PartTodo *first, const char *queryString,
					   QueryEnvironment *queryEnv);

/* ------------------------------------------------------------------------- */
/* Reading an option                                                         */
/* ------------------------------------------------------------------------- */

/*
 * The parser over an option the rewrite wrote.  Its value is the user's text
 * from the option's location on, so a position in it is one in queryString;
 * if what is there is not the value -- a statement that did not come through
 * the rewrite, written by hand -- positions are not known, and none is
 * reported.
 */
static GpPartParser *
option_parser(DefElem *def, const char *queryString)
{
	GpPartParser *p = palloc0(sizeof(GpPartParser));
	char	   *text = defGetString(def);
	int			len = strlen(text);
	int			base = def->location;

	if (queryString == NULL || base < 0 || (int) strlen(queryString) < base + len ||
		strncmp(queryString + base, text, len) != 0)
		base = -1;

	p->ts = GpTokenize(text);
	p->limit = p->ts->ntoks;
	p->errsrc = (queryString != NULL) ? queryString : text;
	p->base = base;
	p->check_exprs = false;
	return p;
}

/* The same over stored text -- a template -- which has no place in the
 * statement's. */
static GpPartParser *
text_parser(const char *text)
{
	GpPartParser *p = palloc0(sizeof(GpPartParser));

	p->ts = GpTokenize(text);
	p->limit = p->ts->ntoks;
	p->errsrc = text;
	p->base = -1;
	p->check_exprs = false;
	return p;
}

static ParseState *
make_pstate(const char *queryString)
{
	ParseState *pstate = make_parsestate(NULL);

	pstate->p_sourcetext = queryString;
	return pstate;
}

/* ------------------------------------------------------------------------- */
/* Running PostgreSQL's statements                                           */
/* ------------------------------------------------------------------------- */

/*
 * A statement of PostgreSQL's, as a subcommand of the one being run: through
 * ProcessUtility, so that every hook sees it -- M2's dispatch among them --
 * with the user's text, so that an error in it is reported where the user
 * wrote what it came from.
 */
static void
run_utility(Node *stmt, const char *queryString, QueryEnvironment *queryEnv)
{
	PlannedStmt *pstmt = makeNode(PlannedStmt);

	pstmt->commandType = CMD_UTILITY;
	pstmt->canSetTag = false;
	pstmt->utilityStmt = stmt;
	pstmt->stmt_location = -1;
	pstmt->stmt_len = 0;

	ProcessUtility(pstmt, queryString != NULL ? queryString : "", false,
				   PROCESS_UTILITY_SUBCOMMAND, NULL, queryEnv, None_Receiver,
				   NULL);
	CommandCounterIncrement();
}

static RenameStmt *
rename_stmt(RangeVar *rv, const char *newname)
{
	RenameStmt *r = makeNode(RenameStmt);

	r->renameType = OBJECT_TABLE;
	r->relation = rv;
	r->newname = pstrdup(newname);
	return r;
}

static AlterObjectSchemaStmt *
set_schema_stmt(RangeVar *rv, const char *newschema)
{
	AlterObjectSchemaStmt *s = makeNode(AlterObjectSchemaStmt);

	s->objectType = OBJECT_TABLE;
	s->relation = rv;
	s->newschema = pstrdup(newschema);
	return s;
}

/* ALTER TABLE parent DETACH or ATTACH PARTITION part [bound] */
static AlterTableStmt *
partition_cmd_stmt(RangeVar *parent, RangeVar *part, PartitionBoundSpec *bound,
				   AlterTableType subtype)
{
	AlterTableStmt *at = makeNode(AlterTableStmt);
	AlterTableCmd *cmd = makeNode(AlterTableCmd);
	PartitionCmd *pc = makeNode(PartitionCmd);

	pc->name = part;
	pc->bound = bound;
	cmd->subtype = subtype;
	cmd->def = (Node *) pc;
	at->relation = parent;
	at->cmds = list_make1(cmd);
	at->objtype = OBJECT_TABLE;
	return at;
}

/* ------------------------------------------------------------------------- */
/* Bounds                                                                    */
/* ------------------------------------------------------------------------- */

/*
 * A value, as text PostgreSQL's parser reads back to the same value: the
 * type's output, cast to the type.  PostgreSQL 19's parse analysis of a
 * partition bound takes raw expressions only, so a bound EVERY computed, or
 * one read from the catalog, goes this way.  The output is written as
 * pg_dump writes it -- ISO dates, PostgreSQL's intervals, floats with every
 * digit -- so that the user's settings cannot change what is read back.
 */
static Node *
const_to_raw(Const *c)
{
	A_Const    *ac = makeNode(A_Const);
	TypeCast   *tc;
	Oid			typoutput;
	bool		isvarlena;
	int			nestlevel;
	char	   *str;

	ac->location = -1;
	if (c->constisnull)
	{
		ac->isnull = true;
		return (Node *) ac;
	}

	getTypeOutputInfo(c->consttype, &typoutput, &isvarlena);
	nestlevel = NewGUCNestLevel();
	(void) set_config_option("datestyle", "ISO", PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);
	(void) set_config_option("intervalstyle", "postgres", PGC_USERSET,
							 PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	(void) set_config_option("extra_float_digits", "3", PGC_USERSET,
							 PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	str = OidOutputFunctionCall(typoutput, c->constvalue);
	AtEOXact_GUC(true, nestlevel);

	ac->val.sval.type = T_String;
	ac->val.sval.sval = str;

	tc = makeNode(TypeCast);
	tc->arg = (Node *) ac;
	tc->typeName = makeTypeNameFromOid(c->consttype, c->consttypmod);
	tc->location = -1;
	return (Node *) tc;
}

static ColumnRef *
infinite_bound(const char *which)
{
	ColumnRef  *cref = makeNode(ColumnRef);

	cref->fields = list_make1(makeString(pstrdup(which)));
	cref->location = -1;
	return cref;
}

/* A bound list, as raw expressions: Consts, the catalog's PartitionRangeDatums,
 * or MINVALUE and MAXVALUE, which are ColumnRefs already. */
static List *
bound_to_raw(List *datums)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, datums)
	{
		Node	   *n = lfirst(lc);

		if (IsA(n, PartitionRangeDatum))
		{
			PartitionRangeDatum *prd = (PartitionRangeDatum *) n;

			if (prd->kind == PARTITION_RANGE_DATUM_MINVALUE)
				n = (Node *) infinite_bound("minvalue");
			else if (prd->kind == PARTITION_RANGE_DATUM_MAXVALUE)
				n = (Node *) infinite_bound("maxvalue");
			else
				n = prd->value;
		}
		if (IsA(n, Const))
			n = const_to_raw((Const *) n);
		result = lappend(result, n);
	}
	return result;
}

/*
 * One value of a bound, transformed to the partition key's type: PostgreSQL
 * 19's transformPartitionBoundValue, which is static there, as Cloudberry
 * exports it.
 */
static Const *
bound_value(ParseState *pstate, Node *val, const char *colName, Oid colType,
			int32 colTypmod, Oid partCollation)
{
	Node	   *value;

	value = transformExpr(pstate, val, EXPR_KIND_PARTITION_BOUND);
	Assert(!contain_var_clause(value));

	value = coerce_to_target_type(pstate,
								  value, exprType(value),
								  colType,
								  colTypmod,
								  COERCION_ASSIGNMENT,
								  COERCE_IMPLICIT_CAST,
								  -1);
	if (value == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("specified value cannot be cast to type %s for column \"%s\"",
						format_type_be(colType), colName),
				 parser_errposition(pstate, exprLocation(val))));

	if (!IsA(value, Const))
	{
		assign_expr_collations(pstate, value);
		value = (Node *) expression_planner((Expr *) value);
		value = (Node *) evaluate_expr((Expr *) value, colType, colTypmod,
									   partCollation);
		if (!IsA(value, Const))
			elog(ERROR, "could not evaluate partition bound expression");
	}
	else
		((Const *) value)->constcollid = partCollation;

	((Const *) value)->location = exprLocation(val);
	return (Const *) value;
}

/* The name of the key's column i, for an error about a value of it. */
static char *
key_column_name(Relation rel, int i)
{
	PartitionKey key = RelationGetPartitionKey(rel);

	if (key->partattrs[i] != 0)
		return get_attname(RelationGetRelid(rel), key->partattrs[i], false);
	return deparse_expression(list_nth(key->partexprs, 0),
							  deparse_context_for(RelationGetRelationName(rel),
												  RelationGetRelid(rel)),
							  false, false);
}

/*
 * Turn START ... EXCLUSIVE or END ... INCLUSIVE into PostgreSQL's inclusive
 * start or exclusive end, by adding the type's smallest step.  Only for the
 * types that have one; at the type's largest value constisnull is set, and
 * the caller makes the end MAXVALUE (or refuses the START).
 */
static void
convert_exclusive_start_inclusive_end(Const *constval, Oid part_col_typid,
									  int32 part_col_typmod,
									  bool is_exclusive_start)
{
	if (part_col_typmod != -1 &&
		(part_col_typid == TIMEOID ||
		 part_col_typid == TIMETZOID ||
		 part_col_typid == TIMESTAMPOID ||
		 part_col_typid == TIMESTAMPTZOID ||
		 part_col_typid == INTERVALOID))
	{
		if (is_exclusive_start)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("START EXCLUSIVE not supported when partition key has precision specification: %s",
							format_type_with_typemod(part_col_typid, part_col_typmod)),
					 errhint("Specify an inclusive START value and remove the EXCLUSIVE keyword")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("END INCLUSIVE not supported when partition key has precision specification: %s",
							format_type_with_typemod(part_col_typid, part_col_typmod)),
					 errhint("Specify an exclusive END value and remove the INCLUSIVE keyword")));
	}

	switch (part_col_typid)
	{
		case INT2OID:
			{
				int16		value = DatumGetInt16(constval->constvalue);

				if (value < PG_INT16_MAX)
					constval->constvalue = Int16GetDatum(value + 1);
				else
					constval->constisnull = true;
			}
			break;
		case INT4OID:
			{
				int32		value = DatumGetInt32(constval->constvalue);

				if (value < PG_INT32_MAX)
					constval->constvalue = Int32GetDatum(value + 1);
				else
					constval->constisnull = true;
			}
			break;
		case INT8OID:
			{
				int64		value = DatumGetInt64(constval->constvalue);

				if (value < PG_INT64_MAX)
					constval->constvalue = Int64GetDatum(value + 1);
				else
					constval->constisnull = true;
			}
			break;
		case DATEOID:
			{
				DateADT		value = DatumGetDateADT(constval->constvalue);

				if (!DATE_IS_NOEND(value))
					constval->constvalue = DateADTGetDatum(value + 1);
				else
					constval->constisnull = true;
			}
			break;
		case TIMEOID:
			{
				TimeADT		value = DatumGetTimeADT(constval->constvalue);
				struct pg_tm tt,
						   *tm = &tt;
				fsec_t		fsec;

				time2tm(value, tm, &fsec);
				if (tm->tm_hour != HOURS_PER_DAY)
					constval->constvalue = TimeADTGetDatum(value + 1);
				else
					constval->constisnull = true;
			}
			break;
		case TIMETZOID:
			{
				TimeTzADT  *valueptr = DatumGetTimeTzADTP(constval->constvalue);
				struct pg_tm tt,
						   *tm = &tt;
				fsec_t		fsec;
				int			tz;

				timetz2tm(valueptr, tm, &fsec, &tz);
				if (tm->tm_hour != HOURS_PER_DAY)
					valueptr->time += 1;
				else
					constval->constisnull = true;
			}
			break;
		case TIMESTAMPOID:
			{
				Timestamp	value = DatumGetTimestamp(constval->constvalue);

				if (!TIMESTAMP_IS_NOEND(value))
					constval->constvalue = TimestampGetDatum(value + 1);
				else
					constval->constisnull = true;
			}
			break;
		case TIMESTAMPTZOID:
			{
				TimestampTz value = DatumGetTimestampTz(constval->constvalue);

				if (!TIMESTAMP_IS_NOEND(value))
					constval->constvalue = TimestampTzGetDatum(value + 1);
				else
					constval->constisnull = true;
			}
			break;
		case INTERVALOID:
			{
				Interval   *intervalp = DatumGetIntervalP(constval->constvalue);

				if (intervalp->month == PG_INT32_MAX &&
					intervalp->day == PG_INT32_MAX &&
					intervalp->time == PG_INT64_MAX)
					constval->constisnull = true;
				else if (intervalp->time < PG_INT64_MAX)
					intervalp->time += 1;
				else if (intervalp->day < PG_INT32_MAX)
					intervalp->day += 1;
				else
					intervalp->month += 1;
			}
			break;
		default:
			if (is_exclusive_start)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("START EXCLUSIVE not supported for partition key data type: %s",
								format_type_be(part_col_typid)),
						 errhint("Specify an inclusive START value and remove the EXCLUSIVE keyword")));
			else
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("END INCLUSIVE not supported for partition key data type: %s",
								format_type_be(part_col_typid)),
						 errhint("Specify an exclusive END value and remove the INCLUSIVE keyword")));
			break;
	}
}

/* A Const of the key's type, holding a copy of `value`. */
static Const *
key_const(PartitionKey key, Datum value)
{
	return makeConst(key->parttypid[0], key->parttypmod[0], key->parttypcoll[0],
					 key->parttyplen[0],
					 datumCopy(value, key->parttypbyval[0], key->parttyplen[0]),
					 false, key->parttypbyval[0]);
}

/* ------------------------------------------------------------------------- */
/* START, END and EVERY                                                      */
/* ------------------------------------------------------------------------- */

/*
 * The partitions of START (s) END (e) EVERY (n): s, s + n, s + 2n, ..., up to
 * e, with the + of the key's type, which for a date takes an interval.
 * Cloudberry's PartEveryIterator.
 */
typedef struct PartEveryIterator
{
	PartitionKey partkey;
	Datum		endVal;
	bool		isEndValMaxValue;

	ExprState  *plusexprstate;
	ParamListInfo plusexpr_params;
	EState	   *estate;

	Datum		currStart;
	Datum		currEnd;
	bool		called;
	bool		endReached;

	/* for context in error messages */
	ParseState *pstate;
	int			end_location;
	int			every_location;
} PartEveryIterator;

static PartEveryIterator *
init_every_iterator(ParseState *pstate, PartitionKey partkey,
					const char *part_col_name, Node *start, bool startExclusive,
					Node *end, bool endInclusive, Node *every)
{
	PartEveryIterator *iter;
	Datum		startVal = 0;
	Datum		endVal = 0;
	bool		isEndValMaxValue = false;
	Oid			part_col_typid = get_partition_col_typid(partkey, 0);
	int32		part_col_typmod = get_partition_col_typmod(partkey, 0);
	Oid			part_col_collation = get_partition_col_collation(partkey, 0);

	if (start)
	{
		Const	   *startConst;

		startConst = bound_value(pstate, start, part_col_name, part_col_typid,
								 part_col_typmod, part_col_collation);
		if (startConst->constisnull)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot use NULL with range partition specification"),
					 parser_errposition(pstate, exprLocation(start))));

		if (startExclusive)
			convert_exclusive_start_inclusive_end(startConst, part_col_typid,
												  part_col_typmod, true);
		if (startConst->constisnull)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("START EXCLUSIVE is out of range"),
					 parser_errposition(pstate, exprLocation(start))));

		startVal = startConst->constvalue;
	}

	if (end)
	{
		/* MINVALUE and MAXVALUE are ColumnRefs */
		if (IsA(end, ColumnRef) &&
			list_length(((ColumnRef *) end)->fields) == 1 &&
			IsA(linitial(((ColumnRef *) end)->fields), String) &&
			(strcmp(strVal(linitial(((ColumnRef *) end)->fields)), "minvalue") == 0 ||
			 strcmp(strVal(linitial(((ColumnRef *) end)->fields)), "maxvalue") == 0))
			isEndValMaxValue = true;
		else
		{
			Const	   *endConst;

			endConst = bound_value(pstate, end, part_col_name, part_col_typid,
								   part_col_typmod, part_col_collation);
			if (endConst->constisnull)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("cannot use NULL with range partition specification"),
						 parser_errposition(pstate, exprLocation(end))));

			if (endInclusive)
				convert_exclusive_start_inclusive_end(endConst, part_col_typid,
													  part_col_typmod, false);
			if (endConst->constisnull)
				isEndValMaxValue = true;

			endVal = endConst->constvalue;
		}
	}

	iter = palloc0(sizeof(PartEveryIterator));
	iter->partkey = partkey;
	iter->endVal = endVal;
	iter->isEndValMaxValue = isEndValMaxValue;

	if (every)
	{
		Node	   *plusexpr;
		Param	   *param;

		if (start == NULL || end == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("EVERY clause requires START and END"),
					 parser_errposition(pstate, exprLocation(every))));

		/*
		 * EVERY is not cast to the key's type but handed to its + operator:
		 * a timestamp key's EVERY is an interval.
		 */
		param = makeNode(Param);
		param->paramkind = PARAM_EXTERN;
		param->paramid = 1;
		param->paramtype = part_col_typid;
		param->paramtypmod = part_col_typmod;
		param->paramcollid = part_col_collation;
		param->location = -1;

		plusexpr = (Node *) make_op(pstate,
									list_make2(makeString("pg_catalog"), makeString("+")),
									(Node *) param,
									(Node *) transformExpr(pstate, every, EXPR_KIND_PARTITION_BOUND),
									pstate->p_last_srf,
									-1);

		if (IsA(plusexpr, CollateExpr))
		{
			Oid			exprCollOid = exprCollation(plusexpr);

			if (OidIsValid(exprCollOid) &&
				exprCollOid != DEFAULT_COLLATION_OID &&
				exprCollOid != part_col_collation)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("collation of partition bound value for column \"%s\" does not match partition key collation \"%s\"",
								part_col_name, get_collation_name(part_col_collation))));
		}

		plusexpr = coerce_to_target_type(pstate,
										 plusexpr, exprType(plusexpr),
										 part_col_typid,
										 part_col_typmod,
										 COERCION_ASSIGNMENT,
										 COERCE_IMPLICIT_CAST,
										 -1);
		if (plusexpr == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("specified value cannot be cast to type %s for column \"%s\"",
							format_type_be(part_col_typid), part_col_name)));

		iter->estate = CreateExecutorState();

		iter->plusexpr_params = makeParamList(1);
		iter->plusexpr_params->params[0].value = (Datum) 0;
		iter->plusexpr_params->params[0].isnull = true;
		iter->plusexpr_params->params[0].pflags = 0;
		iter->plusexpr_params->params[0].ptype = part_col_typid;

		iter->estate->es_param_list_info = iter->plusexpr_params;

		iter->plusexprstate = ExecInitExprWithParams((Expr *) plusexpr,
													 iter->plusexpr_params);
	}

	iter->currEnd = startVal;
	iter->currStart = (Datum) 0;
	iter->called = false;
	iter->endReached = false;

	iter->pstate = pstate;
	iter->end_location = exprLocation(end);
	iter->every_location = exprLocation(every);

	return iter;
}

static void
free_every_iterator(PartEveryIterator *iter)
{
	if (iter->estate)
		FreeExecutorState(iter->estate);
}

/* The next partition of START/END/EVERY, in currStart and currEnd. */
static bool
next_part_bound(PartEveryIterator *iter)
{
	bool		firstcall = !iter->called;

	iter->called = true;

	if (iter->plusexprstate)
	{
		Datum		next;
		int32		cmpval;
		bool		isnull;

		if (iter->endReached)
			return false;

		iter->plusexpr_params->params[0].isnull = false;
		iter->plusexpr_params->params[0].value = iter->currEnd;

		next = ExecEvalExprSwitchContext(iter->plusexprstate,
										 GetPerTupleExprContext(iter->estate),
										 &isnull);
		/* no built-in + returns NULL, but a user's could */
		if (isnull)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("could not compute next partition boundary with EVERY, plus-operator returned NULL"),
					 parser_errposition(iter->pstate, iter->every_location)));

		iter->currStart = iter->currEnd;

		/* Is the next bound past END? */
		cmpval = DatumGetInt32(FunctionCall2Coll(&iter->partkey->partsupfunc[0],
												 iter->partkey->partcollation[0],
												 next,
												 iter->endVal));
		if (cmpval >= 0)
		{
			iter->endReached = true;
			iter->currEnd = iter->endVal;
		}
		else
		{
			/* the next bound must be past this one, or + is not behaving */
			cmpval = DatumGetInt32(FunctionCall2Coll(&iter->partkey->partsupfunc[0],
													 iter->partkey->partcollation[0],
													 iter->currEnd,
													 next));
			if (cmpval >= 0)
			{
				if (firstcall)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("EVERY parameter too small"),
							 parser_errposition(iter->pstate, iter->every_location)));
				else
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("END parameter not reached before type overflows"),
							 parser_errposition(iter->pstate, iter->end_location)));
			}

			iter->currEnd = next;
		}

		return true;
	}

	/* Without EVERY, one partition over the whole range */
	if (!firstcall)
		return false;

	iter->currStart = iter->currEnd;
	iter->currEnd = iter->endVal;
	iter->endReached = true;
	return true;
}

/* ------------------------------------------------------------------------- */
/* Names                                                                     */
/* ------------------------------------------------------------------------- */

/*
 * <parent>_<level>_prt_<name>, or <parent>_<level>_prt_<number> made unique,
 * as Cloudberry names a partition.  The label part is never shortened, so a
 * name too long to fit is refused rather than left to makeObjectName, which
 * asserts, and in a build without assertions shortens the table's name and
 * the level away: __prt_<name>.
 */
static char *
choose_partition_name(const char *parentname, int level, Oid nsp,
					  const char *partname, int partnum)
{
	char		partsubstring[NAMEDATALEN];
	char		levelstr[NAMEDATALEN];

	snprintf(levelstr, NAMEDATALEN, "%d", level);

	if (partname)
	{
		if (strlen(partname) > NAMEDATALEN - 8)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("name \"%s\" for child partition is too long",
							partname)));
		snprintf(partsubstring, NAMEDATALEN, "prt_%s", partname);
		return makeObjectName(parentname, levelstr, partsubstring);
	}

	Assert(partnum > 0);
	snprintf(partsubstring, NAMEDATALEN, "prt_%d", partnum);
	return ChooseRelationName(parentname, levelstr, partsubstring, nsp, false);
}

static int
partition_level(Oid relid)
{
	return list_length(get_partition_ancestors(relid)) + 1;
}

static Oid
partition_root(Oid relid)
{
	List	   *ancestors = get_partition_ancestors(relid);

	return ancestors != NIL ? llast_oid(ancestors) : relid;
}

/* ------------------------------------------------------------------------- */
/* Templates                                                                 */
/* ------------------------------------------------------------------------- */

/*
 * A hierarchy's SUBPARTITION TEMPLATEs, one per level, which Cloudberry keeps
 * in gp_partition_template by the root's OID and the level: here in the
 * root's "gp" label, as one value of "<level>:<length>:<text>" entries, the
 * text being the template's list as the user wrote it.  A label goes with
 * its table, is transactional, and pg_dump writes it.
 */
typedef struct PartTemplate
{
	int			level;
	char	   *text;
} PartTemplate;

static List *
templates_read(Oid rootid)
{
	ObjectAddress addr;
	char	   *value;
	char	   *s;
	List	   *result = NIL;

	ObjectAddressSet(addr, RelationRelationId, rootid);
	value = GpLabelGet(&addr, GP_LABEL_partition_templates);
	if (value == NULL)
		return NIL;

	s = value;
	while (*s != '\0')
	{
		PartTemplate *t = palloc(sizeof(PartTemplate));
		char	   *colon;
		int			len;

		t->level = (int) strtol(s, &colon, 10);
		if (*colon != ':')
			elog(ERROR, "malformed partition template label of relation %u", rootid);
		len = (int) strtol(colon + 1, &s, 10);
		if (*s != ':' || len < 0 || (int) strlen(s + 1) < len)
			elog(ERROR, "malformed partition template label of relation %u", rootid);
		t->text = pnstrdup(s + 1, len);
		s += 1 + len;
		result = lappend(result, t);
	}
	return result;
}

static void
templates_write(Oid rootid, List *templates)
{
	ObjectAddress addr;
	StringInfoData buf;
	ListCell   *lc;

	initStringInfo(&buf);
	foreach(lc, templates)
	{
		PartTemplate *t = lfirst(lc);

		appendStringInfo(&buf, "%d:%d:%s", t->level, (int) strlen(t->text), t->text);
	}
	ObjectAddressSet(addr, RelationRelationId, rootid);
	GpLabelSet(&addr, GP_LABEL_partition_templates, buf.len > 0 ? buf.data : NULL);
}

/* The template for the partitions of level `level`'s members, or NULL. */
static char *
template_get(Oid rootid, int level)
{
	ListCell   *lc;

	foreach(lc, templates_read(rootid))
	{
		PartTemplate *t = lfirst(lc);

		if (t->level == level)
			return t->text;
	}
	return NULL;
}

/* Set it, or with NULL remove it; returns whether there was one. */
static bool
template_set(Oid rootid, int level, const char *text)
{
	List	   *templates = templates_read(rootid);
	List	   *result = NIL;
	bool		found = false;
	ListCell   *lc;

	foreach(lc, templates)
	{
		PartTemplate *t = lfirst(lc);

		if (t->level == level)
		{
			found = true;
			continue;
		}
		result = lappend(result, t);
	}
	if (text != NULL)
	{
		PartTemplate *t = palloc(sizeof(PartTemplate));

		t->level = level;
		t->text = pstrdup(text);
		result = lappend(result, t);
	}
	templates_write(rootid, result);
	return found;
}

/* The key chain of a clause's SUBPARTITION BYs, from the clause's text. */
static PartLevel *
levels_of_key(GpPartParser *p, GpPartKey *key)
{
	PartLevel  *first = NULL;
	PartLevel  *last = NULL;

	for (; key != NULL; key = key->sub)
	{
		PartLevel  *l = palloc0(sizeof(PartLevel));

		l->spec = GpPartKeySpec(p, key);
		l->tp = p;
		l->template_def = key->template_def;
		l->location = GpPartLocation(p, key->location);
		if (last == NULL)
			first = l;
		else
			last->next = l;
		last = l;
	}
	return first;
}

/* ------------------------------------------------------------------------- */
/* An existing hierarchy                                                     */
/* ------------------------------------------------------------------------- */

/*
 * The key of a partitioned table, as a PARTITION BY would say it:
 * Cloudberry's generatePartitionSpec, which gives ADD PARTITION the key of
 * the new partition's partitions from an existing one of its siblings.
 */
static PartitionSpec *
partition_spec_of(Relation rel)
{
	HeapTuple	tuple;
	Form_pg_partitioned_table form;
	oidvector  *opclass;
	oidvector  *collation;
	Datum		datum;
	bool		isnull;
	PartitionSpec *spec = makeNode(PartitionSpec);

	tuple = SearchSysCache1(PARTRELID, ObjectIdGetDatum(RelationGetRelid(rel)));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "missing partition key information for oid %u",
			 RelationGetRelid(rel));
	form = (Form_pg_partitioned_table) GETSTRUCT(tuple);

	spec->strategy = form->partstrat;
	spec->location = -1;

	datum = SysCacheGetAttrNotNull(PARTRELID, tuple,
								   Anum_pg_partitioned_table_partclass);
	opclass = (oidvector *) DatumGetPointer(datum);
	datum = SysCacheGetAttrNotNull(PARTRELID, tuple,
								   Anum_pg_partitioned_table_partcollation);
	collation = (oidvector *) DatumGetPointer(datum);

	(void) SysCacheGetAttr(PARTRELID, tuple,
						   Anum_pg_partitioned_table_partexprs, &isnull);
	if (!isnull)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("SUBPARTITION BY contain expressions. Cannot ADD PARTITION if expressions in partition key using legacy syntax"),
				 errhint("Table was created using new partition syntax. Hence, use CREATE TABLE... PARTITION OF instead.")));

	for (int i = 0; i < form->partnatts; i++)
	{
		PartitionElem *elem = makeNode(PartitionElem);
		AttrNumber	attno = form->partattrs.values[i];
		HeapTuple	tmp;

		elem->name = pstrdup(NameStr(TupleDescAttr(RelationGetDescr(rel), attno - 1)->attname));
		elem->location = -1;

		tmp = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclass->values[i]));
		if (!HeapTupleIsValid(tmp))
			elog(ERROR, "cache lookup failed for opclass %u", opclass->values[i]);
		elem->opclass = list_make2(makeString(get_namespace_name(((Form_pg_opclass) GETSTRUCT(tmp))->opcnamespace)),
								   makeString(pstrdup(NameStr(((Form_pg_opclass) GETSTRUCT(tmp))->opcname))));
		ReleaseSysCache(tmp);

		if (OidIsValid(collation->values[i]))
		{
			tmp = SearchSysCache1(COLLOID, ObjectIdGetDatum(collation->values[i]));
			if (!HeapTupleIsValid(tmp))
				elog(ERROR, "collation with OID %u does not exist", collation->values[i]);
			elem->collation = list_make2(makeString(get_namespace_name(((Form_pg_collation) GETSTRUCT(tmp))->collnamespace)),
										 makeString(pstrdup(NameStr(((Form_pg_collation) GETSTRUCT(tmp))->collname))));
			ReleaseSysCache(tmp);
		}

		spec->partParams = lappend(spec->partParams, elem);
	}

	ReleaseSysCache(tuple);
	return spec;
}

/*
 * The partition an ALTER TABLE command names, by name, by a value it holds,
 * or as the default one: Cloudberry's GpFindTargetPartition.
 */
static Oid
find_target(Relation parent, GpPartParser *p, GpPartId *id, bool missing_ok,
			const char *queryString)
{
	PartitionDesc partdesc = RelationGetPartitionDesc(parent, false);
	Oid			target = InvalidOid;

	switch (id->kind)
	{
		case GP_PART_ID_DEFAULT:
			target = get_default_oid_from_partdesc(partdesc);
			if (!OidIsValid(target) && !missing_ok)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("DEFAULT partition of relation \"%s\" does not exist",
								RelationGetRelationName(parent))));
			break;

		case GP_PART_ID_NAME:
			{
				/*
				 * By the name Cloudberry gives it, <parent>_<level>_prt_<name>,
				 * in the parent's schema; a partition renamed by hand, or made
				 * with PostgreSQL's syntax, is found by FOR (value).
				 */
				char		levelstr[NAMEDATALEN];
				char		partsubstring[NAMEDATALEN];
				RangeVar   *rv;
				Oid			relid = InvalidOid;

				/*
				 * A name longer than choose_partition_name lets a partition
				 * have is none's, and makeObjectName would assert on it, which
				 * Cloudberry's GpFindTargetPartition does not check for.
				 */
				if (strlen(id->name) <= NAMEDATALEN - 8)
				{
					snprintf(levelstr, NAMEDATALEN, "%d", partition_level(RelationGetRelid(parent)));
					snprintf(partsubstring, NAMEDATALEN, "prt_%s", id->name);
					rv = makeRangeVar(get_namespace_name(RelationGetNamespace(parent)),
									  makeObjectName(RelationGetRelationName(parent), levelstr,
													 partsubstring),
									  -1);

					/* no such relation is "relation "s.t_1_prt_x" does not exist" */
					relid = RangeVarGetRelid(rv, AccessShareLock, missing_ok);
				}
				for (int i = 0; OidIsValid(relid) && i < partdesc->nparts; i++)
				{
					if (partdesc->oids[i] == relid)
						target = relid;
				}
				if (!OidIsValid(target) && !missing_ok)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("partition \"%s\" of \"%s\" does not exist",
									id->name, RelationGetRelationName(parent))));
			}
			break;

		case GP_PART_ID_VALUE:
			{
				PartitionKey key = RelationGetPartitionKey(parent);
				PartitionBoundInfo boundinfo = partdesc->boundinfo;
				ParseState *pstate = make_pstate(queryString);
				List	   *vals = GpPartExprList(p, id->values, true);
				Datum		values[PARTITION_MAX_KEYS];
				bool		isnull[PARTITION_MAX_KEYS];
				int			partidx = -1;
				int			i = 0;
				ListCell   *lc;

				if (list_length(vals) > RelationGetDescr(parent)->natts ||
					list_length(vals) > key->partnatts)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("too many columns in boundary specification (%d > %d)",
									list_length(vals),
									Min(RelationGetDescr(parent)->natts, key->partnatts))));

				foreach(lc, vals)
				{
					Const	   *c = bound_value(pstate, lfirst(lc),
												key_column_name(parent, i),
												get_partition_col_typid(key, i),
												get_partition_col_typmod(key, i),
												get_partition_col_collation(key, i));

					values[i] = c->constvalue;
					isnull[i] = c->constisnull;
					i++;
				}
				for (; i < key->partnatts; i++)
				{
					values[i] = (Datum) 0;
					isnull[i] = true;
				}

				/* get_partition_for_tuple, which is static in PostgreSQL 19 */
				switch (key->strategy)
				{
					case PARTITION_STRATEGY_HASH:
						{
							uint64		rowHash = compute_partition_hash_value(key->partnatts,
																			   key->partsupfunc,
																			   key->partcollation,
																			   values, isnull);

							partidx = boundinfo->indexes[rowHash % boundinfo->nindexes];
						}
						break;
					case PARTITION_STRATEGY_LIST:
						if (isnull[0])
						{
							if (partition_bound_accepts_nulls(boundinfo))
								partidx = boundinfo->null_index;
						}
						else
						{
							bool		equal;
							int			off = partition_list_bsearch(key->partsupfunc,
																	 key->partcollation,
																	 boundinfo,
																	 values[0], &equal);

							if (off >= 0 && equal)
								partidx = boundinfo->indexes[off];
						}
						break;
					case PARTITION_STRATEGY_RANGE:
						{
							bool		hasnull = false;

							for (i = 0; i < key->partnatts; i++)
								hasnull |= isnull[i];
							if (!hasnull)
							{
								bool		equal;
								int			off = partition_range_datum_bsearch(key->partsupfunc,
																				key->partcollation,
																				boundinfo,
																				key->partnatts,
																				values, &equal);

								partidx = boundinfo->indexes[off + 1];
							}
						}
						break;
				}
				if (partidx < 0 && key->strategy != PARTITION_STRATEGY_HASH)
					partidx = boundinfo->default_index;

				if (partidx < 0)
				{
					if (missing_ok)
						break;
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("partition for specified value of %s does not exist",
									RelationGetRelationName(parent))));
				}

				if (partdesc->oids[partidx] == get_default_oid_from_partdesc(partdesc))
					ereport(ERROR,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("FOR expression matches DEFAULT partition for specified value of relation \"%s\"",
									RelationGetRelationName(parent)),
							 errhint("FOR expression may only specify a non-default partition in this context.")));

				target = partdesc->oids[partidx];
			}
			break;
	}

	return target;
}

/* ------------------------------------------------------------------------- */
/* The partitions of a partition list                                        */
/* ------------------------------------------------------------------------- */

/*
 * WITH (appendonly = ..., orientation = ...), Greenplum's way of choosing a
 * table's storage, taken out of an option list and returned as the access
 * method it names -- heap for appendonly=false, and ao_row or ao_column,
 * which M5 brings, for true -- or accessMethod, which comes first.
 * Cloudberry's grammar does this for every CREATE TABLE and every partition
 * (gram.y's greenplumLegacyAOoptions); here, for the partitions of the
 * classic clause and the table it is on.
 */
char *
GpPartitionLegacyAccessMethod(const char *accessMethod, List **options)
{
	List	   *amended = NIL;
	bool		appendoptimized = false;
	bool		is_column_oriented = false;
	bool		appendoptimized_found = false;
	bool		is_column_oriented_found = false;
	ListCell   *lc;

	foreach(lc, *options)
	{
		DefElem    *elem = lfirst_node(DefElem, lc);

		if (elem->defnamespace == NULL &&
			(strcmp(elem->defname, "appendoptimized") == 0 ||
			 strcmp(elem->defname, "appendonly") == 0))
		{
			if (appendoptimized_found)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("parameter \"appendonly\" specified more than once")));
			appendoptimized = defGetBoolean(elem);
			appendoptimized_found = true;
		}
		else if (elem->defnamespace == NULL &&
				 strcmp(elem->defname, "orientation") == 0)
		{
			const char *value = defGetString(elem);

			if (is_column_oriented_found)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("parameter \"orientation\" specified more than once")));
			if (strcmp(value, "column") != 0 && strcmp(value, "row") != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid parameter value for \"orientation\": \"%s\"", value)));
			is_column_oriented = strcmp(value, "column") == 0;
			is_column_oriented_found = true;
		}
		else
			amended = lappend(amended, elem);
	}
	*options = amended;

	if (!appendoptimized && is_column_oriented_found)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("invalid option \"orientation\" for base relation"),
				 errhint("Table orientation only valid for Append Optimized relations, create an AO relation to use table orientation.")));

	if (accessMethod)
		return pstrdup(accessMethod);
	if (appendoptimized && is_column_oriented)
		return pstrdup("ao_column");
	if (appendoptimized)
		return pstrdup("ao_row");
	/* appendonly=false is heap, where no appendonly at all is the default */
	if (appendoptimized_found)
		return pstrdup("heap");
	return NULL;
}

/*
 * The options of an element: WITH (...), less tablename, which names it, and
 * the storage it chooses (*am).
 */
static List *
elem_options(GpPartParser *p, GpPartElem *elem, char **tablename, char **am)
{
	List	   *options = NIL;
	ListCell   *lc;

	*tablename = NULL;
	*am = NULL;
	if (elem->with.from < 0)
		return NIL;

	foreach(lc, GpPartWithOptions(p, elem->with))
	{
		DefElem    *def = lfirst_node(DefElem, lc);

		/*
		 * WITH (tablename = 'x') names the partition itself: how Greenplum's
		 * old dump wrote a partition, and still accepted.
		 */
		if (def->defnamespace == NULL && strcmp(def->defname, "tablename") == 0)
		{
			if (def->arg == NULL || !IsA(def->arg, String))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("invalid tablename specification")));
			*tablename = pstrdup(defGetString(def));
			continue;
		}
		options = lappend(options, def);
	}
	*am = GpPartitionLegacyAccessMethod(NULL, &options);
	return options;
}

static PartChild *
new_child(Relation parentrel, const char *partname, GpPartElem *elem,
		  GpPartParser *p, int *partnum, const char *tablename, int level)
{
	PartChild  *c = palloc0(sizeof(PartChild));

	/*
	 * Every partition takes a number, named or not, so that the unnamed ones
	 * are numbered as Greenplum 6 numbered them: a default partition, which
	 * is always named, is first and takes 1.
	 */
	(*partnum)++;
	if (tablename != NULL)
		c->relname = pstrdup(tablename);
	else
		c->relname = choose_partition_name(RelationGetRelationName(parentrel),
										   level,
										   RelationGetNamespace(parentrel),
										   partname, *partnum);
	c->strategy = RelationGetPartitionKey(parentrel)->strategy;
	c->location = (elem != NULL) ? GpPartLocation(p, elem->location) : -1;
	return c;
}

/* START (...) END (...) EVERY (...): one partition, or one for each step */
static List *
range_children(ParseState *pstate, Relation parentrel, GpPartParser *p,
			   GpPartElem *elem, int *partnum, const char *tablename,
			   int level, bool *implicit)
{
	GpPartBound *bound = elem->bound;
	PartitionKey partkey = RelationGetPartitionKey(parentrel);
	PartEveryIterator *iter;
	Node	   *start = NULL;
	Node	   *end = NULL;
	Node	   *every = NULL;
	char	   *partcolname;
	List	   *result = NIL;
	int			i = 0;

	if (bound == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("missing boundary specification in partition \"%s\" of type RANGE",
						elem->name),
				 parser_errposition(pstate, GpPartLocation(p, elem->location))));
	if (bound->kind != GP_PART_BOUND_RANGE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("invalid boundary specification for RANGE partition"),
				 parser_errposition(pstate, GpPartLocation(p, elem->location))));

	/*
	 * Only one column: a multi-column range key is written with PostgreSQL's
	 * syntax, as Cloudberry says.
	 */
	if (partkey->partnatts != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("too many columns for RANGE partition -- only one column is allowed")));
	partcolname = key_column_name(parentrel, 0);

	if (bound->start)
	{
		List	   *vals = GpPartExprList(p, bound->start->vals, false);

		if (list_length(vals) != partkey->partnatts)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("number of START values should cover all partition key columns"),
					 parser_errposition(pstate, GpPartLocation(p, bound->start->location))));
		start = linitial(vals);
	}
	else
		*implicit = true;

	if (bound->end)
	{
		List	   *vals = GpPartExprList(p, bound->end->vals, false);

		if (list_length(vals) != partkey->partnatts)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("number of END values should cover all partition key columns"),
					 parser_errposition(pstate, GpPartLocation(p, bound->end->location))));
		end = linitial(vals);
	}
	else
		*implicit = true;

	/*
	 * WITH (tablename = ...) is how a partition was dumped, one to a
	 * statement, and EVERY is then ignored, as it always was.
	 */
	if (tablename == NULL && bound->every.from >= 0)
	{
		List	   *vals = GpPartExprList(p, bound->every, false);

		if (list_length(vals) != partkey->partnatts)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("number of EVERY values should cover all partition key columns"),
					 parser_errposition(pstate, GpPartLocation(p, bound->location))));
		every = linitial(vals);
	}

	iter = init_every_iterator(pstate, partkey, partcolname,
							   start, bound->start && !bound->start->inclusive,
							   end, bound->end && bound->end->inclusive,
							   every);

	while (next_part_bound(iter))
	{
		PartChild  *c;
		char	   *partname = elem->name;

		if (every && elem->name)
			partname = psprintf("%s_%d", elem->name, ++i);

		c = new_child(parentrel, partname, elem, p, partnum, tablename, level);
		if (start)
			c->lower = list_make1(key_const(partkey, iter->currStart));
		if (end && iter->endReached && iter->isEndValMaxValue)
			c->upper = list_make1(infinite_bound("maxvalue"));
		else if (end)
			c->upper = list_make1(key_const(partkey, iter->currEnd));
		result = lappend(result, c);
	}

	free_every_iterator(iter);
	return result;
}

/* VALUES (...) */
static PartChild *
list_child(ParseState *pstate, Relation parentrel, GpPartParser *p,
		   GpPartElem *elem, int *partnum, const char *tablename, int level)
{
	PartChild  *c;
	PartitionBoundSpec *check;
	ListCell   *lc;

	if (elem->bound == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("missing boundary specification in partition \"%s\" of type LIST",
						elem->name),
				 parser_errposition(pstate, GpPartLocation(p, elem->location))));
	if (elem->bound->kind != GP_PART_BOUND_LIST)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("invalid boundary specification for LIST partition"),
				 parser_errposition(pstate, GpPartLocation(p, elem->location))));

	c = new_child(parentrel, elem->name, elem, p, partnum, tablename, level);
	foreach(lc, GpPartExprList(p, elem->bound->values, true))
	{
		Node	   *v = lfirst(lc);

		if (IsA(v, RowExpr))
			elog(ERROR, "VALUES specification with more than one column not allowed");
		c->listdatums = lappend(c->listdatums, v);
	}

	/*
	 * The values are read against the key now, as Cloudberry reads them, so
	 * that a value of the wrong type is refused before any partition is made
	 * -- and a template is checked against the partitions it will be used
	 * for (SET SUBPARTITION TEMPLATE).  What the partition is made with is
	 * what the user wrote, which PostgreSQL reads again.
	 */
	check = makeNode(PartitionBoundSpec);
	check->strategy = PARTITION_STRATEGY_LIST;
	check->listdatums = copyObject(c->listdatums);
	check->location = -1;
	(void) transformPartitionBound(pstate, parentrel, check);
	return c;
}

/*
 * The comparison Cloudberry sorts a range partition list's partitions by
 * before filling in the bounds that were left out: by START, or END where
 * there is none, the default partition last.
 */
static PartitionKey sort_key;

static int
range_compare(const PartChild *b1, const PartChild *b2)
{
	int32		cmpval = 0;
	PartitionKey key = sort_key;

	if (b1->is_default != b2->is_default)
		return b2->is_default ? -1 : 1;

	if (b1->lower != NIL && b2->lower != NIL)
	{
		for (int i = 0; i < key->partnatts; i++)
		{
			cmpval = DatumGetInt32(FunctionCall2Coll(&key->partsupfunc[i],
													 key->partcollation[i],
													 castNode(Const, list_nth(b1->lower, i))->constvalue,
													 castNode(Const, list_nth(b2->lower, i))->constvalue));
			if (cmpval != 0)
				break;
		}
	}
	else if (b1->upper != NIL && b2->upper != NIL)
	{
		for (int i = 0; i < key->partnatts; i++)
		{
			Node	   *n1 = list_nth(b1->upper, i);
			Node	   *n2 = list_nth(b2->upper, i);

			if (!IsA(n1, Const))
				return 1;		/* maxvalue */
			if (!IsA(n2, Const))
				return -1;
			cmpval = DatumGetInt32(FunctionCall2Coll(&key->partsupfunc[i],
													 key->partcollation[i],
													 ((Const *) n1)->constvalue,
													 ((Const *) n2)->constvalue));
			if (cmpval != 0)
				break;
		}
	}
	else if (b1->lower != NIL && b2->upper != NIL)
	{
		for (int i = 0; i < key->partnatts; i++)
		{
			Node	   *n2 = list_nth(b2->upper, i);

			if (!IsA(n2, Const))
				return -1;
			cmpval = DatumGetInt32(FunctionCall2Coll(&key->partsupfunc[i],
													 key->partcollation[i],
													 castNode(Const, list_nth(b1->lower, i))->constvalue,
													 ((Const *) n2)->constvalue));
			if (cmpval != 0)
				break;
		}

		/*
		 * b1 starting where b2 ends goes after it, so that b1's start can be
		 * taken from b2's end.
		 */
		if (cmpval == 0)
			cmpval = 1;
	}
	else if (b1->upper != NIL && b2->lower != NIL)
	{
		for (int i = 0; i < key->partnatts; i++)
		{
			Node	   *n1 = list_nth(b1->upper, i);

			if (!IsA(n1, Const))
				return 1;
			cmpval = DatumGetInt32(FunctionCall2Coll(&key->partsupfunc[i],
													 key->partcollation[i],
													 ((Const *) n1)->constvalue,
													 castNode(Const, list_nth(b2->lower, i))->constvalue));
			if (cmpval != 0)
				break;
		}
	}

	return cmpval;
}

static int
part_range_cmp(const ListCell *a, const ListCell *b)
{
	return range_compare(lfirst(a), lfirst(b));
}

/*
 * Fill in the START or END a range partition left out: Cloudberry's
 * deduceImplicitRangeBounds.  For CREATE TABLE, from the partitions next to
 * it in the same list, or MINVALUE and MAXVALUE at the ends; for ALTER TABLE
 * ... ADD PARTITION, which adds one, from the partitions already there.
 */
static List *
deduce_range_bounds(ParseState *pstate, Relation parentrel, List *children,
					bool alter)
{
	PartitionKey key = RelationGetPartitionKey(parentrel);
	PartitionDesc desc = RelationGetPartitionDesc(parentrel, true);

	sort_key = key;
	list_sort(children, part_range_cmp);

	if (!alter)
	{
		PartChild  *prev = NULL;
		ListCell   *lc;

		foreach(lc, children)
		{
			PartChild  *c = lfirst(lc);

			if (c->is_default)
				continue;

			if (c->lower == NIL)
			{
				if (prev != NULL)
				{
					if (prev->upper != NIL)
						c->lower = prev->upper;
					else
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
								 errmsg("cannot derive starting value of partition based upon ending of previous partition"),
								 parser_errposition(pstate, c->location)));
				}
				else
					c->lower = list_make1(infinite_bound("minvalue"));
			}
			if (c->upper == NIL)
			{
				PartChild  *next = lnext(children, lc) ? lfirst(lnext(children, lc)) : NULL;

				if (next != NULL)
				{
					if (next->lower != NIL)
						c->upper = next->lower;
					else
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
								 errmsg("cannot derive ending value of partition based upon starting of next partition"),
								 parser_errposition(pstate, c->location)));
				}
				else
					c->upper = list_make1(infinite_bound("maxvalue"));
			}
			prev = c;
		}
	}
	else
	{
		PartChild  *c;

		if (list_length(children) != 1)
			elog(ERROR, "cannot add more than one partition to existing partitioned table in one command");
		c = linitial(children);

		if (!c->is_default)
		{
			if (c->lower == NIL && c->upper == NIL)
				elog(ERROR, "must specify partition bounds");

			if (c->lower == NIL)
			{
				Datum		upper = castNode(Const, linitial(c->upper))->constvalue;
				bool		equal;
				int			off;

				/* the highest existing bound at or below the new END */
				off = partition_range_datum_bsearch(key->partsupfunc,
													key->partcollation,
													desc->boundinfo,
													key->partnatts,
													&upper, &equal);
				if (off != -1 && !equal &&
					desc->boundinfo->kind[off][0] == PARTITION_RANGE_DATUM_VALUE)
					c->lower = list_make1(key_const(key, desc->boundinfo->datums[off][0]));
				else
					c->lower = list_make1(infinite_bound("minvalue"));
			}

			if (c->upper == NIL)
			{
				Datum		lower = castNode(Const, linitial(c->lower))->constvalue;
				bool		equal;
				int			off;

				/* the lowest existing bound above the new START */
				off = partition_range_datum_bsearch(key->partsupfunc,
													key->partcollation,
													desc->boundinfo,
													key->partnatts,
													&lower, &equal);
				off++;
				if (off < desc->boundinfo->ndatums &&
					desc->boundinfo->kind[off][0] == PARTITION_RANGE_DATUM_VALUE)
					c->upper = list_make1(key_const(key, desc->boundinfo->datums[off][0]));
				else
					c->upper = list_make1(infinite_bound("maxvalue"));
			}
		}
	}

	return children;
}

/*
 * The partitions a partition list describes, of parentrel, named but not yet
 * made: Cloudberry's generatePartitions.  `sub` is the key of their own
 * partitions, and the template that gives them, if any.
 */
static List *
make_children(Relation parentrel, GpPartParser *p, GpPartDef *def,
			  PartLevel *sub, bool alter, const char *queryString)
{
	ParseState *pstate = make_pstate(queryString);
	int			level = partition_level(RelationGetRelid(parentrel));
	PartitionKey key = RelationGetPartitionKey(parentrel);
	List	   *elems = NIL;
	List	   *result = NIL;
	GpPartElem *defaultelem = NULL;
	bool		implicit = false;
	int			partnum = 0;
	ListCell   *lc;

	if (gp_max_partition_level > 0 && level > gp_max_partition_level)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("Exceeds maximum configured partitioning level of %d",
						gp_max_partition_level)));

	/*
	 * An append-optimized, column-oriented table's column encodings are M5's;
	 * on the heap they mean nothing, as Cloudberry says.
	 */
	if (def->encodings != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ENCODING clause only supported with column oriented tables")));

	/*
	 * The default partition first, as Greenplum 6 numbered it, so that the
	 * others are numbered as they always were.
	 */
	foreach(lc, def->elems)
	{
		GpPartElem *elem = lfirst(lc);

		if (elem->is_default)
		{
			if (defaultelem != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("multiple default partitions are not allowed"),
						 parser_errposition(pstate, GpPartLocation(p, elem->location))));
			defaultelem = elem;
			elems = lcons(elem, elems);
		}
		else
			elems = lappend(elems, elem);
	}

	foreach(lc, elems)
	{
		GpPartElem *elem = lfirst(lc);
		GpPartParser *subp = NULL;
		GpPartDef  *subdef = NULL;
		List	   *options;
		char	   *tablename;
		char	   *am;
		List	   *children;
		ListCell   *lc2;

		if (sub != NULL)
		{
			if (sub->template_def != NULL)
			{
				if (elem->subparts != NULL)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("subpartition configuration conflicts with subpartition template"),
							 parser_errposition(pstate, GpPartLocation(p, elem->subparts->location))));
				subp = sub->tp;
				subdef = sub->template_def;
			}
			else
			{
				subp = p;
				subdef = elem->subparts;
			}
			if (subdef == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("no partitions specified at depth %d", level + 1),
						 parser_errposition(pstate, sub->location)));
		}
		else if (elem->subparts != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("subpartition specification provided but table doesn't have SUBPARTITION BY clause"),
					 parser_errposition(pstate, GpPartLocation(p, elem->subparts->location))));

		if (def->is_template && elem->encodings != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("partition specific ENCODING clause not supported in SUBPARTITION TEMPLATE"),
					 parser_errposition(pstate, GpPartLocation(p, elem->location))));
		if (elem->encodings != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("ENCODING clause only supported with column oriented tables")));

		options = elem_options(p, elem, &tablename, &am);

		if (elem->is_default)
		{
			PartChild  *c = new_child(parentrel, elem->name, elem, p, &partnum,
									  tablename, level);

			c->is_default = true;
			children = list_make1(c);
		}
		else if (key->strategy == PARTITION_STRATEGY_RANGE)
			children = range_children(pstate, parentrel, p, elem, &partnum,
									  tablename, level, &implicit);
		else if (key->strategy == PARTITION_STRATEGY_LIST)
			children = list_make1(list_child(pstate, parentrel, p, elem,
											 &partnum, tablename, level));
		else
			elog(ERROR, "Not supported partition strategy");

		foreach(lc2, children)
		{
			PartChild  *c = lfirst(lc2);

			c->options = copyObject(options);
			c->accessmethod = am;
			c->tablespace = elem->tablespace;
			c->subp = subp;
			c->subdef = subdef;
			c->sublevel = sub;
		}
		result = list_concat(result, children);
	}

	if (implicit)
		result = deduce_range_bounds(pstate, parentrel, result, alter);

	free_parsestate(pstate);
	return result;
}


/* ------------------------------------------------------------------------- */
/* Making them                                                               */
/* ------------------------------------------------------------------------- */

/*
 * What a partition needs of its parent, taken while the parent is open.  No
 * relation of this file's may be open while a statement it makes runs:
 * CREATE TABLE ... PARTITION OF, DETACH and ATTACH refuse a table the session
 * is using (CheckTableNotInUse), which Cloudberry avoids by not checking for
 * the partitions it makes (its tablecmds.c, ORIGIN_GP_CLASSIC_ALTER_GEN).
 */
typedef struct PartParent
{
	Oid			relid;
	char	   *relname;
	char	   *nspname;
	char		relpersistence;
	Oid			relowner;
	char	   *policy;			/* its "gp" label's distributed_by, or NULL */
	char	   *numsegments;	/* and numsegments, NULL for every segment */
} PartParent;

static PartParent *
parent_of(Relation rel)
{
	PartParent *pp = palloc0(sizeof(PartParent));
	ObjectAddress addr;

	pp->relid = RelationGetRelid(rel);
	pp->relname = pstrdup(RelationGetRelationName(rel));
	pp->nspname = get_namespace_name(RelationGetNamespace(rel));
	pp->relpersistence = rel->rd_rel->relpersistence;
	pp->relowner = rel->rd_rel->relowner;
	ObjectAddressSet(addr, RelationRelationId, pp->relid);
	pp->policy = GpLabelGet(&addr, GP_LABEL_distributed_by);
	pp->numsegments = GpLabelGet(&addr, GP_LABEL_numsegments);
	return pp;
}

static RangeVar *
parent_rv(const PartParent *pp)
{
	RangeVar   *rv = makeRangeVar(pstrdup(pp->nspname), pstrdup(pp->relname), -1);

	rv->relpersistence = pp->relpersistence;
	return rv;
}

/* A relation by its OID, as a statement names it. */
static RangeVar *
rv_of(Oid relid)
{
	RangeVar   *rv = makeRangeVar(get_namespace_name(get_rel_namespace(relid)),
								  get_rel_name(relid), -1);

	rv->relpersistence = get_rel_persistence(relid);
	return rv;
}

static Oid
relation_owner(Oid relid)
{
	HeapTuple	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	Oid			owner;

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	owner = ((Form_pg_class) GETSTRUCT(tuple))->relowner;
	ReleaseSysCache(tuple);
	return owner;
}

/*
 * Give a new partition its parent's privileges, the table's and its columns',
 * as Cloudberry's DefineRelation does (CopyRelationAcls): PostgreSQL gives a
 * new partition none.  The parent's owner is the partition's, so its ACL can
 * be taken as it is.  A column is matched by name, not number: a parent
 * with a dropped column numbers its columns differently from a partition
 * made after the drop, which Cloudberry's copy did not allow for.  What has
 * privileges already -- a default ACL, or this having run -- keeps them.
 */
static void
copy_acls(Oid srcId, Oid destId)
{
	Relation	classrel = table_open(RelationRelationId, RowExclusiveLock);
	Relation	attrel = table_open(AttributeRelationId, RowExclusiveLock);
	HeapTuple	srcTuple = SearchSysCache1(RELOID, ObjectIdGetDatum(srcId));
	HeapTuple	destTuple;
	Oid			ownerId;
	Datum		aclDatum;
	bool		isNull;
	bool		destNull;
	CatCList   *attlist;

	if (!HeapTupleIsValid(srcTuple))
		elog(ERROR, "cache lookup failed for relation %u", srcId);
	destTuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(destId));
	if (!HeapTupleIsValid(destTuple))
		elog(ERROR, "cache lookup failed for relation %u", destId);
	ownerId = ((Form_pg_class) GETSTRUCT(destTuple))->relowner;

	aclDatum = SysCacheGetAttr(RELOID, srcTuple, Anum_pg_class_relacl, &isNull);
	(void) SysCacheGetAttr(RELOID, destTuple, Anum_pg_class_relacl, &destNull);
	if (!isNull && destNull)
	{
		Acl		   *acl = DatumGetAclPCopy(aclDatum);
		Datum		values[Natts_pg_class] = {0};
		bool		nulls[Natts_pg_class] = {0};
		bool		replaces[Natts_pg_class] = {0};
		HeapTuple	newTuple;
		Oid		   *newmembers;
		int			nnewmembers;

		replaces[Anum_pg_class_relacl - 1] = true;
		values[Anum_pg_class_relacl - 1] = PointerGetDatum(acl);
		newTuple = heap_modify_tuple(destTuple, RelationGetDescr(classrel),
									 values, nulls, replaces);
		CatalogTupleUpdate(classrel, &newTuple->t_self, newTuple);

		nnewmembers = aclmembers(acl, &newmembers);
		updateAclDependencies(RelationRelationId, destId, 0, ownerId,
							  0, NULL, nnewmembers, newmembers);
	}

	attlist = SearchSysCacheList1(ATTNUM, ObjectIdGetDatum(srcId));
	for (int i = 0; i < attlist->n_members; i++)
	{
		HeapTuple	attSrcTuple = &attlist->members[i]->tuple;
		Form_pg_attribute attSrc = (Form_pg_attribute) GETSTRUCT(attSrcTuple);
		AttrNumber	destnum;
		HeapTuple	attDestTuple;
		Acl		   *acl;
		Datum		values[Natts_pg_attribute] = {0};
		bool		nulls[Natts_pg_attribute] = {0};
		bool		replaces[Natts_pg_attribute] = {0};
		HeapTuple	newTuple;
		Oid		   *newmembers;
		int			nnewmembers;

		if (attSrc->attnum <= 0 || attSrc->attisdropped)
			continue;
		aclDatum = SysCacheGetAttr(ATTNUM, attSrcTuple, Anum_pg_attribute_attacl,
								   &isNull);
		if (isNull)
			continue;
		destnum = get_attnum(destId, NameStr(attSrc->attname));
		if (destnum == InvalidAttrNumber)
			continue;

		acl = DatumGetAclPCopy(aclDatum);
		attDestTuple = SearchSysCacheCopy2(ATTNUM, ObjectIdGetDatum(destId),
										   Int16GetDatum(destnum));
		if (!HeapTupleIsValid(attDestTuple))
			elog(ERROR, "cache lookup failed for attribute %d of relation %u",
				 destnum, destId);
		(void) SysCacheGetAttr(ATTNUM, attDestTuple, Anum_pg_attribute_attacl,
							   &destNull);
		if (!destNull)
			continue;

		replaces[Anum_pg_attribute_attacl - 1] = true;
		values[Anum_pg_attribute_attacl - 1] = PointerGetDatum(acl);
		newTuple = heap_modify_tuple(attDestTuple, RelationGetDescr(attrel),
									 values, nulls, replaces);
		CatalogTupleUpdate(attrel, &newTuple->t_self, newTuple);

		nnewmembers = aclmembers(acl, &newmembers);
		updateAclDependencies(RelationRelationId, destId, destnum, ownerId,
							  0, NULL, nnewmembers, newmembers);
	}
	ReleaseSysCacheList(attlist);

	ReleaseSysCache(srcTuple);
	table_close(attrel, RowExclusiveLock);
	table_close(classrel, RowExclusiveLock);

	CommandCounterIncrement();
}

/*
 * CREATE TABLE ... PARTITION OF parent, in PostgreSQL's syntax, has just run:
 * a partition of a table of Cloudberry's gets the parent's privileges, as
 * every partition does in Cloudberry.
 */
void
GpPartitionMade(CreateStmt *stmt)
{
	Oid			parentid;
	Oid			childid;

	if (stmt->partbound == NULL || list_length(stmt->inhRelations) != 1)
		return;
	parentid = RangeVarGetRelid(linitial_node(RangeVar, stmt->inhRelations), NoLock, true);
	childid = RangeVarGetRelid(stmt->relation, NoLock, true);
	if (!OidIsValid(parentid) || !OidIsValid(childid) ||
		!get_rel_relispartition(childid) ||
		get_partition_parent(childid, true) != parentid ||
		!GpPartitionIsClassic(parentid))
		return;
	copy_acls(parentid, childid);
}

/*
 * Make one partition: CREATE TABLE child PARTITION OF parent FOR VALUES ...,
 * with the parent's persistence, owned by the parent's owner as Cloudberry's
 * are, and distributed as the parent is.
 */
static Oid
create_child(const PartParent *parent, PartChild *c, const char *queryString,
			 QueryEnvironment *queryEnv)
{
	CreateStmt *cs = makeNode(CreateStmt);
	PartitionBoundSpec *bound = makeNode(PartitionBoundSpec);
	Oid			relid;

	bound->strategy = c->strategy;
	bound->is_default = c->is_default;
	bound->listdatums = c->listdatums;
	bound->lowerdatums = bound_to_raw(c->lower);
	bound->upperdatums = bound_to_raw(c->upper);
	bound->location = (c->strategy == PARTITION_STRATEGY_LIST) ? -1 : c->location;

	cs->relation = makeRangeVar(pstrdup(parent->nspname), c->relname, -1);
	cs->relation->relpersistence = parent->relpersistence;
	cs->inhRelations = list_make1(parent_rv(parent));
	cs->partbound = bound;
	cs->partspec = (c->sublevel != NULL) ? copyObject(c->sublevel->spec) : NULL;
	cs->options = c->options;
	cs->oncommit = ONCOMMIT_NOOP;
	cs->tablespacename = c->tablespace;
	cs->accessMethod = c->accessmethod;
	cs->if_not_exists = false;

	run_utility((Node *) cs, queryString, queryEnv);

	relid = RangeVarGetRelid(cs->relation, NoLock, false);

	/* Cloudberry's partitions are the parent's owner's, whoever makes them */
	if (relation_owner(relid) != parent->relowner)
	{
		AlterTableStmt *at = makeNode(AlterTableStmt);
		AlterTableCmd *cmd = makeNode(AlterTableCmd);
		RoleSpec   *owner = makeNode(RoleSpec);

		owner->roletype = ROLESPEC_CSTRING;
		owner->rolename = GetUserNameFromId(parent->relowner, false);
		owner->location = -1;
		cmd->subtype = AT_ChangeOwner;
		cmd->newowner = owner;
		at->relation = makeRangeVar(pstrdup(parent->nspname), c->relname, -1);
		at->cmds = list_make1(cmd);
		at->objtype = OBJECT_TABLE;
		run_utility((Node *) at, queryString, queryEnv);
	}

	/* and distributed as the parent is (make_distributedby_for_rel) */
	if (parent->policy != NULL)
	{
		ObjectAddress addr;

		ObjectAddressSet(addr, RelationRelationId, relid);
		GpLabelSet(&addr, GP_LABEL_distributed_by, parent->policy);
		/* over the parent's segments, as a partial parent's partitions are */
		if (parent->numsegments != NULL)
			GpLabelSet(&addr, GP_LABEL_numsegments, parent->numsegments);
	}

	/* with the parent's privileges */
	copy_acls(parent->relid, relid);

	return relid;
}

/*
 * Make the partitions of a partitioned table, and theirs, a level at a time:
 * every partition of one level before the partitions of the next, in the
 * order Cloudberry's utility loop makes them.
 */
static void
create_all(PartTodo *first, const char *queryString, QueryEnvironment *queryEnv)
{
	List	   *todo = list_make1(first);

	while (todo != NIL)
	{
		PartTodo   *t = linitial(todo);
		Relation	parentrel;
		PartParent *parent;
		List	   *children;
		ListCell   *lc;

		todo = list_delete_first(todo);

		parentrel = table_open(t->relid, NoLock);
		children = make_children(parentrel, t->p, t->def, t->sub, t->alter,
								 queryString);
		parent = parent_of(parentrel);
		table_close(parentrel, NoLock);

		foreach(lc, children)
		{
			PartChild  *c = lfirst(lc);
			Oid			relid = create_child(parent, c, queryString, queryEnv);

			if (c->sublevel != NULL)
			{
				PartTodo   *next = palloc0(sizeof(PartTodo));

				next->relid = relid;
				next->p = c->subp;
				next->def = c->subdef;
				next->sub = c->sublevel->next;
				next->alter = false;
				todo = lappend(todo, next);
			}
		}
	}
}

/* ------------------------------------------------------------------------- */
/* CREATE TABLE                                                              */
/* ------------------------------------------------------------------------- */

/*
 * The partitions of a table CREATE TABLE ... PARTITION BY ... (...) has just
 * made, from its gp.partition_by option.
 */
void
GpPartitionCreate(Oid relid, DefElem *option, const char *queryString,
				  QueryEnvironment *queryEnv)
{
	GpPartParser *p = option_parser(option, queryString);
	GpPartClause *clause = GpPartParseClause(p, 0);
	PartTodo   *todo;
	PartLevel  *levels;
	int			level = partition_level(relid);

	if (clause == NULL || clause->def == NULL)
		elog(ERROR, "malformed gp.%s option", GP_PARTITION_BY_OPTION);

	/*
	 * Each level's template, kept with the root under the level whose
	 * partitions it gives partitions to.
	 */
	levels = levels_of_key(p, clause->key->sub);
	for (PartLevel *l = levels; l != NULL; l = l->next, level++)
	{
		if (l->template_def != NULL)
			(void) template_set(partition_root(relid), level,
								GpPartSpanText(p, l->template_def->text));
	}

	todo = palloc0(sizeof(PartTodo));
	todo->relid = relid;
	todo->p = p;
	todo->def = clause->def;
	todo->sub = levels;
	create_all(todo, queryString, queryEnv);
}

/* ------------------------------------------------------------------------- */
/* ALTER TABLE                                                               */
/* ------------------------------------------------------------------------- */

static void
check_partitioned(Relation rel)
{
	if (rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("table \"%s\" is not partitioned",
						RelationGetRelationName(rel))));
}

/* The bound a partition was made with, from the catalog. */
static PartitionBoundSpec *
stored_bound(Oid relid, List **reloptions)
{
	HeapTuple	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	PartitionBoundSpec *bound = NULL;
	Datum		datum;
	bool		isnull;

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	datum = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_relpartbound, &isnull);
	if (!isnull)
		bound = stringToNode(TextDatumGetCString(datum));
	if (reloptions != NULL)
	{
		datum = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions, &isnull);
		*reloptions = isnull ? NIL : untransformRelOptions(datum);
	}
	ReleaseSysCache(tuple);
	return bound;
}

/* The same, as raw expressions a partition can be made with. */
static PartitionBoundSpec *
raw_bound(PartitionBoundSpec *bound)
{
	PartitionBoundSpec *raw = copyObject(bound);

	raw->listdatums = bound_to_raw(bound->listdatums);
	raw->lowerdatums = bound_to_raw(bound->lowerdatums);
	raw->upperdatums = bound_to_raw(bound->upperdatums);
	raw->location = -1;
	return raw;
}

/* ADD [DEFAULT] PARTITION */
static void
cmd_add(Oid relid, GpPartParser *p, GpPartCmd *cmd, const char *queryString,
		QueryEnvironment *queryEnv)
{
	GpPartDef  *def = palloc0(sizeof(GpPartDef));
	PartLevel  *first = NULL;
	PartLevel  *last = NULL;
	Oid			rootid = partition_root(relid);
	int			level = partition_level(relid);
	Relation	temprel = table_open(relid, NoLock);
	PartTodo   *todo;

	def->elems = list_make1(cmd->elem);

	/*
	 * The key of each level below, from the first partition already there at
	 * each depth, and its template, if the hierarchy has one: the new
	 * partition is given its partitions as its siblings were.
	 */
	for (;;)
	{
		PartitionDesc partdesc = RelationGetPartitionDesc(temprel, false);
		PartLevel  *l;
		char	   *template_text;
		Oid			firstid;

		if (partdesc->nparts == 0)
			elog(ERROR, "GPDB add partition syntax needs at least one sibling to exist");
		if (partdesc->is_leaf[0])
			break;

		firstid = partdesc->oids[0];
		table_close(temprel, NoLock);
		temprel = table_open(firstid, AccessShareLock);

		l = palloc0(sizeof(PartLevel));
		l->spec = partition_spec_of(temprel);
		l->location = -1;
		template_text = template_get(rootid, level);
		if (template_text != NULL)
		{
			l->tp = text_parser(template_text);
			l->template_def = GpPartParseTemplate(l->tp, 0);
		}
		level++;
		if (last == NULL)
			first = l;
		else
			last->next = l;
		last = l;
	}
	table_close(temprel, NoLock);

	todo = palloc0(sizeof(PartTodo));
	todo->relid = relid;
	todo->p = p;
	todo->def = def;
	todo->sub = first;
	todo->alter = true;
	create_all(todo, queryString, queryEnv);
}

/* DROP [DEFAULT] PARTITION */
static void
cmd_drop(Oid relid, GpPartParser *p, GpPartCmd *cmd, const char *queryString,
		 QueryEnvironment *queryEnv)
{
	Relation	rel = table_open(relid, NoLock);
	Oid			partrelid = find_target(rel, p, cmd->id, cmd->missing_ok, queryString);
	DropStmt   *drop;

	if (!OidIsValid(partrelid))
	{
		table_close(rel, NoLock);
		return;
	}

	if (RelationGetPartitionDesc(rel, false)->nparts == 1)
		ereport(ERROR,
				(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				 errmsg("cannot drop partition \"%s\" of \"%s\" -- only one remains",
						get_rel_name(partrelid), RelationGetRelationName(rel)),
				 errhint("Use DROP TABLE \"%s\" to remove the table and the final partition ",
						 RelationGetRelationName(rel))));
	table_close(rel, NoLock);

	drop = makeNode(DropStmt);
	drop->objects = list_make1(list_make2(makeString(get_namespace_name(get_rel_namespace(partrelid))),
										  makeString(get_rel_name(partrelid))));
	drop->removeType = OBJECT_TABLE;
	drop->behavior = cmd->behavior;
	drop->missing_ok = cmd->missing_ok;
	run_utility((Node *) drop, queryString, queryEnv);
}

/* The partition a command names, found with its parent open. */
static Oid
target_of(Oid relid, GpPartParser *p, GpPartId *id, bool missing_ok,
		  const char *queryString)
{
	Relation	rel = table_open(relid, NoLock);
	Oid			target = find_target(rel, p, id, missing_ok, queryString);

	table_close(rel, NoLock);
	return target;
}

/* TRUNCATE [DEFAULT] PARTITION */
static void
cmd_truncate(Oid relid, GpPartParser *p, GpPartCmd *cmd,
			 const char *queryString, QueryEnvironment *queryEnv)
{
	TruncateStmt *trunc = makeNode(TruncateStmt);

	trunc->relations = list_make1(rv_of(target_of(relid, p, cmd->id, false, queryString)));
	trunc->restart_seqs = false;
	trunc->behavior = cmd->behavior;
	run_utility((Node *) trunc, queryString, queryEnv);
}

/*
 * Is this a partitioned table of Cloudberry's -- one with a partition named
 * as Cloudberry names one, <parent>_<level>_prt_<name>, the parent's name
 * perhaps shortened to fit?  What Cloudberry does to every partitioned table
 * -- its partitions renamed with it, GRANT reaching them -- the port does to
 * these, so that one made with PostgreSQL's syntax behaves as PostgreSQL's
 * does.  No PostgreSQL regression test names a partition so.
 */
bool
GpPartitionIsClassic(Oid relid)
{
	char	   *parentname;
	ListCell   *lc;

	if (get_rel_relkind(relid) != RELKIND_PARTITIONED_TABLE)
		return false;
	parentname = get_rel_name(relid);

	foreach(lc, find_inheritance_children(relid, NoLock))
	{
		char	   *name = get_rel_name(lfirst_oid(lc));
		char	   *prt;

		for (prt = strstr(name, "_prt_"); prt != NULL; prt = strstr(prt + 1, "_prt_"))
		{
			char	   *d = prt;

			/* _<digits> before it, and before that a start of the parent's name */
			while (d > name && isdigit((unsigned char) d[-1]))
				d--;
			if (d == prt || d == name || d[-1] != '_' || d - 1 == name)
				continue;
			if (strncmp(parentname, name, d - 1 - name) == 0)
				return true;
		}
	}
	return false;
}

/*
 * GRANT and REVOKE on a partitioned table of Cloudberry's reach every
 * partition below it, as Cloudberry's do (aclchk.c's objectNamesToOids),
 * unless the table is named with ONLY.  Returns the statement's objects with
 * the partitions added after each such table, or NIL if there is none.
 */
List *
GpPartitionGrantObjects(GrantStmt *stmt)
{
	List	   *result = NIL;
	bool		expanded = false;
	ListCell   *lc;

	if (stmt->objtype != OBJECT_TABLE || stmt->targtype != ACL_TARGET_OBJECT)
		return NIL;

	foreach(lc, stmt->objects)
	{
		RangeVar   *rv = lfirst_node(RangeVar, lc);
		Oid			relid;
		ListCell   *lc2;

		relid = rv->inh ? RangeVarGetRelid(rv, NoLock, true) : InvalidOid;
		if (!OidIsValid(relid) || !GpPartitionIsClassic(relid))
		{
			result = lappend(result, rv);
			continue;
		}

		result = lappend(result, rv);
		foreach(lc2, list_delete_first(find_all_inheritors(relid, NoLock, NULL)))
			result = lappend(result, rv_of(lfirst_oid(lc2)));
		expanded = true;
	}
	return expanded ? result : NIL;
}

/*
 * Rename the partitions below a partitioned table whose names begin with its
 * old name, as Cloudberry's RenameRelation does (GpRenameChildPartitions):
 * t_1_prt_a becomes t2_1_prt_a when t becomes t2.  Only for a hierarchy with
 * Cloudberry's names in it -- a partition called <parent>_<n>_prt_... -- so
 * that one made with PostgreSQL's syntax renames as it does in PostgreSQL.
 */
void
GpPartitionRenamed(Oid relid, const char *oldname, const char *newname)
{
	List	   *oids;
	List	   *names = NIL;
	int			oldlen = strlen(oldname);
	int			renamed = 1;
	int			skipped = 0;
	ListCell   *lc;
	ListCell   *lc2;

	/* its partitions are named for its old name */
	if (get_rel_relkind(relid) != RELKIND_PARTITIONED_TABLE)
		return;
	oids = list_delete_first(find_all_inheritors(relid, AccessExclusiveLock, NULL));
	foreach(lc, oids)
		names = lappend(names, get_rel_name(lfirst_oid(lc)));
	{
		bool		classic = false;

		foreach(lc, names)
		{
			const char *relname = lfirst(lc);
			const char *rest = relname + oldlen;

			if (strncmp(relname, oldname, oldlen) != 0 || rest[0] != '_' ||
				!isdigit((unsigned char) rest[1]))
				continue;
			rest++;
			while (isdigit((unsigned char) *rest))
				rest++;
			if (strncmp(rest, "_prt_", 5) == 0)
				classic = true;
		}
		if (!classic)
			return;
	}

	forboth(lc, oids, lc2, names)
	{
		char	   *relname = lfirst(lc2);
		char		newpartname[NAMEDATALEN * 2];

		if (strncmp(oldname, relname, oldlen) == 0)
		{
			snprintf(newpartname, sizeof(newpartname), "%s%s",
					 newname, relname + oldlen);
			if (strlen(newpartname) < NAMEDATALEN)
			{
				RenameRelationInternal(lfirst_oid(lc), newpartname, false, false);
				CommandCounterIncrement();
				renamed++;
				continue;
			}
		}
		skipped++;
	}

	if (skipped > 0)
		elog(WARNING, "renamed %d relations, skipped %d child partitions as old parent name is not part of partition name",
			 renamed, skipped);
}

/* RENAME [DEFAULT] PARTITION ... TO name */
static void
cmd_rename(Oid relid, GpPartParser *p, GpPartCmd *cmd,
		   const char *queryString, QueryEnvironment *queryEnv)
{
	Oid			partrelid;
	char	   *newrelname;

	/* the label part of the name is prt_<name>, which is never shortened */
	if (strlen(cmd->newname) > NAMEDATALEN - 8)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("name \"%s\" for child partition is too long",
						cmd->newname)));

	partrelid = target_of(relid, p, cmd->id, false, queryString);
	LockRelationOid(partrelid, AccessExclusiveLock);
	newrelname = choose_partition_name(get_rel_name(relid), partition_level(relid),
									   get_rel_namespace(partrelid), cmd->newname, 0);
	run_utility((Node *) rename_stmt(rv_of(partrelid), newrelname),
				queryString, queryEnv);
}

/*
 * EXCHANGE PARTITION ... WITH TABLE t: the partition is detached, t attached
 * with its bound, and the two swap names (and schemas), so that t is the
 * partition and the partition is t.  Cloudberry's AtExecGPExchangePartition.
 */
static void
cmd_exchange(Oid relid, GpPartParser *p, GpPartCmd *cmd,
			 const char *queryString, QueryEnvironment *queryEnv)
{
	Oid			partrelid;
	Oid			newrelid;
	RangeVar   *oldpartrv;
	RangeVar   *newpartrv;
	RangeVar   *tmprv;
	PartitionBoundSpec *bound;
	char	   *newpartname;
	char		tmpname[NAMEDATALEN];
	char		tmpname2[NAMEDATALEN];

	if (cmd->validation >= 0)
		ereport(NOTICE,
				(errmsg("specifying \"%s\" acts as no operation",
						cmd->validation ? "WITH VALIDATION" : "WITHOUT VALIDATION"),
				 errdetail("If the new partition is a regular table, validation is performed "
						   "to make sure all the rows obey partition constraint. "
						   "If the new partition is external or foreign table, no validation is performed.")));

	partrelid = target_of(relid, p, cmd->id, false, queryString);
	if (get_rel_relkind(partrelid) == RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot EXCHANGE PARTITION for relation \"%s\" -- partition has children",
						get_rel_name(relid))));
	oldpartrv = rv_of(partrelid);
	snprintf(tmpname, sizeof(tmpname), "pg_temp_%u", partrelid);
	tmprv = makeRangeVar(pstrdup(oldpartrv->schemaname), pstrdup(tmpname), -1);
	bound = stored_bound(partrelid, NULL);

	newpartrv = makeRangeVarFromNameList(stringToQualifiedNameList(GpPartSpanText(p, cmd->table), NULL));
	newrelid = RangeVarGetRelid(newpartrv, AccessShareLock, false);
	newpartrv = rv_of(newrelid);
	newpartname = pstrdup(newpartrv->relname);
	snprintf(tmpname2, sizeof(tmpname2), "pg_temp_%u", newrelid);

	run_utility((Node *) partition_cmd_stmt(rv_of(relid), oldpartrv, NULL,
											AT_DetachPartition),
				queryString, queryEnv);
	run_utility((Node *) partition_cmd_stmt(rv_of(relid), copyObject(newpartrv),
											raw_bound(bound), AT_AttachPartition),
				queryString, queryEnv);

	run_utility((Node *) rename_stmt(oldpartrv, tmprv->relname), queryString, queryEnv);

	if (strcmp(oldpartrv->schemaname, newpartrv->schemaname) != 0)
	{
		run_utility((Node *) set_schema_stmt(tmprv, newpartrv->schemaname),
					queryString, queryEnv);
		tmprv = makeRangeVar(pstrdup(newpartrv->schemaname), pstrdup(tmprv->relname), -1);

		run_utility((Node *) rename_stmt(newpartrv, tmpname2), queryString, queryEnv);
		run_utility((Node *) set_schema_stmt(makeRangeVar(pstrdup(newpartrv->schemaname),
														  pstrdup(tmpname2), -1),
											 oldpartrv->schemaname),
					queryString, queryEnv);
		newpartrv = makeRangeVar(pstrdup(oldpartrv->schemaname), pstrdup(tmpname2), -1);
	}

	run_utility((Node *) rename_stmt(newpartrv, oldpartrv->relname), queryString, queryEnv);
	run_utility((Node *) rename_stmt(tmprv, newpartname), queryString, queryEnv);
}

/*
 * SPLIT [DEFAULT] PARTITION: the partition is detached and renamed, two
 * partitions made in its place, its rows put back through the table, and the
 * old one dropped.  PostgreSQL 19 has no SPLIT PARTITION of its own.
 * Cloudberry's AtExecGPSplitPartition.
 */
static void
cmd_split(Oid relid, GpPartParser *p, GpPartCmd *cmd,
		  const char *queryString, QueryEnvironment *queryEnv)
{
	ParseState *pstate = make_pstate(queryString);
	Relation	rel = table_open(relid, NoLock);
	PartitionKey partkey = RelationGetPartitionKey(rel);
	PartParent *parent = parent_of(rel);
	Oid			partrelid;
	Relation	partrel;
	char	   *defaultpartname = NULL;
	RangeVar   *oldpartrv;
	RangeVar   *tmprv;
	char		tmpname[NAMEDATALEN];
	PartitionBoundSpec *bound;
	PartitionBoundSpec *bound1;
	PartitionBoundSpec *bound2;
	List	   *reloptions;
	char	   *tablespace;
	char	   *accessmethod;
	char	   *partname1 = NULL;
	char	   *partname2 = NULL;
	char	   *part_col_name;
	Oid			part_col_typid;
	int32		part_col_typmod;
	Oid			part_col_collation;
	int			level = partition_level(relid);
	int			partnum = 0;
	PartChild  *c1;
	PartChild  *c2;
	StringInfoData sql;
	StringInfoData cols;
	TupleDesc	desc = RelationGetDescr(rel);

	partrelid = find_target(rel, p, cmd->id, false, queryString);
	partrel = table_open(partrelid, AccessShareLock);

	if (partrelid == get_default_oid_from_partdesc(RelationGetPartitionDesc(rel, false)))
		defaultpartname = pstrdup(RelationGetRelationName(partrel));
	if (partrel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
		elog(ERROR, "Cannot split external partition");
	if (partrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot SPLIT PARTITION for relation \"%s\" -- partition has children",
						RelationGetRelationName(rel)),
				 errhint("Try splitting the child partitions.")));

	oldpartrv = rv_of(partrelid);
	snprintf(tmpname, sizeof(tmpname), "pg_temp_%u", partrelid);
	tmprv = makeRangeVar(pstrdup(oldpartrv->schemaname), pstrdup(tmpname), -1);
	bound = stored_bound(partrelid, &reloptions);
	tablespace = OidIsValid(partrel->rd_rel->reltablespace) ?
		get_tablespace_name(partrel->rd_rel->reltablespace) : NULL;
	accessmethod = get_am_name(partrel->rd_rel->relam);
	table_close(partrel, NoLock);

	/* the two names, from INTO (a, b) */
	if (cmd->into1 != NULL)
	{
		Oid			intorel1 = find_target(rel, p, cmd->into1, true, queryString);
		Oid			intorel2 = find_target(rel, p, cmd->into2, true, queryString);

		if (OidIsValid(intorel1) && OidIsValid(intorel2))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("both INTO partitions already exist")));

		/* splitting the default, INTO has to name it, by name or as DEFAULT */
		if (defaultpartname)
		{
			if (intorel1 == partrelid)
			{
				if (cmd->into2->kind != GP_PART_ID_NAME)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("new partition in INTO clause must be given by name"),
							 parser_errposition(pstate, GpPartLocation(p, cmd->into2->location))));
				partname1 = cmd->into2->name;
			}
			else if (intorel2 == partrelid)
			{
				if (cmd->into1->kind != GP_PART_ID_NAME)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("new partition in INTO clause must be given by name"),
							 parser_errposition(pstate, GpPartLocation(p, cmd->into1->location))));
				partname1 = cmd->into1->name;
			}
			else
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("default partition name missing from INTO clause")));
		}
		else
		{
			partname1 = cmd->into1->name;
			partname2 = cmd->into2->name;
		}
	}

	Assert(partkey->partnatts == 1);
	part_col_name = key_column_name(rel, 0);
	part_col_typid = get_partition_col_typid(partkey, 0);
	part_col_typmod = get_partition_col_typmod(partkey, 0);
	part_col_collation = get_partition_col_collation(partkey, 0);

	bound1 = makeNode(PartitionBoundSpec);
	bound1->strategy = bound->strategy;
	bound1->is_default = false;
	bound2 = bound;

	switch (bound->strategy)
	{
		case PARTITION_STRATEGY_RANGE:
			{
				List	   *start = cmd->start ? GpPartExprList(p, cmd->start->vals, false) : NIL;
				List	   *end = cmd->end ? GpPartExprList(p, cmd->end->vals, false) :
				GpPartExprList(p, cmd->at, true);
				bool		startExclusive = cmd->start && !cmd->start->inclusive;
				bool		endInclusive = cmd->end && cmd->end->inclusive;
				Const	   *endConst;

				if (list_length(end) != partkey->partnatts)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("number of END values should cover all partition key columns"),
							 parser_errposition(pstate, cmd->end ?
												GpPartLocation(p, cmd->end->location) : -1)));
				endConst = bound_value(pstate, linitial(end), part_col_name,
									   part_col_typid, part_col_typmod,
									   part_col_collation);
				if (endConst->constisnull)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("cannot use NULL with range partition specification"),
							 parser_errposition(pstate, GpPartLocation(p, cmd->split_location))));
				if (endInclusive)
					convert_exclusive_start_inclusive_end(endConst, part_col_typid,
														  part_col_typmod, false);
				if (!endConst->constisnull)
					bound1->upperdatums = list_make1(key_const(partkey, endConst->constvalue));
				else
					bound1->upperdatums = list_make1(infinite_bound("maxvalue"));

				if (start != NIL)
				{
					Const	   *startConst;

					if (list_length(start) != partkey->partnatts)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
								 errmsg("number of START values should cover all partition key columns"),
								 parser_errposition(pstate, GpPartLocation(p, cmd->start->location))));
					startConst = bound_value(pstate, linitial(start), part_col_name,
											 part_col_typid, part_col_typmod,
											 part_col_collation);
					if (startConst->constisnull)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
								 errmsg("cannot use NULL with range partition specification"),
								 parser_errposition(pstate, GpPartLocation(p, cmd->split_location))));
					if (startExclusive)
						convert_exclusive_start_inclusive_end(startConst, part_col_typid,
															  part_col_typmod, true);
					bound1->lowerdatums = list_make1(key_const(partkey, startConst->constvalue));
				}
				else
				{
					if (defaultpartname)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("AT clause cannot be used when splitting a default RANGE partition")));

					/* AT: the first half keeps the partition's start */
					bound1->lowerdatums = bound2->lowerdatums;
					bound2->lowerdatums = copyObject(bound1->upperdatums);
				}
			}
			break;

		case PARTITION_STRATEGY_LIST:
			{
				PartitionBoundSpec *newvals = bound1;
				PartitionBoundSpec *remaining = bound2;
				ListCell   *lc;

				if (cmd->start || cmd->end)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("cannot SPLIT LIST PARTITION with START"),
							 errhint("Use SPLIT with the AT clause instead.")));
				if (level != 1)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("SPLIT PARTITION is not currently supported when leaf partition is list partitioned in multi level partition table")));

				foreach(lc, GpPartExprList(p, cmd->at, true))
				{
					Const	   *value = bound_value(pstate, lfirst(lc), part_col_name,
													part_col_typid, part_col_typmod,
													part_col_collation);

					if (list_member(newvals->listdatums, value))
						continue;

					if (!remaining->is_default)
					{
						ListCell   *lc2;
						bool		found = false;

						foreach(lc2, remaining->listdatums)
						{
							if (equal(lfirst(lc2), value))
							{
								remaining->listdatums = foreach_delete_current(remaining->listdatums, lc2);
								found = true;
								break;
							}
						}
						if (!found)
							ereport(ERROR,
									(errcode(ERRCODE_WRONG_OBJECT_TYPE),
									 errmsg("AT clause parameter is not a member of the target partition specification")));
						if (remaining->listdatums == NIL)
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("AT clause cannot contain all values in the partition to be split")));
					}

					newvals->listdatums = lappend(newvals->listdatums, value);
				}

				/* splitting the default, the new values are made first */
				if (!defaultpartname)
				{
					bound1 = remaining;
					bound2 = newvals;
				}
			}
			break;

		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("partition strategy: %c not supported by SPLIT partition",
							bound->strategy),
					 parser_errposition(pstate, GpPartLocation(p, cmd->split_location))));
	}

	/*
	 * The two new partitions' names are chosen now, while the old partition
	 * still has its own, as Cloudberry chooses them.
	 */
	c1 = new_child(rel, partname1, NULL, p, &partnum, NULL, level);
	c1->is_default = bound1->is_default;
	c1->strategy = bound1->strategy;
	c1->listdatums = bound_to_raw(bound1->listdatums);
	c1->lower = bound1->lowerdatums;
	c1->upper = bound1->upperdatums;
	c1->options = reloptions;
	c1->tablespace = tablespace;
	c1->accessmethod = accessmethod;

	c2 = new_child(rel, defaultpartname ? NULL : partname2, NULL, p, &partnum,
				   defaultpartname, level);
	c2->is_default = bound2->is_default;
	c2->strategy = bound2->strategy;
	c2->listdatums = bound_to_raw(bound2->listdatums);
	c2->lower = bound2->lowerdatums;
	c2->upper = bound2->upperdatums;
	c2->options = reloptions;
	c2->tablespace = tablespace;
	c2->accessmethod = accessmethod;

	/*
	 * Its rows go back through the table by column name: a partition attached
	 * or exchanged in may have its columns in another order.
	 */
	initStringInfo(&cols);
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (att->attisdropped || att->attgenerated)
			continue;
		appendStringInfo(&cols, "%s%s", cols.len > 0 ? ", " : "",
						 quote_identifier(NameStr(att->attname)));
	}
	initStringInfo(&sql);
	appendStringInfo(&sql, "INSERT INTO %s.%s (%s) SELECT %s FROM %s.%s",
					 quote_identifier(parent->nspname),
					 quote_identifier(parent->relname),
					 cols.data, cols.data,
					 quote_identifier(tmprv->schemaname),
					 quote_identifier(tmprv->relname));
	table_close(rel, NoLock);

	/* the partition goes, and comes back under another name */
	run_utility((Node *) partition_cmd_stmt(parent_rv(parent), oldpartrv, NULL,
											AT_DetachPartition),
				queryString, queryEnv);
	run_utility((Node *) rename_stmt(oldpartrv, tmprv->relname), queryString, queryEnv);

	/* the two in its place */
	(void) create_child(parent, c1, queryString, queryEnv);
	(void) create_child(parent, c2, queryString, queryEnv);

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	if (SPI_execute(sql.data, false, 0) != SPI_OK_INSERT)
		elog(ERROR, "could not move the rows of a split partition");
	SPI_finish();

	/* and the old partition goes */
	{
		DropStmt   *drop = makeNode(DropStmt);

		drop->objects = list_make1(list_make2(makeString(tmprv->schemaname),
											  makeString(tmprv->relname)));
		drop->removeType = OBJECT_TABLE;
		drop->behavior = DROP_CASCADE;
		drop->missing_ok = false;
		run_utility((Node *) drop, queryString, queryEnv);
	}
}

/* SET SUBPARTITION TEMPLATE (...), or () to remove it */
static void
cmd_set_template(Oid origid, Oid relid, GpPartParser *p, GpPartCmd *cmd,
				 const char *queryString)
{
	int			level = partition_level(relid);
	Oid			rootid = partition_root(relid);

	if (cmd->template_def != NULL)
	{
		Relation	rel = table_open(relid, NoLock);
		PartitionDesc partdesc = RelationGetPartitionDesc(rel, false);
		Relation	firstrel;

		if (partdesc->nparts == 0)
			elog(ERROR, "GPDB SET SUBPARTITION TEMPLATE syntax needs at least one sibling to exist");

		firstrel = table_open(partdesc->oids[0], AccessShareLock);
		table_close(rel, NoLock);
		if (firstrel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
			elog(ERROR, "level %d is not partitioned and hence can't set subpartition template for the same",
				 level);

		/* below a level with partitions, the next level needs one too */
		if (!RelationGetPartitionDesc(firstrel, false)->is_leaf[0] &&
			template_get(rootid, level + 1) == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("can't add sub-partition template at level %d since next level template doesn't exist",
							level),
					 errhint("Add sub-partition template for next level.")));

		/* the template, tried against the partition it will be used for */
		(void) make_children(firstrel, p, cmd->template_def, NULL, false, queryString);
		table_close(firstrel, NoLock);

		(void) template_set(rootid, level, GpPartSpanText(p, cmd->template_def->text));
	}
	else if (!template_set(rootid, level, NULL))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("relation \"%s\" does not have a level %d subpartition template specification",
						get_rel_name(origid), level)));
}

/*
 * One of ALTER TABLE's partition commands, on the table the statement names:
 * Cloudberry's ATExecGPPartCmds.  ALTER PARTITION goes down the hierarchy to
 * the partition its command is for.
 */
static void
run_cmd(Oid relid, GpPartParser *p, GpPartCmd *cmd, const char *queryString,
		QueryEnvironment *queryEnv)
{
	Oid			origid = relid;
	Relation	rel;

	/* the lock Cloudberry's ALTER TABLE takes for each */
	LockRelationOid(relid, (cmd->kind == GP_PART_CMD_TRUNCATE ||
							cmd->kind == GP_PART_CMD_ALTER) ?
					AccessShareLock : AccessExclusiveLock);

	while (cmd->kind == GP_PART_CMD_ALTER)
	{
		GpPartParser *nested;

		rel = table_open(relid, AccessShareLock);
		check_partitioned(rel);
		relid = find_target(rel, p, cmd->id, false, queryString);
		table_close(rel, NoLock);
		cmd = cmd->sub;

		/*
		 * A command after ALTER PARTITION reports no position: Cloudberry's
		 * nested command has no query text of its own to count one in.
		 */
		nested = palloc(sizeof(GpPartParser));
		*nested = *p;
		nested->base = -1;
		p = nested;
	}

	/*
	 * A partition that is not partitioned itself can only be moved to
	 * another tablespace.
	 */
	rel = table_open(relid, AccessShareLock);
	if (cmd->kind != GP_PART_CMD_SET_TABLESPACE)
		check_partitioned(rel);
	table_close(rel, NoLock);

	switch (cmd->kind)
	{
		case GP_PART_CMD_ADD:
			cmd_add(relid, p, cmd, queryString, queryEnv);
			break;
		case GP_PART_CMD_DROP:
			cmd_drop(relid, p, cmd, queryString, queryEnv);
			break;
		case GP_PART_CMD_EXCHANGE:
			cmd_exchange(relid, p, cmd, queryString, queryEnv);
			break;
		case GP_PART_CMD_RENAME:
			cmd_rename(relid, p, cmd, queryString, queryEnv);
			break;
		case GP_PART_CMD_SET_TEMPLATE:
			cmd_set_template(origid, relid, p, cmd, queryString);
			break;
		case GP_PART_CMD_SPLIT:
			cmd_split(relid, p, cmd, queryString, queryEnv);
			break;
		case GP_PART_CMD_TRUNCATE:
			cmd_truncate(relid, p, cmd, queryString, queryEnv);
			break;
		case GP_PART_CMD_SET_TABLESPACE:
			{
				AlterTableStmt *at = makeNode(AlterTableStmt);
				AlterTableCmd *atc = makeNode(AlterTableCmd);

				atc->subtype = AT_SetTableSpace;
				atc->name = cmd->tablespace;
				at->relation = rv_of(relid);
				at->cmds = list_make1(atc);
				at->objtype = OBJECT_TABLE;
				run_utility((Node *) at, queryString, queryEnv);
			}
			break;
		case GP_PART_CMD_OTHER:
			/* Cloudberry's grammar takes any command here; nothing does it */
			elog(ERROR, "Not implemented");
			break;
		case GP_PART_CMD_ALTER:
			Assert(false);
			break;
	}
}

/*
 * The partition commands of an ALTER TABLE, its gp.partition_cmd options, in
 * the order they were written, once the rest of the statement has run.
 */
void
GpPartitionAlter(AlterTableStmt *stmt, List *options, const char *queryString,
				 QueryEnvironment *queryEnv)
{
	Oid			relid;
	ListCell   *lc;

	relid = RangeVarGetRelid(stmt->relation, NoLock, stmt->missing_ok);
	if (!OidIsValid(relid))
		return;					/* IF EXISTS, of a table there is not */

	foreach(lc, options)
	{
		DefElem    *def = lfirst_node(DefElem, lc);
		GpPartParser *p = option_parser(def, queryString);
		GpPartCmd  *cmd;
		int			end;

		cmd = GpPartParseCmd(p, 0, &end);
		run_cmd(relid, p, cmd, queryString, queryEnv);
		CommandCounterIncrement();
	}
}

/* Is this option a partition command? */
static bool
is_partition_cmd(DefElem *def)
{
	return def->defnamespace != NULL &&
		strcmp(def->defnamespace, GP_OPTION_NS) == 0 &&
		strcmp(def->defname, GP_PARTITION_CMD_OPTION) == 0;
}

/*
 * Take the partition commands out of an ALTER TABLE: each is SET
 * (gp.partition_cmd = '...') among its commands, and a SET with nothing else
 * in it goes, as a tag's does.
 */
List *
GpPartitionTakeCmds(AlterTableStmt *stmt)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, stmt->cmds)
	{
		AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lc);
		List	   *opts;
		ListCell   *lc2;

		if (cmd->subtype != AT_SetRelOptions)
			continue;
		opts = (List *) cmd->def;
		foreach(lc2, opts)
		{
			DefElem    *def = lfirst_node(DefElem, lc2);

			if (is_partition_cmd(def))
			{
				result = lappend(result, def);
				opts = foreach_delete_current(opts, lc2);
			}
		}
		cmd->def = (Node *) opts;
		if (opts == NIL)
			stmt->cmds = foreach_delete_current(stmt->cmds, lc);
	}
	return result;
}

/* Does an ALTER TABLE have one? */
bool
GpPartitionHasCmds(AlterTableStmt *stmt)
{
	ListCell   *lc;

	foreach(lc, stmt->cmds)
	{
		AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lc);
		ListCell   *lc2;

		if (cmd->subtype != AT_SetRelOptions)
			continue;
		foreach(lc2, (List *) cmd->def)
		{
			if (is_partition_cmd(lfirst_node(DefElem, lc2)))
				return true;
		}
	}
	return false;
}
