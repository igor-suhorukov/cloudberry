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
 * compat/cb_selfuncs.h
 *	  Two number-shaped helpers ORCA needs, which PostgreSQL keeps to itself.
 *
 * ORCA builds its own statistics objects and has to turn a datum into a
 * double to do it -- a histogram bucket bound is a number to ORCA whatever
 * its SQL type.  PostgreSQL does the same conversion in selfuncs.c, for its
 * own selectivity estimates, and keeps it static.
 *
 * THIS FILE CORRECTS THE PLAN.  cloudberry.md records, under "Corrections to
 * the plan", that selfuncs.c and subselect.c "are not part of the compat
 * layer" because the functions ORCA calls from them "exist in PostgreSQL 19
 * unchanged".  They do exist -- and they are static, so nothing outside
 * their own file can call them.  Cloudberry's contribution to both files is
 * exactly to export them.  The check that produced the wrong answer looked
 * for the function in PostgreSQL's sources; what decides it is whether a
 * header declares it.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_ORCA_COMPAT_CB_SELFUNCS_H
#define GP_ORCA_COMPAT_CB_SELFUNCS_H

#include "postgres.h"

#include "utils/numeric.h"

/*
 * A time-shaped value as a double, on PostgreSQL's own internal scale:
 * microseconds since 2000-01-01 for the timestamp types, microseconds since
 * midnight for the time ones.  The scale does not matter to ORCA, which only
 * ever compares two of them; what matters is that every value of one type
 * lands on the same scale.
 *
 * *failure is set, and 0 returned, for a type this cannot convert.  The
 * caller has to look: 0 is a perfectly good timestamp.
 */
extern double convert_timevalue_to_scalar(Datum value, Oid typid,
										  bool *failure);

/*
 * A numeric as a double, saturating instead of raising.
 *
 * "No overflow" is the whole point: a numeric holds values no double can,
 * and a statistics bound that is out of range is still a usable bound once
 * it becomes an infinity.  Raising here would lose the histogram over one
 * bucket.
 */
extern double numeric_to_double_no_overflow(Numeric num);

#endif							/* GP_ORCA_COMPAT_CB_SELFUNCS_H */
