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
-----------------------------------------------------------------------------

CREATE FUNCTION gp_sql.create_tag(tagname name,
								  allowed_values text[] DEFAULT NULL,
								  if_not_exists boolean DEFAULT false)
RETURNS void
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
				 allowed_values);
END;
$$;

COMMENT ON FUNCTION gp_sql.create_tag(name, text[], boolean) IS
	'define a tag; what Cloudberry writes as CREATE TAG';

/*
 * ALTER TAG ... ADD/DROP ALLOWED_VALUES and UNSET ALLOWED_VALUES.  Adding is
 * a union and dropping a difference, so that repeating either is harmless.
 *
 * Cloudberry refuses to drop a value that some object is tagged with
 * (checkDropTagValue).  The same is done here, over this database: a value in
 * use in another database cannot be seen from here, which is the price of
 * definitions that are not shared.
 */
CREATE FUNCTION gp_sql.alter_tag(tagname name,
								 add_values text[] DEFAULT NULL,
								 drop_values text[] DEFAULT NULL,
								 unset_values boolean DEFAULT false)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
	cur text[];
	v	text;
BEGIN
	SELECT t.allowed_values INTO cur
	  FROM gp_sql.tag t WHERE t.tagname = alter_tag.tagname;

	IF NOT FOUND THEN
		RAISE EXCEPTION 'tag "%" does not exist', tagname
			USING ERRCODE = 'undefined_object';
	END IF;

	IF unset_values THEN
		cur := NULL;
	END IF;

	IF drop_values IS NOT NULL THEN
		FOREACH v IN ARRAY drop_values LOOP
			IF EXISTS (SELECT 1 FROM gp_sql.tag_descriptions d
						WHERE d.tagname = alter_tag.tagname AND d.tagvalue = v) THEN
				RAISE EXCEPTION 'cannot drop tag "%" value "%", which is in use',
					tagname, v
					USING ERRCODE = 'check_violation';
			END IF;
		END LOOP;
		SELECT array_agg(x ORDER BY ord) INTO cur
		  FROM unnest(cur) WITH ORDINALITY AS u(x, ord)
		 WHERE NOT (x = ANY (drop_values));
	END IF;

	IF add_values IS NOT NULL THEN
		SELECT array_agg(DISTINCT x) INTO cur
		  FROM unnest(coalesce(cur, '{}'::text[]) || add_values) AS u(x);
	END IF;

	UPDATE gp_sql.tag t SET allowed_values = cur
	 WHERE t.tagname = alter_tag.tagname;
END;
$$;

COMMENT ON FUNCTION gp_sql.alter_tag(name, text[], text[], boolean) IS
	'change a tag''s allowed values; what Cloudberry writes as ALTER TAG';

CREATE FUNCTION gp_sql.rename_tag(tagname name, newname name)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
	moved bigint;
BEGIN
	UPDATE gp_sql.tag t SET tagname = newname WHERE t.tagname = rename_tag.tagname;
	IF NOT FOUND THEN
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

COMMENT ON FUNCTION gp_sql.rename_tag(name, name) IS
	'rename a tag; what Cloudberry writes as ALTER TAG ... RENAME TO';

CREATE FUNCTION gp_sql.drop_tag(tagname name, missing_ok boolean DEFAULT false)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
	used bigint;
BEGIN
	IF NOT EXISTS (SELECT 1 FROM gp_sql.tag t WHERE t.tagname = drop_tag.tagname) THEN
		IF missing_ok THEN
			RAISE NOTICE 'tag "%" does not exist, skipping', tagname;
			RETURN;
		END IF;
		RAISE EXCEPTION 'tag "%" does not exist', tagname
			USING ERRCODE = 'undefined_object';
	END IF;

	/*
	 * RESTRICT, always, as in Cloudberry: an assignment names the tag, and an
	 * assignment in another database cannot be reached from here.
	 */
	SELECT count(*) INTO used FROM gp_sql.tag_descriptions d
	 WHERE d.tagname = drop_tag.tagname;
	IF used > 0 THEN
		RAISE EXCEPTION 'cannot drop tag "%" while % object(s) carry it',
			tagname, used
			USING ERRCODE = 'dependent_objects_still_exist',
				  HINT = 'Remove the tag from those objects first.';
	END IF;

	DELETE FROM gp_sql.tag t WHERE t.tagname = drop_tag.tagname;
END;
$$;

COMMENT ON FUNCTION gp_sql.drop_tag(name, boolean) IS
	'drop a tag; what Cloudberry writes as DROP TAG';

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
