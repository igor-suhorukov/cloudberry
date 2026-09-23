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
 * gp_segment.h
 *	  gp_segment_id, through O10's column-reference fallback.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_SEGMENT_H
#define GP_SEGMENT_H

#include "postgres.h"

#include "nodes/nodes.h"

/*
 * Is this expression gp_segment_id of range table entry varno's row -- a call
 * of gp_internal.segment_of() on it, which a segment answers for itself?
 */
extern bool GpSegmentIsSegmentOf(Node *node, Index varno);

/* The parser's and ruleutils' hooks, on every node; see gp_segment.c. */
extern void GpSegmentInit(void);

#endif							/* GP_SEGMENT_H */
