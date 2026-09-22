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
 * gp_partition.c
 *	  A parser for Cloudberry's classic partition clauses.
 *
 * Greenplum's partition syntax -- START (...) END (...) EVERY (...), DEFAULT
 * PARTITION, SUBPARTITION BY and SUBPARTITION TEMPLATE, and the ALTER TABLE
 * commands ADD, DROP, ALTER, EXCHANGE, RENAME, SPLIT and TRUNCATE PARTITION
 * -- is the first of Cloudberry's syntax that needs a parser of its own
 * rather than a word replaced (cloudberry.md, "Whether the forked gram.y is
 * still wanted").  This is that parser: Cloudberry's productions, written as
 * recursive descent over the tokens PostgreSQL's scanner makes.  It does not
 * move when PostgreSQL's grammar does; the syntax is Greenplum's legacy, and
 * does not change either.
 *
 * It is run twice for each clause.  O26's rewrite runs it when the statement
 * is parsed, to find where the clause ends and to raise its syntax errors
 * where Cloudberry's grammar raises them, at the same token; the clause is
 * then carried, as written, in one option of its statement.  gp_sql runs it
 * again on that option when the statement is executed, and acts on what it
 * says.  In between the clause is text, because decision 11 lets the rewrite
 * hand PostgreSQL's grammar only PostgreSQL's parse nodes.
 *
 * What is PostgreSQL's in a clause -- the expressions of START, END, EVERY,
 * VALUES and FOR, a key's column list, a WITH list -- is read by PostgreSQL's
 * grammar, with its locations put back where the user wrote them.  So the
 * expressions a partition bound may hold are exactly PostgreSQL's, and an
 * error in one is reported at the token in it.
 *
 * Where the two grammars disagree about a statement, PostgreSQL 19's reading
 * wins when it has one (GpPartIsCmd): ALTER TABLE t DROP partition drops a
 * column called "partition" in PostgreSQL, and is a syntax error in
 * Cloudberry, where PARTITION is reserved.
 *
 * Cloudberry source this file is made of:
 *	  src/backend/parser/gram.y: the rules between "START GPDB LEGACY
 *	  PARTITION SYNTAX RULES" and its END, alter_table_partition_cmd and
 *	  the rules it uses, PartitionIdentKeyword
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/keywords.h"
#include "mb/pg_wchar.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "parser/parser.h"
#include "utils/builtins.h"

#include "gp_grammar.h"
#include "gp_grammar_int.h"
#include "gp_partition.h"

/*
 * The keywords a partition may be named by: Cloudberry's PartitionIdentKeyword,
 * less the words that are no keyword in PostgreSQL 19 and arrive as
 * identifiers.  Sorted, for bsearch.
 */
static const char *const partition_name_keywords[] = {
	"abort", "absolute", "access", "action", "admin", "after", "aggregate", "also",
	"always", "asensitive", "assertion", "assignment", "at", "atomic", "attach", "attribute",
	"authorization", "backward", "before", "begin", "bigint", "binary", "bit", "boolean",
	"breadth", "by", "cache", "call", "called", "cascade", "cascaded", "chain",
	"characteristics", "checkpoint", "class", "close", "cluster", "coalesce", "columns", "comment",
	"commit", "committed", "compression", "configuration", "conflict", "connection", "constraints", "content",
	"conversion", "copy", "cost", "csv", "cube", "cursor", "cycle", "database",
	"deallocate", "dec", "decimal", "declare", "defaults", "deferred", "definer", "delete",
	"delimiter", "delimiters", "depends", "depth", "detach", "dictionary", "disable", "domain",
	"double", "drop", "each", "enable", "encoding", "encrypted", "enum", "escape",
	"excluding", "exclusive", "execute", "exists", "explain", "external", "extract", "family",
	"filter", "finalize", "first", "float", "force", "format", "forward", "freeze",
	"function", "generated", "global", "granted", "greatest", "grouping", "groups", "handler",
	"header", "hold", "if", "immediate", "immutable", "implicit", "import", "include",
	"including", "increment", "index", "indexes", "inherit", "inherits", "inout", "input",
	"insensitive", "insert", "instead", "int", "integer", "interval", "invoker", "isolation",
	"key", "language", "large", "last", "least", "level", "listen", "load",
	"local", "location", "lock", "locked", "logged", "match", "maxvalue", "method",
	"minvalue", "mode", "move", "name", "names", "national", "nchar", "new",
	"next", "no", "none", "nothing", "notify", "nowait", "nullif", "nulls",
	"numeric", "object", "of", "oids", "old", "operator", "option", "options",
	"others", "out", "outer", "overlay", "overriding", "owned", "owner", "parallel",
	"partial", "password", "policy", "position", "precision", "prepare", "prepared", "preserve",
	"prior", "privileges", "procedural", "procedure", "procedures", "publication", "quote", "range",
	"read", "real", "reassign", "referencing", "reindex", "relative", "release", "rename",
	"repeatable", "replace", "reset", "restart", "restrict", "return", "returns", "revoke",
	"role", "rollback", "rollup", "routine", "routines", "row", "rows", "rule",
	"savepoint", "scalar", "schema", "schemas", "scroll", "search", "security", "sequence",
	"serializable", "session", "set", "setof", "sets", "share", "show", "simple",
	"smallint", "sql", "stable", "start", "statement", "statistics", "stdin", "stdout",
	"storage", "stored", "strict", "substring", "support", "sysid", "system", "tablesample",
	"temp", "template", "temporary", "ties", "time", "timestamp", "transaction", "transform",
	"treat", "trigger", "trim", "truncate", "trusted", "type", "uncommitted", "unencrypted",
	"unknown", "unlisten", "until", "update", "vacuum", "valid", "validator", "value",
	"values", "varchar", "verbose", "version", "view", "volatile", "work", "write",
	"zone",
};

static GpPartDef *parse_elem_list(GpPartParser *p, int open, bool sub, int *next);
static GpPartCmd *parse_cmd(GpPartParser *p, int i, bool nested, int *end);

/* ------------------------------------------------------------------------- */
/* Tokens, within the statement                                              */
/* ------------------------------------------------------------------------- */

/* Token i of the clause: nothing at or past the statement's end matches. */
static inline bool
p_kw(const GpPartParser *p, int i, const char *word)
{
	return i < p->limit && tok_is_kw(p->ts, i, word);
}

static inline bool
p_word(const GpPartParser *p, int i, const char *word)
{
	return i < p->limit && tok_is_word(p->ts, i, word);
}

static inline bool
p_char(const GpPartParser *p, int i, char c)
{
	return i < p->limit && tok_is_char(p->ts, i, c);
}

/* A ColId: an identifier, or a keyword that is not reserved. */
static bool
p_colid(const GpPartParser *p, int i)
{
	const GpTok *t;
	int			kwnum;

	if (i >= p->limit)
		return false;
	t = &p->ts->toks[i];
	if (t->code == GP_IDENT)
		return true;
	if (t->kw == NULL)
		return false;
	kwnum = ScanKeywordLookup(t->kw, &ScanKeywords);
	return kwnum >= 0 &&
		(ScanKeywordCategories[kwnum] == UNRESERVED_KEYWORD ||
		 ScanKeywordCategories[kwnum] == COL_NAME_KEYWORD);
}

static int
name_cmp(const void *a, const void *b)
{
	return strcmp(*(const char *const *) a, *(const char *const *) b);
}

/* What a partition may be called: PartitionColId, an identifier or one of
 * the keywords above. */
static bool
p_partname(const GpPartParser *p, int i)
{
	const GpTok *t;

	if (i >= p->limit)
		return false;
	t = &p->ts->toks[i];
	if (t->code == GP_IDENT)
		return true;
	if (t->kw == NULL)
		return false;
	return bsearch(&t->kw, partition_name_keywords,
				   lengthof(partition_name_keywords), sizeof(char *),
				   name_cmp) != NULL;
}

/* Where byte `off` of the text read is in the text the user sent, or -1. */
int
GpPartLocation(const GpPartParser *p, int off)
{
	if (p->base < 0 || off < 0)
		return -1;
	return p->base + off;
}

/* The same as a character position for errposition(), or 0 for none. */
static int
p_errpos(const GpPartParser *p, int off)
{
	int			at = GpPartLocation(p, off);

	if (at < 0)
		return 0;
	return pg_mbstrlen_with_len(p->errsrc, at) + 1;
}

/*
 * Cloudberry's syntax error at token i: "syntax error at or near" the token
 * as the user wrote it, or "at end of input", with the caret under it.  A
 * token at or past the statement's end is its ';', if it has one.
 */
static void
p_syntax_error(const GpPartParser *p, int i)
{
	const GpTokens *ts = p->ts;
	int			off;

	if (i >= ts->ntoks)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("syntax error at end of input"),
				 errposition(p_errpos(p, ts->srclen))));

	off = ts->toks[i].off;
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("syntax error at or near \"%s\"",
					pnstrdup(ts->src + off, tok_stop(ts, i) - off)),
			 errposition(p_errpos(p, off))));
}

