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
 * ic_proxy/ic_internal.h
 *	  The include overlay of Cloudberry's interconnect proxy: in the place of
 *	  Cloudberry's contrib/interconnect/ic_internal.h, which its ic_proxy.h
 *	  includes for the ic-tcp and ic-udp structures its backend half uses,
 *	  and which its server asks only for CONTAINER_OF; see ./cdb/cdbvars.h.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_COMPAT_IC_PROXY_IC_INTERNAL_H
#define GP_COMPAT_IC_PROXY_IC_INTERNAL_H

/* the struct a member is of, from the member's address */
#define CONTAINER_OF(ptr, type, member) \
	({ \
		const typeof( ((type *)0)->member ) *__member_ptr = (ptr); \
		(type *)( (char *)__member_ptr - offsetof(type,member) ); \
	})

#endif							/* GP_COMPAT_IC_PROXY_IC_INTERNAL_H */
