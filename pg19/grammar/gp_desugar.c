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
 * gp_desugar.c
 *	  Cloudberry's SQL, rewritten into PostgreSQL 19's.
 *
 * O26 puts a hook at the top of raw_parser so that an extension can read a
 * statement before PostgreSQL's grammar does.  Decision 11 says what the hook
 * must give back: only PG19 parse nodes -- namespaced options, security
 * labels and function calls, which are the forms the modules of this port
 * already handle.  Then pg_dump, event triggers and every ProcessUtility hook
 * see standard statements, and nothing downstream has to know that
 * Cloudberry's syntax exists.
 *
 * ONE STATEMENT FOR ONE.  A statement of the user's is one statement of
 * PostgreSQL's, never a statement followed by others: a string of statements
 * cannot be prepared, runs as one implicit transaction, and answers with a
 * result nobody asked for.  So what a clause of Cloudberry's becomes is, in
 * this order of preference:
 *
 *   - an option of its own statement, where PostgreSQL's grammar has a list
 *     for one -- WITH (gp_tag.env = 'prod'), a foreign table's OPTIONS, a
 *     function's SET gp.execute_on -- which a ProcessUtility hook takes out
 *     again before PostgreSQL would refuse it;
 *   - where it has none, a DefElem put on the statement's parse node once the
 *     grammar has built it (GpAttachCarriers), which the same hooks take out;
 *   - and a statement PostgreSQL has no counterpart of at all -- CREATE TAG,
 *     CREATE PROFILE, CREATE TASK -- is a CALL of a procedure, which answers
 *     with a command tag and no row, as the statement did in Cloudberry.
 *
 * What this is, and what it is not.  "Porting the Cloudberry code" describes
 * the grammar as a fork of PG19's gram.y with Cloudberry's productions
 * applied, re-generated whenever either changes.  This is not that.  It is a
 * rewriter over PostgreSQL's own scanner: the statement is tokenised with
 * core_yylex, the parts written in Cloudberry's spelling are replaced by the
 * standard spelling, and the result is handed to standard_raw_parser.  The
 * scanner is the real one, so comments, dollar quoting, Unicode escapes and
 * standard_conforming_strings behave exactly as they do everywhere else.
 *
 * What a fork would give that this does not: nothing a user sees any more.
 * Three things were counted, and none holds:
 *
 *   - Cloudberry's syntax nested inside an expression is reached: DECODE and
 *     CASE x WHEN IS NOT DISTINCT FROM y, the only such syntax the port
 *     takes, are found wherever they are, nested in each other or not.
 *     (MEDIAN(x) needs no rewrite, being a call in PostgreSQL's grammar.)
 *   - Positions are the user's.  The rewritten text records where each byte
 *     of it came from, and the caret under a syntax error, and every
 *     location in the parse tree, is put back where the user wrote it --
 *     text the rewrite wrote standing for the token it replaces.
 *   - A comment written inside a clause that is replaced is dropped with it,
 *     but only from the text handed to the grammar, which nothing else
 *     reads.  Parse analysis, the logs and pg_stat_statements are given the
 *     text the user sent, and a fork's scanner would skip the comment just
 *     as this one does.
 *
 * What it gives that a fork would not: no copy of PostgreSQL's grammar, of
 * 20,000 lines, to re-base whenever PostgreSQL changes it.  What it keeps
 * instead is gp_parseloc.c's list of the nodes that carry a location, which
 * each release that adds one has to be checked against.
 *
 * Cloudberry source this file is made of:
 *	  the Cloudberry-only productions of src/backend/parser/gram.y
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/keywords.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "parser/parser.h"
#include "parser/scanner.h"
#include "parser/scansup.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/guc.h"
#include "utils/json.h"
#include "utils/regproc.h"

#include "cb_module.h"
#include "gp_dispatch.h"
#include "gp_grammar.h"
#include "gp_grammar_int.h"
#include "gp_partition.h"

/*
 * Words that can only appear in Cloudberry's spelling of something.  A
 * statement with none of them cannot need rewriting, and finding that out is
 * one pass over the text rather than a whole lexing.
 */
static const char *const gp_trigger_words[] = {
	"tag", "profile", "noprofile", "distributed", "randomly", "replicated",
	"task", "directory", "storage", "dynamic", "incremental", "unset",
	"account", "execute", "decode", "subpartition", "gp_dist_random",
	"orientation", "encoding",
	"reorganize", "external", "reject", "protocol",
	"createexttable", "nocreateexttable", "newline", "resource", "deny",
	NULL
};

/*
 * The classic partition clauses all say PARTITION, and so do a window's
 * PARTITION BY and PostgreSQL's own partitioning, which are far commoner.
 * Every one of Cloudberry's is in a CREATE or an ALTER, so the word counts
 * only in a text that has one of those too: a query with OVER (PARTITION BY
 * ...) is not tokenised for it.  So do ALTER TABLE's EXPAND and SHRINK,
 * words a query may use for anything, and its SET WITH (...) and SET ACCESS
 * METHOD ... WITH (...), whose options may be any.
 */
static const char *const gp_trigger_ddl_words[] = {"partition", "expand", "shrink", "with", NULL};
static const char *const gp_ddl_words[] = {"create", "alter", NULL};

/* Does one of `words` start at str[i]? */
static bool
word_at(const char *str, int len, int i, const char *const *words)
{
	for (int w = 0; words[w] != NULL; w++)
	{
		int			wl = strlen(words[w]);

		if (pg_tolower((unsigned char) str[i]) == words[w][0] &&
			i + wl <= len && pg_strncasecmp(str + i, words[w], wl) == 0)
			return true;
	}
	return false;
}

/*
 * Two-word triggers, for a clause whose words are each too common to list
 * above.
 *
 * The data-access attributes are all "<word> SQL".  "sql" on its own would
 * fire on every LANGUAGE sql, which is most function DDL, and "no" on a large
 * share of ordinary SQL; the pair fires on neither.  This is a prefilter, so
 * being approximate is allowed in one direction only -- it may say yes to a
 * statement with nothing to rewrite, but a no must be right.  The one thing
 * it would miss is a comment between the two words, which nothing writes.
 *
 * WHEN IS is CASE x WHEN IS NOT DISTINCT FROM y, and MISSING FIELDS COPY's
 * FILL MISSING FIELDS.  The second word of a pair is matched as the start of
 * one, so WHEN is_active fires it too, which costs a tokenisation and
 * rewrites nothing.
 */
static const char *const gp_trigger_pairs[][2] = {
	{"no", "sql"},
	{"contains", "sql"},
	{"reads", "sql"},
	{"modifies", "sql"},
	{"when", "is"},
	{"missing", "fields"},
	{NULL, NULL}
};

/* ------------------------------------------------------------------------- */
/* Tokens                                                                    */
/* ------------------------------------------------------------------------- */

static bool
is_word_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		(c >= '0' && c <= '9') || c == '_';
}

static bool
looks_interesting(const char *str)
{
	int			len = strlen(str);
	bool		ddl_word = false;
	bool		ddl = false;

	for (int i = 0; i < len; i++)
	{
		/* Only where a word can start, so this is one pass and no more. */
		if (i > 0 && is_word_char(str[i - 1]))
			continue;

		if (!ddl_word && word_at(str, len, i, gp_trigger_ddl_words))
			ddl_word = true;
		if (!ddl && word_at(str, len, i, gp_ddl_words))
			ddl = true;
		if (ddl_word && ddl)
			return true;

		for (int w = 0; gp_trigger_words[w] != NULL; w++)
		{
			int			wl = strlen(gp_trigger_words[w]);

			if (pg_tolower((unsigned char) str[i]) != gp_trigger_words[w][0])
				continue;
			if (i + wl <= len &&
				pg_strncasecmp(str + i, gp_trigger_words[w], wl) == 0)
				return true;
		}

		/* count(), which Cloudberry takes for count(*): see count_close() */
		if (pg_tolower((unsigned char) str[i]) == 'c' && i + 5 <= len &&
			pg_strncasecmp(str + i, "count", 5) == 0)
		{
			int			j = i + 5;

			while (j < len && scanner_isspace(str[j]))
				j++;
			if (j < len && str[j] == '(')
			{
				j++;
				while (j < len && scanner_isspace(str[j]))
					j++;
				if (j < len && str[j] == ')')
					return true;
			}
		}

		for (int w = 0; gp_trigger_pairs[w][0] != NULL; w++)
		{
			int			wl = strlen(gp_trigger_pairs[w][0]);
			int			sl = strlen(gp_trigger_pairs[w][1]);
			int			j;

			if (pg_tolower((unsigned char) str[i]) != gp_trigger_pairs[w][0][0])
				continue;
			if (i + wl > len ||
				pg_strncasecmp(str + i, gp_trigger_pairs[w][0], wl) != 0)
				continue;

			/* The first word has to end here, or "no" would match "node". */
			j = i + wl;
			if (j < len && is_word_char(str[j]))
				continue;

			while (j < len && !is_word_char(str[j]))
				j++;

			if (j + sl <= len &&
				pg_strncasecmp(str + j, gp_trigger_pairs[w][1], sl) == 0)
				return true;
		}
	}

	return false;
}

/*
 * Tokenise with the server's own scanner, so that what counts as a token here
 * is what counts as one everywhere else.
 */
GpTokens *
GpTokenize(const char *str)
{
	core_yyscan_t yyscanner;
	core_yy_extra_type yyextra;
	GpTokens   *out = palloc0(sizeof(GpTokens));
	int			maxtoks = 64;
	core_YYSTYPE yylval;
	YYLTYPE		yylloc;
	int			code;

	out->src = str;
	out->srclen = strlen(str);
	out->toks = palloc(maxtoks * sizeof(GpTok));

	yyscanner = scanner_init(str, &yyextra, &ScanKeywords, ScanKeywordTokens);

	while ((code = core_yylex(&yylval, &yylloc, yyscanner)) != 0)
	{
		GpTok	   *t;

		if (out->ntoks == maxtoks)
		{
			maxtoks *= 2;
			out->toks = repalloc(out->toks, maxtoks * sizeof(GpTok));
		}

		t = &out->toks[out->ntoks++];
		t->code = code;
		t->off = yylloc;
		t->kw = NULL;
		t->str = NULL;
		t->ival = 0;

		if (code >= GP_FIRST_KEYWORD)
			t->kw = yylval.keyword;
		else if (code == GP_IDENT || code == GP_SCONST || code == GP_FCONST ||
				 code == GP_BCONST || code == GP_XCONST || code == GP_OP ||
				 code == GP_UIDENT || code == GP_USCONST)
			t->str = yylval.str;
		else if (code == GP_ICONST || code == GP_PARAM)
			t->ival = yylval.ival;
	}

	scanner_finish(yyscanner);

	return out;
}

/* ------------------------------------------------------------------------- */
/* Where the rewritten text came from                                        */
/* ------------------------------------------------------------------------- */

/*
 * A rewrite moves things.  Every position PostgreSQL reports -- the caret
 * under a syntax error, the location a parse node keeps for the errors of
 * parse analysis -- is an offset into the text the grammar read, and psql
 * puts its caret at that offset of the text the user sent.  Without this,
 * everything after a rewritten part of a statement was reported somewhere
 * else: one DECODE early in a SELECT moved the caret of every error after it.
 *
 * So the rewritten text is built as segments, each either a copy of the
 * user's text or text the rewrite wrote, the latter charged to one place in
 * the user's text: the token it stands for.  gp_raw_parser maps the position
 * of a syntax error through them, and gp_parseloc.c every location in the
 * parse tree, so what is reported is where the user wrote it.
 */
typedef struct GpSeg
{
	int			out;			/* first byte in the rewritten text */
	int			len;
	int			src;			/* where they came from, or what they stand for */
	bool		copied;			/* a copy of the user's text from src onwards */
} GpSeg;

typedef struct GpOut
{
	StringInfoData buf;
	const char *src;			/* the user's text */
	GpSeg	   *segs;
	int			nsegs;
	int			maxsegs;
} GpOut;

struct GpPosMap
{
	GpSeg	   *segs;
	int			nsegs;
	int			outlen;
	int			srclen;
};

static void
out_init(GpOut *o, const char *src)
{
	initStringInfo(&o->buf);
	o->src = src;
	o->nsegs = 0;
	o->maxsegs = 16;
	o->segs = palloc(o->maxsegs * sizeof(GpSeg));
}

/* Record where the next `len` bytes, about to be appended, came from. */
static void
out_seg(GpOut *o, int len, int src, bool copied)
{
	GpSeg	   *last = (o->nsegs > 0) ? &o->segs[o->nsegs - 1] : NULL;

	if (len <= 0)
		return;

	/* A copy that carries on from the one before is the same segment. */
	if (last != NULL && copied && last->copied &&
		last->out + last->len == o->buf.len && last->src + last->len == src)
	{
		last->len += len;
		return;
	}

	if (o->nsegs == o->maxsegs)
	{
		o->maxsegs *= 2;
		o->segs = repalloc(o->segs, o->maxsegs * sizeof(GpSeg));
	}
	o->segs[o->nsegs].out = o->buf.len;
	o->segs[o->nsegs].len = len;
	o->segs[o->nsegs].src = src;
	o->segs[o->nsegs].copied = copied;
	o->nsegs++;
}

/* The user's text [from, to), as it is. */
static void
out_copy(GpOut *o, int from, int to)
{
	if (to <= from)
		return;
	out_seg(o, to - from, from, true);
	appendBinaryStringInfo(&o->buf, o->src + from, to - from);
}

/* Text of the rewrite's own, standing for the user's text at `at`. */
static void
out_text(GpOut *o, const char *text, int at)
{
	int			len = strlen(text);

	out_seg(o, len, at, false);
	appendBinaryStringInfo(&o->buf, text, len);
}

/* Another piece, built over the same user's text. */
static void
out_append(GpOut *o, const GpOut *part)
{
	for (int k = 0; k < part->nsegs; k++)
	{
		const GpSeg *s = &part->segs[k];

		out_seg(o, s->len, s->src, s->copied);
		appendBinaryStringInfo(&o->buf, part->buf.data + s->out, s->len);
	}
}

/*
 * GpPosMapSource
 *		The offset in the user's text that an offset in the rewritten text
 *		stands for: within a copy, the byte it was copied from; within text
 *		the rewrite wrote, the token that text stands for; at the end, the
 *		end.
 */
int
GpPosMapSource(const GpPosMap *map, int offset)
{
	int			lo = 0;
	int			hi = map->nsegs - 1;
	const GpSeg *s;

	if (offset < 0)
		return offset;
	if (offset >= map->outlen || map->nsegs == 0)
		return map->srclen;

	/* the last segment that starts at or before the offset */
	while (lo < hi)
	{
		int			mid = (lo + hi + 1) / 2;

		if (map->segs[mid].out <= offset)
			lo = mid;
		else
			hi = mid - 1;
	}

	s = &map->segs[lo];
	return s->copied ? s->src + (offset - s->out) : s->src;
}

/*
 * GpPosMapCopied
 *		As GpPosMapSource, but -1 -- unknown -- for an offset in text the
 *		rewrite wrote.
 *
 * For what is only in the rewrite: a constant it wrote, such as the name a
 * call of gp_sql's is given.  pg_stat_statements replaces each constant of a
 * statement by $n at its location, in the user's text, and requires every
 * one to be inside the statement; a constant charged to the clause it came
 * from can be neither, and one of an added statement was before its start,
 * which stops an assert-enabled server.  Unknown, it is left alone.
 */
int
GpPosMapCopied(const GpPosMap *map, int offset)
{
	int			lo = 0;
	int			hi = map->nsegs - 1;
	const GpSeg *s;

	if (offset < 0)
		return offset;
	if (offset >= map->outlen || map->nsegs == 0)
		return map->srclen;

	while (lo < hi)
	{
		int			mid = (lo + hi + 1) / 2;

		if (map->segs[mid].out <= offset)
			lo = mid;
		else
			hi = mid - 1;
	}

	s = &map->segs[lo];
	return s->copied ? s->src + (offset - s->out) : -1;
}

GpPosMap *
GpPosMapSpan(int prefix, int from, int len, int suffix, int srclen)
{
	GpPosMap   *m = palloc(sizeof(GpPosMap));
	GpSeg	   *s = palloc(3 * sizeof(GpSeg));
	int			n = 0;

	if (prefix > 0)
		s[n++] = (GpSeg) {0, prefix, from, false};
	if (len > 0)
		s[n++] = (GpSeg) {prefix, len, from, from >= 0};
	if (suffix > 0)
		s[n++] = (GpSeg) {prefix + len, suffix, from >= 0 ? from + len : -1, false};

	m->segs = s;
	m->nsegs = n;
	m->outlen = prefix + len + suffix;
	m->srclen = srclen;
	return m;
}

/* ------------------------------------------------------------------------- */
/* Rewriting                                                                 */
/* ------------------------------------------------------------------------- */

/*
 * One statement is rewritten into a buffer: the parts that are already
 * PostgreSQL's are copied across, and the parts that are Cloudberry's are
 * replaced, or taken out and carried on the parse node instead.  Nothing is
 * ever added as a statement of its own (see "one statement for one" above).
 */
typedef struct GpEdit
{
	int			from;			/* byte offset, inclusive */
	int			to;				/* byte offset, exclusive */
	char	   *text;			/* what goes there instead */
	GpOut	   *piece;			/* or this, which keeps where its parts came
								 * from; for an expression's rewrite */
	int			seq;			/* the order it was made in */
} GpEdit;

typedef struct GpRewrite
{
	const GpTokens *ts;
	int			first;			/* first token of the statement */
	int			last;			/* one past its last token */
	bool		changed;
	bool		whole;			/* body replaces the statement outright */
	List	   *edits;			/* GpEdit, in whatever order they were found */
	int			nedits;
	StringInfoData body;		/* what the statement becomes, when whole */
	GpOut		text;			/* the statement with its edits, when not */
	char		object;			/* what find_subject found: 't' a table, 'f' a
								 * foreign table, 'v' a view, 'm' a materialized
								 * view, 'S' a sequence, 'i' an index, or 0 */
	int			subject_end;	/* the token after the subject's name, or -1 */
	GpOut		options;		/* namespaced options for its WITH list, each
								 * standing for the clause it came from */
	StringInfoData fdw_options; /* a foreign table's, for its OPTIONS list */
	int			fdw_options_at; /* where that list goes when the SERVER
								 * clause is the rewrite's own, or -1 */
	List	   *carriers;		/* DefElem for its parse node; see
								 * GpAttachCarriers */
} GpRewrite;

static void
rw_init(GpRewrite *rw, const GpTokens *ts, int first, int last)
{
	rw->ts = ts;
	rw->first = first;
	rw->last = last;
	rw->changed = false;
	rw->whole = false;
	rw->edits = NIL;
	rw->nedits = 0;
	initStringInfo(&rw->body);
	out_init(&rw->text, ts->src);
	rw->object = 0;
	rw->subject_end = -1;
	out_init(&rw->options, ts->src);
	initStringInfo(&rw->fdw_options);
	rw->fdw_options_at = -1;
	rw->carriers = NIL;
}

/* Replace [from, to) with `text`.  Edits may be found in any order. */
static void
rw_edit(GpRewrite *rw, int from, int to, const char *text)
{
	GpEdit	   *e = palloc(sizeof(GpEdit));

	e->from = from;
	e->to = to;
	e->text = text ? pstrdup(text) : pstrdup("");
	e->piece = NULL;
	e->seq = rw->nedits++;
	rw->edits = lappend(rw->edits, e);
	rw->changed = true;
}

/* Replace [from, to) with a piece that knows where its parts came from. */
static void
rw_edit_piece(GpRewrite *rw, int from, int to, GpOut *piece)
{
	GpEdit	   *e = palloc(sizeof(GpEdit));

	e->from = from;
	e->to = to;
	e->text = NULL;
	e->piece = piece;
	e->seq = rw->nedits++;
	rw->edits = lappend(rw->edits, e);
	rw->changed = true;
}

/*
 * A clause of Cloudberry's that PostgreSQL's grammar has no place for on this
 * statement -- TAG on CREATE SCHEMA, CREATE USER or CREATE SEQUENCE, PROFILE
 * on ALTER USER -- carried to the statement's parse node instead, as a
 * DefElem in the namespace of the module that takes it out again: "gp_tag"
 * for gp_sql's tags, "gp" for gp_security's profiles.  A value of NULL takes
 * the thing away, as UNSET TAG and NOPROFILE do.  `at` is the clause, in the
 * user's text, which is where an error about it is reported.
 */
static void
rw_add_carrier(GpRewrite *rw, const char *nspace, const char *name,
			   const char *value, int at)
{
	DefElem    *def;

	def = makeDefElemExtended(pstrdup(nspace), pstrdup(name),
							  value != NULL ? (Node *) makeString(pstrdup(value)) : NULL,
							  value != NULL ? DEFELEM_SET : DEFELEM_DROP, at);
	rw->carriers = lappend(rw->carriers, def);
	rw->changed = true;
}

/*
 * Cloudberry's syntax error at token i, where PostgreSQL's grammar would give
 * another error, or stop at another token: "syntax error at or near" the
 * token as the user wrote it, with the caret under it.
 */
static void
ts_syntax_error(const GpTokens *ts, int i)
{
	int			off;

	if (i >= ts->ntoks)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("syntax error at end of input"),
				 errposition(pg_mbstrlen_with_len(ts->src, ts->srclen) + 1)));

	off = ts->toks[i].off;
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("syntax error at or near \"%s\"",
					pnstrdup(ts->src + off, tok_stop(ts, i) - off)),
			 errposition(pg_mbstrlen_with_len(ts->src, off) + 1)));
}

static void
rw_syntax_error(const GpRewrite *rw, int i)
{
	ts_syntax_error(rw->ts, i);
}

/* This statement becomes something else entirely; body is what it becomes. */
static void
rw_whole(GpRewrite *rw)
{
	rw->whole = true;
	rw->changed = true;
	rw->edits = NIL;
	resetStringInfo(&rw->body);
}

static int
edit_cmp(const ListCell *a, const ListCell *b)
{
	const GpEdit *ea = (const GpEdit *) lfirst(a);
	const GpEdit *eb = (const GpEdit *) lfirst(b);

	if (ea->from != eb->from)
		return (ea->from < eb->from) ? -1 : 1;
	/* an insertion goes in before the text an edit at the same place cuts */
	if (ea->to != eb->to)
		return (ea->to < eb->to) ? -1 : 1;
	/* and two insertions in the order they were made */
	return (ea->seq < eb->seq) ? -1 : (ea->seq > eb->seq);
}

/*
 * A namespaced option the statement is to carry in its WITH list, such as
 * gp.distributed_by = '(a)': what a clause of Cloudberry's becomes when the
 * statement it is on can take one, so that the statement stays one statement.
 * rw_place_options puts them in, all together, once the clauses are read.
 *
 * The option stands for the clause it came from, at `at` in the user's text,
 * wherever in the statement it is put: that is its DefElem's location once
 * the grammar has built one, and where an error about it is reported.  The
 * classic partition clause depends on it, being carried verbatim in its
 * option: a position in the option's value is one in the user's text,
 * counted from there.
 */
static void
rw_add_option(GpRewrite *rw, const char *option, int at)
{
	if (rw->options.buf.len > 0)
		out_text(&rw->options, ", ", at);
	out_text(&rw->options, option, at);
}

/* Replace [from, to) with the options, between `before` and `after`. */
static void
rw_edit_options(GpRewrite *rw, int from, int to, const char *before,
				const char *after)
{
	GpOut	   *piece = palloc(sizeof(GpOut));

	out_init(piece, rw->ts->src);
	out_text(piece, before, from);
	out_append(piece, &rw->options);
	out_text(piece, after, from);
	rw_edit_piece(rw, from, to, piece);
}

/*
 * Put the statement's options where PostgreSQL's grammar expects them: into
 * a WITH (...) the statement already has, or in a new one just before the
 * first of ON COMMIT, TABLESPACE, AS and WHERE -- where CREATE TABLE, CREATE
 * TABLE AS, CREATE [MATERIALIZED] VIEW and CREATE INDEX each have their WITH
 * -- or else at its end, where the clauses taken out of it were.  WITHOUT
 * OIDS, the other thing that can stand where WITH does, gives way to one.
 */
static void
rw_place_options(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			depth = 0;
	int			at;

	if (rw->options.buf.len == 0 || rw->whole)
		return;

	for (int j = (rw->subject_end >= 0 ? rw->subject_end : rw->first); j < rw->last; j++)
	{
		if (tok_is_char(ts, j, '('))
		{
			if (depth == 0 && tok_is_kw(ts, j - 1, "with"))
			{
				rw_edit_options(rw, tok_end(ts, j), tok_end(ts, j), "", ", ");
				return;
			}
			depth++;
			continue;
		}
		if (tok_is_char(ts, j, ')'))
		{
			depth--;
			continue;
		}
		if (depth != 0)
			continue;

		if (tok_is_kw(ts, j, "without") && tok_is(ts, j + 1, "oids"))
		{
			rw_edit_options(rw, ts->toks[j].off, tok_end(ts, j + 1),
							"WITH (", ")");
			return;
		}
		if ((tok_is_kw(ts, j, "on") && tok_is_kw(ts, j + 1, "commit")) ||
			tok_is_kw(ts, j, "tablespace") || tok_is_kw(ts, j, "as") ||
			tok_is_kw(ts, j, "where"))
		{
			rw_edit_options(rw, ts->toks[j].off, ts->toks[j].off,
							"WITH (", ") ");
			return;
		}
	}

	at = (rw->last < ts->ntoks) ? ts->toks[rw->last].off : ts->srclen;
	rw_edit_options(rw, at, at, " WITH (", ")");
}

/*
 * An option a foreign table is to carry in its OPTIONS list: a TAG or a
 * DISTRIBUTED BY on CREATE FOREIGN TABLE, which has no WITH list for them.
 * Its name is quoted, "gp_tag.env" 'prod', because a generic option's name is
 * one identifier; gp_sql's ProcessUtility hook takes these out before the
 * wrapper's validator would refuse them.
 */
static void
rw_add_fdw_option(GpRewrite *rw, const char *name, const char *value)
{
	if (rw->fdw_options.len > 0)
		appendStringInfoString(&rw->fdw_options, ", ");
	appendStringInfo(&rw->fdw_options, "%s %s",
					 quote_identifier(name), quote_literal_cstr(value));
}

/*
 * Put a foreign table's options where PostgreSQL's grammar has them: into the
 * OPTIONS (...) after SERVER name, or a new one there.  Cloudberry's clauses
 * come after that list, so it is always before them.
 */