/* The same at the token that starts at byte `off`, which a node's
 * location names. */
static void
p_syntax_error_at(const GpPartParser *p, int off)
{
	for (int i = 0; i < p->ts->ntoks; i++)
	{
		if (p->ts->toks[i].off == off)
			p_syntax_error(p, i);
	}
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("syntax error"),
			 errposition(p_errpos(p, off))));
}

/* The parenthesised group at token i: its closing parenthesis. */
static int
p_parens(const GpPartParser *p, int i)
{
	int			close;

	if (!p_char(p, i, '('))
		p_syntax_error(p, i);
	close = match_close(p->ts, i, p->limit);
	if (close < 0)
		p_syntax_error(p, p->limit);
	return close;
}

/* What is between the parentheses at tokens open and close. */
static GpPartSpan
inside(const GpPartParser *p, int open, int close)
{
	GpPartSpan	s;

	s.from = p->ts->toks[open].off + 1;
	s.to = p->ts->toks[close].off;
	return s;
}

/* Tokens [first, last], as written. */
static GpPartSpan
span_of(const GpPartParser *p, int first, int last)
{
	GpPartSpan	s;

	s.from = p->ts->toks[first].off;
	s.to = tok_stop(p->ts, last);
	return s;
}

char *
GpPartSpanText(const GpPartParser *p, GpPartSpan span)
{
	return pnstrdup(p->ts->src + span.from, span.to - span.from);
}

/* ------------------------------------------------------------------------- */
/* What PostgreSQL's grammar reads                                           */
/* ------------------------------------------------------------------------- */

/*
 * `prefix`, the user's text in `span`, and `suffix`, read by PostgreSQL's
 * grammar as one statement, with every location in what comes back put where
 * the user wrote it -- the words around the span stand for its start and
 * end -- and a syntax error reported there too.
 */
static Node *
p_parse(GpPartParser *p, const char *prefix, GpPartSpan span,
		const char *suffix)
{
	char	   *text = GpPartSpanText(p, span);
	char	   *sql = psprintf("%s%s%s", prefix, text, suffix);
	GpPosMap   *map;
	GpParseErrorArg errarg;
	ErrorContextCallback errcallback;
	List	   *raw;

	map = GpPosMapSpan(strlen(prefix), GpPartLocation(p, span.from),
					   span.to - span.from, strlen(suffix),
					   strlen(p->errsrc));

	errarg.original = p->errsrc;
	errarg.rewritten = sql;
	errarg.map = map;
	errcallback.callback = GpParseErrorCallback;
	errcallback.arg = &errarg;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	raw = raw_parser(sql, RAW_PARSE_DEFAULT);

	error_context_stack = errcallback.previous;

	GpRemapParseLocations(raw, map);

	if (list_length(raw) != 1)
		p_syntax_error_at(p, span.from);
	return linitial_node(RawStmt, raw)->stmt;
}

