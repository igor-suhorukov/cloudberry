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
 * gp_dirxact.h
 *	  A database's and a tablespace's directories follow the transaction
 *	  that made or dropped them.  See gp_dirxact.c.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_DIRXACT_H
#define GP_DIRXACT_H

#include "nodes/parsenodes.h"

/*
 * Database db's directory in tablespace spc -- in every tablespace where spc
 * is InvalidOid -- removed as the transaction commits (at_commit) or aborts,
 * however it ends: here, by COMMIT or ROLLBACK PREPARED, or after a restart.
 * "created" says the database is one the transaction made, whose buffers go
 * too.
 */
extern void GpDirxactDatabase(Oid db, Oid spc, bool at_commit, bool created);

/* Tablespace spc's directories on this node, likewise. */
extern void GpDirxactTablespace(Oid spc, bool at_commit);

/*
 * DROP TABLESPACE whose directories go as the transaction commits, as
 * Cloudberry's DropTableSpace() leaves them (tablespace.c).
 */
extern void GpDropTableSpace(DropTableSpaceStmt *stmt);

/*
 * The redo of gp_core's records of a prepared part's file,
 * XLOG_GP_CORE_DIRXACT and XLOG_GP_CORE_DIRXACT_END (gp_dbcopy.h).
 */
extern void GpDirxactRedo(uint8 info, const char *data, int len);

/* Called from gp_core's _PG_init. */
extern void GpDirxactInit(void);

#endif							/* GP_DIRXACT_H */
