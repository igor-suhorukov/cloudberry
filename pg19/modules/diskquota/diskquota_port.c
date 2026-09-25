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
 * diskquota_port.c
 *	  What diskquota_port.h promises: the segments asked through gp_core.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/table.h"
#include "access/transam.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "storage/proc.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "diskquota_port.h"

/* Set while this node runs, for the worker, what a segment would. */
static bool in_segment_call = false;

bool
dq_segment_call(void)
{
	return in_segment_call;
}

/*
 * One node: the query run here, through SPI, as its only segment would run
 * it.  The rows are copied into slots of "desc" one at a time.
 */
static int
local_query(const char *sql, TupleDesc desc, DqSegmentRowFn fn, void *arg)
{
	TupleTableSlot *slot = MakeSingleTupleTableSlot(desc, &TTSOpsHeapTuple);
	bool		save = in_segment_call;
	int			ret;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "[diskquota] SPI_connect failed");
	in_segment_call = true;
	PG_TRY();
	{
		ret = SPI_execute(sql, false, 0);
	}
	PG_FINALLY();
	{
		in_segment_call = save;
	}
	PG_END_TRY();
	if (ret < 0)
		elog(ERROR, "[diskquota] SPI_execute(\"%s\") failed: %d", sql, ret);

	if (fn != NULL && SPI_tuptable != NULL)
	{
		for (uint64 i = 0; i < SPI_processed; i++)
		{
			ExecStoreHeapTuple(SPI_tuptable->vals[i], slot, false);
			fn(slot, 0, arg);
			ExecClearTuple(slot);
		}
	}
	SPI_finish();
	ExecDropSingleTupleTableSlot(slot);
	return 1;
}

int
dq_segments_query(const char *sql, TupleDesc desc, DqSegmentRowFn fn,
				  void *arg)
{
	GpGatherState *gather;
	TupleTableSlot *slot;
	int			content;
	int			nsegments;

	if (!DQ_HAS_SEGMENTS())
		return local_query(sql, desc, fn, arg);

	slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	gather = GpGatherStart(sql, desc);
	nsegments = GpGatherSegmentCount(gather);
	while (GpGatherNext(gather, slot, &content))
	{
		fn(slot, content, arg);
		ExecClearTuple(slot);
	}
	GpGatherEnd(gather);
	ExecDropSingleTupleTableSlot(slot);
	return nsegments;
}

void
dq_segments_command(const char *sql)
{
	if (!DQ_HAS_SEGMENTS())
	{
		(void) local_query(sql, NULL, NULL, NULL);
		return;
	}
	GpDispatchCommand(sql);
}

/*
 * Cloudberry's worker turns ORCA off for what it runs, which ORCA would plan
 * only to fall back.  The port's ORCA is gp_orca's setting, there when the
 * module is loaded.
 */
void
dq_optimizer_off(void)
{
	if (GetConfigOption("gp.optimizer", true, false) != NULL)
		(void) set_config_option("gp.optimizer", "off", PGC_USERSET,
								 PGC_S_SESSION, GUC_ACTION_SET, true, 0,
								 false);
}

/*
 * The port's modules' relations in this database, looked up once in a
 * transaction: a new one is made only by CREATE or ALTER EXTENSION.
 */
static HTAB *port_relations = NULL;
static LocalTransactionId port_relations_lxid = InvalidLocalTransactionId;

bool
dq_is_system_relation(Oid relid)
{
	bool found;

	if (relid < FirstNormalObjectId) return true;

	if (port_relations == NULL || port_relations_lxid != MyProc->vxid.lxid)
	{
		HASHCTL ctl;

		if (port_relations != NULL) hash_destroy(port_relations);
		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize    = sizeof(Oid);
		ctl.entrysize  = sizeof(Oid);
		ctl.hcxt       = TopMemoryContext;
		port_relations = hash_create("diskquota port relations", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

		if (SPI_connect() != SPI_OK_CONNECT) elog(ERROR, "[diskquota] SPI_connect failed");
		if (SPI_execute(DQ_PORT_RELATIONS_SQL, true, 0) != SPI_OK_SELECT)
			elog(ERROR, "[diskquota] could not read the port's relations");
		for (uint64 i = 0; i < SPI_processed; i++)
		{
			bool isnull;
			Oid  oid = DatumGetObjectId(SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnull));

			if (!isnull) (void)hash_search(port_relations, &oid, HASH_ENTER, NULL);
		}
		SPI_finish();
		port_relations_lxid = MyProc->vxid.lxid;
	}

	(void)hash_search(port_relations, &relid, HASH_FIND, &found);
	return found;
}

ProcNumber
dq_relation_backend(char relpersistence, Oid relnamespace)
{
	if (relpersistence != RELPERSISTENCE_TEMP)
		return INVALID_PROC_NUMBER;
	if (isTempOrTempToastNamespace(relnamespace))
		return ProcNumberForTempRelations();
	return GetTempNamespaceProcNumber(relnamespace);
}

/*
 * The temporary relation of this database a file number is of, where there is
 * one: pg_class's index of file numbers, read as RelidByRelfilenumber() reads
 * it, for the rows it passes over.
 */
Oid
dq_temp_relid_by_relfilenumber(Oid spcOid, RelFileNumber relNumber)
{
	Relation    pg_class;
	SysScanDesc scan;
	ScanKeyData key[2];
	HeapTuple   tuple;
	Oid         relid = InvalidOid;

	/* pg_class's reltablespace is 0 for the database's own */
	if (spcOid == MyDatabaseTableSpace) spcOid = InvalidOid;

	pg_class = table_open(RelationRelationId, AccessShareLock);
	ScanKeyInit(&key[0], Anum_pg_class_reltablespace, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(spcOid));
	ScanKeyInit(&key[1], Anum_pg_class_relfilenode, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(relNumber));
	scan = systable_beginscan(pg_class, ClassTblspcRelfilenodeIndexId, true, NULL, 2, key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_class form = (Form_pg_class)GETSTRUCT(tuple);

		if (form->relpersistence == RELPERSISTENCE_TEMP)
		{
			relid = form->oid;
			break;
		}
	}
	systable_endscan(scan);
	table_close(pg_class, AccessShareLock);
	return relid;
}
