/*-------------------------------------------------------------------------
 *
 * trino_client.c
 *	  Trino REST protocol client on top of libcurl.
 *
 * Protocol summary (https://trino.io/docs/current/develop/client-protocol.html):
 *   POST /v1/statement with the SQL text as the body.  The JSON reply carries
 *   an "id", possibly "columns" and "data", and a "nextUri".  Keep GETting
 *   nextUri until it is absent; "error" signals failure; HTTP 503 means
 *   "retry later".  DELETE nextUri cancels a running query.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <curl/curl.h>

#include "common/base64.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

#include "trino_fdw.h"

#define TRINO_USER_AGENT "trino_fdw/" TRINO_FDW_VERSION_STR
#define TRINO_MAX_503_RETRIES 120	/* ~1 minute of back-off */

struct TrinoQuery
{
	TrinoOptions *opts;
	CURL	   *curl;
	struct curl_slist *headers;
	MemoryContext mcxt;			/* lifetime of the query object */
	MemoryContext page_cxt;		/* reset for every response page */
	MemoryContextCallback cb;

	StringInfoData resp;		/* response body of the last request */
	char	   *sql;
	char	   *query_id;
	char	   *next_uri;		/* NULL once the query has finished */

	int			ncols;
	char	  **col_names;
	char	  **col_types;

	TjValue    *data;			/* current page's rows */
	int			nrows;
	int			cur;

	int64		update_count;
	bool		closed;
};

static bool curl_initialized = false;

void
trino_client_init(void)
{
	if (!curl_initialized)
	{
		if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FDW_OUT_OF_MEMORY),
					 errmsg("trino_fdw: could not initialize libcurl")));
		curl_initialized = true;
	}
}

/* libcurl write callback: append to the response buffer */
static size_t
trino_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	StringInfo	buf = (StringInfo) userdata;
	size_t		n = size * nmemb;

	/* StringInfo is limited to ~1GB; Trino pages are far smaller */
	appendBinaryStringInfo(buf, ptr, (int) n);
	return n;
}

/*
 * libcurl progress callback: abort the transfer when a cancel request or
 * other interrupt is pending, so a long-running Trino request does not make
 * the backend unresponsive to pg_cancel_backend().
 */
static int
trino_progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
				  curl_off_t ultotal, curl_off_t ulnow)
{
	/*
	 * Only abort for interrupts that will end the statement.  Other pending
	 * interrupts (sinval catch-up, NOTIFY, ...) are serviced between pages.
	 */
	if (QueryCancelPending || ProcDiePending)
		return 1;
	return 0;
}

static void
trino_release_curl(TrinoQuery *q)
{
	if (q->curl)
	{
		curl_easy_cleanup(q->curl);
		q->curl = NULL;
	}
	if (q->headers)
	{
		curl_slist_free_all(q->headers);
		q->headers = NULL;
	}
}

/*
 * Best-effort cancellation of a query still running on the server.  Never
 * raises an error: this is called from cleanup paths.
 */
static void
trino_cancel_silently(TrinoQuery *q)
{
	if (q->curl == NULL || q->next_uri == NULL)
		return;

	curl_easy_setopt(q->curl, CURLOPT_URL, q->next_uri);
	curl_easy_setopt(q->curl, CURLOPT_HTTPGET, 1L);
	curl_easy_setopt(q->curl, CURLOPT_CUSTOMREQUEST, "DELETE");
	curl_easy_setopt(q->curl, CURLOPT_TIMEOUT, 5L);
	curl_easy_setopt(q->curl, CURLOPT_NOPROGRESS, 1L);
	(void) curl_easy_perform(q->curl);
	q->next_uri = NULL;
}

/* Memory context reset/delete callback: release resources on error paths */
static void
trino_query_mcxt_callback(void *arg)
{
	TrinoQuery *q = (TrinoQuery *) arg;

	if (!q->closed)
	{
		trino_cancel_silently(q);
		trino_release_curl(q);
		q->closed = true;
	}
}

static char *
trino_base_url(TrinoOptions *opts)
{
	StringInfoData buf;

	initStringInfo(&buf);
	appendStringInfo(&buf, "%s://%s:%d", opts->ssl ? "https" : "http",
					 opts->host, opts->port);
	return buf.data;
}

