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
 * gp_dbcopy.h
 *	  A database copied or moved takes its modules' directories with it.
 *	  See gp_dbcopy.c.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_DBCOPY_H
#define GP_DBCOPY_H

#include "nodes/parsenodes.h"

/*
 * gp_core's resource manager, among the custom ones (128-255): not one
 * PostgreSQL's wiki lists as taken (CustomWALResourceManagers), and none of
 * the port's others (gp_sql's 198, PAX's 199, gp_ao's 200 and 201).
 * gp_dbcopy.c registers it; its records are a database's directory copied,
 * the nodes' states as the coordinator publishes them (gp_cluster.c), a
 * prepared part's file of what its directories are to become, written and
 * removed (gp_dirxact.c), the transaction IDs the coordinator gives to
 * nobody again (gp_dtx.c), and the distributed transactions in their second
 * phase, for a hot standby coordinator's queries: one that begins it, one
 * forgotten, and all of them (gp_dtx_standby.c).
 */
#define GP_CORE_RMGR_ID			197

#define XLOG_GP_CORE_DBCOPY		0x00	/* a database's directory, copied */
#define XLOG_GP_CORE_CLUSTER	0x10	/* the nodes' states, gpsegconfig_dump */
#define XLOG_GP_CORE_DIRXACT	0x20	/* a prepared part's file, written */
#define XLOG_GP_CORE_DIRXACT_END	0x30	/* and removed */
#define XLOG_GP_CORE_GXID		0x40	/* transaction IDs, given to nobody again */
#define XLOG_GP_CORE_DTX_COMMITTING	0x50	/* a commit's second phase begins */
#define XLOG_GP_CORE_DTX_FORGET	0x60	/* ... and is done, or never was */
#define XLOG_GP_CORE_DTX_RUNNING	0x70	/* every commit in its second phase */

/*
 * The rest of CREATE DATABASE, where a node runs createdb() itself (gp_ddl.c,
 * a segment's part in a distributed transaction): the new database's
 * directories left to the transaction's abort, and the modules' copied.
 */
extern void GpDbcopyCreatedb(CreatedbStmt *stmt);

/* ALTER DATABASE ... SET TABLESPACE inside the transaction that asks. */
extern void GpMoveDatabase(const char *dbname, const char *tblspcname);

/* Called from gp_core's _PG_init, before GpDdlInit(). */
extern void GpDbcopyInit(void);

#endif							/* GP_DBCOPY_H */
