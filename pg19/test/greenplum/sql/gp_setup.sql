--
-- The port's setup for Cloudberry's tests on a cluster, which run after it.
--
-- What Cloudberry has built in, the port has as modules, preloaded on every
-- node, and as extensions, whose objects a database has once it creates
-- them: here, in the coordinator's database, and by DDL dispatch in each
-- segment's.
--
-- The planner plans the setup in either pass: its statements are the
-- extensions' scripts', which ORCA would try one by one, in every group of
-- the suite.  The tests after it are the pass's, in a session of their own.
--
SET gp.optimizer = off;
-- gp_core's own the database has already: a cluster's coordinator makes it
-- in every database a superuser makes (gp_ddl.c, create_core_extension()).
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS gp_core;
RESET client_min_messages;
CREATE EXTENSION gp_orca;
CREATE EXTENSION gp_sql;
CREATE EXTENSION gp_ao;
CREATE EXTENSION gp_exttable;
CREATE EXTENSION gp_security;
CREATE EXTENSION gp_resource;
CREATE EXTENSION gp_inject_fault;
-- faults for everyone, as Cloudberry's script grants them and its tests
-- inject them, some as roles of their own (gp_inject_fault--1.0.sql)
GRANT EXECUTE ON FUNCTION gp_inject_fault(text, text, text, text, text, int4, int4, int4, int4, int4) TO PUBLIC;
SELECT extname FROM pg_extension WHERE extname LIKE 'gp\_%' ORDER BY 1;
SELECT count(*) AS segments FROM gp.segment_configuration() WHERE content >= 0;
--
-- A database a test makes is template1's copy, which has the extensions too:
-- every database of Cloudberry's has what they give.  Not gp_inject_fault,
-- which Cloudberry's template1 has not, and a test makes where it sets a
-- fault (vacuum_ao_aux_only's CREATE EXTENSION gp_inject_fault); and nothing
-- of any of them in schema public, which a test drops in a copy of template1
-- (gp_upgrade_cornercases).
--
\c template1
SET gp.optimizer = off;
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS gp_core;
CREATE EXTENSION IF NOT EXISTS gp_orca;
CREATE EXTENSION IF NOT EXISTS gp_sql;
CREATE EXTENSION IF NOT EXISTS gp_ao;
CREATE EXTENSION IF NOT EXISTS gp_exttable;
CREATE EXTENSION IF NOT EXISTS gp_security;
CREATE EXTENSION IF NOT EXISTS gp_resource;
SELECT e.extname, n.nspname FROM pg_extension e JOIN pg_namespace n ON n.oid = e.extnamespace
 WHERE n.nspname = 'public' ORDER BY 1;
RESET client_min_messages;
\c regression