static struct curl_slist *
trino_build_headers(TrinoOptions *opts)
{
	struct curl_slist *h = NULL;
	StringInfoData buf;

	initStringInfo(&buf);

#define ADD_HEADER(fmt, ...) \
	do { \
		resetStringInfo(&buf); \
		appendStringInfo(&buf, fmt, __VA_ARGS__); \
		h = curl_slist_append(h, buf.data); \
	} while (0)

	ADD_HEADER("X-Trino-User: %s", opts->user);
	ADD_HEADER("X-Trino-Source: %s", opts->source);
	ADD_HEADER("User-Agent: %s", TRINO_USER_AGENT);
	if (opts->catalog)
		ADD_HEADER("X-Trino-Catalog: %s", opts->catalog);
	if (opts->schema)
		ADD_HEADER("X-Trino-Schema: %s", opts->schema);
	if (opts->session_properties)
		ADD_HEADER("X-Trino-Session: %s", opts->session_properties);
	if (opts->client_tags)
		ADD_HEADER("X-Trino-Client-Tags: %s", opts->client_tags);
	/* without this Trino downgrades every timestamp to millisecond precision */
	h = curl_slist_append(h, "X-Trino-Client-Capabilities: PARAMETRIC_DATETIME,SESSION_AUTHORIZATION");
	h = curl_slist_append(h, "Content-Type: text/plain; charset=utf-8");
	h = curl_slist_append(h, "Accept: application/json");
	h = curl_slist_append(h, "Expect:");	/* no 100-continue dance */
#undef ADD_HEADER

	pfree(buf.data);
	return h;
}

static CURL *
trino_make_curl(TrinoQuery *q)
{
	CURL	   *curl = curl_easy_init();
	TrinoOptions *opts = q->opts;

	if (curl == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_OUT_OF_MEMORY),
				 errmsg("trino_fdw: could not create libcurl handle")));

	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, trino_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &q->resp);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, trino_progress_cb);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, q->headers);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long) opts->connect_timeout);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long) opts->request_timeout);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

	if (opts->password)
	{
		curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long) CURLAUTH_BASIC);
		curl_easy_setopt(curl, CURLOPT_USERNAME, opts->user);
		curl_easy_setopt(curl, CURLOPT_PASSWORD, opts->password);
	}
	if (opts->ssl)
	{
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, opts->ssl_verify ? 1L : 0L);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, opts->ssl_verify ? 2L : 0L);
		if (opts->ca_file)
			curl_easy_setopt(curl, CURLOPT_CAINFO, opts->ca_file);
	}
	return curl;
}

/*
 * Perform one HTTP request, retrying on 503.  Returns the HTTP status and
 * leaves the body in q->resp.
 */
static long
trino_http(TrinoQuery *q, const char *method, const char *url, const char *body)
{
	int			retries = 0;
	long		sleep_us = 50000;	/* 50ms, doubling up to 500ms */

	for (;;)
	{
		CURLcode	rc;
		long		status = 0;

		resetStringInfo(&q->resp);
		curl_easy_setopt(q->curl, CURLOPT_URL, url);
		curl_easy_setopt(q->curl, CURLOPT_CUSTOMREQUEST, NULL);
		if (strcmp(method, "POST") == 0)
		{
			curl_easy_setopt(q->curl, CURLOPT_POST, 1L);
			curl_easy_setopt(q->curl, CURLOPT_POSTFIELDS, body);
			curl_easy_setopt(q->curl, CURLOPT_POSTFIELDSIZE, (long) strlen(body));
		}
		else
		{
			curl_easy_setopt(q->curl, CURLOPT_HTTPGET, 1L);
			if (strcmp(method, "DELETE") == 0)
				curl_easy_setopt(q->curl, CURLOPT_CUSTOMREQUEST, "DELETE");
		}

		rc = curl_easy_perform(q->curl);

		if (rc == CURLE_ABORTED_BY_CALLBACK)
		{
			/* a cancel is pending; this normally does not return */
			CHECK_FOR_INTERRUPTS();

			/*
			 * Interrupts were held off.  Re-fetching a page is idempotent in
			 * Trino's protocol, but re-POSTing would run the query twice.
			 */
			if (strcmp(method, "POST") != 0 && retries++ < TRINO_MAX_503_RETRIES)
				continue;
			ereport(ERROR,
					(errcode(ERRCODE_QUERY_CANCELED),
					 errmsg("trino_fdw: request to Trino was interrupted")));
		}
		if (rc != CURLE_OK)
			ereport(ERROR,
					(errcode(ERRCODE_FDW_UNABLE_TO_ESTABLISH_CONNECTION),
					 errmsg("trino_fdw: request to Trino at %s failed: %s",
							url, curl_easy_strerror(rc))));

		curl_easy_getinfo(q->curl, CURLINFO_RESPONSE_CODE, &status);

		if (status == 503 && retries++ < TRINO_MAX_503_RETRIES)
		{
			CHECK_FOR_INTERRUPTS();
			pg_usleep(sleep_us);
			if (sleep_us < 500000)
				sleep_us *= 2;
			continue;
		}
		return status;
	}
}

