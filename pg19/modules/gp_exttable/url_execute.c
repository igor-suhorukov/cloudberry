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
 * url_execute.c
 *	  An EXECUTE external table's command: run through the shell, its
 *	  standard output read or its standard input written, and its standard
 *	  error kept for the message if it fails.
 *
 * PostgreSQL's OpenPipeStream() leaves the command's stderr where the
 * server's goes, so the pipes are made here, as Cloudberry makes them.  A
 * child made by fork() does not inherit the interval timers the backend
 * arms, which Cloudberry saves and restores around it (cdbtimer.c).
 *
 * Cloudberry sources this file is made of:
 *	  src/backend/access/external/url_execute.c,
 *	  src/backend/storage/file/execute_pipe.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/resowner.h"

#include "gp_exttable.h"

#define EXEC_DATA_P 0			/* index to data pipe */
#define EXEC_ERR_P 1			/* index to error pipe  */

/*
 * What has to be cleaned up on error: the child's pipes, and the child,
 * waited for.  In TopMemoryContext, and released with its resource owner.
 */
typedef struct execute_handle_t
{
	int			pid;
	int			pipes[2];		/* only out and err needed */

	ResourceOwner owner;
	struct execute_handle_t *next;
	struct execute_handle_t *prev;
} execute_handle_t;

typedef struct URL_EXECUTE_FILE
{
	URL_FILE	common;
	char	   *shexec;			/* shell command-line */
	execute_handle_t *handle;
} URL_EXECUTE_FILE;

static execute_handle_t *open_execute_handles;
static bool execute_resowner_callback_registered;

static void pclose_without_stderr(int *pipes);
static char *interpretError(int exitCode, char *buf, size_t buflen, char *err, size_t errlen);
static const char *getSignalNameFromCode(int signo);
static void cleanup_execute_handle(execute_handle_t *h);

static execute_handle_t *
create_execute_handle(void)
{
	execute_handle_t *h;

	h = MemoryContextAlloc(TopMemoryContext, sizeof(execute_handle_t));
	h->pid = -1;
	h->pipes[EXEC_DATA_P] = -1;
	h->pipes[EXEC_ERR_P] = -1;

	h->owner = CurrentResourceOwner;
	h->next = open_execute_handles;
	h->prev = NULL;
	if (open_execute_handles)
		open_execute_handles->prev = h;
	open_execute_handles = h;

	return h;
}

static void
destroy_execute_handle(execute_handle_t *h)
{
	int			hpid = h->pid;

	cleanup_execute_handle(h);
	if (hpid != -1)
	{
		int			status;

		waitpid(hpid, &status, 0);
	}
}

static void
cleanup_execute_handle(execute_handle_t *h)
{
	if (h->prev)
		h->prev->next = h->next;
	else
		open_execute_handles = h->next;
	if (h->next)
		h->next->prev = h->prev;

	if (h->pipes[EXEC_DATA_P] != -1)
		close(h->pipes[EXEC_DATA_P]);
	if (h->pipes[EXEC_ERR_P] != -1)
		close(h->pipes[EXEC_ERR_P]);

	pfree(h);
}

static void
execute_abort_callback(ResourceReleasePhase phase, bool isCommit,
					   bool isTopLevel, void *arg)
{
	execute_handle_t *curr;
	execute_handle_t *next;

	if (phase != RESOURCE_RELEASE_AFTER_LOCKS)
		return;

	next = open_execute_handles;
	while (next)
	{
		curr = next;
		next = curr->next;

		if (curr->owner == CurrentResourceOwner)
		{
			if (isCommit)
				elog(WARNING, "execute-type external table reference leak: %p still referenced", curr);
			destroy_execute_handle(curr);
		}
	}
}

/*
 * popen() with the child's stderr on a pipe of its own: the data pipe the
 * parent reads (or, forwrite, writes), and the error pipe it reads.
 */