/*
 * A constant as Cloudberry's grammar allows one in VALUES, FOR and AT
 * (tab_part_val): a literal, a typed literal or a cast of one, possibly
 * negated.  PostgreSQL's grammar makes each of those an A_Const under
 * TypeCasts and unary minuses; anything else is where Cloudberry's grammar
 * would have stopped.
 */
static Node *
not_a_constant(Node *n)
{
	switch (nodeTag(n))
	{
		case T_A_Const:
			return NULL;
		case T_TypeCast:
			return not_a_constant(((TypeCast *) n)->arg);
		case T_A_Expr:
			{
				A_Expr	   *e = (A_Expr *) n;

				if (e->kind == AEXPR_OP && e->lexpr == NULL &&
					list_length(e->name) == 1 &&
					strcmp(strVal(linitial(e->name)), "-") == 0)
					return not_a_constant(e->rexpr);
				return n;
			}
		default:
			return n;
	}
}

/* Where a node read by PostgreSQL's grammar is in the text read, or -1. */
static int
node_offset(const GpPartParser *p, Node *n)
{
	int			loc = exprLocation(n);

	if (loc < 0 || p->base < 0)
		return -1;
	return loc - p->base;
}

/*
 * Cloudberry's grammar reads CAST (v AS type) in a constant with its own
 * rule, whose TypeCast is at AS (tab_part_val_no_paran, makeTypeCast's @4)
 * where PostgreSQL's is at CAST; exprLocation() takes the leftmost of the
 * two and the value, which is then the value.  An error about the bound --
 * one partition overlapping another -- is reported there, so the TypeCast is
 * moved to where Cloudberry has it.
 */
static void
cast_at_as(const GpPartParser *p, Node *n)
{
	TypeCast   *tc;
	int			i;
	int			depth = 0;

	if (IsA(n, A_Expr))
	{
		if (((A_Expr *) n)->rexpr != NULL)
			cast_at_as(p, ((A_Expr *) n)->rexpr);
		return;
	}
	if (!IsA(n, TypeCast))
		return;
	tc = (TypeCast *) n;
	cast_at_as(p, tc->arg);
	if (tc->location < 0 || p->base < 0)
		return;

	for (i = 0; i < p->ts->ntoks && p->ts->toks[i].off != tc->location - p->base; i++)
		;
	if (i >= p->ts->ntoks || !tok_is_kw(p->ts, i, "cast"))
		return;
	for (i++; i < p->limit; i++)
	{
		if (tok_is_char(p->ts, i, '('))
			depth++;
		else if (tok_is_char(p->ts, i, ')'))
		{
			if (--depth == 0)
				return;
		}
		else if (depth == 1 && tok_is_kw(p->ts, i, "as"))
		{
			tc->location = p->base + p->ts->toks[i].off;
			return;
		}
	}
}

List *
GpPartExprList(GpPartParser *p, GpPartSpan span, bool constants_only)
{
	Node	   *stmt = p_parse(p, "VALUES (", span, ")");
	SelectStmt *sel;
	List	   *exprs;
	ListCell   *lc;

	if (!IsA(stmt, SelectStmt) ||
		list_length(((SelectStmt *) stmt)->valuesLists) != 1)
		p_syntax_error_at(p, span.from);
	sel = (SelectStmt *) stmt;
	exprs = linitial(sel->valuesLists);

	if (!constants_only)
		return exprs;

	foreach(lc, exprs)
	{
		Node	   *e = lfirst(lc);
		Node	   *bad;

		/* VALUES ((1, 2), (3, 4)): a value of two columns is a row */
		if (IsA(e, RowExpr) && ((RowExpr *) e)->row_format == COERCE_IMPLICIT_CAST)
		{
			ListCell   *lc2;

			foreach(lc2, ((RowExpr *) e)->args)
			{
				if ((bad = not_a_constant(lfirst(lc2))) != NULL)
					p_syntax_error_at(p, node_offset(p, bad));
				cast_at_as(p, lfirst(lc2));
			}
			continue;
		}
		if ((bad = not_a_constant(e)) != NULL)
			p_syntax_error_at(p, node_offset(p, bad));
		cast_at_as(p, e);
	}

	return exprs;
}

static const char *
strategy_word(char strategy)
{
	switch (strategy)
	{
		case PARTITION_STRATEGY_RANGE:
			return "RANGE";
		case PARTITION_STRATEGY_LIST:
			return "LIST";
		default:
			return "HASH";
	}
}

PartitionSpec *
GpPartKeySpec(GpPartParser *p, GpPartKey *key)
{
	Node	   *stmt;
	char	   *prefix;

	prefix = psprintf("CREATE TABLE gp_partition_key () PARTITION BY %s ",
					  strategy_word(key->strategy));
	stmt = p_parse(p, prefix, key->params, "");
	if (!IsA(stmt, CreateStmt) || ((CreateStmt *) stmt)->partspec == NULL)
		p_syntax_error_at(p, key->params.from);
	return ((CreateStmt *) stmt)->partspec;
}

List *
GpPartWithOptions(GpPartParser *p, GpPartSpan with)
{
	Node	   *stmt = p_parse(p, "CREATE TABLE gp_partition_with () WITH ",
							   with, "");

	if (!IsA(stmt, CreateStmt))
		p_syntax_error_at(p, with.from);
	return ((CreateStmt *) stmt)->options;
}

/*
 * The legacy syntax takes a key's columns and nothing else: an expression in
 * the key is refused (gram.y's check_expressions_in_partition_key).
 */
