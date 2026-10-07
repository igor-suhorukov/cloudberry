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
 * gp_explain.h
 *	  Cloudberry's options of EXPLAIN, SLICETABLE and LOCUS, and EXPLAIN
 *	  ANALYZE's statistics of what the segments ran.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_EXPLAIN_H
#define GP_EXPLAIN_H

struct DefElem;
struct EState;
struct PlanState;

/* Cloudberry's gp_enable_explain_allstat */
extern bool gp_enable_explain_allstat;

/*
 * The mark an explained statement's fragment carries, which the segment
 * measures the fragment by (gp_motion.c); NULL for any other statement's.
 */
extern struct DefElem *GpExplainFragmentMark(struct EState *estate);

/*
 * The node whose segments' statements answer now -- a gather closing its
 * cursors (gp_scan.c) -- or NULL for none.  Returns the one before, to be
 * given back.
 */
extern struct PlanState *GpExplainAnswerFor(struct PlanState *node);

/*
 * gp_resource's: the most memory this process has reserved, in bytes,
 * which EXPLAIN ANALYZE's "Vmem reserved" says of each slice.
 */
typedef int64 (*GpExplainVmemReserved) (void);
extern void GpExplainSetVmemReserved(GpExplainVmemReserved reserved);

/*
 * Another module's figures of its own nodes, which EXPLAIN ANALYZE prints
 * on the coordinator from what the segments that ran them kept
 * (GpCoreApi.explain_register): "collect" appends a node's bytes, or
 * nothing, and says whether it did; "deposit" is given a segment's bytes
 * for the coordinator's node.
 */
struct StringInfoData;
typedef bool (*GpExplainCollect) (struct PlanState *ps,
								  struct StringInfoData *buf);
typedef void (*GpExplainDeposit) (struct PlanState *ps, int content,
								  const char *data, int len);
extern void GpExplainRegister(GpExplainCollect collect,
							  GpExplainDeposit deposit);

extern void GpExplainInit(void);

#endif							/* GP_EXPLAIN_H */
