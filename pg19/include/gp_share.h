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
 * gp_share.h
 *	  One transaction, several backends of a segment: the shared snapshot.
 *
 * Cloudberry runs the slices of a query at once, each in a gang of its own,
 * and on a segment one backend of the session -- the writer -- owns the
 * transaction while the others, its readers, read what it wrote and has not
 * committed (sharedsnapshot.c).  The port does the same with R2 and R4: the
 * writer publishes its snapshot and its transaction's state for a statement,
 * and a reader adopts both before its own transaction takes a snapshot; see
 * gp_share.c.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_SHARE_H
#define GP_SHARE_H

#include "postgres.h"

#include "utils/snapshot.h"

/*
 * The setting a reader's transaction is given, as "<writer pid>/<key>", to
 * read as a part of the writer's; see gp_share.c.
 */
#define GP_SHARE_SETTING	"gp.shared_snapshot"

/* How long a publication's key may be. */
#define GP_SHARE_KEYLEN		64

/*
 * The writer, as a fragment it runs starts: its snapshot and its
 * transaction's state, under "key", for the readers the coordinator starts
 * beside it.
 */
extern void GpSharePublish(const char *key, Snapshot snapshot);

/* Is this backend's transaction a reader's of another backend's? */
extern bool GpShareIsReader(void);

/* The setting, the hooks and the callbacks; from gp_core's _PG_init. */
extern void GpShareInit(void);

#endif							/* GP_SHARE_H */
