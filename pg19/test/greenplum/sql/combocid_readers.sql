--
-- What combocid_gp is for, counted rather than listed: its cursor's MOVE
-- 9900 passes over the rows a redistributed join gives first, which come in
-- no fixed order (manifest).  A query whose slices run at once reads, in
-- the segments' readers, what its writer's transaction wrote before the
-- statement and not after: a reader adopts the writer's transaction
-- (XactAdoptTransactionState(), R4), and finds each combo command ID it
-- meets -- the writer's, of a row the transaction wrote and then updated --
-- in what the writer published (R2's combocid_miss_hook): gp_share.c.
-- combocid_gp asks besides of a combo command ID the writer makes after a
-- reader started, which a reader here does not meet: it runs its slice to
-- the end at once, the slice that receives its rows keeping them, so that
-- the cursor's later FETCHes give what was read as it was declared.
--
-- A table of its own, where combocid_gp's is temporary: a slice that scans
-- a temporary table runs in the writer, whose buffers hold it.  ORCA's plan
-- of the join redistributes its rows, in readers; the planner's reads each
-- table through a gather of its own, on the writers, and joins on the
-- coordinator, so that its pass has none -- which readers_as_planned says.
--
create schema combocid_readers;
set search_path = combocid_readers;

-- The session's readers on the segments, as the coordinator holds them:
-- asked of the coordinator alone, where a segment's pg_stat_activity would
-- answer a transaction once, as it first read it.
create function readers() returns bigint as $$
  select count(*) from gp_backend_info() where type = 'r'
$$ language sql;

-- What a cursor has left to give: its rows, those the transaction's later
-- UPDATE wrote, which it must not give, and those it gives more than once.
create function cursor_rest(c refcursor, out total int, out updated2 int, out twice int) as $$
declare
  r record;
  seen bool[] := array_fill(false, array[10000]);
begin
  total := 0; updated2 := 0; twice := 0;
  loop
    fetch c into r;
    exit when not found;
    total := total + 1;
    if r.t = 'updated2' then
      updated2 := updated2 + 1;
    end if;
    if seen[r.i] then
      twice := twice + 1;
    end if;
    seen[r.i] := true;
  end loop;
end;
$$ language plpgsql;

create table manycombocids (i int, t text, distkey int) distributed by (distkey);
create index on manycombocids (i);
select readers() as readers_before;

begin;
-- filled and updated in the transaction: every row's command IDs, and
-- every combo command ID, its own
insert into manycombocids select g, 'initially inserted', 1 from generate_series(1, 10000) g;
analyze manycombocids;
do $$
begin
  set local enable_seqscan = off;
  for j in 1..10 loop
    update manycombocids set t = 'updated1' where i = j;
  end loop;
end;
$$;

-- the cursor's readers read the rows and the ten updated ones, each old
-- version's combo command ID the writer's
declare c cursor for select a.i, b.i as bi, a.t from manycombocids a, manycombocids b where a.i = b.i and a.distkey = 1;
move 1 from c;
select (readers() > 0) = current_setting('gp.optimizer')::bool as readers_as_planned;

-- more combo command IDs, made after the cursor was declared, whose rows it
-- does not give
do $$
begin
  set local enable_seqscan = off;
  for j in 1..1000 loop
    update manycombocids set t = 'updated2' where i = j * 10;
  end loop;
end;
$$;
select * from cursor_rest('c');

-- a statement after the UPDATE reads what it wrote, in its readers too,
-- each of its combo command IDs found in what the writer published
select count(*), sum((a.t = 'updated2')::int) as updated2
  from manycombocids a, manycombocids b where a.i = b.i and a.distkey = 1;
rollback;

drop schema combocid_readers cascade;