static int
popen_with_stderr(int *pipes, const char *exe, bool forwrite)
{
	int			data[2];
	int			err[2];
	int			pid;
	const int	READ = 0;
	const int	WRITE = 1;

	if (pipe(data) < 0)
		return -1;
	if (pipe(err) < 0)
	{
		close(data[READ]);
		close(data[WRITE]);
		return -1;
	}

	fflush(NULL);
	pid = fork();

	if (pid > 0)
	{
		if (forwrite)
		{
			close(data[READ]);
			pipes[EXEC_DATA_P] = data[WRITE];
		}
		else
		{
			close(data[WRITE]);
			pipes[EXEC_DATA_P] = data[READ];
		}
		close(err[WRITE]);
		pipes[EXEC_ERR_P] = err[READ];
		return pid;
	}
	else if (pid == 0)
	{
		if (forwrite)
		{
			close(data[WRITE]);
			if (dup2(data[READ], STDIN_FILENO) < 0)
				_exit(EXIT_FAILURE);
			close(data[READ]);
		}
		else
		{
			close(data[READ]);
			if (dup2(data[WRITE], STDOUT_FILENO) < 0)
				_exit(EXIT_FAILURE);
			close(data[WRITE]);
		}

		close(err[READ]);
		if (dup2(err[WRITE], STDERR_FILENO) < 0)
			_exit(EXIT_FAILURE);
		close(err[WRITE]);

		execl("/bin/sh", "sh", "-c", exe, (char *) NULL);
		_exit(EXIT_FAILURE);
	}

	close(data[READ]);
	close(data[WRITE]);
	close(err[READ]);
	close(err[WRITE]);
	return -1;
}

static void
read_err_msg(int fid, StringInfo sinfo)
{
	char		ebuf[512];

	for (;;)
	{
		int			nread = read(fid, ebuf, sizeof(ebuf));

		if (nread == 0)
			break;
		else if (nread > 0)
			appendBinaryStringInfo(sinfo, ebuf, nread);
		else if (errno == EINTR)
			continue;
		else
		{
			appendStringInfoString(sinfo, "error string unavailable due to read error");
			break;
		}
	}
}

/*
 * Close the pipes and return the child's termination status, its stderr in
 * sinfo.
 */
static int
pclose_with_stderr(int pid, int *pipes, StringInfo sinfo)
{
	int			status = 0;

	close(pipes[EXEC_DATA_P]);
	pipes[EXEC_DATA_P] = -1;
	read_err_msg(pipes[EXEC_ERR_P], sinfo);
	close(pipes[EXEC_ERR_P]);
	pipes[EXEC_ERR_P] = -1;

	while (waitpid(pid, &status, 0) < 0)
	{
		if (errno != EINTR)
			return -1;
	}
	return status;
}

static void
make_export(char *name, const char *value, StringInfo buf)
{
	char		ch;

	/* shell-quoted: every ' becomes '\'' */
	appendStringInfo(buf, "%s='", name);
	for (; 0 != (ch = *value); value++)
	{
		if (ch == '\'')
			appendStringInfoString(buf, "'\\'");
		appendStringInfoChar(buf, ch);
	}
	appendStringInfo(buf, "' && export %s && ", name);
}

char *
make_command(const char *cmd, extvar_t *ev)
{
	StringInfoData buf;

	initStringInfo(&buf);

	make_export("GP_MASTER_HOST", ev->GP_MASTER_HOST, &buf);
	make_export("GP_MASTER_PORT", ev->GP_MASTER_PORT, &buf);
	make_export("GP_SEG_PG_CONF", ev->GP_SEG_PG_CONF, &buf);
	make_export("GP_SEG_DATADIR", ev->GP_SEG_DATADIR, &buf);
	make_export("GP_DATABASE", ev->GP_DATABASE, &buf);
	make_export("GP_USER", ev->GP_USER, &buf);
	make_export("GP_DATE", ev->GP_DATE, &buf);
	make_export("GP_TIME", ev->GP_TIME, &buf);
	make_export("GP_XID", ev->GP_XID, &buf);
	make_export("GP_CID", ev->GP_CID, &buf);
	make_export("GP_SN", ev->GP_SN, &buf);
	make_export("GP_SEGMENT_ID", ev->GP_SEGMENT_ID, &buf);
	make_export("GP_SEG_PORT", ev->GP_SEG_PORT, &buf);
	make_export("GP_SESSION_ID", ev->GP_SESSION_ID, &buf);
	make_export("GP_SEGMENT_COUNT", ev->GP_SEGMENT_COUNT, &buf);
	if (ev->GP_QUERY_STRING)
		make_export("GP_QUERY_STRING", ev->GP_QUERY_STRING, &buf);

	appendStringInfoString(&buf, cmd);

	return buf.data;
}

