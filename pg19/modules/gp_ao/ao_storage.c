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
 * ao_storage.c
 *	  An append-optimized table's files, kept in 8K pages of its relation.
 *
 * Cloudberry keeps each segment file of an append-optimized table, and each
 * column's of one stored by column, as a file of its own, relfilenode.N,
 * written past the buffer manager.  PostgreSQL 19's tools read every file of
 * a relation as 8K pages -- base backup and pg_checksums verify their
 * checksums, incremental backup and pg_rewind copy the blocks WAL names --
 * so the port keeps them in pages (gpdb_hook.md, Track D §2.3, D2): every
 * page of the relation is a page PostgreSQL knows how to check, through the
 * buffer manager, and logged by this module's resource manager, so that a
 * server replaying it without gp_ao stops at the first record rather than
 * reading pages it cannot interpret.
 *
 * Block 0 is the metapage: the relation's storage ID, which keys its rows in
 * gp_ao's tables; the row numbers each segment file has handed out; the
 * first block no extent has taken; and the blocks of the extent map.  Each
 * file is a run of extents, extent k 2^k pages up to 1024 and 1024 after,
 * taken at the end of the relation as a file grows, and the map lists them
 * in the order they were taken.  A file's byte stream is its extents' pages'
 * payloads, the bytes after each page's header, one after another.
 *
 * Neither the map nor the row numbers are transactional.  An extent a writer
 * took is its file's for good: a writer that rolls back leaves its bytes
 * past the end the file's committed row says, and the next writer of the
 * file writes over them, as Cloudberry's does over what an aborted writer
 * left in its file.  A row number is never handed out twice, since an index
 * may still name the row an aborted writer numbered.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "catalog/namespace.h"
#include "catalog/storage.h"
#include "commands/sequence.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "gp_ao.h"

/*
 * gp_ao's resource manager, among the custom ones (128-255): not one
 * PostgreSQL's wiki lists as taken, not Cloudberry's PAX's 199 and not
 * gp_sql's 198.  Track D §4 proposed it.
 */
#define GP_AO_RMGR_ID			200

#define XLOG_GP_AO_NEWPAGE		0x00	/* a page, whole: the metapage made */
#define XLOG_GP_AO_WRITE		0x10	/* bytes of a file, into a page */
#define XLOG_GP_AO_MAP			0x20	/* an extent, added to the map */
#define XLOG_GP_AO_ROWNUM		0x30	/* row numbers, handed out */

#define AO_META_MAGIC			0x414F4D31	/* "AOM1" */
#define AO_META_VERSION			1
#define AO_MAP_MAGIC			0x414F4D50	/* "AOMP" */

#define AO_META_COLUMNAR		0x0001

/* The largest extent is 2^AO_EXTENT_MAX_SHIFT pages. */
#define AO_EXTENT_MAX_SHIFT		10
#define AO_EXTENT_PAGES(k)		((uint32) 1 << Min((k), AO_EXTENT_MAX_SHIFT))
/* The pages of extents 0 to AO_EXTENT_MAX_SHIFT together. */
#define AO_EXTENTS_GROWING_PAGES	(((uint64) 2 << AO_EXTENT_MAX_SHIFT) - 1)

typedef struct AoMetaPageData
{
	uint32		magic;
	uint32		version;
	int64		storage_id;
	uint32		flags;
	BlockNumber next_free_block;	/* the first block no extent has taken */
	uint32		nmappages;
	uint32		nmapentries;	/* over all the map's pages */
	BlockNumber mappages[AO_MAX_MAP_PAGES];
	int64		next_rownum[AO_MAX_SEGNO + 1];
} AoMetaPageData;

typedef struct AoMapEntry
{
	uint32		filenum;
	uint32		extno;
	BlockNumber start;
	uint32		npages;
} AoMapEntry;

typedef struct AoMapPageData
{
	uint32		magic;
	uint32		nentries;
	AoMapEntry	entries[FLEXIBLE_ARRAY_MEMBER];
} AoMapPageData;

#define AO_MAP_ENTRIES_PER_PAGE \
	((AO_PAGE_PAYLOAD - offsetof(AoMapPageData, entries)) / sizeof(AoMapEntry))

#define AoMetaPageGet(page)	((AoMetaPageData *) PageGetContents(page))
#define AoMapPageGet(page)	((AoMapPageData *) PageGetContents(page))

/* ---- WAL records -------------------------------------------------------- */

typedef struct xl_gp_ao_write
{
	uint16		offset;			/* in the page's payload */
	uint16		len;
	bool		init;			/* the page is new */
} xl_gp_ao_write;

