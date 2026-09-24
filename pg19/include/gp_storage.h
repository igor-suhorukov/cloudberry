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
 * gp_storage.h
 *	  Storage handlers: how a directory table's files reach a storage server.
 *
 * A tablespace names a storage server (WITH (gp.server = ...)), and the
 * server's "protocol" option names the handler that reaches it, where
 * Cloudberry's pg_tablespace names a library and a function
 * (spcfilehandlerbin, spcfilehandlersrc) that give its FileAm.  A handler is
 * a module of its own, which registers as it is loaded -- before gp_sql or
 * after it, through a rendezvous variable -- and which gp_sql hands, with
 * each file, the server's options and the calling user's mapping for it:
 * the credentials, which it reads from gp.maintenance_database, on the
 * coordinator or on a segment (gp_sql's storage.c).
 *
 * The methods are what a directory table does with a file -- write one
 * whole, read one whole, remove one, remove a table's directory -- which is
 * Cloudberry's FileAm (storage/ufile.h) as its directory tables use it.  A
 * method may raise; the removals gp_sql runs as a transaction ends it turns
 * into warnings.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_STORAGE_H
#define GP_STORAGE_H

#include "postgres.h"

#include "fmgr.h"
#include "nodes/pg_list.h"
#include "utils/memutils.h"

/* A file of a storage server, and what reaching it takes. */
typedef struct GpStorageFile
{
	const char *server;			/* the storage server */
	List	   *server_options;	/* DefElem: its options */
	List	   *user_options;	/* DefElem: the calling user's mapping's, or
								 * PUBLIC's; NIL when there is none to read */
	const char *path;			/* relative to what the server reaches */
} GpStorageFile;

typedef struct GpStorageHandler
{
	const char *protocol;		/* the servers' "protocol" option it serves */

	/* Write a new file whole; one that is there already is an error. */
	void		(*write_file) (const GpStorageFile *file, const char *data,
							   Size len);
	/* A file's content, or NULL when there is no such file. */
	bytea	   *(*read_file) (const GpStorageFile *file);
	/* Remove a file; false when there was none. */
	bool		(*remove_file) (const GpStorageFile *file);
	/* Remove a directory and everything in it: a dropped directory table's. */
	void		(*remove_directory) (const GpStorageFile *file);
} GpStorageHandler;

/* Where the registered handlers are: a List of GpStorageHandler *. */
#define GP_STORAGE_RENDEZVOUS	"gp_sql storage handlers"

/*
 * Register a handler, from a module's _PG_init.  The handler lives as long as
 * the module, and a protocol served twice is served by the first.
 */
static inline void
GpStorageRegisterHandler(const GpStorageHandler *handler)
{
	List	  **handlers = (List **) find_rendezvous_variable(GP_STORAGE_RENDEZVOUS);
	MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);

	*handlers = lappend(*handlers, (void *) handler);
	MemoryContextSwitchTo(oldcxt);
}

/*
 * gp_sql's: the options of a storage server, and the calling user's mapping
 * for it -- its own, or else PUBLIC's -- as pg_user_mappings shows them to
 * that user, which is its own with USAGE on the server.  Read in
 * gp.maintenance_database, the coordinator's from a segment.  A server that
 * is not there is an error; a mapping that is not, NIL.
 */
extern List *GpStorageServerOptions(const char *server);
extern List *GpStorageUserOptions(const char *server);

/* The value of an option of such a list, or NULL. */
extern const char *GpStorageOption(List *options, const char *name);

#endif							/* GP_STORAGE_H */