static void
rw_place_fdw_options(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			depth = 0;

	if (rw->fdw_options.len == 0 || rw->whole)
		return;

	/* after the SERVER clause the rewrite wrote itself */
	if (rw->fdw_options_at >= 0)
	{
		rw_edit(rw, rw->fdw_options_at, rw->fdw_options_at,
				psprintf(" OPTIONS (%s)", rw->fdw_options.data));
		return;
	}

	for (int j = (rw->subject_end >= 0 ? rw->subject_end : rw->first); j < rw->last; j++)
	{
		if (tok_is_char(ts, j, '('))
			depth++;
		else if (tok_is_char(ts, j, ')'))
			depth--;
		if (depth != 0 || !tok_is_kw(ts, j, "server") || !tok_is_name(ts, j + 1))
			continue;

		if (tok_is_kw(ts, j + 2, "options") && tok_is_char(ts, j + 3, '('))
		{
			int			close = skip_parens(ts, j + 3) - 1;

			rw_edit(rw, ts->toks[close].off, ts->toks[close].off,
					psprintf(", %s", rw->fdw_options.data));
		}
		else
			rw_edit(rw, tok_stop(ts, j + 1), tok_stop(ts, j + 1),
					psprintf(" OPTIONS (%s)", rw->fdw_options.data));
		return;
	}

	/* No SERVER: not a statement PostgreSQL will take, and it says so. */
}

/*
 * Put the statement together: the source, with the edits applied in order.
 * Text an edit writes stands for the place it was written at.
 */
static void
rw_finish_body(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			start = ts->toks[rw->first].off;
	int			end = (rw->last < ts->ntoks) ? ts->toks[rw->last].off : ts->srclen;
	int			copied = start;
	ListCell   *lc;

	if (rw->whole)
		return;

	list_sort(rw->edits, edit_cmp);

	foreach(lc, rw->edits)
	{
		GpEdit	   *e = (GpEdit *) lfirst(lc);

		if (e->from < copied)	/* two edits over the same text */
			continue;
		out_copy(&rw->text, copied, e->from);
		if (e->piece != NULL)
			out_append(&rw->text, e->piece);
		else
			out_text(&rw->text, e->text, e->from);
		copied = e->to;
	}

	out_copy(&rw->text, copied, end);
}

/* The source text of tokens [from, to). */
static char *
rw_text(const GpTokens *ts, int from, int to)
{
	int			start = ts->toks[from].off;
	int			end = (to > from) ? tok_end(ts, to - 1) : start;
	char	   *s = palloc(end - start + 1);

	memcpy(s, ts->src + start, end - start);
	s[end - start] = '\0';

	/* Trailing whitespace comes from ending a span at the next token. */
	for (int i = end - start - 1; i >= 0 && (s[i] == ' ' || s[i] == '\t' ||
											 s[i] == '\n' || s[i] == '\r'); i--)
		s[i] = '\0';

	return s;
}

/* A qualified name starting at `i`: name[.name[.name]].  Returns the end. */
static int
skip_qualified_name(const GpTokens *ts, int i)
{
	if (!tok_is_name(ts, i))
		return i;
	i++;
	while (tok_is_char(ts, i, '.') && tok_is_name(ts, i + 1))
		i += 2;
	return i;
}

/* A comma-separated list of string constants; returns the end. */
static int
collect_strings(const GpTokens *ts, int i, StringInfo out)
{
	bool		first = true;

	appendStringInfoString(out, "ARRAY[");
	while (tok_is_string(ts, i))
	{
		if (!first)
			appendStringInfoString(out, ", ");
		appendStringInfoString(out, quote_literal_cstr(ts->toks[i].str));
		first = false;
		i++;
		if (!tok_is_char(ts, i, ','))
			break;
		i++;
	}
	appendStringInfoString(out, "]::text[]");

	return i;
}

/*
 * A comma-separated list of names, each an identifier as the scanner gives
 * it -- downcased, or dequoted -- which is what a tag, a profile or a task is
 * called.  A qualified name is none of those, and makes the list NIL, so
 * that the statement is left for PostgreSQL's grammar to refuse.
 */
static void
collect_names(const GpTokens *ts, int i, List **names)
{
	for (;;)
	{
		if (!tok_is_name(ts, i))
			break;
		if (tok_is_char(ts, i + 1, '.'))
		{
			*names = NIL;
			return;
		}
		*names = lappend(*names, tok_name(ts, i));
		i++;
		if (!tok_is_char(ts, i, ','))
			break;
		i++;
	}
}

/* ARRAY['a', 'b']::<type>[] of a list of names. */
static char *
name_array(List *names, const char *type)
{
	StringInfoData buf;
	ListCell   *lc;

	initStringInfo(&buf);
	appendStringInfoString(&buf, "ARRAY[");
	foreach(lc, names)
		appendStringInfo(&buf, "%s%s", lc == list_head(names) ? "" : ", ",
						 quote_literal_cstr((char *) lfirst(lc)));
	appendStringInfo(&buf, "]::%s[]", type);
	return buf.data;
}

/* ------------------------------------------------------------------------- */
/* Statements that become a CALL                                             */
/* ------------------------------------------------------------------------- */

/*
 * These are statements PostgreSQL has no counterpart of, and each becomes a
 * CALL of the procedure that does what it did.  A CALL answers as a DDL
 * statement does -- a command tag and no row -- where the SELECT of a
 * function they were before answered with a row of its own: psql printed it,
 * and a driver told to expect no result set, as JDBC's executeUpdate is, was
 * handed one.  The tag is CALL, because PostgreSQL's list of command tags is
 * fixed and has no CREATE TAG in it.
 */

/*
 * CREATE TAG [IF NOT EXISTS] name [ALLOWED_VALUES 'a', 'b']
 *	 -> CALL gp_sql.create_tag('name', ARRAY[...], if_not_exists)
 */
static bool
rw_create_tag(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			i = rw->first;
	bool		if_not_exists = false;
	int			nameend;
	StringInfoData values;

	if (!tok_is(ts, i, "create") || !tok_is(ts, i + 1, "tag"))
		return false;
	i += 2;

	if (tok_is(ts, i, "if") && tok_is(ts, i + 1, "not") && tok_is(ts, i + 2, "exists"))
	{
		if_not_exists = true;
		i += 3;
	}

	if (!tok_is_name(ts, i))
		return false;
	nameend = i + 1;

	initStringInfo(&values);
	if (tok_is(ts, nameend, "allowed_values"))
		(void) collect_strings(ts, nameend + 1, &values);
	else
		appendStringInfoString(&values, "NULL");

	rw_whole(rw);
	appendStringInfo(&rw->body, "CALL gp_sql.create_tag(%s, %s, %s)",
					 quote_literal_cstr(tok_name(ts, i)), values.data,
					 if_not_exists ? "true" : "false");
	return true;
}

/*
 * ALTER TAG [IF EXISTS] name { ADD | DROP } ALLOWED_VALUES 'a', ...
 * ALTER TAG [IF EXISTS] name UNSET ALLOWED_VALUES
 * ALTER TAG [IF EXISTS] name RENAME TO newname
 * ALTER TAG name OWNER TO role
 *
 * The form is matched before the statement is taken over: one that is none
 * of these is left to the grammar, which refuses it, rather than rewritten
 * into nothing.
 */
static bool
rw_alter_tag(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			i = rw->first;
	bool		missing_ok = false;
	char	   *name;
	StringInfoData values;

	if (!tok_is(ts, i, "alter") || !tok_is(ts, i + 1, "tag"))
		return false;
	i += 2;

	if (tok_is(ts, i, "if") && tok_is(ts, i + 1, "exists"))
	{
		missing_ok = true;
		i += 2;
	}

	if (!tok_is_name(ts, i))
		return false;
	name = tok_name(ts, i);
	i++;

	if (tok_is(ts, i, "rename") && tok_is(ts, i + 1, "to") && tok_is_name(ts, i + 2))
	{
		rw_whole(rw);
		appendStringInfo(&rw->body, "CALL gp_sql.rename_tag(%s, %s, missing_ok => %s)",
						 quote_literal_cstr(name),
						 quote_literal_cstr(tok_name(ts, i + 2)),
						 missing_ok ? "true" : "false");
	}
	else if (tok_is(ts, i, "unset") && tok_is(ts, i + 1, "allowed_values"))
	{
		rw_whole(rw);
		appendStringInfo(&rw->body,
						 "CALL gp_sql.alter_tag(%s, unset_values => true, missing_ok => %s)",
						 quote_literal_cstr(name), missing_ok ? "true" : "false");
	}
	else if ((tok_is(ts, i, "add") || tok_is(ts, i, "drop")) &&
			 tok_is(ts, i + 1, "allowed_values"))
	{
		bool		adding = tok_is(ts, i, "add");

		rw_whole(rw);
		initStringInfo(&values);
		(void) collect_strings(ts, i + 2, &values);
		appendStringInfo(&rw->body, "CALL gp_sql.alter_tag(%s, %s => %s, missing_ok => %s)",
						 quote_literal_cstr(name),
						 adding ? "add_values" : "drop_values", values.data,
						 missing_ok ? "true" : "false");
	}
	else if (tok_is(ts, i, "owner") && tok_is(ts, i + 1, "to") && tok_is_name(ts, i + 2))
	{
		/* CURRENT_USER and its kind are PostgreSQL's to evaluate */
		const char *owner =
			(tok_is_kw(ts, i + 2, "current_user") ||
			 tok_is_kw(ts, i + 2, "current_role") ||
			 tok_is_kw(ts, i + 2, "session_user"))
			? tok_name(ts, i + 2)
			: quote_literal_cstr(tok_name(ts, i + 2));

		rw_whole(rw);
		appendStringInfo(&rw->body, "CALL gp_sql.alter_tag_owner(%s, %s, missing_ok => %s)",
						 quote_literal_cstr(name), owner,
						 missing_ok ? "true" : "false");
	}
	else
		return false;

	return true;
}

/* DROP TAG [IF EXISTS] a, b -> one CALL, over them all */
static bool
rw_drop_tag(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			i = rw->first;
	bool		missing_ok = false;
	List	   *names = NIL;

	if (!tok_is(ts, i, "drop") || !tok_is(ts, i + 1, "tag"))
		return false;
	i += 2;

	if (tok_is(ts, i, "if") && tok_is(ts, i + 1, "exists"))
	{
		missing_ok = true;
		i += 2;
	}

	collect_names(ts, i, &names);
	if (names == NIL)
		return false;

	rw_whole(rw);
	appendStringInfo(&rw->body, "CALL gp_sql.drop_tag(%s, %s)",
					 name_array(names, "name"), missing_ok ? "true" : "false");
	return true;
}

/*
 * CREATE PROFILE name [LIMIT FAILED_LOGIN_ATTEMPTS n PASSWORD_LOCK_TIME n
 *						PASSWORD_REUSE_MAX n]
 * ALTER PROFILE name LIMIT ...
 *
 * Cloudberry's grammar offers these three of pg_profile's settings.
 */
static bool
rw_profile(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			i = rw->first;
	bool		creating;
	char	   *name;
	StringInfoData args;

	if (tok_is(ts, i, "create"))
		creating = true;
	else if (tok_is(ts, i, "alter"))
		creating = false;
	else
		return false;

	if (!tok_is(ts, i + 1, "profile") || !tok_is_name(ts, i + 2))
		return false;

	name = tok_name(ts, i + 2);
	i += 3;

	initStringInfo(&args);

	if (tok_is(ts, i, "limit"))
	{
		i++;
		while (i < rw->last)
		{
			const char *setting;
			bool		negative = false;

			if (tok_is(ts, i, "failed_login_attempts"))
				setting = "failed_login_attempts";
			else if (tok_is(ts, i, "password_lock_time"))
				setting = "password_lock_time";
			else if (tok_is(ts, i, "password_reuse_max"))
				setting = "password_reuse_max";
			else
				break;

			i++;
			if (tok_is_char(ts, i, '-'))
			{
				negative = true;
				i++;
			}
			if (i >= rw->last || ts->toks[i].code != GP_ICONST)
				return false;

			appendStringInfo(&args, ", %s => %s%d", setting,
							 negative ? "-" : "", ts->toks[i].ival);
			i++;
		}
	}

	rw_whole(rw);
	appendStringInfo(&rw->body, "CALL gp_security.%s_profile(%s%s)",
					 creating ? "create" : "alter",
					 quote_literal_cstr(name), args.data);
	return true;
}

/* DROP PROFILE [IF EXISTS] a, b -> one CALL, over them all */
static bool
rw_drop_profile(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			i = rw->first;
	bool		missing_ok = false;
	List	   *names = NIL;

	if (!tok_is(ts, i, "drop") || !tok_is(ts, i + 1, "profile"))
		return false;
	i += 2;

	if (tok_is(ts, i, "if") && tok_is(ts, i + 1, "exists"))
	{
		missing_ok = true;
		i += 2;
	}

	collect_names(ts, i, &names);
	if (names == NIL)
		return false;

	rw_whole(rw);
	appendStringInfo(&rw->body, "CALL gp_security.drop_profile(%s, %s)",
					 name_array(names, "name"), missing_ok ? "true" : "false");
	return true;
}

/* ------------------------------------------------------------------------- */
/* Resource queues and resource groups, gp_resource's                        */
/* ------------------------------------------------------------------------- */

/*
 * One option of a queue's or a group's, into the text array its procedure is
 * called with: the name alone, or name=k:value, k saying what the value was
 * as Cloudberry's grammar read it -- i an integer, f a number with a fraction
 * or an exponent, s a string, w a word -- which decides what its defGet*()
 * answer.  gp_resource's resdefs.c reads them back.
 */
static void
append_option(StringInfo arr, const char *name, char kind, const char *value)
{
	char	   *item = value != NULL ? psprintf("%s=%c:%s", name, kind, value)
		: pstrdup(name);

	appendStringInfo(arr, "%s%s", arr->len > 0 ? ", " : "",
					 quote_literal_cstr(item));
}

/*
 * The value of one of Cloudberry's def_args at token i: a signed number, a
 * string, or a word -- a type name or a keyword, to its grammar.  Returns
 * the index after it, or -1 where there is none.
 */
static int
option_value(const GpTokens *ts, int i, int limit, char *kind, char **value)
{
	bool		negative = false;

	if (i < limit && (tok_is_char(ts, i, '-') || tok_is_char(ts, i, '+')))
	{
		negative = tok_is_char(ts, i, '-');
		i++;
		if (i >= limit ||
			(ts->toks[i].code != GP_ICONST && ts->toks[i].code != GP_FCONST))
			return -1;
	}
	if (i >= limit)
		return -1;

	switch (ts->toks[i].code)
	{
		case GP_ICONST:
			*kind = 'i';
			*value = psprintf("%s%d", negative ? "-" : "", ts->toks[i].ival);
			return i + 1;
		case GP_FCONST:
			*kind = 'f';
			*value = psprintf("%s%s", negative ? "-" : "", ts->toks[i].str);
			return i + 1;
		case GP_SCONST:
			*kind = 's';
			*value = ts->toks[i].str;
			return i + 1;
		default:
			if (!tok_is_name(ts, i))
				return -1;
			*kind = 'w';
			*value = tok_name(ts, i);
			return i + 1;
	}
}

/*
 * Cloudberry's "definition", ( name [= value] [, ...] ), whose "(" is at
 * `open`, into the array: returns the index after its ")".
 */
static int
definition_list(GpRewrite *rw, int open, StringInfo arr)
{
	const GpTokens *ts = rw->ts;
	int			close;
	int			i = open + 1;

	if (!tok_is_char(ts, open, '('))
		rw_syntax_error(rw, open);
	close = match_close(ts, open, rw->last);
	if (close < 0)
		rw_syntax_error(rw, rw->last);
	if (i == close)
		rw_syntax_error(rw, close);

	while (i < close)
	{
		char	   *name;
		char		kind = 0;
		char	   *value = NULL;

		if (!tok_is_name(ts, i))
			rw_syntax_error(rw, i);
		name = tok_name(ts, i++);
		if (tok_is_char(ts, i, '='))
		{
			int			next = option_value(ts, i + 1, close, &kind, &value);

			if (next < 0)
				rw_syntax_error(rw, i + 1);
			i = next;
		}
		append_option(arr, name, kind, value);

		if (i < close)
		{
			if (!tok_is_char(ts, i, ','))
				rw_syntax_error(rw, i);
			if (++i == close)
				rw_syntax_error(rw, close);
		}
	}
	return close + 1;
}

/*
 * CREATE RESOURCE QUEUE name [ACTIVE THRESHOLD n] [COST THRESHOLD n]
 *		[IGNORE THRESHOLD n] [OVERCOMMIT | NOOVERCOMMIT] ... [WITH (...)]
 * ALTER RESOURCE QUEUE name [those] [WITH (...)] [WITHOUT (...)]
 * DROP RESOURCE QUEUE name
 *	 -> CALL gp_resource.create_resource_queue('name', ARRAY[...]), and
 *		alter_ and drop_
 *
 * The options go in the order Cloudberry's grammar lists them, with the marks
 * it puts where the WITH and WITHOUT lists begin -- "withliststart", always
 * at CREATE, and "withoutliststart" -- which CreateQueue() and AlterQueue()
 * read them by, and whose names a user may therefore not give an option.
 */
/*
 * A U&"..." identifier's name, as parser.c's str_udeescape() makes it: each
 * \XXXX and \+XXXXXX the character it codes, a surrogate pair one, and \\ a
 * backslash; then truncated, as an identifier is (a UESCAPE clause after it
 * is not read).
 */
static char *
udeescape_ident(GpRewrite *rw, int i)
{
	const char *in = rw->ts->toks[i].str;
	StringInfoData buf;
	pg_wchar	pair = 0;

	initStringInfo(&buf);
	while (*in)
	{
		pg_wchar	cp = 0;
		int			ndigits;
		unsigned char utf8[MAX_UNICODE_EQUIVALENT_STRING + 1];

		if (in[0] != '\\')
		{
			appendStringInfoChar(&buf, *in++);
			continue;
		}
		if (in[1] == '\\')
		{
			appendStringInfoChar(&buf, '\\');
			in += 2;
			continue;
		}
		if (in[1] == '+')
		{
			ndigits = 6;
			in += 2;
		}
		else
		{
			ndigits = 4;
			in += 1;
		}
		for (int k = 0; k < ndigits; k++)
		{
			if (!isxdigit((unsigned char) in[k]))
				rw_syntax_error(rw, i);
			cp = (cp << 4) + (isdigit((unsigned char) in[k]) ? in[k] - '0' :
							  pg_ascii_tolower((unsigned char) in[k]) - 'a' + 10);
		}
		in += ndigits;
		if (is_utf16_surrogate_first(cp))
		{
			pair = cp;
			continue;
		}
		if (pair != 0)
		{
			if (!is_utf16_surrogate_second(cp))
				rw_syntax_error(rw, i);
			cp = surrogate_pair_to_codepoint(pair, cp);
			pair = 0;
		}
		memset(utf8, 0, sizeof(utf8));
		pg_unicode_to_server(cp, utf8);
		appendStringInfoString(&buf, (char *) utf8);
	}
	truncate_identifier(buf.data, buf.len, true);
	return buf.data;
}

/*
 * A resource queue's or group's name where Cloudberry's grammar reads a
 * ColId: an identifier, quoted or U&-quoted, or a keyword that is not
 * reserved; anything else a syntax error at it, as Cloudberry's grammar
 * gives ("CREATE RESOURCE GROUP group ..." is one).
 */
static char *
rw_colid(GpRewrite *rw, int i)
{
	const GpTokens *ts = rw->ts;
	int			kwnum;

	if (i >= rw->last)
		rw_syntax_error(rw, i);
	if (ts->toks[i].code == GP_IDENT)
		return ts->toks[i].str;
	if (ts->toks[i].code == GP_UIDENT)
		return udeescape_ident(rw, i);
	if (ts->toks[i].kw != NULL)
	{
		kwnum = ScanKeywordLookup(ts->toks[i].kw, &ScanKeywords);
		if (kwnum >= 0 &&
			(ScanKeywordCategories[kwnum] == UNRESERVED_KEYWORD ||
			 ScanKeywordCategories[kwnum] == COL_NAME_KEYWORD))
			return pstrdup(ts->toks[i].kw);
	}
	rw_syntax_error(rw, i);
	return NULL;				/* not reached */
}

static bool
rw_resource_queue(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			i = rw->first;
	bool		creating = tok_is(ts, i, "create");
	char	   *name;
	StringInfoData arr;

	if (!(creating || tok_is(ts, i, "alter") || tok_is(ts, i, "drop")) ||
		!tok_is(ts, i + 1, "resource") || !tok_is(ts, i + 2, "queue"))
		return false;
	i += 3;
	name = rw_colid(rw, i++);

	if (tok_is(ts, rw->first, "drop"))
	{
		if (i != rw->last)
			rw_syntax_error(rw, i);
		rw_whole(rw);
		appendStringInfo(&rw->body, "CALL gp_resource.drop_resource_queue(%s)",
						 quote_literal_cstr(name));
		return true;
	}

	initStringInfo(&arr);
	for (;;)
	{
		if (tok_is(ts, i, "active") || tok_is(ts, i, "cost") ||
			tok_is(ts, i, "ignore"))
		{
			const char *option = tok_is(ts, i, "active") ? "active_statements" :
				tok_is(ts, i, "cost") ? "max_cost" : "min_cost";
			char		kind = 0;
			char	   *value = NULL;
			int			next;

			if (!tok_is(ts, i + 1, "threshold"))
				rw_syntax_error(rw, i + 1);
			next = option_value(ts, i + 2, rw->last, &kind, &value);
			if (next < 0 || kind == 's' || kind == 'w')
				rw_syntax_error(rw, i + 2);
			append_option(&arr, option, kind, value);
			i = next;
		}
		else if (tok_is(ts, i, "overcommit") || tok_is(ts, i, "noovercommit"))
		{
			append_option(&arr, "cost_overcommit", 'i',
						  tok_is(ts, i, "overcommit") ? "1" : "0");
			i++;
		}
		else
			break;
	}

	if (creating)
	{
		append_option(&arr, "withliststart", 0, NULL);
		if (tok_is(ts, i, "with"))
			i = definition_list(rw, i + 1, &arr);
	}
	else if (tok_is(ts, i, "with") || tok_is(ts, i, "without"))
	{
		append_option(&arr, "withliststart", 0, NULL);
		if (tok_is(ts, i, "with"))
			i = definition_list(rw, i + 1, &arr);
		append_option(&arr, "withoutliststart", 0, NULL);
		if (tok_is(ts, i, "without"))
			i = definition_list(rw, i + 1, &arr);
	}

	if (i != rw->last)
		rw_syntax_error(rw, i);

	rw_whole(rw);
	appendStringInfo(&rw->body, "CALL gp_resource.%s_resource_queue(%s, ARRAY[%s]::text[])",
					 creating ? "create" : "alter", quote_literal_cstr(name),
					 arr.data);
	return true;
}

/*
 * CREATE RESOURCE GROUP name WITH (...)
 * ALTER RESOURCE GROUP name SET option value
 * DROP RESOURCE GROUP name
 *	 -> CALL gp_resource.create_resource_group('name', ARRAY[...]), and
 *		alter_ and drop_
 *
 * ALTER takes one option, whose value is a signed integer but for CPUSET's
 * and IO_LIMIT's, strings, as Cloudberry's grammar has it; anything else is
 * its syntax error.
 */
static bool
rw_resource_group(GpRewrite *rw)
{
	static const char *const int_options[] = {
		"concurrency", "cpu_max_percent", "cpu_weight", "memory_quota",
		"min_cost", NULL
	};
	const GpTokens *ts = rw->ts;
	int			i = rw->first;
	const char *verb;
	char	   *name;
	StringInfoData arr;

	if (tok_is(ts, i, "create"))
		verb = "create";
	else if (tok_is(ts, i, "alter"))
		verb = "alter";
	else if (tok_is(ts, i, "drop"))
		verb = "drop";
	else
		return false;
	if (!tok_is(ts, i + 1, "resource") || !tok_is(ts, i + 2, "group"))
		return false;
	i += 3;
	name = rw_colid(rw, i++);

	initStringInfo(&arr);
	if (strcmp(verb, "create") == 0)
	{
		if (!tok_is(ts, i, "with"))
			rw_syntax_error(rw, i);
		i = definition_list(rw, i + 1, &arr);
	}
	else if (strcmp(verb, "alter") == 0)
	{
		const char *option = NULL;

		if (!tok_is(ts, i, "set"))
			rw_syntax_error(rw, i);
		i++;
		for (int k = 0; int_options[k] != NULL; k++)
		{
			if (tok_is(ts, i, int_options[k]))
				option = int_options[k];
		}
		if (option != NULL)
		{
			bool		negative = tok_is_char(ts, i + 1, '-');
			int			v = i + 1 + (negative ? 1 : 0);

			if (v >= rw->last || ts->toks[v].code != GP_ICONST)
				rw_syntax_error(rw, v);
			append_option(&arr, option, 'i',
						  psprintf("%s%d", negative ? "-" : "", ts->toks[v].ival));
			i = v + 1;
		}
		else if (tok_is(ts, i, "cpuset") || tok_is(ts, i, "io_limit"))
		{
			if (!tok_is_string(ts, i + 1))
				rw_syntax_error(rw, i + 1);
			append_option(&arr, tok_is(ts, i, "cpuset") ? "cpuset" : "io_limit",
						  's', ts->toks[i + 1].str);
			i += 2;
		}
		else
			rw_syntax_error(rw, i);
	}

	if (i != rw->last)
		rw_syntax_error(rw, i);

	rw_whole(rw);
	if (strcmp(verb, "drop") == 0)
		appendStringInfo(&rw->body, "CALL gp_resource.drop_resource_group(%s)",
						 quote_literal_cstr(name));
	else
		appendStringInfo(&rw->body, "CALL gp_resource.%s_resource_group(%s, ARRAY[%s]::text[])",
						 verb, quote_literal_cstr(name), arr.data);
	return true;
}

/*
 * COMMENT ON RESOURCE QUEUE name IS 'text' | NULL, and RESOURCE GROUP's
 *	 -> CALL gp_resource.comment_on_resource_queue('name', 'text')
 *
 * A comment is the definition's own: pg_shdescription has no class for it.
 */