typedef struct xl_gp_ao_map
{
	AoMapEntry	entry;
	uint32		entry_index;	/* on its map page */
	uint32		nmapentries;	/* the map's entries, after this one */
	BlockNumber next_free_block;
	BlockNumber mapblk;
	bool		newmappage;
} xl_gp_ao_map;

typedef struct xl_gp_ao_rownum
{
	int32		segno;
	int64		next_rownum;
} xl_gp_ao_rownum;

/* ---- the extents of files, as this backend last read the map ------------ */

typedef struct AoRelCacheKey
{
	RelFileLocator locator;
	ProcNumber	backend;
} AoRelCacheKey;

typedef struct AoRelCache
{
	AoRelCacheKey key;
	int64		storage_id;
	uint32		nloaded;		/* map entries read */
	HTAB	   *files;			/* filenum -> AoFileExtents */
} AoRelCache;

static HTAB *ao_rel_cache = NULL;
static MemoryContext ao_storage_cxt = NULL;

/* ------------------------------------------------------------------------- */
/* Pages                                                                     */
/* ------------------------------------------------------------------------- */

static void
ao_page_set_lower(Page page, Size used)
{
	((PageHeader) page)->pd_lower = AO_PAYLOAD_OFFSET + used;
}

static AoMetaPageData *
ao_metapage_check(Relation rel, Page page)
{
	AoMetaPageData *meta = AoMetaPageGet(page);

	if (PageIsNew(page) || meta->magic != AO_META_MAGIC)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("append-optimized table \"%s\" has no valid metapage",
						RelationGetRelationName(rel))));
	if (meta->version != AO_META_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("append-optimized table \"%s\" is of version %u of the storage format, where %u is known",
						RelationGetRelationName(rel), meta->version,
						AO_META_VERSION)));
	return meta;
}

/*
 * The metapage of a new relfilenode, written through smgr, since the
 * relcache still has the relation's old one, and logged whole.  Returns the
 * storage ID it took.
 */
int64
ao_storage_init(SMgrRelation srel, RelFileLocator rlocator, char persistence,
				bool columnar)
{
	PGIOAlignedBlock buf;
	Page		page = buf.data;
	AoMetaPageData *meta;
	Oid			seqid;

	if (persistence == RELPERSISTENCE_UNLOGGED)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("unlogged append-optimized tables are not supported")));

	seqid = get_relname_relid("storage_id_seq",
							  get_namespace_oid("gp_ao", false));
	if (!OidIsValid(seqid))
		elog(ERROR, "gp_ao.storage_id_seq is missing");

	PageInit(page, BLCKSZ, 0);
	meta = AoMetaPageGet(page);
	memset(meta, 0, sizeof(AoMetaPageData));
	meta->magic = AO_META_MAGIC;
	meta->version = AO_META_VERSION;
	meta->storage_id = nextval_internal(seqid, false);
	meta->flags = columnar ? AO_META_COLUMNAR : 0;
	meta->next_free_block = AO_METAPAGE_BLKNO + 1;
	for (int i = 0; i <= AO_MAX_SEGNO; i++)
		meta->next_rownum[i] = 1;
	ao_page_set_lower(page, sizeof(AoMetaPageData));

	if (persistence == RELPERSISTENCE_PERMANENT && XLogIsNeeded())
	{
		XLogRecPtr	recptr;

		XLogBeginInsert();
		XLogRegisterBlock(0, &rlocator, MAIN_FORKNUM, AO_METAPAGE_BLKNO, page,
						  REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
		recptr = XLogInsert(GP_AO_RMGR_ID, XLOG_GP_AO_NEWPAGE);
		PageSetLSN(page, recptr);
	}

	PageSetChecksum(page, AO_METAPAGE_BLKNO);
	smgrextend(srel, MAIN_FORKNUM, AO_METAPAGE_BLKNO, page, false);

	return meta->storage_id;
}

/* The relation's metapage, pinned and locked in mode. */
static Buffer
ao_metapage_read(Relation rel, int mode)
{
	Buffer		buf = ReadBuffer(rel, AO_METAPAGE_BLKNO);

	LockBuffer(buf, mode);
	(void) ao_metapage_check(rel, BufferGetPage(buf));
	return buf;
}

/* ------------------------------------------------------------------------- */
/* The map, and this backend's copy of it                                    */
/* ------------------------------------------------------------------------- */

