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
 * compat/cb_lsyscache.h
 *	  The catalog lookups Cloudberry adds to PostgreSQL's lsyscache.c.
 *
 * ORCA reaches the catalogs through one door, the gpdb:: wrapper layer, and
 * a large part of what it asks for there is not PostgreSQL's: Cloudberry
 * adds some 1,400 lines to src/backend/utils/cache/lsyscache.c, of which
 * ORCA calls 29 functions.  The port does not build that file -- it builds no
 * PostgreSQL file Cloudberry modified -- so the functions are re-implemented
 * here, against PostgreSQL 19.
 *
 * WHY THIS HEADER IS NOT AT compat/utils/lsyscache.h.  The other compat
 * header is at compat/optimizer/walkers.h, mirroring Cloudberry's own path,
 * which is safe because PostgreSQL 19 has no optimizer/walkers.h to hide.
 * It does have utils/lsyscache.h.  compat/ comes before the server's include
 * directory, so a file of that name here would shadow PostgreSQL's own
 * header for every translation unit in the module -- the same hazard that
 * stops the port putting Cloudberry's src/include on the include path at
 * all.  The declarations are Cloudberry's additions, not a replacement for
 * PostgreSQL's header, so the file is named for what it holds and includes
 * the real header rather than standing in for it.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_ORCA_COMPAT_CB_LSYSCACHE_H
#define GP_ORCA_COMPAT_CB_LSYSCACHE_H

#include "access/htup.h"
#include "nodes/pg_list.h"
#include "parser/parse_coerce.h"
#include "utils/lsyscache.h"

/*
 * Types.
 */
extern char *get_type_name(Oid oid);

/*
 * Functions and aggregates.
 *
 * PostgreSQL answers most of these already, but for one function at a time
 * and in the shape its own callers want.  ORCA builds metadata objects and
 * wants the whole argument list at once, so these return Lists.
 */
extern bool function_exists(Oid oid);
extern bool aggregate_exists(Oid oid);
extern List *get_func_arg_types(Oid funcid);
extern List *get_func_output_arg_types(Oid funcid);
extern Oid	get_agg_transtype(Oid aggid);
extern Oid	get_aggregate(const char *aggname, Oid oidType);

/*
 * Casts.
 *
 * One call for the three questions ORCA asks together: is there a cast, is
 * it free, and what function performs it.
 */
extern bool get_cast_func(Oid oidSrc, Oid oidDest, bool *is_binary_coercible,
						  Oid *oidCastFunc, CoercionPathType *pathtype);

/*
 * A helper Cloudberry keeps beside them, for the arrays get_func_arg_info
 * hands back.
 */
extern void pfree_ptr_array(char **ptrarray, int nelements);

#endif							/* GP_ORCA_COMPAT_CB_LSYSCACHE_H */
