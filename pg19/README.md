# `pg19/` — Apache Cloudberry as extensions on PostgreSQL 19

This directory holds everything the PostgreSQL 19 port adds. Cloudberry's own
sources stay at their existing paths, so that `git merge upstream/main` keeps
applying to them; this directory is the only new top-level directory.

    compat/      compatibility headers (PG16 -> PG19 shims, field accessors)
    include/     headers the port's own modules share
    modules/     one directory per extension module: build, glue, SQL, control
    grammar/     O26's desugaring: Cloudberry's spelling of a statement
                 rewritten into PostgreSQL's, before the grammar sees it
    orca/        ORCA: its core taken from Cloudberry's tree unmodified, and
                 the translator, which is the port's own code
    docker/      the Compose project: patched PG19, the modules, a cluster
    gpMgmt/      Cloudberry's management tools on PostgreSQL 19's own: what
                 is installed, and the port's copies of the files it changes
    test/        the port's test harness

## ORCA

`gp_orca` is split in two, and the split is worth knowing about because it
decides how much of ORCA the port has to own.

ORCA's four core libraries — `libgpos`, `libnaucrates`, `libgpopt`,
`libgpdbcost`, 920 sources and some 380k lines — include **no PostgreSQL
header at all**. There is no PostgreSQL 16 in them to port, so the build
compiles them from Cloudberry's own tree at their own paths, unmodified, and
they need no adaptation: they build clean against PostgreSQL 19's toolchain
with no warnings. That is also what keeps Cloudberry's own ORCA features —
plan hints, parallel scans, the dedup-superset preprocessor, partial
aggregation below joins — without porting a line of them.

What the core does reach for is the `gpdb::` namespace, and the PostgreSQL
globals it declares `extern` itself. That wrapper layer, and the
Query/plan ↔ DXL translator beside it, are the whole of ORCA's coupling to
PostgreSQL, and they live here under `orca/` as the port's own code.

`core_sources.txt` is a checked-in list rather than a glob, so that a source
added or removed upstream shows up in review; `list-core-sources.sh`
refreshes it.

## What is built

Each module is a shared library built against the **patched** PostgreSQL 19
from `github/postgres`, branch `REL_19_STABLE_CLOUDBERRY`. The build finds it
through `pg_config`; it never uses `src/include` of this tree, because those
are PostgreSQL 16 headers.

Modules that must be preloaded refuse to load any other way, so a server
without them in `shared_preload_libraries` fails cleanly instead of running
half-initialised. `gp_core` publishes a rendezvous variable that the other
modules look for, which is how they detect that they were listed before it.

## Building

    meson setup build -Dpg_config=/path/to/patched/pg19/bin/pg_config
    ninja -C build
    ninja -C build install

Or, without installing anything on the host:

    docker compose -f pg19/docker/compose.yml build
    docker compose -f pg19/docker/compose.yml run --rm tests load

## Status

Milestones **M0** and **M1** are complete, and **M2 — a cluster — is built**
(2026-09-23), the interconnect included.  **M3 — distributed transactions —
is built** (2026-09-24): two-phase commit, distributed snapshots, the global
deadlock detector, row locks under ORCA, and the loopback to the
maintenance database, which joins the distributed transaction.

On one node (M1):

- `gp_matview` — incrementally maintained materialized views and dynamic
  tables.  A view over one table, over several, or over a table joined to
  itself is maintained by delta, as are `count`, `sum` and `avg`; what the
  delta cannot express — an outer join, `min`, `max`, TRUNCATE — is
  recomputed.  A dynamic table refreshes itself through `gp_task`.
- `gp_task` — the task scheduler, run by a background worker, its jobs in
  one database (`gp.task_database`), written there from any other and read
  from any other as Cloudberry's `pg_task` and `pg_task_run_history`, a
  user's own and a superuser's all; a schedule is cron's five fields or, as
  Cloudberry's may be, an interval of 1 to 59 seconds.
- `gp_sql` — the Cloudberry-only SQL surface: tags, defined once for the
  cluster as Cloudberry's are, whose owner is not dropped while it owns
  one; directory tables and storage servers, kept in one database
  (`gp.maintenance_database`) for the cluster, a directory table's files
  kept on a storage server through the handler a module registers for the
  server's protocol, given the user's credentials (`include/gp_storage.h`);
  and Cloudberry's spelling of statements through O26 — classic partition
  clauses, `DISTRIBUTED BY`, `DECODE`, `gp_dist_random('t')`.
