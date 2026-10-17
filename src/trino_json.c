/*-------------------------------------------------------------------------
 *
 * trino_json.c
 *	  Tiny JSON document model built on PostgreSQL's own JSON parser.
 *
 * We parse each Trino response into a tree of TjValue nodes allocated in
 * CurrentMemoryContext.  The tree is small enough per page that building a
 * DOM is simpler and safer than a streaming design.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/jsonapi.h"
#include "mb/pg_wchar.h"
#include "utils/json.h"

#include "trino_fdw.h"

/*
 * Semantic-action callbacks changed signature in PG 16 (they now return
 * JsonParseErrorType).  Paper over the difference.
 */
#if PG_VERSION_NUM >= 160000
#define TJ_CB_RET JsonParseErrorType
#define TJ_CB_OK return JSON_SUCCESS
#else
#define TJ_CB_RET void
#define TJ_CB_OK return
#endif

typedef struct TjParseState
{
	TjValue    *root;
	List	   *stack;			/* stack of containers being built */
	char	   *pending_field;	/* field name awaiting its value */
} TjParseState;

static TjValue *
tj_new(TjType type)
{
	TjValue    *v = palloc0(sizeof(TjValue));

	v->type = type;
	return v;
}

/* attach a freshly completed value to its parent, or make it the root */
static void
tj_attach(TjParseState *st, TjValue *v)
{
	TjValue    *parent;

	if (st->stack == NIL)
	{
		st->root = v;
		return;
	}
	parent = (TjValue *) llast(st->stack);
	if (parent->type == TJ_OBJECT)
	{
		TjField    *f = palloc(sizeof(TjField));

		f->name = st->pending_field;
		f->value = v;
		st->pending_field = NULL;
		parent->elems = lappend(parent->elems, f);
	}
	else
		parent->elems = lappend(parent->elems, v);
}

static TJ_CB_RET
tj_object_start(void *state)
{
	TjParseState *st = (TjParseState *) state;
	TjValue    *v = tj_new(TJ_OBJECT);

	tj_attach(st, v);
	st->stack = lappend(st->stack, v);
	TJ_CB_OK;
}

static TJ_CB_RET
tj_array_start(void *state)
{
	TjParseState *st = (TjParseState *) state;
	TjValue    *v = tj_new(TJ_ARRAY);

	tj_attach(st, v);
	st->stack = lappend(st->stack, v);
	TJ_CB_OK;
}

static TJ_CB_RET
tj_container_end(void *state)
{
	TjParseState *st = (TjParseState *) state;

	st->stack = list_truncate(st->stack, list_length(st->stack) - 1);
	TJ_CB_OK;
}

static TJ_CB_RET
tj_object_field_start(void *state, char *fname, bool isnull)
{
	TjParseState *st = (TjParseState *) state;

	st->pending_field = fname;
	TJ_CB_OK;
}

static TJ_CB_RET
tj_scalar(void *state, char *token, JsonTokenType tokentype)
{
	TjParseState *st = (TjParseState *) state;
	TjValue    *v;

	switch (tokentype)
	{
		case JSON_TOKEN_STRING:
			v = tj_new(TJ_STRING);
			v->str = token;
			break;
		case JSON_TOKEN_NUMBER:
			v = tj_new(TJ_NUMBER);
			v->str = token;
			break;
		case JSON_TOKEN_TRUE:
			v = tj_new(TJ_TRUE);
			break;
		case JSON_TOKEN_FALSE:
			v = tj_new(TJ_FALSE);
			break;
		default:
			v = tj_new(TJ_NULL);
			break;
	}
	tj_attach(st, v);
	TJ_CB_OK;
}

/*
 * Parse a JSON document.  Raises an error on malformed input.
 */
