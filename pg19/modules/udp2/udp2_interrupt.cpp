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
 * udp2_interrupt.cpp
 *	  UDP2's interrupt callback: an interrupt that raises an error, as a C++
 *	  exception.
 *
 * UDP2's core looks for interrupts through a callback as it waits for a
 * packet or an acknowledgement, from inside frames that hold its locks --
 * its receive waits under a std::unique_lock of its static mtx.  A
 * PostgreSQL error, a longjmp, would jump over those frames and leave the
 * lock held, and their other objects not destroyed.  So the callback asks
 * udp2.c whether an interrupt raises an error now -- which takes it, and
 * keeps its error -- and if so throws, as the core's own errors are thrown:
 * the core's frames unwind, their locks released, to its C interface, which
 * catches every exception and returns with its error set; udp2.c then
 * raises the interrupt's error.  Nothing is thrown while the core tears a
 * statement down, which must not fail.
 *
 * C++, and no PostgreSQL header: this is the core's side of the adapter.
 *
 * Cloudberry sources this file stands in for:
 *	  contrib/udp2/ic_udp2.c (CheckInterrupts(), ML_CHECK_FOR_INTERRUPTS)
 *
 *-------------------------------------------------------------------------
 */
#include <sstream>
#include <stdexcept>
#include <string>

#include "ic_except.hpp"

extern "C"
{
	/* udp2.c's */
	extern bool udp2_interrupted(void);

	void		udp2_check_interrupts(int teardownActive);
}

void
udp2_check_interrupts(int teardownActive)
{
	if (!teardownActive && udp2_interrupted())
		throw ICException("interrupted", __FILE__, __LINE__);
}
