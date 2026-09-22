--
-- The port's setup for Cloudberry's tests on a cluster, which run after it.
--
-- What Cloudberry has built in, the port has as modules, preloaded on every
-- node, and as extensions, whose objects a database has once it creates
-- them: here, in the coordinator's database, and by DDL dispatch in each
-- segment's.
--
CREATE EXTENSION gp_core;
CREATE EXTENSION gp_orca;
CREATE EXTENSION gp_sql;
SELECT extname FROM pg_extension WHERE extname LIKE 'gp\_%' ORDER BY 1;
SELECT count(*) AS segments FROM gp.segment_configuration() WHERE content >= 0;
