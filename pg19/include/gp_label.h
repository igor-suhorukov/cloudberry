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
 * gp_label.h
 *	  The "gp" security label: where the port keeps what Cloudberry keeps in
 *	  columns of its own.
 *
 * Cloudberry adds columns to PostgreSQL's catalogs -- pg_class.relisivm,
 * pg_proc.proexeclocation and seventeen others -- which an extension cannot
 * do.  A security label can hold the same thing: it is transactional, it is
 * dropped with its object, and pg_dump writes it, so a labelled object still
 * round-trips.  Shared objects can be labelled too, which is what the role
 * and database attributes need.
 *
 * One provider, "gp", holds them all, and its label is a list of key=value
 * pairs; a key with no value is a flag.  Keys come from GP_LABEL_KEYS below,
 * so that a typo is rejected when the label is set rather than ignored when
 * it is read.
 *
 * Labels are readable by every user (pg_seclabels is a plain view), so
 * nothing secret goes in one.  Password history and storage credentials live
 * in revoked tables instead.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_LABEL_H
#define GP_LABEL_H

#include "postgres.h"

#include "catalog/objectaddress.h"

#define GP_LABEL_PROVIDER	"gp"

/*
 * The keys a "gp" label may carry.  X(key, takes_value, what it is for)
 *
 * Adding a key here is what makes it settable; the check hook refuses
 * anything else.
 */
#define GP_LABEL_KEYS(X) \
	X(incremental,     false, "this materialized view is maintained incrementally") \
	X(dynamic_schedule, true, "the schedule a dynamic table refreshes on") \
	X(distributed_by,  true,  "the distribution key of a table") \
	X(execute_on,      true,  "where a function may run") \
	X(data_access,     true,  "what a function does with SQL") \
	X(replicate_safe,  false, "this aggregate may run on a replicated slice") \
	X(directory_location, true, "where a directory table keeps its files") \
	X(profile,         true,  "the password profile a role is under") \
	X(storage_server,  true,  "the storage server a tablespace's files go through") \
	X(locked_until,    true,  "when a password profile stops locking a role out") \
	X(failed_logins,   true,  "how many times in a row a role has failed to log in") \
	X(partition_templates, true, "a partitioned table's SUBPARTITION TEMPLATEs, per level")

typedef enum GpLabelKey
{
#define X(key, takes_value, doc)	GP_LABEL_##key,
	GP_LABEL_KEYS(X)
#undef X
	GP_LABEL_NKEYS
} GpLabelKey;

/*
 * Read one key of an object's "gp" label.  Returns NULL when the object has
 * no label or the label has no such key; for a flag key the value is an empty
 * string when it is present.  The result is palloc'd in the current context.
 */
extern char *GpLabelGet(const ObjectAddress *object, GpLabelKey key);

/* Is a flag key present? */
extern bool GpLabelHas(const ObjectAddress *object, GpLabelKey key);

/*
 * Set or remove one key, leaving the object's other keys alone.  A NULL value
 * removes the key; an empty string sets a flag.
 */
extern void GpLabelSet(const ObjectAddress *object, GpLabelKey key,
					   const char *value);

/* Registered by gp_core during preload. */
extern void GpLabelRegisterProvider(void);

#endif							/* GP_LABEL_H */
