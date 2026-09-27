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
 * gp_dtm_debug.c
 *	  Cloudberry's debug_dtm_action: a segment's part of a distributed
 *	  transaction failing at a command, as its tests ask.
 *
 * Cloudberry's segment raises an error as it begins or ends a DTX protocol
 * command -- Distributed Prepare, Distributed Commit Prepared, a
 * subtransaction's begin, release or rollback -- or a dispatched SQL
 * command of a tag, where the settings name it (exec_mpp_dtx_protocol_command()
 * and exec_simple_query(), tcop/postgres.c); and its coordinator answers the
 * failure as it answers any: a retry of Commit Prepared over a new gang, an
 * abort, a WARNING.  The settings are not synced: a SET reaches the gang
 * that exists, or the one it makes, and a gang made later -- the retry's --
 * has them at their defaults.
 *
 * The port's segments are sent SQL where Cloudberry's are sent protocol
 * commands -- PREPARE TRANSACTION, COMMIT PREPARED, SAVEPOINT gp_sp_N -- and
 * a SET is not sent to them.  So the coordinator decides: at each command
 * it sends, it asks this file what the settings ask of each segment's part
 * (GpDtmDebugProtocol(), GpDtmDebugSql()), and sends the segment named the
 * error Cloudberry's raises, for that segment to raise itself -- instead of
 * the command, or after it (gp_internal.dtm_raise()); a PREPARE and a
 * one-phase COMMIT fail as themselves, the error raised as the segment
 * prepares or commits (gp_internal.dtm_fail_at_commit()).  Which gang the
 * settings apply to is kept as Cloudberry's gangs keep them: a SET arms the
 * gang that exists, or the next one, and a gang made after it is not armed.
 *
 * A function's block with an exception handler is a subtransaction, which
 * Cloudberry's coordinator sends the segments as the block begins, and whose
 * rollback's failure there it raises as the block cleans up; the port sends
 * it with the block's first statement that goes to the segments, and rolls
 * it back as the coordinator's abort, which raises nothing.  While the
 * settings ask for a subtransaction's failure, a PL/pgSQL plugin sends it as
 * Cloudberry's does, as the block begins, and raises what the segments
 * answered its rollback with at the next statement (gp_dispatch.c).
 *
 * Cloudberry sources this file stands in for:
 *	  src/backend/tcop/postgres.c (CheckDebugDtmActionProtocol(),
 *	  CheckDebugDtmActionSqlCommandTag() and their errors) and the settings
 *	  of src/backend/utils/misc/guc_gp.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "nodes/parsenodes.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"

#include "gp_dispatch.h"
#include "gp_dtm_debug.h"
#include "gp_dtx.h"
#include "plpgsql.h"

/* Cloudberry's ERRCODE_FAULT_INJECT */
#define ERRCODE_GP_FAULT_INJECT		MAKE_SQLSTATE('X','X','0','0','9')

#define GP_DTM_TARGET_NONE		0
#define GP_DTM_TARGET_PROTOCOL	1
#define GP_DTM_TARGET_SQL		2

static int	gp_debug_dtm_action = GP_DTM_ACTION_NONE;
static int	gp_debug_dtm_action_target = GP_DTM_TARGET_NONE;
static int	gp_debug_dtm_action_protocol = GP_DTX_NONE;
static int	gp_debug_dtm_action_segment = -2;
static char *gp_debug_dtm_action_sql_command_tag = NULL;
static int	gp_debug_dtm_action_nestinglevel = 0;
static bool gp_debug_dtm_action_primary = true;
static bool gp_debug_print_full_dtm = false;
bool		gp_debug_abort_after_distributed_prepared = false;

static const struct config_enum_entry dtm_action_options[] = {
	{"none", GP_DTM_ACTION_NONE, false},
	{"delay", GP_DTM_ACTION_DELAY, false},
	{"fail_begin_command", GP_DTM_ACTION_FAIL_BEGIN, false},
	{"fail_end_command", GP_DTM_ACTION_FAIL_END, false},
	{"panic_begin_command", GP_DTM_ACTION_PANIC_BEGIN, false},
	{NULL, 0, false}
};

