# trino_fdw — a native PostgreSQL foreign-data wrapper for Trino

`trino_fdw` lets PostgreSQL query (and insert into) tables that live in a
[Trino](https://trino.io) cluster, as ordinary foreign tables:

```sql
CREATE EXTENSION trino_fdw;

CREATE SERVER trino FOREIGN DATA WRAPPER trino_fdw
  OPTIONS (host 'trino.example.com', port '8080', catalog 'hive');
CREATE USER MAPPING FOR PUBLIC SERVER trino OPTIONS (user 'analytics');

IMPORT FOREIGN SCHEMA sales FROM SERVER trino INTO sales;

SELECT region, sum(amount)
FROM sales.orders
WHERE order_date >= DATE '2024-01-01' AND status IN ('open', 'shipped')
GROUP BY region;
```

It is written in C against the PostgreSQL FDW API and talks to Trino's REST
protocol directly over libcurl. Responses are parsed with the JSON parser that
ships with PostgreSQL, so **libcurl is the only dependency**: no JDBC, no
Java, no Python bridge.

## Features

* **Projection and WHERE pushdown.** Only the columns a query needs are
  fetched, and every restriction that has identical semantics in Trino is
  evaluated there: comparisons, arithmetic, `IN` lists, `LIKE`, `IS NULL`,
  `IS DISTINCT FROM`, `COALESCE`/`GREATEST`/`LEAST`, boolean logic, a set of
  common functions, and typed literals for numbers, strings, dates, times,
  timestamps, UUIDs and binary data. Anything else is evaluated locally, so
  pushdown never changes results.
* **Join pushdown.** Inner, left, right and full joins between tables on the
  same server run as one Trino query, including nested joins, with WHERE and
  ON conditions placed where their semantics require.
* **Aggregate pushdown.** `count`, `sum`, `min`, `max`, `avg` (over doubles),
  `bool_and`/`bool_or`, with `DISTINCT` and `FILTER`, plus `GROUP BY` and
  `HAVING`, are computed by Trino, also on top of a pushed-down join.
* **ORDER BY, LIMIT and OFFSET pushdown**, including `ORDER BY ... LIMIT n`
  which Trino executes as a cheap TopN. Collation-dependent text ordering is
  deliberately kept local.
* **Query parameters and parameterized paths.** Prepared-statement
  parameters and sub-select results are sent as literals at execution time,
  and a nested loop with a small local outer table can probe Trino once per
  outer row (`WHERE t.k = $1`) instead of fetching the whole table.
* **`IMPORT FOREIGN SCHEMA`** with a full Trino → PostgreSQL type mapping.
* **INSERT** (including `COPY ... FROM` and `INSERT ... RETURNING`), batched
  into multi-row `INSERT ... VALUES` statements.
* **UPDATE and DELETE**, executed as single Trino statements when the whole
  statement can be expressed in Trino (connector permitting).
* **Spooled client protocol.** Large results are fetched as segments from
  the object storage Trino spools them to, when the server has spooling
  enabled, with acknowledgement so Trino can prune them early.
* **`ANALYZE`** on foreign tables, sampling with `TABLESAMPLE BERNOULLI`.
* **`EXPLAIN`** shows the exact Trino SQL that will run.
* **Passthrough functions** `trino_query()` and `trino_execute()` for
  anything that cannot be expressed through a foreign table (aggregates you
  want Trino to compute, DDL, `DELETE`, ...).
* **Interruptible.** `pg_cancel_backend()` and `statement_timeout` abort
  in-flight HTTP requests promptly and cancel the query on the Trino side.
  Stopping a scan early (e.g. a local `LIMIT`) cancels the remote query too.
* **Robust protocol handling.** Follows `nextUri` paging, retries on HTTP 503
  as the protocol requires, requests full-precision timestamps, and surfaces
  Trino error names, types and query ids in PostgreSQL error messages.
* Optional HTTPS with certificate verification and HTTP basic authentication.

Supported PostgreSQL versions: 14 through 17 (developed and tested on 16).
Any reasonably recent Trino works; the test-suite uses Trino 476.

## Building and installing

You need the PostgreSQL server development headers (`postgresql-server-dev-NN`
on Debian/Ubuntu, `postgresqlNN-devel` on RPM systems) and libcurl headers
(`libcurl4-openssl-dev` / `libcurl-devel`).

```sh
make                      # uses pg_config from PATH
sudo make install
# or, for a specific installation:
make PG_CONFIG=/usr/lib/postgresql/16/bin/pg_config && sudo make install PG_CONFIG=...
```

If `make` fails with `clang-NN: No such file or directory`, your PostgreSQL
packages were built with LLVM JIT support and expect clang for the optional
bitcode files. Either install the matching `clang` package or build without
it: `make with_llvm=no && sudo make with_llvm=no install`.

Then, in each database: `CREATE EXTENSION trino_fdw;`

## Setting up

### Server

```sql
CREATE SERVER trino FOREIGN DATA WRAPPER trino_fdw OPTIONS (
  host 'trino.example.com',   -- required
  port '8443',                -- default 8080
  ssl 'true',                 -- use https (default false)
  catalog 'hive',             -- default catalog for tables on this server
  schema 'default'            -- default schema for tables on this server
);
```

| Option | Default | Meaning |
|---|---|---|
| `host` | — | Trino coordinator host name (required) |
| `port` | `8080` | Coordinator port |
| `ssl` | `false` | Use HTTPS |
| `ssl_verify` | `true` | Verify the server certificate when `ssl` is on |
| `ca_file` | — | CA bundle used to verify the server certificate |
| `catalog` | — | Catalog used for tables that do not set their own |
| `schema` | — | Schema used for tables that do not set `schema_name` |
| `source` | `trino_fdw` | Value of the `X-Trino-Source` header (shows up in Trino's query history) |
| `session_properties` | — | Comma-separated `name=value` list sent as `X-Trino-Session` |
| `client_tags` | — | Comma-separated list sent as `X-Trino-Client-Tags` (resource groups) |
| `spooling` | `true` | Request the spooled client protocol; see below |
| `connect_timeout` | `10` | Seconds to wait when connecting |
| `request_timeout` | `0` | Seconds a single HTTP request may take (0 = unlimited) |
| `startup_cost` | `100` | Planner cost of starting a Trino query |
| `tuple_cost` | `0.2` | Planner cost per row transferred |
| `default_rows` | `10000` | Row estimate for tables that have not been `ANALYZE`d |
| `batch_size` | `500` | Rows per `INSERT` statement sent to Trino |

### User mapping

```sql
CREATE USER MAPPING FOR alice SERVER trino OPTIONS (user 'alice', password 'secret');
CREATE USER MAPPING FOR PUBLIC SERVER trino OPTIONS (user 'readonly');
```

| Option | Default | Meaning |
|---|---|---|
| `user` | the PostgreSQL role name | Sent as `X-Trino-User` |
| `password` | — | Enables HTTP basic authentication (Trino requires HTTPS for this) |

A user mapping is optional: without one the current PostgreSQL role name is
used as the Trino user.

### Foreign tables

```sql
CREATE FOREIGN TABLE orders (
  id        bigint,
  customer  text    OPTIONS (column_name 'customer_name'),
  total     numeric(12,2),
  placed_at timestamptz
) SERVER trino OPTIONS (catalog 'hive', schema_name 'sales', table_name 'orders');
```

| Table option | Meaning |
|---|---|
| `catalog` | Overrides the server's catalog |
| `schema_name` | Overrides the server's schema |
| `table_name` | Remote table name (default: the foreign table's name) |
| `batch_size` | Overrides the server's insert batch size |
| `default_rows` | Row estimate before `ANALYZE` |

| Column option | Meaning |
|---|---|
| `column_name` | Remote column name (default: the local column name) |

Identifiers are always sent double-quoted, so use the exact (usually
lower-case) names Trino reports.

### IMPORT FOREIGN SCHEMA

```sql
IMPORT FOREIGN SCHEMA tiny FROM SERVER trino INTO tpch_tiny;
IMPORT FOREIGN SCHEMA tiny LIMIT TO (orders, lineitem) FROM SERVER trino INTO tpch_tiny;
IMPORT FOREIGN SCHEMA tiny EXCEPT (lineitem) FROM SERVER trino INTO tpch_tiny;
-- a server without a default catalog:
IMPORT FOREIGN SCHEMA tiny FROM SERVER trino INTO tpch_tiny OPTIONS (catalog 'tpch');
```

## Type mapping

Values arrive as JSON and are converted through the PostgreSQL input function
of the local column type, so you may declare any compatible type. `IMPORT
FOREIGN SCHEMA` uses this mapping:

| Trino | PostgreSQL |
|---|---|
| `boolean` | `boolean` |
| `tinyint`, `smallint` | `smallint` |
| `integer` | `integer` |
| `bigint` | `bigint` |
| `real` | `real` |
| `double` | `double precision` |
| `decimal(p,s)` | `numeric(p,s)` |
| `varchar(n)` / `varchar` | `varchar(n)` / `text` |
| `char(n)` | `char(n)` |
| `varbinary` | `bytea` |
| `json` | `jsonb` |
| `date` | `date` |
| `time(p)` [with time zone] | `time(p)` [with time zone] (p capped at 6) |
| `timestamp(p)` [with time zone] | `timestamp(p)` [with time zone] (p capped at 6) |
| `interval year to month`, `interval day to second` | `interval` |
| `uuid` | `uuid` |
| `ipaddress` | `inet` |
| `array(T)` for scalar `T` | `T[]` |
| `array(array(...))`, `array(row(...))`, `map(...)`, `row(...)` | `jsonb` |
| anything else (`Geometry`, `HyperLogLog`, ...) | `text` |

Notes:

* `NaN`, `Infinity` and `-Infinity` doubles round-trip correctly.
* Negative day-to-second intervals (`-1 02:00:00` in Trino) are converted so
  the sign applies to the whole value.
* `row` values arrive as JSON arrays of field values, `map` values as JSON
  objects.

## What gets pushed down

`EXPLAIN` shows the generated SQL:

```
EXPLAIN (COSTS OFF) SELECT name FROM nation WHERE regionkey = 1 ORDER BY nationkey DESC LIMIT 3;
 Foreign Scan on nation
   Trino SQL: SELECT "nationkey", "name" FROM "tpch"."tiny"."nation" WHERE ("regionkey" = 1) ORDER BY "nationkey" DESC NULLS FIRST LIMIT 3
```

Pushed to Trino when all operands are of supported types:

* `= <> < <= > >=` (ordering comparisons only on non-collatable types),
  `+ - * / %` on numbers, `||` on strings, `LIKE` / `NOT LIKE`
  (with `ESCAPE '\'`, matching PostgreSQL semantics), `IS [NOT] NULL`,
  `IS DISTINCT FROM`, `AND/OR/NOT`, `x IN (...)` / `x NOT IN (...)`
* `COALESCE`, `GREATEST`, `LEAST`, `lower`, `upper`, `length`, `trim`,
  `ltrim`, `rtrim`, `substr`, `abs`, `round`, `ceil`, `floor`, `sqrt`
* Exact widening casts (`smallint` → `integer` → `bigint` → `numeric`,
  `real` → `double precision`); all other casts stay local
* `ORDER BY` on numeric, date/time and boolean columns; `LIMIT`/`OFFSET`
  with constant values when every WHERE clause was pushed down
* Joins (`INNER`, `LEFT`, `RIGHT`, `FULL`) between foreign tables of the same
  server when all join conditions are shippable; WHERE conditions that are
  not shippable can still be evaluated locally after an inner join. Joins
  are kept local when a whole-row reference is needed, for `SELECT ... FOR
  UPDATE`, and when the query is an UPDATE/DELETE joining the table.
* Aggregates: `count(*)`, `count(x)`, `sum`, `min`, `max` (numbers, dates,
  times, timestamps), `avg(double precision)`, `bool_and`, `bool_or`,
  `every`, each with optional `DISTINCT` and `FILTER (WHERE ...)`;
  `GROUP BY` on shippable expressions; `HAVING`. An aggregate whose result
  type or rounding would differ (`avg` over integers or numerics, `min`/`max`
  over text) keeps the whole aggregation local.

```
EXPLAIN (COSTS OFF) SELECT c.mktsegment, count(*) FROM customer c JOIN orders o ON o.custkey = c.custkey GROUP BY 1 ORDER BY 2 DESC LIMIT 3;
 Foreign Scan
   Relations: Aggregate on ((public.customer c) INNER JOIN (public.orders o))
   Trino SQL: SELECT r1."mktsegment", count(*) FROM ("tpch"."tiny"."customer" r1 INNER JOIN "tpch"."tiny"."orders" r2 ON (r1."custkey" = r2."custkey")) GROUP BY r1."mktsegment" ORDER BY count(*) DESC NULLS FIRST LIMIT 3
```

Kept local on purpose: anything involving mutable functions (`now()`,
`random()`), regular expressions, `ILIKE`, text ordering (collations differ),
infinite dates/timestamps, `numeric` `NaN`, dates BC, user-defined operators
and functions, grouping sets, ordered-set and `ORDER BY` aggregates,
`string_agg`/`array_agg`, window functions, and semi/anti joins.

Pushdown decisions are cost based, like any other plan choice. Run `ANALYZE`
on the foreign tables so the planner knows their sizes: without statistics
a join is assumed to produce many rows and may be done locally.

## Writing data

```sql
INSERT INTO orders VALUES (1, 'acme', 99.50, now());
INSERT INTO orders SELECT ... ;          -- sent in batches of batch_size rows
COPY orders FROM '/tmp/orders.csv' CSV;  -- also batched
```

```sql
UPDATE orders SET total = total * 1.1, note = upper(note) WHERE placed_at < DATE '2020-01-01';
DELETE FROM orders WHERE customer IS NULL OR id IN (1, 2, 3);
```

Trino has no row identifiers, so `UPDATE` and `DELETE` only work when the
whole statement can be sent to Trino as one `UPDATE`/`DELETE`: the `WHERE`
clause and the new values must be shippable expressions (parameters and
sub-selects are fine), the statement cannot join other tables, and
`RETURNING` and row-level triggers are not supported. Anything else is
rejected with an explanatory error. The connector behind the table must
support row-level modification (Iceberg, Delta Lake, Hive ACID, ...); the row
count reported is whatever Trino reports.

Each statement and each INSERT batch is one autocommitted Trino query.
**Rolling back the PostgreSQL transaction does not undo changes already sent
to Trino.** `INSERT ... ON CONFLICT` is rejected.

Trino intervals have millisecond precision, so microseconds in inserted
`interval` values are truncated.

## Passthrough functions

```sql
-- any query, with a column definition list:
SELECT * FROM trino_query('trino', $$
  SELECT regionkey, count(*) FROM tpch.tiny.nation GROUP BY 1
$$) AS t(regionkey bigint, n bigint);

-- DDL / DML; returns the update count or -1:
SELECT trino_execute('trino', 'CREATE TABLE memory.default.t AS SELECT 1 AS x');
SELECT trino_execute('trino', 'DELETE FROM memory.default.t WHERE x = 1');
```

Both require `USAGE` privilege on the foreign server and use the caller's
user mapping.

## Statistics

`ANALYZE foreign_table` runs `SELECT count(*)` and a `TABLESAMPLE BERNOULLI`
query on Trino and stores regular PostgreSQL statistics, which the planner
then uses for row estimates. Until then the `default_rows` option is used.

## Spooled protocol

By default the FDW asks for Trino's spooled client protocol
(`X-Trino-Query-Data-Encoding: json`). On a server with spooling enabled,
large results arrive as segments: small ones inline, large ones written by
Trino to object storage and fetched by the FDW directly from there (with the
headers Trino supplies), then acknowledged so Trino can delete them. Servers
without spooling simply answer with the classic inline protocol.

This means that **the PostgreSQL server must be able to reach the storage
Trino spools to** (or the coordinator, when Trino is configured with
`protocol.spooling.retrieval-mode=COORDINATOR_PROXY`). If it cannot, set
`spooling 'false'` on the server. Compressed encodings are not requested.

## Security notes

* With `ssl 'false'` everything, including the `X-Trino-User` header, travels
  in clear text. Use `ssl 'true'` (and a `password`) for anything but a
  trusted network.
* Passwords in user mappings are visible to superusers and to the mapping's
  owner, as with every FDW.
* Trino authorization applies to the configured Trino user; PostgreSQL
  privileges on the foreign table apply on top.

## Testing

The regression suite runs against a small Python mock of the Trino REST
protocol (`test/mock_trino.py`), so it needs no Trino and is deterministic.
With the extension installed and a superuser connection available through the
usual `PG*` environment variables:

```sh
test/run_tests.sh          # starts the mock, runs `make installcheck`
```

The regression runner starts a second mock instance that answers with
spooled-protocol segments, so that code path is covered without object
storage.

The expected output exists in three variants, picked automatically by
pg_regress: the base file (PostgreSQL 16), `trino_fdw_1.out` (14 and 15:
no pre-sorting for ordered-set aggregates, COPY not batched) and
`trino_fdw_2.out` (17 and later: EXPLAIN format of InitPlans).

`test/integration.sh` runs end-to-end checks against a real Trino with the
`tpch` and `memory` catalogs (for example `docker run -p 8080:8080
trinodb/trino`); when the server also has an `iceberg` catalog the
UPDATE/DELETE checks run too. The GitHub Actions workflow runs the regression
suite on PostgreSQL 14–17 and the integration script against Trino 476.

## Limitations

* No transactions spanning Trino statements; every statement autocommits.
* Semi and anti joins (`EXISTS`, `IN (subquery)`) and window functions are
  evaluated locally.
* `UPDATE`/`DELETE` need the whole statement to be shippable, as described
  above.
* Compressed spooling encodings (`json+zstd`, `json+lz4`) are not used.
