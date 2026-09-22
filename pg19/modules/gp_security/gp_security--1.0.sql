/* pg19/modules/gp_security/gp_security--1.0.sql */

\echo Use "CREATE EXTENSION gp_security" to load this file. \quit

/******************************************************************************
 * Password profiles
 *
 * A profile is a NOLOGIN role carrying a shared "gp_profile" security label,
 * and the link from a role to its profile is the "gp" label key of the same
 * name.  Both are shared, so they are read while logging in to any database,
 * which is what pg_profile and pg_authid.rolprofile are for in Cloudberry.
 *
 * The one thing that may not be a label is the password history: a label is
 * readable by everyone, and an old verifier is worth attacking offline.  It
 * goes in a table here, revoked from PUBLIC.
 *****************************************************************************/

GRANT USAGE ON SCHEMA gp_security TO PUBLIC;

CREATE TABLE gp_security.password_history (
	rolname		name NOT NULL,
	verifier	text NOT NULL,
	set_at		timestamptz NOT NULL DEFAULT now()
);

CREATE INDEX password_history_rolname_index
	ON gp_security.password_history (rolname, set_at DESC);

COMMENT ON TABLE gp_security.password_history IS
	'old password verifiers; Cloudberry keeps these in the shared catalog pg_password_history';

SELECT pg_catalog.pg_extension_config_dump('gp_security.password_history', '');

/* Nobody reads this but the server.  It holds what a password hashes to. */
REVOKE ALL ON gp_security.password_history FROM PUBLIC;

-----------------------------------------------------------------------------
-- Defining a profile
--
-- Procedures, because each is what a statement of Cloudberry's becomes: O26
-- writes CREATE PROFILE as CALL gp_security.create_profile(...), which
-- answers as a DDL statement does, with a command tag and no row.
-----------------------------------------------------------------------------

/*
 * The limits, as Cloudberry's grammar names them.  NULL means "say nothing
 * about this", which leaves it to the default profile; -2 is UNLIMITED, as in
 * Cloudberry.
 */
CREATE PROCEDURE gp_security.create_profile(profile name,
										   failed_login_attempts int DEFAULT NULL,
										   password_lock_time int DEFAULT NULL,
										   password_life_time int DEFAULT NULL,
										   password_grace_time int DEFAULT NULL,
										   password_reuse_time int DEFAULT NULL,
										   password_reuse_max int DEFAULT NULL,
										   password_allow_hashed boolean DEFAULT NULL,
										   password_verify_function text DEFAULT NULL)
LANGUAGE plpgsql
AS $$
DECLARE
	settings jsonb;
BEGIN
	IF EXISTS (SELECT 1 FROM pg_catalog.pg_roles r WHERE r.rolname = profile) THEN
		RAISE EXCEPTION 'role "%" already exists', profile
			USING ERRCODE = 'duplicate_object',
				  HINT = 'A profile is a role, so the two share their names.';
	END IF;

	settings := gp_security.profile_settings_json(
					failed_login_attempts, password_lock_time,
					password_life_time, password_grace_time,
					password_reuse_time, password_reuse_max,
					password_allow_hashed, password_verify_function);

	EXECUTE format('CREATE ROLE %I NOLOGIN', profile);
	EXECUTE format('SECURITY LABEL FOR gp_profile ON ROLE %I IS %L',
				   profile, settings::text);
END;
$$;

COMMENT ON PROCEDURE gp_security.create_profile(name, int, int, int, int, int, int, boolean, text) IS
	'define a password profile; what Cloudberry writes as CREATE PROFILE';

CREATE PROCEDURE gp_security.alter_profile(profile name,
										  failed_login_attempts int DEFAULT NULL,
										  password_lock_time int DEFAULT NULL,
										  password_life_time int DEFAULT NULL,
										  password_grace_time int DEFAULT NULL,
										  password_reuse_time int DEFAULT NULL,
										  password_reuse_max int DEFAULT NULL,
										  password_allow_hashed boolean DEFAULT NULL,
										  password_verify_function text DEFAULT NULL,
										  unset text[] DEFAULT NULL)
LANGUAGE plpgsql
AS $$
DECLARE
	have jsonb;
	gone text;
