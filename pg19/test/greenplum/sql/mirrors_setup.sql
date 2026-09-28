--
-- A group with mirrors runs tests that list every tablespace on every node
-- (gp_tablespace_with_faults, gp_tablespace): Cloudberry runs them after its
-- parallel_schedule, whose tablespace test has dropped the tablespace
-- test_setup makes, which the port's groups begin with.  So it is dropped
-- here, first in such a group, in each pass.
--
DROP TABLESPACE regress_tblspace;
SELECT count(*) AS tablespaces FROM pg_tablespace WHERE spcname = 'regress_tblspace'
UNION ALL
SELECT count(*) FROM gp_dist_random('pg_tablespace') WHERE spcname = 'regress_tblspace';