URL_FILE *
url_execute_fopen(char *url, bool forwrite, extvar_t *ev)
{
	URL_EXECUTE_FILE *file;
	int			save_errno;
	char	   *cmd;

	Assert(strncmp(url, EXEC_URL_PREFIX, strlen(EXEC_URL_PREFIX)) == 0);
	cmd = url + strlen(EXEC_URL_PREFIX);

	file = palloc0(sizeof(URL_EXECUTE_FILE));
	file->common.type = CFTYPE_EXEC;
	file->common.url = pstrdup(url);
	file->shexec = make_command(cmd, ev);

	if (!execute_resowner_callback_registered)
	{
		RegisterResourceReleaseCallback(execute_abort_callback, NULL);
		execute_resowner_callback_registered = true;
	}

	file->handle = create_execute_handle();

	/*
	 * SIGPIPE as a command pipeline expects it, not ignored as a backend has
	 * it (PostgresMain()), which the child would inherit; ignored again after.
	 */
	pqsignal(SIGPIPE, PG_SIG_DFL);
	file->handle->pid = popen_with_stderr(file->handle->pipes, file->shexec,
										  forwrite);
	save_errno = errno;
	pqsignal(SIGPIPE, PG_SIG_IGN);

	elog(DEBUG5, "EXTERNAL TABLE EXECUTE Command: %s", file->shexec);
	if (file->handle->pid == -1)
	{
		errno = save_errno;
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("cannot start external table command: %m"),
				 errdetail("Command: %s", cmd)));
	}

	return (URL_FILE *) file;
}

void
url_execute_fclose(URL_FILE *file, bool failOnError, const char *relname)
{
	URL_EXECUTE_FILE *efile = (URL_EXECUTE_FILE *) file;
	StringInfoData sinfo;
	char	   *url;
	int			ret = 0;

	initStringInfo(&sinfo);

	if (failOnError)
		ret = pclose_with_stderr(efile->handle->pid, efile->handle->pipes, &sinfo);
	else
	{
		/*
		 * Not waited for, as Cloudberry does not wait: its next write to the
		 * pipe closed here ends it, and the query does not wait on it.
		 */
		pclose_without_stderr(efile->handle->pipes);
		efile->handle->pipes[EXEC_DATA_P] = -1;
		efile->handle->pipes[EXEC_ERR_P] = -1;
	}

	cleanup_execute_handle(efile->handle);
	efile->handle = NULL;

	url = pstrdup(file->url);
	if (ret == -1)
	{
		ereport((failOnError ? ERROR : LOG),
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("cannot close external table %s command: %m",
						(relname ? relname : "")),
				 errdetail("command: %s", url)));
	}
	else if (ret != 0)
	{
		char		buf[512];

		ereport((failOnError ? ERROR : LOG),
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("external table %s command ended with %s",
						(relname ? relname : ""),
						interpretError(ret, buf, sizeof(buf), sinfo.data, sinfo.len)),
				 errdetail("Command: %s", url)));
	}
	pfree(url);
	pfree(sinfo.data);
	pfree(file);
}

bool
url_execute_feof(URL_FILE *file, int bytesread)
{
	return (bytesread == 0);
}

bool
url_execute_ferror(URL_FILE *file, int bytesread, char *ebuf, int ebuflen)
{
	URL_EXECUTE_FILE *efile = (URL_EXECUTE_FILE *) file;
	int			ret;
	int			nread;

	ret = (bytesread == -1);
	if (ret && ebuflen > 0 && ebuf != NULL)
	{
		for (;;)
		{
			nread = read(efile->handle->pipes[EXEC_ERR_P], ebuf, ebuflen - 1);
			if (nread == -1 && errno == EINTR)
			{
				CHECK_FOR_INTERRUPTS();
				continue;
			}
			break;
		}

		if (nread != -1)
			ebuf[nread] = 0;
		else
			strlcpy(ebuf, "error string unavailable due to read error", ebuflen);
	}

	return ret;
}

