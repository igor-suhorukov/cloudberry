/* pg19/modules/gp_sql/gp_sql--1.0.sql */

\echo Use "CREATE EXTENSION gp_sql" to load this file. \quit

/******************************************************************************
 * Tags
 *
 * Cloudberry keeps definitions in the shared catalog pg_tag and assignments in
 * pg_tag_description.  Here the definitions are an ordinary table and the
 * assignments are "gp_tag" security labels -- see tag.c for why.
 *
 * Two things follow from an ordinary table.  Definitions are per database
 * rather than cluster-wide, so a tag has to be defined in each database it is
 * used in; a tag used where it is not defined is refused when it is set, not
 * silently ignored.  And who may change a definition is settled by row-level
 * security rather than by C code: a row belongs to its owner, and only its
 * owner may change it, which is the rule Cloudberry's pg_tag_ownercheck
 * applies.
 *****************************************************************************/

/* Everyone may reach the module's own objects; what they may do with them is
 * settled below, by row-level security and by who owns the object a tag is
 * being put on. */
GRANT USAGE ON SCHEMA gp_sql TO PUBLIC;

CREATE TABLE gp_sql.tag (
	tagname			name PRIMARY KEY,
	tagowner		oid NOT NULL,
	allowed_values	text[]
		CONSTRAINT tag_allowed_values_not_empty
		CHECK (allowed_values IS NULL OR
			   (array_ndims(allowed_values) = 1 AND
				array_length(allowed_values, 1) > 0 AND
				array_position(allowed_values, NULL) IS NULL))
);

COMMENT ON TABLE gp_sql.tag IS
	'the tags this database knows; Cloudberry keeps these in the shared catalog pg_tag';

SELECT pg_catalog.pg_extension_config_dump('gp_sql.tag', '');

ALTER TABLE gp_sql.tag ENABLE ROW LEVEL SECURITY;

CREATE POLICY tag_read ON gp_sql.tag FOR SELECT USING (true);

CREATE POLICY tag_write ON gp_sql.tag FOR ALL
	USING (pg_catalog.pg_has_role(tagowner, 'USAGE'))
	WITH CHECK (pg_catalog.pg_has_role(tagowner, 'USAGE'));

GRANT SELECT, INSERT, UPDATE, DELETE ON gp_sql.tag TO PUBLIC;

/*
 * A security label cannot be put on an index, so the tags of one live here.
 * This is the single place where the port's tags round-trip less well than
 * the rest: they are dumped with this table rather than beside the index.
 */
CREATE TABLE gp_sql.index_tag (
	indexrelid	oid NOT NULL,
	tagname		name NOT NULL,
	tagvalue	text NOT NULL,
	PRIMARY KEY (indexrelid, tagname)
);

COMMENT ON TABLE gp_sql.index_tag IS
	'tags of indexes, which PostgreSQL security labels cannot reach';

SELECT pg_catalog.pg_extension_config_dump('gp_sql.index_tag', '');

ALTER TABLE gp_sql.index_tag ENABLE ROW LEVEL SECURITY;

CREATE POLICY index_tag_read ON gp_sql.index_tag FOR SELECT USING (true);

CREATE POLICY index_tag_write ON gp_sql.index_tag FOR ALL
	USING (pg_catalog.pg_has_role(
			   (SELECT c.relowner FROM pg_catalog.pg_class c
				 WHERE c.oid = indexrelid), 'USAGE')
		   /* the index is already gone: let its rows be cleaned up */
		   OR NOT EXISTS (SELECT 1 FROM pg_catalog.pg_class c
						   WHERE c.oid = indexrelid))
	WITH CHECK (pg_catalog.pg_has_role(
					(SELECT c.relowner FROM pg_catalog.pg_class c
					  WHERE c.oid = indexrelid), 'USAGE'));

GRANT SELECT, INSERT, UPDATE, DELETE ON gp_sql.index_tag TO PUBLIC;

-----------------------------------------------------------------------------
-- Defining a tag: CREATE TAG, ALTER TAG, DROP TAG
--
-- Procedures, because each is what a statement of Cloudberry's becomes: O26
-- writes CREATE TAG as CALL gp_sql.create_tag(...), which answers as a DDL
-- statement does, with a command tag and no row.
-----------------------------------------------------------------------------

/*
 * A tag's allowed values with more added, by Cloudberry's rules
 * (src/backend/commands/tag.c, transformTagValues): each value once, none of
 * more than 256 bytes, and no more than 300 of them; a new value goes after
 * the ones there, in the order given, and the messages are Cloudberry's.
 * NULL, for no list, where there are none.
 */
CREATE FUNCTION gp_sql.add_allowed_values(cur text[], add_values text[])
RETURNS text[]
LANGUAGE plpgsql IMMUTABLE
AS $$
DECLARE
	result text[] := coalesce(cur, '{}'::text[]);
	v	text;
BEGIN
	FOREACH v IN ARRAY coalesce(add_values, '{}'::text[]) LOOP
		IF v = ANY (result) THEN
			RAISE EXCEPTION 'allowed value "%" has been added', v
				USING ERRCODE = 'duplicate_object';
		END IF;
		IF octet_length(v) > 256 THEN
			RAISE EXCEPTION 'added allowed value "%" has exceeded max 256 length', v
				USING ERRCODE = 'program_limit_exceeded';
		END IF;
		result := result || v;
	END LOOP;

	IF cardinality(result) > 300 THEN
		RAISE EXCEPTION 'Allowed_values only allow 300 values.'
			USING ERRCODE = 'program_limit_exceeded';
	END IF;

	RETURN CASE WHEN cardinality(result) = 0 THEN NULL ELSE result END;
