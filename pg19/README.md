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

Milestone **M1**, in progress.  Every module builds and loads, and the ones
that may only be preloaded refuse to load any other way.  These carry a
feature rather than a stub:

- `gp_matview` — incrementally maintained materialized views and dynamic
  tables, complete as far as one node goes.  A view over one table, over
  several, or over a table joined to itself is maintained by delta, as are
  `count`, `sum` and `avg`; what the delta cannot express — an outer join,
  `min`, `max`, TRUNCATE — is recomputed, which is slower and just as correct.
  A dynamic table is `WITH (gp.dynamic_schedule = '…')` and refreshes itself
  through `gp_task`.
- `gp_task` — the task scheduler.  `CALL gp_task.create_task(…)` and the
  rest, over tables in one database, run on their schedules by a background
  worker.
- `gp_sql` — the Cloudberry-only SQL surface: tags, directory tables and
  storage servers, each built out of something PostgreSQL already has.
- `gp_security` — password profiles, as shared labels on roles, with the live
  state in shared memory and one background worker.
- `gp_core` — the `gp` security label provider the others write through, and
  O26's desugaring, which lets every one of the above be written the way
  Cloudberry writes it.
- `gp_orca` — ORCA is linked in and comes up.  Planning through it is next;
  see `cloudberry.md`, "Next".

The rest are stubs.  The milestones that fill them are in `cloudberry.md`,
"Porting the Cloudberry code in `github/cloudberry`".
