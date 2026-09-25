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
 * external.c
 *	  What an external table's foreign-table options say, and which of its
 *	  locations each segment reads.
 *
 * The locations are handed out to the segments as Cloudberry's planner hands
 * them out (create_external_scan_uri_list()): a file:// or http:// URI to one
 * primary each, a file on the host the URI names; gpfdist's URIs over every
 * primary, as many times over as it takes; a command to the segments the ON
 * clause names.  Cloudberry does it once, on the coordinator, and puts the
 * list in the plan.  Here a segment may plan its own scan -- the planner's
 * gather sends it the query -- so the choice is made so that any node makes
 * the same one: from the cluster's configuration, which every node reads
 * alike, and where Cloudberry picks segments at random, from a generator
 * seeded with the table and the statement, which the segments share.
 *
 * Cloudberry sources this file is made of:
 *	  src/backend/access/external/external.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "commands/defrem.h"
#include "common/hashfn.h"
#include "common/pg_prng.h"
#include "foreign/foreign.h"
#include "mb/pg_wchar.h"
#include "nodes/makefuncs.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_exttable.h"

/* ------------------------------------------------------------------------- */
/* fstream's allocator and messages, which the backend's are                 */
/* ------------------------------------------------------------------------- */

void		gfile_printf_then_putc_newline(const char *format,...) pg_attribute_printf(1, 2);
void	   *gfile_malloc(size_t size);
void		gfile_free(void *a);

void
gfile_printf_then_putc_newline(const char *format,...)
{
	char	   *a;
	va_list		va;
	int			i;

	va_start(va, format);
	i = vsnprintf(0, 0, format, va);
	va_end(va);

	if (i < 0)
		elog(NOTICE, "gfile_printf_then_putc_newline vsnprintf failed.");
	else
	{
		a = palloc(i + 1);
		va_start(va, format);
		vsnprintf(a, i + 1, format, va);
		va_end(va);
		elog(NOTICE, "%s", a);
		pfree(a);
	}
}

void *
gfile_malloc(size_t size)
{
	return palloc(size);
}

void
gfile_free(void *a)
{
	pfree(a);
}

/* ------------------------------------------------------------------------- */
/* The catalog                                                               */
/* ------------------------------------------------------------------------- */

/* Is relid a foreign table of gp_exttable_server? */
bool
rel_is_external_table(Oid relid)
{
	HeapTuple	tuple;
	Oid			server;
	ForeignServer *fs;
	bool		result;

	if (get_rel_relkind(relid) != RELKIND_FOREIGN_TABLE)
		return false;

	tuple = SearchSysCache1(FOREIGNTABLEREL, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		return false;
	server = ((Form_pg_foreign_table) GETSTRUCT(tuple))->ftserver;
	ReleaseSysCache(tuple);

	fs = GetForeignServer(server);
	result = strcmp(fs->servername, GP_EXTTABLE_SERVER_NAME) == 0;
	return result;
}

/*
 * Split the locations string, which separates URIs with | and escapes | and
 * \ in them with \.
 */
static char *
strsep_uri(char **uris)
{
	char	   *index;
	char	   *result;
	size_t		len;
	int			j = 0;

	if ((index = *uris) == NULL)
		return NULL;
	if (*index == '\0')
		return NULL;

	len = strlen(index);
	result = (char *) palloc(len + 1);
	for (;;)
	{
		if (*index == '\0')
		{
			result[j++] = '\0';
			*uris = index;
			break;
		}
		else if (*index == '\\')
		{
			index++;
			if (*index == '\\' || *index == '|')
				result[j++] = *index;
			else if (*index == '\0')
			{
				/* a version before the escapes, whose \ was the last */
				result[j++] = '\\';
				continue;
			}
			else
			{
				result[j++] = '\\';
				result[j++] = *index;
			}
			index++;
		}
		else if (*index == '|')
		{
			index++;
			result[j++] = '\0';
			*uris = index;
			break;
		}
		else
		{
			result[j++] = *index;
			index++;
		}
	}
	return result;
}

List *
TokenizeLocationUris(char *uris)
{
	char	   *uri;
	List	   *result = NIL;

	Assert(uris != NULL);

	while ((uri = strsep_uri(&uris)) != NULL)
		result = lappend(result, makeString(uri));

	return result;
}

ExtTableEntry *
GetExtTableEntry(Oid relid)
{
	ExtTableEntry *extentry;

	extentry = GetExtTableEntryIfExists(relid);
	if (!extentry)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("missing pg_foreign_table entry for relation \"%s\"",
						get_rel_name(relid))));
	return extentry;
}

