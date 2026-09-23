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
 * gp_gdd.h
 *	  The global deadlock detector.
 *
 * Two transactions can wait for each other on different segments, each
 * waiting where the other holds a row, and neither segment sees a cycle.
 * Without the detector Cloudberry keeps that from happening: an UPDATE or
 * DELETE of a distributed table, and SELECT ... FOR UPDATE, lock the whole
 * table.  With it (gp.enable_global_deadlock_detector) they lock rows, and a
 * process on the coordinator gathers every node's waits, finds a cycle and
 * cancels the youngest transaction in it; see gp_gdd.c.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_GDD_H
#define GP_GDD_H

#include "postgres.h"

/* gp.enable_global_deadlock_detector: rows are locked, not tables. */
extern PGDLLIMPORT bool gp_enable_global_deadlock_detector;

/*
 * A backend says who it is, for the detector: a segment's dispatched
 * backend the coordinator session it works for, the coordinator's the
 * number of the transaction it runs.  From the executor and utility hooks,
 * once per transaction.
 */
extern void GpGddNoteBackend(void);

/* The settings, and the detector's process; from gp_core's _PG_init. */
extern void GpGddInit(void);

#endif							/* GP_GDD_H */
