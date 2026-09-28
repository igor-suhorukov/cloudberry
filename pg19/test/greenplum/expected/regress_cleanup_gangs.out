--
-- regress.so's cleanupAllGangs(), which segspace calls to close the
-- session's connections to the segments: Cloudberry's dispatch makes it,
-- later in the schedule than segspace, whose expected output has it from a
-- run before.  The port's cb_regress.c serves it.
--
\getenv abs_builddir PG_ABS_BUILDDIR
\set regress_dll :abs_builddir '/regress.so'
CREATE OR REPLACE FUNCTION cleanupAllGangs() RETURNS BOOL
AS :'regress_dll', 'cleanupAllGangs' LANGUAGE C;