static const struct config_enum_entry dtm_target_options[] = {
	{"none", GP_DTM_TARGET_NONE, false},
	{"protocol", GP_DTM_TARGET_PROTOCOL, false},
	{"sql", GP_DTM_TARGET_SQL, false},
	{NULL, 0, false}
};

static const struct config_enum_entry dtm_protocol_options[] = {
	{"none", GP_DTX_NONE, false},
	{"abort_no_prepared", GP_DTX_ABORT_NO_PREPARED, false},
	{"prepare", GP_DTX_PREPARE, false},
	{"abort_some_prepared", GP_DTX_ABORT_SOME_PREPARED, false},
	{"commit_onephase", GP_DTX_COMMIT_ONEPHASE, false},
	{"commit_prepared", GP_DTX_COMMIT_PREPARED, false},
	{"abort_prepared", GP_DTX_ABORT_PREPARED, false},
	{"retry_commit_prepared", GP_DTX_RETRY_COMMIT_PREPARED, false},
	{"retry_abort_prepared", GP_DTX_RETRY_ABORT_PREPARED, false},
	{"recovery_commit_prepared", GP_DTX_RECOVERY_COMMIT_PREPARED, false},
	{"recovery_abort_prepared", GP_DTX_RECOVERY_ABORT_PREPARED, false},
	{"subtransaction_begin", GP_DTX_SUBTRANSACTION_BEGIN, false},
	{"subtransaction_release", GP_DTX_SUBTRANSACTION_RELEASE, false},
	{"subtransaction_rollback", GP_DTX_SUBTRANSACTION_ROLLBACK, false},
	{NULL, 0, false}
};

/* A protocol command's name, as Cloudberry's command tags give it (cmdtaglist.h). */
static const char *
dtx_command_name(GpDtxCommand command)
{
	switch (command)
	{
		case GP_DTX_NONE:
			return "None";
		case GP_DTX_ABORT_NO_PREPARED:
			return "Distributed Abort (No Prepared)";
		case GP_DTX_PREPARE:
			return "Distributed Prepare";
		case GP_DTX_ABORT_SOME_PREPARED:
			return "Distributed Abort (Some Prepared)";
		case GP_DTX_COMMIT_ONEPHASE:
			return "Distributed Commit (one-phase)";
		case GP_DTX_COMMIT_PREPARED:
			return "Distributed Commit Prepared";
		case GP_DTX_ABORT_PREPARED:
			return "Distributed Abort Prepared";
		case GP_DTX_RETRY_COMMIT_PREPARED:
			return "Retry Distributed Commit Prepared";
		case GP_DTX_RETRY_ABORT_PREPARED:
			return "Retry Distributed Abort Prepared";
		case GP_DTX_RECOVERY_COMMIT_PREPARED:
			return "Recovery Commit Prepared";
		case GP_DTX_RECOVERY_ABORT_PREPARED:
			return "Recovery Abort Prepared";
		case GP_DTX_SUBTRANSACTION_BEGIN:
			return "Begin Internal Subtransaction";
		case GP_DTX_SUBTRANSACTION_RELEASE:
			return "Release Current Subtransaction";
		case GP_DTX_SUBTRANSACTION_ROLLBACK:
			return "Rollback Current Subtransaction";
	}
	return "Unknown";
}

/* ------------------------------------------------------------------------- */
/* The gang the settings apply to                                            */
/* ------------------------------------------------------------------------- */

/*
 * Cloudberry's settings reach the gang that exists as they are set, or the
 * one the SET makes; a gang made later has them at their defaults.  So a
 * SET is stamped with that gang, and the settings apply while it lasts.
 */
static uint64 gang_generation = 0;	/* the gangs made so far */
static bool gang_alive = false;
static uint64 armed_generation = 0; /* the gang of the last SET; 0 none */

