--
-- An insert's rows spread over segment files, as ao_segfile's settings ask:
-- each segment's files and their rows, which ao_segfile's count does not
-- show.  gp.appendonly_insert_files_tuples_range rows go to a file, and the
-- insert turns to the next, begun until there are
-- gp.appendonly_insert_files, then from the first again -- a table by row
-- and by column alike.  A table the transaction made writes one file, as
-- Cloudberry's ShouldUseReservedSegno() says, and a subtransaction that
-- rolls back takes its writers with it.
--
create schema ao_segfile_spread;
set search_path = ao_segfile_spread;
set gp.appendonly_insert_files = 3;
set gp.appendonly_insert_files_tuples_range = 2;
-- one key, so that every row is on one segment
create table sp_row (k int, v int) using ao_row distributed by (k);
create table sp_col (k int, v int) using ao_column distributed by (k);
insert into sp_row select 1, i from generate_series(1, 7) i;
insert into sp_col select 1, i from generate_series(1, 7) i;
select segno, tupcount from gp_toolkit.__gp_aoseg('sp_row') where tupcount > 0 order by segno;
select segno, tupcount from gp_toolkit.__gp_aocsseg('sp_col') where column_num = 0 and tupcount > 0 order by segno;
select count(*), sum(v) from sp_row;
select count(*), sum(v) from sp_col;
-- ANALYZE counts them, the segments' files over the number of segments
analyze sp_row;
select segfilecount from pg_appendonly where relid = 'sp_row'::regclass;
-- a table made in the transaction writes one file
begin;
create table sp_new (k int, v int) using ao_row distributed by (k);
insert into sp_new select 1, i from generate_series(1, 7) i;
select segno, tupcount from gp_toolkit.__gp_aoseg('sp_new') where tupcount > 0 order by segno;
commit;
-- as does CREATE TABLE AS
create table sp_ctas using ao_row as select * from sp_row distributed by (k);
select count(*) from gp_toolkit.__gp_aoseg('sp_ctas') where tupcount > 0;
-- a subtransaction that rolls back takes its files' rows with it
begin;
create table sp_sub (k int, v int) using ao_row distributed by (k);
commit;
begin;
savepoint s;
insert into sp_sub select 1, i from generate_series(1, 5) i;
rollback to s;
insert into sp_sub select 1, i from generate_series(10, 12) i;
commit;
select count(*), sum(v) from sp_sub;
select sum(tupcount) from gp_toolkit.__gp_aoseg('sp_sub');
reset gp.appendonly_insert_files;
reset gp.appendonly_insert_files_tuples_range;
-- start_ignore
drop schema ao_segfile_spread cascade;
-- end_ignore