static bool
rw_comment_resource(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			i = rw->first;
	const char *kind;
	const char *comment = NULL;
	char	   *name;

	if (!tok_is(ts, i, "comment") || !tok_is(ts, i + 1, "on") ||
		!tok_is(ts, i + 2, "resource"))
		return false;
	if (tok_is(ts, i + 3, "queue"))
		kind = "queue";
	else if (tok_is(ts, i + 3, "group"))
		kind = "group";
	else
		return false;
	name = rw_colid(rw, i + 4);
	if (!tok_is(ts, i + 5, "is"))
		rw_syntax_error(rw, i + 5);
	if (tok_is_string(ts, i + 6))
		comment = quote_literal_cstr(ts->toks[i + 6].str);
	else if (tok_is(ts, i + 6, "null"))
		comment = "NULL";
	else
		rw_syntax_error(rw, i + 6);
	if (i + 7 != rw->last)
		rw_syntax_error(rw, i + 7);

	rw_whole(rw);
	appendStringInfo(&rw->body, "CALL gp_resource.comment_on_resource_%s(%s, %s)",
					 kind, quote_literal_cstr(name), comment);
	return true;
}

/*
 * CREATE TASK [IF NOT EXISTS] name SCHEDULE 's' [DATABASE d] [USER u] AS 'cmd'
 * ALTER TASK [IF EXISTS] name [SCHEDULE 's'] [DATABASE d] [USER u]
 *			  [ACTIVE {TRUE|FALSE}] [AS 'cmd']
 */
static bool
rw_task(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			i = rw->first;
	bool		creating;
	bool		if_exists_clause = false;	/* IF NOT EXISTS or IF EXISTS */
	char	   *name;
	char	   *schedule = NULL;
	char	   *database = NULL;
	char	   *username = NULL;
	char	   *command = NULL;
	const char *active = NULL;

	if (tok_is(ts, i, "create"))
		creating = true;
	else if (tok_is(ts, i, "alter"))
		creating = false;
	else
		return false;

	if (!tok_is(ts, i + 1, "task"))
		return false;
	i += 2;

	if (creating && tok_is(ts, i, "if") && tok_is(ts, i + 1, "not") &&
		tok_is(ts, i + 2, "exists"))
	{
		if_exists_clause = true;
		i += 3;
	}
	else if (!creating && tok_is(ts, i, "if") && tok_is(ts, i + 1, "exists"))
	{
		if_exists_clause = true;
		i += 2;
	}

	if (!tok_is_name(ts, i))
		return false;
	name = tok_name(ts, i);
	i++;

	while (i < rw->last)
	{
		if (tok_is(ts, i, "schedule") && tok_is_string(ts, i + 1))
		{
			schedule = ts->toks[i + 1].str;
			i += 2;
		}
		else if (tok_is(ts, i, "database") && tok_is_name(ts, i + 1))
		{
			database = tok_name(ts, i + 1);
			i += 2;
		}
		else if (tok_is(ts, i, "user") && tok_is_name(ts, i + 1))
		{
			username = tok_name(ts, i + 1);
			i += 2;
		}
		else if (tok_is(ts, i, "active") && tok_is_name(ts, i + 1))
		{
			active = tok_is(ts, i + 1, "true") ? "true" : "false";
			i += 2;
		}
		else if (tok_is(ts, i, "as") && tok_is_string(ts, i + 1))
		{
			command = ts->toks[i + 1].str;
			i += 2;
		}
		else
			break;
	}

	if (creating && (schedule == NULL || command == NULL))
		return false;

	rw_whole(rw);

	if (creating)
	{
		appendStringInfo(&rw->body, "CALL gp_task.create_task(%s, %s, %s",
						 quote_literal_cstr(name),
						 quote_literal_cstr(schedule),
						 quote_literal_cstr(command));
		if (database != NULL)
			appendStringInfo(&rw->body, ", database => %s",
							 quote_literal_cstr(database));
		if (username != NULL)
			appendStringInfo(&rw->body, ", username => %s",
							 quote_literal_cstr(username));
		if (if_exists_clause)
			appendStringInfoString(&rw->body, ", if_not_exists => true");
		appendStringInfoChar(&rw->body, ')');
	}
	else
	{
		appendStringInfo(&rw->body, "CALL gp_task.alter_task(%s",
						 quote_literal_cstr(name));
		if (schedule != NULL)
			appendStringInfo(&rw->body, ", schedule => %s",
							 quote_literal_cstr(schedule));
		if (command != NULL)
			appendStringInfo(&rw->body, ", command => %s",
							 quote_literal_cstr(command));
		if (database != NULL)
			appendStringInfo(&rw->body, ", database => %s",
							 quote_literal_cstr(database));
		if (username != NULL)
			appendStringInfo(&rw->body, ", username => %s",
							 quote_literal_cstr(username));
		if (active != NULL)
			appendStringInfo(&rw->body, ", active => %s", active);
		if (if_exists_clause)
			appendStringInfoString(&rw->body, ", missing_ok => true");
		appendStringInfoChar(&rw->body, ')');
	}

	return true;
}

/* DROP TASK [IF EXISTS] a, b -> one CALL, over them all */
static bool
rw_drop_task(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			i = rw->first;
	bool		missing_ok = false;
	List	   *names = NIL;

	if (!tok_is(ts, i, "drop") || !tok_is(ts, i + 1, "task"))
		return false;
	i += 2;

	if (tok_is(ts, i, "if") && tok_is(ts, i + 1, "exists"))
	{
		missing_ok = true;
		i += 2;
	}

	collect_names(ts, i, &names);
	if (names == NIL)
		return false;

	rw_whole(rw);
	appendStringInfo(&rw->body, "CALL gp_task.drop_task(%s, %s)",
					 name_array(names, "text"), missing_ok ? "true" : "false");
	return true;
}

/*
 * CREATE DIRECTORY TABLE [IF NOT EXISTS] name [USING am] [TABLESPACE ts]
 *		[TAG (...)]
 *	 -> CREATE TABLE [IF NOT EXISTS] name (relative_path text PRIMARY KEY, ...)
 *		[USING am] WITH (gp.directory_table = true[, gp_tag....]) [TABLESPACE ts]
 *
 * A directory table here is an ordinary table of Cloudberry's five columns
 * whose "gp" label says where its files are, so the statement is the CREATE
 * TABLE that makes one, and gp_sql's ProcessUtility hook takes the option out
 * and claims the table once it exists.  It used to be a call of
 * gp_sql.create_directory_table(), which answered with a row and dropped a
 * TAG clause without a word.  The columns are Cloudberry's
 * GetDirectoryTableSchema, in its order, which that function also uses.
 *
 * WITH LOCATION names where Cloudberry keeps the files; the port chooses that
 * itself, a directory per table in the database's directory, so the clause is
 * refused rather than dropped.
 *
 * Returns false when this is not a CREATE DIRECTORY TABLE; otherwise the
 * subject, *name and *after, is the table, as find_subject gives it, so that
 * the TAG clause is found as it is on any CREATE TABLE.
 */
static bool
rw_create_directory_table(GpRewrite *rw, char **name, int *after)
{
	const GpTokens *ts = rw->ts;
	int			i = rw->first;
	int			nameend;

	if (!tok_is(ts, i, "create") || !tok_is(ts, i + 1, "directory") ||
		!tok_is(ts, i + 2, "table"))
		return false;
	i += 3;

	if (tok_is(ts, i, "if") && tok_is(ts, i + 1, "not") && tok_is(ts, i + 2, "exists"))
		i += 3;

	nameend = skip_qualified_name(ts, i);
	if (nameend == i)
		return false;

	for (int j = nameend; j < rw->last; j++)
	{
		if (tok_is_kw(ts, j, "with") && tok_is(ts, j + 1, "location"))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("WITH LOCATION is not supported for a directory table"),
					 errdetail("A directory table keeps its files in a directory of its own in the database's directory.")));
	}

	/* DIRECTORY is Cloudberry's; the rest is CREATE TABLE's already. */
	rw_edit(rw, ts->toks[rw->first + 1].off, ts->toks[rw->first + 2].off, "");
	rw_edit(rw, tok_stop(ts, nameend - 1), tok_stop(ts, nameend - 1),
			" (relative_path text PRIMARY KEY, size bigint,"
			" last_modified timestamptz, md5 text, tag text)");
	rw_add_option(rw, "gp.directory_table = true", ts->toks[rw->first + 1].off);

	rw->object = 't';
	*name = rw_text(ts, i, nameend);
	*after = nameend;
	return true;
}

/* ------------------------------------------------------------------------- */
/* External tables                                                           */
/* ------------------------------------------------------------------------- */

/*
 * A list of names, comma-separated, from token i -- a FORMAT option's
 * columns -- as String nodes; *after is the token after it.
 */
static List *
ext_name_list(const GpTokens *ts, int i, int *after)
{
	List	   *names = NIL;

	for (;;)
	{
		if (!tok_is_name(ts, i))
			ts_syntax_error(ts, i);
		names = lappend(names, makeString(tok_name(ts, i)));
		i++;
		if (!tok_is_char(ts, i, ','))
			break;
		i++;
	}
	*after = i;
	return names;
}

/* A string after an optional AS, from token i, for a FORMAT option. */
static Node *
ext_as_string(const GpTokens *ts, int *i)
{
	if (tok_is_kw(ts, *i, "as"))
		(*i)++;
	if (!tok_is_string(ts, *i))
		ts_syntax_error(ts, *i);
	return (Node *) makeString(pstrdup(ts->toks[(*i)++].str));
}

/*
 * FORMAT '...' (...), the options between the brackets at i and close:
 * Cloudberry's words for COPY's options, DELIMITER 'x' NULL 'y' CSV HEADER
 * and the rest (gram.y, format_opt_item), or name = value, comma-separated,
 * which a custom format's formatter takes (format_def_item), each as the
 * DefElem its grammar makes.
 */
static List *
ext_format_options(const GpTokens *ts, int i, int close)
{
	List	   *opts = NIL;

	if (i == close)
		return NIL;

	if (tok_is_name(ts, i) && tok_is_char(ts, i + 1, '='))
	{
		for (;;)
		{
			char	   *label;
			Node	   *arg;

			if (!tok_is_name(ts, i) || !tok_is_char(ts, i + 1, '='))
				ts_syntax_error(ts, i);
			label = tok_name(ts, i);
			i += 2;
			if (tok_is_char(ts, i, '('))
			{
				arg = (Node *) ext_name_list(ts, i + 1, &i);
				if (!tok_is_char(ts, i, ')'))
					ts_syntax_error(ts, i);
				i++;
			}
			else if (tok_is_string(ts, i) || ts->toks[i].code == GP_FCONST)
				arg = (Node *) makeString(pstrdup(ts->toks[i++].str));
			else if (ts->toks[i].code == GP_ICONST)
				arg = (Node *) makeString(psprintf("%d", ts->toks[i++].ival));
			else if (tok_is_name(ts, i))
			{
				StringInfoData name;

				/* a function's name, qualified or not, or a word */
				initStringInfo(&name);
				appendStringInfoString(&name, tok_name(ts, i++));
				while (tok_is_char(ts, i, '.') && tok_is_name(ts, i + 1))
				{
					appendStringInfo(&name, ".%s", tok_name(ts, i + 1));
					i += 2;
				}
				arg = (Node *) makeString(name.data);
			}
			else
				ts_syntax_error(ts, i);
			opts = lappend(opts, makeDefElem(label, arg, -1));
			if (i == close)
				break;
			if (!tok_is_char(ts, i, ','))
				ts_syntax_error(ts, i);
			i++;
		}
		return opts;
	}

	while (i < close)
	{
		DefElem    *d;

		if (tok_is(ts, i, "delimiter"))
		{
			i++;
			d = makeDefElem("delimiter", ext_as_string(ts, &i), -1);
		}
		else if (tok_is_kw(ts, i, "null"))
		{
			i++;
			d = makeDefElem("null", ext_as_string(ts, &i), -1);
		}
		else if (tok_is(ts, i, "csv"))
		{
			i++;
			d = makeDefElem("csv", (Node *) makeInteger(true), -1);
		}
		else if (tok_is(ts, i, "header"))
		{
			i++;
			d = makeDefElem("header", (Node *) makeInteger(true), -1);
		}
		else if (tok_is(ts, i, "quote"))
		{
			i++;
			d = makeDefElem("quote", ext_as_string(ts, &i), -1);
		}
		else if (tok_is(ts, i, "escape"))
		{
			i++;
			d = makeDefElem("escape", ext_as_string(ts, &i), -1);
		}
		else if (tok_is(ts, i, "force") && tok_is_kw(ts, i + 1, "not") &&
				 tok_is_kw(ts, i + 2, "null"))
			d = makeDefElem("force_not_null",
							(Node *) ext_name_list(ts, i + 3, &i), -1);
		else if (tok_is(ts, i, "force") && tok_is(ts, i + 1, "quote") &&
				 tok_is_char(ts, i + 2, '*'))
		{
			i += 3;
			d = makeDefElem("force_quote", (Node *) makeNode(A_Star), -1);
		}
		else if (tok_is(ts, i, "force") && tok_is(ts, i + 1, "quote"))
			d = makeDefElem("force_quote",
							(Node *) ext_name_list(ts, i + 2, &i), -1);
		else if (tok_is(ts, i, "fill") && tok_is(ts, i + 1, "missing") &&
				 tok_is(ts, i + 2, "fields"))
		{
			i += 3;
			d = makeDefElem("fill_missing_fields", (Node *) makeInteger(true), -1);
		}
		else if (tok_is(ts, i, "newline"))
		{
			i++;
			d = makeDefElem("newline", ext_as_string(ts, &i), -1);
		}
		else
			ts_syntax_error(ts, i);
		opts = lappend(opts, d);
	}
	if (i != close)
		ts_syntax_error(ts, i);
	return opts;
}

/*
 * [LOG ERRORS [INTO t | PERSISTENTLY]] SEGMENT REJECT LIMIT n [ROWS | PERCENT]
 * at token i, as Cloudberry's grammar reads it for an external table
 * (ExtSingleRowErrorHandling) and, without PERSISTENTLY, for COPY
 * (OptSingleRowErrorHandling), with its errors: false where there is none.
 * *after is the token after it; *log_errors "t", "p" or "f".
 *
 * LOG ERRORS INTO t is refused, as Cloudberry refuses an error table,
 * unless gp.ignore_error_table (gp_exttable's) says to ignore the INTO.
 */
static bool
sreh_clause(GpRewrite *rw, int i, bool persistently_ok, int *after, int *limit,
			bool *rows, const char **log_errors)
{
	const GpTokens *ts = rw->ts;
	int			j = i;

	*log_errors = "f";
	*rows = true;
	if (tok_is(ts, j, "log") && tok_is(ts, j + 1, "errors"))
	{
		*log_errors = "t";
		j += 2;
		if (persistently_ok && tok_is(ts, j, "persistently"))
		{
			*log_errors = "p";
			j++;
		}
		else if (tok_is_kw(ts, j, "into"))
		{
			int			e = skip_qualified_name(ts, j + 1);
			const char *ignore = GetConfigOption("gp.ignore_error_table", true, false);
			bool		ignored = false;

			if (e == j + 1)
				rw_syntax_error(rw, j + 1);
			if (ignore == NULL || !parse_bool(ignore, &ignored) || !ignored)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("error table is not supported"),
						 errhint("Set gp.ignore_error_table to ignore the [INTO error-table] clause for backward compatibility."),
						 errposition(pg_mbstrlen_with_len(ts->src, ts->toks[j].off) + 1)));
			ereport(WARNING,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("error table is not supported"),
					 errhint("Use gp_read_error_log() and gp_truncate_error_log() to view and manage the internal error log associated with your table.")));
			j = e;
		}
		if (!tok_is(ts, j, "segment"))
			rw_syntax_error(rw, j);
	}
	if (!tok_is(ts, j, "segment") || !tok_is(ts, j + 1, "reject") ||
		!tok_is_kw(ts, j + 2, "limit"))
		return false;
	if (j + 3 >= ts->ntoks || ts->toks[j + 3].code != GP_ICONST)
		rw_syntax_error(rw, j + 3);
	*limit = ts->toks[j + 3].ival;
	j += 4;
	if (tok_is(ts, j, "rows"))
		j++;
	else if (tok_is(ts, j, "percent"))
	{
		*rows = false;
		j++;
	}
	if (!*rows && (*limit < 1 || *limit > 100))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid PERCENT value. Should be (1 - 100)")));
	if (*rows && *limit < 2)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid (ROWS) reject limit. Should be 2 or larger")));
	*after = j;
	return true;
}

/*
 * CREATE [READABLE | WRITABLE] EXTERNAL [WEB] TABLE t (...)
 *	   LOCATION ('...', ...) [ON ...] | EXECUTE '...' [ON ...]
 *	   FORMAT '...' [(...)] [OPTIONS (...)] [ENCODING ...]
 *	   [[LOG ERRORS [PERSISTENTLY]] SEGMENT REJECT LIMIT n [ROWS | PERCENT]]
 *	   [DISTRIBUTED ...] [TAG (...)]
 *	 -> CREATE FOREIGN TABLE t (...) SERVER gp_exttable_server
 *		OPTIONS ("gp_exttable.spec" '...')
 *
 * Cloudberry's external table is a foreign table of gp_exttable_fdw's server,
 * as its own CREATE EXTERNAL TABLE makes it (exttablecmds.c).  What the
 * clauses say is checked and made the table's options by gp_exttable
 * (exttable_ddl.c), which needs the catalogs -- the columns LIKE names, the
 * database's encoding -- so here they are only read, as Cloudberry's grammar
 * reads them (gram.y, CreateExternalStmt), with its errors, and carried in
 * one option: the DefElem nodes its grammar makes, written out as nodes are.
 * DISTRIBUTED and TAG are left to the rewrites every foreign table has,
 * which put what they become in the same list.
 *
 * Returns false when this is not a CREATE EXTERNAL TABLE; otherwise the
 * subject, *name and *after, is the table, and *after the token after its
 * clauses, where DISTRIBUTED or TAG may follow.
 */
static bool
rw_create_external_table(GpRewrite *rw, char **name, int *after)
{
	const GpTokens *ts = rw->ts;
	int			i = rw->first + 1;
	int			nameend;
	int			close;
	int			start;
	int			j;
	int			stop;
	bool		writable = false;
	bool		web = false;
	bool		command = false;
	List	   *spec = NIL;
	List	   *on = NIL;

	if (!tok_is(ts, rw->first, "create"))
		return false;
	if (tok_is(ts, i, "readable") || tok_is(ts, i, "writable"))
		writable = tok_is(ts, i++, "writable");
	if (!tok_is(ts, i, "external"))
		return false;
	i++;
	if (tok_is(ts, i, "web"))
	{
		web = true;
		i++;
	}
	if (tok_is(ts, i, "temp") || tok_is(ts, i, "temporary"))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a temporary external table is not supported"),
				 errdetail("An external table is a foreign table, which PostgreSQL makes in no temporary schema."),
				 errposition(pg_mbstrlen_with_len(ts->src, ts->toks[i].off) + 1)));
	if (!tok_is(ts, i, "table"))
		return false;
	i++;

	nameend = skip_qualified_name(ts, i);
	if (nameend == i)
		rw_syntax_error(rw, i);
	if (!tok_is_char(ts, nameend, '('))
		rw_syntax_error(rw, nameend);
	close = match_close(ts, nameend, rw->last);
	if (close < 0)
		rw_syntax_error(rw, rw->last);
	start = j = close + 1;

	spec = lappend(spec, makeDefElem("writable", (Node *) makeBoolean(writable), -1));
	spec = lappend(spec, makeDefElem("web", (Node *) makeBoolean(web), -1));

	/* LOCATION ('uri', ...) or EXECUTE 'command' */
	if (tok_is(ts, j, "location"))
	{
		List	   *locs = NIL;

		if (!tok_is_char(ts, j + 1, '('))
			rw_syntax_error(rw, j + 1);
		j += 2;
		for (;;)
		{
			if (!tok_is_string(ts, j))
				rw_syntax_error(rw, j);
			/* Cloudberry's grammar refuses one twice (cdb_string_list) */
			foreach_node(String, loc, locs)
				if (strcmp(strVal(loc), ts->toks[j].str) == 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("duplicate location uri"),
							 errposition(pg_mbstrlen_with_len(ts->src, ts->toks[j].off) + 1)));
			locs = lappend(locs, makeString(pstrdup(ts->toks[j++].str)));
			if (tok_is_char(ts, j, ')'))
				break;
			if (!tok_is_char(ts, j, ','))
				rw_syntax_error(rw, j);
			j++;
		}
		j++;
		spec = lappend(spec, makeDefElem("location", (Node *) locs, -1));
	}
	else if (tok_is_kw(ts, j, "execute"))
	{
		if (!tok_is_string(ts, j + 1))
			rw_syntax_error(rw, j + 1);
		spec = lappend(spec, makeDefElem("command",
										 (Node *) makeString(pstrdup(ts->toks[j + 1].str)),
										 -1));
		command = true;
		j += 2;
	}
	else
		rw_syntax_error(rw, j);

	/* ON ALL | HOST ['h'] | MASTER | COORDINATOR | SEGMENT n | n, each */
	while (tok_is_kw(ts, j, "on"))
	{
		DefElem    *d;

		if (tok_is_kw(ts, j + 1, "all"))
		{
			d = makeDefElem("all", (Node *) makeInteger(true), -1);
			j += 2;
		}
		else if (tok_is(ts, j + 1, "host") && tok_is_string(ts, j + 2))
		{
			d = makeDefElem("hostname",
							(Node *) makeString(pstrdup(ts->toks[j + 2].str)), -1);
			j += 3;
		}
		else if (tok_is(ts, j + 1, "host"))
		{
			d = makeDefElem("eachhost", (Node *) makeInteger(true), -1);
			j += 2;
		}
		else if (tok_is(ts, j + 1, "master") || tok_is(ts, j + 1, "coordinator"))
		{
			d = makeDefElem("coordinator", (Node *) makeInteger(true), -1);
			j += 2;
		}
		else if (tok_is(ts, j + 1, "segment") && j + 2 < ts->ntoks &&
				 ts->toks[j + 2].code == GP_ICONST)
		{
			d = makeDefElem("segment", (Node *) makeInteger(ts->toks[j + 2].ival), -1);
			j += 3;
		}
		else if (j + 1 < ts->ntoks && ts->toks[j + 1].code == GP_ICONST)
		{
			d = makeDefElem("random", (Node *) makeInteger(ts->toks[j + 1].ival), -1);
			j += 2;
		}
		else
			rw_syntax_error(rw, j + 1);
		on = lappend(on, d);
	}

	/* Cloudberry's grammar's own checks of EXECUTE */
	if (command)
	{
		if (!web)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("EXECUTE may not be used with a regular external table"),
					 errhint("Use CREATE EXTERNAL WEB TABLE instead.")));
		if (on != NIL && writable)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("ON clause may not be used with a writable external table")));
		/* ON ALL, as the grammar gives it, where a readable one has none */
		if (on == NIL && !writable)
			on = list_make1(makeDefElem("all", (Node *) makeInteger(true), -1));
	}
	spec = lappend(spec, makeDefElem("on", (Node *) on, -1));

	/* FORMAT 'name' [(...)] */
	if (!tok_is(ts, j, "format"))
		rw_syntax_error(rw, j);
	if (!tok_is_string(ts, j + 1))
		rw_syntax_error(rw, j + 1);
	spec = lappend(spec, makeDefElem("format",
									 (Node *) makeString(pstrdup(ts->toks[j + 1].str)),
									 -1));
	j += 2;
	if (tok_is_char(ts, j, '('))
	{
		int			fclose = match_close(ts, j, rw->last);

		if (fclose < 0)
			rw_syntax_error(rw, rw->last);
		spec = lappend(spec, makeDefElem("format_opts",
										 (Node *) ext_format_options(ts, j + 1, fclose),
										 -1));
		j = fclose + 1;
	}

	/* OPTIONS (name 'value', ...) */
	if (tok_is(ts, j, "options") && tok_is_char(ts, j + 1, '('))
	{
		List	   *opts = NIL;

		j += 2;
		while (!tok_is_char(ts, j, ')'))
		{
			if (!tok_is_name(ts, j))
				rw_syntax_error(rw, j);
			if (!tok_is_string(ts, j + 1))
				rw_syntax_error(rw, j + 1);
			opts = lappend(opts, makeDefElem(tok_name(ts, j),
											 (Node *) makeString(pstrdup(ts->toks[j + 1].str)),
											 -1));
			j += 2;
			if (tok_is_char(ts, j, ','))
				j++;
			else if (!tok_is_char(ts, j, ')'))
				rw_syntax_error(rw, j);
		}
		j++;
		spec = lappend(spec, makeDefElem("options", (Node *) opts, -1));
	}

	/* ENCODING [=] 'name' | number, the last of them */
	while (tok_is_kw(ts, j, "encoding"))
	{
		Node	   *enc;

		j++;
		if (tok_is_char(ts, j, '='))
			j++;
		if (tok_is_string(ts, j))
			enc = (Node *) makeString(pstrdup(ts->toks[j].str));
		else if (j < ts->ntoks && ts->toks[j].code == GP_ICONST)
			enc = (Node *) makeInteger(ts->toks[j].ival);
		else
			rw_syntax_error(rw, j);
		j++;
		spec = lappend(spec, makeDefElem("encoding", enc, -1));
	}

	/* [LOG ERRORS [INTO t | PERSISTENTLY]] SEGMENT REJECT LIMIT n [ROWS | PERCENT] */
	{
		const char *log_errors;
		int			limit;
		bool		rows;

		if (sreh_clause(rw, j, true, &j, &limit, &rows, &log_errors))
		{
			if (writable)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("single row error handling may not be used with a writable external table")));
			spec = lappend(spec, makeDefElem("reject_limit", (Node *) makeInteger(limit), -1));
			spec = lappend(spec, makeDefElem("reject_rows", (Node *) makeBoolean(rows), -1));
			spec = lappend(spec, makeDefElem("log_errors",
											 (Node *) makeString(pstrdup(log_errors)), -1));
		}
	}

	/* what may follow: DISTRIBUTED, TAG, or nothing */
	if (j < rw->last && !tok_is(ts, j, "distributed") && !tok_is(ts, j, "tag"))
		rw_syntax_error(rw, j);

	/* CREATE FOREIGN TABLE t (...) SERVER gp_exttable_server OPTIONS (...) */
	stop = tok_stop(ts, j - 1);
	rw_edit(rw, ts->toks[rw->first].off, ts->toks[i].off, "CREATE FOREIGN TABLE ");
	rw_edit(rw, ts->toks[start].off, stop, "SERVER gp_exttable_server");
	rw_add_fdw_option(rw, "gp_exttable.spec", nodeToString(spec));
	rw->fdw_options_at = stop;

	rw->object = 'f';
	*name = rw_text(ts, i, nameend);
	*after = j;
	return true;
}

