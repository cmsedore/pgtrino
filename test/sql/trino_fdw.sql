-- Regression tests for trino_fdw.
-- They run against test/mock_trino.py listening on 127.0.0.1:18080
-- (see test/run_tests.sh), so results are deterministic and need no Trino.

SET client_min_messages = warning;
SET timezone = 'UTC';
SET datestyle = 'ISO, YMD';
SET extra_float_digits = 0;
SET intervalstyle = 'postgres';

SELECT trino_fdw_version();

-- the mock records every statement it receives; start from a clean log
CREATE SERVER mock_init FOREIGN DATA WRAPPER trino_fdw
  OPTIONS (host '127.0.0.1', port '18080', catalog 'mock', schema 's1');
SELECT count(*) >= 0 AS ok FROM trino_query('mock_init', 'SELECT sql FROM sqllog') AS t(sql text);
DROP SERVER mock_init;

-- ===================================================================
-- option validation
-- ===================================================================
CREATE SERVER mock FOREIGN DATA WRAPPER trino_fdw
  OPTIONS (host '127.0.0.1', port '18080', catalog 'mock', schema 's1');
CREATE USER MAPPING FOR PUBLIC SERVER mock OPTIONS (user 'tester');

CREATE SERVER badopt FOREIGN DATA WRAPPER trino_fdw OPTIONS (hostname 'x');
CREATE SERVER badopt FOREIGN DATA WRAPPER trino_fdw OPTIONS (host 'x', port '99999');
CREATE SERVER badopt FOREIGN DATA WRAPPER trino_fdw OPTIONS (host 'x', ssl 'maybe');
CREATE SERVER badopt FOREIGN DATA WRAPPER trino_fdw OPTIONS (host 'x', tuple_cost '-1');
CREATE SERVER nohost FOREIGN DATA WRAPPER trino_fdw OPTIONS (port '1');
CREATE FOREIGN TABLE nohost_t (x int) SERVER nohost;
SELECT * FROM nohost_t;
ALTER SERVER mock OPTIONS (ADD batch_size '0');
ALTER FOREIGN TABLE nohost_t OPTIONS (ADD column_name 'x');
ALTER FOREIGN TABLE nohost_t ALTER COLUMN x OPTIONS (ADD table_name 'x');

-- ===================================================================
-- IMPORT FOREIGN SCHEMA and type mapping
-- ===================================================================
CREATE SCHEMA imp;
IMPORT FOREIGN SCHEMA s1 FROM SERVER mock INTO imp;
\d imp.t1
\d imp.t2
\d imp."weird name"
SELECT sql FROM trino_query('mock', 'SELECT sql FROM sqllog') AS t(sql text) ORDER BY 1;

DROP SCHEMA imp CASCADE;
CREATE SCHEMA imp;
IMPORT FOREIGN SCHEMA s1 LIMIT TO (t2) FROM SERVER mock INTO imp;
\det imp.*
DROP SCHEMA imp CASCADE;
CREATE SCHEMA imp;
IMPORT FOREIGN SCHEMA s1 EXCEPT (t2, "weird name") FROM SERVER mock INTO imp;
\det imp.*
IMPORT FOREIGN SCHEMA s1 FROM SERVER mock INTO imp OPTIONS (bogus 'x');
DROP SCHEMA imp CASCADE;

-- a server without a catalog needs one at import time
CREATE SERVER nocat FOREIGN DATA WRAPPER trino_fdw OPTIONS (host '127.0.0.1', port '18080');
CREATE SCHEMA imp;
IMPORT FOREIGN SCHEMA s1 FROM SERVER nocat INTO imp;
IMPORT FOREIGN SCHEMA s1 LIMIT TO (t2) FROM SERVER nocat INTO imp OPTIONS (catalog 'mock');
\det+ imp.t2
DROP SCHEMA imp CASCADE;

-- ===================================================================
-- scanning and value conversion
-- ===================================================================
CREATE FOREIGN TABLE t1 (
  id integer,
  name varchar(10),
  val double precision,
  created timestamp(6),
  flag boolean,
  tags text[],
  attrs jsonb,
  big jsonb,
  bin bytea,
  dec numeric(10,2),
  d date,
  u uuid,
  ts_tz timestamptz,
  dur interval
) SERVER mock OPTIONS (table_name 't1');

SELECT * FROM t1 ORDER BY id;
SELECT id, tags[2], attrs->'a', big->1->0, dur FROM t1 ORDER BY id;
SELECT count(*) FROM t1;
SELECT sql FROM trino_query('mock', 'SELECT sql FROM sqllog') AS t(sql text) ORDER BY 1;

-- stopping early cancels the remote query (the mock logs the DELETE)
SELECT id FROM t1 WHERE name ~ 'a' LIMIT 1;
SELECT sql FROM trino_query('mock', 'SELECT sql FROM sqllog') AS t(sql text) ORDER BY 1;

