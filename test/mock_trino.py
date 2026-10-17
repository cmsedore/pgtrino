#!/usr/bin/env python3
"""
A small stand-in for a Trino coordinator, used by the regression tests.

It implements just enough of the Trino client REST protocol for trino_fdw:

  POST   /v1/statement                 submit SQL, get the first page
  GET    /v1/statement/executing/...   follow nextUri
  DELETE /v1/statement/executing/...   cancel

It does not execute SQL.  Instead it serves fixed tables and records every
statement it receives in a "sqllog" table, so tests can assert on exactly
which SQL the FDW generated and on how it handles paging, errors and 503s.

Tables (catalog "mock", schema "s1"):
  t1          three rows covering most value types, served in two pages
  t1_slow     like t1, but two empty "QUEUED" pages precede the data
  t1_flaky    like t1, but the first nextUri fetch answers 503 once
  boom        fails immediately with a Trino error
  boom_late   returns one page of data, then fails
  sqllog      (n integer, sql varchar) statements received since last read
  empty       zero rows
"""
import base64
import hashlib
import json
import re
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HOST = "127.0.0.1"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18080

T1_COLUMNS = [
    ("id", "integer"),
    ("name", "varchar(10)"),
    ("val", "double"),
    ("created", "timestamp(6)"),
    ("flag", "boolean"),
    ("tags", "array(varchar)"),
    ("attrs", "map(varchar, integer)"),
    ("big", "array(array(integer))"),
    ("bin", "varbinary"),
    ("dec", "decimal(10,2)"),
    ("d", "date"),
    ("u", "uuid"),
    ("ts_tz", "timestamp(3) with time zone"),
    ("dur", "interval day to second"),
]
T1_ROWS = [
    [1, "alpha", 1.5, "2024-01-02 03:04:05.000000", True, ["x", "y"], {"a": 1},
     [[1], [2, 3]], base64.b64encode(b"\xde\xad\xbe\xef").decode(), "12.34",
     "2024-01-02", "12345678-1234-5678-1234-567812345678",
     "2024-01-02 03:04:05.123 UTC", "0 00:00:01.500"],
    [2, "beta", "NaN", None, False, [], {}, None, None, "-0.01", None, None,
     "2024-06-01 12:00:00.000 America/New_York", "-1 02:03:04.000"],
    [3, None, "-Infinity", "1999-12-31 23:59:59.999999", None, None, None,
     None, "", "0.00", "1999-12-31", None, None, None],
]
T2_COLUMNS = [("k", "bigint"), ("v", "decimal(10,2)"), ("note", "varchar")]
T2_ROWS = [[10, "1.00", "ten"], [20, "2.00", None]]

SCHEMA_TABLES = {
    "t1": T1_COLUMNS,
    "t2": T2_COLUMNS,
    "weird name": [("Col One", "integer"), ("x\"y", "varchar")],
}

lock = threading.Lock()
sql_log = []            # every statement received, in order
queries = {}            # query id -> list of remaining responses


def col_desc(name, typ):
    return {"name": name, "type": typ, "typeSignature": {"rawType": typ.split("(")[0], "arguments": []}}


def base_url():
    return "http://%s:%d" % (HOST, PORT)


def make_page(qid, token, columns, data, more, error=None, update=None):
    page = {"id": qid, "infoUri": base_url() + "/ui/query.html?" + qid,
            "stats": {"state": "RUNNING" if more else ("FAILED" if error else "FINISHED")}}
    if columns is not None:
        page["columns"] = [col_desc(n, t) for n, t in columns]
    if data is not None:
        page["data"] = data
    if more:
        page["nextUri"] = "%s/v1/statement/executing/%s/%s/%d" % (base_url(), qid, "tok", token)
    if error:
        page["error"] = error
    if update:
        page["updateType"], page["updateCount"] = update
    return page