static void
check_key_columns(GpPartParser *p, GpPartKey *key)
{
	PartitionSpec *spec = GpPartKeySpec(p, key);
	ListCell   *lc;

	foreach(lc, spec->partParams)
	{
		PartitionElem *e = lfirst_node(PartitionElem, lc);

		if (e->expr != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("expressions in partition key not supported in legacy GPDB partition syntax"),
					 errposition(e->location >= 0 ?
								 pg_mbstrlen_with_len(p->errsrc, e->location) + 1 : 0)));
	}
}

/* ------------------------------------------------------------------------- */
/* CREATE TABLE ... PARTITION BY                                             */
/* ------------------------------------------------------------------------- */

/*
 * PARTITION BY strategy (key) or SUBPARTITION BY strategy (key), with `by` at
 * BY.  *next is the token after the key's list.
 */
static GpPartKey *
parse_key(GpPartParser *p, int by, int *next)
{
	GpPartKey  *key = palloc0(sizeof(GpPartKey));
	int			i = by + 1;
	int			close;
	char	   *strategy;

	key->location = p->ts->toks[by - 1].off;

	if (!p_colid(p, i))
		p_syntax_error(p, i);
	strategy = tok_name(p->ts, i);
	key->strategy_name = strategy;
	if (strcmp(strategy, "range") == 0)
		key->strategy = PARTITION_STRATEGY_RANGE;
	else if (strcmp(strategy, "list") == 0)
		key->strategy = PARTITION_STRATEGY_LIST;
	else if (strcmp(strategy, "hash") == 0)
		key->strategy = PARTITION_STRATEGY_HASH;
	else
		key->strategy = 0;		/* check_strategy reports it */

	close = p_parens(p, i + 1);
	if (close == i + 2)			/* PARTITION BY RANGE () */
		p_syntax_error(p, close);
	key->params = span_of(p, i + 1, close);
	*next = close + 1;
	return key;
}

/* gram.y's parsePartitionStrategy, whose error has no position */
static void
check_strategy(const GpPartKey *key)
{
	if (key->strategy == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unrecognized partitioning strategy \"%s\"",
						key->strategy_name)));
}

/* Does a partition boundary start at token i? */
static bool
starts_bound(const GpPartParser *p, int i)
{
	return p_kw(p, i, "values") || p_kw(p, i, "start") || p_kw(p, i, "end");
}

/* START (...) or END (...), and INCLUSIVE or EXCLUSIVE, at token i. */
static GpPartRangeItem *
parse_range_item(GpPartParser *p, int i, int *next)
{
	GpPartRangeItem *item = palloc0(sizeof(GpPartRangeItem));
	bool		is_start = p_kw(p, i, "start");
	int			close = p_parens(p, i + 1);

	if (close == i + 2)			/* START () */
		p_syntax_error(p, close);

	item->vals = inside(p, i + 1, close);
	item->location = p->ts->toks[i].off;
	item->inclusive = is_start;
	*next = close + 1;

	if (p_word(p, *next, "inclusive"))
	{
		item->inclusive = true;
		(*next)++;
	}
	else if (p_kw(p, *next, "exclusive"))
	{
		item->inclusive = false;
		(*next)++;
	}

	if (p->check_exprs)
		(void) GpPartExprList(p, item->vals, false);
	return item;
}

/*
 * VALUES (...), or START (...) [END (...)] [EVERY (...)], or END (...)
 * [EVERY (...)], at token i.  ALTER TABLE ... ADD PARTITION has no EVERY.
 */
static GpPartBound *
parse_bound(GpPartParser *p, int i, bool every, int *next)
{
	GpPartBound *b = palloc0(sizeof(GpPartBound));

	b->location = p->ts->toks[i].off;
	b->every.from = -1;

	if (p_kw(p, i, "values"))
	{
		int			close = p_parens(p, i + 1);

		if (close == i + 2)		/* VALUES () */
			p_syntax_error(p, close);
		b->kind = GP_PART_BOUND_LIST;
		b->values = inside(p, i + 1, close);
		if (p->check_exprs)
			(void) GpPartExprList(p, b->values, true);
		*next = close + 1;
		return b;
	}

	b->kind = GP_PART_BOUND_RANGE;
	if (p_kw(p, i, "start"))
		b->start = parse_range_item(p, i, &i);
	if (p_kw(p, i, "end"))
		b->end = parse_range_item(p, i, &i);

	if (every && p_word(p, i, "every"))
	{
		int			close = p_parens(p, i + 1);

		if (close == i + 2)		/* EVERY () */
			p_syntax_error(p, close);
		b->every = inside(p, i + 1, close);
		if (p->check_exprs)
			(void) GpPartExprList(p, b->every, false);
		i = close + 1;
	}

	*next = i;
	return b;
}

/*
 * COLUMN name ENCODING (...) or DEFAULT COLUMN ENCODING (...), at token i:
 * an append-optimized, column-oriented table's column storage, which M5
 * brings.  It is read here so that the clause parses; gp_sql refuses it.
 */
static GpPartSpan
parse_encoding(GpPartParser *p, int i, int *next)
{
	int			first = i;
	int			close;

	if (p_kw(p, i, "default"))
	{
		if (!p_kw(p, i + 1, "column"))
			p_syntax_error(p, i + 1);
		i += 2;
	}
	else
	{
		/* COLUMN ColId */
		if (!p_colid(p, i + 1))
			p_syntax_error(p, i + 1);
		i += 2;
	}

	if (!p_kw(p, i, "encoding"))
		p_syntax_error(p, i);
	close = p_parens(p, i + 1);
	*next = close + 1;
	return span_of(p, first, close);
}

static bool
starts_encoding(const GpPartParser *p, int i)
{
	return p_kw(p, i, "column") ||
		(p_kw(p, i, "default") && p_kw(p, i + 1, "column"));
}

/*
 * What may follow a partition's name and boundary: WITH (...) or WITHOUT
 * OIDS, TABLESPACE name, column encodings, and its own list of partitions.
 * ALTER TABLE ... ADD PARTITION has no encodings.
 */