END;
$$;

CREATE PROCEDURE gp_sql.create_tag(tagname name,
								   allowed_values text[] DEFAULT NULL,
								   if_not_exists boolean DEFAULT false)
LANGUAGE plpgsql
AS $$
BEGIN
	IF EXISTS (SELECT 1 FROM gp_sql.tag t WHERE t.tagname = create_tag.tagname) THEN
		IF if_not_exists THEN
			RAISE NOTICE 'tag "%" already exists, skipping', tagname;
			RETURN;
		END IF;
		RAISE EXCEPTION 'tag "%" already exists', tagname
			USING ERRCODE = 'duplicate_object';
	END IF;

	INSERT INTO gp_sql.tag (tagname, tagowner, allowed_values)
		 VALUES (tagname,
				 pg_catalog.to_regrole(CURRENT_USER::text)::oid,
				 gp_sql.add_allowed_values(NULL, allowed_values));
END;
$$;

COMMENT ON PROCEDURE gp_sql.create_tag(name, text[], boolean) IS
	'define a tag; what Cloudberry writes as CREATE TAG';

/*
 * ALTER TAG ... ADD/DROP ALLOWED_VALUES and UNSET ALLOWED_VALUES, by
 * Cloudberry's rules: a value added is refused if it is there already, and
 * one dropped if it is not (add_allowed_values says the rest).  This was once
 * a union and a difference, so that repeating either was harmless, and
 * Cloudberry's tag test found the port answering where Cloudberry refuses.
 *
 * Cloudberry refuses to drop a value that some object is tagged with
 * (checkDropTagValue).  The same is done here, over this database: a value in
 * use in another database cannot be seen from here, which is the price of
 * definitions that are not shared.
 *
 * missing_ok is ALTER TAG IF EXISTS, which says so and does nothing.
 */
CREATE PROCEDURE gp_sql.alter_tag(tagname name,
								  add_values text[] DEFAULT NULL,
								  drop_values text[] DEFAULT NULL,
								  unset_values boolean DEFAULT false,
								  missing_ok boolean DEFAULT false)
LANGUAGE plpgsql
AS $$
DECLARE
	cur text[];
	v	text;
BEGIN
	SELECT t.allowed_values INTO cur
	  FROM gp_sql.tag t WHERE t.tagname = alter_tag.tagname;

	IF NOT FOUND THEN
		IF missing_ok THEN
			RAISE NOTICE 'tag "%" does not exist, skipping', tagname;
			RETURN;
		END IF;
		RAISE EXCEPTION 'tag "%" does not exist', tagname
			USING ERRCODE = 'undefined_object';
	END IF;

	IF unset_values THEN
		cur := NULL;
	END IF;

	IF drop_values IS NOT NULL THEN
		FOREACH v IN ARRAY drop_values LOOP
			IF cur IS NULL OR NOT (v = ANY (cur)) THEN
				RAISE EXCEPTION 'allowed value "%" not found', v
					USING ERRCODE = 'undefined_object';
			END IF;
			IF EXISTS (SELECT 1 FROM gp_sql.tag_descriptions d
						WHERE d.tagname = alter_tag.tagname AND d.tagvalue = v) THEN
				RAISE EXCEPTION 'cannot drop tag "%" value "%", which is in use',
					tagname, v
					USING ERRCODE = 'check_violation';
			END IF;
			cur := array_remove(cur, v);
		END LOOP;
		IF cardinality(cur) = 0 THEN
			cur := NULL;
		END IF;
	END IF;

	IF add_values IS NOT NULL THEN
		cur := gp_sql.add_allowed_values(cur, add_values);
	END IF;

	UPDATE gp_sql.tag t SET allowed_values = cur
	 WHERE t.tagname = alter_tag.tagname;
END;
$$;

COMMENT ON PROCEDURE gp_sql.alter_tag(name, text[], text[], boolean, boolean) IS
	'change a tag''s allowed values; what Cloudberry writes as ALTER TAG';

CREATE PROCEDURE gp_sql.rename_tag(tagname name, newname name,
								   missing_ok boolean DEFAULT false)
LANGUAGE plpgsql
AS $$
DECLARE
	moved bigint;
BEGIN
	UPDATE gp_sql.tag t SET tagname = newname WHERE t.tagname = rename_tag.tagname;
	IF NOT FOUND THEN
		IF missing_ok THEN
			RAISE NOTICE 'tag "%" does not exist, skipping', tagname;
			RETURN;
		END IF;
		RAISE EXCEPTION 'tag "%" does not exist', tagname
			USING ERRCODE = 'undefined_object';
	END IF;

	/*
	 * The assignments name the tag rather than pointing at a row, so they
	 * have to be rewritten.  Only this database's are reachable.
	 */
	SELECT count(*) INTO moved FROM gp_sql.tag_descriptions d
	 WHERE d.tagname = rename_tag.tagname;
	IF moved > 0 THEN
		RAISE EXCEPTION 'cannot rename tag "%" while % object(s) carry it',
			tagname, moved
			USING ERRCODE = 'object_in_use',
				  HINT = 'Remove the tag from those objects first.';
	END IF;

	UPDATE gp_sql.index_tag i SET tagname = newname
	 WHERE i.tagname = rename_tag.tagname;
END;
$$;

COMMENT ON PROCEDURE gp_sql.rename_tag(name, name, boolean) IS
	'rename a tag; what Cloudberry writes as ALTER TAG ... RENAME TO';

/*
 * DROP TAG a, b is one statement, so it is one CALL, over all of them: each
 * is dropped or refused in turn, and one refused takes the others back with
 * it, as the statement would.
 */