size_t
url_execute_fread(void *ptr, size_t size, URL_FILE *file, CopyFromState pstate)
{
	URL_EXECUTE_FILE *efile = (URL_EXECUTE_FILE *) file;
	ssize_t		n;

	for (;;)
	{
		n = read(efile->handle->pipes[EXEC_DATA_P], ptr, size);
		if (n == -1 && errno == EINTR)
		{
			CHECK_FOR_INTERRUPTS();
			continue;
		}
		break;
	}

	return n;
}

size_t
url_execute_fwrite(void *ptr, size_t size, URL_FILE *file)
{
	URL_EXECUTE_FILE *efile = (URL_EXECUTE_FILE *) file;
	int			fd = efile->handle->pipes[EXEC_DATA_P];
	size_t		offset = 0;
	const char *p = (const char *) ptr;

	while (size > offset)
	{
		ssize_t		n = write(fd, p, size - offset);

		if (n == -1)
		{
			if (errno == EINTR)
			{
				CHECK_FOR_INTERRUPTS();
				continue;
			}
			return -1;
		}
		if (n == 0)
			break;
		offset += n;
		p = (const char *) ptr + offset;
	}

	if (offset < size)
		elog(WARNING, "partial write, expected %zu, written %zu", size, offset);

	return offset;
}

static char *
interpretError(int rc, char *buf, size_t buflen, char *err, size_t errlen)
{
	if (WIFEXITED(rc))
	{
		int			exitCode = WEXITSTATUS(rc);

		if (exitCode >= 128)
		{
			exitCode -= 128;
			snprintf(buf, buflen, "SHELL TERMINATED by signal %s (%d)",
					 getSignalNameFromCode(exitCode), exitCode);
		}
		else if (exitCode == 0)
			snprintf(buf, buflen, "EXITED; rc=%d", exitCode);
		else
			snprintf(buf, buflen, "error. %s", err);
	}
	else if (WIFSIGNALED(rc))
	{
		int			signalCode = WTERMSIG(rc);

		snprintf(buf, buflen, "TERMINATED by signal %s (%d)",
				 getSignalNameFromCode(signalCode), signalCode);
	}
	else if (WIFSTOPPED(rc))
	{
		int			signalCode = WSTOPSIG(rc);

		snprintf(buf, buflen, "STOPPED by signal %s (%d)",
				 getSignalNameFromCode(signalCode), signalCode);
	}
	else
		snprintf(buf, buflen, "UNRECOGNIZED termination; rc=%#x", rc);

	return buf;
}

static const char *
getSignalNameFromCode(int signo)
{
	static const struct
	{
		int			code;
		const char *name;
	}			signals[] = {
		{SIGHUP, "SIGHUP"}, {SIGINT, "SIGINT"}, {SIGQUIT, "SIGQUIT"},
		{SIGILL, "SIGILL"}, {SIGTRAP, "SIGTRAP"}, {SIGABRT, "SIGABRT"},
		{SIGFPE, "SIGFPE"}, {SIGKILL, "SIGKILL"}, {SIGBUS, "SIGBUS"},
		{SIGSEGV, "SIGSEGV"}, {SIGSYS, "SIGSYS"}, {SIGPIPE, "SIGPIPE"},
		{SIGALRM, "SIGALRM"}, {SIGTERM, "SIGTERM"}, {SIGURG, "SIGURG"},
		{SIGSTOP, "SIGSTOP"}, {SIGTSTP, "SIGTSTP"}, {SIGCONT, "SIGCONT"},
		{SIGCHLD, "SIGCHLD"}, {SIGTTIN, "SIGTTIN"}, {SIGTTOU, "SIGTTOU"},
		{SIGIO, "SIGIO"}, {SIGXCPU, "SIGXCPU"}, {SIGXFSZ, "SIGXFSZ"},
		{SIGVTALRM, "SIGVTALRM"}, {SIGPROF, "SIGPROF"}, {SIGWINCH, "SIGWINCH"},
		{SIGUSR1, "SIGUSR1"}, {SIGUSR2, "SIGUSR2"},
	};

	for (int i = 0; i < lengthof(signals); i++)
		if (signals[i].code == signo)
			return signals[i].name;
	return "UNRECOGNIZED";
}

static void
pclose_without_stderr(int *pipes)
{
	close(pipes[EXEC_DATA_P]);
	close(pipes[EXEC_ERR_P]);
}
