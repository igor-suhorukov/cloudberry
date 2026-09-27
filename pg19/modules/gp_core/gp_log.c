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
 * gp_log.c
 *	  Cloudberry's own log of each server, a CSV file of thirty columns
 *	  beside PostgreSQL's log, and gp_toolkit's functions that read it.
 *
 * Cloudberry's servers log in a format of their own: each message a server
 * logs is a record of thirty columns in log/gpdb-<time>.csv under its data
 * directory -- when and by whom, the session and the command it is part of,
 * the segment, the transactions, and the message with the statement that
 * raised it -- which gp_toolkit's __gp_log_segment_ext and
 * __gp_log_coordinator_ext read, external tables of each node's files.
 * PostgreSQL 19 logs where it is configured to, which the port leaves as it
 * is, and so what the client is sent.  gp_core writes Cloudberry's file
 * beside PostgreSQL's log, from emit_log_hook: for each message PostgreSQL
 * logs, one record, written by the process that logs it, as Cloudberry's
 * syslogger writes the record a backend sends it
 * (syslogger_write_errordata()).  gp.log_format, Cloudberry's gp_log_format,
 * says whether: csv, the default, as Cloudberry's, or text, where a node's
 * log is PostgreSQL's alone.
 *
 * What a record says is what Cloudberry's does:
 *
 *   - its message, detail and hint without the whitespace they end in,
 *     where the client is sent the message as well (cdb_tidy_message());
 *   - the statement, where PostgreSQL would log it: on a segment, the
 *     coordinator's statement the backend runs a part of, which is
 *     Cloudberry's segment's debug_query_string.  A DDL tree's text carries
 *     it (gp_ddl.c), and a statement the coordinator sends a segment for
 *     its client ends with it in a comment (GpLogStatementComment()): a
 *     gather's cursor, whose FETCHes are its statement's too, and a
 *     reader's slice.  A segment's lines of such a statement --
 *     log_min_duration_statement's and log_statement's -- name it too;
 *   - after an ERROR's record, one of its statement, "An exception was
 *     encountered during the execution of statement: ...", which
 *     Cloudberry's backend logs as it recovers from the error
 *     (elog_exception_statement());
 *   - the session, con<N>, and on the coordinator the client's command it
 *     is part of, cmd<N>, a number for each statement of the client's that
 *     logs; the node's content id, seg<N>; the distributed transaction,
 *     which is the coordinator's own, dx<N>, and the local one and the
 *     subtransaction.
 *
 * The file is gpdb-%Y-%m-%d_%H%M%S.csv in log_directory -- the directory
 * PostgreSQL's collector writes in, log/ under the data directory unless it
 * is set, which gp_core makes where no collector has -- named by the time it
 * was begun, and begun again as PostgreSQL's collector begins its own:
 * each log_rotation_age, at a multiple of it in log_timezone, and after
 * log_rotation_size.  Every process appends to it, each record in one
 * write(), which a file opened for appending keeps whole; a file begun
 * after the size is every process's next through shared memory.  A message
 * logged before gp_core was loaded is in PostgreSQL's log alone.
 *
 * gp_toolkit's functions read the node's files, in their names' order, a
 * record at a time, as Cloudberry's external tables read them with cat: a
 * record not whole -- one being written -- is passed over, and so is a line
 * that is none of these records.
 *
 * Cloudberry sources this file stands in for:
 *	  src/backend/utils/error/elog.c (write_message_to_server_log(),
 *	  elog_exception_statement(), cdb_tidy_message()),
 *	  src/backend/postmaster/syslogger.c (syslogger_write_errordata()) and
 *	  gpcontrib/gp_toolkit/gp_toolkit--1.3.sql's external tables of the logs
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>

#include "access/xact.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "libpq/libpq-be.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/miscnodes.h"
#include "pgstat.h"
#include "pgtime.h"
#include "postmaster/syslogger.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/portal.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_log.h"

/* The columns of a record, as gp_toolkit's external tables have them. */
#define GP_LOG_COLUMNS		30

/* How the file is named: by the time it was begun, in log_timezone. */
#define GP_LOG_FILENAME		"gpdb-%Y-%m-%d_%H%M%S.csv"

