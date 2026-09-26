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
CREATE EXTENSION gp_core;
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
-- every database of Cloudberry's has what they give.
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
CREATE EXTENSION IF NOT EXISTS gp_inject_fault;
GRANT EXECUTE ON FUNCTION gp_inject_fault(text, text, text, text, text, int4, int4, int4, int4, int4) TO PUBLIC;
RESET client_min_messages;
\c regression