CREATE PROCEDURE gp_sql.drop_tag(tagnames name[], missing_ok boolean DEFAULT false)
LANGUAGE plpgsql
AS $$
DECLARE
	one name;
	used bigint;
BEGIN
	FOREACH one IN ARRAY tagnames LOOP
		IF NOT EXISTS (SELECT 1 FROM gp_sql.tag t WHERE t.tagname = one) THEN
			IF missing_ok THEN
				RAISE NOTICE 'tag "%" does not exist, skipping', one;
				CONTINUE;
			END IF;
			RAISE EXCEPTION 'tag "%" does not exist', one
				USING ERRCODE = 'undefined_object';
		END IF;

		/*
		 * RESTRICT, always, as in Cloudberry: an assignment names the tag, and
		 * an assignment in another database cannot be reached from here.
		 */
		SELECT count(*) INTO used FROM gp_sql.tag_descriptions d
		 WHERE d.tagname = one;
		IF used > 0 THEN
			RAISE EXCEPTION 'cannot drop tag "%" while % object(s) carry it',
				one, used
				USING ERRCODE = 'dependent_objects_still_exist',
					  HINT = 'Remove the tag from those objects first.';
		END IF;

		DELETE FROM gp_sql.tag t WHERE t.tagname = one;
	END LOOP;
END;
$$;

COMMENT ON PROCEDURE gp_sql.drop_tag(name[], boolean) IS
	'drop tags; what Cloudberry writes as DROP TAG';

-----------------------------------------------------------------------------
-- Reading the tags of an object
-----------------------------------------------------------------------------

CREATE FUNCTION gp_sql.relation_tags(obj regclass) RETURNS jsonb
LANGUAGE sql STABLE STRICT
BEGIN ATOMIC
	SELECT CASE WHEN c.relkind IN ('i', 'I')
				THEN (SELECT jsonb_object_agg(i.tagname, i.tagvalue)
						FROM gp_sql.index_tag i WHERE i.indexrelid = obj)
				ELSE (SELECT s.label::jsonb
						FROM pg_catalog.pg_seclabel s
					   WHERE s.objoid = obj
						 AND s.classoid = 'pg_catalog.pg_class'::regclass
						 AND s.objsubid = 0
						 AND s.provider = 'gp_tag')
		   END
	  FROM pg_catalog.pg_class c WHERE c.oid = obj;
END;

CREATE FUNCTION gp_sql.role_tags(obj regrole) RETURNS jsonb
LANGUAGE sql STABLE STRICT
BEGIN ATOMIC
	SELECT s.label::jsonb FROM pg_catalog.pg_shseclabel s
	 WHERE s.objoid = obj
	   AND s.classoid = 'pg_catalog.pg_authid'::regclass
	   AND s.provider = 'gp_tag';
END;

CREATE FUNCTION gp_sql.schema_tags(obj regnamespace) RETURNS jsonb
LANGUAGE sql STABLE STRICT
BEGIN ATOMIC
	SELECT s.label::jsonb FROM pg_catalog.pg_seclabel s
	 WHERE s.objoid = obj
	   AND s.classoid = 'pg_catalog.pg_namespace'::regclass
	   AND s.objsubid = 0
	   AND s.provider = 'gp_tag';
END;

CREATE FUNCTION gp_sql.database_tags(dbname name) RETURNS jsonb
LANGUAGE sql STABLE STRICT
BEGIN ATOMIC
	SELECT s.label::jsonb FROM pg_catalog.pg_shseclabel s
	 JOIN pg_catalog.pg_database d ON d.oid = s.objoid
	 WHERE d.datname = dbname
	   AND s.classoid = 'pg_catalog.pg_database'::regclass
	   AND s.provider = 'gp_tag';
END;

CREATE FUNCTION gp_sql.tablespace_tags(spcname name) RETURNS jsonb
LANGUAGE sql STABLE STRICT
BEGIN ATOMIC
	SELECT s.label::jsonb FROM pg_catalog.pg_shseclabel s
	 JOIN pg_catalog.pg_tablespace t ON t.oid = s.objoid
	 WHERE t.spcname = tablespace_tags.spcname
	   AND s.classoid = 'pg_catalog.pg_tablespace'::regclass
	   AND s.provider = 'gp_tag';
END;

-----------------------------------------------------------------------------
-- Giving an object a tag, and taking one away
--
-- Each of these builds a SECURITY LABEL statement, so that PostgreSQL makes
-- the same ownership check it would make had the user written one.  The label
-- carries every tag the object has, so setting one reads the others first.
-----------------------------------------------------------------------------

/* Which SECURITY LABEL object type a relation is. */
CREATE FUNCTION gp_sql.relation_label_objtype(obj regclass) RETURNS text
LANGUAGE sql STABLE STRICT
BEGIN ATOMIC
	SELECT CASE c.relkind
			   WHEN 'r' THEN 'TABLE'
			   WHEN 'p' THEN 'TABLE'
			   WHEN 'v' THEN 'VIEW'
			   WHEN 'm' THEN 'MATERIALIZED VIEW'
			   WHEN 'S' THEN 'SEQUENCE'
			   WHEN 'f' THEN 'FOREIGN TABLE'
		   END
	  FROM pg_catalog.pg_class c WHERE c.oid = obj;
END;

CREATE FUNCTION gp_sql.set_relation_tag(obj regclass, tagname name, tagvalue text)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
	kind "char";
	objtype text;
