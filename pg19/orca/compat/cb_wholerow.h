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
 * compat/cb_wholerow.h
 *	  A table's old row, carried up an UPDATE's plan to its ModifyTable.
 *
 * See compat/wholerow.c.
 *
 *-------------------------------------------------------------------------
 */
#ifndef CB_WHOLEROW_H
#define CB_WHOLEROW_H

#include "nodes/plannodes.h"

/*
 * The whole row of the scan that produced column "resno" of "plan", in its
 * table's row type -- "rtable" is the plan's range table -- appended to the
 * target list of that scan and of every node between it and "plan": its
 * resno in "plan"'s target list, or InvalidAttrNumber where a node between
 * cannot pass it on.
 */
extern AttrNumber gp_orca_carry_whole_row(Plan *plan, AttrNumber resno,
										  List *rtable);

#endif							/* CB_WHOLEROW_H */