/* The comment GpLogStatementComment() makes, "/" "*gp:statement ..." "*" "/". */
#define GP_STATEMENT_MARKER	"/*gp:statement "

/* gp.log_format: Cloudberry's gp_log_format. */
typedef enum GpLogFormat
{
	GP_LOG_FORMAT_TEXT,
	GP_LOG_FORMAT_CSV,
} GpLogFormat;

static int	gp_log_format = GP_LOG_FORMAT_CSV;

static const struct config_enum_entry gp_log_format_options[] = {
	{"text", GP_LOG_FORMAT_TEXT, false},
	{"csv", GP_LOG_FORMAT_CSV, false},
	{NULL, 0, false}
};

/*
 * The file every process appends to, by the time it was begun: a process
 * whose file is another opens this one before it writes.
 */
typedef struct GpLogShared
{
	slock_t		mutex;
	pg_time_t	begun;
} GpLogShared;

static GpLogShared *log_shared = NULL;

/* This process's file: its descriptor, its time, and the directory it is in. */
static int	log_fd = -1;
static pg_time_t log_begun = 0;
static char log_dir[MAXPGPATH];

/* In the hook, which logs nothing of its own, and writes nothing twice. */
static bool log_writing = false;

/*
 * The client's commands, as Cloudberry's gp_command_count counts them, and
 * the start of the last one counted: cmd<N> of a record.
 */
static int	log_command_count = 0;
static TimestampTz log_command_start = 0;

/*
 * coordinator_statement()'s last answer, and what it was of: the text a
 * message from the coordinator was, and the start of the statement --
 * the next message's text may be at the same place.
 */
static const char *coordinator_of = NULL;
static TimestampTz coordinator_start = 0;
static char *coordinator_text = NULL;

static emit_log_hook_type prev_emit_log_hook = NULL;
static shmem_request_hook_type prev_shmem_request = NULL;
static shmem_startup_hook_type prev_shmem_startup = NULL;

/* ------------------------------------------------------------------------- */
/* The coordinator's statement, on a segment                                 */
/* ------------------------------------------------------------------------- */

/*
 * The comment a statement the coordinator sends a segment for its client
 * ends with: GP_STATEMENT_MARKER, the client's statement -- clipped as
 * pg_stat_activity keeps it, as a DDL tree's text carries it (gp_ddl.c) --
 * and the comment's end.  Each "*" of the statement is written "\s" and
 * each "\" "\\", so that nothing in it starts or ends a comment, which
 * PostgreSQL's lexer nests.
 */
const char *
GpLogStatementComment(void)
{
	StringInfoData buf;
	int			len;

	if (debug_query_string == NULL || GpClusterIsDispatched())
		return "";

	len = pg_mbcliplen(debug_query_string, strlen(debug_query_string),
					   pgstat_track_activity_query_size - 1);
	initStringInfo(&buf);
	appendStringInfoString(&buf, " " GP_STATEMENT_MARKER);
	for (int i = 0; i < len; i++)
	{
		char		c = debug_query_string[i];

		if (c == '*')
			appendStringInfoString(&buf, "\\s");
		else if (c == '\\')
			appendStringInfoString(&buf, "\\\\");
		else
			appendStringInfoChar(&buf, c);
	}
	appendStringInfoString(&buf, "*/");
	return buf.data;
}

/*
 * The statement a text ends with in such a comment, or NULL.  Nothing in
 * the comment but its start has a "*", so its start is the last one before
 * its end.
 */
static char *
statement_of(const char *text)
{
	size_t		n = strlen(text);
	size_t		markerlen = strlen(GP_STATEMENT_MARKER);
	const char *end;
	const char *p;
	StringInfoData buf;

	if (n < markerlen + 2 || strcmp(text + n - 2, "*/") != 0)
		return NULL;
	end = text + n - 2;
	for (p = end - 1; p > text && *p != '*'; p--)
		;
	if (p == text || strncmp(p - 1, GP_STATEMENT_MARKER, markerlen) != 0)
		return NULL;

	initStringInfo(&buf);
	for (p = p - 1 + markerlen; p < end; p++)
	{
		if (*p == '\\' && p + 1 < end)
		{
			p++;
			appendStringInfoChar(&buf, *p == 's' ? '*' : *p);
		}
		else
			appendStringInfoChar(&buf, *p);
	}
	return buf.data;
}