BEGIN
	SELECT c.relkind INTO kind FROM pg_catalog.pg_class c WHERE c.oid = obj;

	IF kind IN ('i', 'I') THEN
		/*
		 * An index carries its tags in a table of ours, so the ownership
		 * check PostgreSQL would have made has to be made here.
		 */
		IF NOT pg_catalog.pg_has_role(
				(SELECT c.relowner FROM pg_catalog.pg_class c WHERE c.oid = obj),
				'USAGE') THEN
			RAISE EXCEPTION 'must be owner of index %', obj::text
				USING ERRCODE = 'insufficient_privilege';
		END IF;
		PERFORM gp_sql.validate_tag(tagname, tagvalue);
		INSERT INTO gp_sql.index_tag AS i (indexrelid, tagname, tagvalue)
			 VALUES (obj, tagname, tagvalue)
		ON CONFLICT (indexrelid, tagname)
		DO UPDATE SET tagvalue = excluded.tagvalue;
		RETURN;
	END IF;

	objtype := gp_sql.relation_label_objtype(obj);
	IF objtype IS NULL THEN
		RAISE EXCEPTION 'tags are not supported for this kind of relation'
			USING ERRCODE = 'wrong_object_type';
	END IF;

	EXECUTE format('SECURITY LABEL FOR gp_tag ON %s %s IS %L',
				   objtype, obj::text,
				   (coalesce(gp_sql.relation_tags(obj), '{}'::jsonb)
					|| jsonb_build_object(tagname, tagvalue))::text);
END;
$$;

COMMENT ON FUNCTION gp_sql.set_relation_tag(regclass, name, text) IS
	'give a table, view, sequence, foreign table or index a tag; Cloudberry writes TAG (name = ''value'')';

CREATE FUNCTION gp_sql.unset_relation_tag(obj regclass, tagname name)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
	kind "char";
	left_over jsonb;
	objtype text;
BEGIN
	SELECT c.relkind INTO kind FROM pg_catalog.pg_class c WHERE c.oid = obj;

	IF kind IN ('i', 'I') THEN
		IF NOT pg_catalog.pg_has_role(
				(SELECT c.relowner FROM pg_catalog.pg_class c WHERE c.oid = obj),
				'USAGE') THEN
			RAISE EXCEPTION 'must be owner of index %', obj::text
				USING ERRCODE = 'insufficient_privilege';
		END IF;
		DELETE FROM gp_sql.index_tag i
		 WHERE i.indexrelid = obj AND i.tagname = unset_relation_tag.tagname;
		RETURN;
	END IF;

	objtype := gp_sql.relation_label_objtype(obj);
	left_over := coalesce(gp_sql.relation_tags(obj), '{}'::jsonb) - tagname::text;

	EXECUTE format('SECURITY LABEL FOR gp_tag ON %s %s IS %s',
				   objtype, obj::text,
				   CASE WHEN left_over = '{}'::jsonb THEN 'NULL'
						ELSE quote_literal(left_over::text) END);
END;
$$;

COMMENT ON FUNCTION gp_sql.unset_relation_tag(regclass, name) IS
	'take a tag off a relation; Cloudberry writes UNSET TAG (name)';

CREATE FUNCTION gp_sql.set_role_tag(obj regrole, tagname name, tagvalue text)
RETURNS void
LANGUAGE plpgsql
AS $$
BEGIN
	EXECUTE format('SECURITY LABEL FOR gp_tag ON ROLE %I IS %L',
				   obj::text,
				   (coalesce(gp_sql.role_tags(obj), '{}'::jsonb)
					|| jsonb_build_object(tagname, tagvalue))::text);
END;
$$;

CREATE FUNCTION gp_sql.unset_role_tag(obj regrole, tagname name)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
	left_over jsonb := coalesce(gp_sql.role_tags(obj), '{}'::jsonb) - tagname::text;
BEGIN
	EXECUTE format('SECURITY LABEL FOR gp_tag ON ROLE %I IS %s',
				   obj::text,
				   CASE WHEN left_over = '{}'::jsonb THEN 'NULL'
						ELSE quote_literal(left_over::text) END);
END;
$$;

CREATE FUNCTION gp_sql.set_schema_tag(obj regnamespace, tagname name, tagvalue text)
RETURNS void
LANGUAGE plpgsql
AS $$
BEGIN
	EXECUTE format('SECURITY LABEL FOR gp_tag ON SCHEMA %s IS %L',
				   obj::text,
				   (coalesce(gp_sql.schema_tags(obj), '{}'::jsonb)
					|| jsonb_build_object(tagname, tagvalue))::text);
END;
$$;

CREATE FUNCTION gp_sql.unset_schema_tag(obj regnamespace, tagname name)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
	left_over jsonb := coalesce(gp_sql.schema_tags(obj), '{}'::jsonb) - tagname::text;
BEGIN
	EXECUTE format('SECURITY LABEL FOR gp_tag ON SCHEMA %s IS %s',
				   obj::text,
				   CASE WHEN left_over = '{}'::jsonb THEN 'NULL'
						ELSE quote_literal(left_over::text) END);
END;
$$;

/*
 * ALTER SCHEMA s TAG (...) and ALTER SCHEMA s UNSET TAG (...), each a whole
 * statement of Cloudberry's, and one CALL of this: PostgreSQL has no ALTER
 * SCHEMA that takes an option, as ALTER DATABASE and ALTER TABLESPACE do, so
 * there is nothing for the tags to be options of.  The label is written with
 * SECURITY LABEL, so the schema has to be the user's.
 */
CREATE PROCEDURE gp_sql.alter_schema_tags(obj regnamespace,
										  set_tags jsonb DEFAULT NULL,
										  unset_tags name[] DEFAULT NULL)
