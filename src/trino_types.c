/*-------------------------------------------------------------------------
 *
 * trino_types.c
 *	  Map Trino types to PostgreSQL types and convert JSON cell values.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "catalog/pg_type.h"
#include "common/base64.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

#include "trino_fdw.h"

/*
 * Split a Trino type signature such as
 *   "timestamp(3) with time zone"  or  "map(varchar, array(integer))"
 * into base name, top-level parameters and trailing suffix.
 */
static void
parse_type_signature(const char *sig, char **name, List **params, char **suffix)
{
	const char *p = sig;
	const char *start;
	StringInfoData buf;

	*params = NIL;
	*suffix = NULL;

	while (*p && isspace((unsigned char) *p))
		p++;
	start = p;
	while (*p && *p != '(' && !isspace((unsigned char) *p))
		p++;
	*name = pnstrdup(start, p - start);

	if (*p == '(')
	{
		int			depth = 0;

		p++;
		initStringInfo(&buf);
		for (; *p; p++)
		{
			if (*p == '(')
				depth++;
			else if (*p == ')')
			{
				if (depth == 0)
				{
					p++;
					break;
				}
				depth--;
			}
			if (*p == ',' && depth == 0)
			{
				*params = lappend(*params, pstrdup(buf.data));
				resetStringInfo(&buf);
				continue;
			}
			if (buf.len == 0 && isspace((unsigned char) *p))
				continue;
			appendStringInfoChar(&buf, *p);
		}
		if (buf.len > 0)
			*params = lappend(*params, pstrdup(buf.data));
	}

	while (*p && isspace((unsigned char) *p))
		p++;
	if (*p)
	{
		char	   *s = pstrdup(p);
		int			n = strlen(s);

		while (n > 0 && isspace((unsigned char) s[n - 1]))
			s[--n] = '\0';
		*suffix = s;
	}
}

static int
param_int(List *params, int n, int dflt)
{
	const char *s;

	if (n >= list_length(params))
		return dflt;
	s = (const char *) list_nth(params, n);
	if (!isdigit((unsigned char) s[0]))
		return dflt;
	return atoi(s);
}

/* Types whose values arrive as JSON containers we must keep as jsonb */
static bool
is_structural(const char *name)
{
	return pg_strcasecmp(name, "row") == 0 ||
		pg_strcasecmp(name, "map") == 0;
}

/*
 * Return a PostgreSQL type name (as SQL text) for a Trino type signature.
 */
char *
trino_type_to_pg(const char *trino_type)
{
	char	   *name;
	List	   *params;
	char	   *suffix;
	bool		with_tz;

	parse_type_signature(trino_type, &name, &params, &suffix);
	with_tz = suffix && pg_strcasecmp(suffix, "with time zone") == 0;

	if (pg_strcasecmp(name, "boolean") == 0)
		return "boolean";
	if (pg_strcasecmp(name, "tinyint") == 0 || pg_strcasecmp(name, "smallint") == 0)
		return "smallint";
	if (pg_strcasecmp(name, "integer") == 0)
		return "integer";
	if (pg_strcasecmp(name, "bigint") == 0)
		return "bigint";
	if (pg_strcasecmp(name, "real") == 0)
		return "real";
	if (pg_strcasecmp(name, "double") == 0)
		return "double precision";
	if (pg_strcasecmp(name, "decimal") == 0)
	{
		if (list_length(params) >= 1)
			return psprintf("numeric(%d,%d)", param_int(params, 0, 38),
							param_int(params, 1, 0));
		return "numeric";
	}
	if (pg_strcasecmp(name, "varchar") == 0)
	{
		int			n = param_int(params, 0, 0);

		if (n > 0 && n <= 10485760)
			return psprintf("varchar(%d)", n);
		return "text";
	}
	if (pg_strcasecmp(name, "char") == 0)
	{
		int			n = param_int(params, 0, 1);

		if (n > 0 && n <= 10485760)
			return psprintf("char(%d)", n);
		return "text";
	}
	if (pg_strcasecmp(name, "varbinary") == 0)
		return "bytea";
	if (pg_strcasecmp(name, "json") == 0)
		return "jsonb";
	if (pg_strcasecmp(name, "date") == 0)
		return "date";
	if (pg_strcasecmp(name, "time") == 0)
	{
		int			p = Min(param_int(params, 0, 3), 6);

		return psprintf("time(%d)%s", p, with_tz ? " with time zone" : "");
	}
	if (pg_strcasecmp(name, "timestamp") == 0)
	{
		int			p = Min(param_int(params, 0, 3), 6);

		return psprintf("timestamp(%d)%s", p, with_tz ? " with time zone" : "");
	}
	if (pg_strcasecmp(name, "interval") == 0)
		return "interval";
	if (pg_strcasecmp(name, "uuid") == 0)
		return "uuid";
	if (pg_strcasecmp(name, "ipaddress") == 0)
		return "inet";
	if (pg_strcasecmp(name, "array") == 0)
	{
		const char *inner_sig;
		char	   *inner_name;
		List	   *inner_params;
		char	   *inner_suffix;
		char	   *inner;

		if (list_length(params) != 1)
			return "jsonb";
		inner_sig = (const char *) linitial(params);
		parse_type_signature(inner_sig, &inner_name, &inner_params, &inner_suffix);
		/*
		 * Nested Trino arrays can be ragged, which PostgreSQL arrays cannot
		 * represent, so anything but a scalar element type becomes jsonb.
		 */
		if (is_structural(inner_name) || pg_strcasecmp(inner_name, "json") == 0 ||
			pg_strcasecmp(inner_name, "array") == 0)
			return "jsonb";
		inner = trino_type_to_pg(inner_sig);
		if (strcmp(inner, "jsonb") == 0)
			return "jsonb";
		return psprintf("%s[]", inner);
	}
	if (is_structural(name))
		return "jsonb";

	/* Geometry, HyperLogLog, color, tdigest, ... : keep the textual form */
	return "text";
}