-- paging with empty pages, 503 retry, zero rows
CREATE FOREIGN TABLE t1_slow (id integer, name varchar(10)) SERVER mock;
CREATE FOREIGN TABLE t1_flaky (id integer, name varchar(10)) SERVER mock;
CREATE FOREIGN TABLE empty (k bigint, v numeric) SERVER mock;
SELECT * FROM t1_slow ORDER BY id;
SELECT * FROM t1_flaky ORDER BY id;
SELECT * FROM empty;
SELECT count(*) FROM empty;

-- column_name option and quoting
CREATE FOREIGN TABLE weird (one integer OPTIONS (column_name 'Col One'), xy text OPTIONS (column_name 'x"y'))
  SERVER mock OPTIONS (table_name 'weird name');
EXPLAIN (COSTS OFF) SELECT * FROM weird WHERE one = 1;

-- a table mapped to the wrong local types
CREATE FOREIGN TABLE t1_bad (id date) SERVER mock OPTIONS (table_name 't1');
SELECT * FROM t1_bad;
CREATE FOREIGN TABLE t1_nocol (nosuch integer) SERVER mock OPTIONS (table_name 't1');
SELECT * FROM t1_nocol;

-- ===================================================================
-- errors from Trino
-- ===================================================================
CREATE FOREIGN TABLE boom (id integer) SERVER mock;
SELECT * FROM boom;
CREATE FOREIGN TABLE boom_late (id integer, name text) SERVER mock;
SELECT * FROM boom_late;
CREATE FOREIGN TABLE nosuch (id integer) SERVER mock;
SELECT * FROM nosuch;
ALTER SERVER mock OPTIONS (ADD source 'reject-me');
SELECT * FROM t1;
ALTER SERVER mock OPTIONS (DROP source);

-- ===================================================================
-- pushdown: what goes to Trino and what stays local
-- ===================================================================
CREATE FOREIGN TABLE t2 (k bigint, v numeric(10,2), note text) SERVER mock;

-- projection
EXPLAIN (COSTS OFF) SELECT k FROM t2;
EXPLAIN (COSTS OFF) SELECT 1 FROM t2;
EXPLAIN (COSTS OFF) SELECT t2 FROM t2;
EXPLAIN (COSTS OFF) SELECT count(*) FROM t2;

-- operators and literals
EXPLAIN (COSTS OFF) SELECT k FROM t2 WHERE k = 1 AND v > 1.5 AND note <> 'x';
EXPLAIN (COSTS OFF) SELECT k FROM t2 WHERE k + 1 > 2 * v - 3 OR k % 2 = 0;
EXPLAIN (COSTS OFF) SELECT k FROM t2 WHERE k::numeric = v AND k = 1::smallint::bigint AND v::double precision = 1;
EXPLAIN (COSTS OFF) SELECT k FROM t2 WHERE note LIKE 'a\_%' AND note NOT LIKE '%z';
EXPLAIN (COSTS OFF) SELECT k FROM t2 WHERE note || 'x' = 'yx';
EXPLAIN (COSTS OFF) SELECT k FROM t2 WHERE k IN (1, 2, 3) AND k NOT IN (4, 5);
EXPLAIN (COSTS OFF) SELECT k FROM t2 WHERE k = ANY ('{}'::bigint[]) OR k <> ALL ('{}'::bigint[]);
EXPLAIN (COSTS OFF) SELECT k FROM t2 WHERE v IS NULL AND note IS NOT NULL AND NOT (k = 1);
EXPLAIN (COSTS OFF) SELECT k FROM t2 WHERE k IS DISTINCT FROM 3;
EXPLAIN (COSTS OFF) SELECT k FROM t2 WHERE coalesce(v, 0) > 1 AND greatest(k, 5) = 5 AND least(k, v) < 2;
EXPLAIN (COSTS OFF) SELECT k FROM t2 WHERE lower(note) = 'a' AND upper(note) = 'A' AND length(note) = 1
  AND btrim(note) = 'a' AND abs(k) = 1 AND round(v) = 1 AND round(v, 1) = 1.5 AND substr(note, 1, 2) = 'ab'
  AND ceil(v) = 2 AND floor(v) = 1 AND sqrt(v) < 2;
EXPLAIN (COSTS OFF) SELECT k FROM t2 WHERE k = 1::smallint OR k = 1::bigint OR v = 1::real OR v = 1.5::double precision;
EXPLAIN (COSTS OFF) SELECT k FROM t2 WHERE note = 'it''s' OR note = E'back\\slash';
EXPLAIN (COSTS OFF) SELECT k FROM t2 WHERE k = 1 AND true;