/*
 * COPY ... [LOG ERRORS [INTO t]] SEGMENT REJECT LIMIT n [ROWS | PERCENT]
 *	 -> COPY ..., carrying gp_exttable.reject_limit, reject_limit_type and
 *		log_errors to its parse node
 * COPY ... FILL MISSING FIELDS
 *	 -> COPY ..., carrying gp_exttable.fill_missing_fields
 * COPY ... NEWLINE [AS] 'lf'
 *	 -> COPY ..., carrying gp_exttable.newline
 *
 * Cloudberry's options of COPY FROM that PostgreSQL's COPY has not: gp_core's
 * COPY takes them out again and reads the data through gp_exttable's filter
 * (gp_modify.c, copysreh.c), which takes the last two as options of their
 * own names too, as a COPY's option list in brackets gives them to
 * PostgreSQL's grammar.  Each clause wherever it is among the options, and
 * each of them in one statement.
 */
static bool
rw_copy_options(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			depth = 0;
	bool		found = false;

	if (!tok_is_kw(ts, rw->first, "copy"))
		return false;
	for (int j = rw->first + 1; j < rw->last; j++)
	{
		int			after;
		int			limit;
		bool		rows;
		const char *log_errors;
		int			at = ts->toks[j].off;

		if (tok_is_char(ts, j, '('))
			depth++;
		else if (tok_is_char(ts, j, ')'))
			depth--;
		if (depth != 0)
			continue;

		if (tok_is(ts, j, "fill") && tok_is(ts, j + 1, "missing") &&
			tok_is(ts, j + 2, "fields"))
		{
			rw_edit(rw, at, tok_stop(ts, j + 2), "");
			rw_add_carrier(rw, "gp_exttable", "fill_missing_fields", "true", at);
			j += 2;
			found = true;
		}
		else if (tok_is(ts, j, "newline") &&
				 (tok_is_string(ts, j + 1) ||
				  (tok_is_kw(ts, j + 1, "as") && tok_is_string(ts, j + 2))))
		{
			int			str = tok_is_kw(ts, j + 1, "as") ? j + 2 : j + 1;

			rw_edit(rw, at, tok_stop(ts, str), "");
			rw_add_carrier(rw, "gp_exttable", "newline", ts->toks[str].str, at);
			j = str;
			found = true;
		}
		else if (((tok_is(ts, j, "log") && tok_is(ts, j + 1, "errors")) ||
				  (tok_is(ts, j, "segment") && tok_is(ts, j + 1, "reject"))) &&
				 sreh_clause(rw, j, false, &after, &limit, &rows, &log_errors))
		{
			rw_edit(rw, at, tok_stop(ts, after - 1), "");
			rw_add_carrier(rw, "gp_exttable", "reject_limit", psprintf("%d", limit), at);
			rw_add_carrier(rw, "gp_exttable", "reject_limit_type", rows ? "r" : "p", at);
			rw_add_carrier(rw, "gp_exttable", "log_errors", log_errors, at);
			j = after - 1;
			found = true;
		}
	}
	return found;
}

/*
 * DROP EXTERNAL [WEB] TABLE	-> DROP FOREIGN TABLE
 * ALTER EXTERNAL TABLE			-> ALTER FOREIGN TABLE
 */
static bool
rw_external_spelling(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			i = rw->first;

	if (tok_is(ts, i, "drop") && tok_is(ts, i + 1, "external") &&
		tok_is(ts, i + 2, "web") && tok_is(ts, i + 3, "table"))
	{
		rw_edit(rw, ts->toks[i + 1].off, ts->toks[i + 3].off, "FOREIGN ");
		return true;
	}
	if ((tok_is(ts, i, "drop") || tok_is(ts, i, "alter")) &&
		tok_is(ts, i + 1, "external") && tok_is(ts, i + 2, "table"))
	{
		rw_edit(rw, ts->toks[i + 1].off, ts->toks[i + 2].off, "FOREIGN ");
		return true;
	}
	return false;
}

/*
 * The statements of Cloudberry's protocols, each a CALL of gp_exttable's
 * procedure (protocol.c):
 *
 *	 CREATE [TRUSTED] PROTOCOL name (readfunc = 'f', writefunc = 'g',
 *									 validatorfunc = 'h')
 *	   -> CALL gp_exttable.create_protocol('name', trusted,
 *										   ARRAY['readfunc', 'f', ...])
 *	 DROP PROTOCOL [IF EXISTS] name, ... [CASCADE | RESTRICT]
 *	   -> CALL gp_exttable.drop_protocol(ARRAY[...], if_exists, cascade)
 *	 ALTER PROTOCOL name RENAME TO newname
 *	   -> CALL gp_exttable.rename_protocol('name', 'newname')
 *	 ALTER PROTOCOL name OWNER TO role
 *	   -> CALL gp_exttable.alter_protocol_owner('name', 'role')
 *	 GRANT privileges ON PROTOCOL name, ... TO role, ... [WITH GRANT OPTION]
 *	 REVOKE [GRANT OPTION FOR] privileges ON PROTOCOL name, ... FROM role, ...
 *		[CASCADE | RESTRICT]
 *	   -> CALL gp_exttable.grant_protocol(is_grant, ARRAY[privileges],
 *										  ARRAY[names], ARRAY[roles],
 *										  grant_option, cascade)
 *
 * A form none of these is left to the grammar, which refuses it.
 */

/* A role as GRANT names it -- PUBLIC, CURRENT_USER, a name -- at i. */
static char *
protocol_role(const GpTokens *ts, int *i)
{
	if (tok_is(ts, *i, "group") && tok_is_name(ts, *i + 1))
		(*i)++;
	if (!tok_is_name(ts, *i))
		return NULL;
	return tok_name(ts, (*i)++);
}

/* A comma-separated list of names from i, into *names; the token after it. */
static int
protocol_names(const GpTokens *ts, int i, List **names)
{
	for (;;)
	{
		if (!tok_is_name(ts, i))
			return -1;
		*names = lappend(*names, tok_name(ts, i++));
		if (!tok_is_char(ts, i, ','))
			return i;
		i++;
	}
}

static bool
rw_protocol(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			i = rw->first;
	List	   *names = NIL;

	/* CREATE [TRUSTED] PROTOCOL name (...) */
	if (tok_is(ts, i, "create") &&
		(tok_is(ts, i + 1, "protocol") ||
		 (tok_is(ts, i + 1, "trusted") && tok_is(ts, i + 2, "protocol"))))
	{
		bool		trusted = tok_is(ts, i + 1, "trusted");
		char	   *name;
		List	   *defs = NIL;

		i += trusted ? 3 : 2;
		if (!tok_is_name(ts, i) || !tok_is_char(ts, i + 1, '('))
			return false;
		name = tok_name(ts, i);
		i += 2;
		while (!tok_is_char(ts, i, ')'))
		{
			StringInfoData value;

			if (!tok_is_name(ts, i))
				return false;
			defs = lappend(defs, tok_name(ts, i++));
			initStringInfo(&value);
			if (tok_is_char(ts, i, '='))
			{
				i++;
				if (tok_is_string(ts, i))
					appendStringInfoString(&value, ts->toks[i++].str);
				else if (tok_is_name(ts, i))
				{
					appendStringInfoString(&value, quote_identifier(tok_name(ts, i++)));
					while (tok_is_char(ts, i, '.') && tok_is_name(ts, i + 1))
					{
						appendStringInfo(&value, ".%s", quote_identifier(tok_name(ts, i + 1)));
						i += 2;
					}
				}
				else
					return false;
			}
			defs = lappend(defs, value.data);
			if (tok_is_char(ts, i, ','))
				i++;
			else if (!tok_is_char(ts, i, ')'))
				return false;
		}
		if (i + 1 != rw->last)
			return false;
		rw_whole(rw);
		appendStringInfo(&rw->body, "CALL gp_exttable.create_protocol(%s, %s, %s)",
						 quote_literal_cstr(name), trusted ? "true" : "false",
						 name_array(defs, "text"));
		return true;
	}

	/* DROP PROTOCOL [IF EXISTS] name, ... [CASCADE | RESTRICT] */
	if (tok_is(ts, i, "drop") && tok_is(ts, i + 1, "protocol"))
	{
		bool		if_exists = false;
		bool		cascade = false;

		i += 2;
		if (tok_is(ts, i, "if") && tok_is(ts, i + 1, "exists"))
		{
			if_exists = true;
			i += 2;
		}
		if ((i = protocol_names(ts, i, &names)) < 0)
			return false;
		if (tok_is(ts, i, "cascade") || tok_is(ts, i, "restrict"))
			cascade = tok_is(ts, i++, "cascade");
		if (i != rw->last)
			return false;
		rw_whole(rw);
		appendStringInfo(&rw->body, "CALL gp_exttable.drop_protocol(%s, %s, %s)",
						 name_array(names, "text"), if_exists ? "true" : "false",
						 cascade ? "true" : "false");
		return true;
	}

	/* ALTER PROTOCOL name RENAME TO newname | OWNER TO role */
	if (tok_is(ts, i, "alter") && tok_is(ts, i + 1, "protocol") &&
		tok_is_name(ts, i + 2))
	{
		char	   *name = tok_name(ts, i + 2);

		i += 3;
		if (tok_is(ts, i, "rename") && tok_is(ts, i + 1, "to") &&
			tok_is_name(ts, i + 2) && i + 3 == rw->last)
		{
			rw_whole(rw);
			appendStringInfo(&rw->body, "CALL gp_exttable.rename_protocol(%s, %s)",
							 quote_literal_cstr(name),
							 quote_literal_cstr(tok_name(ts, i + 2)));
			return true;
		}
		if (tok_is(ts, i, "owner") && tok_is(ts, i + 1, "to") &&
			tok_is_name(ts, i + 2) && i + 3 == rw->last)
		{
			rw_whole(rw);
			appendStringInfo(&rw->body, "CALL gp_exttable.alter_protocol_owner(%s, %s)",
							 quote_literal_cstr(name),
							 quote_literal_cstr(tok_name(ts, i + 2)));
			return true;
		}
		return false;
	}

	/* GRANT ... ON PROTOCOL ... TO ..., REVOKE ... ON PROTOCOL ... FROM ... */
	if (tok_is(ts, i, "grant") || tok_is(ts, i, "revoke"))
	{
		bool		is_grant = tok_is(ts, i, "grant");
		bool		grant_option = false;
		bool		cascade = false;
		List	   *privs = NIL;
		List	   *roles = NIL;

		i++;
		if (!is_grant && tok_is(ts, i, "grant") && tok_is(ts, i + 1, "option") &&
			tok_is(ts, i + 2, "for"))
		{
			grant_option = true;
			i += 3;
		}
		if (tok_is(ts, i, "all"))
		{
			privs = lappend(privs, "all");
			i++;
			if (tok_is(ts, i, "privileges"))
				i++;
		}
		else if ((i = protocol_names(ts, i, &privs)) < 0)
			return false;
		if (!tok_is(ts, i, "on") || !tok_is(ts, i + 1, "protocol"))
			return false;
		if ((i = protocol_names(ts, i + 2, &names)) < 0)
			return false;
		if (!tok_is(ts, i, is_grant ? "to" : "from"))
			return false;
		i++;
		for (;;)
		{
			char	   *role = protocol_role(ts, &i);

			if (role == NULL)
				return false;
			roles = lappend(roles, role);
			if (!tok_is_char(ts, i, ','))
				break;
			i++;
		}
		if (is_grant && tok_is(ts, i, "with") && tok_is(ts, i + 1, "grant") &&
			tok_is(ts, i + 2, "option"))
		{
			grant_option = true;
			i += 3;
		}
		if (!is_grant && (tok_is(ts, i, "cascade") || tok_is(ts, i, "restrict")))
			cascade = tok_is(ts, i++, "cascade");
		if (i != rw->last)
			return false;
		rw_whole(rw);
		appendStringInfo(&rw->body,
						 "CALL gp_exttable.grant_protocol(%s, %s, %s, %s, %s, %s)",
						 is_grant ? "true" : "false", name_array(privs, "text"),
						 name_array(names, "text"), name_array(roles, "text"),
						 grant_option ? "true" : "false", cascade ? "true" : "false");
		return true;
	}

	return false;
}

/* ------------------------------------------------------------------------- */
/* Clauses inside a statement PostgreSQL already has                         */
/* ------------------------------------------------------------------------- */

typedef enum GpSubjKind
{
	GP_SUBJ_NONE,
	GP_SUBJ_RELATION,
	GP_SUBJ_SCHEMA,
	GP_SUBJ_DATABASE,
	GP_SUBJ_TABLESPACE,
	GP_SUBJ_ROLE,
} GpSubjKind;

/*
 * What a CREATE or ALTER statement is about, so that a TAG clause on it knows
 * where in the statement to go.  Only the kinds Cloudberry lets one be written
 * on.  For a relation, *object says which kind: 't' a table, 'f' a foreign
 * table, 'v' a view, 'm' a materialized view, 'S' a sequence, 'i' an index --
 * which decides the list its CREATE can carry the tag in: a WITH list, a
 * foreign table's OPTIONS, or none, when it goes on the parse node.  An index
 * need not be named; its name is then empty, and *after is where ON begins.
 */
static GpSubjKind
find_subject(const GpTokens *ts, int first, int last, char **name, int *after,
			 char *object)
{
	int			i = first;
	GpSubjKind	kind = GP_SUBJ_NONE;
	bool		foreign = false;
	int			e;

	*object = 0;

	if (tok_is(ts, i, "create"))
	{
		i++;
		/* everything CREATE allows before the object's kind */
		while (i < last &&
			   (tok_is(ts, i, "or") || tok_is(ts, i, "replace") ||
				tok_is(ts, i, "global") || tok_is(ts, i, "local") ||
				tok_is(ts, i, "temp") || tok_is(ts, i, "temporary") ||
				tok_is(ts, i, "unlogged") || tok_is(ts, i, "recursive") ||
				tok_is(ts, i, "foreign") || tok_is(ts, i, "unique") ||
				tok_is(ts, i, "incremental") || tok_is(ts, i, "dynamic")))
		{
			foreign |= tok_is(ts, i, "foreign");
			i++;
		}
	}
	else if (tok_is(ts, i, "alter"))
	{
		i++;
		if (tok_is(ts, i, "foreign"))
		{
			foreign = true;
			i++;
		}
	}
	else
		return GP_SUBJ_NONE;

	if (tok_is(ts, i, "table"))
	{
		kind = GP_SUBJ_RELATION;
		*object = foreign ? 'f' : 't';
	}
	else if (tok_is(ts, i, "view"))
	{
		kind = GP_SUBJ_RELATION;
		*object = 'v';
	}
	else if (tok_is(ts, i, "sequence"))
	{
		kind = GP_SUBJ_RELATION;
		*object = 'S';
	}
	else if (tok_is(ts, i, "materialized") && tok_is(ts, i + 1, "view"))
	{
		kind = GP_SUBJ_RELATION;
		*object = 'm';
		i++;
	}
	else if (tok_is(ts, i, "index"))
	{
		kind = GP_SUBJ_RELATION;
		*object = 'i';
		if (tok_is(ts, i + 1, "concurrently"))
			i++;
		if (tok_is(ts, first, "create") && tok_is(ts, i + 1, "on"))
		{
			*name = pstrdup("");
			*after = i + 1;
			return kind;
		}
	}
	else if (tok_is(ts, i, "schema"))
		kind = GP_SUBJ_SCHEMA;
	else if (tok_is(ts, i, "database"))
		kind = GP_SUBJ_DATABASE;
	else if (tok_is(ts, i, "tablespace"))
		kind = GP_SUBJ_TABLESPACE;
	else if (tok_is(ts, i, "role") || tok_is(ts, i, "user"))
		kind = GP_SUBJ_ROLE;
	else
		return GP_SUBJ_NONE;

	i++;

	if (tok_is(ts, i, "if") && tok_is(ts, i + 1, "not") && tok_is(ts, i + 2, "exists"))
		i += 3;
	else if (tok_is(ts, i, "if") && tok_is(ts, i + 1, "exists"))
		i += 2;

	e = skip_qualified_name(ts, i);
	if (e == i)
		return GP_SUBJ_NONE;

	*name = rw_text(ts, i, e);
	*after = e;
	return kind;
}

/* One TAG (...) or UNSET TAG (...) clause, as it was written. */
typedef struct GpTagClause
{
	int			start;			/* its first token: TAG, UNSET, or WITH */
	int			after;			/* the token after its closing parenthesis */
	bool		unset;
	List	   *keys;			/* char *: the tags it names */
	List	   *values;			/* char *: their values, NULL for UNSET TAG */
} GpTagClause;

/*
 * The TAG and UNSET TAG clauses of a statement, from token `from` on, at the
 * statement's own level.
 *
 * TAG ( ... ) is Cloudberry's clause only when what is in it reads like one,
 * and all of it does: tag(x) in the query of a CREATE TABLE AS is a function
 * call, and TAG () or a list with a hole in it is the syntax error it is in
 * Cloudberry.  Either is left where it is, for PostgreSQL's grammar to refuse
 * or accept.
 *
 * On CREATE SCHEMA, Cloudberry's grammar has WITH TAG (...) and nothing else,
 * so WITH goes with the clause and a bare TAG there is left for PostgreSQL to
 * refuse, as Cloudberry does.
 */
static List *
find_tag_clauses(const GpRewrite *rw, GpSubjKind kind, int from)
{
	const GpTokens *ts = rw->ts;
	bool		creating = tok_is(ts, rw->first, "create");
	List	   *clauses = NIL;
	int			depth = 0;

	for (int i = from; i < rw->last; i++)
	{
		bool		unset;
		int			open;
		int			close;
		int			j;
		List	   *keys = NIL;
		List	   *values = NIL;
		GpTagClause *c;

		if (tok_is_char(ts, i, '('))
		{
			depth++;
			continue;
		}
		if (tok_is_char(ts, i, ')'))
		{
			depth--;
			continue;
		}
		if (depth != 0)
			continue;

		unset = tok_is(ts, i, "unset") && tok_is(ts, i + 1, "tag");
		if (!unset && !(tok_is(ts, i, "tag") && tok_is_char(ts, i + 1, '(')))
			continue;

		open = unset ? i + 2 : i + 1;
		if (!tok_is_char(ts, open, '('))
			continue;
		close = skip_parens(ts, open) - 1;

		if (kind == GP_SUBJ_SCHEMA && creating &&
			(unset || i == from || !tok_is(ts, i - 1, "with")))
			continue;

		/* The pairs, or for UNSET TAG the names, and nothing else. */
		j = open + 1;
		while (j < close && tok_is_name(ts, j))
		{
			keys = lappend(keys, tok_name(ts, j));
			j++;
			if (unset)
				values = lappend(values, NULL);
			else
			{
				if (!tok_is_char(ts, j, '=') || !tok_is_string(ts, j + 1))
					break;
				values = lappend(values, ts->toks[j + 1].str);
				j += 2;
			}
			if (j < close && tok_is_char(ts, j, ','))
				j++;
			else
				break;
		}
		if (keys == NIL || j != close)
			continue;

		c = palloc(sizeof(GpTagClause));
		c->start = (kind == GP_SUBJ_SCHEMA && creating) ? i - 1 : i;
		c->after = close + 1;
		c->unset = unset;
		c->keys = keys;
		c->values = values;
		clauses = lappend(clauses, c);

		i = close;
	}

	return clauses;
}

/*
 * A tag as an option of CREATE DATABASE or ALTER DATABASE.  Neither has a
 * namespaced option, but both take any identifier for an option's name, so a
 * tag is "gp_tag.env" = 'prod' -- or = DEFAULT, which is UNSET TAG -- written
 * one after another, as the statement writes its options.  gp_sql's
 * ProcessUtility hook takes them out again before createdb() or
 * AlterDatabase() would refuse them.
 */
static void
tag_db_option(StringInfo opts, const char *key, const char *value)
{
	appendStringInfo(opts, " %s = %s",
					 quote_identifier(psprintf("gp_tag.%s", key)),
					 value != NULL ? quote_literal_cstr(value) : "DEFAULT");
}

/* gp_tag.env = 'prod', ... for a WITH, SET or RESET list; no values for RESET. */
static char *
tag_reloptions(const GpTagClause *c)
{
	StringInfoData opts;
	ListCell   *k;
	ListCell   *v;

	initStringInfo(&opts);
	forboth(k, c->keys, v, c->values)
	{
		if (opts.len > 0)
			appendStringInfoString(&opts, ", ");
		appendStringInfo(&opts, "gp_tag.%s", quote_identifier((char *) lfirst(k)));
		if (lfirst(v) != NULL)
			appendStringInfo(&opts, " = %s", quote_literal_cstr((char *) lfirst(v)));
	}
	return opts.data;
}

/*
 * ALTER SCHEMA s TAG (...), ALTER DATABASE, ALTER TABLESPACE and ALTER USER,
 * whose TAG and UNSET TAG are each a whole statement of Cloudberry's:
 *
 *	 ALTER DATABASE d TAG (env = 'prod')
 *	   -> ALTER DATABASE d "gp_tag.env" = 'prod'
 *	 ALTER TABLESPACE t UNSET TAG (env)
 *	   -> ALTER TABLESPACE t RESET (gp_tag.env)
 *	 ALTER USER u TAG (env = 'prod')
 *	   -> ALTER USER u, carrying gp_tag.env = 'prod' (GpAttachCarriers)
 *	 ALTER SCHEMA s TAG (env = 'prod')
 *	   -> CALL gp_sql.alter_schema_tags('s'::regnamespace, '{"env": "prod"}')
 *
 * Each is the statement PostgreSQL has for altering that kind of object,
 * which then answers as that statement does, and checks what it checks --
 * that the database or tablespace is the user's.  ALTER USER with no options
 * checks next to nothing, so gp_sql checks for the tags what SECURITY LABEL
 * would.  PostgreSQL has no ALTER SCHEMA that takes an option, or that does
 * nothing, so there it is a CALL, whose command tag is CALL; the procedure
 * writes the label with SECURITY LABEL, which checks the schema is the
 * user's.
 */
static void
rw_alter_tags_whole(GpRewrite *rw, GpSubjKind kind, const char *name,
					const GpTagClause *c)
{
	const GpTokens *ts = rw->ts;
	int			start = ts->toks[c->start].off;
	int			end = (c->after < ts->ntoks) ? ts->toks[c->after].off : ts->srclen;
	ListCell   *k;
	ListCell   *v;
	StringInfoData buf;

	initStringInfo(&buf);

	switch (kind)
	{
		case GP_SUBJ_DATABASE:
			forboth(k, c->keys, v, c->values)
				tag_db_option(&buf, (char *) lfirst(k), (char *) lfirst(v));
			appendStringInfoChar(&buf, ' ');
			rw_edit(rw, start, end, buf.data);
			break;

		case GP_SUBJ_TABLESPACE:
			rw_edit(rw, start, end,
					psprintf("%s (%s) ", c->unset ? "RESET" : "SET", tag_reloptions(c)));
			break;

		case GP_SUBJ_ROLE:
			forboth(k, c->keys, v, c->values)
				rw_add_carrier(rw, "gp_tag", (char *) lfirst(k), (char *) lfirst(v),
							   start);
			rw_edit(rw, start, end, " ");
			break;

		case GP_SUBJ_SCHEMA:
			rw_whole(rw);
			appendStringInfo(&rw->body, "CALL gp_sql.alter_schema_tags(%s::regnamespace, ",
							 quote_literal_cstr(name));
			if (c->unset)
				appendStringInfo(&rw->body, "unset_tags => %s)",
								 name_array(c->keys, "name"));
			else
			{
				StringInfoData json;

				/* A JSON literal, not jsonb_build_object: 50 tags is 100 arguments. */
				initStringInfo(&json);
				appendStringInfoChar(&json, '{');
				forboth(k, c->keys, v, c->values)
				{
					if (json.len > 1)
						appendStringInfoString(&json, ", ");
					escape_json(&json, (char *) lfirst(k));
					appendStringInfoString(&json, ": ");
					escape_json(&json, (char *) lfirst(v));
				}
				appendStringInfoChar(&json, '}');
				appendStringInfo(&rw->body, "set_tags => %s::jsonb)",
								 quote_literal_cstr(json.data));
			}
			break;

		default:
			break;
	}
}

/*
 * TAG (name = 'value', ...) and UNSET TAG (name, ...), wherever Cloudberry
 * lets them be written, each becoming part of a statement PostgreSQL has --
 * one statement for one, never the statement followed by calls:
 *
 *   - on CREATE TABLE, CREATE TABLE AS, CREATE [MATERIALIZED] VIEW, CREATE
 *     INDEX and CREATE DIRECTORY TABLE, options of the statement, gp_tag.env
 *     = 'prod' in its WITH list (rw_add_option);
 *   - on CREATE FOREIGN TABLE, which has no WITH list, options in its OPTIONS
 *     list, "gp_tag.env" 'prod' (rw_add_fdw_option);
 *   - on CREATE DATABASE and CREATE TABLESPACE, options as each statement
 *     takes one (tag_db_option, and the tablespace's WITH list);
 *   - on CREATE SCHEMA, CREATE USER and CREATE SEQUENCE, whose statements
 *     have no list a namespaced option can go in, DefElems carried to the
 *     statement's parse node (rw_add_carrier);
 *   - on ALTER of a table, view, materialized view, index, sequence or
 *     foreign table, TAG (...) is SET (gp_tag....) and UNSET TAG (...) is
 *     RESET (gp_tag....), in place, among the statement's other actions;
 *   - and on ALTER of a schema, database, tablespace or role, see
 *     rw_alter_tags_whole.
 *
 * gp_sql's ProcessUtility hook takes each of them out again, checks the tags
 * before the statement runs, and puts them on the object once it exists.
 * Nothing about a tag is decided here but where it goes.
 */
