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
 * gp_dtm_debug.h
 *	  Cloudberry's debug_dtm_action: a segment's part of a distributed
 *	  transaction failing at a command, as its tests ask.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_DTM_DEBUG_H
#define GP_DTM_DEBUG_H

/*
 * Cloudberry's DTX protocol commands, the values of debug_dtm_action_protocol
 * (cdb/cdbtm.h).  The port sends SQL where Cloudberry sends them, and names
 * each SQL command by the protocol command it stands in for.
 */
typedef enum GpDtxCommand
{
	GP_DTX_NONE = 0,
	GP_DTX_ABORT_NO_PREPARED,
	GP_DTX_PREPARE,
	GP_DTX_ABORT_SOME_PREPARED,
	GP_DTX_COMMIT_ONEPHASE,
	GP_DTX_COMMIT_PREPARED,
	GP_DTX_ABORT_PREPARED,
	GP_DTX_RETRY_COMMIT_PREPARED,
	GP_DTX_RETRY_ABORT_PREPARED,
	GP_DTX_RECOVERY_COMMIT_PREPARED,
	GP_DTX_RECOVERY_ABORT_PREPARED,
	GP_DTX_SUBTRANSACTION_BEGIN,
	GP_DTX_SUBTRANSACTION_RELEASE,
	GP_DTX_SUBTRANSACTION_ROLLBACK
} GpDtxCommand;

/* Cloudberry's values of debug_dtm_action */
#define GP_DTM_ACTION_NONE			0
#define GP_DTM_ACTION_DELAY			1
#define GP_DTM_ACTION_FAIL_BEGIN	2
#define GP_DTM_ACTION_FAIL_END		3
#define GP_DTM_ACTION_PANIC_BEGIN	4

/* Cloudberry's debug_abort_after_distributed_prepared */
extern bool gp_debug_abort_after_distributed_prepared;

/*
 * What debug_dtm_action asks of content's part at a protocol command, or at
 * a SQL command of this tag -- GP_DTM_ACTION_NONE where it asks nothing --
 * and the error Cloudberry's segment raises there, which *msg is set to.
 * "level" is the coordinator's nesting level Cloudberry sends a
 * subtransaction's command with, 0 for the others.
 */
extern int	GpDtmDebugProtocol(GpDtxCommand command, int content, int level,
							   char **msg);
extern int	GpDtmDebugSql(const char *tag, int content, char **msg);

/*
 * The statement a segment raises that error by; and the one it runs before
 * a PREPARE TRANSACTION or COMMIT so that it fails as itself.
 */
extern char *GpDtmDebugRaiseStatement(int action, const char *msg);
extern char *GpDtmDebugFailAtCommitStatement(const char *msg);

/* Is a SQL command of this tag's failure asked for, of any segment? */
extern bool GpDtmDebugSqlArmed(const char *tag);

/* Do the settings apply to the gang there is, asking for anything? */
extern bool GpDtmDebugArmed(void);

/* Is subtransaction level "level" a user's SAVEPOINT (else an internal one)? */
extern bool GpDtmDebugLevelIsUser(int level);

/* The gang the settings apply to: made, and closed (gp_dispatch.c). */
extern void GpDtmDebugGangMade(void);
extern void GpDtmDebugGangClosed(void);

/*
 * A distributed transaction's gid as Cloudberry's messages give it, "gid=N",
 * the coordinator's transaction ID of the gp_dtx_ gid.
 */
extern char *GpDtmDebugGidDetail(const char *gid, const char *state);

extern void GpDtmDebugInit(void);

#endif							/* GP_DTM_DEBUG_H */
