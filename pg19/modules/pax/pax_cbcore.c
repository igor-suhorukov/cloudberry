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
 * pax_cbcore.c
 *	  Functions of Cloudberry's core that PAX calls and PostgreSQL 19 has
 *	  not, or keeps static (declared in the port's comm/cbdb_api.h):
 *
 *	  extractcolumns_from_node()	Cloudberry's access/aocs/aocsam_handler.c:
 *									the columns of a relation an expression
 *									reads
 *	  system_nextsampleblock()		PostgreSQL 19's access/tablesample/system.c,
 *									which Cloudberry exports: TABLESAMPLE
 *									SYSTEM's choice of the next block, which
 *									PAX asks of its rows for a method that
 *									chooses none, as BERNOULLI
 *
 *	  Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tsmapi.h"
#include "common/hashfn.h"
#include "nodes/execnodes.h"
#include "nodes/nodeFuncs.h"
#include "nodes/primnodes.h"

/* The declarations are PAX's, in C++ (comm/cbdb_api.h): C linkage here. */
extern bool extractcolumns_from_node(Node *expr, bool *cols, AttrNumber natts);
extern BlockNumber system_nextsampleblock(SampleScanState *node,
										  BlockNumber nblocks);

struct ExtractcolumnContext
{
	bool	   *cols;
	AttrNumber	natts;
	bool		found;
};

static bool
extractcolumns_walker(Node *node, struct ExtractcolumnContext *ecCtx)
{
	if (node == NULL)
		return false;

	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (IS_SPECIAL_VARNO(var->varno))
			return false;

		if (var->varattno > 0 && var->varattno <= ecCtx->natts)
		{
			ecCtx->cols[var->varattno - 1] = true;
			ecCtx->found = true;
		}

		/*
		 * If all attributes are included, set all entries in mask to true.
		 */
		else if (var->varattno == 0)
		{
			for (AttrNumber attno = 0; attno < ecCtx->natts; attno++)
				ecCtx->cols[attno] = true;
			ecCtx->found = true;

			return true;
		}

		return false;
	}

	return expression_tree_walker(node, extractcolumns_walker, (void *) ecCtx);
}

bool
extractcolumns_from_node(Node *expr, bool *cols, AttrNumber natts)
{
	struct ExtractcolumnContext ecCtx;

	ecCtx.cols = cols;
	ecCtx.natts = natts;
	ecCtx.found = false;

	extractcolumns_walker(expr, &ecCtx);

	return ecCtx.found;
}

/* PostgreSQL 19's system.c's sampler state, which the function reads. */
typedef struct
{
	uint64		cutoff;			/* select blocks with hash less than this */
	uint32		seed;			/* random seed */
	BlockNumber nextblock;		/* next block to consider sampling */
	OffsetNumber lt;			/* last tuple returned from current block */
} SystemSamplerData;

BlockNumber
system_nextsampleblock(SampleScanState *node, BlockNumber nblocks)
{
	SystemSamplerData *sampler = (SystemSamplerData *) node->tsm_state;
	BlockNumber nextblock = sampler->nextblock;
	uint32		hashinput[2];

	/*
	 * We compute the hash by applying hash_any to an array of 2 uint32's
	 * containing the block number and seed.  This is efficient to set up, and
	 * with the current implementation of hash_any, it gives
	 * machine-independent results, which is a nice property for regression
	 * testing.
	 *
	 * These words in the hash input are the same throughout the block:
	 */
	hashinput[1] = sampler->seed;

	/*
	 * Loop over block numbers until finding suitable block or reaching end of
	 * relation.
	 */
	for (; nextblock < nblocks; nextblock++)
	{
		uint32		hash;

		hashinput[0] = nextblock;

		hash = DatumGetUInt32(hash_any((const unsigned char *) hashinput,
									   (int) sizeof(hashinput)));
		if (hash < sampler->cutoff)
			break;
	}

	if (nextblock < nblocks)
	{
		/* Found a suitable block; remember where we should start next time */
		sampler->nextblock = nextblock + 1;
		return nextblock;
	}

	/* Done, but let's reset nextblock to 0 for safety. */
	sampler->nextblock = 0;
	return InvalidBlockNumber;
}