static AoRelCache *
ao_rel_cache_lookup(Relation rel, bool *found)
{
	AoRelCacheKey key;

	if (ao_rel_cache == NULL)
	{
		HASHCTL		ctl;

		ao_storage_cxt = AllocSetContextCreate(TopMemoryContext,
											   "gp_ao extent maps",
											   ALLOCSET_DEFAULT_SIZES);
		ctl.keysize = sizeof(AoRelCacheKey);
		ctl.entrysize = sizeof(AoRelCache);
		ctl.hcxt = ao_storage_cxt;
		ao_rel_cache = hash_create("gp_ao extent maps", 64, &ctl,
								   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	memset(&key, 0, sizeof(key));
	key.locator = rel->rd_locator;
	key.backend = rel->rd_backend;
	return hash_search(ao_rel_cache, &key, HASH_ENTER, found);
}

static void
ao_rel_cache_reset(AoRelCache *entry, int64 storage_id)
{
	HASHCTL		ctl;

	if (entry->files)
		hash_destroy(entry->files);
	ctl.keysize = sizeof(uint32);
	ctl.entrysize = sizeof(AoFileExtents);
	ctl.hcxt = ao_storage_cxt;
	entry->files = hash_create("gp_ao file extents", 16, &ctl,
							   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	entry->storage_id = storage_id;
	entry->nloaded = 0;
}

/*
 * Read the map entries this backend has not read yet.  The map only grows,
 * so what was read stays true; a storage ID that is not the one read says
 * the relfilenode is another relation's now, and everything is read again.
 * The caller holds the metapage locked.
 */
static void
ao_map_refresh(Relation rel, AoRelCache *entry, AoMetaPageData *meta)
{
	if (entry->files == NULL || entry->storage_id != meta->storage_id)
		ao_rel_cache_reset(entry, meta->storage_id);

	while (entry->nloaded < meta->nmapentries)
	{
		uint32		pageno = entry->nloaded / AO_MAP_ENTRIES_PER_PAGE;
		Buffer		buf;
		AoMapPageData *map;

		if (pageno >= meta->nmappages)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("extent map of append-optimized table \"%s\" is shorter than its metapage says",
							RelationGetRelationName(rel))));
		buf = ReadBuffer(rel, meta->mappages[pageno]);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		map = AoMapPageGet(BufferGetPage(buf));
		if (map->magic != AO_MAP_MAGIC)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("extent map page %u of append-optimized table \"%s\" is not one",
							meta->mappages[pageno], RelationGetRelationName(rel))));

		while (entry->nloaded < meta->nmapentries &&
			   entry->nloaded / AO_MAP_ENTRIES_PER_PAGE == pageno)
		{
			uint32		i = entry->nloaded % AO_MAP_ENTRIES_PER_PAGE;
			AoMapEntry *e;
			AoFileExtents *file;
			bool		found;

			if (i >= map->nentries)
				break;			/* written after the metapage we read */
			e = &map->entries[i];
			file = hash_search(entry->files, &e->filenum, HASH_ENTER, &found);
			if (!found)
			{
				file->nextents = 0;
				file->maxextents = 8;
				file->starts = MemoryContextAlloc(ao_storage_cxt,
												  sizeof(BlockNumber) * 8);
			}
			if (e->extno != (uint32) file->nextents)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("extent map of append-optimized table \"%s\" lists extent %u of file %u after %d of its extents",
								RelationGetRelationName(rel), e->extno,
								e->filenum, file->nextents)));
			if (file->nextents == file->maxextents)
			{
				file->maxextents *= 2;
				file->starts = repalloc(file->starts,
										sizeof(BlockNumber) * file->maxextents);
			}
			file->starts[file->nextents++] = e->start;
			entry->nloaded++;
		}
		UnlockReleaseBuffer(buf);
		if (entry->nloaded % AO_MAP_ENTRIES_PER_PAGE != 0 &&
			entry->nloaded < meta->nmapentries)
			break;				/* a page read short; try again next time */
	}
}

/*
 * The storage ID of rel's files, the one its metapage holds, with this
 * backend's copy of its map made current.  Every way into a table's files
 * starts here.
 */
int64
ao_storage_id(Relation rel)
{
	Buffer		buf = ao_metapage_read(rel, BUFFER_LOCK_SHARE);
	AoMetaPageData *meta = AoMetaPageGet(BufferGetPage(buf));
	AoRelCache *entry;
	bool		found;
	int64		storage_id;

	entry = ao_rel_cache_lookup(rel, &found);
	if (!found)
		entry->files = NULL;
	ao_map_refresh(rel, entry, meta);
	storage_id = meta->storage_id;
	UnlockReleaseBuffer(buf);
	return storage_id;
}