/*
 * On a segment, the coordinator's statement this backend runs a part of,
 * as what it was sent carries it: a DDL tree's text, and a statement that
 * ends with GpLogStatementComment()'s comment -- a gather's cursor, whose
 * FETCH and CLOSE are its statement's too, and a reader's slice.  NULL
 * where it is none of these.  Worked out once a message: what it was sent
 * may be a plan of megabytes, and a statement may log many lines.
 */
static const char *
coordinator_statement(void)
{
	const char *text = debug_query_string;
	TimestampTz start;
	char	   *statement = NULL;
	const char *tree;
	int			len;

	if (text == NULL || !GpClusterIsDispatched())
		return NULL;
	start = GetCurrentStatementStartTimestamp();
	if (text == coordinator_of && start == coordinator_start)
		return coordinator_text;

	if (GpDispatchIsTreeText(text))
	{
		if ((tree = GpDispatchTreeStatement(text, &len)) != NULL)
			statement = pnstrdup(tree, len);
	}
	else
	{
		if (strncmp(text, "FETCH ", 6) == 0 || strncmp(text, "CLOSE ", 6) == 0)
		{
			const char *cursor = strstr(text, "gp_gather_");
			Portal		portal = cursor != NULL ? GetPortalByName(cursor) : NULL;

			text = portal != NULL ? portal->sourceText : NULL;
		}
		if (text != NULL)
			statement = statement_of(text);
	}

	if (coordinator_text != NULL)
		pfree(coordinator_text);
	coordinator_text = statement != NULL
		? MemoryContextStrdup(TopMemoryContext, statement) : NULL;
	coordinator_of = debug_query_string;
	coordinator_start = start;
	return coordinator_text;
}

/* ------------------------------------------------------------------------- */
/* The file                                                                  */
/* ------------------------------------------------------------------------- */

static void
log_shmem_request(void)
{
	if (prev_shmem_request)
		prev_shmem_request();
	RequestAddinShmemSpace(MAXALIGN(sizeof(GpLogShared)));
}

static void
log_shmem_startup(void)
{
	bool		found;

	if (prev_shmem_startup)
		prev_shmem_startup();

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	log_shared = ShmemInitStruct("gp_core log", sizeof(GpLogShared), &found);
	if (!found)
	{
		SpinLockInit(&log_shared->mutex);
		log_shared->begun = 0;
	}
	LWLockRelease(AddinShmemInitLock);
}

/*
 * The time the file a record written now belongs in was begun, by
 * log_rotation_age: its last multiple, counted in log_timezone, as
 * PostgreSQL's collector begins a file by time (set_next_rotation_time());
 * the server's start where no file is begun by time.
 */
static pg_time_t
log_period(pg_time_t now, long gmtoff)
{
	int			interval;

	if (Log_RotationAge <= 0)
		return PgStartTime != 0 ? timestamptz_to_time_t(PgStartTime) : now;
	interval = Log_RotationAge * SECS_PER_MINUTE;
	now += gmtoff;
	now -= now % interval;
	return now - gmtoff;
}

/* Open the file begun at "begun", making log_directory where it is not. */
static void
log_open(pg_time_t begun)
{
	char		name[64];
	char		path[MAXPGPATH];

	if (log_fd >= 0)
		close(log_fd);
	log_fd = -1;

	pg_strftime(name, sizeof(name), GP_LOG_FILENAME,
				pg_localtime(&begun, log_timezone));
	snprintf(path, sizeof(path), "%s/%s", Log_directory, name);
	log_fd = open(path, O_WRONLY | O_APPEND | O_CREAT | PG_BINARY, Log_file_mode);
	if (log_fd < 0 && errno == ENOENT)
	{
		(void) MakePGDirectory(Log_directory);
		log_fd = open(path, O_WRONLY | O_APPEND | O_CREAT | PG_BINARY,
					  Log_file_mode);
	}
	log_begun = begun;
	strlcpy(log_dir, Log_directory, sizeof(log_dir));
}

