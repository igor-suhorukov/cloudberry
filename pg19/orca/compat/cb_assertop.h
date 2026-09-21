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
 * compat/cb_assertop.h
 *	  Assert, the node ORCA puts over rows that must pass a test.
 *
 * Cloudberry's executor has it as AssertOp
 * (github/cloudberry/src/backend/executor/nodeAssertOp.c).  PostgreSQL 19
 * has no such node and a module cannot add a node type, so the port's is a
 * CustomScan: the plan node PostgreSQL provides for an extension's own
 * executor code.  Its child is the plan's outer plan, as AssertOp's is,
 * rather than one of custom_plans, so that EXPLAIN can name the columns its
 * test and its output read from that child.
 *
 * The plan node:
 *
 *	scan.plan.lefttree	the rows to test
 *	scan.plan.targetlist	what it returns, in terms of its child (OUTER_VAR)
 *	custom_exprs			the tests, likewise; a row fails if one is false
 *	custom_private			two String nodes: the SQLSTATE to raise and the
 *							message
 *
 * Built by CTranslatorDXLToPlStmt::TranslateDXLAssert.
 *
 *-------------------------------------------------------------------------
 */
#ifndef CB_ASSERTOP_H
#define CB_ASSERTOP_H

#include "nodes/extensible.h"

/* The CustomScan's methods; a plan's node points here. */
extern const CustomScanMethods gp_orca_assert_methods;

/*
 * Register the methods by name, so that a plan read back from its text form
 * -- a parallel worker's, or one a test prints and reads -- finds them.  Once
 * per process, from _PG_init.
 */
extern void gp_orca_register_assert(void);

#endif							/* CB_ASSERTOP_H */