ExtTableEntry *
GetExtTableEntryIfExists(Oid relid)
{
	ForeignTable *ft;
	ExtTableEntry *extentry;

	if (!rel_is_external_table(relid))
		return NULL;

	ft = GetForeignTable(relid);
	extentry = GetExtFromForeignTableOptions(ft->options, relid);
	pfree(ft);

	return extentry;
}

ExtTableEntry *
GetExtFromForeignTableOptions(List *ftoptions, Oid relid)
{
	ExtTableEntry *extentry;
	ListCell   *lc;
	List	   *entryOptions = NIL;
	char	   *arg;
	bool		rejectlimit_found = false;
	bool		rejectlimittype_found = false;
	bool		logerrors_found = false;
	bool		encoding_found = false;
	bool		iswritable_found = false;
	bool		executeon_found = false;

	extentry = (ExtTableEntry *) palloc0(sizeof(ExtTableEntry));

	foreach(lc, ftoptions)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (pg_strcasecmp(def->defname, "location_uris") == 0)
		{
			extentry->urilocations = TokenizeLocationUris(defGetString(def));
			continue;
		}
		if (pg_strcasecmp(def->defname, "execute_on") == 0)
		{
			extentry->execlocations = list_make1(makeString(defGetString(def)));
			executeon_found = true;
			continue;
		}
		if (pg_strcasecmp(def->defname, "command") == 0)
		{
			extentry->command = defGetString(def);
			continue;
		}
		if (pg_strcasecmp(def->defname, "format_type") == 0)
		{
			arg = defGetString(def);
			extentry->fmtcode = arg[0];
			continue;
		}
		/* only CSV needs this, for ProcessCopyOptions(); added below */
		if (pg_strcasecmp(def->defname, "format") == 0)
			continue;
		if (pg_strcasecmp(def->defname, "reject_limit") == 0)
		{
			extentry->rejectlimit = atoi(defGetString(def));
			rejectlimit_found = true;
			continue;
		}
		if (pg_strcasecmp(def->defname, "reject_limit_type") == 0)
		{
			arg = defGetString(def);
			extentry->rejectlimittype = arg[0];
			rejectlimittype_found = true;
			continue;
		}
		if (pg_strcasecmp(def->defname, "log_errors") == 0)
		{
			arg = defGetString(def);
			extentry->logerrors = arg[0];
			logerrors_found = true;
			continue;
		}
		if (pg_strcasecmp(def->defname, "encoding") == 0)
		{
			extentry->encoding = atoi(defGetString(def));
			encoding_found = true;
			continue;
		}
		if (pg_strcasecmp(def->defname, "is_writable") == 0)
		{
			extentry->iswritable = defGetBoolean(def);
			iswritable_found = true;
			continue;
		}
		/* the port's own, which says where the rows go: no COPY option */
		if (def->defnamespace != NULL || strchr(def->defname, '.') != NULL)
			continue;

		entryOptions = lappend(entryOptions,
							   makeDefElem(def->defname,
										   (Node *) makeString(pstrdup(defGetString(def))), -1));
	}

	if (fmttype_is_csv(extentry->fmtcode))
		entryOptions = lappend(entryOptions,
							   makeDefElem("format", (Node *) makeString("csv"), -1));

	if (!executeon_found)
		extentry->execlocations = list_make1(makeString("ALL_SEGMENTS"));
	if (!iswritable_found)
		extentry->iswritable = false;
	if (!encoding_found)
		extentry->encoding = GetDatabaseEncoding();
	if (!logerrors_found)
		extentry->logerrors = LOG_ERRORS_DISABLE;
	if (!rejectlimit_found)
		extentry->rejectlimit = -1;
	if (!rejectlimittype_found)
		extentry->rejectlimittype = -1;

	extentry->options = entryOptions;

	return extentry;
}