/*
 * Append records to the file they belong in: the one every process writes
 * to now -- begun again when the time has come, and after this write when
 * it has grown past log_rotation_size.  Nothing is raised: a record that
 * cannot be written is lost, as the collector loses one.
 */
static void
log_write(const char *data, int len, pg_time_t now, long gmtoff)
{
	pg_time_t	begun = log_period(now, gmtoff);

	if (log_shared != NULL)
	{
		SpinLockAcquire(&log_shared->mutex);
		if (begun > log_shared->begun)
			log_shared->begun = begun;
		else
			begun = log_shared->begun;
		SpinLockRelease(&log_shared->mutex);
	}

	if (log_fd < 0 || begun != log_begun || strcmp(log_dir, Log_directory) != 0)
		log_open(begun);
	if (log_fd < 0)
		return;

	while (len > 0)
	{
		ssize_t		written = write(log_fd, data, len);

		if (written <= 0)
			return;
		data += written;
		len -= written;
	}

	if (log_shared != NULL && Log_RotationSize > 0 &&
		lseek(log_fd, 0, SEEK_CUR) >= (off_t) Log_RotationSize * 1024)
	{
		SpinLockAcquire(&log_shared->mutex);
		if (log_shared->begun == begun)
			log_shared->begun = Max(now, begun + 1);
		SpinLockRelease(&log_shared->mutex);
	}
}

/* ------------------------------------------------------------------------- */
/* A record                                                                  */
/* ------------------------------------------------------------------------- */

/* A text column: quoted, its quotes doubled; nothing for NULL.  Then a comma. */
static void
append_text(StringInfo buf, const char *s)
{
	if (s != NULL)
	{
		appendStringInfoCharMacro(buf, '"');
		for (; *s != '\0'; s++)
		{
			if (*s == '"')
				appendStringInfoCharMacro(buf, '"');
			appendStringInfoCharMacro(buf, *s);
		}
		appendStringInfoCharMacro(buf, '"');
	}
	appendStringInfoCharMacro(buf, ',');
}

/*
 * A number, after its prefix -- or nothing where "positive" asks for a
 * positive one and it is not, as Cloudberry's syslogger_write_int32()
 * leaves con, cmd, slice and the transactions out.  Then a comma.
 */
static void
append_int(StringInfo buf, const char *prefix, int32 value, bool positive)
{
	if (!positive || value > 0)
		appendStringInfo(buf, "%s%d", prefix, value);
	appendStringInfoCharMacro(buf, ',');
}

/* A time to the second, as the session's start is written.  Then a comma. */
static void
append_time(StringInfo buf, pg_time_t t)
{
	char		s[128];

	if (t != 0)
	{
		pg_strftime(s, sizeof(s), "%Y-%m-%d %H:%M:%S %Z",
					pg_localtime(&t, log_timezone));
		appendStringInfoString(buf, s);
	}
	appendStringInfoCharMacro(buf, ',');
}

/*
 * A message as Cloudberry logs one it sends the client too: without the
 * whitespace it ends in, and NULL where nothing else is left
 * (cdb_strip_trailing_whitespace()).
 */
static const char *
tidy(const char *s, bool trim)
{
	size_t		n;

	if (s == NULL || !trim)
		return s;
	n = strlen(s);
	while (n > 0 && (unsigned char) s[n - 1] <= ' ')
		n--;
	return n == 0 ? NULL : pnstrdup(s, n);
}

/*
 * The command a client backend's record is part of, on the coordinator: a
 * new number for each statement of the client's -- each has a start of its
 * own -- as Cloudberry's coordinator counts a statement it is sent
 * (increment_command_count()).  0 for none; a segment's records have none.
 */
static int32
log_command(void)
{
	TimestampTz start;

	if (MyBackendType != B_BACKEND || GpClusterIsDispatched())
		return 0;
	start = GetCurrentStatementStartTimestamp();
	if (start != log_command_start)
	{
		log_command_start = start;
		if (++log_command_count <= 0)
			log_command_count = 1;
	}
	return log_command_count;
}

/*
 * One record, in the columns and the order of Cloudberry's
 * syslogger_write_errordata(), onto "buf"; "now" and its offset from GMT in
 * log_timezone, for the file it goes in.
 */
