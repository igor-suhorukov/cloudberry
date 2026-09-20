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
 * What a fork would give that this does not:
 *
 *   - error positions inside a rewritten clause point at the rewritten text,
 *     not at what the user wrote;
 *   - a comment written inside a clause that is replaced is dropped with it;
 *   - Cloudberry syntax nested inside an expression is not reached, because
 *     this recognises statements and clauses, not expressions.  Nothing in
 *     the surface below is ever nested that way.
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
#include "utils/builtins.h"

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
	"execute",
	NULL
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
	StringInfoData body;		/* the statement as it will be run */
	StringInfoData after;		/* statements to run after it */
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
	initStringInfo(&rw->after);
}

/* Replace [from, to) with `text`.  Edits may be found in any order. */
static void
rw_edit(GpRewrite *rw, int from, int to, const char *text)
{
	GpEdit	   *e = palloc(sizeof(GpEdit));

	e->from = from;
	e->to = to;
	e->text = text ? pstrdup(text) : pstrdup("");
	rw->edits = lappend(rw->edits, e);
	rw->changed = true;
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
	return 0;
}

/* Put the statement together: the source, with the edits applied in order. */
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
		if (e->from > copied)
			appendBinaryStringInfo(&rw->body, ts->src + copied, e->from - copied);
		appendStringInfoString(&rw->body, e->text);
		copied = e->to;
	}

	if (end > copied)
		appendBinaryStringInfo(&rw->body, ts->src + copied, end - copied);
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