static int
parse_elem_tail(GpPartParser *p, int i, GpPartElem *elem, bool encodings)
{
	if (p_kw(p, i, "with"))
	{
		int			close = p_parens(p, i + 1);

		elem->with = span_of(p, i + 1, close);
		if (p->check_exprs)
			(void) GpPartWithOptions(p, elem->with);
		i = close + 1;
	}
	else if (p_kw(p, i, "without"))
	{
		if (!p_kw(p, i + 1, "oids"))
			p_syntax_error(p, i + 1);
		elem->without_oids = true;
		i += 2;
	}

	if (p_kw(p, i, "tablespace"))
	{
		if (!p_colid(p, i + 1))
			p_syntax_error(p, i + 1);
		elem->tablespace = tok_name(p->ts, i + 1);
		i += 2;
	}

	while (encodings && starts_encoding(p, i))
	{
		GpPartSpan *s = palloc(sizeof(GpPartSpan));

		*s = parse_encoding(p, i, &i);
		elem->encodings = lappend(elem->encodings, s);
	}

	if (p_char(p, i, '('))
		elem->subparts = parse_elem_list(p, i, true, &i);

	return i;
}

/*
 * One element of a partition list at token i (TabPartitionElem, or with sub
 * TabSubPartitionElem): a named partition, the default one, an unnamed one,
 * or a column encoding written among them, which comes back in *encoding.
 */
static GpPartElem *
parse_elem(GpPartParser *p, int i, bool sub, int *next, GpPartSpan **encoding)
{
	const char *partition = sub ? "subpartition" : "partition";
	GpPartElem *elem;

	*encoding = NULL;

	if (starts_encoding(p, i))
	{
		*encoding = palloc(sizeof(GpPartSpan));
		**encoding = parse_encoding(p, i, next);
		return NULL;
	}

	elem = palloc0(sizeof(GpPartElem));
	elem->location = (i < p->ts->ntoks) ? p->ts->toks[i].off : p->ts->srclen;
	elem->with.from = -1;

	if (p_word(p, i, partition))
	{
		if (!p_partname(p, i + 1))
			p_syntax_error(p, i + 1);
		elem->name = tok_name(p->ts, i + 1);
		i += 2;
		if (starts_bound(p, i))
			elem->bound = parse_bound(p, i, true, &i);
	}
	else if (p_kw(p, i, "default"))
	{
		if (!p_word(p, i + 1, partition))
			p_syntax_error(p, i + 1);
		if (!p_partname(p, i + 2))
			p_syntax_error(p, i + 2);
		elem->name = tok_name(p->ts, i + 2);
		elem->is_default = true;
		i += 3;
	}
	else if (starts_bound(p, i))
		elem->bound = parse_bound(p, i, true, &i);
	else
		p_syntax_error(p, i);

	*next = parse_elem_tail(p, i, elem, true);
	return elem;
}

/* ( element, ... ) at token open: a partition list, or with sub a list of
 * subpartitions. */
static GpPartDef *
parse_elem_list(GpPartParser *p, int open, bool sub, int *next)
{
	GpPartDef  *def = palloc0(sizeof(GpPartDef));
	int			close = p_parens(p, open);
	int			i = open + 1;

	/* gram.y's @2: the list's first element, where an error about it is */
	def->location = p->ts->toks[Min(open + 1, close)].off;
	def->text = span_of(p, open, close);

	for (;;)
	{
		GpPartSpan *encoding;
		GpPartElem *elem = parse_elem(p, i, sub, &i, &encoding);

		if (elem != NULL)
			def->elems = lappend(def->elems, elem);
		else
			def->encodings = lappend(def->encodings, encoding);

		if (p_char(p, i, ','))
		{
			i++;
			continue;
		}
		if (i == close)
			break;
		p_syntax_error(p, i);
	}

	*next = close + 1;
	return def;
}

/* SUBPARTITION TEMPLATE's list, at its opening parenthesis. */
static GpPartDef *
parse_template(GpPartParser *p, int open, int *next)
{
	GpPartDef  *def = parse_elem_list(p, open, true, next);
	ListCell   *lc;

	def->is_template = true;
	def->location = p->ts->toks[open].off;	/* and a template's, its '(' */

	/* a little syntax check on templates, as gram.y has it */
	foreach(lc, def->elems)
	{
		if (((GpPartElem *) lfirst(lc))->subparts != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("template cannot contain specification for child partition")));
	}
	return def;
}

GpPartDef *
GpPartParseTemplate(GpPartParser *p, int i)
{
	int			next;

	return parse_template(p, i, &next);
}

/*
 * SUBPARTITION BY ... [SUBPARTITION TEMPLATE (...)], as many levels as there
 * are, at token i.
 */
static GpPartKey *
parse_subpartition_by(GpPartParser *p, int i, int *next)
{
	GpPartKey  *first = NULL;
	GpPartKey  *last = NULL;

	while (p_word(p, i, "subpartition"))
	{
		GpPartKey  *key;

		if (!p_kw(p, i + 1, "by"))
			p_syntax_error(p, i + 1);
		key = parse_key(p, i + 1, &i);
		check_strategy(key);
		if (p->check_exprs)
			check_key_columns(p, key);

		if (p_word(p, i, "subpartition") && p_kw(p, i + 1, "template"))
			key->template_def = parse_template(p, i + 2, &i);

		if (last == NULL)
			first = key;
		else
			last->sub = key;
		last = key;
	}

	*next = i;
	return first;
}