static void
append_record(StringInfo buf, int elevel, int sqlerrcode, const char *message,
			  const char *detail, const char *hint, const char *internalquery,
			  int internalpos, const char *context, const char *statement,
			  int cursorpos, const char *funcname, const char *filename,
			  int lineno, const char *stack, pg_time_t *now, long *gmtoff)
{
	struct timeval tv;
	pg_time_t	stamp;
	struct pg_tm *tm;
	char		s[128];
	char		us[8];
	bool		client = MyBackendType == B_BACKEND;

	/* logtime, to the microsecond */
	gettimeofday(&tv, NULL);
	stamp = (pg_time_t) tv.tv_sec;
	tm = pg_localtime(&stamp, log_timezone);
	pg_strftime(s, sizeof(s), "%Y-%m-%d %H:%M:%S        %Z", tm);
	snprintf(us, sizeof(us), ".%06d", (int) tv.tv_usec);
	memcpy(s + 19, us, 7);
	appendStringInfoString(buf, s);
	appendStringInfoCharMacro(buf, ',');
	*now = stamp;
	*gmtoff = tm->tm_gmtoff;

	/* loguser, logdatabase, logpid, logthread, loghost, logport */
	append_text(buf, MyProcPort != NULL ? MyProcPort->user_name : NULL);
	append_text(buf, MyProcPort != NULL ? MyProcPort->database_name : NULL);
	append_int(buf, "p", MyProcPid, false);
	append_int(buf, "th", (int32) (uintptr_t) pthread_self(), false);
	append_text(buf, MyProcPort != NULL ? MyProcPort->remote_host : NULL);
	append_text(buf, MyProcPort != NULL ? MyProcPort->remote_port : NULL);

	/* logsessiontime, logtransaction */
	append_time(buf, MyProcPort != NULL ? timestamptz_to_time_t(MyStartTimestamp) : 0);
	append_int(buf, "", (int32) GetTopTransactionIdIfAny(), false);

	/*
	 * logsession, logcmdcount, logsegment, logslice, logdistxact,
	 * loglocalxact, logsubxact: the port knows no slice by number on a
	 * segment, and its distributed transaction is the coordinator's own.
	 */
	append_int(buf, "con", client ? GpClusterSessionId() : 0, true);
	append_int(buf, "cmd", log_command(), true);
	append_int(buf, "seg", GpClusterContentId(), false);
	append_int(buf, "slice", 0, true);
	append_int(buf, "dx", GpClusterBackendRole() == GP_ROLE_DISPATCH
			   ? (int32) GetTopTransactionIdIfAny() : 0, true);
	append_int(buf, "x", (int32) GetCurrentTransactionIdIfAny(), true);
	append_int(buf, "sx", (int32) GetCurrentSubTransactionId(), true);

	/* logseverity, logstate, logmessage, logdetail, loghint */
	append_text(buf, error_severity(elevel));
	append_text(buf, unpack_sql_state(sqlerrcode));
	append_text(buf, message);
	append_text(buf, detail);
	append_text(buf, hint);

	/* logquery, logquerypos, logcontext, logdebug, logcursorpos */
	append_text(buf, internalquery);
	append_int(buf, "", internalpos, true);
	append_text(buf, context);
	append_text(buf, statement);
	append_int(buf, "", cursorpos, false);

	/* logfunction, logfile, logline, logstack */
	append_text(buf, funcname);
	append_text(buf, filename);
	append_int(buf, "", lineno, true);
	if (stack != NULL)
	{
		append_text(buf, stack);
		buf->len--;				/* the last column: no comma */
	}
	appendStringInfoCharMacro(buf, '\n');
}

/*
 * Would a message of this level be logged here: is_log_level_output() for
 * log_min_messages, of LOG, which PostgreSQL ranks above ERROR in the log.
 */
static bool
log_is_logged(void)
{
	int			min = log_min_messages[MyBackendType];

	return min == LOG || min <= ERROR;
}

/* ------------------------------------------------------------------------- */
/* The hooks                                                                 */
/* ------------------------------------------------------------------------- */

