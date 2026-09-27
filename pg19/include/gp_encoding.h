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
 * gp_encoding.h
 *	  A table access method of another module's that takes Cloudberry's
 *	  ENCODING clauses, which gp_ao keeps: its check of a column's options.
 *
 * Cloudberry's table access methods check a column's ENCODING clause through
 * callbacks of its tableam.h (validate_column_encoding_clauses and
 * transform_column_encoding_clauses), which PostgreSQL 19's has not.  The
 * port's gp_ao takes the clauses whatever the method, keeps each column's
 * options as a security label of its provider, on the column, and asks a
 * method registered here -- PAX -- to check them, as only the method knows
 * its encodings.  The method reads the labels as it writes.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_ENCODING_H
#define GP_ENCODING_H

#include "fmgr.h"
#include "nodes/pg_list.h"
#include "utils/memutils.h"

/*
 * A column's options as an ENCODING clause gives them, DefElems of Strings:
 * the check raises, in the method's words, where they are not its own.
 */
typedef void (*GpEncodingCheck) (List *opts);

typedef struct GpEncodingMethod
{
	const char *amname;			/* the table access method's name */
	GpEncodingCheck check;
} GpEncodingMethod;

/* Where the registered methods are: a List of GpEncodingMethod *. */
#define GP_ENCODING_RENDEZVOUS	"gp_ao encoding methods"

/*
 * Register a method, from a module's _PG_init.  It lives as long as the
 * module; a method registered twice is the first's.
 */
static inline void
GpEncodingRegisterMethod(const GpEncodingMethod *method)
{
	List	  **methods = (List **) find_rendezvous_variable(GP_ENCODING_RENDEZVOUS);
	MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);

	*methods = lappend(*methods, (void *) method);
	MemoryContextSwitchTo(oldcxt);
}

/* The method registered by this name, or NULL. */
static inline const GpEncodingMethod *
GpEncodingMethodOf(const char *amname)
{
	List	  **methods = (List **) find_rendezvous_variable(GP_ENCODING_RENDEZVOUS);
	ListCell   *lc;

	if (amname == NULL)
		return NULL;
	foreach(lc, *methods)
	{
		const GpEncodingMethod *method = (const GpEncodingMethod *) lfirst(lc);

		if (strcmp(method->amname, amname) == 0)
			return method;
	}
	return NULL;
}

#endif							/* GP_ENCODING_H */
