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
 * resgroup.c
 *	  Resource groups at run time.
 *
 * Cloudberry sources this file is made of:
 *	  src/backend/utils/resgroup/resgroup.c, resgroup_helper.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/transam.h"
#include "catalog/catalog.h"
#include "nodes/bitmapset.h"
#include "utils/builtins.h"

#include "gp_resource.h"

/* Is the cpuset controller there to give a group cores of its own? */
bool		gp_resource_group_enable_cgroup_cpuset = false;

Size
ResGroupShmemSize(void)
{
	return 0;
}

void
ResGroupShmemRequest(void)
{
}

void
ResGroupShmemInit(void)
{
}

void
ResGroupInit(void)
{
}

void
ResGroupDefsChanged(void)
{
}

bool
ResGroupIsAssigned(void)
{
	return false;
}

/*
 * CpusetToBitset(): "1,3-5" is cores 1, 3, 4 and 5; NULL where the text is
 * not a cpuset.
 */
Bitmapset *
ResGroupCpusetToBitset(const char *cpuset, int len)
{
	int			pos = 0;
	int			num1 = 0;
	int			num2 = 0;
	enum
	{
		Initial,
		Begin,
		Number,
		Interval,
		Number2
	}			s = Initial;
	Bitmapset  *bms = NULL;

	if (cpuset == NULL || len <= 0)
		return NULL;
	while (pos < len && cpuset[pos])
	{
		char		c = cpuset[pos++];

		if (c == ',')
		{
			if (s == Initial || s == Begin)
				continue;
			else if (s == Interval)
				return NULL;
			else if (s == Number)
			{
				bms = bms_add_member(bms, num1);
				num1 = 0;
				s = Begin;
			}
			else if (s == Number2)
			{
				if (num1 > num2)
					return NULL;
				for (int i = num1; i <= num2; ++i)
					bms = bms_add_member(bms, i);
				num1 = num2 = 0;
				s = Begin;
			}
		}
		else if (c == '-')
		{
			if (s != Number)
				return NULL;
			s = Interval;
		}
		else if (isdigit((unsigned char) c))
		{
			if (s == Initial || s == Begin)
				s = Number;
			else if (s == Interval)
				s = Number2;
			if (s == Number)
				num1 = num1 * 10 + (c - '0');
			else
				num2 = num2 * 10 + (c - '0');
		}
		else if (c == '\n')
			break;
		else
			return NULL;
	}
	if (s == Number)
		bms = bms_add_member(bms, num1);
	else if (s == Number2)
	{
		if (num1 > num2)
			return NULL;
		for (int i = num1; i <= num2; ++i)
			bms = bms_add_member(bms, i);
	}
	else if (s == Interval)
		return NULL;
	return bms;
}

bool
ResGroupCpusetIsValid(const char *cpuset)
{
	return ResGroupCpusetToBitset(cpuset, strlen(cpuset)) != NULL;
}

/* EnsureCpusetIsAvailable(ERROR) */
void
ResGroupEnsureCpusetIsAvailable(void)
{
	if (!IsResGroupEnabled())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("resource group must be enabled to use cpuset feature")));
	if (!gp_resource_group_enable_cgroup_cpuset)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cgroup is not properly configured to use the cpuset feature"),
				 errhint("Extra cgroup configurations are required to enable this feature, "
						 "please refer to the Cloudberry Documentations for details")));
}

/* An io_limit, as the group's definition keeps it */
char *
ResGroupNormalizeIoLimit(const char *io_limit)
{
	if (!IsResGroupEnabled())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("resource group must be enabled to use io limit feature")));
	return pstrdup(io_limit);
}

Oid
ResGroupNewOid(void)
{
	return GetNewObjectId();
}

void
ResGroupValidate(List *defs, ResGroupDef *def)
{
}

void
ResGroupCreated(ResGroupDef *def)
{
}

void
ResGroupAltered(ResGroupDef *old, ResGroupDef *def, ResGroupLimitType type)
{
}

void
ResGroupDropped(Oid groupid)
{
}

void
ResGroupCheckDrop(Oid groupid, const char *name)
{
}
