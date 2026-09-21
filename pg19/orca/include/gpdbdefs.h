//---------------------------------------------------------------------------
//
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
//	@filename:
//		gpdbdefs.h
//
//	@doc:
//		Every PostgreSQL header the translator needs, behind one
//		`extern "C"`.
//
//		Ported from github/cloudberry/src/include/gpopt/utils/gpdbdefs.h.
//		It is an umbrella and nothing else -- no declarations of its own --
//		so the port of it is a port of a list.
//
//		Three kinds of change from Cloudberry's list:
//
//		  * PostgreSQL headers that exist unchanged in 19 are kept as they
//		    are.  Most of the list.
//
//		  * Headers Cloudberry patched are followed by the port's compat
//		    header, which declares what Cloudberry added.  `utils/lsyscache.h`
//		    and `cb_lsyscache.h` are the pair that matters most: 30-odd of the
//		    names the translator calls are in the second one.
//
//		  * Cloudberry-only headers with no PostgreSQL 19 counterpart are
//		    dropped, and the line says which milestone brings them back.
//		    Nothing is stubbed here: a translator file that needs one of them
//		    should fail to compile, naming it, rather than compile against
//		    something that is not there yet.
//
//---------------------------------------------------------------------------

#ifndef GPDBDefs_H
#define GPDBDefs_H

extern "C" {

#include "postgres.h"

#include "catalog/namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "commands/defrem.h"
#include "commands/trigger.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "nodes/makefuncs.h"
#include "nodes/nodes.h"
#include "nodes/pg_list.h"
#include "nodes/plannodes.h"
#include "optimizer/planmain.h"
#include "optimizer/tlist.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "tcop/dest.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/elog.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

/*
 * The compat layer: what Cloudberry added to the PostgreSQL files above, and
 * what the port has re-implemented because it does not build those files.
 * Each one sits under the PostgreSQL header it extends.
 *
 * "optimizer/walkers.h" and "utils/faultinjector.h" resolve to the port's
 * copies under orca/compat, because PostgreSQL 19 has no header by either
 * name; the others are spelled cb_*.h so that a reader can see at a glance
 * which declarations are Cloudberry's.
 */
#include "cb_clauses.h"		  /* optimizer/clauses.h */
#include "cb_lsyscache.h"	  /* utils/lsyscache.h */
#include "cb_nodes.h"		  /* nodes/nodes.h */
#include "cb_plancat.h"		  /* optimizer/plancat.h */
#include "cb_prepagg.h"		  /* optimizer/prep.h */
#include "cb_selfuncs.h"	  /* utils/selfuncs.h */
#include "cb_subselect.h"	  /* optimizer/subselect.h */
#include "cb_tlist.h"		  /* optimizer/tlist.h */
#include "optimizer/walkers.h"
#include "utils/faultinjector.h"

/*
 * gp_core's view of the cluster: Gp_role, getgpsegmentCount(),
 * IS_SINGLENODE().  Cloudberry reads these from cdb/cdbvars.h, which is a
 * file the port does not have; they come over the rendezvous variable
 * instead.  See pg19/compat/cb_compat.h.
 */
#include "cb_compat.h"

/*
 * ORCA's settings.  Cloudberry reads these from cdb/cdbvars.h too -- one
 * header for the cluster and for the 438 settings of guc_gp.c -- and its
 * gpdbdefs.h pulls that header in, which is how every translator file that
 * reads a setting comes to see it declared.  The port keeps the two apart and
 * so has to name both here: leaving this one out makes a file that reads
 * gp.optimizer_enable_foreign_table fail in the file that reads it rather than
 * in the umbrella, which is a worse place to find out.
 */
#include "gp_orca_guc.h"

/*
 * Left out of Cloudberry's list, and why:
 *
 *	 cdb/cdbhash.h				M2.  Hashing a distribution key, and asking
 *								whether two opfamilies hash compatibly.
 *	 cdb/cdbmutate.h			M2.  Motion, and the flow of a plan.
 *	 cdb/cdbutil.h				M2.  Segment configuration.
 *	 cdb/partitionselection.h	M2.  The PartitionSelector plan node.
 *	 utils/uri.h				M5.  External table locations.
 */

}  // end extern C

#endif	// GPDBDefs_H

// EOF