void
GpDtmDebugGangMade(void)
{
	gang_generation++;
	gang_alive = true;
}

void
GpDtmDebugGangClosed(void)
{
	gang_alive = false;
}

static bool
settings_apply(int content)
{
	return GpDtmDebugArmed() && gp_debug_dtm_action_segment == content;
}

bool
GpDtmDebugArmed(void)
{
	return gp_debug_dtm_action != GP_DTM_ACTION_NONE &&
		gang_alive && armed_generation == gang_generation;
}

/* ------------------------------------------------------------------------- */
/* What the settings ask                                                     */
/* ------------------------------------------------------------------------- */

char *
GpDtmDebugRaiseStatement(int action, const char *msg)
{
	return psprintf("SELECT gp_internal.dtm_raise(%d, %s)", action,
					quote_literal_cstr(msg));
}

char *
GpDtmDebugFailAtCommitStatement(const char *msg)
{
	return psprintf("SELECT gp_internal.dtm_fail_at_commit(%s)",
					quote_literal_cstr(msg));
}

int
GpDtmDebugProtocol(GpDtxCommand command, int content, int level, char **msg)
{
	int			action = gp_debug_dtm_action;
	const char *name = dtx_command_name(command);

	*msg = NULL;
	if (!settings_apply(content) ||
		gp_debug_dtm_action_target != GP_DTM_TARGET_PROTOCOL ||
		gp_debug_dtm_action_protocol != (int) command ||
		(gp_debug_dtm_action_nestinglevel != 0 &&
		 gp_debug_dtm_action_nestinglevel != level))
		return GP_DTM_ACTION_NONE;

	switch (action)
	{
		case GP_DTM_ACTION_FAIL_BEGIN:
			*msg = psprintf("Raise ERROR for debug_dtm_action = %d, debug_dtm_action_protocol = %s",
							action, name);
			break;
		case GP_DTM_ACTION_FAIL_END:
			*msg = psprintf("Raise error for debug_dtm_action = %d, debug_dtm_action_protocol = %s",
							action, name);
			break;
		case GP_DTM_ACTION_PANIC_BEGIN:
			*msg = psprintf("PANIC for debug_dtm_action = %d, debug_dtm_action_protocol = %s",
							action, name);
			break;
		default:
			/* "delay", which nothing of Cloudberry's reads either */
			return GP_DTM_ACTION_NONE;
	}
	return action;
}

int
GpDtmDebugSql(const char *tag, int content, char **msg)
{
	int			action = gp_debug_dtm_action;

	*msg = NULL;
	if (!settings_apply(content) ||
		gp_debug_dtm_action_target != GP_DTM_TARGET_SQL ||
		gp_debug_dtm_action_sql_command_tag == NULL ||
		strcmp(gp_debug_dtm_action_sql_command_tag, tag) != 0 ||
		(action != GP_DTM_ACTION_FAIL_BEGIN && action != GP_DTM_ACTION_FAIL_END))
		return GP_DTM_ACTION_NONE;

	*msg = psprintf("Raise ERROR for debug_dtm_action = %d, commandTag = %s",
					action, tag);
	return action;
}

bool
GpDtmDebugSqlArmed(const char *tag)
{
	return gp_debug_dtm_action != GP_DTM_ACTION_NONE &&
		gang_alive && armed_generation == gang_generation &&
		gp_debug_dtm_action_target == GP_DTM_TARGET_SQL &&
		gp_debug_dtm_action_sql_command_tag != NULL &&
		strcmp(gp_debug_dtm_action_sql_command_tag, tag) == 0;
}

char *
GpDtmDebugGidDetail(const char *gid, const char *state)
{
	FullTransactionId gxid;

	if (!GpDtxParseGid(gid, &gxid))
		return psprintf("gid=%s, state=%s", gid, state);
	return psprintf("gid=" UINT64_FORMAT ", state=%s",
					U64FromFullTransactionId(gxid), state);
}

