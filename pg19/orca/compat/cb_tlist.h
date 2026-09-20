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
 * compat/cb_tlist.h
 *	  What Cloudberry adds to PostgreSQL's tlist.c for ORCA.
 *
 * One function.  PostgreSQL's tlist.c answers "is this expression in the
 * target list" with the first match and stops; ORCA wants every match.
 *
 * Named cb_tlist.h rather than optimizer/tlist.h for the reason
 * compat/cb_lsyscache.h gives: a header here named after a PostgreSQL header
 * would shadow it for the whole module.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_ORCA_COMPAT_CB_TLIST_H
#define GP_ORCA_COMPAT_CB_TLIST_H

#include "nodes/pg_list.h"
#include "nodes/primnodes.h"

extern List *tlist_members(Node *node, List *targetlist);

#endif							/* GP_ORCA_COMPAT_CB_TLIST_H */