static void
log_emit(ErrorData *edata)
{
	StringInfoData buf;
	const char *message;
	const char *statement = NULL;
	const char *coordinator;
	bool		trim;
	pg_time_t	now;
	long		gmtoff;

	if (prev_emit_log_hook)
		prev_emit_log_hook(edata);

	if (gp_log_format != GP_LOG_FORMAT_CSV || !edata->output_to_server ||
		log_writing || log_timezone == NULL)
		return;

	log_writing = true;
	PG_TRY();
	{
		coordinator = coordinator_statement();
		trim = edata->output_to_client;
		message = tidy(edata->message, trim);

		/*
		 * A segment's line of a statement it was sent, which ends with the
		 * statement's text -- "duration: ... statement: ...", "statement:
		 * ..." -- names the coordinator's statement instead, as Cloudberry's
		 * segment's names the statement it was dispatched.
		 */
		if (coordinator != NULL && edata->elevel == LOG && message != NULL)
		{
			size_t		n = strlen(message);
			size_t		m = strlen(debug_query_string);

			if (m > 0 && n >= m && memcmp(message + n - m, debug_query_string, m) == 0)
				message = psprintf("%.*s%s", (int) (n - m), message, coordinator);
		}

		if (check_log_of_query(edata))
			statement = coordinator != NULL ? coordinator : debug_query_string;

		initStringInfo(&buf);
		append_record(&buf, edata->elevel, edata->sqlerrcode, message,
					  tidy(edata->detail_log != NULL ? edata->detail_log : edata->detail, trim),
					  tidy(edata->hint, trim),
					  edata->internalquery, edata->internalpos,
					  edata->hide_ctx ? NULL : edata->context,
					  statement, edata->cursorpos, edata->funcname,
					  edata->filename, edata->lineno, edata->backtrace,
					  &now, &gmtoff);

		/*
		 * A client backend's error, whose statement is logged: the statement
		 * again, as Cloudberry's backend logs it as it recovers from the
		 * error (elog_exception_statement()).
		 */
		if (edata->elevel == ERROR && MyBackendType == B_BACKEND &&
			statement != NULL && log_is_logged())
			append_record(&buf, LOG, ERRCODE_SUCCESSFUL_COMPLETION,
						  psprintf("An exception was encountered during the execution of statement: %s",
								   statement),
						  NULL, NULL, NULL, 0, NULL, statement, 0, NULL, NULL, 0,
						  NULL, &now, &gmtoff);

		log_write(buf.data, buf.len, now, gmtoff);
		pfree(buf.data);
	}
	PG_FINALLY();
	{
		log_writing = false;
	}
	PG_END_TRY();
}

/* ------------------------------------------------------------------------- */
/* Reading                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * A record as the reader has it so far: its fields, each ended by a '\0' in
 * one buffer; and where in a field it is.
 */
typedef enum LogField
{
	FIELD_START,				/* at a field's start */
	FIELD_PLAIN,				/* in one not quoted */
	FIELD_QUOTED,				/* in quotes */
	FIELD_QUOTE,				/* at a quote in quotes: the end, or doubled */
} LogField;

typedef struct LogReader
{
	StringInfoData data;
	int			start[GP_LOG_COLUMNS];
	bool		isnull[GP_LOG_COLUMNS];
	int			nfields;		/* the fields ended; more than GP_LOG_COLUMNS
								 * is a line of something else */
	bool		quoted;			/* the field began with a quote */
	LogField	state;

	/* where the rows go, and how each column is read */
	ReturnSetInfo *rsinfo;
	FmgrInfo	in[GP_LOG_COLUMNS];
	Oid			ioparam[GP_LOG_COLUMNS];
	Oid			type[GP_LOG_COLUMNS];
	MemoryContext rowcxt;
} LogReader;

/*
 * A column's text as a value of this database: bytes of another encoding --
 * the log is every database's -- each read as "?".
 */