- `gp_security` — password profiles.
- `gp_orca` — ORCA plans on one node, with the fallback counters.

On a cluster (M2), `gp_core` and `gp_orca`:

- the nodes, read from a file (`gp.cluster_config`); the dispatcher, an
  ordinary libpq client authenticated with SCRAM, whose statements run in the
  coordinator's transaction, savepoints included;
- DDL on every node with the coordinator's OIDs (R1), and a segment's own
  catalog rows, its temporary namespaces, with OIDs from the top of the OID
  space, which the coordinator's counter does not reach; distribution policies
  hashed by Cloudberry's cdbhash, ANALYZE sampling the segments (O3), CREATE
  TABLE AS and ALTER TABLE ... SET DISTRIBUTED BY;
- DISTRIBUTED BY checked as Cloudberry checks it — its columns, a column's
  operator class, the table's unique constraints and indexes, inheritance —
  in Cloudberry's words, and Cloudberry's legacy hash, the `cdbhash_*_ops`
  classes, which `gp.use_legacy_hashops` gives a new key;
- what a segment says — a trigger's NOTICE — reaching the client, and
  Cloudberry's rules for triggers and for the names it reserves;
- ORCA's distributed plans — the five Motions, a Gather to the one segment
  a slice runs on among them, Split, direct dispatch, the slice table —
  carried out by gp_core, each slice sent the values of the parameters it
  reads; a CTE in a slice the segments run, and ROLLUP, CUBE and grouping
  sets, which ORCA aggregates over one, shared through files each segment
  keeps, Cloudberry's ShareInputScan, by a producer and consumers in its
  slice and in others (`orca/compat/sharedscan.c`), and one the
  coordinator's slice produces read in the coordinator's slices that send
  to the segments; NOT IN as the planner's hashed SubPlan; now() and its kin
  the coordinator's on every segment; a sequence's next value in a slice the
  segments run, taken from the coordinator's sequence a block at a time
  (`modules/gp_core/gp_seq.c`); RETURNING and ON CONFLICT given to the
  ModifyTable beside ORCA's plan, and ORCA's UPDATE and DELETE of a join or
  of a partitioned table -- one that moves rows between partitions or
  segments among them -- re-checked as the planner's are, with row marks on
  the other tables, where the target is not held against a re-check;
  MERGE, ORCA planning its join and a MERGE ModifyTable over it on one node,
  the explicit write over it on a cluster (`orca/merge.c`); and
  PostgreSQL's own plans gathering from the
  segments where ORCA does not plan, writing a distributed table through an
  Explicit Redistribute Motion — each row changed on its segment by its ctid
  there, a row whose key changes moved by a Split that fires no trigger,
  RETURNING (old and new too) and a view's WITH CHECK OPTION and a table's
  policies evaluated on the coordinator, MERGE, WHERE CURRENT OF and ON
  CONFLICT in a WITH query, a replicated table's row found on every segment
  by what it holds, and — with the deadlock detector on — a row another
  transaction updates between the coordinator's read and the segment's
  write refused in Cloudberry's words, where it was passed over;
- ALTER TABLE ... EXPAND TABLE and SHRINK TABLE TO n, as Cloudberry's
  gpexpand runs them, and direct dispatch to the segments a few key values
  are on;
- every segment has each table's distribution policy, the `gp` label the
  coordinator writes;
- Cloudberry's settings of the dispatcher and the planner, as `gp.*`, among
  them direct dispatch's INFO lines and autostats;
- **every slice of a query at once**: the writer, the session's backend on a
  segment, runs one slice, and readers — more backends of the session there,
  reading as a part of the writer's transaction through the shared snapshot
  (R2 and R4), members of its lock group, which plan no parallel workers
  whatever a function sets — run the others, each sender streaming its rows to its
  receivers over a Unix socket or a TCP port, or, with
  `gp.interconnect_type = udpifc`, in UDP packets each receiver acknowledges,
  with Cloudberry's flow control, retransmission and deadlock check, a row
  as a tuple.  The earlier relay through the coordinator carries the slices
  that run on the coordinator or have to run in the writer — the
  coordinator's own, one that scans a temporary table — first, and the rest
  stream; it carries all of them on request (`gp.interconnect_type =
  relay`);
