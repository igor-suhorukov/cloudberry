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
 * gp_oauth_client.c
 *	  A client that logs in with the OAuth bearer token it is given, for the
 *	  security suite.
 *
 *	  gp_oauth_client TOKEN CONNINFO
 *
 * prints "ok" when the login succeeds, or libpq's error when it does not,
 * and exits 0 or 1.  libpq asks for a token through its authdata hook, which
 * psql does not set; this answers with the one it was given, as PostgreSQL
 * 19's own test client does (src/test/modules/oauth_validator), which is not
 * installed with the server.  Where the connection's oauth_issuer is not a
 * discovery document's URL, libpq makes the discovery round trip first, and
 * logs in with the token after it.
 *
 * It is a test program: built and installed only where the tests run.
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <string.h>

#include "libpq-fe.h"

static char *token = NULL;

static int
answer_with_token(PGauthData type, PGconn *conn, void *data)
{
	if (type == PQAUTHDATA_OAUTH_BEARER_TOKEN ||
		type == PQAUTHDATA_OAUTH_BEARER_TOKEN_V2)
	{
		/* a V2 request begins with a V1 one */
		PGoauthBearerRequest *request = data;

		request->token = token;
		return 1;
	}
	return PQdefaultAuthDataHook(type, conn, data);
}

int
main(int argc, char **argv)
{
	PGconn	   *conn;

	if (argc != 3)
	{
		fprintf(stderr, "usage: %s TOKEN CONNINFO\n", argv[0]);
		return 2;
	}
	token = argv[1];

	PQsetAuthDataHook(answer_with_token);
	conn = PQconnectdb(argv[2]);
	if (PQstatus(conn) != CONNECTION_OK)
	{
		printf("%s", PQerrorMessage(conn));
		PQfinish(conn);
		return 1;
	}
	printf("ok\n");
	PQfinish(conn);
	return 0;
}
