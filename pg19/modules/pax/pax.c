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
 * pax.c
 *	  PAX, the partition-attributes-across storage format: the module.
 *
 * PAX is Cloudberry's table access method that keeps a table's rows in
 * files of its own, by column, in a directory beside the relation's
 * (<relfilenode>_pax), each file's rows, statistics and deletions described
 * by a row of the table's aux table, pax.pg_pax_blocks_<relid>, and its
 * bytes logged by PAX's WAL resource manager (199).  The port compiles
 * Cloudberry's C++ where it stands, and the port's copies of the files that
 * had to change (pg19/pax/meson.build); what PostgreSQL 19 does not do for
 * it, the port's core patches do -- its options (O14), the columns a scan
 * reads (O15), a unique index's probe (O16), its directory removed with its
 * files (O21's unlink event) and its files passed over by pg_checksums (O23)
 * -- and gp_core, where its size is measured and its files marked (API 1.14,
 * below).  CREATE EXTENSION pax makes its objects, where Cloudberry's initdb
 * made them with fixed OIDs.
 *
 * This file is the module's: its magic block and _PG_init, which calls
 * PAX's own (pax_init(), access/pax_access_handle.cc), a setting of
 * Cloudberry's core that PAX reads, and ANALYZE's sample of a PAX table,
 * whose blocks are no pages ANALYZE could read.
 *
 * Cloudberry sources this module is made of:
 *	  contrib/pax_storage/
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relscan.h"
#include "access/tableam.h"
#include "commands/vacuum.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "utils/rel.h"
#include "utils/sampling.h"
#include "utils/snapmgr.h"

#include "cb_module.h"
#include "gp_core_api.h"
#include "pax_module.h"

PG_MODULE_MAGIC_EXT(
					.name = "pax",
					.version = GP_VERSION
);

/* The suffix of a PAX table's directory, after its relfilenode. */
#define PAX_DIRECTORY_SUFFIX "_pax"

/*
 * Cloudberry's gp_enable_predicate_pushdown: a scan of a PAX table filters
 * rows by the quals as it reads them, where pax.enable_row_filter asks too.
 */
bool		gp_enable_predicate_pushdown = true;

static analyze_sample_rows_hook_type prev_analyze_sample_rows = NULL;

extern void pax_init(void);

/*
 * ANALYZE's sample of a PAX table, whose block numbers are its files and
 * rows rather than pages: every row the transaction's snapshot sees is read,
 * and a reservoir kept by Vitter's algorithm, as acquire_sample_rows() keeps
 * one of the rows of the pages it reads, then sorted by TID, as it sorts its
 * sample for the correlation.  PAX keeps no dead rows.
 */
static int
compare_rows(const void *a, const void *b, void *arg)
{
	HeapTuple	ha = *(const HeapTuple *) a;
	HeapTuple	hb = *(const HeapTuple *) b;

	return ItemPointerCompare(&ha->t_self, &hb->t_self);
}

static int
pax_acquire_sample_rows(Relation rel, int elevel, HeapTuple *rows,
						int targrows, double *totalrows, double *totaldeadrows)
{
	TableScanDesc scan;
	TupleTableSlot *slot;
	ReservoirStateData rstate;
	Snapshot	snapshot = RegisterSnapshot(GetTransactionSnapshot());
	int			numrows = 0;
	double		liverows = 0;
	double		rowstoskip = -1;

	reservoir_init_selection_state(&rstate, targrows);
	slot = table_slot_create(rel, NULL);
	scan = table_beginscan(rel, snapshot, 0, NULL, SO_NONE);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		vacuum_delay_point(true);
		if (numrows < targrows)
		{
			rows[numrows] = ExecCopySlotHeapTuple(slot);
			rows[numrows]->t_self = slot->tts_tid;
			numrows++;
		}
		else
		{
			if (rowstoskip < 0)
				rowstoskip = reservoir_get_next_S(&rstate, liverows, targrows);
			if (rowstoskip <= 0)
			{
				int			k = (int) (targrows * sampler_random_fract(&rstate.randstate));

				Assert(k >= 0 && k < targrows);
				heap_freetuple(rows[k]);
				rows[k] = ExecCopySlotHeapTuple(slot);
				rows[k]->t_self = slot->tts_tid;
			}
			rowstoskip -= 1;
		}
		liverows += 1;
	}
	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	UnregisterSnapshot(snapshot);

	if (numrows == targrows)
		qsort_interruptible(rows, numrows, sizeof(HeapTuple), compare_rows, NULL);

	*totalrows = liverows;
	*totaldeadrows = 0;

	ereport(elevel,
			(errmsg("\"%s\": %.0f live rows and %.0f dead rows; %d rows in sample",
					RelationGetRelationName(rel), liverows, *totaldeadrows,
					numrows)));
	return numrows;
}

static bool
pax_analyze_sample_rows(Relation relation, AnalyzeSampleRowsFunc *func,
						BlockNumber *totalpages)
{
	BlockNumber pages;
	double		tuples;
	double		allvisfrac;

	/* A table whose rows are on the segments: gp_core samples them there. */
	if (prev_analyze_sample_rows &&
		prev_analyze_sample_rows(relation, func, totalpages))
		return true;
	if (relation->rd_tableam != PaxTableAmRoutine())
		return false;
	*func = pax_acquire_sample_rows;
	table_relation_estimate_size(relation, NULL, &pages, &tuples, &allvisfrac);
	*totalpages = pages;
	return true;
}

void
_PG_init(void)
{
	/*
	 * A custom WAL resource manager and a table access method's extension
	 * routines can only be registered while the postmaster loads libraries.
	 */
	CB_REQUIRE_PRELOAD("pax");
	CB_REQUIRE_CORE("pax");

	DefineCustomBoolVariable("gp.enable_predicate_pushdown",
							 "Enable predicate pushdown, some quals will be pushed down to lower data source.",
							 "Cloudberry calls this gp_enable_predicate_pushdown.  A scan of a PAX table "
							 "filters rows by its quals as it reads them where pax.enable_row_filter "
							 "is on too.",
							 &gp_enable_predicate_pushdown,
							 true,
							 PGC_USERSET, 0,
							 NULL, NULL, NULL);

	PaxCatalogInit();
	pax_init();

	/* O23: a table's directory is PAX's, which pg_checksums leaves alone */
	GpCoreApiLookup()->extension_mark_add(PAX_DIRECTORY_SUFFIX);

	/* and what its tables take is in it, which its relation_size measures */
	GpCoreApiLookup()->size_from_am_register(PaxTableAmRoutine());

	prev_analyze_sample_rows = analyze_sample_rows_hook;
	analyze_sample_rows_hook = pax_analyze_sample_rows;
}