/* ----------------------------------------------------------------
 * Value conversion
 * ---------------------------------------------------------------- */

static void
append_array_literal(StringInfo buf, TjValue *v, Oid elemtype);

/* Quote one element of an array literal */
static void
append_quoted_array_elem(StringInfo buf, const char *s)
{
	appendStringInfoChar(buf, '"');
	for (; *s; s++)
	{
		if (*s == '"' || *s == '\\')
			appendStringInfoChar(buf, '\\');
		appendStringInfoChar(buf, *s);
	}
	appendStringInfoChar(buf, '"');
}

static void
append_array_literal(StringInfo buf, TjValue *v, Oid elemtype)
{
	ListCell   *lc;
	bool		first = true;

	appendStringInfoChar(buf, '{');
	foreach(lc, v->elems)
	{
		TjValue    *e = (TjValue *) lfirst(lc);
		bool		isnull;
		char	   *text;

		if (!first)
			appendStringInfoChar(buf, ',');
		first = false;

		if (e->type == TJ_NULL)
		{
			appendStringInfoString(buf, "NULL");
			continue;
		}
		if (e->type == TJ_ARRAY && elemtype != JSONBOID && elemtype != JSONOID)
		{
			append_array_literal(buf, e, elemtype);
			continue;
		}
		text = trino_value_to_text(e, elemtype, &isnull);
		append_quoted_array_elem(buf, text);
	}
	appendStringInfoChar(buf, '}');
}

/* Decode base64 varbinary into PostgreSQL's hex bytea input format */
static char *
base64_to_bytea_text(const char *b64)
{
	int			srclen = strlen(b64);
	int			dstlen = pg_b64_dec_len(srclen);
	char	   *raw = palloc(dstlen + 1);
	int			n;
	char	   *out;
	static const char hexdigits[] = "0123456789abcdef";
	int			i;

	n = pg_b64_decode(b64, srclen, raw, dstlen);
	if (n < 0)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
				 errmsg("trino_fdw: invalid base64 varbinary value from Trino")));

	out = palloc(2 + n * 2 + 1);
	out[0] = '\\';
	out[1] = 'x';
	for (i = 0; i < n; i++)
	{
		out[2 + i * 2] = hexdigits[(raw[i] >> 4) & 0xf];
		out[3 + i * 2] = hexdigits[raw[i] & 0xf];
	}
	out[2 + n * 2] = '\0';
	pfree(raw);
	return out;
}

/*
 * Trino renders negative day-to-second intervals as "-D HH:MM:SS.fff",
 * which PostgreSQL reads as (-D days) + (HH:MM:SS).  Rewrite it so the sign
 * applies to the whole value.
 */
static char *
fix_negative_interval(const char *s)
{
	const char *space;

	if (s[0] != '-')
		return (char *) s;
	space = strchr(s, ' ');
	if (space == NULL || !isdigit((unsigned char) space[1]))
		return (char *) s;
	return psprintf("%.*s -%s", (int) (space - s), s, space + 1);
}

/*
 * Convert a JSON cell into the text form accepted by pgtype's input
 * function.  Sets *isnull for JSON null.
 */
char *
trino_value_to_text(TjValue *v, Oid pgtype, bool *isnull)
{
	Oid			elemtype;

	*isnull = false;
	if (v == NULL || v->type == TJ_NULL)
	{
		*isnull = true;
		return NULL;
	}

	if (pgtype == JSONOID || pgtype == JSONBOID)
	{
		/* a Trino json value arrives as a string holding JSON text */
		if (v->type == TJ_STRING)
			return v->str;
		return tj_to_cstring(v);
	}

	if (pgtype == BYTEAOID && v->type == TJ_STRING)
		return base64_to_bytea_text(v->str);

	if (v->type == TJ_ARRAY)
	{
		elemtype = get_element_type(pgtype);
		if (OidIsValid(elemtype))
		{
			StringInfoData buf;

			initStringInfo(&buf);
			append_array_literal(&buf, v, elemtype);
			return buf.data;
		}
		return tj_to_cstring(v);
	}

	switch (v->type)
	{
		case TJ_TRUE:
			return "t";
		case TJ_FALSE:
			return "f";
		case TJ_NUMBER:
			return v->str;
		case TJ_STRING:
			if (pgtype == INTERVALOID)
				return fix_negative_interval(v->str);
			return v->str;
		default:
			return tj_to_cstring(v);
	}
}

Datum
trino_value_to_datum(TjValue *v, Oid pgtype, int32 typmod, FmgrInfo *flinfo,
					 Oid typioparam, bool *isnull)
{
	char	   *text = trino_value_to_text(v, pgtype, isnull);

	if (*isnull)
		return (Datum) 0;
	return InputFunctionCall(flinfo, text, typioparam, typmod);
}
