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
 * gp_dbcopy.h
 *	  A database copied or moved takes its modules' directories with it.
 *	  See gp_dbcopy.c.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_DBCOPY_H
#define GP_DBCOPY_H

/*
 * gp_core's resource manager, among the custom ones (128-255): not one
 * PostgreSQL's wiki lists as taken (CustomWALResourceManagers), and none of
 * the port's others (gp_sql's 198, PAX's 199, gp_ao's 200 and 201).
 * gp_dbcopy.c registers it; its records are a database's directory copied,
 * and the nodes' states as the coordinator publishes them (gp_cluster.c).
 */
#define GP_CORE_RMGR_ID			197

#define XLOG_GP_CORE_DBCOPY		0x00	/* a database's directory, copied */
#define XLOG_GP_CORE_CLUSTER	0x10	/* the nodes' states, gpsegconfig_dump */

/* Called from gp_core's _PG_init, before GpDdlInit(). */
extern void GpDbcopyInit(void);

#endif							/* GP_DBCOPY_H */
