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
 * gpsc_frozen.h
 *	  A row of gp_stats_collector's "tbl" log, written frozen and without a
 *	  transaction id (gpsc_frozen.c).
 *
 *-------------------------------------------------------------------------
 */
#ifndef GPSC_FROZEN_H
#define GPSC_FROZEN_H

#include "utils/relcache.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Insert a row of values into rel, frozen, as Cloudberry's frozen_heap_insert() */
extern void gpsc_frozen_insert(Relation rel, Datum *values, bool *nulls);

#ifdef __cplusplus
}
#endif

#endif							/* GPSC_FROZEN_H */
