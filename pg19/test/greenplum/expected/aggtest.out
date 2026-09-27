--
-- aggtest, which PostgreSQL's aggregates test makes from data/agg.data and
-- leaves for the schedule after it, where gp_aggregates reads it: its four
-- rows, in the group gp_aggregates runs in.
--
CREATE TABLE aggtest (a int2, b float4);
INSERT INTO aggtest VALUES (56, 7.8), (100, 99.097), (0, 0.09561), (42, 324.78);
ANALYZE aggtest;