static Datum
log_text(const char *s, int len)
{
	int			encoding = GetDatabaseEncoding();
	int			valid = pg_encoding_verifymbstr(encoding, s, len);
	StringInfoData buf;

	if (valid == len)
		return PointerGetDatum(cstring_to_text_with_len(s, len));

	initStringInfo(&buf);
	while (len > 0)
	{
		appendBinaryStringInfo(&buf, s, valid);
		if (valid == len)
			break;
		appendStringInfoChar(&buf, '?');
		s += valid + 1;
		len -= valid + 1;
		valid = pg_encoding_verifymbstr(encoding, s, len);
	}
	return PointerGetDatum(cstring_to_text_with_len(buf.data, buf.len));
}

/* The field just read ends: whether it is NULL, and where the next begins. */
static void
reader_end_field(LogReader *r)
{
	if (r->nfields < GP_LOG_COLUMNS)
		r->isnull[r->nfields] = !r->quoted && r->data.len == r->start[r->nfields];
	appendStringInfoCharMacro(&r->data, '\0');
	if (++r->nfields < GP_LOG_COLUMNS)
		r->start[r->nfields] = r->data.len;
	r->quoted = false;
	r->state = FIELD_START;
}

/* A record's line ends: a row, if it is one of thirty fields; then the next. */
static void
reader_end_record(LogReader *r)
{
	reader_end_field(r);

	if (r->nfields == GP_LOG_COLUMNS)
	{
		Datum		values[GP_LOG_COLUMNS];
		bool		nulls[GP_LOG_COLUMNS];
		MemoryContext oldcxt = MemoryContextSwitchTo(r->rowcxt);

		for (int i = 0; i < GP_LOG_COLUMNS; i++)
		{
			char	   *s = r->data.data + r->start[i];

			nulls[i] = r->isnull[i];
			values[i] = (Datum) 0;
			if (nulls[i])
				continue;
			if (r->type[i] == TEXTOID)
				values[i] = log_text(s, strlen(s));
			else
			{
				ErrorSaveContext escontext = {T_ErrorSaveContext};

				if (!InputFunctionCallSafe(&r->in[i], s, r->ioparam[i], -1,
										   (Node *) &escontext, &values[i]))
					nulls[i] = true;
			}
		}
		tuplestore_putvalues(r->rsinfo->setResult, r->rsinfo->setDesc,
							 values, nulls);
		MemoryContextSwitchTo(oldcxt);
		MemoryContextReset(r->rowcxt);
	}

	resetStringInfo(&r->data);
	r->nfields = 0;
	r->start[0] = 0;
}

/*
 * A buffer of a file, as CSV of NULL '' and QUOTE '"': a field is NULL
 * where it is empty and not quoted, and a quoted one may hold commas, lines'
 * ends and its quotes doubled.
 */
static void
reader_feed(LogReader *r, const char *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
	{
		char		c = p[i];

		switch (r->state)
		{
			case FIELD_QUOTED:
				if (c == '"')
					r->state = FIELD_QUOTE;
				else
					appendStringInfoCharMacro(&r->data, c);
				continue;
			case FIELD_QUOTE:
				if (c == '"')
				{
					appendStringInfoCharMacro(&r->data, '"');
					r->state = FIELD_QUOTED;
					continue;
				}
				r->state = FIELD_PLAIN;
				break;
			case FIELD_START:
				if (c == '"')
				{
					r->quoted = true;
					r->state = FIELD_QUOTED;
					continue;
				}
				r->state = FIELD_PLAIN;
				break;
			case FIELD_PLAIN:
				break;
		}

		if (c == ',')
			reader_end_field(r);
		else if (c == '\n')
			reader_end_record(r);
		else
			appendStringInfoCharMacro(&r->data, c);
	}
}

/* A file whose name is Cloudberry's log's. */
static bool
log_file_name(const char *name)
{
	size_t		n = strlen(name);

	return n > 9 && strncmp(name, "gpdb-", 5) == 0 &&
		strcmp(name + n - 4, ".csv") == 0;
}

static int
cmp_names(const void *a, const void *b)
{
	return strcmp(*(const char *const *) a, *(const char *const *) b);
}

