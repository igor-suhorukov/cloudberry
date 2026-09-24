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
 * gp_loopback.h
 *	  Another database of this server, reached from any database, its work
 *	  committed with the transaction that asked for it.
 *
 * What Cloudberry keeps in a shared catalog and the port cannot keep in a
 * label lives in one database: task jobs in gp.task_database, storage
 * servers in gp.maintenance_database.  A backend in any other database
 * reaches that one through this.  See gp_loopback.c.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_LOOPBACK_H
#define GP_LOOPBACK_H

#include "postgres.h"

#include "fmgr.h"
#include "nodes/execnodes.h"

/* gp.maintenance_database: where the storage servers are. */
extern const char *GpLoopbackMaintenanceDatabase(void);

/* Is that this backend's own database? */
extern bool GpLoopbackIsHere(const char *dbname);

/*
 * Run `sql` in database `dbname` as a part of this transaction: at its
 * pre-commit, in the order asked for, in one transaction there that commits
 * or rolls back with this one.  What a subtransaction asked for is forgotten
 * if the subtransaction rolls back.  The caller builds `sql` from values it
 * has quoted, never from a caller's SQL: the connection may carry the
 * cluster secret.
 */
extern void GpLoopbackDefer(const char *dbname, const char *sql);

/*
 * Run a query in `dbname` now, as this session's current user, in a
 * read-only transaction of its own there, and put its rows in the tuplestore
 * of a materialised set-returning function, each column read by the input
 * function of rsinfo->setDesc's type.  What this transaction has deferred is
 * not there yet.  Run through SPI when `dbname` is this database.
 */
extern void GpLoopbackQueryInto(const char *dbname, const char *sql,
								ReturnSetInfo *rsinfo);

/* The setting and the transaction callbacks; from gp_core's _PG_init. */
extern void GpLoopbackInit(void);

#endif							/* GP_LOOPBACK_H */
