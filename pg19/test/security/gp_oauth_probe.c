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
 * gp_oauth_probe.c
 *	  An OAuth validator for the security suite: the bearer of the token
 *	  "good" is who the role says, and any other token is refused.
 *
 * PostgreSQL 19's own test validator (src/test/modules/oauth_validator) is
 * not installed with the server, so the suite has this one, which is enough
 * for what it asks: that a login a token was refused for counts against the
 * role's profile, and the discovery round trip before it does not.
 *
 * It is a test module: built and installed only where the tests run.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "libpq/oauth.h"

PG_MODULE_MAGIC_EXT(
					.name = "gp_oauth_probe",
					.version = "1.0"
);

static bool
probe_validate(const ValidatorModuleState *state, const char *token,
			   const char *role, ValidatorModuleResult *result)
{
	result->authorized = strcmp(token, "good") == 0;
	result->authn_id = pstrdup(role);
	if (!result->authorized)
		result->error_detail = "gp_oauth_probe accepts the token \"good\" alone";
	return true;
}

static const OAuthValidatorCallbacks probe_callbacks = {
	PG_OAUTH_VALIDATOR_MAGIC,
	.validate_cb = probe_validate,
};

const OAuthValidatorCallbacks *
_PG_oauth_validator_module_init(void)
{
	return &probe_callbacks;
}
