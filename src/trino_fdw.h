/*-------------------------------------------------------------------------
 *
 * trino_fdw.h
 *	  Foreign-data wrapper for Trino (https://trino.io)
 *
 * The wrapper speaks Trino's REST protocol (POST /v1/statement, then
 * follow nextUri) over libcurl and parses responses with the JSON parser
 * that ships with PostgreSQL, so the only external dependency is libcurl.
 *
 *-------------------------------------------------------------------------
 */
#ifndef TRINO_FDW_H
#define TRINO_FDW_H

#define TRINO_FDW_VERSION_STR "1.0"

#include "postgres.h"

#include "fmgr.h"
#include "foreign/foreign.h"
#include "lib/stringinfo.h"
#include "nodes/pathnodes.h"
#include "nodes/pg_list.h"
#include "utils/relcache.h"

#if PG_VERSION_NUM < 140000
#error "trino_fdw requires PostgreSQL 14 or later"
#endif

/* ------------------------------------------------------------------
 * Minimal JSON document model (trino_json.c)
 * ------------------------------------------------------------------ */
typedef enum TjType
{
	TJ_NULL,
	TJ_TRUE,
	TJ_FALSE,
	TJ_NUMBER,
	TJ_STRING,
	TJ_ARRAY,
	TJ_OBJECT
} TjType;

typedef struct TjValue
{
	TjType		type;
	char	   *str;			/* number text or string contents */
	List	   *elems;			/* TJ_ARRAY: List of TjValue*;
								 * TJ_OBJECT: List of TjField* */
} TjValue;

typedef struct TjField
{
	char	   *name;
	TjValue    *value;
} TjField;

extern TjValue *tj_parse(const char *json, int len);
extern TjValue *tj_get(TjValue *obj, const char *key);
extern const char *tj_get_string(TjValue *obj, const char *key);
extern int	tj_count(TjValue *v);
extern TjValue *tj_nth(TjValue *v, int n);
extern void tj_serialize(StringInfo buf, TjValue *v);
extern char *tj_to_cstring(TjValue *v);

/* ------------------------------------------------------------------
 * Options
 * ------------------------------------------------------------------ */
typedef struct TrinoOptions
{
	/* connection */
	char	   *host;
	int			port;
	bool		ssl;
	bool		ssl_verify;
	char	   *ca_file;
	char	   *user;
	char	   *password;
	char	   *source;
	char	   *catalog;
	char	   *schema;
	char	   *session_properties;
	char	   *client_tags;
	bool		spooling;		/* ask for the spooled protocol */
	int			connect_timeout;	/* seconds */
	int			request_timeout;	/* seconds, 0 = none */

	/* planner knobs */
	double		startup_cost;
	double		tuple_cost;
	double		default_rows;

	/* modify */
	int			batch_size;

	/* table-level */
	char	   *schema_name;
	char	   *table_name;
} TrinoOptions;

extern TrinoOptions *trino_get_options(Oid foreigntableid, Oid serverid, Oid userid);
extern TrinoOptions *trino_get_server_options(Oid serverid, Oid userid);
extern char *trino_get_column_name(Oid relid, AttrNumber attnum);

/* ------------------------------------------------------------------
 * Trino REST client (trino_client.c)
 * ------------------------------------------------------------------ */
typedef struct TrinoQuery TrinoQuery;

extern void trino_client_init(void);
extern TrinoQuery *trino_query_start(TrinoOptions *opts, const char *sql,
									 MemoryContext mcxt);
extern TjValue *trino_query_fetch_row(TrinoQuery *q);
extern int	trino_query_ncolumns(TrinoQuery *q);
extern const char *trino_query_column_name(TrinoQuery *q, int i);
extern const char *trino_query_column_type(TrinoQuery *q, int i);
extern void trino_query_close(TrinoQuery *q);
extern int64 trino_execute_sql(TrinoOptions *opts, const char *sql);

/* ------------------------------------------------------------------
 * Deparsing (trino_deparse.c)
 * ------------------------------------------------------------------ */
