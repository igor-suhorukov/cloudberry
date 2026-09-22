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
 * gp_grammar_int.h
 *	  The tokens O26's rewrite reads a statement as, shared by the rewrite
 *	  (gp_desugar.c) and the parser of the classic partition clauses
 *	  (gp_partition.c).
 *
 * A statement is read with PostgreSQL's own scanner, so what counts as a
 * token here is what counts as one everywhere else.  The helpers are static
 * inline so that no name of theirs is exported from the module: PostgreSQL
 * opens modules with RTLD_GLOBAL, and a name another module exported first
 * would be the one called.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_GRAMMAR_INT_H
#define GP_GRAMMAR_INT_H

#include "postgres.h"

#include "parser/scansup.h"

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

/* Tokenise with the server's own scanner (gp_desugar.c). */
extern GpTokens *GpTokenize(const char *str);

/* Where a token ends, for the purpose of cutting text out. */
static inline int
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
static inline bool
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

/*
 * The same, but a quoted identifier is not the word: "subpartition" names
 * something, and only SUBPARTITION is Cloudberry's keyword.
 */
static inline bool
tok_is_word(const GpTokens *ts, int i, const char *word)
{
	const GpTok *t;

	if (i < 0 || i >= ts->ntoks)
		return false;

	t = &ts->toks[i];

	if (t->kw != NULL)
		return pg_strcasecmp(t->kw, word) == 0;
	if (t->code == GP_IDENT && t->str != NULL && ts->src[t->off] != '"')
		return strcmp(t->str, word) == 0;

	return false;
}

static inline bool
tok_is_char(const GpTokens *ts, int i, char c)
{
	return i >= 0 && i < ts->ntoks && ts->toks[i].code == (int) c;
}

/* The keyword `word` itself, not an identifier spelled the same, quoted. */
static inline bool
tok_is_kw(const GpTokens *ts, int i, const char *word)
{
	return i >= 0 && i < ts->ntoks && ts->toks[i].kw != NULL &&
		pg_strcasecmp(ts->toks[i].kw, word) == 0;
}

/* An identifier or keyword, as a name the rewritten text can use. */
static inline bool
tok_is_name(const GpTokens *ts, int i)
{
	if (i < 0 || i >= ts->ntoks)
		return false;
	return ts->toks[i].code == GP_IDENT || ts->toks[i].kw != NULL;
}

static inline char *
tok_name(const GpTokens *ts, int i)
{
	const GpTok *t = &ts->toks[i];

	if (t->code == GP_IDENT)
		return t->str;
	return pstrdup(t->kw);
}

static inline bool
tok_is_string(const GpTokens *ts, int i)
{
	return i >= 0 && i < ts->ntoks && ts->toks[i].code == GP_SCONST;
}

/* Skip a parenthesised group that starts at `i`; returns the index after it. */
static inline int
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
 * The token that closes the bracket at `open`, counting ( ) and [ ] together,
 * or -1 if it is not closed before `limit`.
 */
static inline int
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
 * Where token i's own text ends, which tok_end() does not say: it runs to the
 * next token, whitespace and comments included.  A single character and a
 * keyword are as long as they are; anything else ends where the whitespace
 * before the next token begins, so a comment between the two stays with the
 * first.
 */
static inline int
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

#endif							/* GP_GRAMMAR_INT_H */
