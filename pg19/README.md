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
is under way** (2026-09-23): two-phase commit, distributed snapshots and the
global deadlock detector.

On one node (M1):

- `gp_matview` — incrementally maintained materialized views and dynamic
  tables.  A view over one table, over several, or over a table joined to
  itself is maintained by delta, as are `count`, `sum` and `avg`; what the
  delta cannot express — an outer join, `min`, `max`, TRUNCATE — is
  recomputed.  A dynamic table refreshes itself through `gp_task`.
- `gp_task` — the task scheduler, run by a background worker.
- `gp_sql` — the Cloudberry-only SQL surface: tags, directory tables and
  storage servers, and Cloudberry's spelling of statements through O26 —
  classic partition clauses, `DISTRIBUTED BY`, `DECODE`, `gp_dist_random('t')`.
- `gp_security` — password profiles.
- `gp_orca` — ORCA plans on one node, with the fallback counters.

On a cluster (M2), `gp_core` and `gp_orca`:

- the nodes, read from a file (`gp.cluster_config`); the dispatcher, an
  ordinary libpq client authenticated with SCRAM, whose statements run in the
  coordinator's transaction, savepoints included;
- DDL on every node with the coordinator's OIDs (R1), distribution policies
  hashed by Cloudberry's cdbhash, ANALYZE sampling the segments (O3), CREATE
  TABLE AS and ALTER TABLE ... SET DISTRIBUTED BY;
- DISTRIBUTED BY checked as Cloudberry checks it — its columns, a column's
  operator class, the table's unique constraints and indexes, inheritance —
  in Cloudberry's words, and Cloudberry's legacy hash, the `cdbhash_*_ops`
  classes, which `gp.use_legacy_hashops` gives a new key;
- what a segment says — a trigger's NOTICE — reaching the client, and
  Cloudberry's rules for triggers and for the names it reserves;
- ORCA's distributed plans — the five Motions, Split, direct dispatch, the
  slice table — carried out by gp_core, each slice sent the values of the
  parameters it reads; and PostgreSQL's own plans gathering from the
  segments where ORCA does not plan, writing a distributed table through an
  Explicit Redistribute Motion — each row changed on its segment by its ctid
  there, a row whose key changes moved by a Split, RETURNING evaluated on
  the coordinator, a replicated table's row found on every segment by what
  it holds;
- every segment has each table's distribution policy, the `gp` label the
  coordinator writes;
- Cloudberry's settings of the dispatcher and the planner, as `gp.*`, among
  them direct dispatch's INFO lines and autostats;
- **every slice of a query at once**: the writer, the session's backend on a
  segment, runs one slice, and readers — more backends of the session there,
  reading as a part of the writer's transaction through the shared snapshot
  (R2 and R4) — run the others, each sender streaming its rows to its
  receivers over a Unix socket or a TCP port.  The earlier relay through the
  coordinator is kept for what cannot stream — a temporary table, the
  coordinator's own slice feeding a reader's — and on request
  (`gp.interconnect_type = relay`);
- `gp_segment_id`, as a call of the row's segment (O10);
- Cloudberry's catalogs by their names, in `pg_catalog`: `gp_id`,
  `gp_segment_configuration` over the cluster file, `gp_configuration_history`,
  and `gp_distribution_policy` over the labels, which a write to it — with
  `allow_system_table_mods`, as Cloudberry's is written — writes;
- partial tables, spread over the first so many segments, which
  `gp_debug_numsegments` (Cloudberry's extension, carried by `gp_sql`) and
  `gp_distribution_policy.numsegments` make, and which ORCA leaves to the
  planner, as Cloudberry's does.

What M2 leaves open: a query whose key is fixed to a few values goes to one
segment or to all of them, not to those few; and an UPDATE that moves a row
fires the row triggers of a DELETE and an INSERT on the segments, where
Cloudberry's Split fires none.

Distributed transactions (M3), in `gp_core`:

- **two-phase commit**: a transaction that wrote on a segment is prepared on
  each segment that wrote, under the coordinator's own transaction ID, whose
  commit record decides it; a process on the coordinator finishes, by that
  record, whatever a failure left prepared.  A segment needs
  `max_prepared_transactions` above zero;
- **distributed snapshots**: each statement is sent the coordinator's
  snapshot of it, and a segment makes its own agree — it waits for a
  transaction the snapshot says committed and it holds only prepared, and
  hides one the snapshot says in progress that it has committed, holding
  back with the replication slot `gp_dtx_horizon` what such a transaction
  deleted;
- **the global deadlock detector** (`gp.enable_global_deadlock_detector`):
  without it an UPDATE or DELETE of a distributed table locks the table, as
  Cloudberry's does; with it rows are locked, and a process on the
  coordinator gathers every node's waits, reduces the graph with
  Cloudberry's own detector (`src/backend/utils/gdd/gdddetector.c`, compiled
  where it lies) and cancels the youngest transaction of a cycle;
- Cloudberry's fault injector, `gp_inject_fault`, for the tests.

The storage, resource and transport modules — `gp_ao`, `pax`, `gp_exttable`,
`gp_resource`, `gp_tde`, `interconnect`, `udp2` — are still stubs: M5 and M6
fill the first five, and the streaming transport lives in `gp_core` for now.

## Tests

`pg19/test/run.sh` runs every suite; `docker compose -f pg19/docker/compose.yml
run --rm tests` runs them in the image built from the branches, and
`... run --rm compare` checks that the patched server still behaves as
vanilla PostgreSQL 19.  The suites: the module suites (among them `cluster`,
a coordinator and two segments, and `hooks`, which drives every hook of the
core series through a test module); `greenplum`, part of Cloudberry's
`greenplum_schedule` on a coordinator and three segments; `singlenode` and
`singlenode_isolation2`, Cloudberry's single-node suites with PostgreSQL 19's
own regression tests; and PostGIS's regression suite.  Each is run under the
planner and under ORCA where it plans.