/* ------------------------------------------------------------------------- */
/* A subtransaction's kind                                                   */
/* ------------------------------------------------------------------------- */

/*
 * A user's SAVEPOINT is sent as SQL, whose command tag the SQL target names;
 * a subtransaction a function begins -- PL/pgSQL's block with an exception
 * handler -- Cloudberry sends as a protocol command.  Which a level is, is
 * known as it starts: after a SAVEPOINT or ROLLBACK TO statement, a user's.
 */
static bool next_is_user = false;
static bool *level_is_user = NULL;
static int	level_is_user_size = 0;

bool
GpDtmDebugLevelIsUser(int level)
{
	return level >= 0 && level < level_is_user_size && level_is_user[level];
}

static void
dtm_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
					 SubTransactionId parentSubid, void *arg)
{
	int			level;

	if (event != SUBXACT_EVENT_START_SUB)
		return;
	level = GetCurrentTransactionNestLevel();
	if (level >= level_is_user_size)
	{
		int			size = Max(16, level * 2);

		if (level_is_user == NULL)
			level_is_user = MemoryContextAllocZero(TopMemoryContext,
												   size * sizeof(bool));
		else
			level_is_user = repalloc0(level_is_user,
									  level_is_user_size * sizeof(bool),
									  size * sizeof(bool));
		level_is_user_size = size;
	}
	level_is_user[level] = next_is_user;
	next_is_user = false;
}

/* ------------------------------------------------------------------------- */
/* A function's blocks                                                       */
/* ------------------------------------------------------------------------- */

/*
 * The PL/pgSQL plugin, installed while the settings ask for a failure of a
 * subtransaction's protocol command: at a block with an exception handler,
 * its subtransaction sent as the block begins -- a failure there is the
 * block's entry's, as Cloudberry's is -- and at every statement's start and
 * end, a rollback's failure the segments answered raised.  A plugin that was
 * there before is called as ever, and given the functions PL/pgSQL gives
 * the one that is.
 */
static PLpgSQL_plugin **plugin_ptr = NULL;
static PLpgSQL_plugin *prev_plugin = NULL;
static PLpgSQL_plugin dtm_plugin;

static bool
subtransactions_asked(void)
{
	return gp_debug_dtm_action != GP_DTM_ACTION_NONE &&
		gp_debug_dtm_action_target == GP_DTM_TARGET_PROTOCOL &&
		(gp_debug_dtm_action_protocol == GP_DTX_SUBTRANSACTION_BEGIN ||
		 gp_debug_dtm_action_protocol == GP_DTX_SUBTRANSACTION_RELEASE ||
		 gp_debug_dtm_action_protocol == GP_DTX_SUBTRANSACTION_ROLLBACK);
}

/* asked, of the gang there is or the one made next */
static bool
subtransactions_armed(void)
{
	return subtransactions_asked() &&
		armed_generation == (gang_alive ? gang_generation : gang_generation + 1);
}

static void
dtm_func_setup(PLpgSQL_execstate *estate, PLpgSQL_function *func)
{
	if (prev_plugin == NULL)
		return;
	prev_plugin->error_callback = dtm_plugin.error_callback;
	prev_plugin->assign_expr = dtm_plugin.assign_expr;
	prev_plugin->assign_value = dtm_plugin.assign_value;
	prev_plugin->eval_datum = dtm_plugin.eval_datum;
	prev_plugin->cast_value = dtm_plugin.cast_value;
	if (prev_plugin->func_setup)
		prev_plugin->func_setup(estate, func);
}

static void
dtm_func_beg(PLpgSQL_execstate *estate, PLpgSQL_function *func)
{
	if (prev_plugin != NULL && prev_plugin->func_beg)
		prev_plugin->func_beg(estate, func);
}

static void
dtm_func_end(PLpgSQL_execstate *estate, PLpgSQL_function *func)
{
	if (prev_plugin != NULL && prev_plugin->func_end)
		prev_plugin->func_end(estate, func);
}