static void
rw_tag_clauses(GpRewrite *rw, GpSubjKind kind, const char *name, int from)
{
	const GpTokens *ts = rw->ts;
	bool		creating = tok_is(ts, rw->first, "create");
	List	   *clauses = find_tag_clauses(rw, kind, from);
	int			with_close = -1;	/* a tablespace's WITH list's ')' */
	ListCell   *lc;

	if (clauses == NIL)
		return;

	/*
	 * ALTER of a schema, database, tablespace or role: Cloudberry's grammar
	 * has ALTER <kind> name TAG (...) and ALTER <kind> name UNSET TAG (...),
	 * each on its own, and no TAG beside anything else -- its tag test
	 * expects ALTER USER u CONNECTION LIMIT 3 TAG (...) and ALTER TABLESPACE t
	 * SET (...) TAG (...) to be syntax errors at TAG.  PostgreSQL's grammar
	 * would refuse them too, but not all at TAG: a role's option may be any
	 * word, and it would be "unrecognized role option", and a database's
	 * any word with a value, so it would be the parenthesis.
	 */
	if (!creating && kind != GP_SUBJ_RELATION)
	{
		GpTagClause *c = (GpTagClause *) linitial(clauses);

		if (c->start != from)
			rw_syntax_error(rw, c->start);
		if (c->after != rw->last)
			rw_syntax_error(rw, c->after);
		rw_alter_tags_whole(rw, kind, name, c);
		return;
	}

	if (creating && kind == GP_SUBJ_TABLESPACE)
	{
		int			depth = 0;

		for (int i = from; i < rw->last; i++)
		{
			if (tok_is_char(ts, i, '('))
			{
				if (depth == 0 && tok_is(ts, i - 1, "with"))
					with_close = skip_parens(ts, i) - 1;
				depth++;
			}
			else if (tok_is_char(ts, i, ')'))
				depth--;
		}
	}

	foreach(lc, clauses)
	{
		GpTagClause *c = (GpTagClause *) lfirst(lc);
		int			start = ts->toks[c->start].off;
		int			end = (c->after < ts->ntoks) ? ts->toks[c->after].off : ts->srclen;
		const char *replacement = " ";
		ListCell   *k;
		ListCell   *v;

		/* Not Cloudberry's grammar either; PostgreSQL refuses it. */
		if (creating && c->unset)
			continue;

		if (!creating)
		{
			/* ALTER of a relation: SET or RESET, in place */
			replacement = psprintf("%s (%s) ", c->unset ? "RESET" : "SET",
								   tag_reloptions(c));
		}
		else if (kind == GP_SUBJ_RELATION && strchr("tvmi", rw->object) != NULL)
		{
			forboth(k, c->keys, v, c->values)
				rw_add_option(rw, psprintf("gp_tag.%s = %s",
										   quote_identifier((char *) lfirst(k)),
										   quote_literal_cstr((char *) lfirst(v))),
							  start);
		}
		else if (kind == GP_SUBJ_RELATION && rw->object == 'f')
		{
			forboth(k, c->keys, v, c->values)
				rw_add_fdw_option(rw, psprintf("gp_tag.%s", (char *) lfirst(k)),
								  (char *) lfirst(v));
		}
		else if (kind == GP_SUBJ_DATABASE)
		{
			StringInfoData opts;

			initStringInfo(&opts);
			forboth(k, c->keys, v, c->values)
				tag_db_option(&opts, (char *) lfirst(k), (char *) lfirst(v));
			appendStringInfoChar(&opts, ' ');
			replacement = opts.data;
		}
		else if (kind == GP_SUBJ_TABLESPACE && with_close >= 0)
			rw_edit(rw, ts->toks[with_close].off, ts->toks[with_close].off,
					psprintf(", %s", tag_reloptions(c)));
		else if (kind == GP_SUBJ_TABLESPACE)
			replacement = psprintf(" WITH (%s) ", tag_reloptions(c));
		else
		{
			/* a schema, a role or a sequence */
			forboth(k, c->keys, v, c->values)
				rw_add_carrier(rw, "gp_tag", (char *) lfirst(k), (char *) lfirst(v),
							   start);
		}

		rw_edit(rw, start, end, replacement);
	}
}

/*
 * DISTRIBUTED BY (a, b) / DISTRIBUTED RANDOMLY / DISTRIBUTED REPLICATED
 *
 * The policy is recorded on the table; what reads it is ORCA's relcache
 * translator, which asks every relation what it is distributed by, and the
 * dispatch of M2.  On one node every table is on the one node, so nothing
 * changes for the statement itself.  It is an option of the statement,
 * which gp_sql's ProcessUtility hook takes out and records once the table
 * exists, so that the statement stays one (see rw_tag_clauses): on CREATE
 * TABLE, CREATE TABLE AS and CREATE MATERIALIZED VIEW gp.distributed_by =
 * '(a,b)' in the WITH list, and on CREATE FOREIGN TABLE "gp.distributed_by"
 * '(a,b)' in the OPTIONS list.  ALTER TABLE ... SET DISTRIBUTED BY becomes
 * an option of the ALTER's SET, in rw_partition_cmds, where the rest of
 * ALTER TABLE's Cloudberry commands are.
 *
 * THE COLUMN LIST KEEPS ITS PARENTHESES, and that is not decoration.  Written
 * bare, a one-column list is indistinguishable from the word that names a
 * policy: DISTRIBUTED BY (random) and DISTRIBUTED RANDOMLY both recorded
 * "random", and nothing downstream could tell a table hashed on a column
 * called "random" from a randomly distributed one.  That is two different
 * distributions under one spelling, and the reader would have answered the
 * wrong one.  So a column list is "(a,b)" and the two policy words stay bare.
 *
 * Each name is quoted as an identifier where it needs to be, so that a column
 * whose name holds a comma or a capital survives the round trip.  The scanner
 * has already downcased an unquoted name and dequoted a quoted one, so what
 * is quoted here is the true column name.
 */
static void
rw_distribution(GpRewrite *rw, const char *policy, int at)
{
	if (rw->object == 'f')
		rw_add_fdw_option(rw, "gp.distributed_by", policy);
	else
		rw_add_option(rw, psprintf("gp.distributed_by = %s",
								   quote_literal_cstr(policy)), at);
}

/*
 * A DISTRIBUTED clause at token i -- BY (a, b opclass), RANDOMLY or
 * REPLICATED -- as the policy gp.distributed_by records, with *after set to
 * the token after it; NULL when there is none.
 *
 * Each column may name the operator class it is hashed with, qualified or
 * not, which is kept as it is written for gp_sql to resolve against the
 * column's type (distribution.c).  Cloudberry's grammar (distributed_by_list)
 * refuses an empty list, and a column named twice at its second naming: so
 * does this, in its words.
 */
static char *
distributed_policy(const GpTokens *ts, int i, int *after)
{
	StringInfoData cols;
	List	   *seen = NIL;
	int			j;

	if (!tok_is(ts, i, "distributed"))
		return NULL;
	if (tok_is(ts, i + 1, "randomly") || tok_is(ts, i + 1, "replicated"))
	{
		*after = i + 2;
		return tok_is(ts, i + 1, "randomly") ? "random" : "replicated";
	}
	if (!tok_is(ts, i + 1, "by") || !tok_is_char(ts, i + 2, '('))
		return NULL;

	initStringInfo(&cols);
	appendStringInfoChar(&cols, '(');
	for (j = i + 3;; j++)
	{
		char	   *name;
		ListCell   *lc;

		if (!tok_is_name(ts, j))
			ts_syntax_error(ts, j);
		name = tok_name(ts, j);
		foreach(lc, seen)
		{
			if (strcmp((char *) lfirst(lc), name) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_COLUMN),
						 errmsg("duplicate column in DISTRIBUTED BY clause"),
						 errposition(pg_mbstrlen_with_len(ts->src, ts->toks[j].off) + 1)));
		}
		seen = lappend(seen, name);
		if (cols.len > 1)
			appendStringInfoChar(&cols, ',');
		appendStringInfoString(&cols, quote_identifier(name));

		/* the column's operator class */
		if (tok_is_name(ts, j + 1))
		{
			j++;
			appendStringInfo(&cols, " %s", quote_identifier(tok_name(ts, j)));
			while (tok_is_char(ts, j + 1, '.') && tok_is_name(ts, j + 2))
			{
				appendStringInfo(&cols, ".%s", quote_identifier(tok_name(ts, j + 2)));
				j += 2;
			}
		}

		if (tok_is_char(ts, j + 1, ')'))
			break;
		if (!tok_is_char(ts, j + 1, ','))
			ts_syntax_error(ts, j + 1);
		j++;
	}
	appendStringInfoChar(&cols, ')');
	*after = j + 2;
	return cols.data;
}

static void
rw_distributed(GpRewrite *rw, int from)
{
	const GpTokens *ts = rw->ts;
	int			depth = 0;

	if (!tok_is(ts, rw->first, "create") || rw->object == 0 ||
		strchr("tmf", rw->object) == NULL)
		return;

	for (int i = from; i < rw->last; i++)
	{
		char	   *policy;
		int			after;

		if (tok_is_char(ts, i, '('))
		{
			depth++;
			continue;
		}
		if (tok_is_char(ts, i, ')'))
		{
			depth--;
			continue;
		}
		if (depth != 0 || (policy = distributed_policy(ts, i, &after)) == NULL)
			continue;

		rw_distribution(rw, policy, ts->toks[i].off);
		rw_edit(rw, ts->toks[i].off,
				(after < ts->ntoks) ? ts->toks[after].off : ts->srclen, " ");
		i = after - 1;
		depth = 0;
	}
}

/*
 * Words that only have to be spelled differently.
 *
 *	 CREATE STORAGE SERVER s			-> CREATE SERVER s FOREIGN DATA WRAPPER gp_storage
 *	 ALTER/DROP STORAGE SERVER s		-> ALTER/DROP SERVER s
 *	 ... STORAGE USER MAPPING ...		-> ... USER MAPPING ...
 *	 REFRESH/DROP DYNAMIC TABLE t		-> REFRESH/DROP MATERIALIZED VIEW t
 */
static bool
rw_storage_and_dynamic(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			i = rw->first;
	bool		did = false;

	/* CREATE STORAGE SERVER name [OPTIONS (...)] */
	if (tok_is(ts, i, "create") && tok_is(ts, i + 1, "storage") &&
		tok_is(ts, i + 2, "server"))
	{
		int			j = i + 3;
		int			e;

		if (tok_is(ts, j, "if") && tok_is(ts, j + 1, "not") && tok_is(ts, j + 2, "exists"))
			j += 3;
		e = skip_qualified_name(ts, j);
		if (e == j)
			return false;

		rw_edit(rw, ts->toks[i + 1].off, ts->toks[i + 2].off, "");	/* drop STORAGE */
		rw_edit(rw, tok_end(ts, e - 1), tok_end(ts, e - 1),
				"FOREIGN DATA WRAPPER gp_storage ");
		return true;
	}

	/* ALTER / DROP STORAGE SERVER, and the USER MAPPING forms */
	for (i = rw->first; i < rw->last; i++)
	{
		if (!tok_is(ts, i, "storage"))
			continue;
		if (tok_is(ts, i + 1, "server") || tok_is(ts, i + 1, "user"))
		{
			rw_edit(rw, ts->toks[i].off, ts->toks[i + 1].off, "");
			did = true;
		}
	}
	if (did)
		return true;

	/* REFRESH / DROP DYNAMIC TABLE */
	i = rw->first;
	if ((tok_is(ts, i, "refresh") || tok_is(ts, i, "drop")) &&
		tok_is(ts, i + 1, "dynamic") && tok_is(ts, i + 2, "table"))
	{
		rw_edit(rw, ts->toks[i + 1].off, tok_end(ts, i + 2), "MATERIALIZED VIEW ");
		return true;
	}

	return false;
}

/*
 * orientation=row, in any option list: WITH (appendonly=true, orientation=row)
 *	 -> orientation='row'
 *
 * ROW is a keyword PostgreSQL's grammar takes as an option's value nowhere,
 * where COLUMN, being reserved, it takes; Cloudberry's grammar took both.
 * Quoted, it is the string gp_ao reads either way.
 */
static bool
rw_orientation_row(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	bool		did = false;

	for (int j = rw->first; j + 2 < rw->last; j++)
	{
		if (tok_is(ts, j, "orientation") && tok_is_char(ts, j + 1, '=') &&
			tok_is_kw(ts, j + 2, "row"))
		{
			rw_edit(rw, ts->toks[j + 2].off, tok_end(ts, j + 2), "'row' ");
			did = true;
		}
	}
	return did;
}

/* ------------------------------------------------------------------------- */
/* ENCODING clauses                                                          */
/* ------------------------------------------------------------------------- */

/*
 * The options of an ENCODING ( ... ) whose '(' is token `open`, as name=value
 * pairs joined by commas, each name after `prefix` and each value quoted
 * where `quote` says; *close is the ')'.  NULL where what is in the
 * parentheses is no such list: the clause is then left where it is, for
 * PostgreSQL's grammar to refuse, as Cloudberry's refuses it.
 */
static char *
encoding_opts(const GpTokens *ts, int open, int *close, const char *prefix,
			  bool quote)
{
	int			end = skip_parens(ts, open) - 1;
	StringInfoData buf;
	int			j = open + 1;

	initStringInfo(&buf);
	while (j < end)
	{
		char	   *name;
		char	   *value;

		if (!tok_is_name(ts, j) || !tok_is_char(ts, j + 1, '=') || j + 2 >= end)
			return NULL;
		name = tok_name(ts, j);
		j += 2;
		if (tok_is_string(ts, j))
			value = ts->toks[j].str;
		else if (tok_is_name(ts, j))
			value = tok_name(ts, j);
		else if (ts->toks[j].code == GP_ICONST || ts->toks[j].code == GP_FCONST)
			value = pnstrdup(ts->src + ts->toks[j].off,
							 tok_stop(ts, j) - ts->toks[j].off);
		else
			return NULL;
		j++;
		if (buf.len > 0)
			appendStringInfoChar(&buf, ',');
		appendStringInfo(&buf, "%s%s=%s", prefix, name,
						 quote ? quote_literal_cstr(value) : value);
		if (j < end)
		{
			if (!tok_is_char(ts, j, ','))
				return NULL;
			j++;
		}
	}
	if (buf.len == 0)
		return NULL;
	*close = end;
	return buf.data;
}

/*
 * One of Cloudberry's column encoding clauses, COLUMN c ENCODING (...) or
 * DEFAULT COLUMN ENCODING (...), as the item of gp_ao.encoding it is (see
 * rw_encoding_create): the classic partition clause's, which gp_sql's
 * partition.c gives each partition it makes.  NULL for a clause that is not
 * one, which the partition clause's own parser has refused already.
 */
char *
GpEncodingSpecItem(const char *clause)
{
	GpTokens   *ts = GpTokenize(clause);
	int			c;
	char	   *opts;

	if (tok_is_kw(ts, 0, "default") && tok_is_kw(ts, 1, "column") &&
		tok_is(ts, 2, "encoding") && tok_is_char(ts, 3, '(') &&
		(opts = encoding_opts(ts, 3, &c, "", false)) != NULL)
		return psprintf("DEFAULT(%s)", opts);
	if (tok_is_kw(ts, 0, "column") && tok_is_name(ts, 1) &&
		tok_is(ts, 2, "encoding") && tok_is_char(ts, 3, '(') &&
		(opts = encoding_opts(ts, 3, &c, "", false)) != NULL)
		return psprintf("COLUMN %s(%s)", quote_identifier(tok_name(ts, 1)), opts);
	return NULL;
}

/* Words that begin a table element that is no column. */
static bool
table_constraint_word(const GpTokens *ts, int i)
{
	return tok_is_kw(ts, i, "constraint") || tok_is_kw(ts, i, "primary") ||
		tok_is_kw(ts, i, "unique") || tok_is_kw(ts, i, "check") ||
		tok_is_kw(ts, i, "foreign") || tok_is_kw(ts, i, "exclude") ||
		tok_is_kw(ts, i, "like") || tok_is_kw(ts, i, "not");
}

/*
 * CREATE TABLE's ENCODING clauses, in its list of columns:
 *
 *	 a int ENCODING (compresstype=zlib), COLUMN b ENCODING (blocksize=8192),
 *	 DEFAULT COLUMN ENCODING (compresstype=zstd)
 *	   -> a int, with gp_ao.encoding = 'a(compresstype=zlib);
 *			COLUMN b(blocksize=8192);DEFAULT(compresstype=zstd)' in its WITH list
 *
 * The option is gp_ao's, which takes it out again, and gives each column its
 * options once the table is made (ao_encoding.c); a column's name is as
 * quote_identifier() writes it, so that DEFAULT alone is the default.  The
 * clauses are Cloudberry's for a table by column, and gp_ao says so where
 * the table is none.
 */
static void
rw_encoding_create(GpRewrite *rw, int after_name)
{
	const GpTokens *ts = rw->ts;
	int			open = after_name;
	int			close;
	int			start;
	int			nelems = 0;
	int		   *starts;
	int		   *ends;
	bool	   *drop;
	int			lastkept = -1;
	StringInfoData spec;

	if (tok_is_kw(ts, open, "partition") && tok_is_kw(ts, open + 1, "of"))
		open = skip_qualified_name(ts, open + 2);
	if (!tok_is_char(ts, open, '('))
		return;
	close = skip_parens(ts, open) - 1;
	if (close >= rw->last)
		return;

	starts = palloc_array(int, close - open + 1);
	ends = palloc_array(int, close - open + 1);
	drop = palloc0_array(bool, close - open + 1);
	initStringInfo(&spec);

	for (start = open + 1; start < close;)
	{
		int			end = start;
		int			depth = 0;
		int			c;
		char	   *opts;
		int			n = nelems++;

		while (end < close && !(depth == 0 && tok_is_char(ts, end, ',')))
		{
			if (tok_is_char(ts, end, '('))
				depth++;
			else if (tok_is_char(ts, end, ')'))
				depth--;
			end++;
		}
		starts[n] = start;
		ends[n] = end;

		if (tok_is_kw(ts, start, "default") && tok_is_kw(ts, start + 1, "column") &&
			tok_is(ts, start + 2, "encoding") && tok_is_char(ts, start + 3, '(') &&
			(opts = encoding_opts(ts, start + 3, &c, "", false)) != NULL &&
			c == end - 1)
		{
			appendStringInfo(&spec, "%sDEFAULT(%s)", spec.len > 0 ? ";" : "", opts);
			drop[n] = true;
		}
		else if (tok_is_kw(ts, start, "column") && tok_is_name(ts, start + 1) &&
				 tok_is(ts, start + 2, "encoding") && tok_is_char(ts, start + 3, '(') &&
				 (opts = encoding_opts(ts, start + 3, &c, "", false)) != NULL &&
				 c == end - 1)
		{
			appendStringInfo(&spec, "%sCOLUMN %s(%s)", spec.len > 0 ? ";" : "",
							 quote_identifier(tok_name(ts, start + 1)), opts);
			drop[n] = true;
		}
		else if (tok_is_name(ts, start) && !table_constraint_word(ts, start))
		{
			depth = 0;
			for (int k = start + 1; k < end; k++)
			{
				if (tok_is_char(ts, k, '('))
					depth++;
				else if (tok_is_char(ts, k, ')'))
					depth--;
				else if (depth == 0 && tok_is(ts, k, "encoding") &&
						 tok_is_char(ts, k + 1, '(') &&
						 (opts = encoding_opts(ts, k + 1, &c, "", false)) != NULL)
				{
					appendStringInfo(&spec, "%s%s(%s)", spec.len > 0 ? ";" : "",
									 quote_identifier(tok_name(ts, start)), opts);
					rw_edit(rw, ts->toks[k].off, tok_end(ts, c), " ");
					break;
				}
			}
		}
		if (!drop[n])
			lastkept = n;
		start = end + 1;
	}

	/*
	 * The clauses that were elements of their own go, with a comma each: the
	 * one after them where a kept element follows, else the one before.
	 */
	for (int n = 0; n < nelems; n++)
	{
		if (!drop[n])
			continue;
		if (n < lastkept)
			rw_edit(rw, ts->toks[starts[n]].off, ts->toks[starts[n + 1]].off, "");
		else
		{
			/* the rest, from the comma after the last kept, or all of it */
			rw_edit(rw, lastkept >= 0 ? ts->toks[ends[lastkept]].off :
					ts->toks[open + 1].off, ts->toks[close].off, "");
			break;
		}
	}

	if (spec.len > 0)
		rw_add_option(rw, psprintf("gp_ao.encoding = %s", quote_literal_cstr(spec.data)),
					  ts->toks[open].off);
}

/*
 * ALTER TABLE's:
 *
 *	 ADD [COLUMN] c int ENCODING (compresstype=zlib)
 *	   -> ADD [COLUMN] c int, ALTER COLUMN c SET (gp_ao.compresstype='zlib')
 *	 ALTER [COLUMN] c SET ENCODING (compresstype=zlib)
 *	   -> ALTER [COLUMN] c SET (gp_ao.compresstype='zlib')
 *
 * A column's options are PostgreSQL's syntax for what the clause says, and
 * gp_ao's ProcessUtility hook takes its own namespace's out of the command
 * before PostgreSQL would refuse them.
 */
static void
rw_encoding_alter(GpRewrite *rw, int after_name)
{
	const GpTokens *ts = rw->ts;
	int			start = after_name;

	while (start < rw->last)
	{
		int			end = start;
		int			depth = 0;
		int			k;
		int			c;
		char	   *opts;

		while (end < rw->last && !(depth == 0 && tok_is_char(ts, end, ',')))
		{
			if (tok_is_char(ts, end, '('))
				depth++;
			else if (tok_is_char(ts, end, ')'))
				depth--;
			end++;
		}

		if (tok_is_kw(ts, start, "add"))
		{
			k = start + 1;
			if (tok_is_kw(ts, k, "column"))
				k++;
			if (tok_is(ts, k, "if") && tok_is(ts, k + 1, "not") && tok_is(ts, k + 2, "exists"))
				k += 3;
			if (tok_is_name(ts, k) && !table_constraint_word(ts, k))
			{
				int			col = k;

				depth = 0;
				for (k = col + 1; k < end; k++)
				{
					if (tok_is_char(ts, k, '('))
						depth++;
					else if (tok_is_char(ts, k, ')'))
						depth--;
					else if (depth == 0 && tok_is(ts, k, "encoding") &&
							 tok_is_char(ts, k + 1, '(') &&
							 (opts = encoding_opts(ts, k + 1, &c, "gp_ao.", true)) != NULL)
					{
						rw_edit(rw, ts->toks[k].off, tok_end(ts, c), " ");
						rw_edit(rw, tok_end(ts, end - 1), tok_end(ts, end - 1),
								psprintf(", ALTER COLUMN %s SET (%s) ",
										 pnstrdup(ts->src + ts->toks[col].off,
												  tok_stop(ts, col) - ts->toks[col].off),
										 opts));
						break;
					}
				}
			}
		}
		else if (tok_is_kw(ts, start, "alter"))
		{
			k = start + 1;
			if (tok_is_kw(ts, k, "column"))
				k++;
			if (tok_is_name(ts, k) && tok_is_kw(ts, k + 1, "set") &&
				tok_is(ts, k + 2, "encoding") && tok_is_char(ts, k + 3, '(') &&
				(opts = encoding_opts(ts, k + 3, &c, "gp_ao.", true)) != NULL &&
				c == end - 1)
				rw_edit(rw, ts->toks[k + 2].off, tok_stop(ts, c),
						psprintf("(%s)", opts));
		}
		start = end + 1;
	}
}

/*
 * ALTER TYPE t SET DEFAULT ENCODING (compresstype=rle_type)
 *	 -> SECURITY LABEL FOR gp_ao ON TYPE t IS 'compresstype=rle_type'
 *
 * gp_ao keeps a type's default for the columns of it as its label.
 */
static bool
rw_alter_type_encoding(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			i = rw->first;
	int			e;
	int			c;
	char	   *opts;

	if (!tok_is(ts, i, "alter") || !tok_is(ts, i + 1, "type"))
		return false;
	e = skip_qualified_name(ts, i + 2);
	if (e == i + 2 || !tok_is_kw(ts, e, "set") || !tok_is_kw(ts, e + 1, "default") ||
		!tok_is(ts, e + 2, "encoding") || !tok_is_char(ts, e + 3, '(') ||
		(opts = encoding_opts(ts, e + 3, &c, "", false)) == NULL ||
		c != rw->last - 1)
		return false;
	rw_whole(rw);
	appendStringInfo(&rw->body, "SECURITY LABEL FOR gp_ao ON TYPE %s IS %s",
					 rw_text(ts, i + 2, e), quote_literal_cstr(opts));
	return true;
}

/*
 * CREATE INCREMENTAL MATERIALIZED VIEW ... AS
 * CREATE DYNAMIC TABLE ... SCHEDULE 's' ... AS
 *
 * These two really do need an option on the statement: gp_matview reads it
 * before the view is made.  So the option goes into the statement's WITH
 * list, where rw_place_options puts every option a statement's clauses
 * become.
 */
static bool
rw_matview_options(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			i = rw->first;
	const char *option = NULL;
	char	   *schedule = NULL;
	int			at;
	int			depth = 0;

	if (!tok_is(ts, i, "create"))
		return false;
	at = (i + 1 < ts->ntoks) ? ts->toks[i + 1].off : ts->srclen;

	if (tok_is(ts, i + 1, "incremental") && tok_is(ts, i + 2, "materialized") &&
		tok_is(ts, i + 3, "view"))
	{
		option = "gp.incremental";
		/* INCREMENTAL is ours; the rest is PostgreSQL's already. */
		rw_edit(rw, ts->toks[i + 1].off, ts->toks[i + 2].off, "");
		i += 4;
	}
	else if (tok_is(ts, i + 1, "dynamic") && tok_is(ts, i + 2, "table"))
	{
		rw_edit(rw, ts->toks[i + 1].off, tok_end(ts, i + 2), "MATERIALIZED VIEW ");
		i += 3;
	}
	else
		return false;

	/* Find the SCHEDULE clause. */
	for (int j = i; j < rw->last; j++)
	{
		if (tok_is_char(ts, j, '('))
		{
			depth++;
			continue;
		}
		if (tok_is_char(ts, j, ')'))
		{
			depth--;
			continue;
		}
		if (depth != 0)
			continue;

		if (option == NULL && tok_is(ts, j, "schedule") && tok_is_string(ts, j + 1))
		{
			schedule = ts->toks[j + 1].str;
			at = ts->toks[j].off;
			rw_edit(rw, ts->toks[j].off,
					(j + 2 < ts->ntoks) ? ts->toks[j + 2].off : ts->srclen, " ");
			j++;
			continue;
		}

		if (tok_is(ts, j, "as"))
			break;
	}

	if (option == NULL)
		option = psprintf("gp.dynamic_schedule = %s",
						  quote_literal_cstr(schedule != NULL ? schedule : "*/5 * * * *"));

	/*
	 * Into the statement's one WITH list, with whatever else its clauses
	 * become (rw_place_options): a DISTRIBUTED BY or a TAG on the same
	 * statement goes there too.
	 */
	rw_add_option(rw, option, at);
	return true;
}

