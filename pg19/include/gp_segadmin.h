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
 * gp_segadmin.h
 *	  Cloudberry's segment administration functions.  See gp_segadmin.c.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_SEGADMIN_H
#define GP_SEGADMIN_H

#include "gp_cluster.h"

/*
 * The nodes as this transaction's own changes leave them, which it has not
 * committed yet: "nodes" is a copy of the places, changed in place -- a node
 * removed empties its place, one added takes an empty one.  Nothing where
 * the transaction has changed nothing.
 */
extern void GpSegadminOverlay(GpSegmentConfig *nodes, int nnodes);

/* The transaction callbacks that make the changes at commit. */
extern void GpSegadminInit(void);

#endif							/* GP_SEGADMIN_H */