LANGUAGE plpgsql
AS $$
DECLARE
	tags jsonb := coalesce(gp_sql.schema_tags(obj), '{}'::jsonb);
BEGIN
	IF set_tags IS NOT NULL THEN
		tags := tags || set_tags;
	END IF;
	IF unset_tags IS NOT NULL THEN
		tags := tags - unset_tags::text[];
	END IF;

	EXECUTE format('SECURITY LABEL FOR gp_tag ON SCHEMA %s IS %s',
				   obj::text,
				   CASE WHEN tags = '{}'::jsonb THEN 'NULL'
						ELSE quote_literal(tags::text) END);
END;
$$;

COMMENT ON PROCEDURE gp_sql.alter_schema_tags(regnamespace, jsonb, name[]) IS
	'give a schema tags, or take them away; what Cloudberry writes as ALTER SCHEMA ... TAG and UNSET TAG';

CREATE FUNCTION gp_sql.set_database_tag(dbname name, tagname name, tagvalue text)
RETURNS void
LANGUAGE plpgsql
AS $$
BEGIN
	EXECUTE format('SECURITY LABEL FOR gp_tag ON DATABASE %I IS %L',
				   dbname,
				   (coalesce(gp_sql.database_tags(dbname), '{}'::jsonb)
					|| jsonb_build_object(tagname, tagvalue))::text);
END;
$$;

CREATE FUNCTION gp_sql.unset_database_tag(dbname name, tagname name)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
	left_over jsonb := coalesce(gp_sql.database_tags(dbname), '{}'::jsonb) - tagname::text;
BEGIN
	EXECUTE format('SECURITY LABEL FOR gp_tag ON DATABASE %I IS %s',
				   dbname,
				   CASE WHEN left_over = '{}'::jsonb THEN 'NULL'
						ELSE quote_literal(left_over::text) END);
END;
$$;

CREATE FUNCTION gp_sql.set_tablespace_tag(spcname name, tagname name, tagvalue text)
RETURNS void
LANGUAGE plpgsql
AS $$
BEGIN
	EXECUTE format('SECURITY LABEL FOR gp_tag ON TABLESPACE %I IS %L',
				   spcname,
				   (coalesce(gp_sql.tablespace_tags(spcname), '{}'::jsonb)
					|| jsonb_build_object(tagname, tagvalue))::text);
END;
$$;

CREATE FUNCTION gp_sql.unset_tablespace_tag(spcname name, tagname name)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
	left_over jsonb := coalesce(gp_sql.tablespace_tags(spcname), '{}'::jsonb) - tagname::text;
BEGIN
	EXECUTE format('SECURITY LABEL FOR gp_tag ON TABLESPACE %I IS %s',
				   spcname,
				   CASE WHEN left_over = '{}'::jsonb THEN 'NULL'
						ELSE quote_literal(left_over::text) END);
END;
$$;

/*
 * The check the label provider makes, as a function, so that the paths which
 * do not go through SECURITY LABEL answer the same.
 */
CREATE FUNCTION gp_sql.validate_tag(tagname name, tagvalue text)
RETURNS void
AS 'MODULE_PATHNAME', 'gp_sql_validate_tag'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_sql.validate_tag(name, text) IS
	'raise unless this database defines the tag and allows the value';

-----------------------------------------------------------------------------
-- What carries what
-----------------------------------------------------------------------------

/*
 * Every assignment this database can see, whatever it is on.  Cloudberry's
 * five views are restrictions of this one, and keep their names.
 *
 * Labels are joined by object identity rather than by name: pg_seclabels
 * spells an object's name for a human, and two schemas can hold relations
 * that are spelled the same.
 */
CREATE VIEW gp_sql.tag_descriptions AS
	SELECT l.classoid::regclass::text AS objclass,
		   l.objoid,
		   d.key::name AS tagname,
		   d.value #>> '{}' AS tagvalue
	  FROM pg_catalog.pg_seclabel l
	  CROSS JOIN LATERAL jsonb_each(l.label::jsonb) AS d(key, value)
	 WHERE l.provider = 'gp_tag' AND l.objsubid = 0
	UNION ALL
	SELECT l.classoid::regclass::text,
		   l.objoid,
		   d.key::name,
		   d.value #>> '{}'
	  FROM pg_catalog.pg_shseclabel l
	  CROSS JOIN LATERAL jsonb_each(l.label::jsonb) AS d(key, value)
	 WHERE l.provider = 'gp_tag'
	UNION ALL
	SELECT 'pg_class', i.indexrelid, i.tagname, i.tagvalue
	  FROM gp_sql.index_tag i;

COMMENT ON VIEW gp_sql.tag_descriptions IS
	'every tag assignment this database can see; Cloudberry keeps these in pg_tag_description';

GRANT SELECT ON gp_sql.tag_descriptions TO PUBLIC;

CREATE VIEW gp_sql.database_tag_descriptions AS
	SELECT d.datname, t.tagname, t.tagvalue
	  FROM gp_sql.tag_descriptions t
	  JOIN pg_catalog.pg_database d ON d.oid = t.objoid
	 WHERE t.objclass = 'pg_database';

CREATE VIEW gp_sql.user_tag_descriptions AS
	SELECT a.rolname, t.tagname, t.tagvalue
	  FROM gp_sql.tag_descriptions t
	  JOIN pg_catalog.pg_roles a ON a.oid = t.objoid
	 WHERE t.objclass = 'pg_authid';

CREATE VIEW gp_sql.tablespace_tag_descriptions AS
	SELECT s.spcname, t.tagname, t.tagvalue
	  FROM gp_sql.tag_descriptions t
	  JOIN pg_catalog.pg_tablespace s ON s.oid = t.objoid
	 WHERE t.objclass = 'pg_tablespace';