TjValue *
tj_parse(const char *json, int len)
{
	TjParseState st;
	JsonSemAction sem;
	JsonLexContext *lex;
	JsonParseErrorType err;

	memset(&st, 0, sizeof(st));
	memset(&sem, 0, sizeof(sem));
	sem.semstate = &st;
	sem.object_start = tj_object_start;
	sem.object_end = tj_container_end;
	sem.array_start = tj_array_start;
	sem.array_end = tj_container_end;
	sem.object_field_start = tj_object_field_start;
	sem.scalar = tj_scalar;

#if PG_VERSION_NUM >= 170000
	lex = makeJsonLexContextCstringLen(NULL, json, len, PG_UTF8, true);
#else
	lex = makeJsonLexContextCstringLen(unconstify(char *, json), len, PG_UTF8, true);
#endif

	err = pg_parse_json(lex, &sem);
	if (err != JSON_SUCCESS)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_REPLY_HANDLE),
				 errmsg("trino_fdw: could not parse JSON response from Trino"),
				 errdetail("%s", json_errdetail(err, lex))));

#if PG_VERSION_NUM >= 170000
	freeJsonLexContext(lex);
#endif

	if (st.root == NULL)
		st.root = tj_new(TJ_NULL);
	return st.root;
}

/* Look up a field in an object; NULL if missing or not an object. */
TjValue *
tj_get(TjValue *obj, const char *key)
{
	ListCell   *lc;

	if (obj == NULL || obj->type != TJ_OBJECT)
		return NULL;
	foreach(lc, obj->elems)
	{
		TjField    *f = (TjField *) lfirst(lc);

		if (strcmp(f->name, key) == 0)
			return f->value;
	}
	return NULL;
}

/* Fetch a string field; NULL if missing, null, or not a string. */
const char *
tj_get_string(TjValue *obj, const char *key)
{
	TjValue    *v = tj_get(obj, key);

	if (v == NULL || v->type != TJ_STRING)
		return NULL;
	return v->str;
}

int
tj_count(TjValue *v)
{
	if (v == NULL || (v->type != TJ_ARRAY && v->type != TJ_OBJECT))
		return 0;
	return list_length(v->elems);
}

TjValue *
tj_nth(TjValue *v, int n)
{
	if (v == NULL || v->type != TJ_ARRAY)
		return NULL;
	if (n < 0 || n >= list_length(v->elems))
		return NULL;
	return (TjValue *) list_nth(v->elems, n);
}

/* Re-serialize a value as compact JSON text. */
void
tj_serialize(StringInfo buf, TjValue *v)
{
	ListCell   *lc;
	bool		first = true;

	switch (v->type)
	{
		case TJ_NULL:
			appendStringInfoString(buf, "null");
			break;
		case TJ_TRUE:
			appendStringInfoString(buf, "true");
			break;
		case TJ_FALSE:
			appendStringInfoString(buf, "false");
			break;
		case TJ_NUMBER:
			appendStringInfoString(buf, v->str);
			break;
		case TJ_STRING:
			escape_json(buf, v->str);
			break;
		case TJ_ARRAY:
			appendStringInfoChar(buf, '[');
			foreach(lc, v->elems)
			{
				if (!first)
					appendStringInfoChar(buf, ',');
				first = false;
				tj_serialize(buf, (TjValue *) lfirst(lc));
			}
			appendStringInfoChar(buf, ']');
			break;
		case TJ_OBJECT:
			appendStringInfoChar(buf, '{');
			foreach(lc, v->elems)
			{
				TjField    *f = (TjField *) lfirst(lc);

				if (!first)
					appendStringInfoChar(buf, ',');
				first = false;
				escape_json(buf, f->name);
				appendStringInfoChar(buf, ':');
				tj_serialize(buf, f->value);
			}
			appendStringInfoChar(buf, '}');
			break;
	}
}

char *
tj_to_cstring(TjValue *v)
{
	StringInfoData buf;

	initStringInfo(&buf);
	tj_serialize(&buf, v);
	return buf.data;
}