/* Every record of this node's files, as the function's rows. */
static void
log_read(FunctionCallInfo fcinfo)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	LogReader	r;
	DIR		   *dir;
	struct dirent *de;
	List	   *names = NIL;
	char	  **sorted;
	int			n = 0;
	char	   *buf;

	InitMaterializedSRF(fcinfo, 0);
	if (rsinfo->setDesc->natts != GP_LOG_COLUMNS)
		elog(ERROR, "a log's row has %d columns, not %d",
			 rsinfo->setDesc->natts, GP_LOG_COLUMNS);

	memset(&r, 0, sizeof(r));
	r.rsinfo = rsinfo;
	for (int i = 0; i < GP_LOG_COLUMNS; i++)
	{
		Oid			proc;

		r.type[i] = TupleDescAttr(rsinfo->setDesc, i)->atttypid;
		getTypeInputInfo(r.type[i], &proc, &r.ioparam[i]);
		fmgr_info(proc, &r.in[i]);
	}
	r.rowcxt = AllocSetContextCreate(CurrentMemoryContext, "gp_core log row",
									 ALLOCSET_DEFAULT_SIZES);
	initStringInfo(&r.data);

	dir = AllocateDir(Log_directory);
	if (dir == NULL && errno == ENOENT)
		return;
	while ((de = ReadDir(dir, Log_directory)) != NULL)
		if (log_file_name(de->d_name))
			names = lappend(names, pstrdup(de->d_name));
	FreeDir(dir);

	sorted = palloc_array(char *, Max(list_length(names), 1));
	foreach_ptr(char, name, names)
		sorted[n++] = name;
	qsort(sorted, n, sizeof(char *), cmp_names);

	buf = palloc(65536);
	for (int i = 0; i < n; i++)
	{
		char		path[MAXPGPATH];
		FILE	   *f;
		size_t		got;

		snprintf(path, sizeof(path), "%s/%s", Log_directory, sorted[i]);
		if ((f = AllocateFile(path, PG_BINARY_R)) == NULL)
		{
			if (errno == ENOENT)
				continue;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\" for reading: %m", path)));
		}
		while ((got = fread(buf, 1, 65536, f)) > 0)
		{
			CHECK_FOR_INTERRUPTS();
			reader_feed(&r, buf, got);
		}
		if (ferror(f))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m", path)));
		FreeFile(f);

		/* a record not whole at a file's end is none */
		resetStringInfo(&r.data);
		r.nfields = 0;
		r.start[0] = 0;
		r.quoted = false;
		r.state = FIELD_START;
	}
	MemoryContextDelete(r.rowcxt);
}

PG_FUNCTION_INFO_V1(gp_log_segment_rows);
PG_FUNCTION_INFO_V1(gp_log_coordinator_rows);

/*
 * gp_toolkit.__gp_log_segment_rows(): each segment's records, read there --
 * Cloudberry's __gp_log_segment_ext, a cat of the CSV files in each
 * segment's log directory.
 */
Datum
gp_log_segment_rows(PG_FUNCTION_ARGS)
{
	if (!GpDispatchFunctionToSegments(fcinfo))
		log_read(fcinfo);
	return (Datum) 0;
}

/*
 * gp_toolkit.__gp_log_coordinator_rows(): this node's records --
 * __gp_log_coordinator_ext, the same ON COORDINATOR.
 */
Datum
gp_log_coordinator_rows(PG_FUNCTION_ARGS)
{
	log_read(fcinfo);
	return (Datum) 0;
}

/* ------------------------------------------------------------------------- */
/* Start-up                                                                  */
/* ------------------------------------------------------------------------- */

void
GpLogInit(void)
{
	DefineCustomEnumVariable("gp.log_format",
							 "Sets the format of the server's own log.",
							 "csv writes Cloudberry's log beside PostgreSQL's, a "
							 "CSV file of thirty columns in log_directory, which "
							 "gp_toolkit's views of the logs read; text writes "
							 "none, the server's log being PostgreSQL's.  "
							 "Cloudberry calls this gp_log_format.",
							 &gp_log_format,
							 GP_LOG_FORMAT_CSV,
							 gp_log_format_options,
							 PGC_SIGHUP,
							 0,
							 NULL, NULL, NULL);

	prev_shmem_request = shmem_request_hook;
	shmem_request_hook = log_shmem_request;
	prev_shmem_startup = shmem_startup_hook;
	shmem_startup_hook = log_shmem_startup;

	prev_emit_log_hook = emit_log_hook;
	emit_log_hook = log_emit;
}