/* ------------------------------------------------------------------------- */
/* Which segment reads what                                                  */
/* ------------------------------------------------------------------------- */

/*
 * Is a segment on the host a URI or an ON HOST clause names?  A segment of
 * the port's test clusters has the directory of its socket for a host name,
 * and is on this machine: localhost, 127.0.0.1 or its own name.
 */
static bool
segment_on_host(const GpSegmentConfig *seg, const char *host)
{
	char		myname[256];

	if (pg_strcasecmp(seg->hostname, host) == 0)
		return true;
	if (seg->hostname[0] != '/')
		return false;
	if (pg_strcasecmp(host, "localhost") == 0 || strcmp(host, "127.0.0.1") == 0 ||
		strcmp(host, "::1") == 0)
		return true;
	if (gethostname(myname, sizeof(myname)) == 0)
	{
		myname[sizeof(myname) - 1] = '\0';
		if (pg_strcasecmp(myname, host) == 0)
			return true;
	}
	return false;
}

/* The host a segment is on, as ON HOST compares and PER_HOST counts. */
static const char *
segment_host(const GpSegmentConfig *seg)
{
	return seg->hostname[0] == '/' ? "localhost" : seg->hostname;
}

/*
 * Cloudberry's makeRandomSegMap(): which of total segments to skip, total_to_skip
 * of them, at random.  The generator is seeded with the table and with the
 * statement, which every segment knows the same: see the file's comment.
 */
static bool *
make_random_seg_map(int total, int total_to_skip, uint64 seed)
{
	bool	   *skip = palloc0(sizeof(bool) * total);
	pg_prng_state prng;
	int			skipped = 0;

	pg_prng_seed(&prng, seed);
	while (skipped < total_to_skip)
	{
		int			i = (int) pg_prng_uint64_range(&prng, 0, total - 1);

		if (!skip[i])
		{
			skip[i] = true;
			skipped++;
		}
	}
	return skip;
}

/* The primaries, as content ids 0..n-1 in order. */
static const GpSegmentConfig **
primaries(int *n)
{
	int			nsegs;
	const GpSegmentConfig *segs = GpClusterSegments(&nsegs);
	int			count = GpClusterSegmentCount();
	const GpSegmentConfig **result = palloc0(sizeof(GpSegmentConfig *) * Max(count, 1));

	for (int i = 0; i < nsegs; i++)
	{
		const GpSegmentConfig *s = &segs[i];

		if (s->content >= 0 && s->content < count && s->role == 'p')
			result[s->content] = s;
	}
	*n = count;
	return result;
}

extern PGDLLIMPORT const char *debug_query_string;