def parse_select(sql):
    """Return (column list, table name) for the SELECT statements the FDW emits."""
    m = re.match(r'^\s*SELECT\s+(.*?)\s+FROM\s+((?:"[^"]*"\.?)+|\S+)', sql, re.S)
    if not m:
        return None, None
    cols = []
    depth = 0
    cur = ""
    for ch in m.group(1):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            cols.append(cur.strip())
            cur = ""
        else:
            cur += ch
    cols.append(cur.strip())
    table = m.group(2).split(".")[-1].strip('"').replace('""', '"')
    return cols, table


def project(columns, rows, selected):
    """Pick the selected columns out of a table."""
    names = [n for n, _ in columns]
    out_cols = []
    idx = []
    for sel in selected:
        if re.match(r'^count\(\*\)$', sel, re.I):
            return [("_col0", "bigint")], [[len(rows)]]
        if sel == "1":
            out_cols.append(("_col0", "integer"))
            idx.append(None)
            continue
        name = sel.strip('"').replace('""', '"')
        if name not in names:
            raise KeyError(name)
        out_cols.append(columns[names.index(name)])
        idx.append(names.index(name))
    out_rows = [[1 if i is None else r[i] for i in idx] for r in rows]
    return out_cols, out_rows


def error_obj(message, name="GENERIC_INTERNAL_ERROR", etype="INTERNAL_ERROR", code=65536):
    return {"message": message, "errorCode": code, "errorName": name, "errorType": etype,
            "errorLocation": {"lineNumber": 1, "columnNumber": 8},
            "failureInfo": {"type": "io.trino.spi.TrinoException", "message": message}}