BEGIN
	SELECT l.label::jsonb INTO have
	  FROM pg_catalog.pg_shseclabel l
	  JOIN pg_catalog.pg_roles r ON r.oid = l.objoid
	 WHERE r.rolname = profile AND l.provider = 'gp_profile';

	IF have IS NULL THEN
		RAISE EXCEPTION 'profile "%" does not exist', profile
			USING ERRCODE = 'undefined_object';
	END IF;

	/* Only the settings that were given are changed. */
	have := have || gp_security.profile_settings_json(
						failed_login_attempts, password_lock_time,
						password_life_time, password_grace_time,
						password_reuse_time, password_reuse_max,
						password_allow_hashed, password_verify_function);

	/*
	 * A setting given as NULL is one the caller said nothing about, so taking
	 * one away has to be asked for by name.
	 */
	IF unset IS NOT NULL THEN
		FOREACH gone IN ARRAY unset LOOP
			have := have - gone;
		END LOOP;
	END IF;

	EXECUTE format('SECURITY LABEL FOR gp_profile ON ROLE %I IS %L',
				   profile, have::text);
END;
$$;

COMMENT ON PROCEDURE gp_security.alter_profile(name, int, int, int, int, int, int, boolean, text, text[]) IS
	'change a profile''s limits; what Cloudberry writes as ALTER PROFILE. A limit given as NULL is left alone; name it in "unset" to take it away.';

/*
 * DROP PROFILE a, b is one statement, so it is one CALL, over all of them:
 * one refused takes the others back with it, as the statement would.
 */
CREATE PROCEDURE gp_security.drop_profile(profiles name[],
										  missing_ok boolean DEFAULT false)
LANGUAGE plpgsql
AS $$
DECLARE
	one name;
	under bigint;
BEGIN
	FOREACH one IN ARRAY profiles LOOP
		IF NOT EXISTS (SELECT 1 FROM gp_security.profiles p WHERE p.profile = one) THEN
			IF missing_ok THEN
				RAISE NOTICE 'profile "%" does not exist, skipping', one;
				CONTINUE;
			END IF;
			RAISE EXCEPTION 'profile "%" does not exist', one
				USING ERRCODE = 'undefined_object';
		END IF;

		/*
		 * RESTRICT, as in Cloudberry: a role left pointing at a profile that
		 * is gone would silently fall back to the default.
		 */
		SELECT count(*) INTO under FROM gp_security.role_profiles rp
		 WHERE rp.profile = one;
		IF under > 0 THEN
			RAISE EXCEPTION 'cannot drop profile "%" while % role(s) are under it',
				one, under
				USING ERRCODE = 'dependent_objects_still_exist';
		END IF;

		EXECUTE format('DROP ROLE %I', one);
	END LOOP;
END;
$$;

COMMENT ON PROCEDURE gp_security.drop_profile(name[], boolean) IS
	'drop profiles; what Cloudberry writes as DROP PROFILE';

/* The limits as a label, with anything not given left out. */
CREATE FUNCTION gp_security.profile_settings_json(failed_login_attempts int,
												  password_lock_time int,
												  password_life_time int,
												  password_grace_time int,
												  password_reuse_time int,
												  password_reuse_max int,
												  password_allow_hashed boolean,
												  password_verify_function text)
RETURNS jsonb
LANGUAGE sql IMMUTABLE
BEGIN ATOMIC
	SELECT coalesce(jsonb_object_agg(k, v), '{}'::jsonb)
	  FROM (VALUES ('failed_login_attempts', to_jsonb(failed_login_attempts)),
				   ('password_lock_time',    to_jsonb(password_lock_time)),
				   ('password_life_time',    to_jsonb(password_life_time)),
				   ('password_grace_time',   to_jsonb(password_grace_time)),
				   ('password_reuse_time',   to_jsonb(password_reuse_time)),
				   ('password_reuse_max',    to_jsonb(password_reuse_max)),
				   ('password_allow_hashed',
					to_jsonb(CASE WHEN password_allow_hashed THEN 1 ELSE 0 END)),
				   ('password_verify_function', to_jsonb(password_verify_function)))
		   AS s(k, v)
	 WHERE v IS NOT NULL AND v <> 'null'::jsonb;
END;

-----------------------------------------------------------------------------
-- Putting a role under one, and locking an account
-----------------------------------------------------------------------------

CREATE FUNCTION gp_security.assign_profile(rolename name, profile name)
RETURNS void
AS 'MODULE_PATHNAME', 'gp_security_assign_profile'
LANGUAGE C;

COMMENT ON FUNCTION gp_security.assign_profile(name, name) IS
	'put a role under a profile, as Cloudberry''s ALTER USER ... PROFILE does; a NULL profile takes it away';

CREATE FUNCTION gp_security.lock_role(rolename name) RETURNS void
AS 'MODULE_PATHNAME', 'gp_security_lock_role' LANGUAGE C STRICT;