/* Raise an error for a Trino "error" object */
static void
trino_raise_error(TrinoQuery *q, TjValue *err)
{
	const char *message = tj_get_string(err, "message");
	const char *name = tj_get_string(err, "errorName");
	const char *type = tj_get_string(err, "errorType");
	TjValue    *loc = tj_get(err, "errorLocation");
	TjValue    *line = tj_get(loc, "lineNumber");
	TjValue    *column = tj_get(loc, "columnNumber");
	int			sqlstate = ERRCODE_FDW_ERROR;

	if (type && strcmp(type, "USER_ERROR") == 0)
		sqlstate = ERRCODE_FDW_INVALID_DESCRIPTOR_FIELD_IDENTIFIER;
	if (name && strcmp(name, "SYNTAX_ERROR") == 0)
		sqlstate = ERRCODE_SYNTAX_ERROR;
	if (name && strcmp(name, "PERMISSION_DENIED") == 0)
		sqlstate = ERRCODE_INSUFFICIENT_PRIVILEGE;
	if (name && (strcmp(name, "TABLE_NOT_FOUND") == 0 ||
				 strcmp(name, "SCHEMA_NOT_FOUND") == 0 ||
				 strcmp(name, "CATALOG_NOT_FOUND") == 0))
		sqlstate = ERRCODE_UNDEFINED_TABLE;
	if (name && strcmp(name, "COLUMN_NOT_FOUND") == 0)
		sqlstate = ERRCODE_UNDEFINED_COLUMN;
	if (name && strcmp(name, "USER_CANCELED") == 0)
		sqlstate = ERRCODE_QUERY_CANCELED;

	ereport(ERROR,
			(errcode(sqlstate),
			 errmsg("trino_fdw: Trino query failed: %s",
					message ? message : "unknown error"),
			 errdetail("Trino error %s (%s), query id %s%s%s%s%s.",
					   name ? name : "?", type ? type : "?",
					   q->query_id ? q->query_id : "?",
					   line ? ", at line " : "",
					   line ? tj_to_cstring(line) : "",
					   column ? ", column " : "",
					   column ? tj_to_cstring(column) : ""),
			 errcontext("Trino SQL: %s", q->sql)));
}

/*
 * Interpret the response body of the last request: capture id, columns,
 * data and nextUri, or raise the server's error.
 */
static void
trino_process_response(TrinoQuery *q, long status)
{
	TjValue    *doc;
	TjValue    *v;
	MemoryContext oldcxt;

	if (status != 200)
	{
		char	   *snippet;
		int			n = Min(q->resp.len, 500);

		snippet = pnstrdup(q->resp.data, n);
		ereport(ERROR,
				(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
				 errmsg("trino_fdw: Trino returned HTTP status %ld", status),
				 q->resp.len > 0 ? errdetail("%s", snippet) : 0,
				 errcontext("Trino SQL: %s", q->sql)));
	}

	MemoryContextReset(q->page_cxt);
	oldcxt = MemoryContextSwitchTo(q->page_cxt);
	doc = tj_parse(q->resp.data, q->resp.len);
	MemoryContextSwitchTo(oldcxt);

	q->data = NULL;
	q->nrows = 0;
	q->cur = 0;

	if (doc->type != TJ_OBJECT)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_REPLY_HANDLE),
				 errmsg("trino_fdw: unexpected response from Trino: not a JSON object")));

	if (q->query_id == NULL && tj_get_string(doc, "id"))
		q->query_id = MemoryContextStrdup(q->mcxt, tj_get_string(doc, "id"));

	/* nextUri: copy into the long-lived context, since the page goes away */
	if (q->next_uri)
	{
		pfree(q->next_uri);
		q->next_uri = NULL;
	}
	if (tj_get_string(doc, "nextUri"))
		q->next_uri = MemoryContextStrdup(q->mcxt, tj_get_string(doc, "nextUri"));

	v = tj_get(doc, "error");
	if (v && v->type == TJ_OBJECT)
	{
		/* the query is already dead on the server; nothing to cancel */
		q->next_uri = NULL;
		trino_raise_error(q, v);
	}

	/* columns, the first time they appear */
	v = tj_get(doc, "columns");
	if (q->ncols == 0 && v && v->type == TJ_ARRAY)
	{
		int			i;

		q->ncols = tj_count(v);
		q->col_names = MemoryContextAlloc(q->mcxt, sizeof(char *) * Max(q->ncols, 1));
		q->col_types = MemoryContextAlloc(q->mcxt, sizeof(char *) * Max(q->ncols, 1));
		for (i = 0; i < q->ncols; i++)
		{
			TjValue    *col = tj_nth(v, i);
			const char *name = tj_get_string(col, "name");
			const char *type = tj_get_string(col, "type");

			q->col_names[i] = MemoryContextStrdup(q->mcxt, name ? name : "");
			q->col_types[i] = MemoryContextStrdup(q->mcxt, type ? type : "unknown");
		}
	}

	v = tj_get(doc, "data");
	if (v && v->type == TJ_ARRAY)
	{
		q->data = v;
		q->nrows = tj_count(v);
	}

	v = tj_get(doc, "updateCount");
	if (v && v->type == TJ_NUMBER)
		q->update_count = strtoll(v->str, NULL, 10);
}