static void
dtm_stmt_beg(PLpgSQL_execstate *estate, PLpgSQL_stmt *stmt)
{
	if (prev_plugin != NULL && prev_plugin->stmt_beg)
		prev_plugin->stmt_beg(estate, stmt);

	GpDispatchRaiseKeptError();
	if (stmt->cmd_type == PLPGSQL_STMT_BLOCK &&
		((PLpgSQL_stmt_block *) stmt)->exceptions != NULL &&
		subtransactions_armed())
	{
		const char *err_text = estate->err_text;

		/* where exec_stmt_block() begins it, and says so of an error */
		estate->err_text = gettext_noop("during statement block entry");
		GpDispatchSubtransactionBeginNow();
		estate->err_text = err_text;
	}
}

static void
dtm_stmt_end(PLpgSQL_execstate *estate, PLpgSQL_stmt *stmt)
{
	if (prev_plugin != NULL && prev_plugin->stmt_end)
		prev_plugin->stmt_end(estate, stmt);

	GpDispatchRaiseKeptError();
}

static PLpgSQL_plugin dtm_plugin = {
	.func_setup = dtm_func_setup,
	.func_beg = dtm_func_beg,
	.func_end = dtm_func_end,
	.stmt_beg = dtm_stmt_beg,
	.stmt_end = dtm_stmt_end,
};

/* after a SET: installed if the settings ask, and taken out if not */
static void
plugin_update(void)
{
	bool		asked = subtransactions_asked();

	if (plugin_ptr == NULL)
		plugin_ptr = (PLpgSQL_plugin **) find_rendezvous_variable("PLpgSQL_plugin");
	if (asked && *plugin_ptr != &dtm_plugin)
	{
		prev_plugin = *plugin_ptr;
		*plugin_ptr = &dtm_plugin;
	}
	else if (!asked && *plugin_ptr == &dtm_plugin)
	{
		*plugin_ptr = prev_plugin;
		prev_plugin = NULL;
	}
}

/* ------------------------------------------------------------------------- */
/* The coordinator's hook: SET, SAVEPOINT and ROLLBACK TO                    */
/* ------------------------------------------------------------------------- */

static ProcessUtility_hook_type prev_ProcessUtility = NULL;
static ExecutorStart_hook_type prev_ExecutorStart = NULL;

/*
 * The user's savepoints by name, and the level each is, for a ROLLBACK TO
 * sent as it is run (below); kept in the transaction's memory.
 */
typedef struct UserSavepoint
{
	char	   *name;
	int			level;
} UserSavepoint;

static List *user_savepoints = NIL;

static int
user_savepoint_level(const char *name)
{
	int			level = -1;

	foreach_ptr(UserSavepoint, sp, user_savepoints)
		if (strcmp(sp->name, name) == 0)
			level = sp->level;	/* the innermost of the name */
	return level;
}

static void
forget_levels_above(int level)
{
	foreach_ptr(UserSavepoint, sp, user_savepoints)
		if (sp->level > level)
			user_savepoints = foreach_delete_current(user_savepoints, sp);
}