bool
ao_storage_is_columnar(Relation rel)
{
	Buffer		buf = ao_metapage_read(rel, BUFFER_LOCK_SHARE);
	bool		columnar;

	columnar = (AoMetaPageGet(BufferGetPage(buf))->flags & AO_META_COLUMNAR) != 0;
	UnlockReleaseBuffer(buf);
	return columnar;
}

/* Forget this backend's copy of rel's map: its files were truncated. */
void
ao_storage_forget(Relation rel)
{
	AoRelCache *entry;
	bool		found;

	entry = ao_rel_cache_lookup(rel, &found);
	if (found && entry->files)
		hash_destroy(entry->files);
	hash_search(ao_rel_cache, &entry->key, HASH_REMOVE, NULL);
}

/* Which extent page p of a file is in, and which page of it. */
static void
ao_page_extent(uint64 p, uint32 *extno, uint32 *within)
{
	if (p < AO_EXTENTS_GROWING_PAGES)
	{
		int			e = pg_leftmost_one_pos64(p + 1);

		*extno = (uint32) e;
		*within = (uint32) (p + 1 - ((uint64) 1 << e));
	}
	else
	{
		uint64		rest = p - AO_EXTENTS_GROWING_PAGES;

		*extno = AO_EXTENT_MAX_SHIFT + 1 + (uint32) (rest >> AO_EXTENT_MAX_SHIFT);
		*within = (uint32) (rest & ((1 << AO_EXTENT_MAX_SHIFT) - 1));
	}
}

/*
 * Take extent extno for file filenum at the end of the relation, and list it
 * in the map.  The caller writes the file and holds the lock of its segment
 * file, so no one else takes an extent for it; the relation's extension lock
 * keeps two writers of different files from taking the same blocks.
 */
