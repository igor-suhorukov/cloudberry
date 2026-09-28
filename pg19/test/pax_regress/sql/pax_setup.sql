-- PAX in the tests' database, in template1 for a database a test makes, and
-- in postgres, which a test's own psql reaches (distributed_transactions), as
-- Cloudberry's initdb makes it in every database; the nodes' default table
-- access method is pax (run.sh).
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pax;
RESET client_min_messages;
SHOW default_table_access_method;
\c template1
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pax;
RESET client_min_messages;
\c postgres
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pax;
RESET client_min_messages;
\c regression
