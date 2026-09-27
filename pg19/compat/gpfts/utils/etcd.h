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
 * gpfts/utils/etcd.h
 *	  The include overlay of gpfts: this reaches a header of Cloudberry's own.
 *
 * gpfts is built with Cloudberry's etcd client compiled where it lies --
 * src/backend/utils/etcd_lib/etcd.c, the keys of src/common/etcdutils.c and
 * the log of src/fe_utils/log.c -- unchanged (meson.build).  They ask for
 * Cloudberry's headers by these names, and the directory that holds them
 * cannot go on the include path: it also holds Cloudberry's copies of
 * PostgreSQL 16's headers, which would then be found instead of PostgreSQL
 * 19's (see task/cron.h).  So the overlay holds one forwarding header for
 * each, and nothing else.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_COMPAT_GPFTS_UTILS_ETCD_H
#define GP_COMPAT_GPFTS_UTILS_ETCD_H

#include "../../../../src/include/utils/etcd.h"

#endif							/* GP_COMPAT_GPFTS_UTILS_ETCD_H */
