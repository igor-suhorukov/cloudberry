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
 * twophase_respell.c
 *	  The libpq calls of Cloudberry's twophase_pqexecparams.c, respelled.
 *
 * Cloudberry's distributed_transactions test runs its twophase_pqexecparams,
 * which sets debug_dtm_action and the settings beside it by the names it is
 * compiled with, Cloudberry's, which a script's respelling cannot reach.  It
 * is compiled where it lies, unmodified, its PQexec() this one: a SET of one
 * of them is sent as the port spells it, gp.debug_dtm_action_..., and
 * anything else as it is.
 *
 *-------------------------------------------------------------------------
 */
#undef PQexec

#include <stdio.h>
#include <string.h>

#include "libpq-fe.h"

PGresult   *cb_respell_PQexec(PGconn *conn, const char *query);

PGresult *
cb_respell_PQexec(PGconn *conn, const char *query)
{
	char		respelled[256];

	if (strncmp(query, "SET debug_dtm_action", strlen("SET debug_dtm_action")) == 0 &&
		snprintf(respelled, sizeof(respelled), "SET gp.%s",
				 query + strlen("SET ")) < (int) sizeof(respelled))
		return PQexec(conn, respelled);
	return PQexec(conn, query);
}
