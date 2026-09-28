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
 * paxc_smgr.cc
 *
 * IDENTIFICATION
 *	  contrib/pax_storage/src/cpp/storage/paxc_smgr.cc
 *
 *
 * Ported to PostgreSQL 19, whose storage manager is md alone: a PAX
 * relation's storage is md's, and PAX's unlink, which removed the
 * relation's PAX directory with its files, follows O21's unlink event,
 * which smgrdounlinkall() raises once a relation's forks are gone.  Where
 * Cloudberry's then removed the relation's first segment at once, md keeps
 * it, as it keeps any relation's, until the next checkpoint: until then the
 * relfilenumber is not given again, and PAX's catalog rows, which follow a
 * relation's storage (modules/pax/pax_catalog.c), rely on it.
 *-------------------------------------------------------------------------
 */

#include "storage/paxc_smgr.h"

#include "comm/paxc_wrappers.h"
#include "storage/wal/paxc_wal.h"

#include <sys/stat.h>

static smgr_file_event_hook_type prev_smgr_file_event_hook = NULL;

namespace paxc {

// After the storage manager has removed a relation's files -- as the
// transaction that drops or truncates it commits, as the one that made it
// aborts, and as either is replayed -- its PAX directory goes, with what is
// in it: O21's unlink event, once for all of a relation's forks.  A relation
// with no PAX directory costs a stat().  Nothing here may fail the
// transaction, whose end is decided: what cannot be removed is a WARNING, or
// a LOG, and is left.
static void smgr_file_event_pax(RelFileLocatorBackend rnode,
                                ForkNumber forknum, SmgrFileEvent event) {
  struct stat st;
  char *path;

  if (prev_smgr_file_event_hook)
    prev_smgr_file_event_hook(rnode, forknum, event);

  if (event != SMGR_FILE_UNLINK) return;

  path = paxc::BuildPaxDirectoryPath(rnode.locator, rnode.backend);
  if (stat(path, &st) == 0)
    paxc::DeletePaxDirectoryPath(path, true);
  else if (errno != ENOENT)
    ereport(WARNING, (errcode_for_file_access(),
                      errmsg("could not stat directory \"%s\": %m", path)));
  pfree(path);

  // redo's unlink (DropRelationFiles()), the one the startup process makes
  if (InRecovery) paxc::XLogForgetRelation(rnode.locator);
}

void RegisterPaxSmgr() {
  prev_smgr_file_event_hook = smgr_file_event_hook;
  smgr_file_event_hook = smgr_file_event_pax;
}

}  // namespace paxc