/* DROP TAG [IF EXISTS] a, b -> one call each */
static bool
rw_drop_tag(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			i = rw->first;
	bool		missing_ok = false;
	List	   *names = NIL;
	ListCell   *lc;
	bool		first = true;

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
	{
		if (!first)
			appendStringInfoString(&rw->body, "; ");
		appendStringInfo(&rw->body, "SELECT gp_sql.drop_tag(%s, %s)",
						 quote_literal_cstr((char *) lfirst(lc)),
						 missing_ok ? "true" : "false");
		first = false;
	}
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
	bool		first = true;

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
	{
		if (!first)
			appendStringInfoString(&rw->body, "; ");
		appendStringInfo(&rw->body, "SELECT gp_security.drop_profile(%s, %s)",
						 quote_literal_cstr((char *) lfirst(lc)),
						 missing_ok ? "true" : "false");
		first = false;
	}
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
	bool		first = true;

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
	{
		if (!first)
			appendStringInfoString(&rw->body, "; ");
		appendStringInfo(&rw->body, "SELECT gp_task.drop_task(%s, %s)",
						 quote_literal_cstr((char *) lfirst(lc)),
						 missing_ok ? "true" : "false");
		first = false;
	}
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
 */
static GpSubjKind
find_subject(const GpTokens *ts, int first, int last, char **name, int *after)
{
	int			i = first;
	GpSubjKind	kind = GP_SUBJ_NONE;
	int			e;

	if (tok_is(ts, i, "create"))
	{
		i++;
		/* everything CREATE allows before the object's kind */
		while (i < last &&
			   (tok_is(ts, i, "or") || tok_is(ts, i, "replace") ||
				tok_is(ts, i, "global") || tok_is(ts, i, "local") ||
				tok_is(ts, i, "temp") || tok_is(ts, i, "temporary") ||
				tok_is(ts, i, "unlogged") || tok_is(ts, i, "recursive") ||
				tok_is(ts, i, "foreign")))
			i++;
	}
	else if (tok_is(ts, i, "alter"))
		i++;
	else
		return GP_SUBJ_NONE;

	if (tok_is(ts, i, "table"))
		kind = GP_SUBJ_RELATION;
	else if (tok_is(ts, i, "view") || tok_is(ts, i, "sequence"))
		kind = GP_SUBJ_RELATION;
	else if (tok_is(ts, i, "materialized") && tok_is(ts, i + 1, "view"))
	{
		kind = GP_SUBJ_RELATION;
		i++;
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
 * TAG (name = 'value', ...) and UNSET TAG (name, ...), wherever Cloudberry
 * lets them be written.  Each becomes a call after the statement, which is
 * what lets them be written on a statement whose own clauses are in a fixed
 * order.
 */
static void
rw_tag_clauses(GpRewrite *rw, GpSubjKind kind, const char *name, int from)
{
	const GpTokens *ts = rw->ts;
	int			depth = 0;
	const char *arg = subject_argument(kind, name);

	for (int i = from; i < rw->last; i++)
	{
		bool		unset;
		int			open;
		int			j;

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
		j = open + 1;
		while (j < rw->last && tok_is_name(ts, j))
		{
			char	   *key = tok_name(ts, j);

			j++;
			if (unset)
				appendStringInfo(&rw->after, "; SELECT %s(%s, %s)",
								 subject_setter(kind, true), arg,
								 quote_literal_cstr(key));
			else
			{
				if (!tok_is_char(ts, j, '=') || !tok_is_string(ts, j + 1))
					break;
				appendStringInfo(&rw->after, "; SELECT %s(%s, %s, %s)",
								 subject_setter(kind, false), arg,
								 quote_literal_cstr(key),
								 quote_literal_cstr(ts->toks[j + 1].str));
				j += 2;
			}

			if (!tok_is_char(ts, j, ','))
				break;
			j++;
		}

		/* Take the clause out of the statement. */
		{
			int			after = skip_parens(ts, open);

			rw_edit(rw, ts->toks[i].off,
					(after < ts->ntoks) ? ts->toks[after].off : ts->srclen, " ");
			if (rw->tag_first < 0)
				rw->tag_first = i;
			rw->tag_last = after;
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
 * changes for the statement itself.
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
			appendStringInfo(&rw->after, "; SELECT gp_sql.set_distribution(%s::regclass, %s)",
							 quote_literal_cstr(name),
							 quote_literal_cstr(tok_is(ts, i + 1, "randomly")
												? "random" : "replicated"));
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

			appendStringInfo(&rw->after, "; SELECT gp_sql.set_distribution(%s::regclass, %s)",
							 quote_literal_cstr(name),
							 quote_literal_cstr(cols.data));
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
 * before the view is made.  So the option is put where PostgreSQL's grammar
 * expects one -- merged into a WITH that is already there, or in a new one
 * just before TABLESPACE or AS, whichever comes first.
 */
static bool
rw_matview_options(GpRewrite *rw)
{
	const GpTokens *ts = rw->ts;
	int			i = rw->first;
	const char *option = NULL;
	char	   *schedule = NULL;
	int			depth = 0;
	int			with_open = -1;
	int			insert_at = -1;

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

	/* Find the SCHEDULE clause, an existing WITH, and where AS begins. */
	for (int j = i; j < rw->last; j++)
	{
		if (tok_is_char(ts, j, '('))
		{
			if (depth == 0 && with_open == -1 && tok_is(ts, j - 1, "with"))
				with_open = j;
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

		if (tok_is(ts, j, "tablespace") || tok_is(ts, j, "as"))
		{
			insert_at = ts->toks[j].off;
			break;
		}
	}

	if (option == NULL)
		option = psprintf("gp.dynamic_schedule = %s",
						  quote_literal_cstr(schedule != NULL ? schedule : "*/5 * * * *"));

	if (with_open >= 0)
		rw_edit(rw, tok_end(ts, with_open), tok_end(ts, with_open),
				psprintf("%s, ", option));
	else if (insert_at >= 0)
		rw_edit(rw, insert_at, insert_at, psprintf("WITH (%s) ", option));
	else
		return false;

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

/*
 * EXECUTE ON ANY | COORDINATOR | MASTER | INITPLAN | ALL SEGMENTS, on
 * CREATE [OR REPLACE] FUNCTION and PROCEDURE and on ALTER FUNCTION and
 * PROCEDURE.
 *
 *	 CREATE FUNCTION f(int) RETURNS int ... EXECUTE ON ALL SEGMENTS
 *	   -> CREATE FUNCTION f(int) RETURNS int ...
 *	      ; SECURITY LABEL FOR gp ON FUNCTION f(int) IS 'execute_on=all_segments'
 *
 * The label is what func_exec_location() reads, and ORCA asks it of every
 * function it meets, so this closes the loop between the syntax and the
 * reader that has been waiting for it.
 *
 * WHAT THIS DOES NOT COVER, and why: the data-access attributes that share
 * Cloudberry's grammar production -- NO SQL, CONTAINS SQL, READS SQL DATA,
 * MODIFIES SQL DATA.  Nothing reads pg_proc.prodataaccess: Cloudberry
 * validates it at DDL time and dumps it, and no planner or executor decision
 * turns on it.  Recognising them would also cost the rewriter a trigger word
 * for "NO SQL", whose only distinctive token is "no" -- a word common enough
 * in ordinary SQL that every statement holding it would be tokenised for
 * nothing.  They stay a syntax error until something needs them.
 */
static bool
rw_execute_on(GpRewrite *rw)
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
	int			i = rw->first;
	const char *objtype;
	int			sig_first;
	int			sig_end;
	StringInfoData sig;
	int			depth = 0;
	bool		found = false;

	/* CREATE [OR REPLACE] FUNCTION|PROCEDURE, or ALTER FUNCTION|PROCEDURE. */
	if (tok_is(ts, i, "create"))
	{
		i++;
		if (tok_is(ts, i, "or") && tok_is(ts, i + 1, "replace"))
			i += 2;
	}
	else if (tok_is(ts, i, "alter"))
		i++;
	else
		return false;

	if (tok_is(ts, i, "function"))
		objtype = "FUNCTION";
	else if (tok_is(ts, i, "procedure"))
		objtype = "PROCEDURE";
	else
		return false;
	i++;

	sig_first = i;
	i = skip_qualified_name(ts, i);
	if (i == sig_first)
		return false;

	initStringInfo(&sig);
	appendStringInfoString(&sig, rw_text(ts, sig_first, i));

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

		appendStringInfoChar(&sig, '(');
		for (int j = i + 1; j < close - 1; j++)
		{
			if (tok_is_char(ts, j, '('))
				inner++;
			else if (tok_is_char(ts, j, ')'))
				inner--;

			if (inner == 0 && tok_is_char(ts, j, ','))
			{
				appendStringInfoString(&sig, ", ");
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
				appendStringInfoChar(&sig, ' ');
			appendStringInfoString(&sig, rw_text(ts, j, j + 1));
			first_in_group = false;
		}
		appendStringInfoChar(&sig, ')');
		sig_end = close;
	}
	else
	{
		/* ALTER FUNCTION f EXECUTE ON ANY: PostgreSQL allows a bare name. */
		sig_end = i;
	}

	/*
	 * Now look for the clause.  It is written among the function's other
	 * options, which is depth 0; a SQL-standard body is where real SQL
	 * tokens start and nothing of Cloudberry's follows, so stop there.
	 */
	for (int j = sig_end; j < rw->last; j++)
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
		if (tok_is(ts, j, "begin"))
			break;
		if (!tok_is(ts, j, "execute") || !tok_is(ts, j + 1, "on"))
			continue;

		for (size_t k = 0; k < lengthof(locations); k++)
		{
			int			after;

			if (!tok_is(ts, j + 2, locations[k].word))
				continue;
			if (locations[k].second != NULL &&
				!tok_is(ts, j + 3, locations[k].second))
				continue;

			after = j + 3 + (locations[k].second != NULL ? 1 : 0);

			appendStringInfo(&rw->after,
							 "; SECURITY LABEL FOR gp ON %s %s IS 'execute_on=%s'",
							 objtype, sig.data, locations[k].value);
			rw_edit(rw, ts->toks[j].off,
					(after < ts->ntoks) ? ts->toks[after].off : ts->srclen, " ");
			j = after - 1;
			found = true;
			break;
		}
	}

	return found;
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
	(void) rw_execute_on(rw);

	kind = find_subject(rw->ts, rw->first, rw->last, &name, &after_name);
	if (kind != GP_SUBJ_NONE)
	{
		rw_tag_clauses(rw, kind, name, after_name);
		if (kind == GP_SUBJ_RELATION)
			rw_distributed(rw, name, after_name);

		/*
		 * ALTER TABLE t TAG (...) is a whole statement of Cloudberry's, not a
		 * clause on one of PostgreSQL's, so with the clause taken out there
		 * is no ALTER left to run.
		 */
		if (tok_is(rw->ts, rw->first, "alter") &&
			rw->tag_first == after_name && rw->tag_last == rw->last)
			rw_whole(rw);
	}
}

char *
GpDesugar(const char *str)
{
	GpTokens   *ts;
	StringInfoData out;
	int			depth = 0;
	int			first = 0;
	bool		changed = false;

	if (str == NULL || !looks_interesting(str))
		return NULL;

	ts = gp_tokenize(str);
	if (ts->ntoks == 0)
		return NULL;

	initStringInfo(&out);

	/* Anything before the first token: a leading comment. */
	appendBinaryStringInfo(&out, str, ts->toks[0].off);

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
				appendBinaryStringInfo(&out, str + ts->toks[i].off,
									   tok_end(ts, i) - ts->toks[i].off);
				first = i + 1;
			}
			continue;
		}

		rw_init(&rw, ts, first, i);
		rw_statement(&rw);
		rw_finish_body(&rw);

		appendStringInfoString(&out, rw.body.data);
		if (rw.after.len > 0)
		{
			const char *a = rw.after.data;
			bool		empty = true;

			for (int k = 0; k < rw.body.len; k++)
			{
				if (rw.body.data[k] != ' ' && rw.body.data[k] != '\t' &&
					rw.body.data[k] != '\n' && rw.body.data[k] != '\r')
				{
					empty = false;
					break;
				}
			}

			/* "; SELECT ..." after nothing is just "SELECT ...". */
			if (empty && a[0] == ';')
				a += 2;
			appendStringInfoString(&out, a);
		}
		changed |= rw.changed || rw.after.len > 0;

		/* The separator, and whatever trails the last statement. */
		if (!at_end)
		{
			int			upto = (i + 1 < ts->ntoks) ? ts->toks[i + 1].off : ts->srclen;

			appendBinaryStringInfo(&out, str + ts->toks[i].off,
								   upto - ts->toks[i].off);
			first = i + 1;
		}
	}

	if (!changed)
	{
		pfree(out.data);
		return NULL;
	}

	return out.data;
}

/* ------------------------------------------------------------------------- */
/* O26                                                                       */
/* ------------------------------------------------------------------------- */

static raw_parser_hook_type prev_raw_parser = NULL;

static List *
gp_raw_parser(const char *str, RawParseMode mode)
{
	char	   *rewritten = NULL;

	/*
	 * Only whole statements.  The other modes parse an expression, a type
	 * name or a PL/pgSQL fragment, and Cloudberry adds nothing to those.
	 */
	if (mode == RAW_PARSE_DEFAULT)
		rewritten = GpDesugar(str);

	if (prev_raw_parser)
		return prev_raw_parser(rewritten != NULL ? rewritten : str, mode);

	return standard_raw_parser(rewritten != NULL ? rewritten : str, mode);
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
