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
 * compat/tlist.c
 *	  What Cloudberry adds to PostgreSQL's tlist.c for ORCA.
 *
 * Ported from github/cloudberry/src/backend/optimizer/util/tlist.c.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "nodes/pg_list.h"

#include "cb_tlist.h"

/*
 * tlist_members
 *		Every entry of the target list whose expression is equal() to the
 *		given one.  NIL if there is none.
 *
 * PostgreSQL has tlist_member(), which returns the first match and stops
 * (pg19/src/backend/optimizer/util/tlist.c:88).  That is the right answer
 * for its callers, which want *a* place the expression is computed.  ORCA
 * wants all of them, because it rewrites references rather than picking one:
 * a target list can compute the same expression twice, under two resjunk
 * flags or two sort groups, and leaving the second occurrence pointing at
 * the first one's column would change what the plan projects.
 *
 * Nothing here changed for PostgreSQL 19.  The entries are not copied, as
 * Cloudberry's comment says; the list is the caller's to free and the
 * TargetEntrys in it are not.
 *
 * Cloudberry asserts IsA(tlentry, TargetEntry) per entry.  The port leaves
 * that to equal(), which reaches the same answer: a list member that is not
 * a TargetEntry has no ->expr to compare and equal() would already be
 * reading the wrong field.  Assert is compiled out in the builds that
 * matter, so it protected nothing the release build could rely on.
 */
List *
tlist_members(Node *node, List *targetlist)
{
	List	   *tlist = NIL;
	ListCell   *lc;

	foreach(lc, targetlist)
	{
		TargetEntry *tlentry = (TargetEntry *) lfirst(lc);

		if (equal(node, tlentry->expr))
			tlist = lappend(tlist, tlentry);
	}

	return tlist;
}