CREATE VIEW gp_sql.schema_tag_descriptions AS
	SELECT n.nspname, t.tagname, t.tagvalue
	  FROM gp_sql.tag_descriptions t
	  JOIN pg_catalog.pg_namespace n ON n.oid = t.objoid
	 WHERE t.objclass = 'pg_namespace';

CREATE VIEW gp_sql.relation_tag_descriptions AS
	SELECT c.relname, n.nspname AS relnamespace, c.relkind, t.tagname, t.tagvalue
	  FROM gp_sql.tag_descriptions t
	  JOIN pg_catalog.pg_class c ON c.oid = t.objoid
	  JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace
	 WHERE t.objclass = 'pg_class';

GRANT SELECT ON gp_sql.database_tag_descriptions,
				gp_sql.user_tag_descriptions,
				gp_sql.tablespace_tag_descriptions,
				gp_sql.schema_tag_descriptions,
				gp_sql.relation_tag_descriptions TO PUBLIC;

/******************************************************************************
 * Directory tables
 *
 * Cloudberry gives a directory table a relkind of its own, a fixed schema and
 * a row in pg_directory_table saying where its files are.  Here it is an
 * ordinary table with the same five columns and a "gp" label holding the
 * location, so the label is both the flag and the value.  Its files live
 * where Cloudberry puts them, in a directory per table inside the database
 * directory.  See dirtable.c.
 *****************************************************************************/

CREATE FUNCTION gp_sql.directory_table_location(dirtable regclass) RETURNS text
AS 'MODULE_PATHNAME', 'gp_sql_dirtable_location'
LANGUAGE C STRICT STABLE;

COMMENT ON FUNCTION gp_sql.directory_table_location(regclass) IS
	'where a directory table keeps its files, or NULL if it is not one';

CREATE FUNCTION gp_sql.claim_directory_table(dirtable regclass) RETURNS text
AS 'MODULE_PATHNAME', 'gp_sql_dirtable_claim'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_sql.claim_directory_table(regclass) IS
	'make an existing table of the right shape into a directory table';

/*
 * A directory table made by a function.  The columns are Cloudberry's
 * GetDirectoryTableSchema, in its order, because the tag column is found by
 * number when DML is checked; CREATE DIRECTORY TABLE writes the same CREATE
 * TABLE, with WITH (gp.directory_table = true), which claims it the same way.
 */
CREATE FUNCTION gp_sql.create_directory_table(dirtable text,
											  tablespace name DEFAULT NULL)
RETURNS regclass
LANGUAGE plpgsql
AS $$
DECLARE
	qname text;
	rel	  regclass;
BEGIN
	/* parse_ident, so that a schema-qualified name is quoted a part at a time */
	SELECT string_agg(pg_catalog.quote_ident(p), '.' ORDER BY ord)
	  INTO qname
	  FROM unnest(pg_catalog.parse_ident(dirtable)) WITH ORDINALITY AS u(p, ord);

	EXECUTE format('CREATE TABLE %s ('
				   '  relative_path text PRIMARY KEY,'
				   '  size bigint,'
				   '  last_modified timestamptz,'
				   '  md5 text,'
				   '  tag text)%s',
				   qname,
				   CASE WHEN tablespace IS NULL THEN ''
						ELSE format(' TABLESPACE %I', tablespace) END);

	rel := qname::regclass;
	PERFORM gp_sql.claim_directory_table(rel);
	RETURN rel;
END;
$$;

COMMENT ON FUNCTION gp_sql.create_directory_table(text, name) IS
	'create a directory table; what Cloudberry writes as CREATE DIRECTORY TABLE';

CREATE FUNCTION gp_sql.directory_table_put(dirtable regclass,
										   relative_path text,
										   content bytea,
										   tag text DEFAULT NULL)
RETURNS bigint
AS 'MODULE_PATHNAME', 'gp_sql_dirtable_put'
LANGUAGE C;

COMMENT ON FUNCTION gp_sql.directory_table_put(regclass, text, bytea, text) IS
	'write a file and the row that describes it; Cloudberry writes COPY ... INTO a directory table';

