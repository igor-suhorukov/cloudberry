--
-- The port's setup for Cloudberry's tests, which run after it.
--
-- What Cloudberry has built in, the port has as modules, preloaded, and as
-- extensions, whose SQL objects a database has once it creates them: the
-- functions DISTRIBUTED BY and a TAG clause become, and the table a tag is
-- checked against.  PostgreSQL's tests ran before this in a database with
-- none of them, as PostgreSQL's own suite expects to; Cloudberry's run with
-- every M1 module's.
--
CREATE EXTENSION gp_core;
CREATE EXTENSION gp_orca;
CREATE EXTENSION gp_task;
CREATE EXTENSION gp_matview;
CREATE EXTENSION gp_sql;
CREATE EXTENSION gp_security;
SELECT extname FROM pg_extension WHERE extname LIKE 'gp\_%' ORDER BY 1;
