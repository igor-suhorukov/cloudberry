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
 * orca_test_am.c
 *	  Two table access methods the orca suite has ORCA plan tables of
 *	  (section 28): heap's routine, changed where gp_orca's relcache
 *	  translator asks what a method is.
 *
 *	orca_virtual_am		hands its rows over in virtual slots, as an
 *						extension's method not built on heap does --
 *						ch_table_engines' mergetree -- and so is taken for
 *						a non-block table.  A table of it can be made,
 *						indexed, written and planned, but not read: heap's
 *						scans store a heap tuple into the scan's slot, and
 *						a virtual slot refuses one.
 *	orca_nobitmap_am	heap's, but without the bitmap scan PostgreSQL 19
 *						leaves optional (scan_bitmap_next_tuple), and heap
 *						in full otherwise.
 *
 * heap_getnext() refuses a table whose routine is not heap's, and heap's
 * functions read with it where they read a table whole, so as gp_probe's
 * method does (test/hooks/gp_probe.c), an index is built and validated with
 * heap's routine in the table's relcache entry for the length of it, and a
 * table's TOAST table is heap's, where heap's own callback would make it of
 * the table's method.
 *
 * It is a test module: built and installed only where the tests run.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tableam.h"
#include "catalog/pg_am_d.h"
#include "executor/tuptable.h"
#include "fmgr.h"

PG_MODULE_MAGIC_EXT(
					.name = "orca_test_am",
					.version = "1.0"
);

PG_FUNCTION_INFO_V1(orca_virtual_am_handler);
PG_FUNCTION_INFO_V1(orca_nobitmap_am_handler);

static TableAmRoutine virtual_am;
static TableAmRoutine nobitmap_am;
static bool made = false;

static const TupleTableSlotOps *
virtual_slot_callbacks(Relation rel)
{
	return &TTSOpsVirtual;
}

static Oid
heap_toast_am(Relation rel)
{
	return HEAP_TABLE_AM_OID;
}

static double
as_heap_index_build_range_scan(Relation table_rel, Relation index_rel,
							   IndexInfo *index_info, bool allow_sync,
							   bool anyvisible, bool progress,
							   BlockNumber start_blockno, BlockNumber numblocks,
							   IndexBuildCallback callback, void *callback_state,
							   TableScanDesc scan)
{
	const TableAmRoutine *own = table_rel->rd_tableam;
	const TableAmRoutine *heap = GetHeapamTableAmRoutine();
	double		result;

	table_rel->rd_tableam = heap;
	PG_TRY();
	{
		result = heap->index_build_range_scan(table_rel, index_rel, index_info,
											  allow_sync, anyvisible, progress,
											  start_blockno, numblocks,
											  callback, callback_state, scan);
	}
	PG_FINALLY();
	{
		table_rel->rd_tableam = own;
	}
	PG_END_TRY();
	return result;
}

static void
as_heap_index_validate_scan(Relation table_rel, Relation index_rel,
							IndexInfo *index_info, Snapshot snapshot,
							ValidateIndexState *state)
{
	const TableAmRoutine *own = table_rel->rd_tableam;
	const TableAmRoutine *heap = GetHeapamTableAmRoutine();

	table_rel->rd_tableam = heap;
	PG_TRY();
	{
		heap->index_validate_scan(table_rel, index_rel, index_info, snapshot,
								  state);
	}
	PG_FINALLY();
	{
		table_rel->rd_tableam = own;
	}
	PG_END_TRY();
}

/* heap's routine, its whole-table reads and TOAST heap's as heap's are */
static void
copy_heap(TableAmRoutine *am)
{
	*am = *GetHeapamTableAmRoutine();
	am->relation_toast_am = heap_toast_am;
	am->index_build_range_scan = as_heap_index_build_range_scan;
	am->index_validate_scan = as_heap_index_validate_scan;
}

static void
make_routines(void)
{
	if (made)
		return;
	copy_heap(&virtual_am);
	virtual_am.slot_callbacks = virtual_slot_callbacks;
	copy_heap(&nobitmap_am);
	nobitmap_am.scan_bitmap_next_tuple = NULL;
	made = true;
}

Datum
orca_virtual_am_handler(PG_FUNCTION_ARGS)
{
	make_routines();
	PG_RETURN_POINTER(&virtual_am);
}

Datum
orca_nobitmap_am_handler(PG_FUNCTION_ARGS)
{
	make_routines();
	PG_RETURN_POINTER(&nobitmap_am);
}