def plan_query(sql):
    """Build the sequence of responses for a statement."""
    s = sql.strip()
    # deterministic ids, so error messages in test output are stable
    qid = "mock_" + hashlib.sha1(s.encode()).hexdigest()[:8]
    pages = []

    if re.match(r"^INSERT\s", s, re.I):
        nrows = len(re.findall(r"\)\s*,\s*\(", s)) + 1
        if "boom" in s:
            pages.append(make_page(qid, 1, None, None, False,
                                   error=error_obj("insert failed on purpose", "CONSTRAINT_VIOLATION", "USER_ERROR", 1)))
        else:
            pages.append(make_page(qid, 1, [("rows", "bigint")], [[nrows]], False, update=("INSERT", nrows)))
        return qid, pages

    if re.match(r"^(CREATE|DROP|DELETE|UPDATE|SET)\s", s, re.I):
        pages.append(make_page(qid, 1, [("result", "boolean")], [[True]], False, update=(s.split()[0].upper(), None)))
        return qid, pages

    if "information_schema.columns" in s:
        m = re.search(r"table_schema = '([^']*)'", s)
        schema = m.group(1) if m else ""
        limit = re.search(r"table_name IN \((.*?)\)", s)
        wanted = None
        if limit:
            wanted = [x.strip().strip("'") for x in limit.group(1).split(",")]
        rows = []
        if schema == "s1":
            for tname in sorted(SCHEMA_TABLES):
                if wanted is not None and tname not in wanted:
                    continue
                for cname, ctype in SCHEMA_TABLES[tname]:
                    rows.append([tname, cname, ctype])
        cols = [("table_name", "varchar"), ("column_name", "varchar"), ("data_type", "varchar")]
        pages.append(make_page(qid, 1, cols, rows, False))
        return qid, pages

    selected, table = parse_select(s)
    if selected is None:
        pages.append(make_page(qid, 1, None, None, False,
                               error=error_obj("mock cannot parse: " + s, "SYNTAX_ERROR", "USER_ERROR", 1)))
        return qid, pages

    if table == "boom":
        pages.append(make_page(qid, 1, None, None, False,
                               error=error_obj("table boom always fails", "TABLE_NOT_FOUND", "USER_ERROR", 44)))
        return qid, pages

    if table == "sqllog":
        # reading the log consumes it: each check shows only new statements
        with lock:
            rows = [[i + 1, q] for i, q in enumerate(sql_log)]
            del sql_log[:]
        cols, rows = project([("n", "integer"), ("sql", "varchar")], rows, selected)
        pages.append(make_page(qid, 1, cols, rows, False))
        return qid, pages

    if table == "empty":
        cols, rows = project(T2_COLUMNS, [], selected)
        pages.append(make_page(qid, 1, cols, rows, True))
        pages.append(make_page(qid, 2, cols, [], False))
        return qid, pages

    if table in ("t1", "t1_slow", "t1_flaky", "boom_late"):
        try:
            cols, rows = project(T1_COLUMNS, T1_ROWS, selected)
        except KeyError as e:
            pages.append(make_page(qid, 1, None, None, False,
                                   error=error_obj("Column '%s' cannot be resolved" % e.args[0], "COLUMN_NOT_FOUND", "USER_ERROR", 47)))
            return qid, pages
        tok = 1
        if table == "t1_slow":
            for _ in range(2):
                p = make_page(qid, tok, None, None, True)
                p["stats"]["state"] = "QUEUED"
                pages.append(p)
                tok += 1
        if any(re.match(r"^count\(\*\)$", c, re.I) for c in selected):
            pages.append(make_page(qid, tok, cols, rows, False))
            return qid, pages
        pages.append(make_page(qid, tok, cols, rows[:2], True))
        tok += 1
        if table == "t1_flaky":
            # the first nextUri fetch is answered with 503 once
            pages.append({"__status__": 503})
        if table == "boom_late":
            pages.append(make_page(qid, tok, cols, None, False,
                                   error=error_obj("worker died", "REMOTE_TASK_ERROR", "INTERNAL_ERROR", 65540)))
        else:
            pages.append(make_page(qid, tok, cols, rows[2:], True))
            tok += 1
            pages.append(make_page(qid, tok, cols, None, False))
        return qid, pages

    if table == "t2":
        try:
            cols, rows = project(T2_COLUMNS, T2_ROWS, selected)
        except KeyError as e:
            pages.append(make_page(qid, 1, None, None, False,
                                   error=error_obj("Column '%s' cannot be resolved" % e.args[0], "COLUMN_NOT_FOUND", "USER_ERROR", 47)))
            return qid, pages
        pages.append(make_page(qid, 1, cols, rows, False))
        return qid, pages

    pages.append(make_page(qid, 1, None, None, False,
                           error=error_obj("Table 'mock.s1.%s' does not exist" % table, "TABLE_NOT_FOUND", "USER_ERROR", 44)))
    return qid, pages


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def send_json(self, obj, status=200):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        if self.path != "/v1/statement":
            self.send_json({"error": "not found"}, 404)
            return
        n = int(self.headers.get("Content-Length", "0"))
        sql = self.rfile.read(n).decode()
        if self.headers.get("X-Trino-User") is None:
            self.send_json({"message": "X-Trino-User missing"}, 400)
            return
        if self.headers.get("X-Trino-Source") == "reject-me":
            self.send_json({"message": "source rejected"}, 403)
            return
        qid, pages = plan_query(sql)
        with lock:
            if "sqllog" not in sql:
                sql_log.append(sql)
            queries[qid] = pages
        self.serve_next(qid)

    def serve_next(self, qid):
        with lock:
            pages = queries.get(qid)
            if not pages:
                self.send_json({"message": "unknown query"}, 410)
                return
            page = pages.pop(0)
        if "__status__" in page:
            self.send_json({"message": "busy"}, page["__status__"])
            return
        self.send_json(page)

    def do_GET(self):
        m = re.match(r"^/v1/statement/executing/([^/]+)/", self.path)
        if not m:
            self.send_json({"message": "not found"}, 404)
            return
        self.serve_next(m.group(1))

    def do_DELETE(self):
        m = re.match(r"^/v1/statement/executing/([^/]+)/", self.path)
        if m:
            with lock:
                queries.pop(m.group(1), None)
                sql_log.append("<cancel %s>" % m.group(1))
        self.send_response(204)
        self.send_header("Content-Length", "0")
        self.end_headers()


if __name__ == "__main__":
    server = ThreadingHTTPServer((HOST, PORT), Handler)
    print("mock trino listening on %s:%d" % (HOST, PORT), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