GpPartClause *
GpPartParseClause(GpPartParser *p, int i)
{
	GpPartClause *c;
	int			j;

	if (!p_kw(p, i, "partition") || !p_kw(p, i + 1, "by"))
		return NULL;

	c = palloc0(sizeof(GpPartClause));
	c->key = parse_key(p, i + 1, &j);
	c->key_end = j;

	/*
	 * PostgreSQL's PARTITION BY has no list after its key, and nothing of
	 * Cloudberry's: the rewrite only moves it, if it is at the end of the
	 * statement, where Cloudberry lets it be written.
	 */
	if (!p_word(p, j, "subpartition") && !p_char(p, j, '('))
	{
		c->end = j;
		return c;
	}

	check_strategy(c->key);

	c->key->sub = parse_subpartition_by(p, j, &j);

	if (p_char(p, j, '('))
		c->def = parse_elem_list(p, j, false, &j);
	else
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("SUBPARTITION BY clause is not allowed when no partitions specified at depth 1")));

	if (p->check_exprs)
		check_key_columns(p, c->key);

	c->end = j;
	return c;
}

/* ------------------------------------------------------------------------- */
/* ALTER TABLE's partition commands                                          */
/* ------------------------------------------------------------------------- */

/* The words that follow ALTER [COLUMN] name in PostgreSQL's grammar. */
static bool
column_action(const GpTokens *ts, int i)
{
	static const char *const words[] = {
		"type", "set", "drop", "add", "restart", "reset", "options"
	};

	for (int k = 0; k < lengthof(words); k++)
		if (tok_is_kw(ts, i, words[k]))
			return true;
	return false;
}

bool
GpPartIsCmd(const GpTokens *ts, int i, int limit)
{
	GpPartParser p = {ts, limit, ts->src, 0, false};
	int			j = i + 2;

	if (p_kw(&p, i, "set"))
		return p_word(&p, i + 1, "subpartition") && p_kw(&p, i + 2, "template");

	if (!p_kw(&p, i, "add") && !p_kw(&p, i, "drop") && !p_kw(&p, i, "alter") &&
		!p_kw(&p, i, "rename") && !p_kw(&p, i, "truncate") &&
		!p_word(&p, i, "exchange") && !p_word(&p, i, "split"))
		return false;

	if (p_kw(&p, i + 1, "default") && p_kw(&p, i + 2, "partition"))
		return true;
	if (!p_kw(&p, i + 1, "partition"))
		return false;

	if (p_kw(&p, i, "truncate") || p_word(&p, i, "exchange") ||
		p_word(&p, i, "split"))
		return true;

	/*
	 * The other four are PostgreSQL's too, about a column: ADD partition
	 * int, DROP partition, ALTER partition TYPE text, RENAME partition TO x.
	 * Each is the partition command only where PostgreSQL's grammar would
	 * refuse it as the column one.
	 */
	if (p_kw(&p, i, "drop"))
		return !(j >= limit || p_char(&p, j, ',') || p_kw(&p, j, "cascade") ||
				 p_kw(&p, j, "restrict"));
	if (p_kw(&p, i, "rename"))
		return !p_kw(&p, j, "to");
	if (p_kw(&p, i, "alter"))
		return !column_action(ts, j) || j >= limit;

	/* ADD PARTITION [name] ... */
	if (j >= limit || p_char(&p, j, ',') || p_char(&p, j, '(') ||
		starts_bound(&p, j) || p_kw(&p, j, "for") ||
		(p_kw(&p, j, "with") && p_char(&p, j + 1, '(')) ||
		(p_kw(&p, j, "without") && p_kw(&p, j + 1, "oids")) ||
		p_kw(&p, j, "tablespace"))
		return true;
	if (tok_is_name(ts, j))
	{
		int			k = j + 1;

		/* ADD partition typename: PostgreSQL's, a column */
		return starts_bound(&p, k) ||
			(p_kw(&p, k, "with") && p_char(&p, k + 1, '(')) ||
			(p_kw(&p, k, "without") && p_kw(&p, k + 1, "oids")) ||
			p_kw(&p, k, "tablespace") ||
			(p_char(&p, k, '(') &&
			 (p_word(&p, k + 1, "subpartition") || p_kw(&p, k + 1, "default") ||
			  starts_bound(&p, k + 1) || p_kw(&p, k + 1, "column")));
	}
	return true;
}

/*
 * A partition's name, or FOR (value), at token i
 * (alter_table_partition_id_spec).
 */
static GpPartId *
parse_id(GpPartParser *p, int i, int *next)
{
	GpPartId   *id = palloc0(sizeof(GpPartId));

	id->location = (i < p->ts->ntoks) ? p->ts->toks[i].off : p->ts->srclen;

	if (p_kw(p, i, "for"))
	{
		int			close = p_parens(p, i + 1);

		if (close == i + 2)		/* FOR () */
			p_syntax_error(p, close);

		/*
		 * FOR (RANK(n)) was how Greenplum 6 named a partition by its
		 * position, and Cloudberry recognises it to say so; any other call
		 * there is where its grammar stops, at the parenthesis after it.
		 */
		if (p_colid(p, i + 2) && p_char(p, i + 3, '(') &&
			match_close(p->ts, i + 3, p->limit) == close - 1)
		{
			if (!tok_is_word(p->ts, i + 2, "rank"))
				p_syntax_error(p, close);
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("addressing partition by RANK is no longer supported"),
					 errhint("Use partition name or FOR (<partition key value>) instead."),
					 errposition(p_errpos(p, p->ts->toks[i + 2].off))));
		}

		id->kind = GP_PART_ID_VALUE;
		id->values = inside(p, i + 1, close);
		id->location = p->ts->toks[i + 2].off;
		if (p->check_exprs)
			(void) GpPartExprList(p, id->values, true);
		*next = close + 1;
		return id;
	}

	if (!p_partname(p, i))
		p_syntax_error(p, i);
	id->kind = GP_PART_ID_NAME;
	id->name = tok_name(p->ts, i);
	*next = i + 1;
	return id;
}

static bool
starts_id(const GpPartParser *p, int i)
{
	return p_kw(p, i, "for") || p_partname(p, i);
}

