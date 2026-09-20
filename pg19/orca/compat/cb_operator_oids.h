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
 * compat/cb_operator_oids.h
 *	  Names for eleven operator OIDs that PostgreSQL has but does not name.
 *
 * PostgreSQL generates pg_operator_d.h from pg_operator.dat, and an entry
 * gets a C name only if it carries an `oid_symbol`.  Cloudberry added one to
 * each of these eleven so that ORCA could write them in a switch; the
 * operators themselves are PostgreSQL's and always have been.
 *
 * So this is a compat header of an unusual kind: nothing is re-implemented,
 * only re-named.  The OIDs below were read out of *PostgreSQL 19's* own
 * pg_operator.dat, not copied from Cloudberry's, and each was checked to
 * carry the same oprname, oprleft and oprright in both trees.  All eleven
 * are identical.  The check matters more than it looks: an OID that had
 * been reused for a different operator would compile perfectly and make
 * ORCA believe a lossy operator preserved distinct values.
 *
 * Used by gpdb::IsOpNDVPreserving, and nothing else.
 *
 *-------------------------------------------------------------------------
 */
#ifndef CB_OPERATOR_OIDS_H
#define CB_OPERATOR_OIDS_H

/*                                             oid     left op right      */
#define Int4AddOperator					551	/* int4      +  int4      */
#define OIDTextConcatenateOperator		654	/* text     ||  text      */
#define Int8AddOperator					684	/* int8      +  int8      */
#define DateIntervalAddOperator			1076 /* date     +  interval  */
#define DateInt4AddOperator				1100 /* date     +  int4      */
#define DateTimeAddOperator				1360 /* date     +  time      */
#define DateTimetzAddOperator			1361 /* date     +  timetz    */
#define NumericAddOperator				1758 /* numeric  +  numeric   */
#define TimestampIntervalAddOperator	2066 /* timestamp +  interval */
#define IntervalTimestampAddOperator	2553 /* interval +  timestamp */
#define Int4DateAddOperator				2555 /* int4     +  date      */

#endif							/* CB_OPERATOR_OIDS_H */
