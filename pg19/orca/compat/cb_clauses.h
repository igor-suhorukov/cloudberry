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
 * compat/cb_clauses.h
 *	  What Cloudberry adds to PostgreSQL's clauses.c for ORCA.
 *
 * Two functions, and both prepare a Query or an expression for translation
 * rather than answering a question about the catalogs.  They are the last
 * thing that happens to a tree before ORCA sees it.
 *
 * Named cb_clauses.h rather than optimizer/clauses.h for the reason
 * compat/cb_lsyscache.h gives: a header here named after a PostgreSQL header
 * would shadow it for the whole module.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_ORCA_COMPAT_CB_CLAUSES_H
#define GP_ORCA_COMPAT_CB_CLAUSES_H

#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"

extern Query *flatten_join_alias_var_optimizer(Query *query, int queryLevel);
extern Expr *transform_array_Const_to_ArrayExpr(Const *c);

#endif							/* GP_ORCA_COMPAT_CB_CLAUSES_H */
