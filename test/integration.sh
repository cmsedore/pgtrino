#!/usr/bin/env bash
# End-to-end checks against a real Trino server (default: localhost:8080 with
# the "tpch" and "memory" catalogs, as provided by the trinodb/trino image):
#
#   docker run -d --name trino -p 8080:8080 trinodb/trino
#   # add a memory catalog: etc/catalog/memory.properties -> connector.name=memory
#   test/integration.sh
#
# Requires the extension to be installed and a superuser psql connection
# (PGHOST/PGUSER/... are honoured).
set -euo pipefail

TRINO_HOST=${TRINO_HOST:-localhost}
TRINO_PORT=${TRINO_PORT:-8080}
DB=${TRINO_FDW_TEST_DB:-trino_fdw_integration}

psql -X -v ON_ERROR_STOP=1 -q -c "DROP DATABASE IF EXISTS $DB" -c "CREATE DATABASE $DB" postgres

psql -X -v ON_ERROR_STOP=1 -q -d "$DB" <<SQL
CREATE EXTENSION trino_fdw;
CREATE SERVER tpch FOREIGN DATA WRAPPER trino_fdw
  OPTIONS (host '$TRINO_HOST', port '$TRINO_PORT', catalog 'tpch');
CREATE SERVER mem FOREIGN DATA WRAPPER trino_fdw
  OPTIONS (host '$TRINO_HOST', port '$TRINO_PORT', catalog 'memory', schema 'default');
CREATE USER MAPPING FOR PUBLIC SERVER tpch OPTIONS (user 'trino_fdw_test');
CREATE USER MAPPING FOR PUBLIC SERVER mem OPTIONS (user 'trino_fdw_test');

CREATE SCHEMA tiny;
IMPORT FOREIGN SCHEMA tiny FROM SERVER tpch INTO tiny;

DO \$\$
DECLARE n bigint; r record;
BEGIN
  SELECT count(*) INTO n FROM tiny.nation;
  IF n <> 25 THEN RAISE EXCEPTION 'nation count: %', n; END IF;

  SELECT count(*) INTO n FROM tiny.orders WHERE orderstatus = 'F' AND totalprice > 100000;
  IF n <> 4718 THEN RAISE EXCEPTION 'filtered orders count: %', n; END IF;

  SELECT name INTO r FROM tiny.nation ORDER BY nationkey DESC LIMIT 1;
  IF r.name <> 'UNITED STATES' THEN RAISE EXCEPTION 'order by/limit: %', r.name; END IF;

  SELECT sum(l.quantity)::bigint AS q INTO r FROM tiny.lineitem l JOIN tiny.orders o ON o.orderkey = l.orderkey
   WHERE o.orderdate = DATE '1995-01-01';
  IF r.q IS NULL THEN RAISE EXCEPTION 'join returned nothing'; END IF;
END \$\$;

-- write path and type round trip via the memory connector
SELECT trino_execute('mem', 'DROP TABLE IF EXISTS memory.default.fdw_it');
SELECT trino_execute('mem', 'CREATE TABLE memory.default.fdw_it (id integer, name varchar, amt decimal(10,2), ts timestamp(6), tags array(varchar), doc json, d date, b varbinary, u uuid)');
CREATE FOREIGN TABLE fdw_it (id int, name text, amt numeric(10,2), ts timestamp(6), tags text[], doc jsonb, d date, b bytea, u uuid)
  SERVER mem OPTIONS (table_name 'fdw_it');
INSERT INTO fdw_it VALUES (1, 'it''s', 1.50, '2024-01-02 03:04:05.678901', ARRAY['x', 'y z'], '{"a": [1, 2]}', '2024-05-06', '\\xcafe', '12345678-1234-5678-1234-567812345678');
INSERT INTO fdw_it (id, name) SELECT g, 'row ' || g FROM generate_series(2, 300) g;

DO \$\$
DECLARE r record; n bigint;
BEGIN
  SELECT * INTO r FROM fdw_it WHERE id = 1;
  IF r.name <> 'it''s' OR r.amt <> 1.50 OR r.ts <> '2024-01-02 03:04:05.678901' OR r.tags <> ARRAY['x', 'y z']
     OR r.doc <> '{"a": [1, 2]}'::jsonb OR r.d <> '2024-05-06' OR r.b <> '\\xcafe' THEN
    RAISE EXCEPTION 'round trip mismatch: %', r;
  END IF;
  SELECT count(*) INTO n FROM fdw_it;
  IF n <> 300 THEN RAISE EXCEPTION 'row count after inserts: %', n; END IF;
  SELECT count(*) INTO n FROM fdw_it WHERE name LIKE 'row 1%' AND id IN (10, 11, 12, 999);
  IF n <> 3 THEN RAISE EXCEPTION 'pushdown count: %', n; END IF;
END \$\$;

ANALYZE fdw_it;
SELECT trino_execute('mem', 'DROP TABLE memory.default.fdw_it');
SQL

psql -X -q -c "DROP DATABASE $DB" postgres
echo "integration tests passed"