/*
 * ALTER USER u PROFILE p / NOPROFILE / ACCOUNT LOCK / ACCOUNT UNLOCK
 *	 -> ALTER USER u, carrying gp.profile = 'p' / gp.profile = DEFAULT /
 *		gp.account = 'lock' / gp.account = 'unlock' (GpAttachCarriers)
 *
 * These are role options in Cloudberry's grammar, which PostgreSQL's does
 * not have, so the statement is PostgreSQL's ALTER USER with nothing left in
 * it but what it carries: gp_security's ProcessUtility hook takes that out and
 * does it, and the statement answers ALTER ROLE, as Cloudberry's does.
 * NOPROFILE is the port's, for taking a profile away.
 *
 * Only on its own, as before: one of these mixed with PostgreSQL's own role
 * options is not taken here.
 */
static bool
rw_role_profile(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			i = rw->first;
	int			at;

	if (!tok_is(ts, i, "alter") ||
		!(tok_is(ts, i + 1, "user") || tok_is(ts, i + 1, "role")))
		return false;
	if (!tok_is_name(ts, i + 2))
		return false;

	i += 3;
	at = ts->toks[Min(i, ts->ntoks - 1)].off;

	if (tok_is(ts, i, "profile") && tok_is_name(ts, i + 1) && i + 2 == rw->last)
		rw_add_carrier(rw, "gp", "profile", tok_name(ts, i + 1), at);
	else if (tok_is(ts, i, "noprofile") && i + 1 == rw->last)
		rw_add_carrier(rw, "gp", "profile", NULL, at);
	else if (tok_is(ts, i, "account") && tok_is(ts, i + 1, "lock") && i + 2 == rw->last)
		rw_add_carrier(rw, "gp", "account", "lock", at);
	else if (tok_is(ts, i, "account") && tok_is(ts, i + 1, "unlock") && i + 2 == rw->last)
		rw_add_carrier(rw, "gp", "account", "unlock", at);
	else
		return false;

	/* The option goes; ALTER USER u stays, and PostgreSQL takes it as is. */
	rw_edit(rw, at, (rw->last < ts->ntoks) ? ts->toks[rw->last].off : ts->srclen, "");
	return true;
}

/*
 * CREATE ROLE ... [NO]CREATEEXTTABLE [(type = '...', protocol = '...')]
 * ALTER ROLE ... [NO]CREATEEXTTABLE [(...)], and USER and GROUP alike
 *	 -> the statement, carrying gp_exttable.exttabauth or exttabnoauth
 *
 * Cloudberry's right to make an external table of gpfdist or http, which
 * its pg_authid keeps in columns of its own (rolcreaterextgpfd and the
 * rest).  The option goes to the statement's parse node, whose options
 * gp_sql's hook takes it out of, and gp_exttable keeps it in the role's
 * label (option.c).  The value is the key-value list as written, "type=
 * readable,protocol=gpfdist", or empty.
 */
static bool
rw_role_exttable(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			i = rw->first;
	bool		did = false;

	if (!(tok_is(ts, i, "create") || tok_is(ts, i, "alter")) ||
		!(tok_is(ts, i + 1, "role") || tok_is(ts, i + 1, "user") ||
		  tok_is(ts, i + 1, "group")) ||
		!tok_is_name(ts, i + 2))
		return false;

	for (int j = i + 3; j < rw->last; j++)
	{
		bool		allow = tok_is(ts, j, "createexttable");
		StringInfoData value;
		int			end = j + 1;

		if (!allow && !tok_is(ts, j, "nocreateexttable"))
			continue;
		initStringInfo(&value);
		if (tok_is_char(ts, end, '('))
		{
			int			close = match_close(ts, end, rw->last);

			if (close < 0)
				rw_syntax_error(rw, rw->last);
			for (int k = end + 1; k < close; k += 4)
			{
				if (!tok_is_name(ts, k) || !tok_is_char(ts, k + 1, '=') ||
					!tok_is_string(ts, k + 2))
					rw_syntax_error(rw, k);
				appendStringInfo(&value, "%s%s=%s", value.len > 0 ? "," : "",
								 tok_name(ts, k), ts->toks[k + 2].str);
				if (k + 3 != close && !tok_is_char(ts, k + 3, ','))
					rw_syntax_error(rw, k + 3);
			}
			end = close + 1;
		}
		rw_add_carrier(rw, "gp_exttable", allow ? "exttabauth" : "exttabnoauth",
					   value.data, ts->toks[j].off);
		rw_edit(rw, ts->toks[j].off, tok_stop(ts, end - 1), "");
		j = end - 1;
		did = true;
	}
	return did;
}

/*
 * CREATE ROLE ... RESOURCE QUEUE q, RESOURCE GROUP g, and ALTER ROLE's, USER
 * and GROUP alike
 *	 -> the statement, carrying gp_resource.resource_queue = 'q' and
 *		resource_group = 'g'
 *
 * Cloudberry's pg_authid keeps them in rolresqueue and rolresgroup; the port
 * keeps them in the role's "gp" label, which gp_resource's ProcessUtility
 * hook writes, taking the options off the statement first (resdefs.c).
 */
static bool
rw_role_resource(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			i = rw->first;
	bool		did = false;

	if (!(tok_is(ts, i, "create") || tok_is(ts, i, "alter")) ||
		!(tok_is(ts, i + 1, "role") || tok_is(ts, i + 1, "user") ||
		  tok_is(ts, i + 1, "group")) ||
		!tok_is_name(ts, i + 2))
		return false;

	for (int j = i + 3; j + 1 < rw->last; j++)
	{
		if (!tok_is(ts, j, "resource") ||
			!(tok_is(ts, j + 1, "queue") || tok_is(ts, j + 1, "group")))
			continue;
		if (j + 2 >= rw->last || !tok_is_name(ts, j + 2))
			rw_syntax_error(rw, j + 2);
		rw_add_carrier(rw, "gp_resource",
					   tok_is(ts, j + 1, "queue") ? "resource_queue" : "resource_group",
					   tok_name(ts, j + 2), ts->toks[j].off);
		rw_edit(rw, ts->toks[j].off, tok_stop(ts, j + 2), "");
		j += 2;
		did = true;
	}
	return did;
}

/*
 * One point of a DENY clause at token k, DAY d [TIME 't'], appended to buf as
 * JSON: {"day": 2} or {"day": "Tuesday"}, and "time" when one was given.
 * Which of the two a day was written as matters -- DAY 2 is Tuesday, DAY '2'
 * no weekday -- so it is kept.  Returns the token after the point.
 */
static int
rw_deny_point(GpRewrite *rw, int k, StringInfo buf)
{
	const GpTokens *ts = rw->ts;

	if (!tok_is(ts, k, "day"))
		rw_syntax_error(rw, k);
	k++;
	appendStringInfoString(buf, "{\"day\": ");
	if (tok_is_string(ts, k))
		escape_json(buf, ts->toks[k].str);
	else if (k < rw->last && ts->toks[k].code == GP_ICONST)
		appendStringInfo(buf, "%d", ts->toks[k].ival);
	else
		rw_syntax_error(rw, k);
	k++;
	if (tok_is(ts, k, "time"))
	{
		if (!tok_is_string(ts, k + 1))
			rw_syntax_error(rw, k + 1);
		appendStringInfoString(buf, ", \"time\": ");
		escape_json(buf, ts->toks[k + 1].str);
		k += 2;
	}
	appendStringInfoChar(buf, '}');
	return k;
}

/*
 * CREATE ROLE ... DENY BETWEEN DAY d [TIME 't'] AND DAY d [TIME 't']
 * CREATE ROLE ... DENY DAY d [TIME 't']
 * ALTER ROLE ... DROP DENY FOR DAY d [TIME 't'], and USER and GROUP alike
 *	 -> the statement, carrying gp.deny = '[point, point]' or '[point]', or
 *		gp.drop_deny = '[point]'
 *
 * The times a role may not log in, which Cloudberry's pg_auth_time_constraint
 * keeps and gp_security keeps in the role's label (deny.c).  The clauses come
 * among the statement's other options, as many as it has, each going to the
 * parse node in the order it was written, where gp_security's hook takes them
 * out again.  DROP DENY is ALTER's only, as in Cloudberry's grammar.  A DENY
 * not followed by DAY or BETWEEN is not one of these -- a role may be called
 * deny -- and is left to PostgreSQL's grammar.
 */
static bool
rw_role_deny(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			i = rw->first;
	bool		altering;
	bool		did = false;

	if (tok_is(ts, i, "create"))
		altering = false;
	else if (tok_is(ts, i, "alter"))
		altering = true;
	else
		return false;
	if (!(tok_is(ts, i + 1, "role") || tok_is(ts, i + 1, "user") ||
		  tok_is(ts, i + 1, "group")) ||
		!tok_is_name(ts, i + 2))
		return false;

	for (int j = i + 3; j < rw->last; j++)
	{
		StringInfoData value;
		bool		drop;
		int			k;

		if (altering && tok_is(ts, j, "drop") && tok_is(ts, j + 1, "deny") &&
			tok_is(ts, j + 2, "for") && tok_is(ts, j + 3, "day"))
		{
			drop = true;
			k = j + 3;
		}
		else if (tok_is(ts, j, "deny") &&
				 (tok_is(ts, j + 1, "day") || tok_is(ts, j + 1, "between")))
		{
			drop = false;
			k = j + 1;
		}
		else
			continue;

		initStringInfo(&value);
		appendStringInfoChar(&value, '[');
		if (!drop && tok_is(ts, k, "between"))
		{
			k = rw_deny_point(rw, k + 1, &value);
			if (!tok_is(ts, k, "and"))
				rw_syntax_error(rw, k);
			appendStringInfoString(&value, ", ");
			k = rw_deny_point(rw, k + 1, &value);
		}
		else
			k = rw_deny_point(rw, k, &value);
		appendStringInfoChar(&value, ']');

		rw_add_carrier(rw, "gp", drop ? "drop_deny" : "deny", value.data,
					   ts->toks[j].off);
		rw_edit(rw, ts->toks[j].off, tok_stop(ts, k - 1), "");
		j = k - 1;
		did = true;
	}
	return did;
}

/* ------------------------------------------------------------------------- */
/* The classic partition clauses                                             */
/* ------------------------------------------------------------------------- */

/*
 * `text` in dollar quotes whose tag is in neither it nor where it ends, so
 * that the literal is the text exactly.
 */
static char *
dollar_quote(const char *text)
{
	int			len = strlen(text);

	for (int n = 0;; n++)
	{
		char	   *tag = (n == 0) ? pstrdup("$gp$") : psprintf("$gp%d$", n);
		char	   *body = psprintf("%s%s", text, tag);
		char	   *hit = strstr(body, tag);

		if (hit == body + len)
			return psprintf("%s%s", tag, body);
	}
}

/*
 * The access method a WITH list's legacy options name -- appendonly or
 * appendoptimized, and orientation -- as Cloudberry's grammar reads them
 * (greenplumLegacyAOoptions()): ao_row, ao_column, heap for appendonly
 * false, or NULL where they name none, or name it in a way this cannot
 * read, which gp_ao's hook reads again.  `open` is the list's parenthesis,
 * `close` the token after its end.
 */
static const char *
legacy_access_method(const GpTokens *ts, int open, int close)
{
	int			appendonly = -1;
	bool		column = false;

	for (int k = open + 1; k + 2 < close; k++)
	{
		char	   *v;

		if ((!tok_is_char(ts, k - 1, '(') && !tok_is_char(ts, k - 1, ',')) ||
			!tok_is_char(ts, k + 1, '='))
			continue;
		v = tok_is_string(ts, k + 2) ? ts->toks[k + 2].str :
			tok_is_name(ts, k + 2) ? tok_name(ts, k + 2) : NULL;
		if (v == NULL)
			continue;
		if (tok_is(ts, k, "appendonly") || tok_is(ts, k, "appendoptimized"))
		{
			if (pg_strcasecmp(v, "true") == 0 || pg_strcasecmp(v, "on") == 0 ||
				pg_strcasecmp(v, "t") == 0 || pg_strcasecmp(v, "yes") == 0)
				appendonly = 1;
			else if (pg_strcasecmp(v, "false") == 0 || pg_strcasecmp(v, "off") == 0 ||
					 pg_strcasecmp(v, "f") == 0 || pg_strcasecmp(v, "no") == 0)
				appendonly = 0;
			else
				return NULL;
		}
		else if (tok_is(ts, k, "orientation"))
			column = (pg_strcasecmp(v, "column") == 0);
	}
	if (appendonly == 1)
		return column ? "ao_column" : "ao_row";
	if (appendonly == 0)
		return "heap";
	return NULL;
}

/*
 * Where PostgreSQL's PARTITION BY goes in CREATE TABLE, the token at `i`
 * being the one after the table's name: after the column list, OF type and
 * its list, or PARTITION OF parent, its list and its bound, and INHERITS
 * (...).  -1 for a statement with none of those, CREATE TABLE AS.
 */
static int
table_structure_end(const GpTokens *ts, int i, int last)
{
	if (tok_is_char(ts, i, '('))
		i = skip_parens(ts, i);
	else if (tok_is_kw(ts, i, "of"))
	{
		i = skip_qualified_name(ts, i + 1);
		if (tok_is_char(ts, i, '('))
			i = skip_parens(ts, i);
	}
	else if (tok_is_kw(ts, i, "partition") && tok_is_kw(ts, i + 1, "of"))
	{
		i = skip_qualified_name(ts, i + 2);
		if (tok_is_char(ts, i, '('))
			i = skip_parens(ts, i);
		if (tok_is_kw(ts, i, "default"))
			i++;
		else if (tok_is_kw(ts, i, "for") && tok_is_kw(ts, i + 1, "values"))
		{
			i += 2;
			if (tok_is_kw(ts, i, "from"))
			{
				i = skip_parens(ts, i + 1);
				if (tok_is_kw(ts, i, "to"))
					i = skip_parens(ts, i + 1);
			}
			else
				i = skip_parens(ts, i + 1);		/* IN (...) or WITH (...) */
		}
		return Min(i, last);
	}
	else
		return -1;

	if (tok_is_kw(ts, i, "inherits") && tok_is_char(ts, i + 1, '('))
		i = skip_parens(ts, i + 1);
	return Min(i, last);
}

/*
 * CREATE TABLE t AS SELECT ... PARTITION BY ...: Cloudberry's grammar reads
 * the clause after the query, where PARTITION, reserved there, cannot be an
 * alias, and refuses it (gram.y's CreateAsStmt).  In PostgreSQL 19 PARTITION
 * is an alias there, and BY a syntax error; a PARTITION BY outside every
 * bracket of a query can only be the clause, so it is read, for its own
 * syntax errors, and refused as Cloudberry refuses it.
 */
static void
rw_ctas_partition_by(GpRewrite *rw, int after_name)
{
	const GpTokens *ts = rw->ts;
	GpPartParser p = {ts, rw->last, ts->src, 0, true};
	int			depth = 0;
	bool		query = false;

	for (int j = after_name; j < rw->last; j++)
	{
		if (tok_is_char(ts, j, '('))
			depth++;
		else if (tok_is_char(ts, j, ')'))
			depth--;
		if (depth != 0)
			continue;
		if (tok_is_kw(ts, j, "as"))
			query = true;
		if (query && tok_is_kw(ts, j, "partition") && tok_is_kw(ts, j + 1, "by"))
		{
			(void) GpPartParseClause(&p, j);
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot create a partitioned table using CREATE TABLE AS SELECT"),
					 errhint("Use CREATE TABLE...LIKE (followed by INSERT...SELECT) instead.")));
		}
	}
}

/*
 * CREATE TABLE ... PARTITION BY RANGE (d) [SUBPARTITION BY ...]
 *	   (START (...) END (...) EVERY (...), DEFAULT PARTITION other)
 *	 -> CREATE TABLE ... PARTITION BY RANGE (d)
 *		  WITH (gp.partition_by = $gp$PARTITION BY RANGE (d) ... other)$gp$)
 *
 * Cloudberry's classic partition clause, which gp_sql's partition.c turns
 * into the partitions once the table exists.  PostgreSQL's grammar keeps the
 * table's own key -- PARTITION BY RANGE (d) is PostgreSQL's too -- and the
 * rest of the clause, which is not, is carried whole and as written in one
 * option of the statement: one statement for one, as the clauses of CREATE
 * TABLE are.  The option stands for the clause's start in the user's text,
 * so that a position in it is a position there.
 *
 * The clause is parsed here as well (gp_partition.c), so that where it ends
 * is known and its syntax errors are raised when the statement is parsed, at
 * the token Cloudberry's grammar raises them at.
 *
 * Cloudberry takes PARTITION BY where PostgreSQL has it and at the end of the
 * statement, after DISTRIBUTED BY (gram.y's OptFirstPartitionSpec and
 * OptSecondPartitionSpec).  At the end, the key moves to where PostgreSQL
 * has it -- which a PostgreSQL PARTITION BY there, with no partition list,
 * needs too.
 */
static void
rw_partition_by(GpRewrite *rw, int after_name)
{
	const GpTokens *ts = rw->ts;
	GpPartParser p = {ts, rw->last, ts->src, 0, true};
	GpPartClause *clause = NULL;
	int			clause_at = -1;
	int			pg_at;
	int			depth = 0;
	int			key_from;
	int			key_to;
	int			end;

	if (!tok_is_kw(ts, rw->first, "create") || rw->object != 't')
		return;

	/* CREATE TABLE ... AS: an AS outside every bracket */
	for (int j = after_name; j < rw->last; j++)
	{
		if (tok_is_char(ts, j, '('))
			depth++;
		else if (tok_is_char(ts, j, ')'))
			depth--;
		else if (depth == 0 && tok_is_kw(ts, j, "as"))
		{
			rw_ctas_partition_by(rw, after_name);
			return;
		}
	}

	pg_at = table_structure_end(ts, after_name, rw->last);
	if (pg_at < 0)
		return;

	depth = 0;
	for (int j = pg_at; j < rw->last; j++)
	{
		GpPartClause *c;

		if (tok_is_char(ts, j, '('))
			depth++;
		else if (tok_is_char(ts, j, ')'))
			depth--;
		if (depth != 0 || !tok_is_kw(ts, j, "partition") ||
			!tok_is_kw(ts, j + 1, "by"))
			continue;

		c = GpPartParseClause(&p, j);
		if (clause != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("only one PARTITION BY clause is allowed"),
					 errposition(pg_mbstrlen_with_len(ts->src, ts->toks[j].off) + 1)));
		clause = c;
		clause_at = j;
		j = c->end - 1;
	}

	/* PostgreSQL's own, where PostgreSQL has it: nothing to do */
	if (clause == NULL || (clause->def == NULL && clause_at == pg_at))
		return;

	key_from = ts->toks[clause_at].off;
	key_to = tok_stop(ts, clause->key_end - 1);
	end = tok_stop(ts, clause->end - 1);

	if (clause->def != NULL)
		rw_add_option(rw, psprintf("gp.%s = %s", GP_PARTITION_BY_OPTION,
								   dollar_quote(pnstrdup(ts->src + key_from,
														 end - key_from))),
					  key_from);

	if (clause_at == pg_at)
	{
		/* the key is where PostgreSQL has it; the rest goes */
		rw_edit(rw, key_to, end, "");
	}
	else
	{
		/* the key moves there, as the user wrote it */
		GpOut	   *piece = palloc(sizeof(GpOut));
		int			at = (pg_at < ts->ntoks) ? ts->toks[pg_at].off : ts->srclen;

		out_init(piece, ts->src);
		out_text(piece, " ", key_from);
		out_copy(piece, key_from, key_to);
		out_text(piece, " ", key_from);
		rw_edit_piece(rw, at, at, piece);
		rw_edit(rw, key_from, end, " ");
	}
}

/*
 * ALTER TABLE t ADD PARTITION ..., DROP PARTITION ..., ALTER PARTITION ...,
 * EXCHANGE, RENAME, SPLIT and TRUNCATE PARTITION, SET SUBPARTITION TEMPLATE
 *	 -> ALTER TABLE t SET (gp.partition_cmd = $gp$ADD PARTITION ...$gp$)
 *
 * Each of Cloudberry's partition commands becomes an option of ALTER TABLE's
 * SET, in place among the statement's other commands, as TAG does; gp_sql's
 * partition.c takes it out and does what it says once the rest has run.  The
 * option stands for the command in the user's text.  A command is parsed here
 * too, for where it ends and for its syntax errors.
 *
 * Four of them can be PostgreSQL's as well -- ADD, DROP, ALTER and RENAME of
 * a column called "partition" -- and are taken only where PostgreSQL's
 * grammar would refuse them (GpPartIsCmd).
 */
static void
rw_partition_cmds(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	GpPartParser p = {ts, rw->last, ts->src, 0, true};
	int			i = rw->first;
	int			e;

	if (!tok_is_kw(ts, i, "alter"))
		return;
	/* ALTER EXTERNAL TABLE and ALTER FOREIGN TABLE take the same commands */
	if (tok_is(ts, i + 1, "external") || tok_is_kw(ts, i + 1, "foreign"))
		i++;
	if (!tok_is_kw(ts, i + 1, "table"))
		return;
	i += 2;
	if (tok_is_kw(ts, i, "if") && tok_is_kw(ts, i + 1, "exists"))
		i += 2;
	if (tok_is_kw(ts, i, "only"))
	{
		i++;
		if (tok_is_char(ts, i, '('))
			i++;
	}
	e = skip_qualified_name(ts, i);
	if (e == i)
		return;
	i = e;
	if (tok_is_char(ts, i, ')') || tok_is_char(ts, i, '*'))
		i++;

	/* its commands, comma-separated */
	while (i < rw->last)
	{
		/*
		 * SET DISTRIBUTED BY (a) / RANDOMLY / REPLICATED, and SET WITH
		 * (REORGANIZE [= true|false]) before one or alone:
		 *	 -> SET (gp.distributed_by = '(a)', gp.reorganize = 'true')
		 * which gp_sql's distribution.c takes out and carries out: the policy,
		 * and on a cluster the rows moved to where it puts them.
		 */
		if (tok_is_kw(ts, i, "set") &&
			(tok_is(ts, i + 1, "distributed") ||
			 (tok_is_kw(ts, i + 1, "with") && tok_is_char(ts, i + 2, '(') &&
			  tok_is(ts, i + 3, "reorganize"))))
		{
			int			from = ts->toks[i].off;
			int			j = i + 1;
			char	   *reorganize = NULL;
			char	   *policy;
			StringInfoData opts;

			if (tok_is_kw(ts, j, "with"))
			{
				int			close = skip_parens(ts, j + 1);

				/*
				 * WITH (REORGANIZE [= value]): alone, true, as a boolean
				 * option is; and alone in the list, as Cloudberry's
				 * ATExecSetDistributedBy() says.
				 */
				for (int k = j + 2; k < close - 1; k++)
				{
					char	   *v;

					if (tok_is_char(ts, k - 1, '(') || tok_is_char(ts, k - 1, ','))
					{
						if (!tok_is(ts, k, "reorganize"))
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("reorganize isn't supported with other options in SET WITH")));
						if (reorganize != NULL)
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("cannot specify more than one option in WITH clause")));
						reorganize = "true";
						continue;
					}
					if (tok_is_char(ts, k, '=') || tok_is_char(ts, k, ','))
						continue;
					/* true or 'true', on or 't': a boolean, however spelled */
					v = rw_text(ts, k, k + 1);
					if (v[0] == '\'')
						v = pnstrdup(v + 1, strlen(v) - 2);
					reorganize = (pg_strcasecmp(v, "true") == 0 ||
								  pg_strcasecmp(v, "on") == 0 ||
								  pg_strcasecmp(v, "t") == 0) ? "true" : "false";
				}
				j = close;
			}
			policy = distributed_policy(ts, j, &e);
			if (policy == NULL && reorganize == NULL)
				break;
			if (policy != NULL)
				j = e;

			initStringInfo(&opts);
			if (policy != NULL)
				appendStringInfo(&opts, "gp.distributed_by = %s",
								 quote_literal_cstr(policy));
			if (reorganize != NULL)
				appendStringInfo(&opts, "%sgp.reorganize = %s",
								 policy != NULL ? ", " : "",
								 quote_literal_cstr(reorganize));
			rw_edit(rw, from, tok_stop(ts, j - 1),
					psprintf("SET (%s)", opts.data));
			i = j;
		}
		/*
		 * EXPAND TABLE, EXPAND PARTITION PREPARE, SHRINK TABLE TO n:
		 *	 -> SET (gp.expand = 'table' | 'partition prepare'),
		 *		SET (gp.shrink = 'n')
		 * which gp_sql's distribution.c carries out: the table spread over
		 * every segment, or over the first n, its rows moved with it.
		 */
		else if (tok_is_word(ts, i, "expand") &&
				 (tok_is_kw(ts, i + 1, "table") ||
				  (tok_is_kw(ts, i + 1, "partition") && tok_is(ts, i + 2, "prepare"))))
		{
			bool		table = tok_is_kw(ts, i + 1, "table");
			int			last = table ? i + 1 : i + 2;

			rw_edit(rw, ts->toks[i].off, tok_stop(ts, last),
					psprintf("SET (gp.expand = '%s')",
							 table ? "table" : "partition prepare"));
			i = last + 1;
		}
		else if (tok_is_word(ts, i, "shrink") && tok_is_kw(ts, i + 1, "table") &&
				 tok_is_kw(ts, i + 2, "to"))
		{
			int			j = i + 3;
			bool		negative = false;

			if (tok_is_char(ts, j, '-') || tok_is_char(ts, j, '+'))
				negative = tok_is_char(ts, j++, '-');
			if (j >= rw->last || ts->toks[j].code != GP_ICONST)
				break;
			rw_edit(rw, ts->toks[i].off, tok_stop(ts, j),
					psprintf("SET (gp.shrink = '%s%d')", negative ? "-" : "",
							 ts->toks[j].ival));
			i = j + 1;
		}
		/*
		 * SET WITH (appendonly=true, orientation=column, compresslevel=5):
		 * a change of access method in Cloudberry's legacy spelling, with the
		 * options the table takes in its new one; and SET ACCESS METHOD m
		 * WITH (...), Cloudberry's own, the same with the method named:
		 *	 -> SET (gp_ao.set_with = $gp$...$gp$)
		 *	 -> SET ACCESS METHOD m, SET (gp_ao.am_with = $gp$...$gp$)
		 * which gp_ao's ProcessUtility hook takes out and carries out.  WITH
		 * (REORGANIZE) is the distribution's, above.
		 */
		else if (tok_is_kw(ts, i, "set") && tok_is_kw(ts, i + 1, "with") &&
				 tok_is_char(ts, i + 2, '(') &&
				 !tok_is_word(ts, skip_parens(ts, i + 2), "distributed"))
		{
			int			close = skip_parens(ts, i + 2);
			bool		am = false;

			for (int k = i + 3; k < close - 1; k++)
			{
				if (!tok_is_char(ts, k - 1, '(') && !tok_is_char(ts, k - 1, ','))
					continue;
				if (tok_is(ts, k, "reorganize"))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("reorganize isn't supported with other options in SET WITH")));
				if (tok_is(ts, k, "appendonly") || tok_is(ts, k, "appendoptimized"))
					am = true;
			}
			if (!am)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("invalid storage type"),
						 errposition(pg_mbstrlen_with_len(ts->src,
														  ts->toks[i + 2].off) + 1)));
			rw_edit(rw, ts->toks[i].off, tok_stop(ts, close - 1),
					psprintf("SET (gp_ao.set_with = %s)",
							 dollar_quote(pnstrdup(ts->src + ts->toks[i + 3].off,
												   ts->toks[close - 1].off -
												   ts->toks[i + 3].off))));
			i = close;
		}
		else if (tok_is_kw(ts, i, "set") && tok_is_kw(ts, i + 1, "access") &&
				 tok_is_kw(ts, i + 2, "method") && tok_is_name(ts, i + 3) &&
				 tok_is_kw(ts, i + 4, "with") && tok_is_char(ts, i + 5, '('))
		{
			int			close = skip_parens(ts, i + 5);
			const char *witham = legacy_access_method(ts, i + 5, close);
			char	   *am = tok_name(ts, i + 3);

			/* Cloudberry's grammar: the options name the method named */
			if (witham != NULL && strcmp(witham, am) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("ACCESS METHOD is specified as \"%s\" but the WITH option indicates it to be \"%s\"",
								am, witham),
						 errposition(pg_mbstrlen_with_len(ts->src,
														  ts->toks[i + 4].off) + 1)));

			rw_edit(rw, ts->toks[i + 4].off, tok_stop(ts, close - 1),
					psprintf(", SET (gp_ao.am_with = %s)",
							 dollar_quote(pnstrdup(ts->src + ts->toks[i + 6].off,
												   ts->toks[close - 1].off -
												   ts->toks[i + 6].off))));
			i = close;
		}
		else if (GpPartIsCmd(ts, i, rw->last))
		{
			int			from = ts->toks[i].off;
			int			to;

			(void) GpPartParseCmd(&p, i, &e);
			to = tok_stop(ts, e - 1);
			rw_edit(rw, from, to,
					psprintf("SET (gp.%s = %s)", GP_PARTITION_CMD_OPTION,
							 dollar_quote(pnstrdup(ts->src + from, to - from))));
			i = e;
		}
		else
		{
			int			depth = 0;

			for (; i < rw->last; i++)
			{
				if (tok_is_char(ts, i, '(') || tok_is_char(ts, i, '['))
					depth++;
				else if (tok_is_char(ts, i, ')') || tok_is_char(ts, i, ']'))
					depth--;
				else if (depth == 0 && tok_is_char(ts, i, ','))
					break;
			}
		}
		if (!tok_is_char(ts, i, ','))
			break;
		i++;
	}
}

