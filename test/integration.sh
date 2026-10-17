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

-- join, aggregate, ORDER BY/LIMIT and parameterized-path pushdown: compare
-- against the same queries with pushdown of joins and aggregates defeated
ANALYZE tiny.nation; ANALYZE tiny.region; ANALYZE tiny.customer; ANALYZE tiny.orders;
CREATE TABLE expected_join AS
  SELECT * FROM trino_query('tpch', \$\$
    SELECT n.name, r.name AS region, count(*) AS cnt
    FROM tpch.tiny.customer c JOIN tpch.tiny.nation n ON n.nationkey = c.nationkey
    JOIN tpch.tiny.region r ON r.regionkey = n.regionkey
    WHERE c.mktsegment = 'BUILDING' GROUP BY n.name, r.name
    ORDER BY cnt DESC, n.name LIMIT 7\$\$) AS t(name text, region text, cnt bigint);
CREATE TABLE actual_join AS
  SELECT n.name, r.name AS region, count(*) AS cnt
  FROM tiny.customer c JOIN tiny.nation n ON n.nationkey = c.nationkey
  JOIN tiny.region r ON r.regionkey = n.regionkey
  WHERE c.mktsegment = 'BUILDING' GROUP BY n.name, r.name
  ORDER BY cnt DESC, n.name LIMIT 7;
DO \$\$
DECLARE r record; n bigint;
BEGIN
  SELECT count(*) INTO n FROM (SELECT * FROM expected_join EXCEPT ALL SELECT * FROM actual_join) d;
  IF n <> 0 THEN RAISE EXCEPTION 'pushed-down join/aggregate differs from Trino result'; END IF;
  SELECT count(*) INTO n FROM actual_join;
  IF n <> 7 THEN RAISE EXCEPTION 'join/aggregate row count: %', n; END IF;

  -- the plan must actually have been pushed down
  n := 0;
  FOR r IN EXECUTE \$q\$EXPLAIN SELECT n.name, count(*) FROM tiny.customer c JOIN tiny.nation n
                     ON n.nationkey = c.nationkey GROUP BY n.name ORDER BY 2 DESC LIMIT 3\$q\$ LOOP
    IF r."QUERY PLAN" LIKE '%Aggregate on ((tiny.customer c) INNER JOIN (tiny.nation n))%' THEN n := n + 1; END IF;
  END LOOP;
  IF n <> 1 THEN RAISE EXCEPTION 'join + aggregate was not pushed down'; END IF;

  -- outer join
  SELECT count(*) INTO n FROM tiny.region rg LEFT JOIN tiny.nation x ON x.regionkey = rg.regionkey AND x.nationkey > 20;
  IF n <> 6 THEN RAISE EXCEPTION 'left join count: %', n; END IF;

  -- parameterized path: nested loop probing Trino per outer row
  CREATE TEMP TABLE keys (k bigint PRIMARY KEY);
  INSERT INTO keys VALUES (1), (17), (24);
  SET LOCAL enable_hashjoin = off; SET LOCAL enable_mergejoin = off;
  SELECT string_agg(x.name, ',' ORDER BY k.k) INTO r FROM keys k JOIN tiny.nation x ON x.nationkey = k.k;
  IF r.string_agg <> 'ARGENTINA,PERU,UNITED STATES' THEN RAISE EXCEPTION 'parameterized join: %', r.string_agg; END IF;
  RESET enable_hashjoin; RESET enable_mergejoin;
END \$\$;
DROP TABLE expected_join, actual_join;

SQL

# UPDATE/DELETE need a connector with row-level modification; use the
# "iceberg" catalog when the server has one, otherwise skip that part.
if psql -X -Atq -d "$DB" -c "SELECT count(*) FROM trino_query('tpch', 'SHOW CATALOGS') AS t(c text) WHERE c = 'iceberg'" | grep -q '^1$'; then
psql -X -v ON_ERROR_STOP=1 -q -d "$DB" <<SQL
CREATE SERVER ice FOREIGN DATA WRAPPER trino_fdw
  OPTIONS (host '$TRINO_HOST', port '$TRINO_PORT', catalog 'iceberg', schema 'fdw_it');
CREATE USER MAPPING FOR PUBLIC SERVER ice OPTIONS (user 'trino_fdw_test');
SELECT trino_execute('ice', 'CREATE SCHEMA IF NOT EXISTS iceberg.fdw_it');
SELECT trino_execute('ice', 'DROP TABLE IF EXISTS iceberg.fdw_it.dml');
SELECT trino_execute('ice', 'CREATE TABLE iceberg.fdw_it.dml AS SELECT nationkey AS id, name, regionkey FROM tpch.tiny.nation');
CREATE FOREIGN TABLE dml (id bigint, name text, regionkey bigint) SERVER ice;
DO \$\$
DECLARE n bigint; r record;
BEGIN
  DELETE FROM dml WHERE regionkey = 1 AND name LIKE '%A%';
  GET DIAGNOSTICS n = ROW_COUNT;
  IF n <> 4 THEN RAISE EXCEPTION 'delete count: %', n; END IF;
  UPDATE dml SET name = lower(name) || '!', regionkey = regionkey + 10 WHERE id IN (5, 6) OR id = (SELECT 7);
  GET DIAGNOSTICS n = ROW_COUNT;
  IF n <> 3 THEN RAISE EXCEPTION 'update count: %', n; END IF;
  SELECT * INTO r FROM dml WHERE id = 6;
  IF r.name <> 'france!' OR r.regionkey <> 13 THEN RAISE EXCEPTION 'update result: %', r; END IF;
  SELECT count(*) INTO n FROM dml;
  IF n <> 21 THEN RAISE EXCEPTION 'row count after dml: %', n; END IF;
END \$\$;
SELECT trino_execute('ice', 'DROP TABLE iceberg.fdw_it.dml');
SQL
else
  echo "no iceberg catalog: skipping UPDATE/DELETE checks"
fi

psql -X -q -c "DROP DATABASE $DB" postgres
echo "integration tests passed"
