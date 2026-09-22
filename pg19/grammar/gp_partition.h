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
 * gp_partition.h
 *	  Cloudberry's classic partition clauses, read.
 *
 * CREATE TABLE ... PARTITION BY RANGE (d) (START (...) END (...) EVERY (...),
 * DEFAULT PARTITION other) and the ALTER TABLE commands that go with it --
 * ADD, DROP, ALTER, EXCHANGE, RENAME, SPLIT and TRUNCATE PARTITION, SET
 * SUBPARTITION TEMPLATE -- are Greenplum's, and PostgreSQL 19's grammar has
 * none of them.  O26 finds such a clause, checks it with the parser here, and
 * carries it verbatim to gp_sql as one option of its statement; gp_sql reads
 * it again with the same parser and makes the partitions (gp_sql's
 * partition.c).  The parser is Cloudberry's productions
 * (src/backend/parser/gram.y, "START GPDB LEGACY PARTITION SYNTAX RULES" and
 * alter_table_partition_cmd), written by hand.
 *
 * What the parser gives back is a description, not parse nodes: the names and
 * keywords of the clause, and where in the text each expression is.  An
 * expression is PostgreSQL's, and PostgreSQL's grammar reads it
 * (GpPartExprList).
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_PARTITION_H
#define GP_PARTITION_H

#include "postgres.h"

#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"

struct GpTokens;

/*
 * The option a clause becomes.  On CREATE TABLE, the clause from PARTITION BY
 * to the end of its partition list, in the statement's WITH list; on ALTER
 * TABLE, one command, as SET (gp.partition_cmd = '...').  The value is the
 * user's text, and the option's location is where it starts in the user's
 * text, so that a position in the value is a position there too.
 */
#define GP_PARTITION_BY_OPTION	"partition_by"
#define GP_PARTITION_CMD_OPTION	"partition_cmd"

/* A stretch of the text the parser read: bytes [from, to). */
typedef struct GpPartSpan
{
	int			from;
	int			to;
} GpPartSpan;

/* START (...) or END (...), and INCLUSIVE or EXCLUSIVE after it */
typedef struct GpPartRangeItem
{
	GpPartSpan	vals;			/* what is between the parentheses */
	bool		inclusive;		/* START: default true; END: default false */
	int			location;		/* START or END */
} GpPartRangeItem;

typedef enum GpPartBoundKind
{
	GP_PART_BOUND_LIST,			/* VALUES (...) */
	GP_PART_BOUND_RANGE			/* START (...) END (...) EVERY (...) */
} GpPartBoundKind;

typedef struct GpPartBound
{
	GpPartBoundKind kind;
	GpPartSpan	values;			/* LIST: between VALUES' parentheses */
	GpPartRangeItem *start;		/* RANGE: either may be missing */
	GpPartRangeItem *end;
	GpPartSpan	every;			/* RANGE: from < 0 if there is none */
	int			location;
} GpPartBound;

struct GpPartDef;

/* One element of a partition list: a partition, or several with EVERY. */
typedef struct GpPartElem
{
	char	   *name;			/* NULL if it has none */
	bool		is_default;
	GpPartBound *bound;			/* NULL if it has none */
	GpPartSpan	with;			/* WITH (...), parentheses and all; from < 0
								 * if there is none */
	bool		without_oids;
	char	   *tablespace;
	List	   *encodings;		/* GpPartSpan of each COLUMN ... ENCODING */
	struct GpPartDef *subparts; /* its own ( SUBPARTITION ... ) list */
	int			location;
} GpPartElem;

/* A parenthesised list of partitions, or a SUBPARTITION TEMPLATE's. */
typedef struct GpPartDef
{
	List	   *elems;			/* GpPartElem */
	List	   *encodings;		/* GpPartSpan of the COLUMN ... ENCODING
								 * directives written among them */
	bool		is_template;
	GpPartSpan	text;			/* the list, parentheses and all */
	int			location;		/* its first element; a template's, its '(' */
} GpPartDef;

/* PARTITION BY or SUBPARTITION BY, and a SUBPARTITION TEMPLATE after it */
typedef struct GpPartKey
{
	char		strategy;		/* PARTITION_STRATEGY_RANGE, _LIST or _HASH,
								 * or 0 for another word */
	char	   *strategy_name;	/* as written */
	GpPartSpan	params;			/* the key's list, parentheses and all */
	GpPartDef  *template_def;	/* NULL if it has none */
	struct GpPartKey *sub;		/* the next level's SUBPARTITION BY */
	int			location;
} GpPartKey;

/* The clause on CREATE TABLE */
typedef struct GpPartClause
{
	GpPartKey  *key;			/* PARTITION BY; key->sub is SUBPARTITION BY */
	GpPartDef  *def;			/* its partition list; NULL for PostgreSQL's
								 * own PARTITION BY, which has none */
	int			key_end;		/* the token after PARTITION BY's list */
	int			end;			/* the token after the clause */
} GpPartClause;

