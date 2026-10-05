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
    release/     the binary release: its packages and their smoke test
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

The modules build only against PostgreSQL 19 with the core patch series,
and run only in that server: configured against any other `pg_config`, the
build stops with an error saying so (`-Drequire_patched_pg=false`
configures anyway).

### The patched PostgreSQL 19

The series is 22 commits on `REL_19_STABLE`, branch
[`REL_19_STABLE_CLOUDBERRY`](https://github.com/igor-suhorukov/postgres/tree/REL_19_STABLE_CLOUDBERRY)
of `igor-suhorukov/postgres`, and it builds as any PostgreSQL 19 does. The
options below are those of `docker/Dockerfile.pg`, the build the port is
tested against; any prefix will do, and `$HOME/pg19` needs no root. On
Debian or Ubuntu:

    sudo apt-get install build-essential meson ninja-build pkg-config bison flex \
        perl python3 python3-dev git gettext libreadline-dev zlib1g-dev \
        libicu-dev libssl-dev liblz4-dev libzstd-dev libxml2-dev libxslt1-dev \
        libldap-dev

    git clone --depth 1 --branch REL_19_STABLE_CLOUDBERRY \
        https://github.com/igor-suhorukov/postgres.git
    cd postgres
    meson setup build --prefix=$HOME/pg19 --buildtype=debugoptimized \
        -Dcassert=true -Dinjection_points=true \
        -Dssl=openssl -Dicu=enabled -Dlz4=enabled -Dzstd=enabled \
        -Dplpython=enabled -Dldap=enabled
    ninja -C build
    ninja -C build install
    export PATH=$HOME/pg19/bin:$PATH

Some of these options serve testing rather than running. `-Dcassert=true`
turns on the server's assertions, which are what catch a hook that breaks
an invariant, and ORCA's own debug checks follow them (the port's
`-Dorca_debug`); measurements are taken with `-Dcassert=false`.
`-Dinjection_points=true` lets `gp_inject_fault()` reach the faults
Cloudberry's tests set inside PostgreSQL's own code, and PL/Python and LDAP
serve the tests that write functions in `plpython3u` or read `ldap` lines
of `pg_hba.conf`. A server built without them runs the modules; only those
tests stop.