CREATE FUNCTION gp_security.unlock_role(rolename name) RETURNS void
AS 'MODULE_PATHNAME', 'gp_security_unlock_role' LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_security.lock_role(name) IS
	'lock an account, as Cloudberry''s ALTER USER ... ACCOUNT LOCK does';
COMMENT ON FUNCTION gp_security.unlock_role(name) IS
	'unlock an account, as Cloudberry''s ALTER USER ... ACCOUNT UNLOCK does';

CREATE FUNCTION gp_security.role_locked_until(rolename name) RETURNS timestamptz
AS 'MODULE_PATHNAME', 'gp_security_role_locked_until' LANGUAGE C STRICT STABLE;

CREATE FUNCTION gp_security.role_failed_logins(rolename name) RETURNS integer
AS 'MODULE_PATHNAME', 'gp_security_role_failed_logins' LANGUAGE C STRICT STABLE;

CREATE FUNCTION gp_security.role_profile(rolename name) RETURNS name
AS 'MODULE_PATHNAME', 'gp_security_role_profile' LANGUAGE C STRICT STABLE;

CREATE FUNCTION gp_security.profile_setting(profile name, setting text) RETURNS integer
AS 'MODULE_PATHNAME', 'gp_security_profile_setting' LANGUAGE C STRICT STABLE;

COMMENT ON FUNCTION gp_security.profile_setting(name, text) IS
	'what a role under this profile is really held to, after the default profile has filled in what it left alone';

-----------------------------------------------------------------------------
-- What is defined, and who is under it
-----------------------------------------------------------------------------

CREATE VIEW gp_security.profiles AS
	SELECT r.rolname AS profile,
		   (l.label::jsonb ->> 'failed_login_attempts')::int AS failed_login_attempts,
		   (l.label::jsonb ->> 'password_lock_time')::int AS password_lock_time,
		   (l.label::jsonb ->> 'password_life_time')::int AS password_life_time,
		   (l.label::jsonb ->> 'password_grace_time')::int AS password_grace_time,
		   (l.label::jsonb ->> 'password_reuse_time')::int AS password_reuse_time,
		   (l.label::jsonb ->> 'password_reuse_max')::int AS password_reuse_max,
		   (l.label::jsonb ->> 'password_allow_hashed')::int AS password_allow_hashed,
		   l.label::jsonb ->> 'password_verify_function' AS password_verify_function
	  FROM pg_catalog.pg_shseclabel l
	  JOIN pg_catalog.pg_roles r ON r.oid = l.objoid
	 WHERE l.provider = 'gp_profile'
	   AND l.classoid = 'pg_catalog.pg_authid'::regclass;

COMMENT ON VIEW gp_security.profiles IS
	'the password profiles of this cluster; Cloudberry keeps these in the shared catalog pg_profile';

GRANT SELECT ON gp_security.profiles TO PUBLIC;

CREATE VIEW gp_security.role_profiles AS
	SELECT r.rolname,
		   gp_security.role_profile(r.rolname) AS profile,
		   gp_security.role_failed_logins(r.rolname) AS failed_logins,
		   gp_security.role_locked_until(r.rolname) AS locked_until
	  FROM pg_catalog.pg_roles r
	 WHERE gp_security.role_profile(r.rolname) IS NOT NULL;

COMMENT ON VIEW gp_security.role_profiles IS
	'which profile each role is under, and the state of its account; Cloudberry keeps these in pg_authid';

GRANT SELECT ON gp_security.role_profiles TO PUBLIC;

/*
 * The profile a role with none of its own is held to.  Cloudberry ships
 * pg_default; a role may not be named pg_*, so this one is gp_default.
 * Everything in it is left unset, so nothing is enforced until an
 * administrator says what it should be.
 *
 * A profile is a role, and a role is the cluster's rather than a database's:
 * the extension created in a second database finds the gp_default the first
 * one made, and keeps it.  A role of that name that is not a profile is still
 * refused, by create_profile.
 */
DO $$
BEGIN
	IF NOT EXISTS (SELECT 1
					 FROM pg_catalog.pg_shseclabel l
					 JOIN pg_catalog.pg_roles r ON r.oid = l.objoid
					WHERE r.rolname = 'gp_default'
					  AND l.classoid = 'pg_catalog.pg_authid'::pg_catalog.regclass
					  AND l.provider = 'gp_profile') THEN
		CALL gp_security.create_profile('gp_default');
	END IF;
END
$$;
