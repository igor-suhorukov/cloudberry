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
 * pax_module.h
 *	  The port's side of PAX's catalog: the OIDs of the extension's objects
 *	  (modules/pax/pax_catalog.c), whose names PAX's code gives through
 *	  comm/pax_rel.h, and the module's C entry points into PAX's C++.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PAX_MODULE_H
#define PAX_MODULE_H

#include "comm/pax_rel.h"

/* The extension's schema, where Cloudberry's initdb made PAX's catalogs. */
#define PAX_NAMESPACE_NAME "pax"

/* Each table's aux table's, Cloudberry's own, which pg_dump passes over. */
#define PAX_AUX_NAMESPACE_NAME "pg_ext_aux"

#ifdef __cplusplus
extern "C" {
#endif

extern void PaxCatalogInit(void);
extern bool PaxCatalogReady(void);

#ifdef __cplusplus
}
#endif

#endif							/* PAX_MODULE_H */