-- typed literals
CREATE FOREIGN TABLE types (b boolean, d date, ts timestamp, tstz timestamptz, t time, u uuid, bin bytea, f real, dbl double precision, n numeric)
  SERVER mock OPTIONS (table_name 't2');
EXPLAIN (COSTS OFF) SELECT b FROM types WHERE b = true AND d = '2024-02-29' AND ts = '2024-02-29 13:14:15.5'
  AND tstz = '2024-02-29 13:14:15+02' AND t = '13:14:15.25' AND u = '12345678-1234-5678-1234-567812345678'
  AND bin = '\xcafe' AND f = 1.5 AND dbl = 2.5 AND n = 3.25;
EXPLAIN (COSTS OFF) SELECT b FROM types WHERE dbl = 'NaN' OR dbl = 'Infinity' OR dbl = '-Infinity';
-- not representable in Trino: infinite dates, numeric NaN, BC dates
EXPLAIN (COSTS OFF) SELECT b FROM types WHERE d = 'infinity' OR ts = '-infinity';
EXPLAIN (COSTS OFF) SELECT b FROM types WHERE n = 'NaN';
EXPLAIN (COSTS OFF) SELECT b FROM types WHERE d = '0044-03-15 BC';

-- things that must stay local: collation-dependent ordering, casts, mutable
-- functions, unknown operators and functions, booleans compared with <
EXPLAIN (COSTS OFF) SELECT k FROM t2 WHERE note > 'a';
EXPLAIN (COSTS OFF) SELECT k FROM t2 WHERE k::text = '1';
EXPLAIN (COSTS OFF) SELECT k FROM t2 WHERE note ~ 'a' OR note ILIKE 'a';
EXPLAIN (COSTS OFF) SELECT k FROM t2 WHERE v > random();
EXPLAIN (COSTS OFF) SELECT k FROM t2 WHERE k = 1 AND md5(note) = 'x';
EXPLAIN (COSTS OFF) SELECT b FROM types WHERE b < true;
EXPLAIN (COSTS OFF) SELECT k FROM t2 WHERE k > 1 AND now() > '2020-01-01';
EXPLAIN (COSTS OFF) SELECT b FROM types WHERE d + 1 > '2024-01-01';
EXPLAIN (COSTS OFF) SELECT b FROM types WHERE bin < '\x00';

-- ORDER BY and LIMIT pushdown
EXPLAIN (COSTS OFF) SELECT k, v FROM t2 ORDER BY k DESC NULLS FIRST, v;
EXPLAIN (COSTS OFF) SELECT k FROM t2 ORDER BY k LIMIT 10;
EXPLAIN (COSTS OFF) SELECT k FROM t2 ORDER BY k LIMIT 10 OFFSET 5;
EXPLAIN (COSTS OFF) SELECT k FROM t2 LIMIT 10;
EXPLAIN (COSTS OFF) SELECT k FROM t2 OFFSET 7;
EXPLAIN (COSTS OFF) SELECT k FROM t2 WHERE v > 1 ORDER BY k LIMIT 3;
-- text ordering stays local, LIMIT follows the local sort
EXPLAIN (COSTS OFF) SELECT k FROM t2 ORDER BY note LIMIT 3;
-- a local qual keeps LIMIT local but the sort can still be remote
EXPLAIN (COSTS OFF) SELECT k FROM t2 WHERE note ~ 'a' ORDER BY k LIMIT 3;
-- non-constant limit, FOR UPDATE, WITH TIES, SRFs are not pushed
EXPLAIN (COSTS OFF) SELECT k FROM t2 ORDER BY k LIMIT (SELECT 1);
EXPLAIN (COSTS OFF) SELECT k FROM t2 ORDER BY k FETCH FIRST 2 ROWS WITH TIES;
EXPLAIN (COSTS OFF) SELECT k, generate_series(1, 2) FROM t2 ORDER BY k LIMIT 2;
-- expressions over pushed-down sorts work through the upper-rel target
EXPLAIN (COSTS OFF) SELECT k * 2 AS dbl, upper(note) FROM t2 ORDER BY k LIMIT 2;
SELECT k * 2 AS dbl, upper(note) FROM t2 ORDER BY k LIMIT 2;
SELECT k FROM t2 ORDER BY k DESC;

-- aggregates are computed locally
EXPLAIN (COSTS OFF) SELECT sum(v) FROM t2 WHERE k > 0;

-- ===================================================================
-- parameters
-- ===================================================================
SET plan_cache_mode = force_generic_plan;
PREPARE p1 (bigint, text) AS SELECT k FROM t2 WHERE k = $1 AND (note = $2 OR note IS NULL);
EXPLAIN (COSTS OFF) EXECUTE p1 (10, 'ten');
EXECUTE p1 (10, 'ten');
EXECUTE p1 (10, NULL);
RESET plan_cache_mode;
EXPLAIN (COSTS OFF) SELECT k FROM t2 WHERE k = (SELECT 20);
SELECT k FROM t2 WHERE k = (SELECT 20);
SELECT sql FROM trino_query('mock', 'SELECT sql FROM sqllog') AS t(sql text) WHERE sql LIKE '%"t2"%' ORDER BY 1;