static void
ao_extent_allocate(Relation rel, uint32 filenum, uint32 extno)
{
	uint32		npages = AO_EXTENT_PAGES(extno);
	Buffer		metabuf;
	Buffer		mapbuf;
	Page		metapage;
	Page		mappage;
	AoMetaPageData *meta;
	AoMapPageData *map;
	BlockNumber nblocks;
	BlockNumber mapblk;
	BlockNumber start;
	bool		newmap = false;
	AoRelCache *entry;
	bool		found;
	AoFileExtents *file;
	uint32		entry_index;

	LockRelationForExtension(rel, ExclusiveLock);

	metabuf = ao_metapage_read(rel, BUFFER_LOCK_EXCLUSIVE);
	metapage = BufferGetPage(metabuf);
	meta = AoMetaPageGet(metapage);

	/* Another backend may have taken it while this one was not looking. */
	entry = ao_rel_cache_lookup(rel, &found);
	if (!found)
		entry->files = NULL;
	ao_map_refresh(rel, entry, meta);
	file = hash_search(entry->files, &filenum, HASH_FIND, NULL);
	if (file != NULL && (uint32) file->nextents > extno)
	{
		UnlockReleaseBuffer(metabuf);
		UnlockRelationForExtension(rel, ExclusiveLock);
		return;
	}
	if ((file ? (uint32) file->nextents : 0) != extno)
		elog(ERROR, "extent %u of file %u of \"%s\" asked for before extent %d",
			 extno, filenum, RelationGetRelationName(rel),
			 file ? file->nextents : 0);

	/*
	 * The blocks the map says are taken are taken, whether or not the file
	 * reaches them: after a crash the relation may be shorter than the
	 * extents it had, since taking one is not logged, and the pages a writer
	 * never wrote were never logged either.
	 */
	nblocks = RelationGetNumberOfBlocks(rel);
	start = Max(nblocks, meta->next_free_block);
	if (meta->nmappages == 0 ||
		meta->nmapentries == meta->nmappages * AO_MAP_ENTRIES_PER_PAGE)
	{
		if (meta->nmappages == AO_MAX_MAP_PAGES)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("append-optimized table \"%s\" has as many extents as it can list",
							RelationGetRelationName(rel)),
					 errhint("VACUUM FULL rewrites it.")));
		mapblk = start++;
		newmap = true;
	}
	else
		mapblk = meta->mappages[meta->nmappages - 1];

	if (start + npages > MaxBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("append-optimized table \"%s\" cannot grow any more",
						RelationGetRelationName(rel))));

	/* The new pages, zeroed, up to the end of the new extent. */
	if (nblocks < start + npages)
		smgrzeroextend(RelationGetSmgr(rel), MAIN_FORKNUM, nblocks,
					   start + npages - nblocks, false);

	if (newmap)
		mapbuf = ReadBufferExtended(rel, MAIN_FORKNUM, mapblk,
									RBM_ZERO_AND_LOCK, NULL);
	else
	{
		mapbuf = ReadBuffer(rel, mapblk);
		LockBuffer(mapbuf, BUFFER_LOCK_EXCLUSIVE);
	}
	mappage = BufferGetPage(mapbuf);

	START_CRIT_SECTION();

	if (newmap)
	{
		PageInit(mappage, BLCKSZ, 0);
		map = AoMapPageGet(mappage);
		map->magic = AO_MAP_MAGIC;
		map->nentries = 0;
		meta->mappages[meta->nmappages++] = mapblk;
	}
	map = AoMapPageGet(mappage);
	entry_index = map->nentries;
	map->entries[entry_index].filenum = filenum;
	map->entries[entry_index].extno = extno;
	map->entries[entry_index].start = start;
	map->entries[entry_index].npages = npages;
	map->nentries++;
	ao_page_set_lower(mappage, offsetof(AoMapPageData, entries) +
					  map->nentries * sizeof(AoMapEntry));
	meta->nmapentries++;
	meta->next_free_block = start + npages;

	MarkBufferDirty(metabuf);
	MarkBufferDirty(mapbuf);

	if (RelationNeedsWAL(rel))
	{
		xl_gp_ao_map xlrec;
		XLogRecPtr	recptr;

		xlrec.entry = map->entries[entry_index];
		xlrec.entry_index = entry_index;
		xlrec.nmapentries = meta->nmapentries;
		xlrec.next_free_block = meta->next_free_block;
		xlrec.mapblk = mapblk;
		xlrec.newmappage = newmap;

		XLogBeginInsert();
		XLogRegisterData(&xlrec, sizeof(xlrec));
		XLogRegisterBuffer(0, metabuf, REGBUF_STANDARD);
		XLogRegisterBuffer(1, mapbuf,
						   REGBUF_STANDARD | (newmap ? REGBUF_WILL_INIT : 0));
		recptr = XLogInsert(GP_AO_RMGR_ID, XLOG_GP_AO_MAP);
		PageSetLSN(metapage, recptr);
		PageSetLSN(mappage, recptr);
	}

	END_CRIT_SECTION();

	/* This backend's copy has it too, read with the map page let go. */
	UnlockReleaseBuffer(mapbuf);
	ao_map_refresh(rel, entry, meta);

	UnlockReleaseBuffer(metabuf);
	UnlockRelationForExtension(rel, ExclusiveLock);
}

/*
 * The block page p of file filenum is in.  A writer asks with allocate, and
 * gets the extents it needs taken; a reader asks for pages below the end
 * its snapshot saw, which the map has.
 */
static BlockNumber
ao_file_block(Relation rel, uint32 filenum, uint64 p, bool allocate)
{
	uint32		extno;
	uint32		within;
	bool		refreshed = false;

	ao_page_extent(p, &extno, &within);

	for (;;)
	{
		bool		found;
		AoRelCache *entry = ao_rel_cache_lookup(rel, &found);
		AoFileExtents *file;
		uint32		have;

		if (!found)
			entry->files = NULL;
		file = entry->files ?
			hash_search(entry->files, &filenum, HASH_FIND, NULL) : NULL;
		have = file ? (uint32) file->nextents : 0;
		if (have > extno)
			return file->starts[extno] + within;

		if (!refreshed)
		{
			/* Read what the map has that this backend has not. */
			Buffer		buf = ao_metapage_read(rel, BUFFER_LOCK_SHARE);

			ao_map_refresh(rel, entry, AoMetaPageGet(BufferGetPage(buf)));
			UnlockReleaseBuffer(buf);
			refreshed = true;
			continue;
		}
		if (!allocate)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("page " UINT64_FORMAT " of file %u of append-optimized table \"%s\" is in no extent",
							p, filenum, RelationGetRelationName(rel))));
		ao_extent_allocate(rel, filenum, have);
	}
}

/* ------------------------------------------------------------------------- */
/* Row numbers                                                               */
/* ------------------------------------------------------------------------- */

/*
 * Hand out count row numbers of segment file segno, and return the first.
 * They are never handed out again, whatever becomes of the transaction.
 */
