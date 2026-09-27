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
 * proxy_backend.h
 *	  What the interconnect module's files share: the proxy transport, and
 *	  the packet size a node's proxy and its backends agree on.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROXY_BACKEND_H
#define PROXY_BACKEND_H

/* The proxy transport, registered (proxy_backend.c) */
extern void GpIcProxyInit(void);

/*
 * The most a packet between a backend and its proxy carries: the node's own
 * gp.max_packet_size, as the proxy was started with it, below what the
 * proxy's 16-bit packet length can say with its header.
 */
extern int	GpIcProxyPacketSize(void);

#endif							/* PROXY_BACKEND_H */