/* PARTITION id, or DEFAULT PARTITION, at token i. */
static GpPartId *
parse_id_or_default(GpPartParser *p, int i, int *next)
{
	if (p_kw(p, i, "partition"))
		return parse_id(p, i + 1, next);

	if (p_kw(p, i, "default") && p_kw(p, i + 1, "partition"))
	{
		GpPartId   *id;

		if (starts_id(p, i + 2))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("cannot specify a name, rank, or value for a DEFAULT partition in this context")));
		id = palloc0(sizeof(GpPartId));
		id->kind = GP_PART_ID_DEFAULT;
		id->location = p->ts->toks[i].off;
		*next = i + 2;
		return id;
	}

	p_syntax_error(p, i);
	return NULL;				/* not reached */
}

static DropBehavior
parse_behavior(GpPartParser *p, int *i)
{
	if (p_kw(p, *i, "cascade"))
	{
		(*i)++;
		return DROP_CASCADE;
	}
	if (p_kw(p, *i, "restrict"))
		(*i)++;
	return DROP_RESTRICT;
}

/* The command after ALTER PARTITION id, which is any command of ALTER
 * TABLE's: the partition ones, SET TABLESPACE, or another. */
static GpPartCmd *
parse_nested(GpPartParser *p, int i, int *end)
{
	GpPartCmd  *cmd;
	int			depth = 0;
	int			j;

	if ((p_kw(p, i, "add") || p_kw(p, i, "drop") || p_kw(p, i, "alter") ||
		 p_kw(p, i, "rename") || p_kw(p, i, "truncate") ||
		 p_word(p, i, "exchange") || p_word(p, i, "split")) &&
		(p_kw(p, i + 1, "partition") ||
		 (p_kw(p, i + 1, "default") && p_kw(p, i + 2, "partition"))))
		return parse_cmd(p, i, true, end);
	if (p_kw(p, i, "set") && p_word(p, i + 1, "subpartition"))
		return parse_cmd(p, i, true, end);

	cmd = palloc0(sizeof(GpPartCmd));
	cmd->location = (i < p->ts->ntoks) ? p->ts->toks[i].off : p->ts->srclen;

	if (p_kw(p, i, "set") && p_kw(p, i + 1, "tablespace"))
	{
		if (!p_colid(p, i + 2))
			p_syntax_error(p, i + 2);
		cmd->kind = GP_PART_CMD_SET_TABLESPACE;
		cmd->tablespace = tok_name(p->ts, i + 2);
		*end = i + 3;
		return cmd;
	}

	/* Another: to the end of the command, and PostgreSQL's grammar reads it. */
	for (j = i; j < p->limit; j++)
	{
		if (p_char(p, j, '(') || p_char(p, j, '['))
			depth++;
		else if (p_char(p, j, ')') || p_char(p, j, ']'))
			depth--;
		else if (depth == 0 && p_char(p, j, ','))
			break;
	}
	if (j == i)
		p_syntax_error(p, i);
	cmd->kind = GP_PART_CMD_OTHER;
	cmd->other = span_of(p, i, j - 1);
	if (p->check_exprs)
		(void) p_parse(p, "ALTER TABLE gp_partition_other ", cmd->other, "");
	*end = j;
	return cmd;
}

