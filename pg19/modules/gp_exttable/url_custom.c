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
 * url_custom.c
 *	  A location of a protocol of the user's: its read or write function,
 *	  called with the buffer to fill or to send.
 *
 * Cloudberry sources this file is made of:
 *	  src/backend/access/external/url_custom.c,
 *	  src/backend/catalog/pg_extprotocol.c's lookups
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/namespace.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/regproc.h"

#include "gp_exttable.h"

typedef struct URL_CUSTOM_FILE
{
	URL_FILE	common;
	FmgrInfo   *protocol_udf;
	ExtProtocolData *extprotocol;
	MemoryContext protcxt;
	Relation	rel;
} URL_CUSTOM_FILE;

/*
 * A protocol's function, as CREATE PROTOCOL recorded it in
 * gp_exttable.protocol: its reader, or with iswritable its writer.
 */
Oid
LookupExtProtocolFunction(const char *prot_name, bool iswritable, bool error)
{
	bool		exists;
	Oid			result = ExtProtocolFunction(prot_name, iswritable, &exists);

	if (!exists && error)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("protocol \"%s\" does not exist", prot_name)));
	if (!OidIsValid(result) && error)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("protocol '%s' has no %s function defined",
						prot_name, iswritable ? "write" : "read")));
	return result;
}

Oid
get_extprotocol_oid(const char *prot_name, bool missing_ok)
{
	return ExtProtocolOid(prot_name, missing_ok);
}

static int32
InvokeExtProtocol(void *ptr, size_t nbytes, URL_CUSTOM_FILE *file,
				  bool last_call)
{
	LOCAL_FCINFO(fcinfo, 0);
	ExtProtocolData *extprotocol = file->extprotocol;
	Datum		d;
	MemoryContext oldcontext;

	extprotocol->type = T_ExtProtocolData;
	extprotocol->prot_url = file->common.url;
	extprotocol->prot_relation = last_call ? NULL : file->rel;
	extprotocol->prot_databuf = last_call ? NULL : (char *) ptr;
	extprotocol->prot_maxbytes = nbytes;
	extprotocol->prot_last_call = last_call;

	InitFunctionCallInfoData(*fcinfo, file->protocol_udf, 0, InvalidOid,
							 (Node *) extprotocol, NULL);

	oldcontext = MemoryContextSwitchTo(file->protcxt);
	d = FunctionCallInvoke(fcinfo);
	MemoryContextSwitchTo(oldcontext);

	if (fcinfo->isnull)
		elog(ERROR, "function %u returned NULL", fcinfo->flinfo->fn_oid);

	return DatumGetInt32(d);
}

URL_FILE *
url_custom_fopen(char *url, bool forwrite, extvar_t *ev, ExternalSelectDesc desc,
				 Relation rel)
{
	URL_CUSTOM_FILE *file;
	MemoryContext oldcontext;
	Oid			procOid;
	char	   *prot_name;
	char	   *colon;

	file = palloc0(sizeof(URL_CUSTOM_FILE));
	file->common.type = CFTYPE_CUSTOM;
	file->common.url = pstrdup(url);

	/*
	 * The table read or written, which the function is called with, as
	 * Cloudberry calls it with its COPY's -- a writer's too, which gpcloud's
	 * s3_export() asks for its format.
	 */
	file->rel = rel;

	prot_name = pstrdup(url);
	colon = strchr(prot_name, ':');
	if (colon)
		*colon = '\0';
	procOid = LookupExtProtocolFunction(prot_name, forwrite, true);

	/*
	 * The user's function keeps what it wants in a context of its own, under
	 * the transaction's, deleted when the location is closed or with the
	 * transaction.
	 */
	file->protcxt = AllocSetContextCreate(TopTransactionContext,
										  "CustomProtocolMemCxt",
										  ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(file->protcxt);
	file->protocol_udf = palloc(sizeof(FmgrInfo));
	file->extprotocol = palloc0(sizeof(ExtProtocolData));
	fmgr_info(procOid, file->protocol_udf);
	MemoryContextSwitchTo(oldcontext);

	file->extprotocol->desc = desc;
	pfree(prot_name);

	return (URL_FILE *) file;
}

void
url_custom_fclose(URL_FILE *file, bool failOnError, const char *relname)
{
	URL_CUSTOM_FILE *cfile = (URL_CUSTOM_FILE *) file;

	if (cfile->protocol_udf)
		(void) InvokeExtProtocol(NULL, 0, cfile, true);
	MemoryContextDelete(cfile->protcxt);
	pfree(cfile);
}

bool
url_custom_feof(URL_FILE *file, int bytesread)
{
	return bytesread == 0;
}

bool
url_custom_ferror(URL_FILE *file, int bytesread, char *ebuf, int ebuflen)
{
	return bytesread == -1;
}

size_t
url_custom_fread(void *ptr, size_t size, URL_FILE *file, CopyFromState pstate)
{
	return (size_t) InvokeExtProtocol(ptr, size, (URL_CUSTOM_FILE *) file, false);
}

size_t
url_custom_fwrite(void *ptr, size_t size, URL_FILE *file)
{
	return (size_t) InvokeExtProtocol(ptr, size, (URL_CUSTOM_FILE *) file, false);
}
