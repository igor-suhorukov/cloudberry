# `pg19/` — Apache Cloudberry as extensions on PostgreSQL 19

This directory holds everything the PostgreSQL 19 port adds. Cloudberry's own
sources stay at their existing paths, so that `git merge upstream/main` keeps
applying to them; this directory is the only new top-level directory.

    compat/      compatibility headers (PG16 -> PG19 shims, field accessors)
    include/     headers the port's own modules share
    modules/     one directory per extension module: build, glue, SQL, control
    docker/      the Compose project: patched PG19, the modules, a cluster
    test/        the port's test harness

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
that may only be preloaded refuse to load any other way.  Two of them carry a
feature rather than a stub:

- `gp_matview` — incrementally maintained materialized views, complete as far
  as one node goes.  A view over one table, over several, or over a table
  joined to itself is maintained by delta, as are `count`, `sum` and `avg`.
  What the delta cannot express — an outer join, `min`, `max`, TRUNCATE — is
  recomputed, which is slower and just as correct.
- `gp_task` — the task scheduler.  `gp_task.create_task()` and friends over
  tables in one database, run on their schedules by a background worker.

The rest are stubs.  The milestones that fill them are in `cloudberry.md`,
"Porting the Cloudberry code in `github/cloudberry`".