static void
dtm_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
				   bool readOnlyTree, ProcessUtilityContext context,
				   ParamListInfo params, QueryEnvironment *queryEnv,
				   DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;

	if (context == PROCESS_UTILITY_TOPLEVEL)
		GpDispatchDropLostTempTables();

	/* a SET of one of them arms the gang that exists, or the next */
	if (IsA(parsetree, VariableSetStmt))
	{
		VariableSetStmt *stmt = (VariableSetStmt *) parsetree;

		if (stmt->name == NULL ||
			strncmp(stmt->name, "gp.debug_dtm_action", strlen("gp.debug_dtm_action")) == 0)
			armed_generation = gang_alive ? gang_generation : gang_generation + 1;
	}

	if (IsA(parsetree, TransactionStmt) && IsTransactionBlock())
	{
		TransactionStmt *stmt = (TransactionStmt *) parsetree;
		int			level = GetCurrentTransactionNestLevel();

		switch (stmt->kind)
		{
			case TRANS_STMT_SAVEPOINT:
				{
					MemoryContext oldcxt = MemoryContextSwitchTo(TopTransactionContext);
					UserSavepoint *sp = palloc_object(UserSavepoint);

					sp->name = pstrdup(stmt->savepoint_name);
					sp->level = level + 1;
					user_savepoints = lappend(user_savepoints, sp);
					MemoryContextSwitchTo(oldcxt);
					next_is_user = true;

					/*
					 * Cloudberry sends a SAVEPOINT to the segments as it runs
					 * it, where the port sends it with the next statement:
					 * where its failure is asked for, it is sent now.
					 */
					if (GpDtmDebugSqlArmed("SAVEPOINT"))
						GpDispatchSavepointNow(level + 1);
					break;
				}
			case TRANS_STMT_RELEASE:
				forget_levels_above(user_savepoint_level(stmt->savepoint_name) - 1);
				break;
			case TRANS_STMT_ROLLBACK_TO:
				{
					int			target = user_savepoint_level(stmt->savepoint_name);

					next_is_user = true;
					if (target > 0)
					{
						forget_levels_above(target);
						/* as SAVEPOINT's, before the coordinator's rollback */
						if (GpDtmDebugSqlArmed("ROLLBACK"))
							GpDispatchRollbackToNow(target);
					}
					break;
				}
			default:
				break;
		}
	}

	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context, params,
							queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);

	if (IsA(parsetree, VariableSetStmt))
		plugin_update();
}

/*
 * A statement's start: the temporary tables a retry's gang took with it,
 * dropped on the coordinator first (gp_dispatch.c).
 */
static void
dtm_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	if (queryDesc->plannedstmt->commandType != CMD_UTILITY &&
		!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
		GpDispatchDropLostTempTables();
	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

/* ------------------------------------------------------------------------- */
/* The segment's side                                                        */
/* ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(gp_dtm_raise);
PG_FUNCTION_INFO_V1(gp_dtm_fail_at_commit);

/*
 * gp_internal.dtm_raise(action, message): the error Cloudberry's segment
 * raises for debug_dtm_action, raised by this segment -- a PANIC for
 * panic_begin_command, as Cloudberry's.
 */
Datum
gp_dtm_raise(PG_FUNCTION_ARGS)
{
	int			action = PG_GETARG_INT32(0);
	char	   *msg = text_to_cstring(PG_GETARG_TEXT_PP(1));

	ereport(action == GP_DTM_ACTION_PANIC_BEGIN ? PANIC : ERROR,
			(errcode(ERRCODE_GP_FAULT_INJECT),
			 errmsg_internal("%s", msg)));
	PG_RETURN_VOID();
}

/*
 * The error a PREPARE TRANSACTION or a COMMIT this transaction ends with is
 * to fail with, as it prepares or commits: so that the segment's part ends
 * as after any failed PREPARE, rolled back, where an error before it would
 * leave the part in an aborted block (gp_internal.dtm_fail_at_commit()).
 */
static char *fail_at_commit = NULL;

Datum
gp_dtm_fail_at_commit(PG_FUNCTION_ARGS)
{
	fail_at_commit = MemoryContextStrdup(TopTransactionContext,
										 text_to_cstring(PG_GETARG_TEXT_PP(0)));
	PG_RETURN_VOID();
}

static void
dtm_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PRE_PREPARE:
			if (fail_at_commit != NULL)
			{
				char	   *msg = pstrdup(fail_at_commit);

				fail_at_commit = NULL;
				ereport(ERROR,
						(errcode(ERRCODE_GP_FAULT_INJECT),
						 errmsg_internal("%s", msg)));
			}
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PREPARE:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_PARALLEL_ABORT:
			/* the transaction's memory, gone with it */
			fail_at_commit = NULL;
			user_savepoints = NIL;
			next_is_user = false;
			break;
		default:
			break;
	}
}

