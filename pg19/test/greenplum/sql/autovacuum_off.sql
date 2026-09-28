--
-- Autovacuum off on every node, as disable_autovacuum turns it off before the
-- schedule's append-optimized tests, whose VACUUMs it must not race: the
-- groups those tests run in begin with this (manifest), each on a cluster of
-- its own, where disable_autovacuum runs in its own group.  And in m7a from
-- bfv_partition, which shows the setting, to autovacuum_on, as the schedule
-- has it off until its enable_autovacuum after dispatch -- not before
-- pg_stat, which it has off too, and whose counts of the segments' scans
-- follow what autoanalyze found here.  ALTER SYSTEM reaches every node, as
-- Cloudberry's does.
--
alter system set autovacuum = off;
select gp_segment_id, pg_reload_conf() from gp_id union select gp_segment_id, pg_reload_conf() from gp_dist_random('gp_id');
-- start_ignore 
-- The reason for restarting cbdb here is that if the subsequent 
-- vacuum ao test(uao*_catalog_tables/threshold) encounters an
-- unfinished transaction, it will fail.
--\!gpstop -ari
-- end_ignore