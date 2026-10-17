#!/usr/bin/env python3
"""
A small stand-in for a Trino coordinator, used by the regression tests.

It implements just enough of the Trino client REST protocol for trino_fdw:

  POST   /v1/statement                 submit SQL, get the first page
  GET    /v1/statement/executing/...   follow nextUri
  DELETE /v1/statement/executing/...   cancel

It does not really execute SQL.  It serves fixed tables, evaluates just
enough of the SELECT list (column references, the handful of aggregates the
FDW pushes down, GROUP BY, ORDER BY, OFFSET/LIMIT) to return plausibly
shaped rows, ignores WHERE clauses, and records every statement in a
"sqllog" table, so tests can assert on exactly which SQL the FDW generated
and on how it handles paging, errors and 503s.

Tables (catalog "mock", schema "s1"):
  t1          three rows covering most value types, served in two pages
  t1_slow     like t1, but two empty "QUEUED" pages precede the data
  t1_flaky    like t1, but the first nextUri fetch answers 503 once
  t2          two rows (k bigint, v decimal(10,2), note varchar)
  boom        fails immediately with a Trino error
  boom_late   returns one page of data, then fails
  sqllog      (n integer, sql varchar) statements received since last read
  empty       zero rows

Joins (FROM (... JOIN ...) with r<N> aliases) return the row-wise zip of
the joined tables.  UPDATE and DELETE statements report an update count of 2.
When started with "spooling" as the second argument the data of the first
page is returned as spooled-protocol segments (one inline, one spooled
through this same server) to exercise that code path.
"""
import base64
import hashlib
import json
import re
import sys
import threading
from decimal import Decimal
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HOST = "127.0.0.1"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18080
SPOOLING = len(sys.argv) > 2 and sys.argv[2] == "spooling"

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

TABLES = {
    "t1": (T1_COLUMNS, T1_ROWS),
    "t1_slow": (T1_COLUMNS, T1_ROWS),
    "t1_flaky": (T1_COLUMNS, T1_ROWS),
    "boom_late": (T1_COLUMNS, T1_ROWS),
    "t2": (T2_COLUMNS, T2_ROWS),
    "empty": (T2_COLUMNS, []),
    "weird name": ([("Col One", "integer"), ("x\"y", "varchar")], [[1, "a"]]),
}
SCHEMA_TABLES = {
    "t1": T1_COLUMNS,
    "t2": T2_COLUMNS,
    "weird name": TABLES["weird name"][0],
}

lock = threading.Lock()
sql_log = []            # every statement received, in order
queries = {}            # query id -> list of remaining responses
spooled = {}            # segment id -> encoded bytes


def col_desc(name, typ):
    return {"name": name, "type": typ,
            "typeSignature": {"rawType": typ.split("(")[0], "arguments": []}}


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


