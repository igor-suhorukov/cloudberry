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
 * What this is, and what it is not.  "Porting the Cloudberry code" describes
 * the grammar as a fork of PG19's gram.y with Cloudberry's productions
 * applied, re-generated whenever either changes.  This is not that.  It is a
 * rewriter over PostgreSQL's own scanner: the statement is tokenised with
 * core_yylex, the parts written in Cloudberry's spelling are replaced by the
 * standard spelling, and the result is handed to standard_raw_parser.  The
 * scanner is the real one, so comments, dollar quoting, Unicode escapes and
 * standard_conforming_strings behave exactly as they do everywhere else.
 *
 * What a fork would give that this does not: a comment written inside a
 * clause that is replaced is dropped with it.  Two more were true until
 * DECODE made this rewrite inside expressions, and are not now:
 *
 *   - Cloudberry's syntax nested inside an expression is reached: DECODE and
 *     CASE x WHEN IS NOT DISTINCT FROM y, the only such syntax the port
 *     takes, are found wherever they are, nested in each other or not.
 *     (MEDIAN(x) needs no rewrite, being a call in PostgreSQL's grammar.)
 *   - Positions are the user's.  The rewritten text records where each byte
 *     of it came from, and the caret under a syntax error, and every
 *     location in the parse tree, is put back where the user wrote it --
 *     text the rewrite wrote standing for the token it replaces.
 *
 * What it gives that a fork would not: nothing to re-base when PostgreSQL
 * changes its grammar, and no second copy of 20,000 lines of it.
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
#include "parser/parser.h"
#include "parser/scanner.h"
#include "parser/scansup.h"
#include "utils/builtins.h"
#include "utils/elog.h"

#include "gp_grammar.h"

/*
 * The core scanner's token codes.  scanner.h does not define them -- bison
 * insists on doing that -- but it does promise what they are: the ASCII
 * characters, and then these, in this order, starting at 258.
 */
#define GP_IDENT		258
#define GP_UIDENT		259
#define GP_FCONST		260
#define GP_SCONST		261
#define GP_USCONST		262
#define GP_BCONST		263
#define GP_XCONST		264
#define GP_OP			265
#define GP_ICONST		266
#define GP_PARAM		267
#define GP_TYPECAST		268
#define GP_DOT_DOT		269
#define GP_COLON_EQUALS	270
#define GP_EQUALS_GREATER 271
#define GP_LESS_EQUALS	272
#define GP_GREATER_EQUALS 273
#define GP_NOT_EQUALS	274

/* Anything above the last of those is a keyword of PostgreSQL's own. */
#define GP_FIRST_KEYWORD	275

typedef struct GpTok
{
	int			code;
	int			off;			/* byte offset of the token's first character */
	const char *kw;				/* canonical spelling, for a keyword */
	char	   *str;			/* the value, for an identifier or literal */
	int			ival;
} GpTok;

typedef struct GpTokens
{
	GpTok	   *toks;
	int			ntoks;
	const char *src;
	int			srclen;
} GpTokens;

/*
 * Words that can only appear in Cloudberry's spelling of something.  A
 * statement with none of them cannot need rewriting, and finding that out is
 * one pass over the text rather than a whole lexing.
 */
static const char *const gp_trigger_words[] = {
	"tag", "profile", "distributed", "randomly", "replicated", "task",
	"directory", "storage", "dynamic", "incremental", "unset", "account",
	"execute", "decode",
	NULL
};

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
 * WHEN IS is CASE x WHEN IS NOT DISTINCT FROM y.  The second word of a pair
 * is matched as the start of one, so WHEN is_active fires it too, which
 * costs a tokenisation and rewrites nothing.
 */
static const char *const gp_trigger_pairs[][2] = {
	{"no", "sql"},
	{"contains", "sql"},
	{"reads", "sql"},
	{"modifies", "sql"},
	{"when", "is"},
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

	for (int i = 0; i < len; i++)
	{
		/* Only where a word can start, so this is one pass and no more. */
		if (i > 0 && is_word_char(str[i - 1]))
			continue;

		for (int w = 0; gp_trigger_words[w] != NULL; w++)
		{
			int			wl = strlen(gp_trigger_words[w]);

			if (pg_tolower((unsigned char) str[i]) != gp_trigger_words[w][0])
				continue;
			if (i + wl <= len &&
				pg_strncasecmp(str + i, gp_trigger_words[w], wl) == 0)
				return true;
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
static GpTokens *
gp_tokenize(const char *str)
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

/* Where a token ends, for the purpose of cutting text out. */
static int
tok_end(const GpTokens *ts, int i)
{
	if (i + 1 < ts->ntoks)
		return ts->toks[i + 1].off;
	return ts->srclen;
}

/*
 * Is token `i` this word?  A word Cloudberry made a keyword and PostgreSQL
 * did not arrives as an identifier; one they both have arrives as a keyword.
 */
static bool
tok_is(const GpTokens *ts, int i, const char *word)
{
	const GpTok *t;

	if (i < 0 || i >= ts->ntoks)
		return false;

	t = &ts->toks[i];

	if (t->kw != NULL)
		return pg_strcasecmp(t->kw, word) == 0;
	if (t->code == GP_IDENT && t->str != NULL)
		return pg_strcasecmp(t->str, word) == 0;

	return false;
}

static bool
tok_is_char(const GpTokens *ts, int i, char c)
{
	return i >= 0 && i < ts->ntoks && ts->toks[i].code == (int) c;
}

/* The keyword `word` itself, not an identifier spelled the same, quoted. */
static bool
tok_is_kw(const GpTokens *ts, int i, const char *word)
{
	return i >= 0 && i < ts->ntoks && ts->toks[i].kw != NULL &&
		pg_strcasecmp(ts->toks[i].kw, word) == 0;
}

/* An identifier or keyword, as a name the rewritten text can use. */
static bool
tok_is_name(const GpTokens *ts, int i)
{
	if (i < 0 || i >= ts->ntoks)
		return false;
	return ts->toks[i].code == GP_IDENT || ts->toks[i].kw != NULL;
}

static char *
tok_name(const GpTokens *ts, int i)
{
	const GpTok *t = &ts->toks[i];

	if (t->code == GP_IDENT)
		return t->str;
	return pstrdup(t->kw);
}

static bool
tok_is_string(const GpTokens *ts, int i)
{
	return i >= 0 && i < ts->ntoks && ts->toks[i].code == GP_SCONST;
}

/* Skip a parenthesised group that starts at `i`; returns the index after it. */
static int
skip_parens(const GpTokens *ts, int i)
{
	int			depth = 0;

	for (; i < ts->ntoks; i++)
	{
		if (tok_is_char(ts, i, '('))
			depth++;
		else if (tok_is_char(ts, i, ')'))
		{
			depth--;
			if (depth == 0)
				return i + 1;
		}
	}

	return i;
}

/*
 * Where token i's own text ends, which tok_end() does not say: it runs to the
 * next token, whitespace and comments included.  A single character and a
 * keyword are as long as they are; anything else ends where the whitespace
 * before the next token begins, so a comment between the two stays with the
 * first.
 */
static int
tok_stop(const GpTokens *ts, int i)
{
	const GpTok *t = &ts->toks[i];
	int			end = tok_end(ts, i);

	if (t->code > 0 && t->code < 256)
		return t->off + 1;
	if (t->kw != NULL)
		return t->off + strlen(t->kw);

	switch (t->code)
	{
		case GP_TYPECAST:
		case GP_DOT_DOT:
		case GP_COLON_EQUALS:
		case GP_EQUALS_GREATER:
		case GP_LESS_EQUALS:
		case GP_GREATER_EQUALS:
		case GP_NOT_EQUALS:
			return t->off + 2;
		case GP_OP:
			return t->off + strlen(t->str);
	}

	while (end > t->off && scanner_isspace(ts->src[end - 1]))
		end--;
	return end;
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

/* ------------------------------------------------------------------------- */
/* Rewriting                                                                 */
/* ------------------------------------------------------------------------- */

/*
 * One statement is rewritten into a buffer: the parts that are already
 * PostgreSQL's are copied across, the parts that are Cloudberry's are
 * replaced, and anything that has to happen afterwards -- a label, a function
 * call -- is added as a statement of its own.
 */
typedef struct GpEdit
{
	int			from;			/* byte offset, inclusive */
	int			to;				/* byte offset, exclusive */
	char	   *text;			/* what goes there instead */
	GpOut	   *piece;			/* or this, which keeps where its parts came
								 * from; for an expression's rewrite */
} GpEdit;

typedef struct GpRewrite
{
	const GpTokens *ts;
	int			first;			/* first token of the statement */
	int			last;			/* one past its last token */
	bool		changed;
	bool		whole;			/* body replaces the statement outright */
	int			tag_first;		/* first token of a TAG clause, or -1 */
	int			tag_last;		/* one past the last token of the last one */
	List	   *edits;			/* GpEdit, in whatever order they were found */
	StringInfoData body;		/* what the statement becomes, when whole */
	GpOut		text;			/* the statement with its edits, when not */
	StringInfoData after;		/* statements to run after it */
	char		object;			/* what find_subject found: 't' a table, 'f' a
								 * foreign table, 'v' a view, 'm' a materialized
								 * view, 'S' a sequence, 'i' an index, or 0 */
	int			subject_end;	/* the token after the subject's name, or -1 */
	StringInfoData options;		/* namespaced options for its WITH list */
	List	   *calls;			/* functions to call, in one SELECT */
	List	   *call_at;		/* the user's text each stands for */
} GpRewrite;

static void
rw_init(GpRewrite *rw, const GpTokens *ts, int first, int last)
{
	rw->ts = ts;
	rw->first = first;
	rw->last = last;
	rw->changed = false;
	rw->whole = false;
	rw->tag_first = -1;
	rw->tag_last = -1;
	rw->edits = NIL;
	initStringInfo(&rw->body);
	out_init(&rw->text, ts->src);
	initStringInfo(&rw->after);
	rw->object = 0;
	rw->subject_end = -1;
	initStringInfo(&rw->options);
	rw->calls = NIL;
	rw->call_at = NIL;
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
	rw->edits = lappend(rw->edits, e);
	rw->changed = true;
}

/*
 * A function to call, in the one SELECT a statement's calls are made in; it
 * stands for the clause of the user's that it was made from, at `at`.
 */
static void
rw_add_call(GpRewrite *rw, char *call, int at)
{
	rw->calls = lappend(rw->calls, call);
	rw->call_at = lappend_int(rw->call_at, at);
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
	return 0;
}

/*
 * A namespaced option the statement is to carry in its WITH list, such as
 * gp.distributed_by = '(a)': what a clause of Cloudberry's becomes when the
 * statement it is on can take one, so that the statement stays one statement.
 * rw_place_options puts them in, all together, once the clauses are read.
 */
static void
rw_add_option(GpRewrite *rw, const char *option)
{
	if (rw->options.len > 0)
		appendStringInfoString(&rw->options, ", ");
	appendStringInfoString(&rw->options, option);
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

	if (rw->options.len == 0 || rw->whole)
		return;

	for (int j = (rw->subject_end >= 0 ? rw->subject_end : rw->first); j < rw->last; j++)
	{
		if (tok_is_char(ts, j, '('))
		{
			if (depth == 0 && tok_is_kw(ts, j - 1, "with"))
			{
				rw_edit(rw, tok_end(ts, j), tok_end(ts, j),
						psprintf("%s, ", rw->options.data));
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
			rw_edit(rw, ts->toks[j].off, tok_end(ts, j + 1),
					psprintf("WITH (%s)", rw->options.data));
			return;
		}
		if ((tok_is_kw(ts, j, "on") && tok_is_kw(ts, j + 1, "commit")) ||
			tok_is_kw(ts, j, "tablespace") || tok_is_kw(ts, j, "as") ||
			tok_is_kw(ts, j, "where"))
		{
			rw_edit(rw, ts->toks[j].off, ts->toks[j].off,
					psprintf("WITH (%s) ", rw->options.data));
			return;
		}
	}

	at = (rw->last < ts->ntoks) ? ts->toks[rw->last].off : ts->srclen;
	rw_edit(rw, at, at, psprintf(" WITH (%s)", rw->options.data));
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

/* A comma-separated list of names; returns the end. */
static int
collect_names(const GpTokens *ts, int i, List **names)
{
	for (;;)
	{
		int			e = skip_qualified_name(ts, i);

		if (e == i)
			break;
		*names = lappend(*names, rw_text(ts, i, e));
		i = e;
		if (!tok_is_char(ts, i, ','))
			break;
		i++;
	}

	return i;
}

/* ------------------------------------------------------------------------- */
/* Statements that become a function call                                    */
/* ------------------------------------------------------------------------- */

/*
 * CREATE TAG [IF NOT EXISTS] name [ALLOWED_VALUES 'a', 'b']
 *	 -> SELECT gp_sql.create_tag('name', ARRAY[...], if_not_exists)
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
	appendStringInfo(&rw->body, "SELECT gp_sql.create_tag(%s, %s, %s)",
					 quote_literal_cstr(tok_name(ts, i)), values.data,
					 if_not_exists ? "true" : "false");
	return true;
}

/*
 * ALTER TAG [IF EXISTS] name { ADD | DROP } ALLOWED_VALUES 'a', ...
 * ALTER TAG name UNSET ALLOWED_VALUES
 * ALTER TAG name RENAME TO newname
 */
static bool
rw_alter_tag(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			i = rw->first;
	char	   *name;
	StringInfoData values;

	if (!tok_is(ts, i, "alter") || !tok_is(ts, i + 1, "tag"))
		return false;
	i += 2;

	if (tok_is(ts, i, "if") && tok_is(ts, i + 1, "exists"))
		i += 2;

	if (!tok_is_name(ts, i))
		return false;
	name = tok_name(ts, i);
	i++;

	rw_whole(rw);

	if (tok_is(ts, i, "rename") && tok_is(ts, i + 1, "to") && tok_is_name(ts, i + 2))
		appendStringInfo(&rw->body, "SELECT gp_sql.rename_tag(%s, %s)",
						 quote_literal_cstr(name),
						 quote_literal_cstr(tok_name(ts, i + 2)));
	else if (tok_is(ts, i, "unset") && tok_is(ts, i + 1, "allowed_values"))
		appendStringInfo(&rw->body,
						 "SELECT gp_sql.alter_tag(%s, unset_values => true)",
						 quote_literal_cstr(name));
	else if ((tok_is(ts, i, "add") || tok_is(ts, i, "drop")) &&
			 tok_is(ts, i + 1, "allowed_values"))
	{
		bool		adding = tok_is(ts, i, "add");

		initStringInfo(&values);
		(void) collect_strings(ts, i + 2, &values);
		appendStringInfo(&rw->body, "SELECT gp_sql.alter_tag(%s, %s => %s)",
						 quote_literal_cstr(name),
						 adding ? "add_values" : "drop_values", values.data);
	}
	else
		return false;

	return true;
}

/* DROP TAG [IF EXISTS] a, b -> one call each, in one SELECT */
static bool
rw_drop_tag(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			i = rw->first;
	bool		missing_ok = false;
	List	   *names = NIL;
	ListCell   *lc;

	if (!tok_is(ts, i, "drop") || !tok_is(ts, i + 1, "tag"))
		return false;
	i += 2;

	if (tok_is(ts, i, "if") && tok_is(ts, i + 1, "exists"))
	{
		missing_ok = true;
		i += 2;
	}

	(void) collect_names(ts, i, &names);
	if (names == NIL)
		return false;

	rw_whole(rw);
	foreach(lc, names)
		rw_add_call(rw, psprintf("gp_sql.drop_tag(%s, %s)",
								 quote_literal_cstr((char *) lfirst(lc)),
								 missing_ok ? "true" : "false"),
					ts->toks[rw->first].off);
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
	appendStringInfo(&rw->body, "SELECT gp_security.%s_profile(%s%s)",
					 creating ? "create" : "alter",
					 quote_literal_cstr(name), args.data);
	return true;
}

/* DROP PROFILE [IF EXISTS] a, b */
static bool
rw_drop_profile(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			i = rw->first;
	bool		missing_ok = false;
	List	   *names = NIL;
	ListCell   *lc;

	if (!tok_is(ts, i, "drop") || !tok_is(ts, i + 1, "profile"))
		return false;
	i += 2;

	if (tok_is(ts, i, "if") && tok_is(ts, i + 1, "exists"))
	{
		missing_ok = true;
		i += 2;
	}

	(void) collect_names(ts, i, &names);
	if (names == NIL)
		return false;

	rw_whole(rw);
	foreach(lc, names)
		rw_add_call(rw, psprintf("gp_security.drop_profile(%s, %s)",
								 quote_literal_cstr((char *) lfirst(lc)),
								 missing_ok ? "true" : "false"),
					ts->toks[rw->first].off);
	return true;
}

/*
 * CREATE DIRECTORY TABLE [IF NOT EXISTS] name [TABLESPACE ts] [TAG (...)]
 *	 -> SELECT gp_sql.create_directory_table('name', 'ts')
 */
static bool
rw_create_directory_table(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			i = rw->first;
	int			nameend;
	char	   *name;
	char	   *tablespace = NULL;

	if (!tok_is(ts, i, "create") || !tok_is(ts, i + 1, "directory") ||
		!tok_is(ts, i + 2, "table"))
		return false;
	i += 3;

	if (tok_is(ts, i, "if") && tok_is(ts, i + 1, "not") && tok_is(ts, i + 2, "exists"))
		i += 3;

	nameend = skip_qualified_name(ts, i);
	if (nameend == i)
		return false;
	name = rw_text(ts, i, nameend);

	if (tok_is(ts, nameend, "tablespace") && tok_is_name(ts, nameend + 1))
		tablespace = tok_name(ts, nameend + 1);

	rw_whole(rw);
	appendStringInfo(&rw->body, "SELECT gp_sql.create_directory_table(%s, %s)",
					 quote_literal_cstr(name),
					 tablespace ? quote_literal_cstr(tablespace) : "NULL");
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
		i += 3;
	else if (!creating && tok_is(ts, i, "if") && tok_is(ts, i + 1, "exists"))
		i += 2;

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
		appendStringInfo(&rw->body, "SELECT gp_task.create_task(%s, %s, %s",
						 quote_literal_cstr(name),
						 quote_literal_cstr(schedule),
						 quote_literal_cstr(command));
		if (database != NULL)
			appendStringInfo(&rw->body, ", database => %s",
							 quote_literal_cstr(database));
		if (username != NULL)
			appendStringInfo(&rw->body, ", username => %s",
							 quote_literal_cstr(username));
		appendStringInfoChar(&rw->body, ')');
	}
	else
	{
		appendStringInfo(&rw->body, "SELECT gp_task.alter_task(%s",
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
		appendStringInfoChar(&rw->body, ')');
	}

	return true;
}

/* DROP TASK [IF EXISTS] a, b */
static bool
rw_drop_task(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			i = rw->first;
	bool		missing_ok = false;
	List	   *names = NIL;
	ListCell   *lc;

	if (!tok_is(ts, i, "drop") || !tok_is(ts, i + 1, "task"))
		return false;
	i += 2;

	if (tok_is(ts, i, "if") && tok_is(ts, i + 1, "exists"))
	{
		missing_ok = true;
		i += 2;
	}

	(void) collect_names(ts, i, &names);
	if (names == NIL)
		return false;

	rw_whole(rw);
	foreach(lc, names)
		rw_add_call(rw, psprintf("gp_task.drop_task(%s, %s)",
								 quote_literal_cstr((char *) lfirst(lc)),
								 missing_ok ? "true" : "false"),
					ts->toks[rw->first].off);
	return true;
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
 * which setter to become.  Only the kinds Cloudberry lets one be written on.
 * For a relation, *object says which kind: 't' a table, 'f' a foreign table,
 * 'v' a view, 'm' a materialized view, 'S' a sequence, 'i' an index -- the
 * difference between one whose statement can carry the tag as an option of
 * its own and one that cannot.  An index need not be named; its name is then
 * empty, and *after is where ON begins.
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

/* The function that puts a tag on this kind of object. */
static const char *
subject_setter(GpSubjKind kind, bool unset)
{
	switch (kind)
	{
		case GP_SUBJ_RELATION:
			return unset ? "gp_sql.unset_relation_tag" : "gp_sql.set_relation_tag";
		case GP_SUBJ_SCHEMA:
			return unset ? "gp_sql.unset_schema_tag" : "gp_sql.set_schema_tag";
		case GP_SUBJ_DATABASE:
			return unset ? "gp_sql.unset_database_tag" : "gp_sql.set_database_tag";
		case GP_SUBJ_TABLESPACE:
			return unset ? "gp_sql.unset_tablespace_tag" : "gp_sql.set_tablespace_tag";
		case GP_SUBJ_ROLE:
			return unset ? "gp_sql.unset_role_tag" : "gp_sql.set_role_tag";
		default:
			return NULL;
	}
}

/* How the object is named in the call: a relation by regclass, else by name. */
static char *
subject_argument(GpSubjKind kind, const char *name)
{
	switch (kind)
	{
		case GP_SUBJ_RELATION:
			return psprintf("%s::regclass", quote_literal_cstr(name));
		case GP_SUBJ_SCHEMA:
			return psprintf("%s::regnamespace", quote_literal_cstr(name));
		case GP_SUBJ_ROLE:
			return psprintf("%s::regrole", quote_literal_cstr(name));
		default:
			return quote_literal_cstr(name);
	}
}

/*
 * Where the tags of a CREATE DATABASE or CREATE TABLESPACE go: into the
 * statement, as options gp_sql's ProcessUtility hook takes out again and puts
 * on the object once it exists.  A database's options are written one after
 * another, each a name that is a quoted identifier, "gp_tag.env" = 'prod',
 * because CREATE DATABASE has no namespaced option; a tablespace's go into
 * its WITH (...) list as gp_tag.env = 'prod', which is the namespace
 * GP_TAG_OPTION_NS (gp_sql.h) names.
 */
static void
tag_option(StringInfo opts, GpSubjKind kind, const char *key, const char *value)
{
	if (kind == GP_SUBJ_DATABASE)
		appendStringInfo(opts, " %s = %s",
						 quote_identifier(psprintf("gp_tag.%s", key)),
						 quote_literal_cstr(value));
	else
		appendStringInfo(opts, "%sgp_tag.%s = %s",
						 opts->len > 0 ? ", " : "",
						 quote_identifier(key), quote_literal_cstr(value));
}

/*
 * TAG (name = 'value', ...) and UNSET TAG (name, ...), wherever Cloudberry
 * lets them be written, each becoming a statement PostgreSQL has -- one
 * statement for one, and not the statement followed by calls.  A string of
 * statements is not one statement: it cannot be prepared, so a driver on the
 * extended protocol got "cannot insert multiple commands into a prepared
 * statement"; it runs as one implicit transaction, so CREATE DATABASE and
 * CREATE TABLESPACE refused to run in it; and the calls' rows came back as a
 * result nobody asked for.  So:
 *
 *   - on CREATE TABLE, CREATE TABLE AS, CREATE [MATERIALIZED] VIEW and CREATE
 *     INDEX, the tags are options of the statement, gp_tag.env = 'prod' in its
 *     WITH list (rw_add_option), which gp_sql's ProcessUtility hook takes out
 *     again and puts on the relation once it exists;
 *   - on CREATE DATABASE and CREATE TABLESPACE, the same, as each statement
 *     takes an option (tag_option);
 *   - on ALTER of a table, view, materialized view or index, TAG (...) is SET
 *     (gp_tag....) and UNSET TAG (...) is RESET (gp_tag....), in place, which
 *     the hook takes out of ALTER TABLE the same way;
 *   - and where no statement has a place for them -- a schema, a role, a
 *     sequence, a foreign table, and ALTER of a database or tablespace -- the
 *     tags are calls to gp_sql's setters, made in one SELECT: the statement
 *     itself when an ALTER is all tags, and after it otherwise, which for
 *     CREATE SCHEMA, CREATE USER, CREATE SEQUENCE and CREATE FOREIGN TABLE is
 *     still two statements.
 *
 * On CREATE SCHEMA, Cloudberry's grammar has WITH TAG (...) and nothing else,
 * so a bare TAG there is left for PostgreSQL to refuse, as Cloudberry does.
 */
static void
rw_tag_clauses(GpRewrite *rw, GpSubjKind kind, const char *name, int from)
{
	const GpTokens *ts = rw->ts;
	int			depth = 0;
	const char *arg = subject_argument(kind, name);
	bool		creating = tok_is(ts, rw->first, "create");
	bool		relopts = (kind == GP_SUBJ_RELATION && rw->object != 0 &&
						   strchr("tvmi", rw->object) != NULL);
	bool		as_options = creating &&
		(relopts || kind == GP_SUBJ_DATABASE || kind == GP_SUBJ_TABLESPACE);
	bool		in_place = !creating && relopts;
	int			with_close = -1;	/* a tablespace's WITH list's ')' */

	for (int i = from; i < rw->last; i++)
	{
		bool		unset;
		int			open;
		int			j;
		int			start = i;
		StringInfoData opts;

		if (tok_is_char(ts, i, '('))
		{
			if (depth == 0 && as_options && kind == GP_SUBJ_TABLESPACE &&
				tok_is(ts, i - 1, "with"))
				with_close = skip_parens(ts, i) - 1;
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

		if (kind == GP_SUBJ_SCHEMA && creating)
		{
			if (unset || i == from || !tok_is(ts, i - 1, "with"))
				continue;
			start = i - 1;		/* WITH goes with it */
		}

		/* Not Cloudberry's grammar either; PostgreSQL refuses it. */
		if (as_options && unset)
			continue;

		/*
		 * Look before cutting.  TAG ( ... ) is Cloudberry's clause only when
		 * what is in it reads like one; tag(x) in the query of a CREATE TABLE
		 * AS is a function call, and has to be left where it is.
		 */
		if (!unset)
		{
			if (!tok_is_name(ts, open + 1) || !tok_is_char(ts, open + 2, '=') ||
				!tok_is_string(ts, open + 3))
				continue;
		}
		else if (!tok_is_name(ts, open + 1))
			continue;

		/* Read the pairs. */
		initStringInfo(&opts);
		j = open + 1;
		while (j < rw->last && tok_is_name(ts, j))
		{
			char	   *key = tok_name(ts, j);

			j++;
			if (unset && in_place)
				appendStringInfo(&opts, "%sgp_tag.%s", opts.len > 0 ? ", " : "",
								 quote_identifier(key));
			else if (unset)
				rw_add_call(rw, psprintf("%s(%s, %s)", subject_setter(kind, true),
										 arg, quote_literal_cstr(key)),
							ts->toks[start].off);
			else
			{
				const char *value;

				if (!tok_is_char(ts, j, '=') || !tok_is_string(ts, j + 1))
					break;
				value = ts->toks[j + 1].str;
				if (as_options &&
					(kind == GP_SUBJ_DATABASE || kind == GP_SUBJ_TABLESPACE))
					tag_option(&opts, kind, key, value);
				else if (as_options || in_place)
				{
					char	   *option = psprintf("gp_tag.%s = %s", quote_identifier(key),
												  quote_literal_cstr(value));

					if (in_place)
						appendStringInfo(&opts, "%s%s", opts.len > 0 ? ", " : "", option);
					else
						rw_add_option(rw, option);
				}
				else
					rw_add_call(rw, psprintf("%s(%s, %s, %s)",
											 subject_setter(kind, false), arg,
											 quote_literal_cstr(key),
											 quote_literal_cstr(value)),
								ts->toks[start].off);
				j += 2;
			}

			if (!tok_is_char(ts, j, ','))
				break;
			j++;
		}

		/*
		 * Take the clause out of the statement, or put what it became in its
		 * place: SET or RESET of an ALTER, a database's options, or a
		 * tablespace's WITH list, into the one it has if it has one.
		 */
		{
			int			after = skip_parens(ts, open);
			const char *replacement = " ";

			if (in_place)
				replacement = psprintf("%s (%s) ", unset ? "RESET" : "SET", opts.data);
			else if (as_options && kind == GP_SUBJ_TABLESPACE && with_close >= 0)
				rw_edit(rw, ts->toks[with_close].off, ts->toks[with_close].off,
						psprintf(", %s", opts.data));
			else if (as_options && kind == GP_SUBJ_TABLESPACE)
				replacement = psprintf(" WITH (%s) ", opts.data);
			else if (as_options && kind == GP_SUBJ_DATABASE)
				replacement = psprintf("%s ", opts.data);

			rw_edit(rw, ts->toks[start].off,
					(after < ts->ntoks) ? ts->toks[after].off : ts->srclen,
					replacement);
			if (!in_place)
			{
				if (rw->tag_first < 0)
					rw->tag_first = start;
				rw->tag_last = after;
			}
			i = after - 1;
			depth = 0;
		}
	}
}

/*
 * DISTRIBUTED BY (a, b) / DISTRIBUTED RANDOMLY / DISTRIBUTED REPLICATED
 *
 * The policy is recorded on the table; what reads it is ORCA's relcache
 * translator, which asks every relation what it is distributed by, and the
 * dispatch of M2.  On one node every table is on the one node, so nothing
 * changes for the statement itself.  On CREATE TABLE, CREATE TABLE AS and
 * CREATE MATERIALIZED VIEW the policy is an option of the statement,
 * gp.distributed_by = '(a,b)', which gp_sql's ProcessUtility hook takes out
 * and records once the table exists, so that the statement stays one (see
 * rw_tag_clauses for why that matters); elsewhere -- a foreign table -- it is
 * a call to gp_sql.set_distribution after the statement.
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
rw_distribution(GpRewrite *rw, const char *name, const char *policy, int at)
{
	if (tok_is(rw->ts, rw->first, "create") && rw->object != 0 &&
		strchr("tm", rw->object) != NULL)
		rw_add_option(rw, psprintf("gp.distributed_by = %s",
								   quote_literal_cstr(policy)));
	else
		rw_add_call(rw, psprintf("gp_sql.set_distribution(%s::regclass, %s)",
								 quote_literal_cstr(name),
								 quote_literal_cstr(policy)),
					at);
}

static void
rw_distributed(GpRewrite *rw, const char *name, int from)
{
	const GpTokens *ts = rw->ts;
	int			depth = 0;

	for (int i = from; i < rw->last; i++)
	{
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
		if (depth != 0 || !tok_is(ts, i, "distributed"))
			continue;

		if (tok_is(ts, i + 1, "randomly") || tok_is(ts, i + 1, "replicated"))
		{
			rw_distribution(rw, name,
							tok_is(ts, i + 1, "randomly") ? "random" : "replicated",
							ts->toks[i].off);
			rw_edit(rw, ts->toks[i].off,
					(i + 2 < ts->ntoks) ? ts->toks[i + 2].off : ts->srclen, " ");
			i++;
		}
		else if (tok_is(ts, i + 1, "by") && tok_is_char(ts, i + 2, '('))
		{
			int			after = skip_parens(ts, i + 2);
			StringInfoData cols;
			bool		first = true;

			initStringInfo(&cols);
			appendStringInfoChar(&cols, '(');
			for (int j = i + 3; j < after - 1; j++)
			{
				if (tok_is_char(ts, j, ','))
					continue;
				if (!tok_is_name(ts, j))
					continue;
				if (!first)
					appendStringInfoChar(&cols, ',');
				appendStringInfoString(&cols, quote_identifier(tok_name(ts, j)));
				first = false;
			}
			appendStringInfoChar(&cols, ')');

			rw_distribution(rw, name, cols.data, ts->toks[i].off);
			rw_edit(rw, ts->toks[i].off,
					(after < ts->ntoks) ? ts->toks[after].off : ts->srclen, " ");
			i = after - 1;
		}

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
	int			depth = 0;

	if (!tok_is(ts, i, "create"))
		return false;

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
	rw_add_option(rw, option);
	return true;
}

/*
 * ALTER USER u PROFILE p / NOPROFILE / ACCOUNT LOCK / ACCOUNT UNLOCK
 *
 * Only on its own: these are role options in Cloudberry's grammar, and one
 * mixed in with PostgreSQL's own options has no single statement to become.
 */
static bool
rw_role_profile(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			i = rw->first;
	char	   *name;

	if (!tok_is(ts, i, "alter") ||
		!(tok_is(ts, i + 1, "user") || tok_is(ts, i + 1, "role")))
		return false;
	if (!tok_is_name(ts, i + 2))
		return false;

	name = tok_name(ts, i + 2);
	i += 3;

	rw_whole(rw);

	if (tok_is(ts, i, "profile") && tok_is_name(ts, i + 1) && i + 2 == rw->last)
		appendStringInfo(&rw->body, "SELECT gp_security.assign_profile(%s, %s)",
						 quote_literal_cstr(name),
						 quote_literal_cstr(tok_name(ts, i + 1)));
	else if (tok_is(ts, i, "noprofile") && i + 1 == rw->last)
		appendStringInfo(&rw->body, "SELECT gp_security.assign_profile(%s, NULL)",
						 quote_literal_cstr(name));
	else if (tok_is(ts, i, "account") && tok_is(ts, i + 1, "lock") && i + 2 == rw->last)
		appendStringInfo(&rw->body, "SELECT gp_security.lock_role(%s)",
						 quote_literal_cstr(name));
	else if (tok_is(ts, i, "account") && tok_is(ts, i + 1, "unlock") && i + 2 == rw->last)
		appendStringInfo(&rw->body, "SELECT gp_security.unlock_role(%s)",
						 quote_literal_cstr(name));
	else
		return false;

	return true;
}

/* ------------------------------------------------------------------------- */
/* Function attributes: where a function may run, and what it does with SQL   */
/* ------------------------------------------------------------------------- */

/*
 * The head of CREATE [OR REPLACE] FUNCTION|PROCEDURE, or of ALTER FUNCTION|
 * PROCEDURE: the object type, the signature written the way SECURITY LABEL
 * takes it, and where the option list begins.
 *
 * The signature goes to SECURITY LABEL as text rather than through
 * regprocedure, and that is deliberate.  PostgreSQL resolves a function
 * signature for SECURITY LABEL by the same rules as for ALTER FUNCTION, so
 * "f(a int, OUT b int)" finds f; regprocedure parses a bare type list and
 * rejects both the parameter names and the OUT.  Letting PostgreSQL parse
 * what the user wrote is the only way to accept everything it would.
 */
static bool
rw_function_head(GpRewrite *rw, const char **objtype, StringInfo sig,
				 int *sig_end, bool *altering)
{
	const GpTokens *ts = rw->ts;
	int			i = rw->first;
	int			sig_first;

	if (tok_is(ts, i, "create"))
	{
		*altering = false;
		i++;
		if (tok_is(ts, i, "or") && tok_is(ts, i + 1, "replace"))
			i += 2;
	}
	else if (tok_is(ts, i, "alter"))
	{
		*altering = true;
		i++;
	}
	else
		return false;

	if (tok_is(ts, i, "function"))
		*objtype = "FUNCTION";
	else if (tok_is(ts, i, "procedure"))
		*objtype = "PROCEDURE";
	else
		return false;
	i++;

	sig_first = i;
	i = skip_qualified_name(ts, i);
	if (i == sig_first)
		return false;

	initStringInfo(sig);
	appendStringInfoString(sig, rw_text(ts, sig_first, i));

	/*
	 * The parameter list, rewritten into the form SECURITY LABEL will take.
	 * A default belongs to CREATE FUNCTION and not to a signature, so
	 * "a int DEFAULT 5" has to become "a int"; everything else is copied,
	 * including OUT parameters, which PostgreSQL ignores when it looks a
	 * function up.  ALTER FUNCTION is already written this way and passes
	 * through unchanged.
	 */
	if (tok_is_char(ts, i, '('))
	{
		int			close = skip_parens(ts, i);
		int			inner = 0;
		bool		skipping = false;
		bool		first_in_group = true;

		appendStringInfoChar(sig, '(');
		for (int j = i + 1; j < close - 1; j++)
		{
			if (tok_is_char(ts, j, '('))
				inner++;
			else if (tok_is_char(ts, j, ')'))
				inner--;

			if (inner == 0 && tok_is_char(ts, j, ','))
			{
				appendStringInfoString(sig, ", ");
				skipping = false;
				first_in_group = true;
				continue;
			}

			if (inner == 0 &&
				(tok_is(ts, j, "default") || tok_is_char(ts, j, '=')))
				skipping = true;

			if (skipping)
				continue;

			if (!first_in_group && !tok_is_char(ts, j, ',') &&
				!tok_is_char(ts, j, '(') && !tok_is_char(ts, j, ')') &&
				!tok_is_char(ts, j, '[') && !tok_is_char(ts, j, ']'))
				appendStringInfoChar(sig, ' ');
			appendStringInfoString(sig, rw_text(ts, j, j + 1));
			first_in_group = false;
		}
		appendStringInfoChar(sig, ')');
		*sig_end = close;
	}
	else
	{
		/* ALTER FUNCTION f EXECUTE ON ANY: PostgreSQL allows a bare name. */
		*sig_end = i;
	}

	return true;
}

/*
 * EXECUTE ON ANY | COORDINATOR | MASTER | INITPLAN | ALL SEGMENTS, and the
 * data-access attributes NO SQL | CONTAINS SQL | READS SQL DATA | MODIFIES
 * SQL DATA, on CREATE [OR REPLACE] FUNCTION and PROCEDURE and on ALTER
 * FUNCTION and PROCEDURE.
 *
 *	 CREATE FUNCTION f(int) RETURNS int ... EXECUTE ON ALL SEGMENTS
 *	   -> CREATE FUNCTION f(int) RETURNS int ...
 *	      ; SECURITY LABEL FOR gp ON FUNCTION f(int) IS 'execute_on=all_segments'
 *
 * The execute_on label is what func_exec_location() reads, and ORCA asks it of
 * every function it meets.
 *
 * THE TWO FAMILIES ARE READ TOGETHER, in one pass, and write one label.  They
 * have to be: SECURITY LABEL *replaces* a provider's label rather than merging
 * into it, so two statements would leave only the second key.  Writing both in
 * one label is also why the keys come out in a fixed order rather than the
 * order they were written -- two spellings of the same function then produce
 * the same label.
 *
 * WHAT DATA ACCESS IS FOR.  Nothing reads it, and that is not a gap in the
 * port: Cloudberry writes pg_proc.prodataaccess, dumps it, and reads it
 * nowhere outside the DDL path -- no planner, executor or dispatcher decision
 * turns on it.  Its whole observable behaviour is the three rules below, from
 * validate_sql_data_access() in Cloudberry's functioncmds.c, and they are
 * checked here rather than in the label provider because Cloudberry rejects
 * the statement before the function is created, and a check on the SECURITY
 * LABEL that follows would reject it after.
 *
 * The default is not written down.  Cloudberry fills in CONTAINS SQL for a
 * LANGUAGE SQL function and NO SQL for everything else at DDL time; here the
 * absence of the key means exactly that, so an ordinary function carries no
 * label at all.
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
	const char *objtype;
	StringInfoData sig;
	int			sig_end;
	bool		altering;
	int			depth = 0;
	const char *exec_on = NULL;
	const char *data_access = NULL;
	bool		saw_immutable = false;
	bool		saw_language_sql = false;
	int			covered_from = -1;
	int			covered_to = -1;
	int			covered_tokens = 0;

	if (!rw_function_head(rw, &objtype, &sig, &sig_end, &altering))
		return false;

	/*
	 * One pass over the option list.  It is written among the function's
	 * other options, which is depth 0; a SQL-standard body is where real SQL
	 * tokens start and nothing of Cloudberry's follows, so stop there.
	 */
	for (int j = sig_end; j < rw->last; j++)
	{
		int			after = -1;
		const char *key_value = NULL;
		bool		is_exec = false;

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

		/* Remembered for the three validation rules below. */
		if (tok_is(ts, j, "immutable"))
			saw_immutable = true;
		if (tok_is(ts, j, "language") && tok_is(ts, j + 1, "sql"))
			saw_language_sql = true;

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
				key_value = locations[k].value;
				is_exec = true;
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
				key_value = accesses[k].value;
				break;
			}
		}

		if (key_value == NULL)
			continue;

		if (is_exec)
			exec_on = key_value;
		else
			data_access = key_value;

		rw_edit(rw, ts->toks[j].off,
				(after < ts->ntoks) ? ts->toks[after].off : ts->srclen, " ");

		if (covered_from < 0)
			covered_from = j;
		covered_to = after;
		covered_tokens += after - j;
		j = after - 1;
	}

	if (exec_on == NULL && data_access == NULL)
		return false;

	/*
	 * Cloudberry's three rules, from validate_sql_data_access().  They are
	 * the whole of what the attribute does.
	 */
	if (data_access != NULL && saw_immutable &&
		(strcmp(data_access, "reads") == 0 ||
		 strcmp(data_access, "modifies") == 0))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("conflicting options"),
				 errhint("IMMUTABLE conflicts with %s SQL DATA.",
						 strcmp(data_access, "reads") == 0 ? "READS" : "MODIFIES")));

	if (data_access != NULL && saw_language_sql &&
		strcmp(data_access, "none") == 0)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("conflicting options"),
				 errhint("A SQL function cannot specify NO SQL.")));

	appendStringInfo(&rw->after, "; SECURITY LABEL FOR gp ON %s %s IS '",
					 objtype, sig.data);
	if (exec_on != NULL)
		appendStringInfo(&rw->after, "execute_on=%s", exec_on);
	if (exec_on != NULL && data_access != NULL)
		appendStringInfoChar(&rw->after, ',');
	if (data_access != NULL)
		appendStringInfo(&rw->after, "data_access=%s", data_access);
	appendStringInfoChar(&rw->after, '\'');

	/*
	 * ALTER FUNCTION f(int) EXECUTE ON ANY is a whole statement of
	 * Cloudberry's, not an action on one of PostgreSQL's: take the clause out
	 * and there is no action left, which ALTER FUNCTION will not accept.  So
	 * when the clauses are the entire action list, the label replaces the
	 * statement rather than following it.  Written beside another action --
	 * ALTER FUNCTION f(int) STRICT EXECUTE ON ANY -- the ALTER stays and does
	 * the rest.  This is the same shape as ALTER TABLE t TAG (...).
	 */
	if (altering && covered_from == sig_end && covered_to == rw->last &&
		covered_tokens == rw->last - sig_end)
		rw_whole(rw);

	return true;
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
 * The token that closes the bracket at `open`, counting ( ) and [ ] together,
 * or -1 if it is not closed before `limit`.
 */
static int
match_close(const GpTokens *ts, int open, int limit)
{
	int			depth = 0;

	for (int j = open; j < limit; j++)
	{
		int			c = ts->toks[j].code;

		if (c == '(' || c == '[')
			depth++;
		else if (c == ')' || c == ']')
		{
			if (--depth == 0)
				return j;
			if (depth < 0)
				return -1;
		}
	}
	return -1;
}

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
	return case_close(sc, i, limit);
}

static void
emit_construct(GpOut *o, const GpExprScan *sc, int i, int stop)
{
	if (tok_is_kw(sc->ts, i, "case"))
		emit_case(o, sc, i, stop);
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

static void
rw_statement(GpRewrite *rw)
{
	GpSubjKind	kind;
	char	   *name = NULL;
	int			after_name = rw->first;

	/* Statements that become something else entirely. */
	if (rw_create_tag(rw) || rw_alter_tag(rw) || rw_drop_tag(rw) ||
		rw_profile(rw) || rw_drop_profile(rw) ||
		rw_create_directory_table(rw) ||
		rw_task(rw) || rw_drop_task(rw) ||
		rw_role_profile(rw))
	{
		/* A statement of Cloudberry's own may still carry a TAG clause. */
		return;
	}

	(void) rw_storage_and_dynamic(rw);
	(void) rw_matview_options(rw);
	(void) rw_function_clauses(rw);

	kind = find_subject(rw->ts, rw->first, rw->last, &name, &after_name,
						&rw->object);
	if (kind != GP_SUBJ_NONE)
	{
		rw->subject_end = after_name;
		rw_tag_clauses(rw, kind, name, after_name);
		if (kind == GP_SUBJ_RELATION)
			rw_distributed(rw, name, after_name);

		/*
		 * ALTER SCHEMA s TAG (...) is a whole statement of Cloudberry's, not a
		 * clause on one of PostgreSQL's, so with the clause taken out there
		 * is no ALTER left to run, only the calls.  (ALTER TABLE t TAG (...)
		 * became ALTER TABLE t SET (...) in place, and is still an ALTER.)
		 */
		if (tok_is(rw->ts, rw->first, "alter") &&
			rw->tag_first == after_name && rw->tag_last == rw->last)
			rw_whole(rw);
	}

	/* DECODE and CASE ... WHEN IS NOT DISTINCT FROM, wherever they are. */
	if (!rw->whole)
		rw_expressions(rw, true);

	/* The options the clauses became, all into one WITH list. */
	rw_place_options(rw);
}

/*
 * The rewrite of `str`, or NULL when there is nothing of Cloudberry's in it.
 *
 * With expr_only, `str` is not a statement but what PL/pgSQL hands the
 * parser for an expression or an assignment, where only an expression of
 * Cloudberry's can be.  With map, *map says where each byte of the result
 * came from.
 */
static char *
desugar(const char *str, bool expr_only, GpPosMap **map)
{
	GpTokens   *ts;
	GpOut		out;
	int			depth = 0;
	int			first = 0;
	bool		changed = false;

	if (str == NULL || !looks_interesting(str))
		return NULL;

	ts = gp_tokenize(str);
	if (ts->ntoks == 0)
		return NULL;

	out_init(&out, str);

	/* Anything before the first token: a leading comment. */
	out_copy(&out, 0, ts->toks[0].off);

	for (int i = 0; i <= ts->ntoks; i++)
	{
		GpRewrite	rw;
		bool		at_end = (i == ts->ntoks);

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

		{
			/*
			 * What the rewrite writes in place of the statement, and the
			 * statements it adds after it, stand for the statement's start,
			 * so that what pg_stat_statements records of an added statement
			 * is the statement it came from; each call in one stands for the
			 * clause of the user's it was made from.
			 */
			int			start = ts->toks[first].off;
			const char *body = rw.whole ? rw.body.data : rw.text.buf.data;
			bool		emitted = false;	/* written anything of it yet? */

			for (int k = 0; body[k] != '\0' && !emitted; k++)
				emitted = (body[k] != ' ' && body[k] != '\t' &&
						   body[k] != '\n' && body[k] != '\r');
			if (rw.whole)
				out_text(&out, rw.body.data, start);
			else
				out_append(&out, &rw.text);

			/* The calls, in one SELECT: the statement, if it is nothing else. */
			if (rw.calls != NIL)
			{
				ListCell   *lc;
				ListCell   *lc2;

				out_text(&out, emitted ? "; SELECT " : "SELECT ", start);
				forboth(lc, rw.calls, lc2, rw.call_at)
				{
					if (lc != list_head(rw.calls))
						out_text(&out, ", ", start);
					out_text(&out, (const char *) lfirst(lc), lfirst_int(lc2));
				}
				emitted = true;
			}

			if (rw.after.len > 0)
			{
				const char *a = rw.after.data;

				/* "; SECURITY LABEL ..." after nothing is just that. */
				if (!emitted && a[0] == ';')
					a += 2;
				out_text(&out, a, start);
			}
		}
		changed |= rw.changed || rw.after.len > 0 || rw.calls != NIL;

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
	return desugar(str, false, NULL);
}

char *
GpDesugarMapped(const char *str, bool expr_only, GpPosMap **map)
{
	return desugar(str, expr_only, map);
}

/* ------------------------------------------------------------------------- */
/* O26                                                                       */
/* ------------------------------------------------------------------------- */

static raw_parser_hook_type prev_raw_parser = NULL;

typedef struct GpParseErrorArg
{
	const char *original;		/* what the caller asked to parse */
	const char *rewritten;		/* what the grammar read */
	const GpPosMap *map;
} GpParseErrorArg;

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
static void
gp_parse_error_callback(void *arg)
{
	GpParseErrorArg *a = (GpParseErrorArg *) arg;
	int			pos = geterrposition();
	int			offset = 0;

	if (pos <= 0)
		return;

	for (int c = 1; c < pos && a->rewritten[offset] != '\0'; c++)
		offset += pg_mblen_cstr(a->rewritten + offset);

	offset = GpPosMapSource(a->map, offset);
	errposition(pg_mbstrlen_with_len(a->original, offset) + 1);
}

static List *
gp_raw_parser(const char *str, RawParseMode mode)
{
	char	   *rewritten = NULL;
	GpPosMap   *map = NULL;
	GpParseErrorArg errarg;
	ErrorContextCallback errcallback;
	List	   *result;

	/*
	 * A whole statement can hold anything of Cloudberry's.  The PL/pgSQL
	 * modes parse an expression or an assignment, which can hold a DECODE or
	 * a CASE ... WHEN IS NOT DISTINCT FROM and nothing else of Cloudberry's;
	 * a type name holds nothing.
	 */
	if (mode == RAW_PARSE_DEFAULT)
		rewritten = GpDesugarMapped(str, false, &map);
	else if (mode != RAW_PARSE_TYPE_NAME)
		rewritten = GpDesugarMapped(str, true, &map);

	if (rewritten == NULL)
	{
		if (prev_raw_parser)
			return prev_raw_parser(str, mode);
		return standard_raw_parser(str, mode);
	}

	errarg.original = str;
	errarg.rewritten = rewritten;
	errarg.map = map;
	errcallback.callback = gp_parse_error_callback;
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
 * that a person debugging one can see it.
 */
Datum
gp_sql_desugar(PG_FUNCTION_ARGS)
{
	char	   *str = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *out = GpDesugar(str);

	PG_RETURN_TEXT_P(cstring_to_text(out != NULL ? out : str));
}
