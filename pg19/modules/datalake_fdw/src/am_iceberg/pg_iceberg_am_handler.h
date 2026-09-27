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
 * pg_iceberg_am_handler.h
 *	  The Iceberg access method's registration with PostgreSQL 19's
 *	  registry of table access methods (O13), the port's own header.
 *
 * Cloudberry's routine carried its own option parser (amoptions);
 * PostgreSQL 19's has none, and a method registers it beside the routine
 * while the postmaster loads the library.
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_ICEBERG_AM_HANDLER_H
#define PG_ICEBERG_AM_HANDLER_H

/* Register the method's option parser; from _PG_init, while preloading. */
extern void pg_iceberg_register_am_extension(void);

#endif							/* PG_ICEBERG_AM_HANDLER_H */
