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
 * memprot/utils/resource_manager.h
 *	  The include overlay of Cloudberry's memory protection: which manager is
 *	  on.
 *
 * Cloudberry's vmem tracker counts only once a resource group is activated,
 * where groups are the manager, since a group's limits decide the count's
 * then; the port has no activation apart from the setting, so a backend's
 * groups are active where gp.resource_manager says they are (memprot.c).
 * See cdb/cdbvars.h for the overlay.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_COMPAT_MEMPROT_RESOURCE_MANAGER_H
#define GP_COMPAT_MEMPROT_RESOURCE_MANAGER_H

extern bool GpMemProtResGroupEnabled(void);

#define IsResGroupEnabled()		GpMemProtResGroupEnabled()
#define IsResGroupActivated()	GpMemProtResGroupEnabled()

#endif							/* GP_COMPAT_MEMPROT_RESOURCE_MANAGER_H */