The port's own tests want more injection points than PostgreSQL has, at
Cloudberry's faults in replication, vacuum, two-phase commit, a spill and
elsewhere. They are not part of the series: to run those tests, apply them
before `meson setup`.

    for p in /path/to/cloudberry/pg19/docker/patches/*.patch; do
        patch -p1 < "$p"
    done

### The modules

They take the server's packages and these besides: xerces-c for ORCA,
protocol buffers for PAX and `gp_stats_collector`, liburing for PAX,
libcurl for external tables, PXF, S3 and `gpfts`, APR and libevent for
`gpfdist` and libyaml for its transforms, jansson for `gpfts`, and libuv
for the interconnect's proxy.

    sudo apt-get install libxerces-c-dev protobuf-compiler libprotobuf-dev \
        liburing-dev libcurl4-openssl-dev libapr1-dev libevent-dev \
        libyaml-dev libjansson-dev libuv1-dev

Of this tree's git submodules the build uses two, both PAX's and both
optional: tabulate, which its dump functions (`dump_pax_file_desc()` and
the rest) print through, and googletest, which its unit tests, the
test-only module `pax_gtest`, are written with.

    git clone --depth 1 --branch extension_postgresql_19 \
        https://github.com/igor-suhorukov/cloudberry.git
    cd cloudberry
    git submodule update --init contrib/pax_storage/src/cpp/contrib/tabulate \
        contrib/pax_storage/src/cpp/contrib/googletest
    cd pg19
    meson setup build -Dpg_config=$HOME/pg19/bin/pg_config
    ninja -C build
    ninja -C build install
    PG_BINDIR=$HOME/pg19/bin test/load/run.sh

The modules install into that server's own directories, gpMgmt's tools
beside its programs, and find its libpq through their run path, so nothing
needs `LD_LIBRARY_PATH`. `test/load/run.sh` then starts servers with them
and checks that every module loads, and that the ones which may only be
preloaded refuse to load any other way.

A part whose libraries are missing is left out rather than failing the
build: `gpfdist` without APR or libevent, `gpfts` without libcurl or
jansson, the proxy without libuv, and PXF and the `http://` and
`gpfdist://` locations of external tables without libcurl. ORCA, PAX and
gpcloud fail the build without theirs, and so does `gp_stats_collector`,
which has no switch, without protocol buffers; `-Dorca=false`,
`-Dpax=false` and `-Dgpcloud=false` leave the first three out. gpMgmt's
tools run on Python 3 with PyGreSQL and psutil (`python3-pygresql`,
`python3-psutil`).

### In Docker

Or, without installing anything on the host:

    docker compose -f pg19/docker/compose.yml build
    docker compose -f pg19/docker/compose.yml run --rm tests load

Compose builds the patched server itself, with `docker/Dockerfile.pg`, from
a clone of the fork, made as above, that lies beside this repository's
checkout (`../postgres`); `PG_SRC=/path/to/postgres` points it elsewhere.

### Binary releases

The fork's GitHub releases carry the server and the modules built, for
Debian 13 and Ubuntu 24.04 on amd64 and arm64, as three packages, each as a
`.deb` and as a `.tar.gz` of the same files, installed in `/usr/local/pgsql`:

    cloudberry-pg19-postgresql   the patched PostgreSQL 19
    cloudberry-pg19-extension    the modules, and gpMgmt beside the server
    cloudberry-pg19-dbgsym       the debug information of both

    sudo apt install ./cloudberry-pg19-postgresql_<version>-debian13_amd64.deb \
                     ./cloudberry-pg19-extension_<version>-debian13_amd64.deb

They are built to run, not to be tested: the server without assertions or
injection points, and without the test-only modules (`-Dhook_tests=false`),
so the test suites run on the Docker build above instead. Their options are
otherwise that build's, named rather than detected, so that a distribution
which lacks a library fails the build instead of releasing a server without
it; and the libraries are in `/usr/local/pgsql/lib` on every architecture.

`.github/workflows/pg19-release.yml` makes a release of what a tag
`pg19-<version>` points at, and PostgreSQL's
`REL_19_STABLE_CLOUDBERRY` as it is then; a version with a `-`, or a
PostgreSQL that is still a beta, makes a pre-release:

    git tag pg19-0.1.0 && git push origin pg19-0.1.0

`docker/Dockerfile.release` builds the packages, `release/package.sh`
packs them, and `release/smoke.sh` installs them on a clean system of the
distribution and runs `test/load/run.sh` on them before anything is
released. A push to the port's branch that changes these builds and
tests them without releasing them (a run by hand would too, but GitHub
offers one only for a workflow on the default branch, and `main` mirrors
apache/cloudberry). The same build on the host, for one distribution
(`RELEASE_BASE`, Debian 13 unless it says otherwise):

    docker compose -f pg19/docker/compose.yml --profile release build release
    docker run --rm -u "$(id -u)" -v "$PWD/dist:/out" cloudberry/pg19-release:latest cp -r /dist/. /out/
    docker run --rm -v "$PWD/dist:/dist:ro" -v "$PWD/pg19:/pg19:ro" debian:trixie-slim bash /pg19/release/smoke.sh

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
  recomputed.  A dynamic table refreshes itself through `gp_task`, by a
  job of Cloudberry's reserved name that runs Cloudberry's REFRESH DYNAMIC
  TABLE where `gp_sql` is preloaded, and Cloudberry's `pg_dynamic_tables`
  and `pg_get_dynamic_table_schedule()` read them.  A
  query is answered from a materialized view that holds what it asks,
  where that costs less (`gp.enable_answer_query_using_materialized_views`,
  Cloudberry's AQUMV), under ORCA too unless `gp.aqumv_under_orca` is off:
  from a view that is up to date, or incremental.  Which views are, from
  what was done to their base tables since each REFRESH, is kept as
  Cloudberry's `gp_matview_aux` and `gp_matview_tables` show it, and a
  REFRESH of a view that is up to date does nothing.
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
  on a cluster each file and its row on the segment its relative path hashes
  to, as Cloudberry keeps them, a file following its row through
  savepoints and two-phase commit, `gp_sql.directory_table_sweep()` for
  what another backend's second phase or a crash leaves, and Cloudberry's
  `COPY BINARY t FROM ... 'path'` and `COPY BINARY DIRECTORY TABLE t 'path'
  TO ...`;
  and Cloudberry's spelling of statements through O26 — classic partition
  clauses, `DISTRIBUTED BY`, `DECODE`, `gp_dist_random('t')`; a classic
  partitioned table's SUBPARTITION TEMPLATEs as Cloudberry's
  `gp_partition_template` shows them, `pg_get_expr(template, relid)`
  printing each as Cloudberry's does.
- `gp_security` — password profiles.
- `gp_orca` — ORCA plans on one node, with the fallback counters.

On a cluster (M2), `gp_core` and `gp_orca`:

- the nodes, read from a file (`gp.cluster_config`); the dispatcher, an
  ordinary libpq client authenticated with SCRAM, whose statements run in the
  coordinator's transaction, savepoints included; a gang's connections made
  all at once, as Cloudberry's are, under `gp.segment_connect_timeout` --
  Cloudberry's gp_segment_connect_timeout and its default, 180 s, where the
  port waited for ever -- a segment in recovery tried again
  `gp.gang_creation_retry_count` times, a segment process saying it is one
  as it starts (`gp.qe_details`), and what fails said in Cloudberry's words;
- each session's id, `gp.session_id`, a number of the coordinator's counter
  taken as the client connects, as Cloudberry's gp_session_id is -- clients
  that connect one after another have ids one after another, which a
  replicated table's parallel retrieve cursor picks its segment by -- and a
  new one once the gang the session had its part on is lost, as Cloudberry's
  session takes one (resetSessionForPrimaryGangLoss()): what is left of the
  old one on the segments, a retrieve session bound to it among them, is no
  part of the new one's.  On one node a backend's id is its process ID, as
  it was: nothing there says which session another backend works for but
  its process;
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
  the other tables, those of an EXISTS or an IN the WHERE clause ANDs in
  too, whose semi-join returns the row each matched, where the target is
  not held against a re-check; MERGE, ORCA planning its join and a MERGE
  ModifyTable over it on one node, with RETURNING, WHEN NOT MATCHED BY
  SOURCE and a partitioned target, a result relation for each partition,
  the explicit write over it on a cluster (`orca/merge.c`); a CTE's
  producer read in other slices run when its slice ends without it, as
  Cloudberry's squelch runs it, and a Sequence that prints its producers
  first; and
  PostgreSQL's own plans gathering from the
  segments where ORCA does not plan, a NOT IN of a distributed table made
  an anti-join with its NULLs conditions beside it
  (`modules/gp_core/gp_subselect.c`), a nested loop's inner gather keyed
  by the join's equality where hash joins are off -- gathered once, each
  outer row given the rows of its key (`modules/gp_core/gp_scan.c`),
  writing a distributed table through an
  Explicit Redistribute Motion — each row changed on its segment by its ctid
  there, a row whose key changes moved by a Split that fires no trigger,
  RETURNING (old and new too) and a view's WITH CHECK OPTION and a table's
  policies evaluated on the coordinator, RETURNING's ctid the row's on its
  segment, MERGE, WHERE CURRENT OF and ON
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
  them direct dispatch's INFO lines and autostats, `gp.max_plan_size`, and
  `gp.print_create_gang_time`'s INFO lines of a gang's connections; a SET of
  the client's, outside a transaction block, told the segments as it runs, as
  Cloudberry dispatches one, so that a value a segment refuses fails the SET
  itself; DISCARD TEMP on every node, in the statement's transaction; and
  `gp.allow_segment_dml`, with which a function a segment runs in its share
  of a plan may write there, as Cloudberry's allow_segment_DML lets one;
- SERIALIZABLE, on a cluster, is REPEATABLE READ, as in Cloudberry and
  Greenplum, and as the plan's Track C drops it (the DTM's effort table,
  "Drop": "SERIALIZABLE (as in Greenplum)"): each node's serializable
  snapshot isolation sees none of another's rows, so a conflict split over
  two segments would commit; one node keeps PostgreSQL's;
- EXPLAIN's `slicetable` and `locus` options, Cloudberry's; **EXPLAIN
  ANALYZE of what the segments ran**, which each segment measures and
  sends the coordinator as an INFO of gp_core's as its part ends: the
  segment with the most rows' figures for a fragment's nodes, the WAL of a
  write's statements, each slice's memory and Vmem reserved, a node's
  Executor Memory, work_mem and spilling segments, and allstat
  (`modules/gp_core/gp_explain.c`); and
  **query metrics** (`gp.enable_query_metrics`): each plan node's
  instrumentation in a slot of shared memory on every node, whose process,
  session and statement it says, which Cloudberry's `gp_instrument_shmem`
  library reads (`modules/gp_core/gp_metrics.c`);
- Cloudberry's **runtime filters** (`gp.enable_runtime_filter`,
  `gp.enable_runtime_filter_pushdown`): a Bloom filter of a hash join's inner
  keys, in a RuntimeFilter node above the outer side of the planner's joins,
  or below a node grouping it by the join's keys, and pushed down into the
  gathers and sequential scans below a join's outer side, through such a
  node too, those of ORCA's slices on the segments among them, which test a
  row as they read it, before their own conditions; a gather sends its
  segments each key's range with its SQL, and a PAX scan is begun with the
  range as its keys, which PAX's sparse filter skips files by
  (`modules/gp_core/gp_rtfilter.c`);
- **every slice of a query at once**: the writer, the session's backend on a
  segment, runs one slice, and readers — more backends of the session there,
  reading as a part of the writer's transaction through the shared snapshot
  (R2 and R4), members of its lock group, which plan no parallel workers
  whatever a function sets — run the others, each sender streaming its rows to its
  receivers over a Unix socket or a TCP port, or, with
  `gp.interconnect_type = udpifc`, in UDP packets each receiver acknowledges,
  with Cloudberry's flow control, retransmission and deadlock check, a row
  as a tuple -- the transports of a table of gp_core's, to which the `udp2`
  module adds Cloudberry's UDP2, its C++ core compiled where it lies, and
  the `interconnect` module Cloudberry's ic-proxy, built where libuv is: a
  background worker on each node carrying every pair of nodes' Motions over
  one connection, by `gp.interconnect_proxy_addresses`; a sorted Gather into one segment merges its senders' streams
  as they come, and the coordinator's gathers ask each segment for one row
  first and ten times as many each batch after, so that a LIMIT above stops
  them soon; a slice whose plan stops reading a Motion -- a hash join whose
  hash table is empty on a segment, a nested loop over an empty table --
  stops its senders there and then, over every transport, as Cloudberry's
  squelch does.  The earlier relay through the coordinator carries the slices
  that run on the coordinator or have to run in the writer — the
  coordinator's own that reads a function's rows or makes its own, a VALUES
  list, one that scans a temporary table — first, and the rest stream, the
  coordinator's own that works on the rows it receives among them, on a
  segment; it carries all of them on request (`gp.interconnect_type =
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

What M2 leaves open under ORCA: a data-modifying statement in WITH, and a
subquery in RETURNING; a MERGE's RETURNING on a cluster, and a MERGE into a
view or a replicated or coordinator's table; an UPDATE or DELETE whose
re-check would copy a row whole -- of a subquery or a function read beside
the target, or a MERGE's source that is not tables, on one node -- or read
a NOT IN's or a NOT EXISTS's table again; and a CTE read in several slices
one of which gp_core relays: they stay the planner's.  The planner's route
sends a partitioned table's UPDATE and DELETE that read only it to the
segments whole, as a plain table's; an index build keeps a distributed
table's pages and rows on the coordinator, as Cloudberry's coordinator
never writes them.

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
  Cloudberry's brings them back, and an empty table's one page; each
  segment's `pg_class` has the pages and rows ANALYZE sampled of it, as
  Cloudberry's segments write them, which the segment's VACUUM keeps where it
  scans too little to count, and an index's pages after ANALYZE are its
  files' on the segments;
- ANALYZE of a partitioned table as Cloudberry's takes it, on one node too:
  the root and the mid-levels as `gp.optimizer_analyze_root_partition` and
  `_midlevel_partition` say, ANALYZE ROOTPARTITION, the leaves before the
  table above them and that table after a partition whose siblings all have
  statistics; and an inheritance tree sampled on the segments in one
  dispatch, as Cloudberry's is;
- the root's statistics merged from its leaves' as Cloudberry merges them,
  each leaf keeping a HyperLogLog counter of each column
  (`gp_hyperloglog_estimator`, `gp_hyperloglog_accum()`), ANALYZE FULLSCAN's
  of every row;
- Cloudberry's fault injector, `gp_inject_fault`, for the tests: its faults
  at the port's own places under Cloudberry's names, and at PostgreSQL 19's
  injection points, among them O29's in PostgreSQL's commit; a fault's error
  Cloudberry's ERRCODE_FAULT_INJECT, XX009, which a test's PL/pgSQL handler
  catches, `when fault_inject` -- a condition PostgreSQL 19's PL/pgSQL does
  not know, which the harness spells `when sqlstate 'XX009'` -- and no fault
  firing in the fault injector's own connection to a node;
- Cloudberry's `debug_dtm_action` settings, `gp.debug_dtm_action*`: a
  segment fails the protocol command -- PREPARE, COMMIT PREPARED, a
  subtransaction's begin, release or rollback -- or the SQL command they
  name, with Cloudberry's error, which the coordinator answers as
  Cloudberry's does: a second phase retried over a new connection, in its
  words; a function's block's failed rollback escaping its handler; and
  `gp.debug_abort_after_distributed_prepared`.

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
waits for its mirror whatever cancels it: `gp_core` holds the cancel off
the wait itself (`gp_fts.c`).  A directory table's files
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
of the core series, O13 to O21, O23 and O32 (since 2026-09-28 O19 and O20,
as PAX's O22, done in the modules, and O23 pg_checksums' reader alone):

- `gp_ao`: append-optimized tables, by row (`ao_row`) and by column
  (`ao_column`), as table access methods whose blocks are 8K pages of the
  table's own relation, through the buffer manager and logged by `gp_ao`'s
  resource manager (ID 200), so that a standby, a base backup and
  `pg_checksums` see them as any relation's pages.  What Cloudberry keeps in
  `pg_aoseg`, `pg_aovisimap` and `pg_aoblkdir` is in three tables of
  `gp_ao`'s.  Compression (zlib, zstd, rle_type), column `ENCODING`, the
  columns `ALTER TABLE` adds without a rewrite, UPDATE, which fetches each
  old row by its TID, a block read and decoded once for the rows it changes
  in it (O20, a core patch until 2026-09-28, gave it the old row from the
  plan), unique indexes, BRIN and Cloudberry's bitmap index, VACUUM
  and its compaction, an insert's rows spread over several segment files
  (`gp.appendonly_insert_files` and `..._tuples_range`) and
  `pg_appendonly.segfilecount` as ANALYZE counts it, on one node and on the
  cluster; and unlogged ones, which a crash empties, with their rows in
  unlogged twins of `gp_ao`'s tables, as Cloudberry's have unlogged aux
  tables;
- `gp_exttable`: external tables, as foreign tables of `gp_exttable_server`
  -- `file://`, `EXECUTE`, `gpfdist://` and `http://` through libcurl, a
  protocol's own functions, text, CSV and a formatter's custom format,
  writable tables, single-row error handling and its error logs -- read on
  the segments, or on the one node; a temporary one (`CREATE EXTERNAL TEMP
  TABLE`), the foreign table in the session's `pg_temp`; Cloudberry's
  protocols and `CREATEEXTTABLE`; and `COPY ... LOG ERRORS SEGMENT REJECT
  LIMIT`;
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
- `pg_hint_plan`: Cloudberry's fork of pg_hint_plan, 1.3.9, whose hints
  reach both planners -- ORCA's through gp_orca's `plan_hint_hook` -- with
  PostgreSQL 19's join search copied for it (`core.c`, made by
  `gen_core.py`) and the enable_* settings a hint sets copied into each
  relation's `pgs_mask`; a session LOADs it, as Cloudberry's tests do; its
  hint table, `hint_plan.hints` under `pg_hint_plan.enable_hint_table`,
  replicated on a cluster, whose hints reach ORCA as the planner hook found
  them -- Cloudberry's ORCA finds them again, a query of the table, which
  failed ORCA's planning of every statement -- and a query planned while
  ORCA plans another the planner's;
- tablespaces, every node's: each node's directory of a tablespace is the
  one of its dbid under the location, as Cloudberry's is, which PostgreSQL
  asks `gp_core` for through O32 -- as a node runs CREATE TABLESPACE, and as
  a mirror or a standby replays it, making a directory of its own on a
  machine it shares with its primary, and removing it again as it runs or
  replays DROP TABLESPACE -- and every node's `pg_tablespace_location()`
  says the location, which `pg_dump` writes; `WITH (contentN = ...)` a
  location of a content's own.  CREATE DATABASE, CREATE and DROP TABLESPACE
  and ALTER DATABASE ... SET TABLESPACE run inside the coordinator's
  distributed transaction, each segment's part prepared with it, as
  Cloudberry's two-phase DDL is: the directories they make or remove follow
  the transaction's end, even after a restart or on a mirror promoted
  between the phases, through a file of each prepared part's that the
  mirror replays (`gp_dirxact.c`, logged by `gp_core`'s resource manager).
  ALTER TABLE ... SET TABLESPACE of a partitioned table moves its
  partitions too, as Cloudberry's does, where PostgreSQL 19 moves none and
  sets only the default for new ones, which ALTER TABLE ONLY still does;
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
  removed with it (O21's unlink event) and marked as PAX's for
  `pg_checksums` (O23, the mark `gp_core`'s to write), logged by PAX's
  resource manager (ID 199), and
  described by the rows of an aux table of its own.  PostgreSQL 19 asks it
  for its options (O14), the columns a scan reads (O15) and a unique index's
  probe (O16), through the registry (O13); `gp_core` measures its tables by
  its `relation_size`, which it registers; and UPDATE fetches the old row by
  its TID.  A row's number, 24 bits of file and 23 of row, is mapped onto
  TIDs laid out for the table, 20 bits of file by default; statistics of
  each file and group, min/max and bloom filters, skip those a scan's
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
- segments added to the running cluster after the last, and the last
  removed, as gpexpand and gpshrink do (M8, `gp_expand.c`): each session
  takes the new number of segments as its next transaction first asks for
  it, with a new gang, unless it has a temporary table, and a segment
  process computes with its coordinator's number, which comes with its
  identity; room for `gp.max_segments` of them is made as the coordinator
  starts; every table is first given the number it is on in its label
  (`gp.expand_pin_numsegments()`), and gpexpand's catalog lock,
  `gp_expand_lock_catalog()`, which every statement that changes a catalog
  takes shared, keeps the catalogs from changing while a segment is copied;
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
  fills each segment's copy.  A dynamic table follows; so does an
  incremental view, which the coordinator keeps up to date once each
  statement is over, from the transition tables the segments' triggers kept:
  it computes the deltas and sends each segment those of its own rows of the
  view, which the view is distributed by -- its GROUP BY columns, or every
  segment for one of a single row (`gp_matview`'s `ivm_cluster.c`).  The
  coordinator keeps which views are up to date, and answers a query from
  one; a write through a partitioned table, whose rows the segments route,
  marks the views of every partition;
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
gpinitstandby, gpactivatestandby, gpdeletesystem, and gpexpand and
gpshrink (M8), whose new segment is a copy of the coordinator made a
segment's, with a cluster file of its own.  gpinitsystem's
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
`gpMgmt/files.txt` lists both.  Every one of Cloudberry's tools is installed:
gpexpand and gpshrink, which add a segment and its mirror to the running
cluster and take them away again (M8), and the rest too: gpcheckcat, its
checks reading each segment's catalog rows through
`gp_internal.segment_query()` and checking what the port keeps in place of
Cloudberry's catalogs — the `gp` labels, gp_ao's segment files, PAX's aux
tables, directory tables' files — against PostgreSQL 19's catalog, its
foreign keys made at build time from `system_fk_info.h`; gppkg, installing
debs on a Debian host; gpload, analyzedb (counting AO and PAX tables'
changes by gp_ao's and PAX's own), gpsd, minirepro, gplogfilter,
gpmemwatcher, gpmemreport, gpcheckperf with gpnetbench and stream,
gpreload, gpdemo and gpdirtableload.  The suites whose tests run
Cloudberry's tools —
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
- the rest of gp_toolkit: `gp_disk_free`, each segment's own; the checks
  for orphaned and missing files and `gp_move_orphaned_files()`, each node
  locking its `pg_class` and checkpointing for itself; an append-optimized
  table's segment files' history (`__gp_aoseg_history`); and the functions
  of a partitioned table, `gp_partitions` among them (`gp_toolkit.c`,
  `gp_partmaint.c`);
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
- Cloudberry's workfile limits, in its words: `gp.workfile_limit_per_query`
  as a run's `temp_file_limit`, and a statement's files and a node's bytes
  counted where they lie as a run ends; gp_toolkit's four workfile views of
  the same files; and a segment's cancel in its QE's words
  (`gp_workfile.c`);
- **parallelism within a segment** (`gp.enable_parallel`, off by default as
  Cloudberry's is; PostgreSQL's `max_parallel_workers_per_gather` and
  parallel costs, sent to the segments): the segment's writer runs a Gather
  of PostgreSQL's in what it runs for the coordinator, whole at its first
  FETCH, its workers in the writer's lock group — a gather's query of the
  planner's route that the coordinator reads to its end, which the segment
  plans with parallel plans allowed, and ORCA's fragments of the writer,
  which the ORCA module gives Gathers where PostgreSQL's costs say they pay:
  over a large sequential scan, a hash join whose outer side's scan the
  participants share and whose inner side each hashes, or an aggregate in
  three stages (`orca/parallel.c`); a reader's slice runs without them, a
  member of a lock group leading none of its own;
  EXPLAIN ANALYZE says "Workers Launched"; `max_worker_processes` is the
  cluster's to size (`modules/gp_core/gp_parallel.c`);
- and the server built with LDAP, for `pg_hba.conf`'s ldap lines.

M8 (2026-09-27) adds `gpfts`, the coordinator's automatic failover
(`bin/gpfts`): Cloudberry's external FTS program, for the coordinator alone,
the segments staying `gp_core`'s FTS's.  Its instances elect a leader
through an etcd lease, as Cloudberry's do, with Cloudberry's etcd client
compiled where it lies; the leader probes the coordinator over SQL, keeps in
etcd the cluster as the coordinator shows it and whether its standby
streams, and when the coordinator stops promotes the standby with
`pg_promote()`, makes it the coordinator with `gp_activate_standby()` and
waits for its distributed transaction recovery — as gpactivatestandby now
waits too.  The nodes' states the coordinator's FTS finds reach its standby
in WAL, a record of `gp_core`'s resource manager the coordinator waits for
its standby to have (`gp_cluster.c`), so that a standby promoted dispatches
to the primaries the old coordinator last had.

M8 brings two of Cloudberry's extensions too, each a module of its
own whose files are compiled where they lie, but for the port's copies of
the ones PostgreSQL 19 or the port changed (`modules/<name>/src`):
`datalake_fdw`, the Iceberg table access method and its catalog and volume
foreign-data wrappers, on `ProcessUtility_hook` and `object_access_hook`,
preloaded after gp_core and gp_sql -- a lake table distributed randomly, as
Cloudberry's is, so that its scans and writes are the segments', and its
metadata engine told by the coordinator, or by a node on its own; and
`gp_stats_collector`, each query's life -- submitted, started, ended, done,
failed, cancelled -- on every node, sent to an agent's Unix socket as
protobuf messages or written into its log table, from the executor's
hooks, `ProcessUtility_hook`, `emit_log_hook`, the transactions' aborts,
and gp_resource's queue, which tells it of a query that fails while it
waits (Cloudberry's `query_info_collect_hook`, `include/gp_query_info.h`);
a plan node's own events, which PostgreSQL 19 calls no hook for, are not
sent.  The suites of the same names run their tests on a coordinator and
three segments and on one node.

And two more of Cloudberry's extensions: `pxf_fdw`, Cloudberry's foreign-data wrapper
of a PXF server, and `gpcloud`, its `s3://` protocol of external tables, with
`gpcheckcloud`, both built against `gp_exttable`'s headers (`access/external.h`
and `access/url.h` among them), whose scan -- its single row error handling
too -- and writer `pxf_fdw` reads and writes its server's data with; and a
foreign table read where its `mpp_execute` says, as Cloudberry reads one --
`'all segments'` on the segments, each its share, over `num_segments` of them
-- the option kept among the object's, and from its wrapper's validator
(`gp_core`'s `gp_foreign.c`).

Parallel retrieve cursors (M8), in `gp_core`, `gp_sql` and `gp_orca`
(`modules/gp_core/gp_endpoint.c`): `DECLARE ... PARALLEL RETRIEVE CURSOR`,
whose top slice runs on a reader of each segment it would be gathered from
and puts its rows into an endpoint there, a shm_mq tuple queue, as
Cloudberry's does; one whose rows meet on the coordinator -- ORDER BY, an
aggregate of the whole, a catalog, a function -- has its endpoint there,
filled as DECLARE runs its plan, where Cloudberry runs it in an entry-db
process, which could not dispatch here.  A retrieve session reads an
endpoint with `RETRIEVE { ALL | n } FROM ENDPOINT`, logged in to its node
with the cursor's token -- Cloudberry's `gp_retrieve_conn` and the token as
a password, asked for once `pg_hba.conf`'s method has let the user in, or
the port's `gp.retrieve_token` -- and runs nothing else; and
`gp_get_endpoints()`, `gp_get_segment_endpoints()`,
`gp_get_session_endpoints()`, their views, and
`gp_wait_parallel_retrieve_cursor()`.  One difference of the port's: ORCA
plans every parallel retrieve cursor, whatever `gp.optimizer` says, since
Cloudberry's are its planner's, whose plans have slices, and the port's
planner of slices is ORCA; a cursor ORCA declines has its endpoint on the
coordinator.

Stock pgvector, 0.8.6 unpatched (`docker/Dockerfile.cbext`), runs on one
node and on a cluster: CREATE EXTENSION on every node; its vectors
distributed by another column, since none of its types hashes; its HNSW
and IVFFlat indexes built on every segment; and its settings, `hnsw.*` and
`ivfflat.*`, sent to the segments (`gp_dispatch.c`).  A nearest-neighbour
search -- a LIMIT over an ORDER BY whose first key is a distance, an
operator an index answers nearest first, pgvector's or GiST's -- sends the
segments its ORDER BY and its LIMIT with the table's scan (`gp_scan.c`'s
`bound_nearest()`): each segment's index finds its own nearest, and the
coordinator sorts the few rows they send and takes its LIMIT, as
Cloudberry's planner puts a Limit below its Gather Motion.  ORCA gives
such a query to the planner, and a query on a table with an HNSW or IVFFlat
index, as Cloudberry's does; a function EXECUTE ON ALL SEGMENTS searches
each segment's index too.  What an index build says of the rows it read is
the segments', which have them (`gp_ddl.c`'s `builds_indexes()`): the
coordinator holds back its NOTICEs as it builds over its copy, which is
empty -- IVFFlat's "created with little data" of no rows -- and the
segments' NOTICEs of a CREATE INDEX, REINDEX, REPACK, CLUSTER or VACUUM
FULL are the client's, each distinct one once.

The encryption module, `gp_tde`, is still a stub: TDE waits for a formal
requirement.  The `ic` suite runs every transport over the same Motion
statements in every run; `ic_greenplum` and `ic_isolation2` run the two
suites' ORCA passes again over udp2 and the proxy on request
(`CB_IC=udp2|proxy|all`).

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
reasons ORCA would not plan a statement in its ORCA pass totalled, and the
port's test of stock pgvector on the cluster; `isolation2`, the
tests of Cloudberry's `isolation2_schedule` that bear on M3 — distributed
transactions and snapshots, locks and the global deadlock detector — on
M4, FTS and mirrors, on M6, resource queues and memory accounting, and on
M7's tools, a node recovered elsewhere and the standby promoted and made
again, and its `parallel_retrieve_cursor_schedule`, M8's parallel retrieve
cursors and their retrieve sessions, run by Cloudberry's own driver on the
same cluster, with a standby
coordinator for the tests that ask for one, and mirrors for the FTS tests; `fts`, M4's, a coordinator and three primaries
each with a mirror, and what FTS does when a mirror or a primary stops;
`gpfts`, M8's, a coordinator with a standby, three pairs, an etcd and two
gpfts instances — one leads, the other takes over when it dies — and the
coordinator stopped, its standby promoted, serving the cluster;
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
pg_basebackup, a segment and its mirror added by gpexpand and taken away by
gpshrink, a standby made and made the coordinator, a mirror moved; and on a
third cluster the rest of the tools (M8): gpcheckcat finding what one
segment alone was given, analyzedb, gpload, gplogfilter, gpmemwatcher and
gpmemreport, gpcheckperf, gpreload, gppkg, gpdirtableload and gpdemo;
`expandshrink`, M8's, Cloudberry's `isolation2_expandshrink_schedule`, as
its CI runs it, a job of its own on a cluster of its own with mirrors;
`dump`, M7's, a cluster's pg_dumpall read back into another cluster, and
one node's into another node, a directory table's files carried by copying
each segment's directory to the segment of the same content;
`dirtable`, directory tables on one node, its standby and a crash, and on a
coordinator and two primaries with mirrors, through two-phase commit, a
restart between its phases, a failover and a storage server; `dbcopy`, a database copied by either
strategy and moved to another tablespace and back, with PAX tables and
directory tables, on one node and a standby; `postgis_cluster`, M7's, stock PostGIS on a
coordinator and three segments, its answers checked against one node's;
`tpc`, on request only, TPC-H's 22 queries and TPC-DS's 99 at scale factor
1 on a coordinator and four segments, their data, queries and a reference
answer to each from DuckDB (pinned, in the image), each query planned by
ORCA and answering as DuckDB answers it -- `CB_TPC=check` in the tests
service -- and each timed under ORCA and under the planner's route, on the
port built without assertions -- the compose file's `tpc` service,
`CB_TPC=time` -- and with as many parallel workers on each segment as
`TPC_WORKERS` lists, `"0 2 4"` for the speedups;
`singlenode` and
`singlenode_isolation2`, Cloudberry's single-node suites with PostgreSQL 19's
own regression tests, and in `singlenode` pgvector's, stock, against its own
expected output; PostGIS's regression suite; `pxf_fdw`, M8's,
Cloudberry's `pxf_fdw` tests and a stand-in for PXF, on one node and on a
cluster; and `gpcloud`, gpcloud's unit tests, `gpcheckcloud` and its
regression schedule against an S3 of the suite's own, moto's server, on a
cluster and on one node.  Each is run under the
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