/* The ALTER TABLE command at token i; nested inside ALTER PARTITION. */
static GpPartCmd *
parse_cmd(GpPartParser *p, int i, bool nested, int *end)
{
	GpPartCmd  *cmd = palloc0(sizeof(GpPartCmd));
	int			j;

	cmd->location = p->ts->toks[i].off;
	cmd->at.from = -1;
	cmd->validation = -1;

	if (p_kw(p, i, "add"))
	{
		GpPartElem *elem = palloc0(sizeof(GpPartElem));

		cmd->kind = GP_PART_CMD_ADD;
		cmd->elem = elem;
		elem->with.from = -1;

		if (p_kw(p, i + 1, "default"))
		{
			/* ADD DEFAULT PARTITION name, and no boundary */
			if (!p_kw(p, i + 2, "partition"))
				p_syntax_error(p, i + 2);
			cmd->id = parse_id(p, i + 3, &j);
			if (cmd->id->kind != GP_PART_ID_NAME)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("can only ADD a partition by name")));
			elem->is_default = true;
			elem->name = cmd->id->name;
			elem->location = p_kw(p, j, "with") ? p->ts->toks[j].off : -1;
		}
		else
		{
			if (!p_kw(p, i + 1, "partition"))
				p_syntax_error(p, i + 1);
			j = i + 2;
			elem->location = -1;	/* bison's, for an empty boundary */

			/* START ( is a boundary, and start START ( a name and one */
			if (!(starts_bound(p, j) && p_char(p, j + 1, '(')) && starts_id(p, j))
			{
				cmd->id = parse_id(p, j, &j);
				if (cmd->id->kind != GP_PART_ID_NAME)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("can only ADD a partition by name")));
				elem->name = cmd->id->name;
			}
			if (starts_bound(p, j))
			{
				elem->location = p->ts->toks[j].off;
				elem->bound = parse_bound(p, j, false, &j);
			}
		}
		j = parse_elem_tail(p, j, elem, false);
	}
	else if (p_kw(p, i, "alter"))
	{
		cmd->kind = GP_PART_CMD_ALTER;
		cmd->id = parse_id_or_default(p, i + 1, &j);
		cmd->sub = parse_nested(p, j, &j);
	}
	else if (p_kw(p, i, "drop"))
	{
		cmd->kind = GP_PART_CMD_DROP;
		if (p_kw(p, i + 1, "default") && p_kw(p, i + 2, "partition") &&
			p_kw(p, i + 3, "if") && p_kw(p, i + 4, "exists"))
		{
			cmd->missing_ok = true;
			cmd->id = palloc0(sizeof(GpPartId));
			cmd->id->kind = GP_PART_ID_DEFAULT;
			cmd->id->location = p->ts->toks[i + 1].off;
			j = i + 5;
		}
		else if (p_kw(p, i + 1, "partition") && p_kw(p, i + 2, "if") &&
				 p_kw(p, i + 3, "exists"))
		{
			cmd->missing_ok = true;
			cmd->id = parse_id(p, i + 4, &j);
		}
		else
			cmd->id = parse_id_or_default(p, i + 1, &j);
		cmd->behavior = parse_behavior(p, &j);
	}
	else if (p_word(p, i, "exchange"))
	{
		int			k;

		cmd->kind = GP_PART_CMD_EXCHANGE;
		cmd->id = parse_id_or_default(p, i + 1, &j);
		if (!p_kw(p, j, "with"))
			p_syntax_error(p, j);
		if (!p_kw(p, j + 1, "table"))
			p_syntax_error(p, j + 1);
		/* qualified_name */
		k = j + 2;
		if (!p_colid(p, k))
			p_syntax_error(p, k);
		k++;
		while (p_char(p, k, '.') && k + 1 < p->limit && tok_is_name(p->ts, k + 1))
			k += 2;
		cmd->table = span_of(p, j + 2, k - 1);
		j = k;
		if (p_kw(p, j, "with") || p_kw(p, j, "without"))
		{
			if (!p_word(p, j + 1, "validation"))
				p_syntax_error(p, j + 1);
			cmd->validation = p_kw(p, j, "with") ? 1 : 0;
			j += 2;
		}
	}
	else if (p_kw(p, i, "rename"))
	{
		cmd->kind = GP_PART_CMD_RENAME;
		cmd->id = parse_id_or_default(p, i + 1, &j);
		if (!p_kw(p, j, "to"))
			p_syntax_error(p, j);
		/* TO IDENT: a keyword will not do */
		if (j + 1 >= p->limit || p->ts->toks[j + 1].code != GP_IDENT)
			p_syntax_error(p, j + 1);
		cmd->newname = tok_name(p->ts, j + 1);
		j += 2;
	}
	else if (p_kw(p, i, "set"))
	{
		cmd->kind = GP_PART_CMD_SET_TEMPLATE;
		if (!p_word(p, i + 1, "subpartition"))
			p_syntax_error(p, i + 1);
		if (!p_kw(p, i + 2, "template"))
			p_syntax_error(p, i + 2);
		j = i + 3;
		if (p_char(p, j, '(') && p_char(p, j + 1, ')'))
			j += 2;				/* SET SUBPARTITION TEMPLATE (): none */
		else
			cmd->template_def = parse_template(p, j, &j);
	}
	else if (p_word(p, i, "split"))
	{
		cmd->kind = GP_PART_CMD_SPLIT;
		if (p_kw(p, i + 1, "default"))
		{
			if (!p_kw(p, i + 2, "partition"))
				p_syntax_error(p, i + 2);
			cmd->id = palloc0(sizeof(GpPartId));
			cmd->id->kind = GP_PART_ID_DEFAULT;
			cmd->id->location = p->ts->toks[i + 1].off;
			j = i + 3;
			if (p_kw(p, j, "start"))
			{
				cmd->start = parse_range_item(p, j, &j);
				if (!p_kw(p, j, "end"))
					p_syntax_error(p, j);
				cmd->split_location = p->ts->toks[j].off;	/* gram.y's @5 */
				cmd->end = parse_range_item(p, j, &j);
			}
			else if (!p_kw(p, j, "at"))
				p_syntax_error(p, j);
		}
		else
		{
			if (!p_kw(p, i + 1, "partition"))
				p_syntax_error(p, i + 1);
			cmd->id = parse_id(p, i + 2, &j);
			if (!p_kw(p, j, "at"))
				p_syntax_error(p, j);
		}

		if (cmd->start == NULL)
		{
			/* AT (value, ...) or AT (VALUES (...)) */
			int			close = p_parens(p, j + 1);

			if (close == j + 2)
				p_syntax_error(p, close);
			cmd->split_location = p->ts->toks[j + 2].off;	/* gram.y's @6 */
			if (p_kw(p, j + 2, "values"))
			{
				int			vclose = p_parens(p, j + 3);

				if (vclose + 1 != close)
					p_syntax_error(p, vclose + 1);
				cmd->at = inside(p, j + 3, vclose);
				cmd->at_values = true;
			}
			else
				cmd->at = inside(p, j + 1, close);
			if (p->check_exprs)
				(void) GpPartExprList(p, cmd->at, true);
			j = close + 1;
		}

		if (p_kw(p, j, "into"))
		{
			int			close = p_parens(p, j + 1);
			int			k;

			cmd->into1 = parse_id_or_default(p, j + 2, &k);
			if (cmd->into1->kind != GP_PART_ID_NAME)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("INTO can only have first partition by name"),
						 errposition(p_errpos(p, cmd->into1->location))));
			if (!p_char(p, k, ','))
				p_syntax_error(p, k);
			cmd->into2 = parse_id_or_default(p, k + 1, &k);
			if (k != close)
				p_syntax_error(p, k);
			if (cmd->id->kind != GP_PART_ID_DEFAULT &&
				cmd->into2->kind != GP_PART_ID_NAME)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("INTO can only have second partition by name"),
						 errposition(p_errpos(p, p->ts->toks[j].off))));
			j = close + 1;
		}
	}
	else if (p_kw(p, i, "truncate"))
	{
		cmd->kind = GP_PART_CMD_TRUNCATE;
		cmd->id = parse_id_or_default(p, i + 1, &j);
		cmd->behavior = parse_behavior(p, &j);
	}
	else
		p_syntax_error(p, i);

	/*
	 * The command ends here.  What may follow it is another command of the
	 * statement's, or the statement's end; the command it is nested in ends
	 * with it.
	 */
	if (!nested && !(j >= p->limit || p_char(p, j, ',')))
		p_syntax_error(p, j);

	*end = j;
	return cmd;
}

GpPartCmd *
GpPartParseCmd(GpPartParser *p, int i, int *end)
{
	return parse_cmd(p, i, false, end);
}
