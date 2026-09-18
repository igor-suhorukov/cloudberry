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
 * gp_label.c
 *	  The "gp" security label provider.
 *
 * See gp_label.h for why the port keeps things here that Cloudberry keeps in
 * catalog columns of its own.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/seclabel.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"

#include "gp_label.h"

static const struct
{
	const char *name;
	bool		takes_value;
}			gp_label_key[GP_LABEL_NKEYS] = {
#define X(key, takes_val, doc)	{#key, takes_val},
	GP_LABEL_KEYS(X)
#undef X
};

/*
 * Parse a label into its keys.  Returns a list of "key=value" strings in
 * order, so that setting one key can put the others back unchanged.
 */
static List *
gp_label_parse(const char *label)
{
	List	   *items = NIL;
	const char *p = label;

	while (p && *p)
	{
		const char *comma = strchr(p, ',');
		int			len = comma ? (int) (comma - p) : (int) strlen(p);
		char	   *item;

		while (len > 0 && (*p == ' ' || *p == '\t'))
		{
			p++;
			len--;
		}
		while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t'))
			len--;

		if (len > 0)
		{
			item = palloc(len + 1);
			memcpy(item, p, len);
			item[len] = '\0';
			items = lappend(items, item);
		}

		if (!comma)
			break;
		p = comma + 1;
	}

	return items;
}

/* The key an item names, or -1 if it names none of ours. */
static int
gp_label_item_key(const char *item, const char **value)
{
	const char *eq = strchr(item, '=');
	int			namelen = eq ? (int) (eq - item) : (int) strlen(item);

	for (int i = 0; i < GP_LABEL_NKEYS; i++)
	{
		if (namelen == (int) strlen(gp_label_key[i].name) &&
			strncmp(item, gp_label_key[i].name, namelen) == 0)
		{
			if (value)
				*value = eq ? eq + 1 : "";
			return i;
		}
	}

	return -1;
}

/*
 * Refuse a label the port would not understand, when it is set rather than
 * when it is read: a key we do not know is far more likely to be a typo than
 * a message from the future.
 */
static void
gp_label_check(const ObjectAddress *object, const char *seclabel)
{
	List	   *items;
	ListCell   *lc;

	if (seclabel == NULL)		/* removing the label is always fine */
		return;

	items = gp_label_parse(seclabel);
	if (items == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" security label is empty", GP_LABEL_PROVIDER),
				 errhint("A label is a comma-separated list of key=value pairs.")));

	foreach(lc, items)
	{
		const char *item = (const char *) lfirst(lc);
		const char *value = NULL;
		int			key = gp_label_item_key(item, &value);

		if (key < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized \"%s\" security label key in \"%s\"",
							GP_LABEL_PROVIDER, item)));

		if (!gp_label_key[key].takes_value && value[0] != '\0')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("\"%s\" takes no value", gp_label_key[key].name)));

		if (gp_label_key[key].takes_value && value[0] == '\0')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("\"%s\" needs a value", gp_label_key[key].name)));
	}
}

char *
GpLabelGet(const ObjectAddress *object, GpLabelKey key)
{
	char	   *label = GetSecurityLabel(object, GP_LABEL_PROVIDER);
	ListCell   *lc;

	if (label == NULL)
		return NULL;

	foreach(lc, gp_label_parse(label))
	{
		const char *item = (const char *) lfirst(lc);
		const char *value = NULL;

		if (gp_label_item_key(item, &value) == (int) key)
			return pstrdup(value);
	}

	return NULL;
}

bool
GpLabelHas(const ObjectAddress *object, GpLabelKey key)
{
	return GpLabelGet(object, key) != NULL;
}

void
GpLabelSet(const ObjectAddress *object, GpLabelKey key, const char *value)
{
	char	   *label = GetSecurityLabel(object, GP_LABEL_PROVIDER);
	StringInfoData buf;
	ListCell   *lc;

	initStringInfo(&buf);

	/* Everything but the key we are setting, in the order it was in. */
	if (label != NULL)
	{
		foreach(lc, gp_label_parse(label))
		{
			const char *item = (const char *) lfirst(lc);

			if (gp_label_item_key(item, NULL) == (int) key)
				continue;
			if (buf.len > 0)
				appendStringInfoChar(&buf, ',');
			appendStringInfoString(&buf, item);
		}
	}

	if (value != NULL)
	{
		if (buf.len > 0)
			appendStringInfoChar(&buf, ',');
		appendStringInfoString(&buf, gp_label_key[key].name);
		if (value[0] != '\0')
			appendStringInfo(&buf, "=%s", value);
	}

	/* An object with nothing left to say loses its label entirely. */
	SetSecurityLabel(object, GP_LABEL_PROVIDER, buf.len > 0 ? buf.data : NULL);
	pfree(buf.data);
}

void
GpLabelRegisterProvider(void)
{
	register_label_provider(GP_LABEL_PROVIDER, gp_label_check);
}