int64
ao_reserve_rownums(Relation rel, int segno, int64 count)
{
	Buffer		buf;
	Page		page;
	AoMetaPageData *meta;
	int64		first;

	Assert(segno > 0 && segno <= AO_MAX_SEGNO);
	buf = ao_metapage_read(rel, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buf);
	meta = AoMetaPageGet(page);
	first = meta->next_rownum[segno];
	if (first + count - 1 > AO_MAX_ROWNUM)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("segment file %d of append-optimized table \"%s\" has no row numbers left",
						segno, RelationGetRelationName(rel)),
				 errhint("VACUUM FULL rewrites the table.")));

	START_CRIT_SECTION();
	meta->next_rownum[segno] = first + count;
	MarkBufferDirty(buf);
	if (RelationNeedsWAL(rel))
	{
		xl_gp_ao_rownum xlrec;
		XLogRecPtr	recptr;

		xlrec.segno = segno;
		xlrec.next_rownum = meta->next_rownum[segno];
		XLogBeginInsert();
		XLogRegisterData(&xlrec, sizeof(xlrec));
		XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
		recptr = XLogInsert(GP_AO_RMGR_ID, XLOG_GP_AO_ROWNUM);
		PageSetLSN(page, recptr);
	}
	END_CRIT_SECTION();

	UnlockReleaseBuffer(buf);
	return first;
}

/* The first row number segment file segno has not handed out. */
int64
ao_next_rownum(Relation rel, int segno)
{
	Buffer		buf = ao_metapage_read(rel, BUFFER_LOCK_SHARE);
	int64		next = AoMetaPageGet(BufferGetPage(buf))->next_rownum[segno];

	UnlockReleaseBuffer(buf);
	return next;
}

/* ------------------------------------------------------------------------- */
/* Files                                                                     */
/* ------------------------------------------------------------------------- */

/*
 * Write len bytes at offset of file filenum.  A page the write starts at the
 * beginning of is new, whatever it held: what a file has past the end a
 * writer starts at is what an aborted writer left there.
 */
void
ao_file_write(Relation rel, uint32 filenum, uint64 offset,
			  const char *data, Size len)
{
	while (len > 0)
	{
		uint64		p = offset / AO_PAGE_PAYLOAD;
		uint32		inpage = offset % AO_PAGE_PAYLOAD;
		uint32		n = Min(len, AO_PAGE_PAYLOAD - inpage);
		BlockNumber blk = ao_file_block(rel, filenum, p, true);
		bool		init = (inpage == 0);
		Buffer		buf;
		Page		page;

		if (blk >= RelationGetNumberOfBlocks(rel))
			buf = ExtendBufferedRelTo(BMR_REL(rel), MAIN_FORKNUM, NULL,
									  EB_CLEAR_SIZE_CACHE, blk + 1,
									  RBM_ZERO_AND_LOCK);
		else if (init)
			buf = ReadBufferExtended(rel, MAIN_FORKNUM, blk, RBM_ZERO_AND_LOCK,
									 NULL);
		else
		{
			buf = ReadBuffer(rel, blk);
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		}
		page = BufferGetPage(buf);
		if (!init && PageIsNew(page))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("page %u of append-optimized table \"%s\" should hold data of file %u, and is empty",
							blk, RelationGetRelationName(rel), filenum)));

		START_CRIT_SECTION();
		if (init)
			PageInit(page, BLCKSZ, 0);
		memcpy((char *) page + AO_PAYLOAD_OFFSET + inpage, data, n);
		if (((PageHeader) page)->pd_lower < AO_PAYLOAD_OFFSET + inpage + n)
			ao_page_set_lower(page, inpage + n);
		MarkBufferDirty(buf);

		if (RelationNeedsWAL(rel))
		{
			xl_gp_ao_write xlrec;
			XLogRecPtr	recptr;

			xlrec.offset = inpage;
			xlrec.len = n;
			xlrec.init = init;
			XLogBeginInsert();
			XLogRegisterData(&xlrec, sizeof(xlrec));
			XLogRegisterBuffer(0, buf,
							   REGBUF_STANDARD | (init ? REGBUF_WILL_INIT : 0));
			XLogRegisterBufData(0, data, n);
			recptr = XLogInsert(GP_AO_RMGR_ID, XLOG_GP_AO_WRITE);
			PageSetLSN(page, recptr);
		}
		END_CRIT_SECTION();
		(void) AO_FAULT("xlog_ao_insert", rel);

		UnlockReleaseBuffer(buf);

		offset += n;
		data += n;
		len -= n;
	}
}