static List *
create_external_scan_uri_list(ExtTableEntry *ext, Oid relid, bool *ismasteronly)
{
	ListCell   *c;
	List	   *modifiedloclist = NIL;
	int			i;
	int			total_primaries;
	const GpSegmentConfig **prim;
	char	  **segdb_file_map;
	bool		using_execute;
	bool		using_location;
	bool		found_candidate = false;
	bool		found_match = false;
	bool		done = false;
	List	   *filenames;
	int			total_to_skip = 0;
	int			max_participants_allowed = 0;
	int			num_segs_participating = 0;
	bool	   *skip_map = NULL;
	bool		should_skip_randomly = false;
	Uri		   *uri;
	char	   *on_clause;
	uint64		seed = ((uint64) relid << 32) ^
		(debug_query_string ? hash_bytes((const unsigned char *) debug_query_string,
										 strlen(debug_query_string)) : 0);
	bool		single = GpClusterIsSingleNode();

	*ismasteronly = false;

	using_execute = (ext->command != NULL);
	using_location = !using_execute;

	if (ext->command && !gp_external_enable_exec)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("using external tables with OS level commands (EXECUTE clause) is disabled"),
				 errhint("To enable set gp_external_enable_exec=on.")));

	if (ext->iswritable)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot read from a WRITABLE external table"),
				 errhint("Create the table as READABLE instead.")));

	if (!using_execute)
		uri = ParseExternalTableUri(strVal(linitial(ext->urilocations)));
	else
		uri = NULL;

	on_clause = (char *) strVal(linitial(ext->execlocations));
	if (strcmp(on_clause, "COORDINATOR_ONLY") == 0 && using_location &&
		uri->protocol != URI_CUSTOM && uri->protocol != URI_FILE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("'ON COORDINATOR' is not supported by this protocol yet")));

	/* On one node, the node is the one primary there is. */
	if (single)
	{
		total_primaries = 1;
		prim = NULL;
	}
	else
		prim = primaries(&total_primaries);

	segdb_file_map = (char **) palloc0(Max(total_primaries, 1) * sizeof(char *));

	/* (1) file:// and http://: one URI to one primary each */
	if (using_location && (uri->protocol == URI_FILE || uri->protocol == URI_HTTP))
	{
		foreach(c, ext->urilocations)
		{
			const char *uri_str = (char *) strVal(lfirst(c));

			uri = ParseExternalTableUri(uri_str);

			found_candidate = false;
			found_match = false;

			if (strcmp(on_clause, "COORDINATOR_ONLY") == 0 && uri->protocol == URI_FILE)
			{
				found_match = true;
				segdb_file_map[0] = pstrdup(uri_str);
				*ismasteronly = true;
			}

			for (i = 0; prim != NULL && i < total_primaries && !found_match; i++)
			{
				const GpSegmentConfig *p = prim[i];

				if (p == NULL || p->status != 'u')
					continue;
				if (uri->protocol == URI_FILE && !segment_on_host(p, uri->hostname))
					continue;

				found_candidate = true;
				if (segdb_file_map[i] == NULL)
				{
					segdb_file_map[i] = pstrdup(uri_str);
					found_match = true;
				}
			}

			/* On one node, the one location it can handle. */
			if (single && segdb_file_map[0] == NULL)
			{
				segdb_file_map[0] = pstrdup(uri_str);
				found_match = true;
			}

			if (!found_match)
			{
				if (uri->protocol == URI_FILE)
				{
					if (found_candidate)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
								 errmsg("could not assign a segment database for \"%s\"",
										uri_str),
								 errdetail("There are more external files than primary segment databases on host \"%s\"",
										   uri->hostname)));
					else
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
								 errmsg("could not assign a segment database for \"%s\"",
										uri_str),
								 errdetail("There isn't a valid primary segment database on host \"%s\"",
										   uri->hostname)));
				}
				else
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("could not assign a segment database for \"%s\"",
									uri_str),
							 errdetail("There are more URIs than total primary segment databases")));
			}
		}
	}
	/* (2) gpfdist(s):// and a custom protocol: over every primary */
	else if (using_location && (uri->protocol == URI_GPFDIST ||
								uri->protocol == URI_GPFDISTS ||
								uri->protocol == URI_CUSTOM))
	{
		if ((strcmp(on_clause, "COORDINATOR_ONLY") == 0 || single) &&
			uri->protocol == URI_CUSTOM)
		{
			segdb_file_map[0] = pstrdup(strVal(linitial(ext->urilocations)));
			*ismasteronly = true;
		}
		else
		{
			if (single)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("gpfdist is not supported in single node mode")));

			num_segs_participating = total_primaries;
			if (uri->protocol == URI_GPFDIST || uri->protocol == URI_GPFDISTS)
				max_participants_allowed = list_length(ext->urilocations) *
					gp_external_max_segs;
			else
				max_participants_allowed = num_segs_participating;

			if (num_segs_participating > max_participants_allowed)
			{
				total_to_skip = num_segs_participating - max_participants_allowed;
				num_segs_participating = max_participants_allowed;
				should_skip_randomly = true;

				elog(NOTICE, "External scan %s will utilize %d out "
					 "of %d segment databases",
					 (uri->protocol == URI_GPFDIST ? "from gpfdist(s) server" : "using custom protocol"),
					 num_segs_participating,
					 total_primaries);
			}

			if (list_length(ext->urilocations) > num_segs_participating)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("there are more external files (URLs) than primary segments that can read them"),
						 errdetail("Found %d URLs and %d primary segments.",
								   list_length(ext->urilocations),
								   num_segs_participating)));

			while (!done)
			{
				foreach(c, ext->urilocations)
				{
					char	   *uri_str = (char *) strVal(lfirst(c));

					modifiedloclist = lappend(modifiedloclist, makeString(pstrdup(uri_str)));
					if (list_length(modifiedloclist) == num_segs_participating)
					{
						done = true;
						break;
					}
				}
			}

			if (should_skip_randomly)
				skip_map = make_random_seg_map(total_primaries, total_to_skip, seed);

			foreach(c, modifiedloclist)
			{
				const char *uri_str = strVal(lfirst(c));

				found_match = false;
				for (i = 0; i < total_primaries && !found_match; i++)
				{
					const GpSegmentConfig *p = prim[i];

					if (p == NULL || p->status != 'u')
						continue;
					if (should_skip_randomly && skip_map[i])
						continue;
					if (segdb_file_map[i] == NULL)
					{
						segdb_file_map[i] = pstrdup(uri_str);
						found_match = true;
					}
				}

				if (!found_match)
					elog(ERROR,
						 "internal error in createplan for external tables when trying to assign segments for gpfdist(s)");
			}
		}
	}
	/* (3) EXECUTE: the segments the ON clause names */
	else if (using_execute)
	{
		char	   *prefixed_command = psprintf("%s%s", EXEC_URL_PREFIX, ext->command);

		if (single && strcmp(on_clause, "COORDINATOR_ONLY") != 0)
		{
			/* one node runs it: it is every segment there is */
			segdb_file_map[0] = prefixed_command;
		}
		else if (strcmp(on_clause, "ALL_SEGMENTS") == 0)
		{
			for (i = 0; i < total_primaries; i++)
				if (prim[i] != NULL && prim[i]->status == 'u')
					segdb_file_map[i] = pstrdup(prefixed_command);
		}
		else if (strcmp(on_clause, "PER_HOST") == 0)
		{
			List	   *visited_hosts = NIL;

			for (i = 0; i < total_primaries; i++)
			{
				const GpSegmentConfig *p = prim[i];
				bool		host_taken = false;

				if (p == NULL || p->status != 'u')
					continue;
				foreach(c, visited_hosts)
					if (pg_strcasecmp(strVal(lfirst(c)), segment_host(p)) == 0)
						host_taken = true;
				if (!host_taken)
				{
					segdb_file_map[i] = pstrdup(prefixed_command);
					visited_hosts = lappend(visited_hosts,
											makeString(pstrdup(segment_host(p))));
				}
			}
		}
		else if (strncmp(on_clause, "HOST:", strlen("HOST:")) == 0)
		{
			char	   *hostname = on_clause + strlen("HOST:");
			bool		match_found = false;

			for (i = 0; i < total_primaries; i++)
			{
				const GpSegmentConfig *p = prim[i];

				if (p != NULL && p->status == 'u' && segment_on_host(p, hostname))
				{
					segdb_file_map[i] = pstrdup(prefixed_command);
					match_found = true;
				}
			}

			if (!match_found)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("could not assign a segment database for command \"%s\")",
								ext->command),
						 errdetail("No valid primary segment was found in the requested host name \"%s\".",
								   hostname)));
		}
		else if (strncmp(on_clause, "SEGMENT_ID:", strlen("SEGMENT_ID:")) == 0)
		{
			int			target_segid = atoi(on_clause + strlen("SEGMENT_ID:"));

			if (target_segid < 0 || target_segid >= total_primaries ||
				prim[target_segid] == NULL || prim[target_segid]->status != 'u')
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("could not assign a segment database for command \"%s\"",
								ext->command),
						 errdetail("The requested segment id %d is not a valid primary segment or doesn't exist in the database",
								   target_segid)));
			segdb_file_map[target_segid] = pstrdup(prefixed_command);
		}
		else if (strncmp(on_clause, "TOTAL_SEGS:", strlen("TOTAL_SEGS:")) == 0)
		{
			int			num_segs_to_use = atoi(on_clause + strlen("TOTAL_SEGS:"));

			if (num_segs_to_use > total_primaries)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("table defined with EXECUTE ON %d but there are only %d valid primary segments in the database",
								num_segs_to_use, total_primaries)));

			total_to_skip = total_primaries - num_segs_to_use;
			skip_map = make_random_seg_map(total_primaries, total_to_skip, seed);

			for (i = 0; i < total_primaries; i++)
			{
				if (prim[i] == NULL || prim[i]->status != 'u' || skip_map[i])
					continue;
				segdb_file_map[i] = pstrdup(prefixed_command);
			}
		}
		else if (strcmp(on_clause, "COORDINATOR_ONLY") == 0)
		{
			segdb_file_map[0] = pstrdup(prefixed_command);
			*ismasteronly = true;
		}
		else
			elog(ERROR, "Internal error in createplan for external tables: got invalid ON clause code %s",
				 on_clause);
	}
	else
		elog(ERROR, "Internal error in createplan for external tables");

	filenames = NIL;
	for (i = 0; i < Max(total_primaries, 1); i++)
		filenames = lappend(filenames,
							makeString(segdb_file_map[i] != NULL ? segdb_file_map[i] : ""));

	return filenames;
}

