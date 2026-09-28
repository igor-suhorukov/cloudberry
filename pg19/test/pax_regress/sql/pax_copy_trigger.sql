--
-- copy2's triggers on PAX tables, in an order that does not vary: the port
-- fetches a row of a PAX table by its TID, so an AFTER INSERT row trigger
-- fires on the row a COPY inserts, where Cloudberry's PAX refused, "not
-- implemented yet on pax relations: TupleFetchRowVersion".  The AFTER
-- trigger updates the table its COPY fills, a statement of a trigger's
-- that writes into its COPY's writer state; each one sees the rows and the
-- updates of the ones before it, as a heap table's statements do.
--
CREATE TEMP TABLE x (a serial, b int, c text not null default 'stuff', d text, e text)
  DISTRIBUTED BY (a);
CREATE FUNCTION fn_x_before () RETURNS TRIGGER AS '
  BEGIN
		NEW.e := ''before trigger fired''::text;
		return NEW;
	END;
' LANGUAGE plpgsql;
CREATE FUNCTION fn_x_after () RETURNS TRIGGER AS '
  BEGIN
		UPDATE x set e=''after trigger fired'' where c=''stuff'';
		return NULL;
	END;
' LANGUAGE plpgsql;
CREATE TRIGGER trg_x_after AFTER INSERT ON x
FOR EACH ROW EXECUTE PROCEDURE fn_x_after();
CREATE TRIGGER trg_x_before BEFORE INSERT ON x
FOR EACH ROW EXECUTE PROCEDURE fn_x_before();
COPY x (a, b, c, d, e) from stdin;
9999	\N	\\N	\NN	\N
10000	21	31	41	51
\.
COPY x (b, d) from stdin;
1	test_1
\.
COPY x (b, d) from stdin;
2	test_2
3	test_3
4	test_4
5	test_5
\.
SELECT * FROM x ORDER BY a;
DROP TABLE x;
DROP FUNCTION fn_x_before();
DROP FUNCTION fn_x_after();

-- each row's trigger updates its row after the rows before it were updated
CREATE TABLE pax_trig_y (a int, e text) DISTRIBUTED BY (a);
CREATE FUNCTION fn_y_after () RETURNS TRIGGER AS $$
BEGIN
	UPDATE pax_trig_y SET e = coalesce(e, '') || 'x' WHERE a = NEW.a;
	RETURN NULL;
END;
$$ LANGUAGE plpgsql;
CREATE TRIGGER trg_y_after AFTER INSERT ON pax_trig_y
FOR EACH ROW EXECUTE PROCEDURE fn_y_after();
INSERT INTO pax_trig_y SELECT g, NULL FROM generate_series(1, 40) g;
COPY pax_trig_y FROM stdin;
41	\N
42	\N
43	\N
44	\N
45	\N
46	\N
47	\N
48	\N
\.
SELECT count(*), count(DISTINCT a), min(e), max(e) FROM pax_trig_y;
DROP TABLE pax_trig_y;
DROP FUNCTION fn_y_after();

-- a trigger's statement in a subtransaction that fails leaves nothing
CREATE TABLE pax_trig_w (a int, e text) DISTRIBUTED BY (a);
CREATE FUNCTION fn_w_after () RETURNS TRIGGER AS $$
BEGIN
	BEGIN
		UPDATE pax_trig_w SET e = 'updated' WHERE a = NEW.a;
		IF NEW.a % 2 = 0 THEN
			RAISE EXCEPTION 'undo';
		END IF;
	EXCEPTION WHEN raise_exception THEN
		NULL;
	END;
	RETURN NULL;
END;
$$ LANGUAGE plpgsql;
CREATE TRIGGER trg_w_after AFTER INSERT ON pax_trig_w
FOR EACH ROW EXECUTE PROCEDURE fn_w_after();
INSERT INTO pax_trig_w SELECT g, 'new' FROM generate_series(1, 10) g;
SELECT * FROM pax_trig_w ORDER BY a;
DROP TABLE pax_trig_w;
DROP FUNCTION fn_w_after();