/* How an ALTER TABLE command names a partition */
typedef enum GpPartIdKind
{
	GP_PART_ID_NAME,			/* PARTITION name */
	GP_PART_ID_VALUE,			/* PARTITION FOR (value) */
	GP_PART_ID_DEFAULT			/* DEFAULT PARTITION */
} GpPartIdKind;

typedef struct GpPartId
{
	GpPartIdKind kind;
	char	   *name;
	GpPartSpan	values;			/* FOR: between its parentheses */
	int			location;
} GpPartId;

typedef enum GpPartCmdKind
{
	GP_PART_CMD_ADD,
	GP_PART_CMD_ALTER,
	GP_PART_CMD_DROP,
	GP_PART_CMD_EXCHANGE,
	GP_PART_CMD_RENAME,
	GP_PART_CMD_SET_TEMPLATE,
	GP_PART_CMD_SPLIT,
	GP_PART_CMD_TRUNCATE,
	GP_PART_CMD_SET_TABLESPACE, /* only after ALTER PARTITION */
	GP_PART_CMD_OTHER			/* another command after ALTER PARTITION */
} GpPartCmdKind;

typedef struct GpPartCmd
{
	GpPartCmdKind kind;
	GpPartId   *id;				/* the partition it is about */
	GpPartElem *elem;			/* ADD: the partition to add */
	struct GpPartCmd *sub;		/* ALTER: the command for the partition */
	bool		missing_ok;		/* DROP ... IF EXISTS */
	DropBehavior behavior;		/* DROP, TRUNCATE */
	char	   *newname;		/* RENAME */
	GpPartSpan	table;			/* EXCHANGE: the table, a qualified name */
	int			validation;		/* EXCHANGE: -1 none, 0 WITHOUT, 1 WITH */
	GpPartDef  *template_def;	/* SET SUBPARTITION TEMPLATE; NULL for () */
	GpPartRangeItem *start;		/* SPLIT DEFAULT PARTITION START ... END */
	GpPartRangeItem *end;
	GpPartSpan	at;				/* SPLIT ... AT (...): from < 0 if none */
	int			split_location; /* SPLIT: its first AT value, or its END */
	bool		at_values;		/* AT (VALUES (...)) rather than AT (...) */
	GpPartId   *into1;			/* SPLIT ... INTO (a, b) */
	GpPartId   *into2;
	char	   *tablespace;		/* SET TABLESPACE */
	GpPartSpan	other;			/* OTHER: the command's text */
	int			location;
} GpPartCmd;

/*
 * The parser's state.  It reads tokens [first, limit) of ts.  An error is
 * reported in errsrc, the text the user sent, where ts->src starts at base:
 * the rewrite reads the statement itself, base 0, and gp_sql the option it
 * became, base the option's location; -1 when that is not known.  With
 * check_exprs, every expression is read by PostgreSQL's grammar too, so that
 * a syntax error in one is raised where Cloudberry's grammar raises it, when
 * the statement is parsed.
 */
typedef struct GpPartParser
{
	const struct GpTokens *ts;
	int			limit;
	const char *errsrc;
	int			base;
	bool		check_exprs;
} GpPartParser;

/*
 * PARTITION BY ... at token i: the clause, whether Cloudberry's or
 * PostgreSQL's own (clause->def is NULL for the second).  NULL if token i
 * does not begin one.
 */
extern GpPartClause *GpPartParseClause(GpPartParser *p, int i);

/*
 * Does an ALTER TABLE command of Cloudberry's begin at token i?  Only where
 * PostgreSQL 19's grammar would refuse the command: ADD, DROP, ALTER and
 * RENAME PARTITION can also be PostgreSQL's, about a column named
 * "partition", and are then left to it.
 */
extern bool GpPartIsCmd(const struct GpTokens *ts, int i, int limit);

/* The ALTER TABLE command at token i; *end is the token after it. */
extern GpPartCmd *GpPartParseCmd(GpPartParser *p, int i, int *end);

/*
 * A SUBPARTITION TEMPLATE's list as stored: its text, "( ... )", read again.
 */
extern GpPartDef *GpPartParseTemplate(GpPartParser *p, int i);

/*
 * The expressions of a span, as PostgreSQL's grammar reads them, with their
 * locations in errsrc.  With constants_only, each must be a constant as
 * Cloudberry's grammar allows one in VALUES, FOR and AT: a literal, possibly
 * cast or negated.  With lists, VALUES ((1), (2)) is two values of one column
 * each; a RowExpr comes back for a value of two columns.
 */
extern List *GpPartExprList(GpPartParser *p, GpPartSpan span,
							bool constants_only);

/* The key of a PARTITION BY or SUBPARTITION BY, as PostgreSQL's node. */
extern PartitionSpec *GpPartKeySpec(GpPartParser *p, GpPartKey *key);

/* WITH (...) of an element, as PostgreSQL's options. */
extern List *GpPartWithOptions(GpPartParser *p, GpPartSpan with);

/* The text of a span, as it was written. */
extern char *GpPartSpanText(const GpPartParser *p, GpPartSpan span);

/* Where a token or byte of the text is in errsrc, or -1. */
extern int	GpPartLocation(const GpPartParser *p, int off);

#endif							/* GP_PARTITION_H */
