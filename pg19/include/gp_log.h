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
 * gp_log.h
 *	  Cloudberry's own log of each server, a CSV file of thirty columns
 *	  that gp_core writes beside PostgreSQL's log (gp_log.c).
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_LOG_H
#define GP_LOG_H

/*
 * What a statement the coordinator sends a segment on its client's behalf
 * ends with: the client's statement in a comment, which the segment's log
 * names as the statement it runs, where Cloudberry's segment names the
 * statement it was dispatched.  "" on a segment, and where the coordinator
 * runs no statement of a client's.
 */
extern const char *GpLogStatementComment(void);

/* Defines gp.log_format and installs the hooks; from gp_core's _PG_init. */
extern void GpLogInit(void);

/*
 * On a segment, the coordinator's statement this backend runs a part of, as
 * what it was sent carries it -- a DDL tree's text, or the comment above --
 * or NULL: what the part's phases show (gp_dtx.c).
 */
extern const char *GpLogCoordinatorStatement(void);

#endif							/* GP_LOG_H */