/* ------------------------------------------------------------------------- */
/* Start-up                                                                  */
/* ------------------------------------------------------------------------- */

void
GpDtmDebugInit(void)
{
	/*
	 * Cloudberry's are GUC_NO_SHOW_ALL and GUC_SUPERUSER_ONLY too, which here
	 * would hide them from pg_settings, where the test harnesses find the
	 * names they respell.
	 */
	DefineCustomEnumVariable("gp.debug_dtm_action",
							 "Sets the debug DTM action.",
							 "Cloudberry calls this debug_dtm_action.",
							 &gp_debug_dtm_action,
							 GP_DTM_ACTION_NONE, dtm_action_options,
							 PGC_SUSET, GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	DefineCustomEnumVariable("gp.debug_dtm_action_target",
							 "Sets the debug DTM action target.",
							 "Cloudberry calls this debug_dtm_action_target.",
							 &gp_debug_dtm_action_target,
							 GP_DTM_TARGET_NONE, dtm_target_options,
							 PGC_SUSET, GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	DefineCustomEnumVariable("gp.debug_dtm_action_protocol",
							 "Sets the debug DTM action protocol.",
							 "Cloudberry calls this debug_dtm_action_protocol.",
							 &gp_debug_dtm_action_protocol,
							 GP_DTX_NONE, dtm_protocol_options,
							 PGC_SUSET, GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	DefineCustomIntVariable("gp.debug_dtm_action_segment",
							"Sets the debug DTM action segment.",
							"Cloudberry calls this debug_dtm_action_segment.",
							&gp_debug_dtm_action_segment,
							-2, -2, 1000,
							PGC_SUSET, GUC_NOT_IN_SAMPLE,
							NULL, NULL, NULL);
	DefineCustomStringVariable("gp.debug_dtm_action_sql_command_tag",
							   "Sets the debug DTM action sql command tag.",
							   "Cloudberry calls this debug_dtm_action_sql_command_tag.",
							   &gp_debug_dtm_action_sql_command_tag,
							   "",
							   PGC_SUSET, GUC_NOT_IN_SAMPLE,
							   NULL, NULL, NULL);
	DefineCustomIntVariable("gp.debug_dtm_action_nestinglevel",
							"Sets the debug DTM action transaction nesting level.",
							"Cloudberry calls this debug_dtm_action_nestinglevel; 0 is any.",
							&gp_debug_dtm_action_nestinglevel,
							0, 0, 1000,
							PGC_SUSET, GUC_NOT_IN_SAMPLE,
							NULL, NULL, NULL);
	DefineCustomBoolVariable("gp.debug_dtm_action_primary",
							 "Specify if the primary or mirror segment is the target of the debug DTM action.",
							 "Cloudberry calls this debug_dtm_action_primary; a mirror runs no command here either.",
							 &gp_debug_dtm_action_primary,
							 true,
							 PGC_SUSET, GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	DefineCustomBoolVariable("gp.debug_abort_after_distributed_prepared",
							 "Cause an abort after all segments are prepared but before the distributed commit is recorded.",
							 "Cloudberry calls this debug_abort_after_distributed_prepared.",
							 &gp_debug_abort_after_distributed_prepared,
							 false,
							 PGC_SUSET, GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	DefineCustomBoolVariable("gp.debug_print_full_dtm",
							 "Prints full DTM information to server log.",
							 "Cloudberry calls this debug_print_full_dtm; gp.dtx_log is the port's.",
							 &gp_debug_print_full_dtm,
							 false,
							 PGC_SUSET, GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);

	RegisterXactCallback(dtm_xact_callback, NULL);
	RegisterSubXactCallback(dtm_subxact_callback, NULL);
	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = dtm_ProcessUtility;
	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = dtm_ExecutorStart;
}