/* ------------------------------------------------------------------------- */
/* Function attributes: where a function may run, and what it does with SQL   */
/* ------------------------------------------------------------------------- */

/*
 * The head of CREATE [OR REPLACE] FUNCTION|PROCEDURE, or of ALTER FUNCTION|
 * PROCEDURE: returns where the option list can begin, which is after the
 * name and the parenthesised parameters, or -1 when this is neither.
 */
static int
rw_function_head(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			i = rw->first;
	int			name_first;

	if (tok_is(ts, i, "create"))
	{
		i++;
		if (tok_is(ts, i, "or") && tok_is(ts, i + 1, "replace"))
			i += 2;
	}
	else if (tok_is(ts, i, "alter"))
		i++;
	else
		return -1;

	if (!tok_is(ts, i, "function") && !tok_is(ts, i, "procedure"))
		return -1;
	i++;

	name_first = i;
	i = skip_qualified_name(ts, i);
	if (i == name_first)
		return -1;

	/* ALTER FUNCTION f EXECUTE ON ANY: PostgreSQL allows a bare name. */
	if (tok_is_char(ts, i, '('))
		i = skip_parens(ts, i);

	return i;
}

/*
 * EXECUTE ON ANY | COORDINATOR | MASTER | INITPLAN | ALL SEGMENTS, and the
 * data-access attributes NO SQL | CONTAINS SQL | READS SQL DATA | MODIFIES
 * SQL DATA, on CREATE [OR REPLACE] FUNCTION and PROCEDURE and on ALTER
 * FUNCTION and PROCEDURE, each becoming an option of the statement, in place:
 *
 *	 CREATE FUNCTION f(int) RETURNS SETOF int ... EXECUTE ON ALL SEGMENTS
 *	   -> CREATE FUNCTION f(int) RETURNS SETOF int ...
 *			SET gp.execute_on = 'all_segments'
 *	 ALTER FUNCTION f(int) READS SQL DATA
 *	   -> ALTER FUNCTION f(int) SET gp.data_access = 'reads'
 *
 * SET is the one option a function takes whose name is anybody's, and
 * gp_sql's ProcessUtility hook takes these out before PostgreSQL would store
 * them as settings, and writes them as the keys of the function's "gp" label
 * -- execute_on, which func_exec_location() reads and ORCA asks of every
 * function it meets, and data_access -- once the function exists.  They were
 * a SECURITY LABEL after the statement, which made CREATE FUNCTION two
 * statements, and which, replacing the whole label, took the other key away
 * when an ALTER set one.
 *
 * WHAT DATA ACCESS IS FOR.  Nothing reads it, and that is not a gap in the
 * port: Cloudberry writes pg_proc.prodataaccess, dumps it, and reads it
 * nowhere outside the DDL path -- no planner, executor or dispatcher decision
 * turns on it.  Its whole observable behaviour is Cloudberry's rules for it,
 * which gp_sql checks against the function as the statement leaves it; so
 * are EXECUTE ON's (see gp_sql's funcattr.c).
 */
static bool
rw_function_clauses(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	static const struct
	{
		const char *word;		/* the word after EXECUTE ON */
		const char *second;		/* and the one after that, or NULL */
		const char *value;		/* what the label records */
	}			locations[] = {
		{"any", NULL, "any"},
		{"coordinator", NULL, "coordinator"},
		{"master", NULL, "coordinator"},
		{"initplan", NULL, "initplan"},
		{"all", "segments", "all_segments"},
	};
	static const struct
	{
		const char *w1;
		const char *w2;
		const char *w3;			/* or NULL */
		const char *value;
	}			accesses[] = {
		{"no", "sql", NULL, "none"},
		{"contains", "sql", NULL, "contains"},
		{"reads", "sql", "data", "reads"},
		{"modifies", "sql", "data", "modifies"},
	};
	int			options_from = rw_function_head(rw);
	int			depth = 0;
	bool		found = false;

	if (options_from < 0)
		return false;

	/*
	 * One pass over the option list.  It is written among the function's
	 * other options, which is depth 0; a SQL-standard body is where real SQL
	 * tokens start and nothing of Cloudberry's follows, so stop there.
	 */
	for (int j = options_from; j < rw->last; j++)
	{
		int			after = -1;
		const char *setting = NULL;
		const char *value = NULL;

		if (tok_is_char(ts, j, '('))
		{
			depth++;
			continue;
		}
		if (tok_is_char(ts, j, ')'))
		{
			depth--;
			continue;
		}
		if (depth != 0)
			continue;
		if (tok_is(ts, j, "begin"))
			break;

		if (tok_is(ts, j, "execute") && tok_is(ts, j + 1, "on"))
		{
			for (size_t k = 0; k < lengthof(locations); k++)
			{
				if (!tok_is(ts, j + 2, locations[k].word))
					continue;
				if (locations[k].second != NULL &&
					!tok_is(ts, j + 3, locations[k].second))
					continue;

				after = j + 3 + (locations[k].second != NULL ? 1 : 0);
				setting = "gp.execute_on";
				value = locations[k].value;
				break;
			}
		}
		else
		{
			for (size_t k = 0; k < lengthof(accesses); k++)
			{
				if (!tok_is(ts, j, accesses[k].w1) ||
					!tok_is(ts, j + 1, accesses[k].w2))
					continue;
				if (accesses[k].w3 != NULL && !tok_is(ts, j + 2, accesses[k].w3))
					continue;

				after = j + 2 + (accesses[k].w3 != NULL ? 1 : 0);
				setting = "gp.data_access";
				value = accesses[k].value;
				break;
			}
		}

		if (setting == NULL)
			continue;

		rw_edit(rw, ts->toks[j].off, tok_stop(ts, after - 1),
				psprintf("SET %s = %s", setting, quote_literal_cstr(value)));
		found = true;
		j = after - 1;
	}

	return found;
}

/* ------------------------------------------------------------------------- */
/* Expressions: DECODE, and CASE x WHEN IS NOT DISTINCT FROM y               */
/* ------------------------------------------------------------------------- */

/*
 * The two expressions of Cloudberry's that PostgreSQL 19's grammar does not
 * have, and what both become:
 *
 *	 DECODE(x, a, r [, b, s ...] [, d])
 *	 CASE x WHEN IS NOT DISTINCT FROM a THEN r ... [ELSE d] END
 *	   -> CASE WHEN (x) IS NOT DISTINCT FROM (a) THEN r ... [ELSE d] END
 *
 * Cloudberry's parser makes each a CASE with x as its operand, which its
 * parse analysis compares with IS NOT DISTINCT FROM rather than = (gram.y's
 * when_operand and decode_expr, parse_expr.c's transformCaseExpr).
 * PostgreSQL 19 compares a CASE's operand with = and nothing else
 * (pg19/src/backend/parser/parse_expr.c:1701-1705), so the only CASE that
 * can say this is a searched one, with x written into each arm.  That is the
 * one difference that shows: x is evaluated for each arm tried rather than
 * once, which costs nothing for a column and is visible for a volatile
 * function.  A fork of the grammar could not do better; the comparison is
 * made in parse analysis, which no grammar reaches.
 *
 * In a CASE with such an arm, every arm is rewritten, the others as
 * WHEN (x) = (a): the = Cloudberry's parse analysis would have made of them.
 * A CASE with none is PostgreSQL's own and is left as it is.
 *
 * DECODE is reserved in Cloudberry's grammar, and with two arguments it is
 * PostgreSQL's decode(text, text) there too (gram.y:19481).  With three or
 * more it is the CASE even where a function called decode takes them, which
 * Cloudberry's case_gp test checks; "decode"(...) and s.decode(...) call the
 * function, as they do there.  Where PostgreSQL 19 lets decode be a name that
 * Cloudberry did not -- a table, an alias, a function being created --
 * decode_is_call() leaves it one.
 *
 * Positions.  What is written here stands for the token Cloudberry's grammar
 * would have given the node it becomes, so an error in it is reported where
 * Cloudberry reports it: IS NOT DISTINCT FROM at the arm's NOT, or for DECODE
 * at the value compared; the = of an ordinary arm at its WHEN.
 */

typedef struct GpExprScan
{
	const GpTokens *ts;
	int			first;			/* first token of the text being rewritten */
	int			last;			/* one past its last */
	bool		create_index;	/* CREATE INDEX, where ON names a table */
	bool		alter_table;	/* ALTER TABLE, where USING is an expression's */
} GpExprScan;

static int	construct_at(const GpExprScan *sc, int i, int limit);
static void emit_construct(GpOut *o, const GpExprScan *sc, int i, int stop);

/*
 * The user's text from byte `from` to byte `to`, which covers tokens
 * [tfrom, tto), with every construct among those tokens rewritten.
 */
static void
emit_bytes(GpOut *o, const GpExprScan *sc, int from, int to, int tfrom, int tto)
{
	int			copied = from;

	for (int j = tfrom; j < tto; j++)
	{
		int			stop = construct_at(sc, j, tto);

		if (stop < 0)
			continue;
		out_copy(o, copied, sc->ts->toks[j].off);
		emit_construct(o, sc, j, stop);
		copied = tok_stop(sc->ts, stop);
		j = stop;
	}
	out_copy(o, copied, to);
}

/* Tokens [from, to), from the first one's start to the last one's end. */
static void
emit_span(GpOut *o, const GpExprScan *sc, int from, int to)
{
	if (from < to)
		emit_bytes(o, sc, sc->ts->toks[from].off, tok_stop(sc->ts, to - 1),
				   from, to);
}

/*
 * Is the decode( at `i` Cloudberry's DECODE, or is decode a name there?
 *
 * In Cloudberry it is always DECODE, a reserved word.  In PostgreSQL 19 it is
 * a name, and a name followed by a parenthesised list is not always a call:
 * CREATE TABLE decode (a int, ...), INSERT INTO decode (a, ...), a CTE or an
 * alias with a column list, CREATE FUNCTION decode(...), a type with
 * modifiers.  Those follow a name, a literal or a closing bracket, or a
 * keyword that introduces a name; a call follows an operator, an opening
 * bracket, a comma, or a keyword that introduces an expression, which is the
 * list below.  A statement that begins at an expression -- PL/pgSQL hands the
 * parser the expression alone -- begins with one.
 *
 * Two keywords introduce an expression only in one statement or position:
 * USING in ALTER TABLE ... ALTER COLUMN ... TYPE ... USING, and ON everywhere
 * but CREATE INDEX, where it names the table.  And one closing bracket is
 * followed by an expression: SELECT DISTINCT ON (...)'s.
 */
static bool
decode_is_call(const GpExprScan *sc, int i, int close)
{
	static const char *const leads[] = {
		"select", "where", "having", "returning", "return", "by", "on",
		"and", "or", "not", "case", "when", "then", "else", "distinct", "all",
		"like", "ilike", "similar", "to", "escape", "between", "symmetric",
		"asymmetric", "overlaps", "in", "from", "for", "placing", "at",
		"zone", "default", "limit", "offset", "first", "next", "rows",
		"range", "groups", "leading", "trailing", "both", "variadic",
		"lateral", "document", "content", "passing", "ref", "value",
		NULL
	};
	const GpTokens *ts = sc->ts;
	const GpTok *p;

	if (i == sc->first)
		return true;

	p = &ts->toks[i - 1];

	if (p->kw != NULL)
	{
		/* CREATE INDEX ... ON decode (a, b, c) names the table */
		if (pg_strcasecmp(p->kw, "on") == 0 && sc->create_index)
			return false;
		if (pg_strcasecmp(p->kw, "using") == 0)
			return sc->alter_table;
		for (int k = 0; leads[k] != NULL; k++)
			if (pg_strcasecmp(p->kw, leads[k]) == 0)
				return true;
		return false;
	}

	/* SELECT DISTINCT ON (a) decode(...) */
	if (p->code == ')')
	{
		int			depth = 0;

		for (int j = i - 1; j > sc->first; j--)
		{
			if (tok_is_char(ts, j, ')'))
				depth++;
			else if (tok_is_char(ts, j, '(') && --depth == 0)
				return tok_is_kw(ts, j - 1, "on") && tok_is_kw(ts, j - 2, "distinct");
		}
		return false;
	}

	switch (p->code)
	{
		case ',':
			/* WITH a AS (...), decode (x, y, z) AS (...) names a query */
			if (tok_is_kw(ts, close + 1, "as") &&
				(tok_is_char(ts, close + 2, '(') ||
				 tok_is_kw(ts, close + 2, "materialized") ||
				 tok_is_kw(ts, close + 2, "not")))
				return false;
			return true;
		case '(':
		case '[':
		case ':':
		case '+':
		case '-':
		case '*':
		case '/':
		case '%':
		case '^':
		case '<':
		case '>':
		case '=':
		case GP_OP:
		case GP_LESS_EQUALS:
		case GP_GREATER_EQUALS:
		case GP_NOT_EQUALS:
		case GP_COLON_EQUALS:
		case GP_EQUALS_GREATER:
			return true;
		default:
			return false;
	}
}

/*
 * DECODE(...) at `i`: its closing parenthesis, or -1 if it is not DECODE --
 * decode spelled with quotes or a schema, fewer than three arguments, or a
 * name rather than a call.
 */
static int
decode_close(const GpExprScan *sc, int i, int limit)
{
	const GpTokens *ts = sc->ts;
	const GpTok *t = &ts->toks[i];
	int			close;
	int			nargs = 0;
	int			depth = 0;

	if (t->code != GP_IDENT || strcmp(t->str, "decode") != 0 ||
		ts->src[t->off] == '"' || !tok_is_char(ts, i + 1, '('))
		return -1;

	close = match_close(ts, i + 1, limit);
	if (close < 0)
		return -1;

	if (close > i + 2)
	{
		nargs = 1;
		for (int j = i + 2; j < close; j++)
		{
			int			c = ts->toks[j].code;

			if (c == '(' || c == '[')
				depth++;
			else if (c == ')' || c == ']')
				depth--;
			else if (c == ',' && depth == 0)
				nargs++;
		}
	}

	if (nargs < 3 || !decode_is_call(sc, i, close))
		return -1;

	return close;
}

/*
 * DECODE(x, a, r [, b, s ...] [, d]) -> CASE WHEN (x) IS NOT DISTINCT FROM
 * (a) THEN r ... [ELSE d] END.  An even count of arguments ends in a default.
 */
static void
emit_decode(GpOut *o, const GpExprScan *sc, int i, int close)
{
	const GpTokens *ts = sc->ts;
	int			nargs = 0;
	int		   *from = palloc((close - i) * sizeof(int));
	int		   *to = palloc((close - i) * sizeof(int));
	int			depth = 0;

	/* the arguments, as token ranges */
	from[0] = i + 2;
	for (int j = i + 2; j < close; j++)
	{
		int			c = ts->toks[j].code;

		if (c == '(' || c == '[')
			depth++;
		else if (c == ')' || c == ']')
			depth--;
		else if (c == ',' && depth == 0)
		{
			to[nargs++] = j;
			from[nargs] = j + 1;
		}
	}
	to[nargs++] = close;

	out_text(o, "CASE", ts->toks[i].off);
	for (int k = 1; k + 1 < nargs; k += 2)
	{
		/* Cloudberry's IS NOT DISTINCT FROM is at the value compared */
		int			at = ts->toks[from[k]].off;

		out_text(o, " WHEN (", at);
		emit_span(o, sc, from[0], to[0]);
		out_text(o, ") IS NOT DISTINCT FROM (", at);
		emit_span(o, sc, from[k], to[k]);
		out_text(o, ") THEN ", ts->toks[from[k + 1]].off);
		emit_span(o, sc, from[k + 1], to[k + 1]);
	}
	if (nargs % 2 == 0)
	{
		out_text(o, " ELSE ", ts->toks[from[nargs - 1]].off);
		emit_span(o, sc, from[nargs - 1], to[nargs - 1]);
	}
	out_text(o, " END", ts->toks[close].off);
}

/* One WHEN of a CASE: the tokens WHEN, THEN, and what follows its result. */
typedef struct GpCaseArm
{
	int			when;
	int			then;			/* -1 if it has none, which PostgreSQL refuses */
	int			stop;			/* the next WHEN, the ELSE, or the END */
} GpCaseArm;

/*
 * The CASE at `i`: its END, and its arms, at its own level -- inside no
 * bracket and no CASE of their own.  Returns the number of arms; *else_at is
 * its ELSE, or -1.  -1 if the CASE does not end before `limit`.
 */
static int
case_arms(const GpExprScan *sc, int i, int limit, int *end, int *else_at,
		  GpCaseArm **arms)
{
	const GpTokens *ts = sc->ts;
	int			depth = 0;
	int			cases = 0;
	int			narms = 0;
	int			maxarms = 8;

	*end = -1;
	*else_at = -1;
	*arms = palloc(maxarms * sizeof(GpCaseArm));

	for (int j = i + 1; j < limit; j++)
	{
		int			c = ts->toks[j].code;

		if (c == '(' || c == '[')
		{
			depth++;
			continue;
		}
		if (c == ')' || c == ']')
		{
			if (--depth < 0)
				return -1;
			continue;
		}
		if (depth != 0)
			continue;

		if (tok_is_kw(ts, j, "case"))
			cases++;
		else if (tok_is_kw(ts, j, "end"))
		{
			if (cases-- > 0)
				continue;
			if (narms > 0 && (*arms)[narms - 1].stop < 0)
				(*arms)[narms - 1].stop = j;
			*end = j;
			return narms;
		}
		else if (cases > 0)
			continue;
		else if (tok_is_kw(ts, j, "when") && *else_at < 0)
		{
			if (narms > 0)
				(*arms)[narms - 1].stop = j;
			if (narms == maxarms)
			{
				maxarms *= 2;
				*arms = repalloc(*arms, maxarms * sizeof(GpCaseArm));
			}
			(*arms)[narms].when = j;
			(*arms)[narms].then = -1;
			(*arms)[narms].stop = -1;
			narms++;
		}
		else if (tok_is_kw(ts, j, "then") && narms > 0 &&
				 (*arms)[narms - 1].then < 0 && *else_at < 0)
			(*arms)[narms - 1].then = j;
		else if (tok_is_kw(ts, j, "else") && narms > 0 && *else_at < 0)
		{
			(*arms)[narms - 1].stop = j;
			*else_at = j;
		}
	}

	return -1;
}

/*
 * Is this arm Cloudberry's WHEN IS ...?  IS where an expression starts can
 * only begin one, or else be the name of a function or type, is(...) or
 * is 'literal', which PostgreSQL's grammar takes as it takes any other.
 */
static bool
arm_is_cloudberrys(const GpTokens *ts, const GpCaseArm *arm)
{
	int			j = arm->when + 1;

	return tok_is_kw(ts, j, "is") && !tok_is_char(ts, j + 1, '(') &&
		!tok_is_string(ts, j + 1);
}

/* CASE x WHEN IS NOT DISTINCT FROM ... at `i`: its END, or -1. */
static int
case_close(const GpExprScan *sc, int i, int limit)
{
	GpCaseArm  *arms;
	int			end;
	int			else_at;
	int			narms;

	if (!tok_is_kw(sc->ts, i, "case"))
		return -1;

	narms = case_arms(sc, i, limit, &end, &else_at, &arms);

	/* a searched CASE, or not one PostgreSQL's grammar would finish */
	if (narms <= 0 || arms[0].when == i + 1)
		return -1;

	for (int k = 0; k < narms; k++)
		if (arm_is_cloudberrys(sc->ts, &arms[k]))
			return end;

	return -1;
}

/*
 * CASE x WHEN ... END, with an IS NOT DISTINCT FROM arm, as a searched CASE.
 *
 * An arm that is not well formed is written so that PostgreSQL's grammar
 * stops at the token Cloudberry's does, and with the position map the error
 * is reported there: after WHEN IS NOT, Cloudberry wants DISTINCT and then
 * FROM, so the arm becomes (x) IS NOT DISTINCT followed by the rest of it,
 * and PostgreSQL wants FROM exactly where Cloudberry wanted what was
 * missing; after WHEN IS with no NOT, the arm stays as it was, and
 * PostgreSQL stops at the same word Cloudberry does, IS being no expression.
 */
static void
emit_case(GpOut *o, const GpExprScan *sc, int i, int end)
{
	const GpTokens *ts = sc->ts;
	GpCaseArm  *arms;
	int			else_at;
	int			narms;
	int			opfrom = i + 1;
	int			opto;

	narms = case_arms(sc, i, end + 1, &end, &else_at, &arms);
	Assert(narms > 0);
	opto = arms[0].when;

	/* CASE, without its operand */
	out_text(o, "CASE ", ts->toks[i].off);

	for (int k = 0; k < narms; k++)
	{
		const GpCaseArm *arm = &arms[k];
		int			w = arm->when;
		int			cond_to = (arm->then >= 0) ? arm->then : arm->stop;
		int			rest;		/* first token after what was rewritten */

		/* WHEN, and whatever follows it up to the condition */
		out_copy(o, ts->toks[w].off, ts->toks[w + 1].off);

		if (arm_is_cloudberrys(ts, arm) && !tok_is_kw(ts, w + 2, "not"))
		{
			/* WHEN IS <no NOT>: as written, for PostgreSQL to refuse */
			rest = w + 1;
		}
		else if (arm_is_cloudberrys(ts, arm))
		{
			int			not_at = ts->toks[w + 2].off;
			int			j = w + 3;
			bool		full = false;

			if (tok_is_kw(ts, j, "distinct"))
			{
				j++;
				if (tok_is_kw(ts, j, "from"))
				{
					j++;
					full = (j < cond_to);
				}
			}

			out_text(o, "(", not_at);
			emit_span(o, sc, opfrom, opto);
			if (full)
			{
				out_text(o, ") IS NOT DISTINCT FROM (", not_at);
				emit_span(o, sc, j, cond_to);
				out_text(o, ")", not_at);
				rest = cond_to;
			}
			else
			{
				/* IS NOT DISTINCT [FROM], less what is missing */
				out_text(o, tok_is_kw(ts, j - 1, "from") ?
						 ") IS NOT DISTINCT FROM " : ") IS NOT DISTINCT ",
						 not_at);
				rest = j;
			}
		}
		else
		{
			int			when_at = ts->toks[w].off;

			/* WHEN a -> WHEN (x) = (a), Cloudberry's = at its WHEN */
			out_text(o, "(", when_at);
			emit_span(o, sc, opfrom, opto);
			out_text(o, ") = (", when_at);
			emit_span(o, sc, w + 1, cond_to);
			out_text(o, ")", when_at);
			rest = cond_to;
		}

		/* THEN and its result, as written, and the space up to the next arm */
		if (rest < cond_to || rest == w + 1)
			emit_bytes(o, sc, ts->toks[rest].off, ts->toks[arm->stop].off,
					   rest, arm->stop);
		else
			emit_bytes(o, sc, tok_stop(ts, rest - 1), ts->toks[arm->stop].off,
					   rest, arm->stop);
	}

	/* ELSE, and END */
	if (else_at >= 0)
		emit_bytes(o, sc, ts->toks[else_at].off, ts->toks[end].off,
				   else_at, end);
	out_copy(o, ts->toks[end].off, tok_stop(ts, end));
}

/* ------------------------------------------------------------------------- */
/* gp_dist_random('t')                                                       */
/* ------------------------------------------------------------------------- */

