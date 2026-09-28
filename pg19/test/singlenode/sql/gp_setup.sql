--
-- The port's setup for Cloudberry's tests, which run after it.
--
-- What Cloudberry has built in, the port has as modules, preloaded, and as
-- extensions, whose SQL objects a database has once it creates them: the
-- functions DISTRIBUTED BY and CREATE TAG become, among others.  PostgreSQL's
-- tests ran before this in a database with none of them, as PostgreSQL's own
-- suite expects to; Cloudberry's run with every M1 module's, gp_ao's and
-- gp_exttable's, M5's -- the append-optimized tables' access methods and
-- their catalogs, and external tables, which one node reads itself -- and
-- gp_resource's, M6's: resource queues and groups -- and the fault
-- injector, gp_inject_fault, which their tests call as roles of their own.
--
CREATE EXTENSION gp_core;
CREATE EXTENSION gp_orca;
CREATE EXTENSION gp_task;
CREATE EXTENSION gp_matview;
CREATE EXTENSION gp_sql;
CREATE EXTENSION gp_security;
CREATE EXTENSION gp_ao;
CREATE EXTENSION gp_exttable;
CREATE EXTENSION gp_resource;
CREATE EXTENSION gp_inject_fault;
GRANT EXECUTE ON FUNCTION gp_inject_fault(text, text, text, text, text, int4, int4, int4, int4, int4) TO PUBLIC;
SELECT extname FROM pg_extension WHERE extname LIKE 'gp\_%' ORDER BY 1;
--
-- A database the tests make is made from template1, which is given the
-- preloaded modules' extensions too, as gpinitsystem gives a cluster's
-- (CREATE_GPEXTENSIONS): on one node gp_core makes none in a new database,
-- and in one without it ANALYZE is PostgreSQL's -- incremental_analyze's
-- database, whose partitioned tables' statistics are their leaves' merged,
-- and tag's other_db, which reads the tags.  PostgreSQL's tests of the
-- second pass make theirs from it too, and answer as PostgreSQL's; their own
-- database, pg_regress's, is made from template0.
--
\c template1
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS gp_core;
CREATE EXTENSION IF NOT EXISTS gp_orca;
CREATE EXTENSION IF NOT EXISTS gp_task;
CREATE EXTENSION IF NOT EXISTS gp_matview;
CREATE EXTENSION IF NOT EXISTS gp_sql;
CREATE EXTENSION IF NOT EXISTS gp_security;
CREATE EXTENSION IF NOT EXISTS gp_ao;
CREATE EXTENSION IF NOT EXISTS gp_exttable;
CREATE EXTENSION IF NOT EXISTS gp_resource;
RESET client_min_messages;
--
-- Cloudberry's tag test defines its tags here and goes on in database
-- postgres, where CREATE TAG and the rest are gp_sql's too.  The definitions
-- are the cluster's, as Cloudberry's are, whichever database makes them.
-- The second pass finds them made.
--
\c postgres
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS gp_core;
CREATE EXTENSION IF NOT EXISTS gp_sql;
RESET client_min_messages;
