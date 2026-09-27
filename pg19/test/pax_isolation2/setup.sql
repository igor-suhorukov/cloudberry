-- After ../isolation2/setup.sql: the append-optimized tables and PAX, which
-- Cloudberry has in every database, in the tests' database.  The nodes'
-- default table access method is pax (run.sh).
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS gp_ao;
CREATE EXTENSION IF NOT EXISTS pax;
RESET client_min_messages;