ExternalScanInfo *
MakeExternalScanInfo(ExtTableEntry *extEntry, Oid relid)
{
	ExternalScanInfo *node = palloc0(sizeof(ExternalScanInfo));
	bool		ismasteronly = false;
	static uint32 scancounter = 0;

	if (extEntry->rejectlimit != -1)
		VerifyRejectLimit(extEntry->rejectlimittype, extEntry->rejectlimit);

	node->uriList = create_external_scan_uri_list(extEntry, relid, &ismasteronly);
	node->fmtType = extEntry->fmtcode;
	node->isMasterOnly = ismasteronly;
	node->rejLimit = -1;
	node->rejLimitInRows = false;
	node->logErrors = LOG_ERRORS_DISABLE;
	if (extEntry->rejectlimit != -1)
	{
		node->rejLimitInRows = (extEntry->rejectlimittype == 'r');
		node->rejLimit = extEntry->rejectlimit;
		node->logErrors = extEntry->logerrors;
	}
	node->encoding = extEntry->encoding;
	node->scancounter = scancounter++;
	node->extOptions = extEntry->options;

	return node;
}

List *
ExternalScanInfoToList(ExternalScanInfo *info)
{
	return list_make5(info->uriList,
					  list_make4(makeInteger(info->fmtType),
								 makeBoolean(info->isMasterOnly),
								 makeInteger(info->rejLimit),
								 makeBoolean(info->rejLimitInRows)),
					  list_make3(makeInteger(info->logErrors),
								 makeInteger(info->encoding),
								 makeInteger((int) info->scancounter)),
					  info->extOptions != NIL ? info->extOptions : NIL,
					  makeInteger(0));
}

ExternalScanInfo *
ExternalScanInfoFromList(List *list)
{
	ExternalScanInfo *info = palloc0(sizeof(ExternalScanInfo));
	List	   *a = (List *) list_nth(list, 1);
	List	   *b = (List *) list_nth(list, 2);

	info->uriList = (List *) linitial(list);
	info->fmtType = (char) intVal(linitial(a));
	info->isMasterOnly = boolVal(lsecond(a));
	info->rejLimit = intVal(lthird(a));
	info->rejLimitInRows = boolVal(lfourth(a));
	info->logErrors = (char) intVal(linitial(b));
	info->encoding = intVal(lsecond(b));
	info->scancounter = (uint32) intVal(lthird(b));
	info->extOptions = (List *) list_nth(list, 3);
	return info;
}
