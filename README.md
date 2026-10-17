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
* **ORDER BY, LIMIT and OFFSET pushdown**, including `ORDER BY ... LIMIT n`
  which Trino executes as a cheap TopN. Collation-dependent text ordering is
  deliberately kept local.
* **Query parameters.** Prepared-statement parameters and sub-select results
  are sent as literals at execution time.
* **`IMPORT FOREIGN SCHEMA`** with a full Trino → PostgreSQL type mapping.
* **INSERT** (including `COPY ... FROM` and `INSERT ... RETURNING`), batched
  into multi-row `INSERT ... VALUES` statements.
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
| `connect_timeout` | `10` | Seconds to wait when connecting |
| `request_timeout` | `0` | Seconds a single HTTP request may take (0 = unlimited) |
| `startup_cost` | `1000` | Planner cost of starting a Trino query |
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

Kept local on purpose: anything involving mutable functions (`now()`,
`random()`), regular expressions, `ILIKE`, text ordering (collations differ),
infinite dates/timestamps, `numeric` `NaN`, dates BC, user-defined operators
and functions, aggregates, joins and window functions.

Aggregates and joins are computed in PostgreSQL after fetching the rows they
need. When that is too slow, let Trino do the work with `trino_query()`.

## Writing data

```sql
INSERT INTO orders VALUES (1, 'acme', 99.50, now());
INSERT INTO orders SELECT ... ;          -- sent in batches of batch_size rows
COPY orders FROM '/tmp/orders.csv' CSV;  -- also batched
```

Each batch is one autocommitted Trino `INSERT`. **Rolling back the PostgreSQL
transaction does not undo rows already sent to Trino.** `UPDATE`, `DELETE` and
`INSERT ... ON CONFLICT` on foreign tables are rejected; use
`trino_execute()` for DML Trino supports.

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

`test/integration.sh` runs end-to-end checks against a real Trino with the
`tpch` and `memory` catalogs (for example `docker run -p 8080:8080
trinodb/trino`). The GitHub Actions workflow runs the regression suite on
PostgreSQL 14–17 and the integration script against Trino 476.

## Limitations and ideas for later

* No join, aggregate or parameterized-path pushdown yet.
* No `UPDATE`/`DELETE`, no transactions spanning Trino statements.
* The Trino spooled client protocol is not used; results are streamed inline
  as JSON pages, which is fine for the row counts a PostgreSQL query usually
  pulls but not ideal for bulk extraction of very large tables.
