--
-- Stock pgvector on a cluster: the extension made on every node, vectors
-- distributed by another column, the aggregates in two stages, each
-- segment's HNSW and IVFFlat indexes, and pgvector's settings on the
-- segments.  pgvector's own tests run on one node, in the singlenode suite;
-- what this adds is the cluster's.  The answers are one node's, on the same
-- rows -- the planner's route gathers the rows to the coordinator, and ORCA
-- reads each segment's -- and the same in both passes.
--
-- A nearest-neighbour search is the planner's: ORCA gives a query on a table
-- with an HNSW or IVFFlat index to it, as Cloudberry's does, and one ordered
-- by a distance ("ORDER BY with ordering operator").  Its route sends each
-- segment the ORDER BY and the LIMIT with the table's scan (gp_scan.c's
-- bound_nearest()), and each segment's index answers its nearest, as
-- Cloudberry's planner puts a Limit below its Gather Motion; the coordinator
-- sorts what they send, and takes its LIMIT.  A function EXECUTE ON ALL
-- SEGMENTS runs its query on each segment too, where its index answers.
--
CREATE EXTENSION vector;
CREATE SCHEMA pgvector;
SET search_path = pgvector, public;

-- What a query's gather sends the segments after its table and conditions:
-- " ORDER BY ... LIMIT n" for a nearest-neighbour search.
CREATE FUNCTION sent(query text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
  FOR line IN EXECUTE 'EXPLAIN (VERBOSE, COSTS OFF) ' || query LOOP
    IF line ~ 'Remote SQL: ' THEN
      RETURN regexp_replace(line, '^.*Remote SQL: SELECT .* FROM ONLY \S+ ?', '');
    END IF;
  END LOOP;
  RETURN NULL;
END $$;

-- the same version on every segment as on the coordinator, each with the
-- two access methods
SELECT count(*) AS segments FROM gp.exec_on_segments($$SELECT extversion FROM pg_extension WHERE extname = 'vector'$$)
 WHERE result = (SELECT extversion FROM pg_extension WHERE extname = 'vector');
SELECT DISTINCT result AS access_methods
  FROM gp.exec_on_segments($$SELECT string_agg(amname, ',' ORDER BY amname) FROM pg_am WHERE amname IN ('hnsw', 'ivfflat')$$);

--
-- The rows, distributed by id: the first column of a type that hashes.
-- Every vector is one of the points of a small lattice, so the nearest ones
-- tie, and a query orders by id after the distance.
--
CREATE TABLE items (id int, embedding vector(3), half halfvec(3), sparse sparsevec(5), bits bit(4));
INSERT INTO items
SELECT g, ARRAY[g % 7, g % 11, g % 13]::vector, ARRAY[g % 5, g % 3, 1]::halfvec,
       format('{1:%s,5:%s}/5', g % 4, g % 9)::sparsevec, (g % 16)::bit(4)
  FROM generate_series(1, 3000) g;
ANALYZE items;
SELECT policytype, distkey FROM gp_distribution_policy WHERE localoid = 'items'::regclass;
SELECT count(DISTINCT gp_segment_id) AS segments, count(*) FROM items;
-- a table of vectors alone: none of pgvector's types hashes, so its rows go
-- to any segment; and no table is distributed by one
CREATE TABLE vectors_only (v vector(3));
SELECT policytype, distkey::text FROM gp_distribution_policy WHERE localoid = 'vectors_only'::regclass;
CREATE TABLE by_vector (v vector(3)) DISTRIBUTED BY (v);

-- each type as it was written, read back from its segment
SELECT id, embedding, half, sparse, bits FROM items WHERE id IN (1, 4, 1001, 2002, 2999) ORDER BY id;
-- the dimensions a column has are checked where a row is made
INSERT INTO items (id, embedding) VALUES (0, '[1,2]');
INSERT INTO items (id, embedding) SELECT id + 5000, subvector(embedding, 1, 2) FROM items WHERE id <= 3;
INSERT INTO items (id, embedding) VALUES (0, '[1,NaN,2]');

--
-- The operators and functions, on the segments' rows: sums of each, as one
-- node sums them -- rounded, as the rows add up in another order.  The
-- zero vector's cosine distance is NaN, as one node says.
--
SELECT round(sum(embedding <-> '[3,3,3]')::numeric, 4) AS l2,
       round(sum(embedding <#> '[3,3,3]')::numeric, 4) AS negative_inner_product,
       round(sum(embedding <+> '[3,3,3]')::numeric, 4) AS l1,
       round(sum(embedding <=> '[1,2,3]') FILTER (WHERE id % 1001 <> 0)::numeric, 4) AS cosine,
       count(*) FILTER (WHERE (embedding <=> '[1,2,3]') = 'NaN') AS cosine_nan
  FROM items;
SELECT round(sum(l2_distance(half, '[1,1,1]'))::numeric, 4) AS halfvec_l2,
       round(sum(sparse <-> '{2:1}/5')::numeric, 4) AS sparsevec_l2,
       sum(hamming_distance(bits, B'1010')) AS hamming,
       round(sum(jaccard_distance(bits, B'1010'))::numeric, 4) AS jaccard
  FROM items;
SELECT round(sum(vector_norm(embedding))::numeric, 4) AS norms,
       sum(vector_dims(embedding)) AS dims,
       round(sum(vector_norm(l2_normalize(embedding)))::numeric, 4) AS unit_norms,
       count(DISTINCT binary_quantize(embedding)) AS quantized
  FROM items;
-- a filter by distance, which the segments evaluate
SELECT count(*) FROM items WHERE embedding <-> '[3,3,3]' < 4 AND sparse <-> '{1:1}/5' < 3;
-- a join of vectors, which a sort or a loop answers: no hash for them
SELECT count(*) FROM items a JOIN items b ON a.embedding = b.embedding WHERE a.id < 50;

--
-- The aggregates in two stages, as ORCA plans them: each segment's partial
-- state -- avg's, an array of sums -- and the coordinator's combine; and
-- grouped by a column pgvector's functions made, which hashes, the partial
-- states redistributed by it.  The planner's route aggregates the rows it
-- gathers, in one stage.  The halfvec sum over few rows, whose sums a
-- half-precision float holds exactly in any order.
--
SET gp.optimizer_force_multistage_agg = on;
SELECT avg(embedding), sum(embedding), avg(half) FROM items;
SELECT sum(half) FROM items WHERE id <= 300;
SELECT binary_quantize(embedding) AS q, count(*), avg(embedding) FROM items GROUP BY 1 ORDER BY 1;
-- and where no row matches: pgvector 0.8.6's combine function refuses two
-- partial states of no rows -- its own test vector_type says so -- which
-- ORCA's two-stage plan gives it, one from each segment, where one node
-- meets it only in a parallel plan; fixed after 0.8.6 (pgvector's dc60ae7).
-- The planner's route, in one stage, answers.
SET gp.optimizer = on;
SELECT avg(embedding) FROM items WHERE id < 0;
SELECT sum(embedding) FROM items WHERE id < 0;
SET gp.optimizer = off;
SELECT avg(embedding) FROM items WHERE id < 0;
RESET gp.optimizer;
RESET gp.optimizer_force_multistage_agg;

--
-- The nearest, each segment's first: before any index, each segment sorts
-- its own rows, and the answers are exact.
--
SELECT id, embedding, round((embedding <-> '[3,3,3]')::numeric, 4) AS distance
  FROM items ORDER BY embedding <-> '[3,3,3]', id LIMIT 8;
SELECT id, embedding FROM items ORDER BY embedding <=> '[1,2,3]', id LIMIT 5;
SELECT id FROM items ORDER BY half <#> '[1,1,1]', id LIMIT 5;
SELECT id FROM items ORDER BY sparse <+> '{1:3,5:8}/5', id LIMIT 5;
SELECT id FROM items ORDER BY bits <~> B'1010', id LIMIT 5;
-- what the segments are sent: the Sort's keys, each by its operator, and
-- the LIMIT -- an OFFSET's rows too -- after the conditions they evaluate
SELECT sent($$SELECT id FROM items ORDER BY embedding <-> '[3,3,3]', id LIMIT 8$$);
SELECT sent($$SELECT id FROM items WHERE id > 100 ORDER BY sparse <+> '{1:3,5:8}/5' LIMIT 5 OFFSET 2$$);
SELECT sent($$SELECT id FROM items ORDER BY embedding <-> '[3,3,3]' DESC LIMIT 3$$);
-- and none where the first key is no distance, or a condition stays here
SELECT sent($$SELECT id FROM items ORDER BY id LIMIT 3$$);
SELECT sent($$SELECT id FROM items ORDER BY l2_distance(embedding, '[3,3,3]') LIMIT 3$$);
SELECT sent($$SELECT id FROM items WHERE id > (random() * 10)::int ORDER BY embedding <-> '[3,3,3]' LIMIT 3$$);

-- an index of each kind, on every node
CREATE INDEX items_embedding_hnsw ON items USING hnsw (embedding vector_l2_ops);
CREATE INDEX items_embedding_cos ON items USING hnsw (embedding vector_cosine_ops) WITH (m = 8, ef_construction = 32);
CREATE INDEX items_half_hnsw ON items USING hnsw (half halfvec_ip_ops);
CREATE INDEX items_sparse_hnsw ON items USING hnsw (sparse sparsevec_l1_ops);
CREATE INDEX items_bits_hnsw ON items USING hnsw (bits bit_hamming_ops);
CREATE INDEX items_quantized_hnsw ON items USING hnsw ((binary_quantize(embedding)::bit(3)) bit_hamming_ops);
-- the coordinator's copy of the table, which is empty, builds its IVFFlat
-- index from no rows, and says so; the segments build theirs from theirs
CREATE INDEX items_embedding_ivf ON items USING ivfflat (embedding vector_l2_ops) WITH (lists = 4);
CREATE INDEX items_half_ivf ON items USING ivfflat (half halfvec_cosine_ops) WITH (lists = 2);
SELECT result AS valid_indexes, count(*) AS segments
  FROM gp.exec_on_segments($$SELECT count(*) FROM pg_index WHERE indrelid = 'pgvector.items'::regclass AND indisvalid$$)
 GROUP BY 1;
SELECT count(*) AS indexes_with_rows
  FROM gp.exec_on_segments($$SELECT count(*) FROM pg_class
                              WHERE relnamespace = 'pgvector'::regnamespace AND relkind = 'i'
                                AND pg_relation_size(oid) > 8192$$)
 WHERE result = '8';
-- a nearest neighbour for each of a few rows, a subquery for each whose
-- distance is to the outer row's, which stays with the coordinator
SELECT q.id, (SELECT i.id FROM items i WHERE i.id <> q.id ORDER BY i.embedding <-> q.embedding, i.id LIMIT 1) AS nearest
  FROM items q WHERE q.id IN (7, 77, 777) ORDER BY q.id;

--
-- Each segment's HNSW index, answering the coordinator's plan: with as
-- many candidates kept (hnsw.ef_search) as a segment has rows, its scan
-- finds the nearest a sort would, the answer exact; with 3 kept, each
-- segment sends its 3, where a sort would have sent the 10 asked for; and
-- an iterative scan goes on past them.  The distance as a function,
-- l2_distance(), is none an index answers: its sort is the coordinator's.
--
CREATE TABLE many (id int, embedding vector(3)) DISTRIBUTED BY (id);
INSERT INTO many SELECT g, ARRAY[g % 97, g % 89, g % 83]::vector FROM generate_series(1, 2400) g;
CREATE INDEX many_hnsw ON many USING hnsw (embedding vector_l2_ops);
ANALYZE many;
SELECT count(*) < 1000 AS fewer_than_1000 FROM many GROUP BY gp_segment_id;
SET hnsw.ef_search = 1000;
SELECT (SELECT array_agg(id) FROM (SELECT id FROM many ORDER BY embedding <-> '[3,3,3]', id LIMIT 10) s) =
       (SELECT array_agg(id) FROM (SELECT id FROM many ORDER BY l2_distance(embedding, '[3,3,3]'), id LIMIT 10) s)
       AS exact;
SET hnsw.ef_search = 3;
SELECT count(*) FROM (SELECT id FROM many ORDER BY embedding <-> '[3,3,3]' LIMIT 10) s;
SET hnsw.iterative_scan = relaxed_order;
SELECT count(*) FROM (SELECT id FROM many ORDER BY embedding <-> '[3,3,3]' LIMIT 10) s;
RESET hnsw.iterative_scan;
RESET hnsw.ef_search;

--
-- Each segment's index, searched on the segment.  Fewer rows than an HNSW
-- scan keeps candidates (hnsw.ef_search, 40) on each segment, so that its
-- scan finds every row it is asked for, as an exact search does.
--
CREATE TABLE near (id int, embedding vector(3)) DISTRIBUTED BY (id);
INSERT INTO near SELECT g, ARRAY[g, g * 2 % 17, g * 3 % 23]::vector FROM generate_series(1, 60) g;
CREATE INDEX near_hnsw ON near USING hnsw (embedding vector_l2_ops);
SELECT gp_segment_id, count(*) < 40 AS fewer_than_ef_search FROM near GROUP BY 1 ORDER BY 1;
CREATE FUNCTION near_local(q vector, k int) RETURNS SETOF int
  AS $$ SELECT id FROM pgvector.near ORDER BY embedding <-> q LIMIT k $$
  LANGUAGE sql EXECUTE ON ALL SEGMENTS SET enable_seqscan = off;
-- each segment's k nearest, and of them the k nearest of all: the exact
-- answer
SELECT count(*) FROM near_local('[10,10,10]', 5);
SELECT n.id FROM near n JOIN near_local('[10,10,10]', 5) l ON l = n.id
 ORDER BY n.embedding <-> '[10,10,10]', n.id LIMIT 5;
SELECT id FROM near ORDER BY embedding <-> '[10,10,10]', id LIMIT 5;
-- each through its index, whose scan keeps hnsw.ef_search candidates: set
-- on the coordinator, it is the segments' too
SET hnsw.ef_search = 3;
SELECT count(*) FROM near_local('[10,10,10]', 10);
-- and an iterative scan goes on past them
SET hnsw.iterative_scan = relaxed_order;
SELECT count(*) FROM near_local('[10,10,10]', 10);
-- every setting of pgvector's, as the coordinator's session has it
SET hnsw.max_scan_tuples = 5000;
SET hnsw.scan_mem_multiplier = 2;
SET ivfflat.probes = 3;
SET ivfflat.iterative_scan = relaxed_order;
SET ivfflat.max_probes = 7;
SELECT DISTINCT result AS segments_settings FROM gp.exec_on_segments($$
  SELECT concat_ws(' ', current_setting('hnsw.ef_search'), current_setting('hnsw.iterative_scan'),
                   current_setting('hnsw.max_scan_tuples'), current_setting('hnsw.scan_mem_multiplier'),
                   current_setting('ivfflat.probes'), current_setting('ivfflat.iterative_scan'),
                   current_setting('ivfflat.max_probes'))$$);
RESET hnsw.ef_search;
RESET hnsw.iterative_scan;
RESET hnsw.max_scan_tuples;
RESET hnsw.scan_mem_multiplier;
RESET ivfflat.probes;
RESET ivfflat.iterative_scan;
RESET ivfflat.max_probes;
SELECT DISTINCT result AS segments_settings FROM gp.exec_on_segments($$
  SELECT concat_ws(' ', current_setting('hnsw.ef_search'), current_setting('ivfflat.probes'))$$);

-- written on the segments, their indexes follow: rows deleted, moved and
-- added, and each segment's VACUUM of its index
DELETE FROM near WHERE id IN (10, 20);
UPDATE near SET embedding = '[10,10,10]' WHERE id = 30;
INSERT INTO near VALUES (61, '[10,10,11]');
VACUUM near;
SELECT n.id FROM near n JOIN near_local('[10,10,10]', 5) l ON l = n.id
 ORDER BY n.embedding <-> '[10,10,10]', n.id LIMIT 5;
SELECT id FROM near ORDER BY embedding <-> '[10,10,10]', id LIMIT 5;
TRUNCATE near;
SELECT count(*) FROM near_local('[10,10,10]', 5);

-- a replicated table: its index on every segment, its rows read from one
CREATE TABLE near_everywhere (id int, embedding vector(3)) DISTRIBUTED REPLICATED;
INSERT INTO near_everywhere SELECT g, ARRAY[g, g, g]::vector FROM generate_series(1, 20) g;
CREATE INDEX ON near_everywhere USING hnsw (embedding vector_l2_ops);
SELECT id FROM near_everywhere ORDER BY embedding <-> '[3,3,3]', id LIMIT 3;
SELECT DISTINCT result AS rows_on_each_segment FROM gp.exec_on_segments($$SELECT count(*) FROM pgvector.near_everywhere$$);

--
-- COPY, text and binary, through the segments; and ANALYZE's statistics of
-- the vector columns, on the coordinator
--
COPY (SELECT id, embedding, half, sparse, bits FROM items WHERE id <= 3 ORDER BY id) TO STDOUT;
\copy (SELECT id, embedding, half, sparse, bits FROM items) TO 'results/pgvector.bin' WITH (FORMAT binary)
CREATE TABLE items_copy (LIKE items);
\copy items_copy FROM 'results/pgvector.bin' WITH (FORMAT binary)
SELECT count(*) FROM ((TABLE items EXCEPT ALL TABLE items_copy) UNION ALL (TABLE items_copy EXCEPT ALL TABLE items)) d;
SELECT attname, null_frac, avg_width, n_distinct FROM pg_stats
 WHERE schemaname = 'pgvector' AND tablename = 'items' ORDER BY attname;

SET client_min_messages = warning;
DROP SCHEMA pgvector CASCADE;
DROP EXTENSION vector;
RESET client_min_messages;