/* Read len bytes at offset of file filenum, which a committed end covers. */
void
ao_file_read(Relation rel, uint32 filenum, uint64 offset, char *data,
			 Size len, BufferAccessStrategy strategy)
{
	while (len > 0)
	{
		uint64		p = offset / AO_PAGE_PAYLOAD;
		uint32		inpage = offset % AO_PAGE_PAYLOAD;
		uint32		n = Min(len, AO_PAGE_PAYLOAD - inpage);
		BlockNumber blk = ao_file_block(rel, filenum, p, false);
		Buffer		buf;
		Page		page;

		buf = ReadBufferExtended(rel, MAIN_FORKNUM, blk, RBM_NORMAL, strategy);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (PageIsNew(page) ||
			((PageHeader) page)->pd_lower < AO_PAYLOAD_OFFSET + inpage + n)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("page %u of append-optimized table \"%s\" holds less of file %u than its end says",
							blk, RelationGetRelationName(rel), filenum)));
		memcpy(data, (char *) page + AO_PAYLOAD_OFFSET + inpage, n);
		UnlockReleaseBuffer(buf);

		offset += n;
		data += n;
		len -= n;
	}
}

/*
 * ALTER TABLE ... SET TABLESPACE: the relation's pages, copied to its new
 * relfilenode as they are, the storage ID with them, and logged by this
 * module rather than as the core's page images.
 */
void
ao_copy_storage(Relation rel, SMgrRelation dst, RelFileLocator dstlocator,
				char persistence)
{
	SMgrRelation src = RelationGetSmgr(rel);
	BlockNumber nblocks;
	PGIOAlignedBlock buf;
	Page		page = buf.data;
	bool		use_wal;

	use_wal = XLogIsNeeded() && persistence == RELPERSISTENCE_PERMANENT;

	nblocks = smgrnblocks(src, MAIN_FORKNUM);
	for (BlockNumber blkno = 0; blkno < nblocks; blkno++)
	{
		CHECK_FOR_INTERRUPTS();

		smgrread(src, MAIN_FORKNUM, blkno, page);
		if (!PageIsNew(page) && use_wal)
		{
			XLogRecPtr	recptr;

			XLogBeginInsert();
			XLogRegisterBlock(0, &dstlocator, MAIN_FORKNUM, blkno, page,
							  REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
			recptr = XLogInsert(GP_AO_RMGR_ID, XLOG_GP_AO_NEWPAGE);
			PageSetLSN(page, recptr);
		}
		if (!PageIsNew(page))
			PageSetChecksum(page, blkno);
		smgrextend(dst, MAIN_FORKNUM, blkno, page, true);
	}

	/*
	 * The copy went past the buffer manager, and was logged if WAL is kept;
	 * where it is not, the new relfilenode is synced at commit, as
	 * RelationCreateStorage() arranged.
	 */
	if (use_wal)
		smgrimmedsync(dst, MAIN_FORKNUM);
}

/* ------------------------------------------------------------------------- */
/* The resource manager                                                      */
/* ------------------------------------------------------------------------- */

static void
ao_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	Buffer		buf;

	switch (info)
	{
		case XLOG_GP_AO_NEWPAGE:
			if (XLogReadBufferForRedo(record, 0, &buf) != BLK_RESTORED)
				elog(ERROR, "gp_ao: page image missing from a new page's record");
			UnlockReleaseBuffer(buf);
			break;

		case XLOG_GP_AO_WRITE:
			{
				xl_gp_ao_write *xlrec = (xl_gp_ao_write *) XLogRecGetData(record);
				XLogRedoAction action;

				action = XLogReadBufferForRedoExtended(record, 0,
													   xlrec->init ? RBM_ZERO_AND_LOCK : RBM_NORMAL,
													   false, &buf);
				if (action == BLK_NEEDS_REDO)
				{
					Page		page = BufferGetPage(buf);
					Size		datalen;
					char	   *data = XLogRecGetBlockData(record, 0, &datalen);

					if (xlrec->init)
						PageInit(page, BLCKSZ, 0);
					memcpy((char *) page + AO_PAYLOAD_OFFSET + xlrec->offset,
						   data, xlrec->len);
					if (((PageHeader) page)->pd_lower <
						AO_PAYLOAD_OFFSET + xlrec->offset + xlrec->len)
						ao_page_set_lower(page, xlrec->offset + xlrec->len);
					PageSetLSN(page, record->EndRecPtr);
					MarkBufferDirty(buf);
				}
				if (BufferIsValid(buf))
					UnlockReleaseBuffer(buf);
				break;
			}

		case XLOG_GP_AO_MAP:
			{
				xl_gp_ao_map *xlrec = (xl_gp_ao_map *) XLogRecGetData(record);
				Buffer		mapbuf;

				if (XLogReadBufferForRedo(record, 0, &buf) == BLK_NEEDS_REDO)
				{
					Page		page = BufferGetPage(buf);
					AoMetaPageData *meta = AoMetaPageGet(page);

					if (xlrec->newmappage)
						meta->mappages[meta->nmappages++] = xlrec->mapblk;
					meta->nmapentries = xlrec->nmapentries;
					meta->next_free_block = xlrec->next_free_block;
					PageSetLSN(page, record->EndRecPtr);
					MarkBufferDirty(buf);
				}
				if (BufferIsValid(buf))
					UnlockReleaseBuffer(buf);

				if (XLogReadBufferForRedoExtended(record, 1,
												  xlrec->newmappage ? RBM_ZERO_AND_LOCK : RBM_NORMAL,
												  false, &mapbuf) == BLK_NEEDS_REDO)
				{
					Page		page = BufferGetPage(mapbuf);
					AoMapPageData *map;

					if (xlrec->newmappage)
					{
						PageInit(page, BLCKSZ, 0);
						map = AoMapPageGet(page);
						map->magic = AO_MAP_MAGIC;
						map->nentries = 0;
					}
					map = AoMapPageGet(page);
					map->entries[xlrec->entry_index] = xlrec->entry;
					map->nentries = xlrec->entry_index + 1;
					ao_page_set_lower(page, offsetof(AoMapPageData, entries) +
									  map->nentries * sizeof(AoMapEntry));
					PageSetLSN(page, record->EndRecPtr);
					MarkBufferDirty(mapbuf);
				}
				if (BufferIsValid(mapbuf))
					UnlockReleaseBuffer(mapbuf);
				break;
			}

		case XLOG_GP_AO_ROWNUM:
			{
				xl_gp_ao_rownum *xlrec = (xl_gp_ao_rownum *) XLogRecGetData(record);

				if (XLogReadBufferForRedo(record, 0, &buf) == BLK_NEEDS_REDO)
				{
					Page		page = BufferGetPage(buf);

					AoMetaPageGet(page)->next_rownum[xlrec->segno] = xlrec->next_rownum;
					PageSetLSN(page, record->EndRecPtr);
					MarkBufferDirty(buf);
				}
				if (BufferIsValid(buf))
					UnlockReleaseBuffer(buf);
				break;
			}

		default:
			elog(PANIC, "gp_ao_redo: unknown op code %u", info);
	}
}