/*
 * Cloudberry's gp_dist_random('t'), in FROM, is the relation t read on every
 * segment, each copy of a replicated table or a catalog once per segment.
 * Its parser makes it so (parse_clause.c, transformRangeFunction): a call of
 * that name with one string argument and no decoration is looked up as a
 * qualified name and becomes t's range table entry, with the alias the call
 * was given -- so without one it is named t, and t.a reads its column.
 *
 * The port's is gp.dist_random(NULL::t), which says which relation through
 * its argument's type (gp_dispatch.c).  So the call becomes that, with the
 * name taken apart as Cloudberry takes it (stringToQualifiedNameList, whose
 * error for a name it cannot read is Cloudberry's too) and each part quoted,
 * and without an alias it is given AS t.  Elsewhere than in FROM Cloudberry
 * has no gp_dist_random, and the call is left for PostgreSQL to refuse.
 */

/* Is token i an item of a FROM list, or of DELETE's USING list? */
static bool
in_from_list(const GpExprScan *sc, int i)
{
	static const char *const ends[] = {
		"select", "where", "group", "having", "order", "limit", "offset",
		"fetch", "returning", "set", "values", "window", "into", "union",
		"intersect", "except", "for", NULL
	};
	const GpTokens *ts = sc->ts;
	int			depth = 0;

	if (i <= sc->first)
		return false;
	if (tok_is_kw(ts, i - 1, "from") || tok_is_kw(ts, i - 1, "join") ||
		tok_is_kw(ts, i - 1, "lateral") || tok_is_kw(ts, i - 1, "using"))
		return true;
	if (!tok_is_char(ts, i - 1, ','))
		return false;

	/* After a comma: whose list is it, at this level of brackets? */
	for (int j = i - 2; j >= sc->first; j--)
	{
		if (tok_is_char(ts, j, ')'))
			depth++;
		else if (tok_is_char(ts, j, '('))
		{
			if (--depth < 0)
				return false;
		}
		else if (depth == 0 && ts->toks[j].kw != NULL)
		{
			if (tok_is_kw(ts, j, "from") || tok_is_kw(ts, j, "using"))
				return true;
			for (int k = 0; ends[k] != NULL; k++)
				if (tok_is_kw(ts, j, ends[k]))
					return false;
		}
	}
	return false;
}

/* gp_dist_random('t') at `i`: its closing parenthesis, or -1. */
static int
dist_random_close(const GpExprScan *sc, int i, int limit)
{
	const GpTokens *ts = sc->ts;
	const GpTok *t = &ts->toks[i];

	if (t->code != GP_IDENT || t->str == NULL ||
		pg_strcasecmp(t->str, "gp_dist_random") != 0)
		return -1;
	if (tok_is_char(ts, i - 1, '.') || i + 3 >= limit ||
		!tok_is_char(ts, i + 1, '(') || !tok_is_string(ts, i + 2) ||
		!tok_is_char(ts, i + 3, ')'))
		return -1;
	/* WITH ORDINALITY is a decoration, and makes it a function again */
	if (tok_is_kw(ts, i + 4, "with") && tok_is_kw(ts, i + 5, "ordinality"))
		return -1;
	if (!in_from_list(sc, i))
		return -1;
	return i + 3;
}

/* Does an alias follow token i: AS, or a name that can be a bare alias? */
static bool
alias_follows(const GpTokens *ts, int i)
{
	int			kwnum;

	if (i >= ts->ntoks)
		return false;
	if (tok_is_kw(ts, i, "as") || ts->toks[i].code == GP_IDENT)
		return true;
	if (ts->toks[i].kw == NULL)
		return false;
	kwnum = ScanKeywordLookup(ts->toks[i].kw, &ScanKeywords);
	return kwnum >= 0 &&
		(ScanKeywordCategories[kwnum] == UNRESERVED_KEYWORD ||
		 ScanKeywordCategories[kwnum] == COL_NAME_KEYWORD);
}

static void
emit_dist_random(GpOut *o, const GpExprScan *sc, int i, int close)
{
	const GpTokens *ts = sc->ts;
	List	   *names = stringToQualifiedNameList(ts->toks[i + 2].str, NULL);
	StringInfoData name;
	ListCell   *lc;

	initStringInfo(&name);
	foreach(lc, names)
	{
		if (lc != list_head(names))
			appendStringInfoChar(&name, '.');
		appendStringInfoString(&name, quote_identifier(strVal(lfirst(lc))));
	}

	/* The type's name stands for the string, as Cloudberry's RangeVar does */
	out_text(o, "gp.dist_random(NULL::", ts->toks[i].off);
	out_text(o, name.data, ts->toks[i + 2].off);
	out_text(o, ")", ts->toks[close].off);
	if (!alias_follows(ts, close + 1))
		out_text(o, psprintf(" AS %s", quote_identifier(strVal(llast(names)))),
				 ts->toks[i].off);
}

/*
 * count() at `i`, which Cloudberry's parse analysis takes for count(*) -- it
 * lets any aggregate be called with no arguments and no star
 * (parse_func.c), and count is the one its tests call so, as an aggregate
 * and as a window function: its closing parenthesis, or -1.  Spelled with
 * quotes or a schema, or where a name rather than a call stands, it is left.
 */
static int
count_close(const GpExprScan *sc, int i, int limit)
{
	const GpTokens *ts = sc->ts;
	const GpTok *t = &ts->toks[i];

	if (t->code != GP_IDENT || strcmp(t->str, "count") != 0 ||
		ts->src[t->off] == '"' || i + 2 >= limit ||
		!tok_is_char(ts, i + 1, '(') || !tok_is_char(ts, i + 2, ')') ||
		!decode_is_call(sc, i, i + 2))
		return -1;
	return i + 2;
}

/*
 * A construct of Cloudberry's starting at token i, ending before `limit`:
 * the index of its last token, or -1.
 */
static int
construct_at(const GpExprScan *sc, int i, int limit)
{
	int			stop;

	if ((stop = decode_close(sc, i, limit)) >= 0)
		return stop;
	if ((stop = dist_random_close(sc, i, limit)) >= 0)
		return stop;
	if ((stop = count_close(sc, i, limit)) >= 0)
		return stop;
	return case_close(sc, i, limit);
}

static void
emit_construct(GpOut *o, const GpExprScan *sc, int i, int stop)
{
	if (tok_is_kw(sc->ts, i, "case"))
		emit_case(o, sc, i, stop);
	else if (sc->ts->toks[i].code == GP_IDENT &&
			 pg_strcasecmp(sc->ts->toks[i].str, "gp_dist_random") == 0)
		emit_dist_random(o, sc, i, stop);
	else if (sc->ts->toks[i].code == GP_IDENT &&
			 strcmp(sc->ts->toks[i].str, "count") == 0)
	{
		out_text(o, "count(", sc->ts->toks[i].off);
		out_text(o, "*", sc->ts->toks[i + 1].off);
		out_text(o, ")", sc->ts->toks[stop].off);
	}
	else
		emit_decode(o, sc, i, stop);
}

/*
 * Every construct of Cloudberry's in the statement, the outermost of each
 * nest becoming one edit; what is nested in it is rewritten with it.
 *
 * DROP, GRANT and REVOKE name functions in lists, DROP FUNCTION f(int),
 * decode(int, int, int), and have no expressions for DECODE to be in.
 */
static void
rw_expressions(GpRewrite *rw, bool statement)
{
	const GpTokens *ts = rw->ts;
	GpExprScan	sc;
	int			i = rw->first;

	if (statement &&
		(tok_is_kw(ts, i, "drop") || tok_is_kw(ts, i, "grant") ||
		 tok_is_kw(ts, i, "revoke")))
		return;

	sc.ts = ts;
	sc.first = rw->first;
	sc.last = rw->last;
	sc.create_index = statement && tok_is_kw(ts, i, "create") &&
		(tok_is_kw(ts, i + 1, "index") ||
		 (tok_is_kw(ts, i + 1, "unique") && tok_is_kw(ts, i + 2, "index")));
	sc.alter_table = statement && tok_is_kw(ts, i, "alter") &&
		tok_is_kw(ts, i + 1, "table");

	for (int j = rw->first; j < rw->last; j++)
	{
		int			stop = construct_at(&sc, j, rw->last);
		GpOut	   *piece;

		if (stop < 0)
			continue;

		piece = palloc(sizeof(GpOut));
		out_init(piece, ts->src);
		emit_construct(piece, &sc, j, stop);
		rw_edit_piece(rw, ts->toks[j].off, tok_stop(ts, stop), piece);
		j = stop;
	}
}

/* ------------------------------------------------------------------------- */
/* The driver                                                                */
/* ------------------------------------------------------------------------- */

static void rw_statement_itself(GpRewrite *rw);

/*
 * CREATE SCHEMA's elements -- CREATE TABLE, CREATE VIEW and the rest, written
 * after it with nothing between them -- are statements of their own to the
 * grammar, and a table among them takes a DISTRIBUTED BY as one would: CREATE
 * SCHEMA s CREATE TABLE t (...) DISTRIBUTED BY (b) gives t its
 * gp.distributed_by, as Cloudberry's grammar gives it its DISTRIBUTED BY.
 * The element's edits are the schema statement's.  Only that: an
 * expression's rewrites are the whole statement's already.
 */
static void
rw_schema_elements(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	List	   *starts = NIL;
	int			depth = 0;
	ListCell   *lc;

	if (!tok_is_kw(ts, rw->first, "create") || !tok_is_kw(ts, rw->first + 1, "schema"))
		return;
	for (int i = rw->first + 2; i < rw->last; i++)
	{
		if (tok_is_char(ts, i, '('))
			depth++;
		else if (tok_is_char(ts, i, ')'))
			depth--;
		else if (depth == 0 && (tok_is_kw(ts, i, "create") || tok_is_kw(ts, i, "grant")))
			starts = lappend_int(starts, i);
	}

	foreach(lc, starts)
	{
		int			start = lfirst_int(lc);
		int			end = lnext(starts, lc) ? lfirst_int(lnext(starts, lc)) : rw->last;
		GpRewrite	sub;
		char	   *name = NULL;
		int			after_name;

		if (!tok_is_kw(ts, start, "create"))
			continue;
		rw_init(&sub, ts, start, end);
		if (find_subject(ts, start, end, &name, &after_name, &sub.object) !=
			GP_SUBJ_RELATION)
			continue;
		sub.subject_end = after_name;
		rw_distributed(&sub, after_name);
		rw_place_options(&sub);
		if (!sub.changed)
			continue;
		foreach_ptr(GpEdit, e, sub.edits)
		{
			e->seq = rw->nedits++;
			rw->edits = lappend(rw->edits, e);
		}
		rw->changed = true;
	}
}

/*
 * A statement, or one EXPLAIN shows: EXPLAIN [ANALYZE] [VERBOSE] and EXPLAIN
 * (options) are followed by a statement of their own, whose Cloudberry
 * clauses -- CREATE TABLE AS ... DISTRIBUTED BY, say -- are rewritten as
 * that statement's would be.
 */
static void
rw_statement(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			first = rw->first;

	if (tok_is_kw(ts, first, "explain"))
	{
		int			c = first + 1;

		if (tok_is_char(ts, c, '('))
			c = skip_parens(ts, c);
		else
			while (tok_is_kw(ts, c, "analyze") || tok_is_kw(ts, c, "analyse") ||
				   tok_is_kw(ts, c, "verbose"))
				c++;
		if (c < rw->last)
			rw->first = c;
	}
	rw_statement_itself(rw);
	rw->first = first;
}

static void
rw_statement_itself(GpRewrite *rw)
{
	GpSubjKind	kind;
	char	   *name = NULL;
	int			after_name = rw->first;

	/* Statements PostgreSQL has no counterpart of: a CALL. */
	if (rw_create_tag(rw) || rw_alter_tag(rw) || rw_drop_tag(rw) ||
		rw_profile(rw) || rw_drop_profile(rw) ||
		rw_task(rw) || rw_drop_task(rw) || rw_protocol(rw) ||
		rw_resource_queue(rw) || rw_resource_group(rw) ||
		rw_comment_resource(rw))
		return;

	/* ALTER USER ... PROFILE and the rest: ALTER USER, carrying it. */
	if (rw_role_profile(rw))
		return;

	/* [NO]CREATEEXTTABLE, carried the same way */
	(void) rw_role_exttable(rw);

	/* RESOURCE QUEUE and RESOURCE GROUP, carried for gp_resource */
	(void) rw_role_resource(rw);

	/* and DENY and DROP DENY, for gp_security */
	(void) rw_role_deny(rw);

	/* ALTER TYPE ... SET DEFAULT ENCODING: a label of gp_ao's. */
	if (rw_alter_type_encoding(rw))
		return;

	(void) rw_storage_and_dynamic(rw);
	(void) rw_orientation_row(rw);
	(void) rw_matview_options(rw);
	(void) rw_function_clauses(rw);
	(void) rw_external_spelling(rw);
	(void) rw_copy_options(rw);

	if (rw_create_directory_table(rw, &name, &after_name) ||
		rw_create_external_table(rw, &name, &after_name))
		kind = GP_SUBJ_RELATION;
	else
		kind = find_subject(rw->ts, rw->first, rw->last, &name, &after_name,
							&rw->object);
	if (kind != GP_SUBJ_NONE)
	{
		rw->subject_end = after_name;
		rw_tag_clauses(rw, kind, name, after_name);
		if (kind == GP_SUBJ_RELATION)
		{
			rw_distributed(rw, after_name);
			rw_partition_by(rw, after_name);
			if (rw->object == 't' && tok_is(rw->ts, rw->first, "create"))
				rw_encoding_create(rw, after_name);
			else if (rw->object == 't' && tok_is(rw->ts, rw->first, "alter"))
				rw_encoding_alter(rw, after_name);
		}
	}

	/* ALTER TABLE's partition commands */
	rw_partition_cmds(rw);

	/* CREATE SCHEMA's CREATE TABLE and the rest */
	rw_schema_elements(rw);

	/*
	 * DECODE, CASE ... WHEN IS NOT DISTINCT FROM and gp_dist_random('t'),
	 * wherever they are.
	 */
	if (!rw->whole)
		rw_expressions(rw, true);

	/* The options the clauses became, all into the statement's own list. */
	rw_place_options(rw);
	rw_place_fdw_options(rw);
}

/*
 * What a statement carries, for its parse node (GpAttachCarriers): the
 * DefElems, and where the statement starts in the user's text, which is how
 * its RawStmt is found once the grammar has built one.
 */
typedef struct GpCarried
{
	int			start;
	List	   *defs;
} GpCarried;

/*
 * The carriers of a statement, shown for gp_sql.desugar() as a comment after
 * it, since the text handed to the grammar has no place for them either.  It
 * is written for a person to read and never parsed; a value that holds the
 * end of a comment is shown with it broken.
 */
static void
out_show_carriers(GpOut *o, List *defs, int at)
{
	StringInfoData buf;
	ListCell   *lc;
	char	   *p;

	initStringInfo(&buf);
	foreach(lc, defs)
	{
		DefElem    *def = lfirst_node(DefElem, lc);

		appendStringInfo(&buf, "%s%s.%s = %s", buf.len > 0 ? ", " : "",
						 def->defnamespace, def->defname,
						 def->arg != NULL ? quote_literal_cstr(strVal(def->arg)) : "DEFAULT");
	}
	while ((p = strstr(buf.data, "*/")) != NULL)
		p[1] = '|';

	out_text(o, psprintf(" /* and on its parse node: %s */", buf.data), at);
}

/*
 * The rewrite of `str`, or NULL when there is nothing of Cloudberry's in it.
 *
 * With expr_only, `str` is not a statement but what PL/pgSQL hands the
 * parser for an expression or an assignment, where only an expression of
 * Cloudberry's can be.  With map, *map says where each byte of the result
 * came from.  With carried, *carried gets what each statement carries to its
 * parse node, a GpCarried each; with show, gp_sql.desugar()'s, it is shown in
 * the text instead.
 */
static char *
desugar(const char *str, bool expr_only, GpPosMap **map, List **carried,
		bool show)
{
	GpTokens   *ts;
	GpOut		out;
	int			depth = 0;
	int			first = 0;
	bool		changed = false;

	if (str == NULL || !looks_interesting(str))
		return NULL;

	ts = GpTokenize(str);
	if (ts->ntoks == 0)
		return NULL;

	out_init(&out, str);

	/* Anything before the first token: a leading comment. */
	out_copy(&out, 0, ts->toks[0].off);

	for (int i = 0; i <= ts->ntoks; i++)
	{
		GpRewrite	rw;
		bool		at_end = (i == ts->ntoks);
		int			start;

		if (!at_end)
		{
			if (tok_is_char(ts, i, '('))
				depth++;
			else if (tok_is_char(ts, i, ')'))
				depth--;
			if (!(depth == 0 && tok_is_char(ts, i, ';')))
				continue;
		}

		if (i == first)			/* an empty statement */
		{
			if (!at_end)
			{
				out_copy(&out, ts->toks[i].off, tok_end(ts, i));
				first = i + 1;
			}
			continue;
		}

		rw_init(&rw, ts, first, i);
		if (expr_only)
			rw_expressions(&rw, false);
		else
			rw_statement(&rw);
		rw_finish_body(&rw);

		/*
		 * The statement, one for one.  What the rewrite writes in place of a
		 * whole statement -- a CALL -- stands for the statement's start.
		 */
		start = ts->toks[first].off;
		if (rw.whole)
			out_text(&out, rw.body.data, start);
		else
			out_append(&out, &rw.text);

		if (rw.carriers != NIL)
		{
			if (show)
				out_show_carriers(&out, rw.carriers, start);
			if (carried != NULL)
			{
				GpCarried  *c = palloc(sizeof(GpCarried));

				c->start = start;
				c->defs = rw.carriers;
				*carried = lappend(*carried, c);
			}
		}
		changed |= rw.changed;

		/* The separator, and whatever trails the last statement. */
		if (!at_end)
		{
			int			upto = (i + 1 < ts->ntoks) ? ts->toks[i + 1].off : ts->srclen;

			out_copy(&out, ts->toks[i].off, upto);
			first = i + 1;
		}
	}

	if (!changed)
	{
		pfree(out.buf.data);
		return NULL;
	}

	if (map != NULL)
	{
		GpPosMap   *m = palloc(sizeof(GpPosMap));

		m->segs = out.segs;
		m->nsegs = out.nsegs;
		m->outlen = out.buf.len;
		m->srclen = ts->srclen;
		*map = m;
	}

	return out.buf.data;
}

char *
GpDesugar(const char *str)
{
	return desugar(str, false, NULL, NULL, true);
}

char *
GpDesugarMapped(const char *str, bool expr_only, GpPosMap **map, List **carried)
{
	return desugar(str, expr_only, map, carried, false);
}

/*
 * GpAttachCarriers
 *		Put on each statement's parse node what its rewrite carried to it.
 *
 * A clause PostgreSQL's grammar has no place for on its statement -- TAG on
 * CREATE SCHEMA, CREATE USER and CREATE SEQUENCE, TAG and PROFILE on ALTER
 * USER -- is taken out of the text, and the DefElems it became are appended
 * here to the list the statement's node keeps its options in, once the
 * grammar has built the node: the statement stays one, and the modules'
 * ProcessUtility hooks take the DefElems out again before PostgreSQL reads
 * that list.  They are nodes of a kind every copy, comparison and
 * serialization of a parse tree handles, so a plan that is cached, or a tree
 * that is copied because it is read-only, keeps them.
 *
 * A statement is found by where it starts in the user's text, which the
 * RawStmt the grammar built for it spans once the positions are the user's.
 * A CREATE SCHEMA keeps its options nowhere, having none, so what it carries
 * goes among its elements, which are the only list it has.
 */
void
GpAttachCarriers(List *parsetree, List *carried)
{
	ListCell   *lc;

	foreach(lc, carried)
	{
		GpCarried  *c = (GpCarried *) lfirst(lc);
		RawStmt    *target = NULL;
		ListCell   *lc2;
		ListCell   *lc3;

		foreach(lc2, parsetree)
		{
			RawStmt    *rs = lfirst_node(RawStmt, lc2);

			if (rs->stmt_location <= c->start &&
				(rs->stmt_len == 0 || c->start < rs->stmt_location + rs->stmt_len))
			{
				target = rs;
				break;
			}
		}
		if (target == NULL)
			elog(ERROR, "O26: the statement at %d was not found in its parse tree",
				 c->start);

		/*
		 * A profile, a lock and a DENY clause are gp_security's to put on a
		 * role, and a queue and a group gp_resource's.  Without the module
		 * nothing would take the option out again, and CREATE or ALTER ROLE
		 * would refuse it as an option it does not know, which says nothing
		 * useful.
		 */
		foreach(lc3, c->defs)
		{
			DefElem    *def = lfirst_node(DefElem, lc3);

			if (strcmp(def->defnamespace, "gp") == 0 &&
				*find_rendezvous_variable(CB_SECURITY_RENDEZVOUS) == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("%s needs \"gp_security\"",
								strcmp(def->defname, "profile") == 0 ?
								"a role's profile" :
								strcmp(def->defname, "account") == 0 ?
								"locking an account" : "a DENY clause"),
						 errhint("Add \"gp_security\" to \"shared_preload_libraries\".")));
			/* and a queue or a group is gp_resource's */
			if (strcmp(def->defnamespace, "gp_resource") == 0 &&
				*find_rendezvous_variable(CB_RESOURCE_RENDEZVOUS) == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("a role's resource %s needs \"gp_resource\"",
								strcmp(def->defname, "resource_queue") == 0 ?
								"queue" : "group"),
						 errhint("Add \"gp_resource\" to \"shared_preload_libraries\".")));
		}

		switch (nodeTag(target->stmt))
		{
			case T_CreateSchemaStmt:
				{
					CreateSchemaStmt *s = (CreateSchemaStmt *) target->stmt;

					s->schemaElts = list_concat(s->schemaElts, c->defs);
				}
				break;
			case T_CreateRoleStmt:
				{
					CreateRoleStmt *s = (CreateRoleStmt *) target->stmt;

					s->options = list_concat(s->options, c->defs);
				}
				break;
			case T_AlterRoleStmt:
				{
					AlterRoleStmt *s = (AlterRoleStmt *) target->stmt;

					s->options = list_concat(s->options, c->defs);
				}
				break;
			case T_CreateSeqStmt:
				{
					CreateSeqStmt *s = (CreateSeqStmt *) target->stmt;

					s->options = list_concat(s->options, c->defs);
				}
				break;
			case T_CopyStmt:
				{
					CopyStmt   *s = (CopyStmt *) target->stmt;

					s->options = list_concat(s->options, c->defs);
				}
				break;
			default:
				elog(ERROR, "O26: a statement of node type %d cannot carry what the rewrite put on it",
					 (int) nodeTag(target->stmt));
		}
	}
}

/* ------------------------------------------------------------------------- */
/* O26                                                                       */
/* ------------------------------------------------------------------------- */

static raw_parser_hook_type prev_raw_parser = NULL;

/*
 * A syntax error in a rewritten statement, reported where the user wrote what
 * the grammar stopped at.  The grammar gives its position as a character
 * count into the text it read; that becomes a byte offset there, the offset
 * in the user's text it stands for, and a character count again.
 *
 * It runs before the callbacks of whoever called the parser, being pushed
 * last: PL/pgSQL's moves the position into the function's body, and has to
 * be given one in the text it handed over.
 */
void
GpParseErrorCallback(void *arg)
{
	GpParseErrorArg *a = (GpParseErrorArg *) arg;
	int			pos = geterrposition();
	int			offset = 0;

	if (pos <= 0)
		return;

	for (int c = 1; c < pos && a->rewritten[offset] != '\0'; c++)
		offset += pg_mblen_cstr(a->rewritten + offset);

	offset = GpPosMapSource(a->map, offset);
	if (offset < 0)
		errposition(0);			/* in text with no place in the user's */
	else
		errposition(pg_mbstrlen_with_len(a->original, offset) + 1);
}

static List *
gp_raw_parser(const char *str, RawParseMode mode)
{
	char	   *rewritten = NULL;
	GpPosMap   *map = NULL;
	List	   *carried = NIL;
	GpParseErrorArg errarg;
	ErrorContextCallback errcallback;
	List	   *result;

	/*
	 * A statement the coordinator dispatched arrives as its parse tree, and
	 * gp_core's parser turns it back into one; there is nothing of
	 * Cloudberry's in it to rewrite, and its text is not SQL.
	 */
	if (GpDispatchIsTreeText(str))
	{
		if (prev_raw_parser)
			return prev_raw_parser(str, mode);
		return standard_raw_parser(str, mode);
	}

	/*
	 * A whole statement can hold anything of Cloudberry's.  The PL/pgSQL
	 * modes parse an expression or an assignment, which can hold a DECODE or
	 * a CASE ... WHEN IS NOT DISTINCT FROM and nothing else of Cloudberry's;
	 * a type name holds nothing.
	 */
	if (mode == RAW_PARSE_DEFAULT)
		rewritten = GpDesugarMapped(str, false, &map, &carried);
	else if (mode != RAW_PARSE_TYPE_NAME)
		rewritten = GpDesugarMapped(str, true, &map, NULL);

	if (rewritten == NULL)
	{
		if (prev_raw_parser)
			return prev_raw_parser(str, mode);
		return standard_raw_parser(str, mode);
	}

	errarg.original = str;
	errarg.rewritten = rewritten;
	errarg.map = map;
	errcallback.callback = GpParseErrorCallback;
	errcallback.arg = &errarg;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	if (prev_raw_parser)
		result = prev_raw_parser(rewritten, mode);
	else
		result = standard_raw_parser(rewritten, mode);

	error_context_stack = errcallback.previous;

	/* And where parse analysis will report its errors, likewise. */
	GpRemapParseLocations(result, map);

	/* What has no place in the text goes on the nodes, now in the user's. */
	GpAttachCarriers(result, carried);

	return result;
}

void
GpGrammarInstallHook(void)
{
	prev_raw_parser = raw_parser_hook;
	raw_parser_hook = gp_raw_parser;
}

PG_FUNCTION_INFO_V1(gp_sql_desugar);

/*
 * gp_sql.desugar(text) -> text
 *
 * What the hook would hand to PostgreSQL's parser, or the statement itself
 * when there is nothing of Cloudberry's in it.  It is here so that the tests
 * can ask what a rewrite produced rather than only whether it worked, and so
 * that a person debugging one can see it.  What a statement carries to its
 * parse node (GpAttachCarriers) is shown in a comment after it, since the
 * text has no place for it.
 */
Datum
gp_sql_desugar(PG_FUNCTION_ARGS)
{
	char	   *str = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *out = GpDesugar(str);

	PG_RETURN_TEXT_P(cstring_to_text(out != NULL ? out : str));
}
