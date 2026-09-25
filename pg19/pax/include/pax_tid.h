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
 * pax_tid.h
 *	  A PAX table's row IDs as PostgreSQL 19 is given them.
 *
 * Inside, PAX names a row as Cloudberry's does (storage/pax_itemptr.h): the
 * number of its file, 24 bits, and its place in the file, 23 bits, with
 * ip_posid up to 32768 -- more than PostgreSQL 19's TID bitmap takes, which
 * refuses an offset past MaxHeapTuplesPerPage, 291 on 8K pages.  So the TIDs
 * the table access method gives the executor and its indexes, and takes from
 * them, are of a layout of the table's own, translated at the method's
 * boundary (access/pax_access_handle.cc): the file number in the block
 * number's high F bits, the row's block of 291 in its file in the low
 * 32 - F, and the row's place in that block, plus one, as the offset.
 *
 * F is the table's, kept in its row of pg_pax_tables and fixed whenever it
 * starts new files -- CREATE, TRUNCATE and a rewrite -- from the rows a file
 * may hold then, pax.max_tuples_per_file: 20 by default, a file of up to
 * 1,191,936 rows and 1,048,575 files a table on each segment (Track D §6,
 * question 6).  A writer never puts more rows in a file than the table's
 * layout has room for, and a table that has used its last file number is
 * refused new files, cleanly, until a TRUNCATE or a rewrite starts it again.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PAX_TID_H
#define PAX_TID_H

#include "storage/pax_itemptr.h"

/* The table's layout: F, the bits of the block number its file number takes. */
#define PAX_TID_MIN_FILE_BITS 1
#define PAX_TID_MAX_FILE_BITS PAX_BLOCK_BIT_SIZE

/* Rows a block of the table's layout holds: the TID bitmap's most. */
#define PAX_TID_ROWS_PER_BLOCK MaxHeapTuplesPerPage

/* The file bits a table starting new files takes for its rows per file. */
static inline int
PaxTidFileBitsFor(uint64 rows_per_file)
{
	uint64		blocks = (rows_per_file + PAX_TID_ROWS_PER_BLOCK - 1) /
		PAX_TID_ROWS_PER_BLOCK;
	int			block_bits = 0;

	while (block_bits < 32 && ((uint64) 1 << block_bits) < blocks)
		block_bits++;
	return Max(PAX_TID_MIN_FILE_BITS,
			   Min(PAX_TID_MAX_FILE_BITS, 32 - block_bits));
}

/* The most rows a file of a table of file_bits holds: PAX's own most too. */
static inline uint64
PaxTidRowsPerFile(int file_bits)
{
	return Min((uint64) PAX_TID_ROWS_PER_BLOCK << (32 - file_bits),
			   (uint64) PAX_MAX_NUM_TUPLES_PER_FILE);
}

/*
 * The last file number a table of file_bits may use: one short of the
 * last its bits hold, so that no block number is InvalidBlockNumber.
 */
static inline uint32
PaxTidMaxFileNumber(int file_bits)
{
	return (uint32) (((uint64) 1 << file_bits) - 2);
}

/* PAX's row ID of a row, as the table gives it. */
static inline ItemPointerData
PaxTidToTable(ItemPointerData internal, int file_bits)
{
	ItemPointerData tid;
	uint32		file = pax::GetBlockNumber(internal);
	uint32		row = pax::GetTupleOffsetInternal(internal);

	ItemPointerSet(&tid,
				   (BlockNumber) (((uint64) file << (32 - file_bits)) |
								  (row / PAX_TID_ROWS_PER_BLOCK)),
				   (OffsetNumber) (row % PAX_TID_ROWS_PER_BLOCK + 1));
	return tid;
}

/* The table's TID of a row, as PAX names it. */
static inline ItemPointerData
PaxTidFromTable(ItemPointerData tid, int file_bits)
{
	BlockNumber block = ItemPointerGetBlockNumberNoCheck(&tid);
	OffsetNumber offset = ItemPointerGetOffsetNumberNoCheck(&tid);
	uint32		file = (uint32) ((uint64) block >> (32 - file_bits));
	uint64		row_block = block & (uint32) (((uint64) 1 << (32 - file_bits)) - 1);

	return pax::MakeCTID(file, (uint32) (row_block * PAX_TID_ROWS_PER_BLOCK +
										 (offset - 1)));
}

namespace paxc {
/*
 * The table's file bits, from its row of pg_pax_tables, remembered in
 * rel->rd_amcache until the relation's cache entry is reset.
 */
int PaxTableFileBits(Relation rel);

/* A new file's number, refused past the last the table's layout has. */
void PaxTableCheckFileNumber(Relation rel, uint32 file_number);
}

namespace cbdb {
/* The same, where a PostgreSQL error becomes a C++ exception. */
int PaxTableFileBits(Relation rel);
void PaxTableCheckFileNumber(Relation rel, uint32 file_number);
}

#endif							/* PAX_TID_H */