-- joins: no parameterized paths, so the join itself runs locally
CREATE TABLE local_k (k bigint);
INSERT INTO local_k VALUES (10), (30);
SELECT l.k, t2.note FROM local_k l JOIN t2 USING (k) ORDER BY 1;
SELECT a.k, b.k FROM t2 a JOIN t2 b ON a.k < b.k ORDER BY 1, 2;

-- ===================================================================
-- INSERT
-- ===================================================================
EXPLAIN (COSTS OFF) INSERT INTO t2 VALUES (1, 2.5, 'x');
INSERT INTO t2 VALUES (1, 2.5, 'x'), (2, NULL, 'it''s');
INSERT INTO t2 SELECT g, g * 1.5, 'row ' || g FROM generate_series(1, 5) g;
INSERT INTO t2 (k) VALUES (99) RETURNING k, note;
COPY t2 FROM STDIN;
7	7.00	seven
8	\N	\N
\.
INSERT INTO t1 VALUES (1, 'n', 1.5, '2024-01-02 03:04:05.123456', true, '{a,"b c"}', '{"j": 1}', '[[1]]', '\xcafe', 12.34,
  '2024-01-02', '12345678-1234-5678-1234-567812345678', '2024-01-02 03:04:05+01', '1 day 02:03:04.5');
INSERT INTO t1 (id, dur) VALUES (2, '1 year 2 months');
INSERT INTO t1 (id, dur) VALUES (3, '1 month 1 day');
INSERT INTO t1 (id, dur) VALUES (5, '30:00:00'), (6, '-1 day -1 hour');
INSERT INTO t1 (id, val) VALUES (4, 'NaN');
SELECT sql FROM trino_query('mock', 'SELECT sql FROM sqllog') AS t(sql text) WHERE sql LIKE 'INSERT%' ORDER BY 1;
UPDATE t2 SET k = 1;
DELETE FROM t2;
INSERT INTO t2 VALUES (1, 1, 'x') ON CONFLICT DO NOTHING;
CREATE FOREIGN TABLE boom_w (id integer) SERVER mock OPTIONS (table_name 'boom');
INSERT INTO boom_w VALUES (1);
-- batch size is honoured
ALTER FOREIGN TABLE t2 OPTIONS (batch_size '2');
INSERT INTO t2 SELECT g, NULL, NULL FROM generate_series(1, 5) g;
SELECT sql FROM trino_query('mock', 'SELECT sql FROM sqllog') AS t(sql text) WHERE sql LIKE 'INSERT INTO "mock"."s1"."t2" ("k", "v", "note") VALUES (%, NULL, NULL)%' ORDER BY 1;

-- ===================================================================
-- passthrough functions
-- ===================================================================
SELECT * FROM trino_query('mock', 'SELECT "k", "v" FROM t2') AS t(k bigint, v numeric);
SELECT * FROM trino_query('mock', 'SELECT "k" FROM t2') AS t(k bigint, v numeric);
SELECT * FROM trino_query('mock', 'SELECT "k" FROM boom') AS t(k bigint);
SELECT * FROM trino_query('mock', 'SELECT "k" FROM t2');
SELECT trino_execute('mock', 'CREATE TABLE x (a int)');
SELECT trino_execute('mock', 'INSERT INTO x VALUES (1), (2)');
SELECT trino_query('nonexistent', 'SELECT 1');

-- ===================================================================
-- privileges: USAGE on the server is required for passthrough
-- ===================================================================
CREATE ROLE trino_fdw_nobody;
SET ROLE trino_fdw_nobody;
SELECT trino_execute('mock', 'SELECT 1');
RESET ROLE;
DROP ROLE trino_fdw_nobody;

-- ===================================================================
-- ANALYZE
-- ===================================================================
ANALYZE t1;
SELECT reltuples FROM pg_class WHERE oid = 't1'::regclass;
SELECT attname, null_frac FROM pg_stats WHERE tablename = 't1' AND attname IN ('id', 'name') ORDER BY 1;
SELECT sql FROM trino_query('mock', 'SELECT sql FROM sqllog') AS t(sql text) WHERE sql LIKE '%count(*)%' OR sql LIKE '%TABLESAMPLE%' ORDER BY 1;

-- ===================================================================
-- cleanup
-- ===================================================================
DROP TABLE local_k;
DROP SERVER mock CASCADE;
DROP SERVER nocat CASCADE;
DROP SERVER nohost CASCADE;
