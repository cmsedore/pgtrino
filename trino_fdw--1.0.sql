/* trino_fdw--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION trino_fdw" to load this file. \quit

CREATE FUNCTION trino_fdw_handler()
RETURNS fdw_handler
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION trino_fdw_validator(text[], oid)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FOREIGN DATA WRAPPER trino_fdw
  HANDLER trino_fdw_handler
  VALIDATOR trino_fdw_validator;

CREATE FUNCTION trino_fdw_version()
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

-- Run arbitrary SQL on a Trino server. The caller must supply a column
-- definition list, e.g.
--   SELECT * FROM trino_query('srv', 'SELECT count(*) FROM tpch.tiny.orders') AS t(n bigint);
CREATE FUNCTION trino_query(server text, sql text)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;

-- Run a statement (DDL, INSERT, DELETE, ...) on a Trino server and return
-- its update count, or -1 when the statement has none.
CREATE FUNCTION trino_execute(server text, sql text)
RETURNS bigint
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;
