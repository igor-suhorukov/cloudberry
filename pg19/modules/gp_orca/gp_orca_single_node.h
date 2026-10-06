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
 * gp_orca_single_node.h
 *	  See gp_orca_single_node.c: gp_orca's single-node build, without
 *	  gp_core.  The API it answers with is cb_compat.h's cb_core_api().
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_ORCA_SINGLE_NODE_H
#define GP_ORCA_SINGLE_NODE_H

/*
 * From _PG_init: refuse gp_core, loaded before gp_orca, and once every
 * preloaded library is in, loaded after it.
 */
extern void GpOrcaSingleNodeInit(void);

#endif							/* GP_ORCA_SINGLE_NODE_H */
