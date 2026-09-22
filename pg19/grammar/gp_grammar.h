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
 * gp_grammar.h
 *	  Cloudberry's SQL, rewritten into PostgreSQL 19's.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_GRAMMAR_H
#define GP_GRAMMAR_H

#include "postgres.h"

#include "nodes/pg_list.h"

/*
 * Rewrite Cloudberry's spelling of a statement into PostgreSQL 19's.
 *
 * Returns the rewritten string, or NULL when there was nothing of
 * Cloudberry's in it -- which is the common case, and costs one pass over the
 * text.  The result is palloc'd in the current context.
 */
extern char *GpDesugar(const char *str);

/*
 * Where each byte of a rewrite came from: a copy of the user's text, or text
 * the rewrite wrote, which stands for the token it replaced.
 */
typedef struct GpPosMap GpPosMap;

/*
 * GpDesugar, and where each byte of the result came from.  With expr_only,
 * str is what PL/pgSQL hands the parser for an expression or an assignment,
 * not a statement, and only DECODE and CASE ... WHEN IS NOT DISTINCT FROM
 * are looked for.
 */
extern char *GpDesugarMapped(const char *str, bool expr_only, GpPosMap **map);

/* The offset in the user's text that an offset in the rewrite stands for. */
extern int	GpPosMapSource(const GpPosMap *map, int offset);

/* The same, or -1 where the rewrite wrote the text rather than copied it. */
extern int	GpPosMapCopied(const GpPosMap *map, int offset);

/* Every location in a raw parse tree of a rewrite, put in the user's text. */
extern void GpRemapParseLocations(List *parsetree, const GpPosMap *map);

/* Installed during preload; O26's raw_parser_hook. */
extern void GpGrammarInstallHook(void);

#endif							/* GP_GRAMMAR_H */