static void
ao_desc(StringInfo buf, XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	char	   *rec = XLogRecGetData(record);

	switch (info)
	{
		case XLOG_GP_AO_WRITE:
			{
				xl_gp_ao_write *xlrec = (xl_gp_ao_write *) rec;

				appendStringInfo(buf, "offset %u, %u bytes%s", xlrec->offset,
								 xlrec->len, xlrec->init ? ", new page" : "");
				break;
			}
		case XLOG_GP_AO_MAP:
			{
				xl_gp_ao_map *xlrec = (xl_gp_ao_map *) rec;

				appendStringInfo(buf, "file %u extent %u: %u pages at block %u",
								 xlrec->entry.filenum, xlrec->entry.extno,
								 xlrec->entry.npages, xlrec->entry.start);
				break;
			}
		case XLOG_GP_AO_ROWNUM:
			{
				xl_gp_ao_rownum *xlrec = (xl_gp_ao_rownum *) rec;

				appendStringInfo(buf, "segno %d: next row " INT64_FORMAT,
								 xlrec->segno, xlrec->next_rownum);
				break;
			}
	}
}

static const char *
ao_identify(uint8 info)
{
	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_GP_AO_NEWPAGE:
			return "NEWPAGE";
		case XLOG_GP_AO_WRITE:
			return "WRITE";
		case XLOG_GP_AO_MAP:
			return "MAP";
		case XLOG_GP_AO_ROWNUM:
			return "ROWNUM";
	}
	return NULL;
}

static const RmgrData ao_rmgr = {
	.rm_name = "gp_ao",
	.rm_redo = ao_redo,
	.rm_desc = ao_desc,
	.rm_identify = ao_identify,
};

/* From _PG_init, which only preload runs. */
void
ao_register_rmgr(void)
{
	RegisterCustomRmgr(GP_AO_RMGR_ID, &ao_rmgr);
}
