CREATE OR REPLACE FUNCTION pg_catalog.create_coordinator_table(table_name regclass)
	RETURNS void
	LANGUAGE C STRICT
	AS 'MODULE_PATHNAME', $$create_coordinator_table$$;
COMMENT ON FUNCTION create_coordinator_table(table_name regclass)
	IS 'create a coordinator table';
