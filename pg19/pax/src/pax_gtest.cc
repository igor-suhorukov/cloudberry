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
 * pax_gtest.cc
 *
 * IDENTIFICATION
 *	  contrib/pax_storage/src/cpp/pax_gtest.cc
 *
 *
 * Ported to PostgreSQL 19: the tests run in a backend, where every function
 * of the server's they call is, rather than in a program linked with
 * Cloudberry's libpostgres.so, which PostgreSQL 19 does not make.  The
 * module pax_gtest has PAX compiled in and the tests, and its function
 * pax_gtest_run(filter) runs the tests the googletest filter names -- or
 * does what a flag of googletest's says, --gtest_list_tests -- and returns
 * how many failed; what googletest prints goes to the backend's
 * standard output, the terminal of the single-user backend the pax_gtest
 * suite runs it in (pg19/test/pax_gtest).  The mocks are Cloudberry's, the
 * directory's taking PostgreSQL 19's RelFileLocator, one of the backend's
 * own set-up that a test program does itself, and the port's layout of a
 * table's row IDs, which a test's relation has no catalog row of; they are
 * taken away
 * again as the run ends, as are the memory context and the resource owner
 * the tests leave current.
 *-------------------------------------------------------------------------
 */

#include <cstdio>
#include "comm/gtest_wrappers.h"

#include "access/paxc_rel_options.h"
#include "catalog/pax_catalog.h"
#include "comm/cbdb_wrappers.h"
#include "storage/oper/pax_stats.h"
#include "comm/guc.h"
#include "pax_tid.h"
#include "cpp-stub/src/stub.h"

bool MockMinMaxGetStrategyProcinfo(Oid, Oid, Oid *, FmgrInfo *,
                                   StrategyNumber) {
  return false;
}

int32 MockGetFastSequences(Oid) {
  static int32 mock_id = 0;
  return mock_id++;
}

void MockInsertMicroPartitionPlaceHolder(Oid, int) {
  // do nothing
}

void MockDeleteMicroPartitionEntry(Oid, Snapshot, int) {
  // do nothing
}

void MockExecStoreVirtualTuple(TupleTableSlot *) {
  // do nothing
}

// a backend's file access, which a test program sets up itself, is set up
// already (LocalFileSystemTest.CopyFile)
void MockInitFileAccess(void) {}

// A test's relation, which has no row in pax.pg_pax_tables -- the tests'
// database has not the extension -- lays its rows out as a table made now
// does (pax_tid.h): the port's, where Cloudberry's layout was fixed.
int MockPaxTableFileBits(Relation) {
  return PaxTidFileBitsFor(pax::pax_max_tuples_per_file);
}

void MockPaxTableCheckFileNumber(Relation, uint32) {}

std::string MockBuildPaxDirectoryPath(RelFileLocator, BackendId) {
  return std::string(".");
}

std::vector<int> MockGetMinMaxColumnIndexes(Relation rel) {
  return std::vector<int>();
}

std::vector<int> MockBloomFilterColumnIndexes(Relation rel) {
  return std::vector<int>();
}

std::vector<std::tuple<pax::ColumnEncoding_Kind, int>> MockGetRelEncodingOptions(Relation rel) {
  return std::vector<std::tuple<pax::ColumnEncoding_Kind, int>>();
}

// Mock global method which is not link from another libarays
void GlobalMock(Stub *stub) {
  stub->set(pax::MinMaxGetPgStrategyProcinfo, MockMinMaxGetStrategyProcinfo);
  stub->set(CPaxGetFastSequences, MockGetFastSequences);
  stub->set(cbdb::BuildPaxDirectoryPath, MockBuildPaxDirectoryPath);
  stub->set(cbdb::InsertMicroPartitionPlaceHolder,
            MockInsertMicroPartitionPlaceHolder);
  stub->set(cbdb::DeleteMicroPartitionEntry, MockDeleteMicroPartitionEntry);
  stub->set(cbdb::GetMinMaxColumnIndexes, MockGetMinMaxColumnIndexes);
  stub->set(cbdb::GetBloomFilterColumnIndexes, MockBloomFilterColumnIndexes);
  stub->set(cbdb::GetRelEncodingOptions, MockGetRelEncodingOptions);
  stub->set(ExecStoreVirtualTuple, MockExecStoreVirtualTuple);
  stub->set(InitFileAccess, MockInitFileAccess);
  stub->set(paxc::PaxTableFileBits, MockPaxTableFileBits);
  stub->set(cbdb::PaxTableFileBits, MockPaxTableFileBits);
  stub->set(paxc::PaxTableCheckFileNumber, MockPaxTableCheckFileNumber);
  stub->set(cbdb::PaxTableCheckFileNumber, MockPaxTableCheckFileNumber);
}

extern "C" {
PG_MODULE_MAGIC;

// pax.c's, which the module has not: its scans filter their rows by the
// quals, as a PAX table's do by default
bool gp_enable_predicate_pushdown = true;

PG_FUNCTION_INFO_V1(pax_gtest_run);
}

// A googletest filter, or one of its flags, --gtest_list_tests among them.
extern "C" Datum pax_gtest_run(PG_FUNCTION_ARGS) {
  std::string arg = PG_ARGISNULL(0) ? "*" : text_to_cstring(PG_GETARG_TEXT_PP(0));
  std::string filter =
      arg.rfind("--", 0) == 0 ? arg : std::string("--gtest_filter=") + arg;
  char arg0[] = "pax_gtest";
  char *argv[] = {arg0, filter.data(), NULL};
  int argc = 2;
  MemoryContext cxt = CurrentMemoryContext;
  ResourceOwner owner = CurrentResourceOwner;
  Stub *stub_global;
  int failed;

  stub_global = new Stub();
  testing::InitGoogleTest(&argc, argv);
  GlobalMock(stub_global);

  (void)RUN_ALL_TESTS();
  failed = testing::UnitTest::GetInstance()->failed_test_count();
  fflush(stdout);

  // the server's own functions as they were, and where the tests left it
  delete stub_global;
  MemoryContextSwitchTo(cxt);
  CurrentResourceOwner = owner;
  PG_RETURN_INT32(failed);
}