typedef struct TrinoRelationInfo
{
	/* base relation */
	bool		pushdown_safe;
	List	   *remote_conds;	/* RestrictInfos we ship to Trino */
	List	   *local_conds;	/* RestrictInfos checked locally */
	Bitmapset  *attrs_used;		/* attrs needed, offset by
								 * FirstLowInvalidHeapAttributeNumber */
	TrinoOptions *opts;
	Oid			relid;
	Oid			userid;
	double		rows;
	int			width;
	Cost		startup_cost;
	Cost		total_cost;
	Cost		rel_startup_cost;
	Cost		rel_total_cost;

	/*
	 * Join relations: the two inputs, the join type and the clauses that go
	 * into the ON clause.  remote_conds/local_conds hold WHERE-level clauses.
	 */
	RelOptInfo *outerrel;		/* join: outer input; grouped/ordered rel:
								 * the input relation being scanned */
	RelOptInfo *innerrel;
	JoinType	jointype;
	List	   *joinclauses;	/* RestrictInfos */

	/* grouped relations (aggregate pushdown) */
	bool		is_grouped;
	List	   *grouped_tlist;	/* what to SELECT: group exprs and Aggrefs */
	List	   *having_remote;	/* Expr list sent as HAVING */
	List	   *having_local;	/* Expr list evaluated locally */

	/* ORDER BY / LIMIT pushdown, carried in a path's fdw_private */
	List	   *pathkeys;		/* pushed-down ORDER BY, or NIL */
	bool		has_limit;
	Node	   *limit_count;
	Node	   *limit_offset;

	char	   *relation_name;	/* EXPLAIN "Relations:" for joins/aggregates */
} TrinoRelationInfo;

extern bool trino_is_foreign_expr(PlannerInfo *root, RelOptInfo *rel,
								  Expr *expr);
extern bool trino_is_sortable_pathkey(PlannerInfo *root, RelOptInfo *rel,
									  PathKey *pathkey);
extern bool trino_is_foreign_param(PlannerInfo *root, RelOptInfo *rel,
								   Expr *expr);
extern void trino_deparse_direct_modify(StringInfo buf, PlannerInfo *root,
										RelOptInfo *rel, CmdType operation,
										List *target_attrs, List *target_exprs,
										List *remote_exprs, List **params_list,
										List **param_offsets);
extern char *trino_relation_name(PlannerInfo *root, RelOptInfo *rel);
extern void trino_deparse_select(StringInfo buf, PlannerInfo *root,
								 RelOptInfo *foreignrel, List *tlist,
								 List *remote_conds, List *pathkeys,
								 bool has_limit, Node *limit_count,
								 Node *limit_offset,
								 List **retrieved_attrs, List **params_list,
								 List **param_offsets);
extern void trino_deparse_relation(StringInfo buf, Relation rel,
								   TrinoOptions *opts);
extern void trino_deparse_insert_prefix(StringInfo buf, Relation rel,
										TrinoOptions *opts, List *target_attrs);
extern void trino_deparse_datum(StringInfo buf, Datum value, Oid type,
								bool isnull);
extern bool trino_type_is_deparsable(Oid type);
extern void trino_quote_identifier(StringInfo buf, const char *ident);
extern void trino_quote_literal(StringInfo buf, const char *str);
extern void trino_deparse_analyze_count(StringInfo buf, Relation rel,
										TrinoOptions *opts);
extern void trino_deparse_analyze_sample(StringInfo buf, Relation rel,
										 TrinoOptions *opts, double percent,
										 List **retrieved_attrs);

/* ------------------------------------------------------------------
 * Type mapping and conversion (trino_types.c)
 * ------------------------------------------------------------------ */
extern char *trino_type_to_pg(const char *trino_type);
extern char *trino_value_to_text(TjValue *v, Oid pgtype, bool *isnull);
extern Datum trino_value_to_datum(TjValue *v, Oid pgtype, int32 typmod,
								  FmgrInfo *flinfo, Oid typioparam,
								  bool *isnull);

#endif							/* TRINO_FDW_H */
