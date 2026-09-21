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
 * compat/cb_nextvalue.h
 *	  An identity column's next value, as ORCA carries it.
 *
 * ORCA has no scalar for PostgreSQL's NextValueExpr, and cannot be handed
 * nextval() in its place, so it is handed a call of a function of gp_orca's
 * own, which DXL to PlannedStmt turns back.  See nextvalue.c.
 *
 * Called through gpdb::NextValueCall, gpdb::IsNextValueFunc and
 * gpdb::NextValueFromCall, from the scalar translators.
 *
 *-------------------------------------------------------------------------
 */
#ifndef CB_NEXTVALUE_H
#define CB_NEXTVALUE_H

#include "nodes/primnodes.h"

/*
 * The call ORCA is handed for `next_value`: gp_orca's function for its type,
 * over the sequence's OID.  NULL where gp_orca's extension, which the
 * function belongs to, is not installed in this database.
 */
extern FuncExpr *gp_orca_next_value_call(const NextValueExpr *next_value);

/* Is `funcid` one of those functions? */
extern bool gp_orca_is_next_value_func(Oid funcid);

/*
 * The NextValueExpr that `call` stands for, if it is a call of one of those
 * functions; NULL if it is any other call.
 */
extern NextValueExpr *gp_orca_next_value_from_call(const FuncExpr *call);

#endif							/* CB_NEXTVALUE_H */