- `gp_segment_id`, as a call of the row's segment (O10), and
  `gp_dist_random('t')`, which a view prints back as it was written (O31);
- Cloudberry's catalogs by their names, in `pg_catalog`: `gp_id`,
  `gp_segment_configuration` over the cluster file, `gp_configuration_history`,
  and `gp_distribution_policy` over the labels, which a write to it — with
  `allow_system_table_mods`, as Cloudberry's is written — writes;
- partial tables, spread over the first so many segments, which
  `gp_debug_numsegments` (Cloudberry's extension, carried by `gp_sql`) and
  `gp_distribution_policy.numsegments` make, and which ORCA leaves to the
  planner, as Cloudberry's does.

What M2 leaves open under ORCA: a data-modifying statement in WITH; a
MERGE's RETURNING and WHEN NOT MATCHED BY SOURCE, and a MERGE into a view or
a partitioned, replicated or coordinator's table; and an UPDATE or DELETE
whose re-check would copy a row whole -- of a subquery or a function read
beside the target -- or read a sublink's again: they stay the planner's.
The planner's route sends a partitioned table's UPDATE and DELETE that read
only it to the segments whole, as a plain table's.

Distributed transactions (M3), in `gp_core`:

- **two-phase commit**: a transaction that wrote on a segment is prepared on
  each segment that wrote, under the coordinator's own transaction ID, whose
  commit record decides it; a process on the coordinator finishes, by that
  record, whatever a failure left prepared.  A segment needs
  `max_prepared_transactions` above zero.  Each segment says with every
  answer whether its part wrote, so nobody is asked as the transaction
  commits, and a part that wrote alone, the coordinator writing nothing,
  commits in one phase, as Cloudberry's does -- ordered after the one-phase
  commits it may have seen, as Cloudberry orders them;
- **distributed snapshots**: each statement is sent the coordinator's
  snapshot of it, and a segment makes its own agree — it waits for a
  transaction the snapshot says committed and it holds only prepared, and
  hides one the snapshot says in progress that it has committed, holding
  back with the replication slot `gp_dtx_horizon` what such a transaction
  deleted;
- **the global deadlock detector** (`gp.enable_global_deadlock_detector`):
  without it an UPDATE or DELETE of a distributed table locks the table,
  and a write of a partitioned table its partitions, as Cloudberry's does
  -- the parser opening the table in that lock, or the rewriter under a
  view, through O30, so that no lock taken after it is an upgrade;
  with it rows are locked, and a process on the coordinator gathers every
  node's waits, reduces the graph with Cloudberry's own detector
  (`src/backend/utils/gdd/gdddetector.c`, compiled where it lies) and
  cancels the youngest transaction of a cycle;
- Cloudberry's columns of `pg_locks` by their names -- `gp_segment_id`,
  `mppsessionid`, `mppiswriter` -- as calls on the row, as `gp_segment_id`
  is (O10), and `gp.session_id`;
- `INSERT ... ON CONFLICT` into a distributed table, the clause each
  segment's;
- **`SELECT ... FOR UPDATE`** and the other locking clauses: under ORCA as
  under the planner, the rows are locked by a LockRows node — at the top of
  the plan on one node, on the segments below the Gather on a cluster with
  the deadlock detector on, for the one-table query Cloudberry's planner
  locks rows for — and otherwise, on a cluster, the table is locked, as
  Cloudberry locks it; with the detector on, which of the two is decided as
  the query is planned, the parser holding AccessShareLock until then, as
  Cloudberry's does;
- **the loopback to the maintenance database**: what Cloudberry keeps in a
  shared catalog and cannot be a label — task jobs, storage servers — lives
  in one database, and any other writes it there through a connection of its
  own, as its transaction commits, in a transaction that is prepared with the
  segments' parts and finished by the same recovery -- on one node too,
  which runs that recovery where it may prepare -- and reads it there, the
  coordinator's from a segment;
- the coordinator's `pg_class` counts a distributed table's pages, rows and
  all-visible pages as the segments do, after VACUUM and ANALYZE, as
  Cloudberry's brings them back;
- Cloudberry's fault injector, `gp_inject_fault`, for the tests: its faults
  at the port's own places under Cloudberry's names, and at PostgreSQL 19's
  injection points, among them O29's in PostgreSQL's commit.

What M3 leaves open: a server that cannot prepare
(`max_prepared_transactions` at zero, PostgreSQL's default) leaves the
loopback's part open until the transaction that asked for it has
committed, and commits it then, so that any failure before that rolls back
both; a crash, or a lost connection, in between loses the part, which only
a prepared part survives.

M4 has begun: a segment may have a mirror, a hot standby streaming from its
primary as `gp_walreceiver`, and FTS, a process of `gp_core`'s on the
coordinator (`gp_fts.c`), brings each pair in sync, marks a mirror that
stops down and lets its primary's commits go on without it, and fails over
from a primary that stops to its mirror.  What it finds is the role, mode
and status `gp_segment_configuration` shows, kept in `gpsegconfig_dump` in
the coordinator's data directory, and the dispatcher follows it, ending a
transaction a failover catches as Cloudberry's does.  A segment's commit
waits for its mirror whatever cancels it (R3).  A directory table's files
are WAL-logged, through `gp_sql`'s own resource manager, `gp_dirtable`
(ID 198), so that a mirror has them; a server that replays them has to
preload `gp_sql`.  A database copied (CREATE DATABASE ... TEMPLATE) or moved
(ALTER DATABASE ... SET TABLESPACE) takes PAX's and the directory tables'
directories with it, which PostgreSQL copies a database without: `gp_core`
copies them, and logs each copy through its own resource manager (ID 197),
so that a mirror or a standby copies its own.  A segment's map of its distributed transactions is
logged, in `gp_internal.distributed_log`, so that it outlives a restart
and a promotion, and a background worker of each segment's, a mirror's
too, keeps the slot that holds back what those transactions deleted.  The
injection points Cloudberry's FTS tests hold are a patch the tests' build
applies (`pg19/docker/patches`), not a patch of the core series.  Left
open: six of Cloudberry's FTS tests, each for what the port did not have
(`cloudberry.md`) -- three of them run since M7's tools (below).

M5 — storage and loading — has begun (2026-09-25), on eleven more patches
of the core series, O13 to O21, O23 and O32:

- `gp_ao`: append-optimized tables, by row (`ao_row`) and by column
  (`ao_column`), as table access methods whose blocks are 8K pages of the
  table's own relation, through the buffer manager and logged by `gp_ao`'s
  resource manager (ID 200), so that a standby, a base backup and
  `pg_checksums` see them as any relation's pages.  What Cloudberry keeps in
  `pg_aoseg`, `pg_aovisimap` and `pg_aoblkdir` is in three tables of
  `gp_ao`'s.  Compression (zlib, zstd, rle_type), column `ENCODING`, the
  columns `ALTER TABLE` adds without a rewrite, UPDATE through the plan's
  old row (O20), unique indexes, BRIN and Cloudberry's bitmap index, VACUUM
  and its compaction, an insert's rows spread over several segment files
  (`gp.appendonly_insert_files` and `..._tuples_range`) and
  `pg_appendonly.segfilecount` as ANALYZE counts it, on one node and on the
  cluster;
- `gp_exttable`: external tables, as foreign tables of `gp_exttable_server`
  -- `file://`, `EXECUTE`, `gpfdist://` and `http://` through libcurl, a
  protocol's own functions, text, CSV and a formatter's custom format,
  writable tables, single-row error handling and its error logs -- read on
  the segments, or on the one node; Cloudberry's protocols and
  `CREATEEXTTABLE`; and `COPY ... LOG ERRORS SEGMENT REJECT LIMIT`;
- `gpfdist`, Cloudberry's file server, built as a program of the port's;
- `diskquota`: Cloudberry's diskquota 2.3, as its library `diskquota-2.3`
  -- a launcher, and a worker for each database that has the extension,
  measure every table on the segments and hold a schema's, a role's and a
  tablespace's quota: the soft limit refuses a statement before it writes,
  and the hard limit stops a load on the segment where it grows past its
  quota, through O21's file events.  For it and for Cloudberry's tests,
  `gp_core` runs a query of `gp_dist_random()` alone that calls a function
  which is not immutable on the segments, with the values its parameters
  and subqueries have on the coordinator, and makes the size functions the
  cluster's, as Cloudberry's are;
- tablespaces, every node's: each node's directory of a tablespace is the
  one of its dbid under the location, as Cloudberry's is, which PostgreSQL
  asks `gp_core` for through O32 -- as a node runs CREATE TABLESPACE, and as
  a mirror or a standby replays it, making a directory of its own on a
  machine it shares with its primary, and removing it again as it runs or
  replays DROP TABLESPACE -- and every node's `pg_tablespace_location()`
  says the location, which `pg_dump` writes;
- `COPY`: Cloudberry's options of `COPY FROM` -- `FILL MISSING FIELDS`,
  `NEWLINE`, and in text an `ESCAPE` of the user's or `OFF` -- through
  `gp_exttable`'s filter, which reads `SEGMENT REJECT LIMIT`'s lines too and
  rejects a row no partition takes; `COPY TO` with such an `ESCAPE`; and an
  error a segment raises in the rows `INSERT`, `CREATE TABLE AS` and `COPY`
  route to it names no `COPY` of the segment's, a `COPY`'s the line of the
  user's data;
- `pax`: Cloudberry's PAX, its store by column, as the table access method
  `pax` of the extension of the same name, which makes in the schema `pax`
  what Cloudberry's initdb made in `pg_ext_aux`.  Its C++ is compiled where
  it stands in `contrib/pax_storage`, as ORCA's is, with the port's copies
  of the files that had to change under `pg19/pax/src`.  A table's files,
  its rows in groups by column, are in a directory beside its relation's,
  removed with it (O22) and marked as PAX's for `pg_checksums` and
  `pg_upgrade` (O23), logged by PAX's resource manager (ID 199), and
  described by the rows of an aux table of its own.  PostgreSQL 19 asks it
  for its options (O14), the columns a scan reads (O15), a unique index's
  probe (O16), its size (O19) and UPDATE's old row (O20), through the
  registry (O13).  A row's number, 24 bits of file and 23 of row, is mapped
  onto TIDs laid out for the table, 20 bits of file by default; statistics
  of each file and group, min/max and bloom filters, skip those a scan's
  conditions rule out; `CLUSTER` orders a table by its cluster columns,
  Z-order or lexical, on every segment; ANALYZE samples it; and the
  coordinator's planner sizes it, and an append-optimized table, as it
  sizes a heap table.

M5's modules are built; what they leave open is in `cloudberry.md`.

M6 — resources and security — is built (2026-09-26), on one more patch of
the core series, O25:

- `gp_resource`: Cloudberry's resource queues and resource groups.  Their
  definitions are a JSON label each on a carrier role, as decided, which
  `pg_resqueue`, `pg_resgroup` and the rest show by Cloudberry's names, and
  a role's queue and group `gp` label keys, which `pg_roles` shows as
  `rolresqueue` and `rolresgroup` (O10); CREATE, ALTER, DROP and COMMENT ON
  RESOURCE QUEUE and RESOURCE GROUP, and a role's RESOURCE clauses, are
  O26's.  A query of a queue takes its slot as ResLockPortal() takes one,
  and waits in shared memory, where the module looks for deadlocks between
  queues and locks itself; a transaction of a group holds one of its slots
  from its first statement to its end, or runs without one where
  Cloudberry's bypass says, and a segment's backend runs in the group the
  dispatch names.  A query runs with its budget — statement_mem, its
  queue's or its group's share — as its work_mem.  The groups' cgroups are
  Cloudberry's code, compiled where it lies (`compat/resgroup/`), v2's on
  the hosts here: CPU limits, weights and cpusets, and I/O limits by
  tablespace.  `pg_resgroup_move_query()` moves a running transaction to
  another group through a signal handler of the module's own, which acts
  where the backend waits on its latch.  Memory protection is Cloudberry's
  vmem tracker, red zone handler and runaway cleaner, compiled where they
  lie (`compat/memprot/`) and told of every memory context's blocks by O25,
  `memory_block_alloc_hook`: a segment's statement or node past its limit
  is refused with Cloudberry's "Out of memory", and the largest session in
  the red zone cancelled;
- `gp_security`: DENY windows, the times a role may not log in, as a key of
  its label; and a client that hangs up, or OAuth's discovery round trip,
  is no failed login under a profile.

M7 — planner parity and tools — has begun (2026-09-26), on one more patch
of the core series, O33, with the three isolation2 tests M4 left for its
tools:

- the coordinator waits for its standby as Cloudberry's does: while the
  standby, connected as `gp_walreceiver`, streams, or has caught up within
  `gp.repl_catchup_within_range` WAL segments, and not otherwise
  (`gp_standby.c`); on a segment the setting says when FTS turns a
  primary's synchronous replication back on;
- Cloudberry's segment administration functions change the cluster's
  nodes while it runs — `gp_add_segment()`, `gp_remove_segment()`,
  `gp_add_segment_mirror()`, `gp_remove_segment_mirror()`,
  `gp_add_master_standby()`, `gp_remove_master_standby()`,
  `gp_update_segment_configuration_mode_status()`, and
  `gp_activate_standby()` for a standby promoted (`gp_segadmin.c`).  A
  call's change is its transaction's: `gp_segment_configuration` shows it
  to the session, a rollback drops it, and the commit writes every change
  at once — the cluster file, `gpsegconfig_dump` and shared memory;
- the coordinator tells the segments a transaction's second phase before
  the transaction ends for the other sessions, as Cloudberry's does, from
  O33, `xact_commit_recorded_hook`, so that no session sees it committed
  while a segment's part is still prepared;
- FTS probes from every coordinator, as Cloudberry's does: a mirror added
  while the coordinator runs is marked up once it streams, and a promoted
  standby probes once `gp_activate_standby()` has made it the coordinator;
- a materialized view's rows are on the segments, as a table's are
  (`gp_refresh.c`): CREATE MATERIALIZED VIEW takes its DISTRIBUTED BY, or
  the key CREATE TABLE AS would choose, and REFRESH -- CONCURRENTLY too --
  fills each segment's copy.  A dynamic table follows; an incremental view
  is refused on a cluster, where its delta maintenance would have to reach
  the segments;
- stock PostGIS on a cluster.  An extension's script runs on every node,
  a query in it each node's own, its tables replicated and the
  coordinator's copy of them emptied (`gp_ddl.c`); the same version of an
  extension on every node, and of the GEOS, PROJ and GDAL PostGIS reports,
  or CREATE and ALTER EXTENSION are refused; ST_Union in one stage, and
  postgis_topology's functions on the coordinator, as the plan decided
  (`gp_sql`'s `extscript.c`); and raster's settings sent to the segments;
- the nodes authenticate each other by certificates, as decision 5 asks
  for production: every connection `gp_core` opens to another node takes
  libpq's TLS options from `gp.internal_sslmode`, `gp.internal_sslcert`,
  `gp.internal_sslkey`, `gp.internal_sslrootcert` and `gp.internal_sslcrl`,
  and a segment takes the node's certificate for any role through a
  `pg_ident.conf` map;
- the coordinator's distributed transaction recovery says when a round has
  reached every node, Cloudberry's "DTM Started", and `gp.dtx_recovered()`
  answers it, which gpstart waits for, as Cloudberry's pg_ctl waits;
- a session's own PREPARE TRANSACTION is refused on a node of a cluster, and
  ALTER SYSTEM is every node's, as in Cloudberry; REFRESH ... CONCURRENTLY
  of a distributed view writes only the rows that changed; CREATE ROLE
  takes PROFILE and ACCOUNT LOCK among its options; and `gp.optimizer_log_fallback`
  logs each statement ORCA would not plan, and why.

What PostgreSQL 19's own pg_dump and pg_dumpall write of a cluster reads
back into another: the port's extensions' schemas, which a superuser may
make; a materialized view with its distribution, filled by REFRESH; a
tag's owner, by name; an incremental view, whose triggers are internal and
made again by its label, and a dynamic table, whose job its label makes
again; and a directory table, given a directory of its own, without its
files, which pg_dump does not carry; an index's tags, which are its table's
label, by the index's name; and a protocol of the user's, which is a label
on each of its functions, made again once the last of them is.  PAX's aux
tables, which are in `pg_ext_aux` now, are not dumped.

Cloudberry's management tools, gpMgmt, are installed beside the server
(`gpMgmt/`, GPHOME the server's prefix), and run on PostgreSQL 19's own
initdb, pg_ctl, pg_basebackup and pg_rewind: gpinitsystem, gpstart,
gpstop, gpstate, gpconfig, gprecoverseg, gpaddmirrors, gpmovemirrors,
gpinitstandby, gpactivatestandby and gpdeletesystem.  gpinitsystem's
`NODE_SSL_DIR` makes a cluster whose nodes authenticate each other by
certificates, and the lines the tools add to a node's `pg_hba.conf` later
follow it (`gppylib/nodetls.py`).  What they ask of Cloudberry's
patched tools that PostgreSQL 19's do not do — pg_basebackup's
`--target-gp-dbid`, `--force-overwrite` and `-E`, pg_rewind's `--slot`, a
connection's `gp_role=utility` — the port's copies do around them
(`gppylib/nodecopy.py`); gpconfig takes a setting by Cloudberry's name and
sets the port's, `gp.*`; and the tools read the cluster from the
coordinator's cluster file and change it through `gp_core`'s segment
administration functions, where Cloudberry's read and write its catalog.
A file the port changes is a copy under `gpMgmt/src`, headed with what it
changes; the rest are installed from Cloudberry's tree as they are.
`gpMgmt/files.txt` lists both, and what is not installed, and why —
gpexpand and gpshrink, since a cluster's segments are fixed when its
coordinator starts; gpcheckcat, since the port keeps Cloudberry's catalogs
as views, labels and files; and the loading, packaging and support tools,
not ported yet.  The suites whose tests run Cloudberry's tools —
`isolation2`, `singlenode_isolation2`, `diskquota` and `greenplum` — run
gpMgmt's.

What Cloudberry's tests asked for next (2026-09-27), in `gp_core`,
`gp_sql`, `gp_orca` and `gp_ao`:

- a statement sent with parameters closes its portal in the same round
  trip, so what its end writes is reported with it; and the INFO lines of a
  one-phase commit and of a rollback name every segment the transaction
  reached, as Cloudberry's do;
- `gp_stat_progress_dtx_recovery`, the distributed transaction recovery's
  phase and counts, and the recovery's commits waiting for a part a
  segment is still finishing;
- PAX's ENCODING clauses, which `gp_ao` keeps and PAX checks and writes by
  (`include/gp_encoding.h`);
- gp_toolkit's views of the cluster, of skew, statistics, bloat and sizes,
  and `gp_param_setting()` by Cloudberry's names; `gp_backend_info()`,
  `gp_opt_version()`, `gp_execution_segment()` and `gp_execution_dbid()`;
- Cloudberry's own log, a CSV file of thirty columns in each node's log
  directory beside PostgreSQL's log, written from `emit_log_hook`
  (`gp_log.c`, `gp.log_format`), and gp_toolkit's views of it: a
  segment's records name the coordinator's statement, which what it is
  sent carries, an error's record is followed by its statement's, and
  `log_min_messages`, `log_min_error_statement` and
  `log_min_duration_statement` reach the segments;
- a record of no declared type carried between the nodes with its row type
  described (`gp_record.c`) — through a Motion, a gather, a query of
  `gp_dist_random()` alone, and as a fragment's parameter or constant;
- a function `EXECUTE ON ALL SEGMENTS` run on every segment — called in
  FROM, and in the SELECT list of a query of no relation — and refused in
  the SELECT list of a query with FROM, as Cloudberry refuses it;
  `pg_proc`'s `prodataaccess` and `proexeclocation`, and `gp.contentid`;
- the UDP interconnect's flow control — capacity, and the loss methods'
  congestion window — and its retry, timer and future-packet settings, by
  Cloudberry's names;
- `agg() OVER (w)` naming the window, as Cloudberry takes it; `GROUP_ID()`;
  and CREATE AGGREGATE's `prefunc` and `repsafe`;
- `pg_stat_last_operation` and `pg_stat_last_shoperation`, which the
  coordinator writes as statements change what they name
  (`gp_metatrack.c`);
- `gp.debug_print_slice_table`; Cloudberry's settings of its planner's
  plans and of the gangs a session keeps accepted, with nothing to apply
  them to here (`gp.eager_two_phase_agg`, `gp.enable_agg_distinct`,
  `gp.enable_sort_limit`, `gp.cost_hashjoin_chainwalk`,
  `gp.cached_segworkers_threshold`); a plan
  of more slices than a segment takes readers for declined by ORCA; a
  publication's, subscription's or event trigger's DROP, RENAME, OWNER TO
  and COMMENT kept on the coordinator, as their CREATE is;
- an error a segment raised carrying the schema, table, column, type and
  constraint it names, and its place in the segment's code, as Cloudberry
  relays it; a table of no columns taking rows on the planner's route; and
  the fault `create_function_fail`;
- and the server built with LDAP, for `pg_hba.conf`'s ldap lines.

The transport and encryption modules — `interconnect`, `udp2`, `gp_tde` —
are still stubs: the streaming transports, tcp and udpifc, live in
`gp_core`, and TDE waits for a formal requirement.

## Tests

`pg19/test/run.sh` runs every suite; `docker compose -f pg19/docker/compose.yml
run --rm tests` runs them in the image built from the branches, and
`... run --rm compare` checks that the patched server still behaves as
vanilla PostgreSQL 19, both built with the tests' patches -- among the
checks, the instructions six workloads take on the two built without
assertions (check 9) -- and `... run --rm meson-vanilla` and
`meson-patched` run PostgreSQL's own tests in each build's tree (check 1).  The suites: the module suites (among them `cluster`,
a coordinator and two segments, and `hooks`, which drives every hook of the
core series through a test module); `greenplum`, Cloudberry's
`greenplum_schedule` on a coordinator and three segments -- every test of it
listed, those that run and those skipped with what stops them, and the
reasons ORCA would not plan a statement in its ORCA pass totalled; `isolation2`, the
tests of Cloudberry's `isolation2_schedule` that bear on M3 — distributed
transactions and snapshots, locks and the global deadlock detector — on
M4, FTS and mirrors, on M6, resource queues and memory accounting, and on
M7's tools, a node recovered elsewhere and the standby promoted and made
again, run by Cloudberry's own driver on the same cluster, with a standby
coordinator for the tests that ask for one, and mirrors for the FTS tests; `fts`, M4's, a coordinator and three primaries
each with a mirror, and what FTS does when a mirror or a primary stops;
`ao`, M5's, append-optimized tables on one node, a standby and recovery;
`diskquota`, M5's too, Cloudberry's diskquota tests on a coordinator and
three segments, its regression schedule as three jobs of its groups and its
isolation2 schedule as one;
`pax`, M5's too, Cloudberry's PAX tests, its `pax_schedule` on a
coordinator and three segments under the planner, as Cloudberry's expected
output has them; `resgroup`, M6's, Cloudberry's resource group schedule
for cgroup v2, whose tests write the cgroups under a parent of each job's
own, `/sys/fs/cgroup/gpdb_<pass>_<group>` -- the tests service is
privileged, and its entrypoint makes them (`test/cgroup.sh`) -- and `memprot`, memory protection's refusals;
`gpmgmt`, M7's, gpMgmt's tools on clusters they make on this host --
gpinitsystem's, a mirror for each primary, and one of primaries alone that
gpaddmirrors gives mirrors, its nodes authenticating each other by
certificates -- each tool checked by what the cluster says after it: a
primary stopped, failed over from and recovered with pg_rewind and with
pg_basebackup, a standby made and made the coordinator, a mirror moved;
`dump`, M7's, a cluster's pg_dumpall read back into another cluster, and
one node's into another node; `dbcopy`, a database copied by either
strategy and moved to another tablespace and back, with PAX tables and
directory tables, on one node and a standby; `postgis_cluster`, M7's, stock PostGIS on a
coordinator and three segments, its answers checked against one node's;
`singlenode` and
`singlenode_isolation2`, Cloudberry's single-node suites with PostgreSQL 19's
own regression tests; and PostGIS's regression suite.  Each is run under the
planner and under ORCA where it plans.

The suites run side by side, as jobs: a suite with two passes is a job a
pass, `pg19/test/jobs` lists how long each job takes so that the longest
start first, and each job's output is printed whole as it finishes, with a
summary of the jobs at the end -- with the CPU time each took, where the
tests service gives the jobs a cgroup each, which weighs as the job is long.
Every server a job makes keeps its data in memory, the tests service's
`/tmp` being a tmpfs, so `pg19/test/jobs` says too how much memory each job
holds (`MEM=`), and a job starts only while the memory it needs is left:
the tests service has a limit of its own, `CB_TESTS_MEM`, 40g unless given,
and a full run takes some 25 GB of it at most, ten jobs at once.
The long suites split themselves further: `greenplum`'s tests over eight
clusters, PostGIS's over eight servers, `isolation2`'s over a cluster a
group of tests, `singlenode`'s Cloudberry half over copies of the server
PostgreSQL's tests ran on, and `diskquota`'s, `pax`'s and `resgroup`'s over
jobs of their groups.  `resgroup`'s test that measures what a group gets of
the CPU runs after the rest, alone, each pass on half the cores: beside the
other jobs, or on fewer cores, the shares it measures missed its bounds.
`JOBS=1` runs one job at a time, `PASSES=planner` only the planner passes,
and `RESULTS_DIR` gets a directory for each job.