/*
 * Submit a query.  Returns after the first response has been processed; it
 * may or may not contain data yet.  The TrinoQuery lives in mcxt and is
 * cleaned up automatically if mcxt is reset or deleted.
 */
TrinoQuery *
trino_query_start(TrinoOptions *opts, const char *sql, MemoryContext mcxt)
{
	TrinoQuery *q;
	MemoryContext oldcxt;
	char	   *url;
	long		status;

	trino_client_init();

	oldcxt = MemoryContextSwitchTo(mcxt);
	q = palloc0(sizeof(TrinoQuery));
	q->opts = opts;
	q->mcxt = mcxt;
	q->page_cxt = AllocSetContextCreate(mcxt, "trino_fdw page",
										ALLOCSET_DEFAULT_SIZES);
	q->sql = pstrdup(sql);
	q->update_count = -1;
	initStringInfo(&q->resp);
	q->headers = trino_build_headers(opts);
	q->curl = trino_make_curl(q);
	url = psprintf("%s/v1/statement", trino_base_url(opts));
	MemoryContextSwitchTo(oldcxt);

	/* make sure the handle is released even if we error out below */
	q->cb.func = trino_query_mcxt_callback;
	q->cb.arg = q;
	MemoryContextRegisterResetCallback(mcxt, &q->cb);

	status = trino_http(q, "POST", url, sql);
	trino_process_response(q, status);
	return q;
}

/*
 * Return the next row (a JSON array of cell values) or NULL when the query
 * has produced all its rows.  The returned value lives in a per-page memory
 * context and is only valid until the next call.
 */
TjValue *
trino_query_fetch_row(TrinoQuery *q)
{
	for (;;)
	{
		long		status;

		if (q->data && q->cur < q->nrows)
			return tj_nth(q->data, q->cur++);

		if (q->next_uri == NULL)
			return NULL;

		CHECK_FOR_INTERRUPTS();
		status = trino_http(q, "GET", q->next_uri, NULL);
		trino_process_response(q, status);
	}
}

int
trino_query_ncolumns(TrinoQuery *q)
{
	return q->ncols;
}

const char *
trino_query_column_name(TrinoQuery *q, int i)
{
	return q->col_names[i];
}

const char *
trino_query_column_type(TrinoQuery *q, int i)
{
	return q->col_types[i];
}

/*
 * Finish with a query: cancel it on the server if it is still running and
 * release the HTTP handle.
 */
void
trino_query_close(TrinoQuery *q)
{
	if (q->closed)
		return;
	trino_cancel_silently(q);
	trino_release_curl(q);
	q->closed = true;
	MemoryContextDelete(q->page_cxt);
	q->page_cxt = NULL;
}

/*
 * Run a statement to completion, discarding any rows.  Returns the update
 * count reported by Trino, or -1 if none.
 */
int64
trino_execute_sql(TrinoOptions *opts, const char *sql)
{
	MemoryContext mcxt;
	TrinoQuery *q;
	int64		count;

	mcxt = AllocSetContextCreate(CurrentMemoryContext, "trino_fdw execute",
								 ALLOCSET_DEFAULT_SIZES);
	q = trino_query_start(opts, sql, mcxt);
	while (trino_query_fetch_row(q) != NULL)
		;
	count = q->update_count;
	trino_query_close(q);
	MemoryContextDelete(mcxt);
	return count;
}