def spool(qid, rows):
    """Encode rows as spooled-protocol segments: one inline, one spooled."""
    half = max(1, len(rows) // 2)
    first, second = rows[:half], rows[half:]
    seg_id = qid + "-" + hashlib.sha1(json.dumps(rows).encode()).hexdigest()[:6]
    with lock:
        spooled[seg_id] = json.dumps(second).encode()
    segments = [{"type": "inline",
                 "data": base64.b64encode(json.dumps(first).encode()).decode(),
                 "metadata": {"rowOffset": 0, "rowsCount": len(first)}}]
    if second:
        segments.append({"type": "spooled",
                         "uri": "%s/v1/spooled/download/%s" % (base_url(), seg_id),
                         "ackUri": "%s/v1/spooled/ack/%s" % (base_url(), seg_id),
                         "headers": {"X-Mock-Segment": [seg_id]},
                         "metadata": {"rowOffset": half, "rowsCount": len(second)}})
    return {"encoding": "json", "segments": segments}


# ---------------------------------------------------------------------------
# a tiny evaluator for the SQL the FDW generates
# ---------------------------------------------------------------------------

def split_top_level(text, sep=","):
    parts, depth, cur, quote = [], 0, "", None
    for ch in text:
        if quote:
            cur += ch
            if ch == quote:
                quote = None
            continue
        if ch in "'\"":
            quote = ch
        elif ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == sep and depth == 0:
            parts.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        parts.append(cur.strip())
    return parts


def unquote_ident(s):
    s = s.strip()
    if s.startswith('"') and s.endswith('"'):
        return s[1:-1].replace('""', '"')
    return s


class Scope:
    """Resolves column references to (alias, column index) for a FROM clause."""

    def __init__(self, from_clause):
        # "cat"."sch"."tbl" r3   or   "cat"."sch"."tbl"
        self.aliases = {}       # alias -> table name
        self.tables = []        # table names in order
        for m in re.finditer(r'"mock"\."s1"\.("(?:[^"]|"")+")(?:\s+(r\d+))?', from_clause):
            table = unquote_ident(m.group(1))
            alias = m.group(2) or "r"
            self.aliases[alias] = table
            self.tables.append((alias, table))
        if not self.tables:
            # an unqualified, possibly unquoted name (passthrough queries)
            m = re.match(r'\s*("(?:[^"]|"")+"|\w+)(?:\s+(r\d+))?', from_clause)
            if m:
                self.aliases[m.group(2) or "r"] = unquote_ident(m.group(1))
                self.tables.append((m.group(2) or "r", unquote_ident(m.group(1))))

    def first_table(self):
        return self.tables[0][1] if self.tables else None

    def rows(self):
        """Zip the joined tables row-wise; dict alias -> row."""
        table_rows = []
        for alias, table in self.tables:
            if table not in TABLES:
                raise KeyError("table:" + table)
            table_rows.append((alias, TABLES[table][1]))
        if not table_rows:
            return []
        n = min(len(r) for _, r in table_rows) if len(table_rows) > 1 else len(table_rows[0][1])
        return [{alias: rows[i] for alias, rows in table_rows} for i in range(n)]

    def column(self, ref):
        """'r1."k"' or '"k"' -> (alias, index, (name, type))."""
        m = re.match(r'^(?:(r\d+)\.)?("(?:[^"]|"")+")$', ref.strip())
        if not m:
            return None
        alias = m.group(1) or (self.tables[0][0] if self.tables else "r")
        name = unquote_ident(m.group(2))
        table = self.aliases.get(alias)
        if table is None or table not in TABLES:
            raise KeyError("table:" + str(table))
        cols = TABLES[table][0]
        for i, (cname, ctype) in enumerate(cols):
            if cname == name:
                return alias, i, (cname, ctype)
        raise KeyError("column:" + name)


AGG_RE = re.compile(r"^(count|sum|min|max|avg|bool_and|bool_or)\((.*)\)(?:\s+FILTER\s*\(.*\))?$", re.I | re.S)


def to_number(v):
    if isinstance(v, bool) or v is None:
        return None
    if isinstance(v, str):
        try:
            d = Decimal(v)
        except Exception:
            return None
        return d if d.is_finite() else None
    if isinstance(v, float) and v != v:
        return None
    return v


def format_number(v, as_decimal):
    if v is None:
        return None
    if as_decimal:
        return str(Decimal(v).quantize(Decimal("0.01")))
    if isinstance(v, Decimal):
        return int(v) if v == v.to_integral_value() else float(v)
    return v


def strip_parens(text):
    text = text.strip()
    while text.startswith("(") and text.endswith(")"):
        depth = 0
        for i, ch in enumerate(text):
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0 and i != len(text) - 1:
                    return text
        text = text[1:-1].strip()
    return text


def eval_item(item, scope, group_rows):
    """Evaluate one SELECT-list item over a group of rows -> (type, value)."""
    item = strip_parens(item)
    if re.match(r"^-?\d+$", item):
        return "integer", int(item)
    m = re.match(r"^(DECIMAL|DOUBLE|REAL) '([^']*)'$", item)
    if m:
        return ("decimal(38,2)" if m.group(1) == "DECIMAL" else "double"), \
            (str(Decimal(m.group(2))) if m.group(1) == "DECIMAL" else float(m.group(2)))
    # binary arithmetic at the top level: a op b
    parts = re.split(r"\s+([-+*/])\s+", item)
    if len(parts) == 3:
        depth, ok = 0, True
        for ch in parts[0]:
            depth += (ch == "(") - (ch == ")")
        ok = depth == 0
        if ok:
            lt, lv = eval_item(parts[0], scope, group_rows)
            rt, rv = eval_item(parts[2], scope, group_rows)
            if lv is None or rv is None:
                return lt, None
            a, b = Decimal(str(lv)), Decimal(str(rv))
            r = {"+": a + b, "-": a - b, "*": a * b, "/": (a / b if b else None)}[parts[1]]
            is_dec = lt.startswith("decimal") or rt.startswith("decimal")
            return lt, format_number(r, is_dec)
    m = AGG_RE.match(item)
    if m:
        func, arg = m.group(1).lower(), m.group(2).strip()
        distinct = arg.upper().startswith("DISTINCT ")
        if distinct:
            arg = arg[9:].strip()
        if arg == "*":
            return "bigint", len(group_rows)
        col = scope.column(arg)
        values = [r[col[0]][col[1]] for r in group_rows]
        non_null = [v for v in values if v is not None]
        if distinct:
            non_null = list({json.dumps(v): v for v in non_null}.values())
        ctype = col[2][1]
        if func == "count":
            return "bigint", len(non_null)
        if func in ("bool_and", "bool_or"):
            return "boolean", (all(non_null) if func == "bool_and" else any(non_null)) if non_null else None
        nums = [to_number(v) for v in non_null]
        nums = [v for v in nums if v is not None]
        is_dec = ctype.startswith("decimal")
        if func == "sum":
            return ("decimal(38,2)" if is_dec else ("bigint" if ctype in ("integer", "bigint", "smallint") else "double")), \
                format_number(sum(nums), is_dec) if nums else None
        if func == "avg":
            return "double", (float(sum(nums)) / len(nums)) if nums else None
        if func in ("min", "max"):
            if not non_null:
                return ctype, None
            if nums and len(nums) == len(non_null):
                return ctype, format_number(min(nums) if func == "min" else max(nums), is_dec)
            return ctype, (min(non_null) if func == "min" else max(non_null))
    col = scope.column(item)
    if col is None:
        raise KeyError("expr:" + item)
    return col[2][1], group_rows[0][col[0]][col[1]] if group_rows else None


def evaluate_select(sql):
    """Return (columns, rows) for a SELECT the FDW generated."""
    m = re.match(r"^\s*SELECT\s+(.*?)\s+FROM\s+(.*)$", sql, re.S)
    if not m:
        raise ValueError("not a select")
    select_list, rest = m.group(1), m.group(2)
    # peel trailing clauses off the FROM part
    limit = offset = None
    order_by = group_by = None
    mm = re.search(r"\s+LIMIT\s+(\d+)\s*$", rest)
    if mm:
        limit, rest = int(mm.group(1)), rest[:mm.start()]
    mm = re.search(r"\s+OFFSET\s+(\d+)\s+ROWS\s*$", rest)
    if mm:
        offset, rest = int(mm.group(1)), rest[:mm.start()]
    mm = re.search(r"\s+ORDER BY\s+(.*)$", rest, re.S)
    if mm:
        order_by, rest = mm.group(1), rest[:mm.start()]
    mm = re.search(r"\s+HAVING\s+(.*)$", rest, re.S)
    if mm:
        rest = rest[:mm.start()]
    mm = re.search(r"\s+GROUP BY\s+(.*)$", rest, re.S)
    if mm:
        group_by, rest = mm.group(1), rest[:mm.start()]
    mm = re.search(r"\s+WHERE\s+(.*)$", rest, re.S)
    if mm:
        rest = rest[:mm.start()]
    from_clause = rest

    scope = Scope(from_clause)
    items = split_top_level(select_list)
    rows = scope.rows()
    is_agg = any(AGG_RE.match(i.strip()) for i in items)

    if is_agg:
        groups = {}
        keys = split_top_level(group_by) if group_by else []
        for r in rows:
            key = json.dumps([r[scope.column(k)[0]][scope.column(k)[1]] for k in keys])
            groups.setdefault(key, []).append(r)
        if not keys:
            groups = {"": rows}
        out_rows, types = [], None
        for grp in groups.values():
            vals = [eval_item(i, scope, grp) for i in items]
            types = [t for t, _ in vals]
            out_rows.append([v for _, v in vals])
        if types is None:
            types = [eval_item(i, scope, [])[0] for i in items]
    else:
        types = None
        out_rows = []
        for r in rows:
            vals = [eval_item(i, scope, [r]) for i in items]
            types = [t for t, _ in vals]
            out_rows.append([v for _, v in vals])
        if types is None:
            types = [eval_item(i, scope, [])[0] for i in items]

    columns = [("_col%d" % i, t) for i, t in enumerate(types)]

    if order_by:
        for spec in reversed(split_top_level(order_by)):
            desc = " DESC" in spec.upper()
            nulls_first = "NULLS FIRST" in spec.upper()
            expr = re.sub(r"\s+(ASC|DESC)(\s+NULLS\s+(FIRST|LAST))?\s*$", "", spec, flags=re.I).strip()
            idx = next((i for i, it in enumerate(items) if it.strip() == expr), None)

            def key(row, idx=idx, expr=expr):
                if idx is not None:
                    v = row[idx]
                else:
                    v = None
                if v is None:
                    return (0 if nulls_first else 1, 0)
                if isinstance(v, str):
                    try:
                        v = float(Decimal(v))
                    except Exception:
                        pass
                return (1 if nulls_first else 0, v if not isinstance(v, str) else v)
            try:
                out_rows.sort(key=key, reverse=desc)
            except TypeError:
                pass
    if offset:
        out_rows = out_rows[offset:]
    if limit is not None:
        out_rows = out_rows[:limit]
    return columns, out_rows, scope.first_table()


def error_obj(message, name="GENERIC_INTERNAL_ERROR", etype="INTERNAL_ERROR", code=65536):
    return {"message": message, "errorCode": code, "errorName": name, "errorType": etype,
            "errorLocation": {"lineNumber": 1, "columnNumber": 8},
            "failureInfo": {"type": "io.trino.spi.TrinoException", "message": message}}


def plan_query(sql, want_spooling=False):
    """Build the sequence of responses for a statement."""
    s = sql.strip()
    use_spooling = SPOOLING and want_spooling
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

    if re.match(r"^(UPDATE|DELETE)\s", s, re.I):
        if "boom" in s:
            pages.append(make_page(qid, 1, None, None, False,
                                   error=error_obj("This connector does not support modifying table rows", "NOT_SUPPORTED", "USER_ERROR", 13)))
        else:
            pages.append(make_page(qid, 1, [("rows", "bigint")], [[2]], False, update=(s.split()[0].upper(), 2)))
        return qid, pages

    if re.match(r"^(CREATE|DROP|SET)\s", s, re.I):
        pages.append(make_page(qid, 1, [("result", "boolean")], [[True]], False, update=(s.split()[0].upper(), None)))
        return qid, pages

    if "information_schema.tables" in s:
        m = re.search(r"table_schema = '([^']*)'", s)
        schema = m.group(1) if m else ""
        limit = re.search(r"table_name IN \((.*?)\)", s)
        wanted = None
        if limit:
            wanted = [x.strip().strip("'") for x in limit.group(1).split(",")]
        rows = [[t] for t in sorted(SCHEMA_TABLES) if schema == "s1" and (wanted is None or t in wanted)]
        pages.append(make_page(qid, 1, [("table_name", "varchar")], rows, False))
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

    if re.search(r'\bFROM\s+(?:"mock"\."s1"\.)?"?sqllog"?', s):
        with lock:
            rows = [[i + 1, q] for i, q in enumerate(sql_log)]
            del sql_log[:]
        selected = split_top_level(re.match(r"^\s*SELECT\s+(.*?)\s+FROM", s, re.S).group(1))
        cols, out = [], []
        for item in selected:
            name = unquote_ident(item)
            cols.append(("n", "integer") if name == "n" else ("sql", "varchar"))
        for r in rows:
            out.append([r[0] if c[0] == "n" else r[1] for c in cols])
        pages.append(make_page(qid, 1, cols, out, False))
        return qid, pages

    try:
        if re.search(r'\bFROM\s+(?:"mock"\."s1"\.)?"?boom"?\b', s):
            raise KeyError("boom:boom")
        cols, rows, table = evaluate_select(s)
    except KeyError as e:
        kind, _, name = e.args[0].partition(":")
        if kind == "boom":
            msg, ename, code = "table boom always fails", "TABLE_NOT_FOUND", 44
        elif kind == "table":
            msg, ename, code = "Table 'mock.s1.%s' does not exist" % name, "TABLE_NOT_FOUND", 44
        else:
            msg, ename, code = "Column '%s' cannot be resolved" % name, "COLUMN_NOT_FOUND", 47
        pages.append(make_page(qid, 1, None, None, False, error=error_obj(msg, ename, "USER_ERROR", code)))
        return qid, pages
    except ValueError:
        pages.append(make_page(qid, 1, None, None, False,
                               error=error_obj("mock cannot parse: " + s, "SYNTAX_ERROR", "USER_ERROR", 1)))
        return qid, pages

    tok = 1
    if table == "t1_slow":
        for _ in range(2):
            p = make_page(qid, tok, None, None, True)
            p["stats"]["state"] = "QUEUED"
            pages.append(p)
            tok += 1
    if table == "empty":
        # two pages, the first one without rows (an aggregate still has one)
        pages.append(make_page(qid, tok, cols, [], True))
        pages.append(make_page(qid, tok + 1, cols, rows, False))
        return qid, pages

    paged = table in ("t1", "t1_slow", "t1_flaky", "boom_late") and len(rows) > 1
    if not paged:
        data = spool(qid, rows) if (use_spooling and rows) else rows
        pages.append(make_page(qid, tok, cols, data, False))
        return qid, pages

    first = spool(qid, rows[:2]) if use_spooling else rows[:2]
    pages.append(make_page(qid, tok, cols, first, True))
    tok += 1
    if table == "t1_flaky":
        pages.append({"__status__": 503})     # first nextUri fetch answers 503 once
    if table == "boom_late":
        pages.append(make_page(qid, tok, cols, None, False,
                               error=error_obj("worker died", "REMOTE_TASK_ERROR", "INTERNAL_ERROR", 65540)))
    else:
        pages.append(make_page(qid, tok, cols, rows[2:], True))
        tok += 1
        pages.append(make_page(qid, tok, cols, None, False))
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
        qid, pages = plan_query(sql, self.headers.get("X-Trino-Query-Data-Encoding") is not None)
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
        if m:
            self.serve_next(m.group(1))
            return
        m = re.match(r"^/v1/spooled/download/([^/?]+)", self.path)
        if m:
            with lock:
                body = spooled.get(m.group(1))
            if body is None or self.headers.get("X-Mock-Segment") != m.group(1):
                self.send_json({"message": "segment not found or headers missing"}, 404)
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        m = re.match(r"^/v1/spooled/ack/([^/?]+)", self.path)
        if m:
            with lock:
                spooled.pop(m.group(1), None)
                sql_log.append("<ack %s>" % m.group(1).split("-")[0])
            self.send_response(204)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self.send_json({"message": "not found"}, 404)

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
    print("mock trino listening on %s:%d%s" % (HOST, PORT, " (spooling)" if SPOOLING else ""), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