CREATE FUNCTION gp_sql.directory_table_get(dirtable regclass, relative_path text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'gp_sql_dirtable_get'
LANGUAGE C STRICT STABLE;

COMMENT ON FUNCTION gp_sql.directory_table_get(regclass, text) IS
	'read one file of a directory table; NULL when it is not there';

CREATE FUNCTION gp_sql.remove_file(dirtable regclass, relative_path text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'gp_sql_dirtable_remove'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_sql.remove_file(regclass, text) IS
	'remove a file and its row; the file goes when the transaction commits';

/*
 * Cloudberry's directory_table(regclass), column for column.  The content of
 * every file is read, so it is for looking at a small directory table rather
 * than for walking a large one.
 */
CREATE FUNCTION gp_sql.directory_table(dirtable regclass)
RETURNS TABLE (scoped_file_url text,
			   relative_path text,
			   tag text,
			   size bigint,
			   last_modified timestamptz,
			   md5 text,
			   content bytea)
LANGUAGE plpgsql STABLE
AS $$
DECLARE
	loc text;
	r	record;
BEGIN
	loc := gp_sql.directory_table_location(dirtable);
	IF loc IS NULL THEN
		RAISE EXCEPTION '"%" is not a directory table', dirtable::text
			USING ERRCODE = 'wrong_object_type';
	END IF;

	FOR r IN EXECUTE format('SELECT relative_path, tag, size, last_modified, md5'
							'  FROM %s ORDER BY relative_path', dirtable::text)
	LOOP
		scoped_file_url := loc || '/' || r.relative_path;
		relative_path := r.relative_path;
		tag := r.tag;
		size := r.size;
		last_modified := r.last_modified;
		md5 := r.md5;
		content := gp_sql.directory_table_get(dirtable, r.relative_path);
		RETURN NEXT;
	END LOOP;
END;
$$;

COMMENT ON FUNCTION gp_sql.directory_table(regclass) IS
	'every file of a directory table, with its contents';

CREATE VIEW gp_sql.directory_tables AS
	SELECT n.nspname AS schemaname,
		   c.relname AS tablename,
		   pg_catalog.pg_get_userbyid(c.relowner) AS tableowner,
		   l.location
	  FROM pg_catalog.pg_class c
	  JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace
	  CROSS JOIN LATERAL gp_sql.directory_table_location(c.oid) AS l(location)
	 WHERE c.relkind = 'r' AND l.location IS NOT NULL;

COMMENT ON VIEW gp_sql.directory_tables IS
	'the tables that hold files; Cloudberry keeps these in pg_directory_table';

GRANT SELECT ON gp_sql.directory_tables TO PUBLIC;

/******************************************************************************
 * Storage servers
 *
 * Cloudberry's gp_storage_server and gp_storage_user_mapping have a foreign
 * server's columns and a foreign server's rules about who may change a
 * mapping, so they become ordinary SERVER and USER MAPPING objects of a
 * data-less foreign data wrapper.  Nothing is reimplemented -- the option
 * bookkeeping, the ownership rules, pg_dump and the pg_user_mappings view
 * that hides another user's options all come with them.
 *
 * The wrapper has no handler on purpose: a storage server is somewhere files
 * live, not something to read foreign tables from.
 *****************************************************************************/

CREATE FOREIGN DATA WRAPPER gp_storage;

COMMENT ON FOREIGN DATA WRAPPER gp_storage IS
	'the wrapper Cloudberry''s storage servers become; it reads nothing itself';

CREATE FUNCTION gp_sql.create_storage_server(servername name,
											 options jsonb DEFAULT NULL)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
	opts text;
BEGIN
	/* jsonb keeps no insertion order, so write them in a stable one. */
	SELECT string_agg(format('%I %L', key, value), ', ' ORDER BY key)
	  INTO opts FROM jsonb_each_text(coalesce(options, '{}'::jsonb));

	EXECUTE format('CREATE SERVER %I FOREIGN DATA WRAPPER gp_storage%s',
				   servername,
				   CASE WHEN opts IS NULL THEN '' ELSE ' OPTIONS (' || opts || ')' END);
END;
$$;

COMMENT ON FUNCTION gp_sql.create_storage_server(name, jsonb) IS
	'define a storage server; what Cloudberry writes as CREATE STORAGE SERVER';

CREATE FUNCTION gp_sql.alter_storage_server(servername name,
											set_options jsonb DEFAULT NULL,
											drop_options text[] DEFAULT NULL)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
	have jsonb;
	parts text[] := '{}';
	k	 text;
BEGIN
	SELECT coalesce(jsonb_object_agg(o.k, o.v), '{}'::jsonb) INTO have
	  FROM gp_sql.storage_server_options(servername) AS o(k, v);

	FOR k IN SELECT jsonb_object_keys(coalesce(set_options, '{}'::jsonb)) LOOP
		parts := parts || format('%s %I %L',
								 CASE WHEN have ? k THEN 'SET' ELSE 'ADD' END,
								 k, set_options ->> k);
	END LOOP;

	IF drop_options IS NOT NULL THEN
		FOREACH k IN ARRAY drop_options LOOP
			parts := parts || format('DROP %I', k);
		END LOOP;
	END IF;

	IF array_length(parts, 1) IS NULL THEN
		RETURN;
	END IF;

	EXECUTE format('ALTER SERVER %I OPTIONS (%s)',
				   servername, array_to_string(parts, ', '));
END;
$$;

COMMENT ON FUNCTION gp_sql.alter_storage_server(name, jsonb, text[]) IS
	'change a storage server''s options; what Cloudberry writes as ALTER STORAGE SERVER';

CREATE FUNCTION gp_sql.drop_storage_server(servername name,
										   missing_ok boolean DEFAULT false)
RETURNS void
LANGUAGE plpgsql
AS $$
BEGIN
	EXECUTE format('DROP SERVER %s %I',
				   CASE WHEN missing_ok THEN 'IF EXISTS' ELSE '' END, servername);
END;
$$;

CREATE FUNCTION gp_sql.create_storage_user_mapping(servername name,
												   username name DEFAULT CURRENT_USER,
												   options jsonb DEFAULT NULL)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
	opts text;
BEGIN
	SELECT string_agg(format('%I %L', key, value), ', ' ORDER BY key)
	  INTO opts FROM jsonb_each_text(coalesce(options, '{}'::jsonb));

	EXECUTE format('CREATE USER MAPPING FOR %I SERVER %I%s',
				   username, servername,
				   CASE WHEN opts IS NULL THEN '' ELSE ' OPTIONS (' || opts || ')' END);
END;
$$;

COMMENT ON FUNCTION gp_sql.create_storage_user_mapping(name, name, jsonb) IS
	'what Cloudberry writes as CREATE STORAGE USER MAPPING; credentials stay in pg_user_mapping, which is revoked';

CREATE FUNCTION gp_sql.drop_storage_user_mapping(servername name,
												 username name DEFAULT CURRENT_USER,
												 missing_ok boolean DEFAULT false)
RETURNS void
LANGUAGE plpgsql
AS $$
BEGIN
	EXECUTE format('DROP USER MAPPING %s FOR %I SERVER %I',
				   CASE WHEN missing_ok THEN 'IF EXISTS' ELSE '' END,
				   username, servername);
END;
$$;

/* A storage server's options, for whoever may see them. */
CREATE FUNCTION gp_sql.storage_server_options(servername name)
RETURNS TABLE (key text, value text)
LANGUAGE sql STABLE STRICT
BEGIN ATOMIC
	SELECT pg_catalog.split_part(o, '=', 1),
		   pg_catalog.substr(o, pg_catalog.strpos(o, '=') + 1)
	  FROM pg_catalog.pg_foreign_server s
	  CROSS JOIN LATERAL unnest(coalesce(s.srvoptions, '{}'::text[])) AS u(o)
	 WHERE s.srvname = servername;
END;

CREATE VIEW gp_sql.storage_servers AS
	SELECT s.srvname AS servername,
		   pg_catalog.pg_get_userbyid(s.srvowner) AS serverowner,
		   s.srvoptions AS options
	  FROM pg_catalog.pg_foreign_server s
	  JOIN pg_catalog.pg_foreign_data_wrapper w ON w.oid = s.srvfdw
	 WHERE w.fdwname = 'gp_storage';

COMMENT ON VIEW gp_sql.storage_servers IS
	'the storage servers of this database; Cloudberry keeps these in the shared catalog gp_storage_server';

GRANT SELECT ON gp_sql.storage_servers TO PUBLIC;

/*
 * Mappings, with the options as pg_user_mappings shows them: a user who may
 * not see another user's credentials gets NULL, which is what protects them.
 */
CREATE VIEW gp_sql.storage_user_mappings AS
	SELECT m.srvname AS servername,
		   m.usename AS username,
		   m.umoptions AS options
	  FROM pg_catalog.pg_user_mappings m
	  JOIN pg_catalog.pg_foreign_server s ON s.srvname = m.srvname
	  JOIN pg_catalog.pg_foreign_data_wrapper w ON w.oid = s.srvfdw
	 WHERE w.fdwname = 'gp_storage';

COMMENT ON VIEW gp_sql.storage_user_mappings IS
	'the storage user mappings of this database; Cloudberry keeps these in gp_storage_user_mapping';

GRANT SELECT ON gp_sql.storage_user_mappings TO PUBLIC;

/*
 * Which storage server a tablespace's files go through, which Cloudberry
 * keeps in pg_tablespace.spcfilehandlersrc and spcfilehandlerbin.  It is
 * written as CREATE TABLESPACE ... WITH (gp.server = 's').
 */
CREATE FUNCTION gp_sql.tablespace_storage_server(spcname name) RETURNS text
AS 'MODULE_PATHNAME', 'gp_sql_tablespace_storage_server'
LANGUAGE C STRICT STABLE;

COMMENT ON FUNCTION gp_sql.tablespace_storage_server(name) IS
	'the storage server a tablespace reaches, or NULL for an ordinary local one';

CREATE VIEW gp_sql.storage_tablespaces AS
	SELECT t.spcname AS tablespacename,
		   s.servername
	  FROM pg_catalog.pg_tablespace t
	  CROSS JOIN LATERAL gp_sql.tablespace_storage_server(t.spcname) AS s(servername)
	 WHERE s.servername IS NOT NULL;

GRANT SELECT ON gp_sql.storage_tablespaces TO PUBLIC;

/******************************************************************************
 * Where DISTRIBUTED BY lands
 *
 * O26's grammar rewrites DISTRIBUTED BY (a, b), DISTRIBUTED RANDOMLY and
 * DISTRIBUTED REPLICATED into an option of the statement they are on, which
 * gp_sql takes out and records as this function does.  What reads the policy
 * is ORCA's relcache translator, which asks every relation what it is
 * distributed by, and the dispatch of M2; on one node every table is on the
 * one node, so recording it is all there is to do here.
 *
 * Three shapes, and nothing else is accepted:
 *
 *	   random			 no key; a row may be on any segment
 *	   replicated		 every segment holds every row
 *	   (a,b)			 hashed on those columns
 *
 * The parentheses are what tells a one-column list from a policy word:
 * DISTRIBUTED BY (random) is not DISTRIBUTED RANDOMLY, and before they were
 * there the two recorded the same thing.
 *****************************************************************************/

CREATE FUNCTION gp_sql.set_distribution(rel regclass, policy text)
RETURNS void
AS 'MODULE_PATHNAME', 'gp_sql_set_distribution'
LANGUAGE C;

COMMENT ON FUNCTION gp_sql.set_distribution(regclass, text) IS
	'record what DISTRIBUTED BY said: random, replicated or (a,b); a NULL policy takes it away';

CREATE FUNCTION gp_sql.distribution(rel regclass) RETURNS text
AS 'MODULE_PATHNAME', 'gp_sql_distribution'
LANGUAGE C STRICT STABLE;

COMMENT ON FUNCTION gp_sql.distribution(regclass) IS
	'the distribution policy a table was given, or NULL';

/*
 * What O26's hook would hand to PostgreSQL's parser.  For looking at a
 * rewrite, and for the tests to assert on one.
 */
CREATE FUNCTION gp_sql.desugar(statement text) RETURNS text
AS 'MODULE_PATHNAME', 'gp_sql_desugar'
LANGUAGE C STRICT IMMUTABLE;

COMMENT ON FUNCTION gp_sql.desugar(text) IS
	'Cloudberry''s spelling of a statement, rewritten into PostgreSQL 19''s';
