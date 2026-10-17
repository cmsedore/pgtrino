/*-------------------------------------------------------------------------
 *
 * trino_fdw.c
 *	  Foreign-data wrapper for Trino: planner and executor callbacks.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>
#include <math.h>

#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_user_mapping.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#if PG_VERSION_NUM >= 180000
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#endif
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/prep.h"
#include "utils/selfuncs.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/sampling.h"
#include "utils/syscache.h"

#include "trino_fdw.h"

PG_MODULE_MAGIC;

void		_PG_init(void);

PG_FUNCTION_INFO_V1(trino_fdw_handler);
PG_FUNCTION_INFO_V1(trino_fdw_validator);
PG_FUNCTION_INFO_V1(trino_fdw_version);
PG_FUNCTION_INFO_V1(trino_query);
PG_FUNCTION_INFO_V1(trino_execute);

/* Indexes into ForeignScan.fdw_private */
enum TrinoScanPrivateIndex
{
	TrinoScanPrivateSql,		/* SQL template (String) */
	TrinoScanPrivateRetrievedAttrs, /* attnums in the scan slot (int list) */
	TrinoScanPrivateParamOffsets,	/* byte offsets of params (int list) */
	TrinoScanPrivateRelid,		/* a foreign table OID (Integer), for options */
	TrinoScanPrivateRelations	/* EXPLAIN description of a join/aggregate
								 * (String), or an empty string */
};

/* Indexes into ForeignScan.fdw_private for a direct UPDATE/DELETE */
enum TrinoDirectPrivateIndex
{
	TrinoDirectPrivateSql,		/* SQL template (String) */
	TrinoDirectPrivateParamOffsets, /* byte offsets of params (int list) */
	TrinoDirectPrivateRelid,	/* target foreign table OID (Integer) */
	TrinoDirectPrivateSetProcessed	/* whether to count rows (Integer) */
};

/* Indexes into ModifyTable fdw_private */
enum TrinoModifyPrivateIndex
{
	TrinoModifyPrivateSqlPrefix,	/* "INSERT INTO ... VALUES " */
	TrinoModifyPrivateTargetAttrs	/* int list */
};

/* Per-column conversion info shared by scans, ANALYZE and trino_query() */
typedef struct TrinoColumn
{
	AttrNumber	attnum;			/* 1-based attribute in the target tupdesc */
	Oid			typoid;
	int32		typmod;
	Oid			typioparam;
	FmgrInfo	infunc;
} TrinoColumn;

typedef struct TrinoScanState
{
	TrinoOptions *opts;
	char	   *sql_template;
	List	   *param_offsets;
	List	   *param_exprs;	/* ExprState list */
	List	   *param_types;	/* Oid list */
	int			ncols;
	TrinoColumn *cols;
	TrinoQuery *query;
	MemoryContext query_cxt;	/* owns the TrinoQuery */
	char	   *sql;			/* final SQL of the running query */
} TrinoScanState;

typedef struct TrinoModifyState
{
	TrinoOptions *opts;
	Relation	rel;
	char	   *sql_prefix;
	List	   *target_attrs;
	StringInfoData buf;
} TrinoModifyState;

static void trinoGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel,
								   Oid foreigntableid);
static void trinoGetForeignPaths(PlannerInfo *root, RelOptInfo *baserel,
								 Oid foreigntableid);
static ForeignScan *trinoGetForeignPlan(PlannerInfo *root, RelOptInfo *foreignrel,
										Oid foreigntableid, ForeignPath *best_path,
										List *tlist, List *scan_clauses,
										Plan *outer_plan);
static void trinoGetForeignUpperPaths(PlannerInfo *root, UpperRelationKind stage,
									  RelOptInfo *input_rel, RelOptInfo *output_rel,
									  void *extra);
static void trinoGetForeignJoinPaths(PlannerInfo *root, RelOptInfo *joinrel,
									 RelOptInfo *outerrel, RelOptInfo *innerrel,
									 JoinType jointype, JoinPathExtraData *extra);
static bool trinoPlanDirectModify(PlannerInfo *root, ModifyTable *plan,
								  Index resultRelation, int subplan_index);
static void trinoBeginDirectModify(ForeignScanState *node, int eflags);
static TupleTableSlot *trinoIterateDirectModify(ForeignScanState *node);
static void trinoEndDirectModify(ForeignScanState *node);
static void trinoExplainDirectModify(ForeignScanState *node, ExplainState *es);
static void trinoBeginForeignScan(ForeignScanState *node, int eflags);
static TupleTableSlot *trinoIterateForeignScan(ForeignScanState *node);
static void trinoReScanForeignScan(ForeignScanState *node);
static void trinoEndForeignScan(ForeignScanState *node);
static void trinoExplainForeignScan(ForeignScanState *node, ExplainState *es);
static List *trinoPlanForeignModify(PlannerInfo *root, ModifyTable *plan,
									Index resultRelation, int subplan_index);
static void trinoBeginForeignModify(ModifyTableState *mtstate,
									ResultRelInfo *rinfo, List *fdw_private,
									int subplan_index, int eflags);
static TupleTableSlot *trinoExecForeignInsert(EState *estate, ResultRelInfo *rinfo,
											  TupleTableSlot *slot,
											  TupleTableSlot *planSlot);
static TupleTableSlot *trinoExecForeignUpdate(EState *estate, ResultRelInfo *rinfo,
											  TupleTableSlot *slot,
											  TupleTableSlot *planSlot);
static TupleTableSlot *trinoExecForeignDelete(EState *estate, ResultRelInfo *rinfo,
											  TupleTableSlot *slot,
											  TupleTableSlot *planSlot);
static TupleTableSlot **trinoExecForeignBatchInsert(EState *estate,
													ResultRelInfo *rinfo,
													TupleTableSlot **slots,
													TupleTableSlot **planSlots,
													int *numSlots);
static int	trinoGetForeignModifyBatchSize(ResultRelInfo *rinfo);
static void trinoEndForeignModify(EState *estate, ResultRelInfo *rinfo);
static void trinoBeginForeignInsert(ModifyTableState *mtstate,
									ResultRelInfo *rinfo);
static void trinoEndForeignInsert(EState *estate, ResultRelInfo *rinfo);
static int	trinoIsForeignRelUpdatable(Relation rel);
static void trinoExplainForeignModify(ModifyTableState *mtstate,
									  ResultRelInfo *rinfo, List *fdw_private,
									  int subplan_index, ExplainState *es);
static bool trinoAnalyzeForeignTable(Relation relation,
									 AcquireSampleRowsFunc *func,
									 BlockNumber *totalpages);
static List *trinoImportForeignSchema(ImportForeignSchemaStmt *stmt,
									  Oid serverOid);

static void setup_columns(TupleDesc tupdesc, List *retrieved_attrs,
						  int *ncols, TrinoColumn **cols);
static void convert_row(TjValue *row, TrinoColumn *cols, int ncols,
						Datum *values, bool *nulls, const char *context);

/* ----------------------------------------------------------------
 * Module entry points
 * ---------------------------------------------------------------- */

void
_PG_init(void)
{
	trino_client_init();
}

Datum
trino_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *routine = makeNode(FdwRoutine);

	routine->GetForeignRelSize = trinoGetForeignRelSize;
	routine->GetForeignPaths = trinoGetForeignPaths;
	routine->GetForeignPlan = trinoGetForeignPlan;
	routine->GetForeignUpperPaths = trinoGetForeignUpperPaths;
	routine->GetForeignJoinPaths = trinoGetForeignJoinPaths;
	routine->BeginForeignScan = trinoBeginForeignScan;
	routine->IterateForeignScan = trinoIterateForeignScan;
	routine->ReScanForeignScan = trinoReScanForeignScan;
	routine->EndForeignScan = trinoEndForeignScan;
	routine->ExplainForeignScan = trinoExplainForeignScan;

	routine->PlanForeignModify = trinoPlanForeignModify;
	routine->BeginForeignModify = trinoBeginForeignModify;
	routine->ExecForeignInsert = trinoExecForeignInsert;
	routine->ExecForeignBatchInsert = trinoExecForeignBatchInsert;
	routine->ExecForeignUpdate = trinoExecForeignUpdate;
	routine->ExecForeignDelete = trinoExecForeignDelete;
	routine->GetForeignModifyBatchSize = trinoGetForeignModifyBatchSize;
	routine->EndForeignModify = trinoEndForeignModify;
	routine->BeginForeignInsert = trinoBeginForeignInsert;
	routine->EndForeignInsert = trinoEndForeignInsert;
	routine->IsForeignRelUpdatable = trinoIsForeignRelUpdatable;
	routine->ExplainForeignModify = trinoExplainForeignModify;
	routine->PlanDirectModify = trinoPlanDirectModify;
	routine->BeginDirectModify = trinoBeginDirectModify;
	routine->IterateDirectModify = trinoIterateDirectModify;
	routine->EndDirectModify = trinoEndDirectModify;
	routine->ExplainDirectModify = trinoExplainDirectModify;

	routine->AnalyzeForeignTable = trinoAnalyzeForeignTable;
	routine->ImportForeignSchema = trinoImportForeignSchema;

	PG_RETURN_POINTER(routine);
}

Datum
trino_fdw_version(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(TRINO_FDW_VERSION_STR));
}

/* ----------------------------------------------------------------
 * Options
 * ---------------------------------------------------------------- */

typedef struct TrinoOptionDef
{
	const char *name;
	Oid			context;		/* catalog OID the option applies to */
} TrinoOptionDef;

static const TrinoOptionDef valid_options[] = {
	{"host", ForeignServerRelationId},
	{"port", ForeignServerRelationId},
	{"ssl", ForeignServerRelationId},
	{"ssl_verify", ForeignServerRelationId},
	{"ca_file", ForeignServerRelationId},
	{"catalog", ForeignServerRelationId},
	{"schema", ForeignServerRelationId},
	{"source", ForeignServerRelationId},
	{"session_properties", ForeignServerRelationId},
	{"client_tags", ForeignServerRelationId},
	{"spooling", ForeignServerRelationId},
	{"connect_timeout", ForeignServerRelationId},
	{"request_timeout", ForeignServerRelationId},
	{"startup_cost", ForeignServerRelationId},
	{"tuple_cost", ForeignServerRelationId},
	{"default_rows", ForeignServerRelationId},
	{"batch_size", ForeignServerRelationId},

	{"user", UserMappingRelationId},
	{"password", UserMappingRelationId},

	{"catalog", ForeignTableRelationId},
	{"schema_name", ForeignTableRelationId},
	{"table_name", ForeignTableRelationId},
	{"batch_size", ForeignTableRelationId},
	{"default_rows", ForeignTableRelationId},

	{"column_name", AttributeRelationId},

	{NULL, InvalidOid}
};

static bool
is_valid_option(const char *name, Oid context)
{
	const TrinoOptionDef *d;

	for (d = valid_options; d->name; d++)
		if (d->context == context && strcmp(d->name, name) == 0)
			return true;
	return false;
}

static int
option_int(DefElem *def, int min, int max)
{
	char	   *s = defGetString(def);
	char	   *end;
	long		v;

	errno = 0;
	v = strtol(s, &end, 10);
	if (*s == '\0' || *end != '\0' || errno != 0 || v < min || v > max)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("trino_fdw: invalid value for option \"%s\": \"%s\"",
						def->defname, s),
				 errdetail("Expected an integer between %d and %d.", min, max)));
	return (int) v;
}

static double
option_double(DefElem *def, double min)
{
	char	   *s = defGetString(def);
	char	   *end;
	double		v;

	errno = 0;
	v = strtod(s, &end);
	if (*s == '\0' || *end != '\0' || errno != 0 || v < min)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("trino_fdw: invalid value for option \"%s\": \"%s\"",
						def->defname, s)));
	return v;
}

Datum
trino_fdw_validator(PG_FUNCTION_ARGS)
{
	List	   *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			catalog = PG_GETARG_OID(1);
	ListCell   *lc;

	foreach(lc, options_list)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (!is_valid_option(def->defname, catalog))
		{
			const TrinoOptionDef *d;
			StringInfoData buf;

			initStringInfo(&buf);
			for (d = valid_options; d->name; d++)
				if (d->context == catalog)
					appendStringInfo(&buf, "%s%s", buf.len > 0 ? ", " : "", d->name);
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("invalid option \"%s\"", def->defname),
					 buf.len > 0 ? errhint("Valid options in this context are: %s",
										   buf.data) :
					 errhint("There are no valid options in this context.")));
		}

		if (strcmp(def->defname, "port") == 0)
			(void) option_int(def, 1, 65535);
		else if (strcmp(def->defname, "connect_timeout") == 0 ||
				 strcmp(def->defname, "request_timeout") == 0)
			(void) option_int(def, 0, INT_MAX);
		else if (strcmp(def->defname, "batch_size") == 0)
			(void) option_int(def, 1, INT_MAX);
		else if (strcmp(def->defname, "default_rows") == 0)
			(void) option_int(def, 1, INT_MAX);
		else if (strcmp(def->defname, "startup_cost") == 0 ||
				 strcmp(def->defname, "tuple_cost") == 0)
			(void) option_double(def, 0.0);
		else if (strcmp(def->defname, "ssl") == 0 ||
				 strcmp(def->defname, "ssl_verify") == 0 ||
				 strcmp(def->defname, "spooling") == 0)
			(void) defGetBoolean(def);
	}

	PG_RETURN_VOID();
}

/* Options of the user mapping for (userid, serverid), or NIL if none */
static List *
get_user_mapping_options(Oid userid, Oid serverid)
{
	HeapTuple	tp;
	Datum		datum;
	bool		isnull;
	List	   *options = NIL;

	tp = SearchSysCache2(USERMAPPINGUSERSERVER,
						 ObjectIdGetDatum(userid), ObjectIdGetDatum(serverid));
	if (!HeapTupleIsValid(tp))
		tp = SearchSysCache2(USERMAPPINGUSERSERVER,
							 ObjectIdGetDatum(InvalidOid), ObjectIdGetDatum(serverid));
	if (!HeapTupleIsValid(tp))
		return NIL;

	datum = SysCacheGetAttr(USERMAPPINGUSERSERVER, tp,
							Anum_pg_user_mapping_umoptions, &isnull);
	if (!isnull)
		options = untransformRelOptions(datum);
	ReleaseSysCache(tp);
	return options;
}

static void
apply_options(TrinoOptions *opts, List *options)
{
	ListCell   *lc;

	foreach(lc, options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);
		const char *name = def->defname;

		if (strcmp(name, "host") == 0)
			opts->host = defGetString(def);
		else if (strcmp(name, "port") == 0)
			opts->port = option_int(def, 1, 65535);
		else if (strcmp(name, "ssl") == 0)
			opts->ssl = defGetBoolean(def);
		else if (strcmp(name, "ssl_verify") == 0)
			opts->ssl_verify = defGetBoolean(def);
		else if (strcmp(name, "ca_file") == 0)
			opts->ca_file = defGetString(def);
		else if (strcmp(name, "catalog") == 0)
			opts->catalog = defGetString(def);
		else if (strcmp(name, "schema") == 0)
			opts->schema = defGetString(def);
		else if (strcmp(name, "schema_name") == 0)
			opts->schema_name = defGetString(def);
		else if (strcmp(name, "table_name") == 0)
			opts->table_name = defGetString(def);
		else if (strcmp(name, "source") == 0)
			opts->source = defGetString(def);
		else if (strcmp(name, "session_properties") == 0)
			opts->session_properties = defGetString(def);
		else if (strcmp(name, "client_tags") == 0)
			opts->client_tags = defGetString(def);
		else if (strcmp(name, "spooling") == 0)
			opts->spooling = defGetBoolean(def);
		else if (strcmp(name, "connect_timeout") == 0)
			opts->connect_timeout = option_int(def, 0, INT_MAX);
		else if (strcmp(name, "request_timeout") == 0)
			opts->request_timeout = option_int(def, 0, INT_MAX);
		else if (strcmp(name, "startup_cost") == 0)
			opts->startup_cost = option_double(def, 0.0);
		else if (strcmp(name, "tuple_cost") == 0)
			opts->tuple_cost = option_double(def, 0.0);
		else if (strcmp(name, "default_rows") == 0)
			opts->default_rows = option_int(def, 1, INT_MAX);
		else if (strcmp(name, "batch_size") == 0)
			opts->batch_size = option_int(def, 1, INT_MAX);
		else if (strcmp(name, "user") == 0)
			opts->user = defGetString(def);
		else if (strcmp(name, "password") == 0)
			opts->password = defGetString(def);
	}
}

TrinoOptions *
trino_get_server_options(Oid serverid, Oid userid)
{
	ForeignServer *server = GetForeignServer(serverid);
	TrinoOptions *opts = palloc0(sizeof(TrinoOptions));

	opts->port = 8080;
	opts->ssl = false;
	opts->ssl_verify = true;
	opts->source = "trino_fdw";
	opts->spooling = true;
	opts->connect_timeout = 10;
	opts->request_timeout = 0;
	/*
	 * Like postgres_fdw's fdw_startup_cost.  Keep this moderate: the planner
	 * treats costs within 1% as equal, so a huge constant would hide the
	 * differences between pushing work down and doing it locally.
	 */
	opts->startup_cost = 100.0;
	opts->tuple_cost = 0.2;
	opts->default_rows = 10000;
	opts->batch_size = 500;
	opts->user = GetUserNameFromId(userid, false);

	apply_options(opts, server->options);
	apply_options(opts, get_user_mapping_options(userid, serverid));

	if (opts->host == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_OPTION_NAME_NOT_FOUND),
				 errmsg("trino_fdw: server \"%s\" has no \"host\" option",
						server->servername)));
	return opts;
}

TrinoOptions *
trino_get_options(Oid foreigntableid, Oid serverid, Oid userid)
{
	ForeignTable *table = GetForeignTable(foreigntableid);
	TrinoOptions *opts;

	if (!OidIsValid(serverid))
		serverid = table->serverid;
	opts = trino_get_server_options(serverid, userid);
	apply_options(opts, table->options);

	if (opts->schema_name == NULL)
		opts->schema_name = opts->schema;
	return opts;
}

/* ----------------------------------------------------------------
 * Planner: base relation
 * ---------------------------------------------------------------- */

static Oid
rel_check_user(RangeTblEntry *rte)
{
#if PG_VERSION_NUM >= 160000
	return GetUserId();			/* checkAsUser handled via RTEPermissionInfo */
#else
	return rte->checkAsUser ? rte->checkAsUser : GetUserId();
#endif
}

static void
trinoGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{
	TrinoRelationInfo *fpinfo;
	RangeTblEntry *rte = planner_rt_fetch(baserel->relid, root);
	ListCell   *lc;
	Selectivity remote_sel;
	double		retrieved_rows;
	QualCost	local_cost;

	fpinfo = palloc0(sizeof(TrinoRelationInfo));
	baserel->fdw_private = fpinfo;
	fpinfo->pushdown_safe = true;
	fpinfo->relid = foreigntableid;
	fpinfo->userid = rel_check_user(rte);
	fpinfo->opts = trino_get_options(foreigntableid, InvalidOid, fpinfo->userid);

	/* classify restrictions */
	foreach(lc, baserel->baserestrictinfo)
	{
		RestrictInfo *ri = (RestrictInfo *) lfirst(lc);

		if (trino_is_foreign_expr(root, baserel, ri->clause))
			fpinfo->remote_conds = lappend(fpinfo->remote_conds, ri);
		else
			fpinfo->local_conds = lappend(fpinfo->local_conds, ri);
	}

	/* columns we must fetch: output columns plus those in local quals */
	fpinfo->attrs_used = NULL;
	pull_varattnos((Node *) baserel->reltarget->exprs, baserel->relid,
				   &fpinfo->attrs_used);
	foreach(lc, fpinfo->local_conds)
	{
		RestrictInfo *ri = (RestrictInfo *) lfirst(lc);

		pull_varattnos((Node *) ri->clause, baserel->relid, &fpinfo->attrs_used);
	}

	/* size estimates: use ANALYZE stats if present, else the option */
	if (baserel->tuples < 0)
	{
		baserel->pages = 0;
		baserel->tuples = fpinfo->opts->default_rows;
	}
	set_baserel_size_estimates(root, baserel);
	fpinfo->rows = baserel->rows;
	fpinfo->width = baserel->reltarget->width;

	remote_sel = clauselist_selectivity(root, fpinfo->remote_conds,
										baserel->relid, JOIN_INNER, NULL);
	retrieved_rows = clamp_row_est(baserel->tuples * remote_sel);
	cost_qual_eval(&local_cost, fpinfo->local_conds, root);

	fpinfo->startup_cost = fpinfo->opts->startup_cost + local_cost.startup;
	fpinfo->total_cost = fpinfo->startup_cost +
		retrieved_rows * (cpu_tuple_cost + fpinfo->opts->tuple_cost +
						  local_cost.per_tuple) +
		fpinfo->rows * cpu_tuple_cost;
	fpinfo->rel_startup_cost = fpinfo->startup_cost;
	fpinfo->rel_total_cost = fpinfo->total_cost;
}

#if PG_VERSION_NUM >= 170000
#define TRINO_CREATE_FOREIGNSCAN_PATH(root, rel, rows, startup, total, pathkeys, outer, priv) \
	create_foreignscan_path(root, rel, NULL, rows, startup, total, pathkeys, outer, NULL, NIL, priv)
#define TRINO_CREATE_FOREIGN_JOIN_PATH(root, rel, rows, startup, total, pathkeys, outer, priv) \
	create_foreign_join_path(root, rel, NULL, rows, startup, total, pathkeys, outer, NULL, NIL, priv)
#define TRINO_CREATE_FOREIGN_UPPER_PATH(root, rel, target, rows, startup, total, pathkeys, priv) \
	create_foreign_upper_path(root, rel, target, rows, startup, total, pathkeys, NULL, NIL, priv)
#else
#define TRINO_CREATE_FOREIGNSCAN_PATH(root, rel, rows, startup, total, pathkeys, outer, priv) \
	create_foreignscan_path(root, rel, NULL, rows, startup, total, pathkeys, outer, NULL, priv)
#define TRINO_CREATE_FOREIGN_JOIN_PATH(root, rel, rows, startup, total, pathkeys, outer, priv) \
	create_foreign_join_path(root, rel, NULL, rows, startup, total, pathkeys, outer, NULL, priv)
#define TRINO_CREATE_FOREIGN_UPPER_PATH(root, rel, target, rows, startup, total, pathkeys, priv) \
	create_foreign_upper_path(root, rel, target, rows, startup, total, pathkeys, NULL, priv)
#endif

/*
 * Rough cost of sorting n rows on the Trino side: half the comparison cost
 * of a local sort, since the work is distributed.
 */
static Cost
remote_sort_cost(double n)
{
	if (n < 2.0)
		n = 2.0;
	return n * log(n) / log(2.0) * cpu_operator_cost;
}

/* Can every pathkey be turned into a Trino ORDER BY item? */
static bool
pathkeys_pushable(PlannerInfo *root, RelOptInfo *rel, List *pathkeys)
{
	ListCell   *lc;

	if (pathkeys == NIL)
		return false;
	foreach(lc, pathkeys)
	{
		if (!trino_is_sortable_pathkey(root, rel, (PathKey *) lfirst(lc)))
			return false;
	}
	return true;
}

/* Cost of fetching a result of the given size, excluding remote work */
static void
transfer_cost(TrinoOptions *opts, double retrieved_rows, Cost *startup, Cost *total)
{
	*startup = opts->startup_cost;
	*total = *startup + retrieved_rows * (cpu_tuple_cost + opts->tuple_cost);
}

/*
 * Add a pre-sorted variant of a path when the query wants its output in an
 * order Trino can produce.  The planner compares it against a local Sort.
 */
static void
add_sorted_path(PlannerInfo *root, RelOptInfo *rel, TrinoRelationInfo *fpinfo,
				bool is_join)
{
	TrinoRelationInfo *sorted;
	ForeignPath *path;

	if (!pathkeys_pushable(root, rel, root->query_pathkeys))
		return;

	sorted = palloc0(sizeof(TrinoRelationInfo));
	sorted->pathkeys = root->query_pathkeys;
	if (is_join)
		path = TRINO_CREATE_FOREIGN_JOIN_PATH(root, rel, fpinfo->rows,
											  fpinfo->startup_cost,
											  fpinfo->total_cost + remote_sort_cost(fpinfo->rows),
											  root->query_pathkeys,
											  rel->lateral_relids,
											  list_make1(sorted));
	else
		path = TRINO_CREATE_FOREIGNSCAN_PATH(root, rel, fpinfo->rows,
											 fpinfo->startup_cost,
											 fpinfo->total_cost + remote_sort_cost(fpinfo->rows),
											 root->query_pathkeys,
											 rel->lateral_relids,
											 list_make1(sorted));
	add_path(rel, (Path *) path);
}

/* State for generate_implied_equalities_for_column's callback */
typedef struct ec_member_arg
{
	Expr	   *current;		/* the EC member we are building clauses for */
	List	   *already_used;	/* EC members already handled */
} ec_member_arg;

static bool
ec_member_matches_foreign(PlannerInfo *root, RelOptInfo *rel,
						  EquivalenceClass *ec, EquivalenceMember *em,
						  void *arg)
{
	ec_member_arg *state = (ec_member_arg *) arg;
	Expr	   *expr = em->em_expr;

	if (state->current == NULL)
	{
		if (list_member(state->already_used, expr))
			return false;
		state->current = expr;
		return true;
	}
	return equal(expr, state->current);
}

/*
 * Parameterized paths: a join clause such as t.k = outer.k can be sent to
 * Trino with the outer value as a literal, which lets a nested loop probe
 * Trino once per outer row.  Each probe is a full Trino query, so these
 * paths only win when the outer side is small; the costs reflect that.
 */
static void
add_parameterized_paths(PlannerInfo *root, RelOptInfo *baserel,
						TrinoRelationInfo *fpinfo)
{
	List	   *ppi_list = NIL;
	ListCell   *lc;

	foreach(lc, baserel->joininfo)
	{
		RestrictInfo *ri = (RestrictInfo *) lfirst(lc);
		Relids		required_outer;
		ParamPathInfo *ppi;

		if (!join_clause_is_movable_to(ri, baserel))
			continue;
		if (!trino_is_foreign_expr(root, baserel, ri->clause))
			continue;
		required_outer = bms_union(ri->clause_relids, baserel->lateral_relids);
		required_outer = bms_del_member(required_outer, baserel->relid);
		if (bms_is_empty(required_outer))
			continue;
		ppi = get_baserel_parampathinfo(root, baserel, required_outer);
		ppi_list = list_append_unique_ptr(ppi_list, ppi);
	}

	/* clauses implied by equivalence classes, e.g. t.k = outer.k */
	if (baserel->has_eclass_joins)
	{
		ec_member_arg arg;

		arg.already_used = NIL;
		for (;;)
		{
			List	   *clauses;

			arg.current = NULL;
			clauses = generate_implied_equalities_for_column(root, baserel,
															 ec_member_matches_foreign,
															 (void *) &arg,
															 baserel->lateral_referencers);
			if (arg.current == NULL)
				break;
			arg.already_used = lappend(arg.already_used, arg.current);

			foreach(lc, clauses)
			{
				RestrictInfo *ri = (RestrictInfo *) lfirst(lc);
				Relids		required_outer;
				ParamPathInfo *ppi;

				if (!join_clause_is_movable_to(ri, baserel))
					continue;
				if (!trino_is_foreign_expr(root, baserel, ri->clause))
					continue;
				required_outer = bms_union(ri->clause_relids, baserel->lateral_relids);
				required_outer = bms_del_member(required_outer, baserel->relid);
				if (bms_is_empty(required_outer))
					continue;
				ppi = get_baserel_parampathinfo(root, baserel, required_outer);
				ppi_list = list_append_unique_ptr(ppi_list, ppi);
			}
		}
	}

	foreach(lc, ppi_list)
	{
		ParamPathInfo *ppi = (ParamPathInfo *) lfirst(lc);
		Selectivity sel;
		double		retrieved_rows;
		Cost		startup_cost;
		Cost		total_cost;
		ForeignPath *path;

		/* the parameter clauses also filter remotely */
		sel = clauselist_selectivity(root, fpinfo->remote_conds, baserel->relid,
									 JOIN_INNER, NULL) *
			clauselist_selectivity(root, ppi->ppi_clauses, baserel->relid,
								   JOIN_INNER, NULL);
		retrieved_rows = clamp_row_est(baserel->tuples * sel);
		transfer_cost(fpinfo->opts, retrieved_rows, &startup_cost, &total_cost);

		path = TRINO_CREATE_FOREIGNSCAN_PATH(root, baserel, ppi->ppi_rows,
											 startup_cost, total_cost, NIL,
											 ppi->ppi_req_outer, NIL);
		add_path(baserel, (Path *) path);
	}
}

static void
trinoGetForeignPaths(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{
	TrinoRelationInfo *fpinfo = (TrinoRelationInfo *) baserel->fdw_private;
	ForeignPath *path;

	path = TRINO_CREATE_FOREIGNSCAN_PATH(root, baserel, fpinfo->rows,
										 fpinfo->startup_cost, fpinfo->total_cost,
										 NIL, baserel->lateral_relids, NIL);
	add_path(baserel, (Path *) path);

	add_sorted_path(root, baserel, fpinfo, false);
	add_parameterized_paths(root, baserel, fpinfo);
}

/* ----------------------------------------------------------------
 * Planner: join pushdown
 * ---------------------------------------------------------------- */

/*
 * Decide whether the join can run on Trino, and if so fill in the joinrel's
 * TrinoRelationInfo (join clauses, WHERE clauses, local clauses).
 */
static bool
foreign_join_ok(PlannerInfo *root, RelOptInfo *joinrel, JoinType jointype,
				RelOptInfo *outerrel, RelOptInfo *innerrel,
				JoinPathExtraData *extra)
{
	TrinoRelationInfo *fpinfo = (TrinoRelationInfo *) joinrel->fdw_private;
	TrinoRelationInfo *fpinfo_o = (TrinoRelationInfo *) outerrel->fdw_private;
	TrinoRelationInfo *fpinfo_i = (TrinoRelationInfo *) innerrel->fdw_private;
	List	   *joinclauses = NIL;
	List	   *vars;
	ListCell   *lc;

	if (jointype != JOIN_INNER && jointype != JOIN_LEFT &&
		jointype != JOIN_RIGHT && jointype != JOIN_FULL)
		return false;
	if (fpinfo_o == NULL || !fpinfo_o->pushdown_safe ||
		fpinfo_i == NULL || !fpinfo_i->pushdown_safe)
		return false;
	/* grouped relations never appear as join inputs, but be explicit */
	if (IS_UPPER_REL(outerrel) || IS_UPPER_REL(innerrel))
		return false;

	/*
	 * Row marks (FOR UPDATE, or an UPDATE/DELETE joining this relation) need
	 * an EvalPlanQual recheck we cannot provide for a remote join.
	 */
	if (root->rowMarks != NIL)
		return false;

	/* PlaceHolderVars evaluated inside the join cannot be deparsed */
	foreach(lc, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(lc);

		if (bms_overlap(phinfo->ph_eval_at, joinrel->relids))
			return false;
	}

	/*
	 * Sort the join's restriction clauses: real join conditions of an outer
	 * join must be shippable (they define the result), WHERE-level clauses
	 * may stay local.
	 */
	foreach(lc, extra->restrictlist)
	{
		RestrictInfo *ri = lfirst_node(RestrictInfo, lc);
		bool		is_remote = trino_is_foreign_expr(root, joinrel, ri->clause);
		bool		is_join_clause = !RINFO_IS_PUSHED_DOWN(ri, joinrel->relids);

		if (is_join_clause)
		{
			if (!is_remote)
			{
				if (jointype != JOIN_INNER)
					return false;
				fpinfo->local_conds = lappend(fpinfo->local_conds, ri);
			}
			else
				joinclauses = lappend(joinclauses, ri);
		}
		else if (is_remote)
		{
			/*
			 * For an inner join, WHERE and ON are equivalent; conditions
			 * spanning both inputs read better in the ON clause.
			 */
			if (jointype == JOIN_INNER &&
				bms_overlap(ri->clause_relids, outerrel->relids) &&
				bms_overlap(ri->clause_relids, innerrel->relids))
				joinclauses = lappend(joinclauses, ri);
			else
				fpinfo->remote_conds = lappend(fpinfo->remote_conds, ri);
		}
		else
			fpinfo->local_conds = lappend(fpinfo->local_conds, ri);
	}

	/*
	 * Fold the inputs' own conditions into the join.  Conditions on the
	 * nullable side of an outer join must be applied before the join, so
	 * they move into the ON clause; local conditions there cannot be
	 * pulled up at all.
	 */
	switch (jointype)
	{
		case JOIN_INNER:
			fpinfo->remote_conds = list_concat(fpinfo->remote_conds,
											   list_copy(fpinfo_i->remote_conds));
			fpinfo->remote_conds = list_concat(fpinfo->remote_conds,
											   list_copy(fpinfo_o->remote_conds));
			fpinfo->local_conds = list_concat(fpinfo->local_conds,
											  list_copy(fpinfo_i->local_conds));
			fpinfo->local_conds = list_concat(fpinfo->local_conds,
											  list_copy(fpinfo_o->local_conds));
			break;
		case JOIN_LEFT:
			if (fpinfo_i->local_conds != NIL)
				return false;
			joinclauses = list_concat(joinclauses, list_copy(fpinfo_i->remote_conds));
			fpinfo->remote_conds = list_concat(fpinfo->remote_conds,
											   list_copy(fpinfo_o->remote_conds));
			fpinfo->local_conds = list_concat(fpinfo->local_conds,
											  list_copy(fpinfo_o->local_conds));
			break;
		case JOIN_RIGHT:
			if (fpinfo_o->local_conds != NIL)
				return false;
			joinclauses = list_concat(joinclauses, list_copy(fpinfo_o->remote_conds));
			fpinfo->remote_conds = list_concat(fpinfo->remote_conds,
											   list_copy(fpinfo_i->remote_conds));
			fpinfo->local_conds = list_concat(fpinfo->local_conds,
											  list_copy(fpinfo_i->local_conds));
			break;
		case JOIN_FULL:
			if (fpinfo_i->remote_conds != NIL || fpinfo_i->local_conds != NIL ||
				fpinfo_o->remote_conds != NIL || fpinfo_o->local_conds != NIL)
				return false;
			break;
		default:
			return false;
	}

	/* every output column must be a plain column of some input */
	vars = pull_var_clause((Node *) joinrel->reltarget->exprs,
						   PVC_RECURSE_PLACEHOLDERS);
	foreach(lc, fpinfo->local_conds)
		vars = list_concat(vars,
						   pull_var_clause((Node *) ((RestrictInfo *) lfirst(lc))->clause,
										   PVC_RECURSE_PLACEHOLDERS));
	foreach(lc, vars)
	{
		Var		   *var = (Var *) lfirst(lc);

		if (!IsA(var, Var) || var->varattno <= 0)
			return false;
	}

	fpinfo->outerrel = outerrel;
	fpinfo->innerrel = innerrel;
	fpinfo->jointype = jointype;
	fpinfo->joinclauses = joinclauses;
	fpinfo->opts = fpinfo_o->opts;
	fpinfo->relid = fpinfo_o->relid;
	fpinfo->userid = fpinfo_o->userid;
	fpinfo->pushdown_safe = true;
	return true;
}

static void
trinoGetForeignJoinPaths(PlannerInfo *root, RelOptInfo *joinrel,
						 RelOptInfo *outerrel, RelOptInfo *innerrel,
						 JoinType jointype, JoinPathExtraData *extra)
{
	TrinoRelationInfo *fpinfo;
	TrinoRelationInfo *fpinfo_o;
	TrinoRelationInfo *fpinfo_i;
	Selectivity local_sel;
	double		retrieved_rows;
	QualCost	local_cost;
	Cost		startup_cost;
	Cost		total_cost;
	ForeignPath *path;

	/* already decided for this joinrel (other join orders) */
	if (joinrel->fdw_private != NULL)
		return;

	fpinfo = palloc0(sizeof(TrinoRelationInfo));
	joinrel->fdw_private = fpinfo;
	if (!foreign_join_ok(root, joinrel, jointype, outerrel, innerrel, extra))
	{
		fpinfo->pushdown_safe = false;
		return;
	}
	fpinfo_o = (TrinoRelationInfo *) outerrel->fdw_private;
	fpinfo_i = (TrinoRelationInfo *) innerrel->fdw_private;
	fpinfo->relation_name = trino_relation_name(root, joinrel);

	/* rows: the planner's estimate; before local quals, a bit more */
	fpinfo->rows = joinrel->rows;
	local_sel = clauselist_selectivity(root, fpinfo->local_conds, 0,
									   jointype, extra->sjinfo);
	retrieved_rows = clamp_row_est(joinrel->rows / Max(local_sel, 1e-6));
	cost_qual_eval(&local_cost, fpinfo->local_conds, root);

	transfer_cost(fpinfo->opts, retrieved_rows, &startup_cost, &total_cost);
	/* the remote join itself: a hash join over both inputs */
	total_cost += (fpinfo_o->rows + fpinfo_i->rows) * cpu_operator_cost;
	total_cost += local_cost.startup + retrieved_rows * local_cost.per_tuple;
	fpinfo->startup_cost = startup_cost;
	fpinfo->total_cost = total_cost;

	path = TRINO_CREATE_FOREIGN_JOIN_PATH(root, joinrel, fpinfo->rows,
										  startup_cost, total_cost, NIL,
										  joinrel->lateral_relids, NIL);
	add_path(joinrel, (Path *) path);

	add_sorted_path(root, joinrel, fpinfo, true);
}

/* ----------------------------------------------------------------
 * Planner: upper relations (GROUP BY, ORDER BY, LIMIT)
 * ---------------------------------------------------------------- */

/*
 * Check the grouping target and HAVING, and build the list of expressions
 * the remote query must return: grouping expressions and aggregates.
 */
static bool
foreign_grouping_ok(PlannerInfo *root, RelOptInfo *grouped_rel, Node *having_qual)
{
	Query	   *query = root->parse;
	TrinoRelationInfo *fpinfo = (TrinoRelationInfo *) grouped_rel->fdw_private;
	PathTarget *grouping_target = grouped_rel->reltarget;
	List	   *tlist = NIL;
	ListCell   *lc;
	int			i = 0;

	foreach(lc, grouping_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		Index		sgref = get_pathtarget_sortgroupref(grouping_target, i);

		if (sgref && get_sortgroupref_clause_noerr(sgref, query->groupClause))
		{
			TargetEntry *tle;

			/* a grouping expression: must be shippable as a whole */
			if (!trino_is_foreign_expr(root, grouped_rel, expr))
				return false;
			tle = makeTargetEntry(expr, list_length(tlist) + 1, NULL, false);
			tle->ressortgroupref = sgref;
			tlist = lappend(tlist, tle);
		}
		else if (trino_is_foreign_expr(root, grouped_rel, expr))
		{
			/* an expression over grouping columns and aggregates */
			tlist = add_to_flat_tlist(tlist, list_make1(expr));
		}
		else
		{
			/*
			 * Not shippable as a whole: fetch the aggregates inside it and
			 * compute the rest locally.  Plain Vars in it are grouping
			 * columns (the parser guarantees that) already in the tlist.
			 */
			List	   *aggvars = pull_var_clause((Node *) expr,
												  PVC_INCLUDE_AGGREGATES);
			ListCell   *lc2;

			foreach(lc2, aggvars)
			{
				Expr	   *sub = (Expr *) lfirst(lc2);

				if (!trino_is_foreign_expr(root, grouped_rel, sub))
					return false;
				if (IsA(sub, Aggref))
					tlist = add_to_flat_tlist(tlist, list_make1(sub));
			}
		}
		i++;
	}

	if (having_qual != NULL)
	{
		foreach(lc, (List *) having_qual)
		{
			Expr	   *expr = (Expr *) lfirst(lc);

			if (trino_is_foreign_expr(root, grouped_rel, expr))
				fpinfo->having_remote = lappend(fpinfo->having_remote, expr);
			else
				fpinfo->having_local = lappend(fpinfo->having_local, expr);
		}
	}

	/* aggregates in local HAVING clauses must be fetched too */
	if (fpinfo->having_local != NIL)
	{
		List	   *aggvars = pull_var_clause((Node *) fpinfo->having_local,
											  PVC_INCLUDE_AGGREGATES);

		foreach(lc, aggvars)
		{
			Expr	   *expr = (Expr *) lfirst(lc);

			if (IsA(expr, Aggref) && !trino_is_foreign_expr(root, grouped_rel, expr))
				return false;
			tlist = add_to_flat_tlist(tlist, list_make1(expr));
		}
	}

	fpinfo->grouped_tlist = tlist;
	return true;
}

static void
add_foreign_grouping_paths(PlannerInfo *root, RelOptInfo *input_rel,
						   RelOptInfo *grouped_rel, GroupPathExtraData *extra)
{
	Query	   *parse = root->parse;
	TrinoRelationInfo *ifpinfo = (TrinoRelationInfo *) input_rel->fdw_private;
	TrinoRelationInfo *fpinfo;
	List	   *group_exprs;
	double		num_groups;
	Cost		startup_cost;
	Cost		total_cost;
	ForeignPath *path;

	if (parse->groupingSets != NIL)
		return;
	if (extra->patype != PARTITIONWISE_AGGREGATE_NONE)
		return;
	if (IS_UPPER_REL(input_rel))
		return;
	/* local quals must run before aggregation, so nothing can be pushed */
	if (ifpinfo->local_conds != NIL)
		return;
	if (root->hasHavingQual && extra->havingQual == NULL)
		return;

	fpinfo = palloc0(sizeof(TrinoRelationInfo));
	fpinfo->is_grouped = true;
	fpinfo->outerrel = input_rel;
	fpinfo->opts = ifpinfo->opts;
	fpinfo->relid = ifpinfo->relid;
	fpinfo->userid = ifpinfo->userid;
	grouped_rel->fdw_private = fpinfo;

	if (!foreign_grouping_ok(root, grouped_rel, extra->havingQual))
	{
		fpinfo->pushdown_safe = false;
		return;
	}
	fpinfo->pushdown_safe = true;
	fpinfo->relation_name = trino_relation_name(root, grouped_rel);

	/* estimates: Trino returns one row per group */
	group_exprs = get_sortgrouplist_exprs(parse->groupClause, fpinfo->grouped_tlist);
	if (group_exprs != NIL)
		num_groups = estimate_num_groups(root, group_exprs, ifpinfo->rows, NULL, NULL);
	else
		num_groups = 1;
	if (fpinfo->having_remote != NIL || fpinfo->having_local != NIL)
	{
		Selectivity sel = clauselist_selectivity(root, fpinfo->having_remote, 0,
												 JOIN_INNER, NULL);

		fpinfo->rows = clamp_row_est(num_groups * sel);
	}
	else
		fpinfo->rows = num_groups;

	transfer_cost(fpinfo->opts, num_groups, &startup_cost, &total_cost);
	/* aggregating on the Trino side: one pass over the input */
	total_cost += ifpinfo->rows * cpu_operator_cost * (list_length(fpinfo->grouped_tlist) + 1);
	fpinfo->startup_cost = startup_cost;
	fpinfo->total_cost = total_cost;

	path = TRINO_CREATE_FOREIGN_UPPER_PATH(root, grouped_rel,
										   grouped_rel->reltarget,
										   fpinfo->rows, startup_cost, total_cost,
										   NIL, NIL);
	add_path(grouped_rel, (Path *) path);
}

static bool
limit_node_ok(Node *node)
{
	Const	   *c;

	if (node == NULL)
		return true;
	if (!IsA(node, Const))
		return false;
	c = (Const *) node;
	if (c->constisnull)
		return true;			/* LIMIT NULL / OFFSET NULL = no limit */
	if (c->consttype != INT8OID)
		return false;
	return DatumGetInt64(c->constvalue) >= 0;
}

/*
 * UPPERREL_ORDERED.  For a base or join input the pre-sorted path on that
 * relation already covers ORDER BY, so only a record is kept for the FINAL
 * stage.  For a grouped input a sorted path is added here.
 */
static void
add_foreign_ordered_paths(PlannerInfo *root, RelOptInfo *input_rel,
						  RelOptInfo *ordered_rel)
{
	TrinoRelationInfo *ifpinfo = (TrinoRelationInfo *) input_rel->fdw_private;
	TrinoRelationInfo *fpinfo;

	fpinfo = palloc0(sizeof(TrinoRelationInfo));
	fpinfo->outerrel = input_rel;
	fpinfo->opts = ifpinfo->opts;
	fpinfo->relid = ifpinfo->relid;
	fpinfo->userid = ifpinfo->userid;
	fpinfo->rows = ifpinfo->rows;
	fpinfo->startup_cost = ifpinfo->startup_cost;
	fpinfo->total_cost = ifpinfo->total_cost + remote_sort_cost(ifpinfo->rows);
	fpinfo->pushdown_safe = !root->parse->hasTargetSRFs &&
		pathkeys_pushable(root, input_rel, root->sort_pathkeys);
	fpinfo->pathkeys = fpinfo->pushdown_safe ? root->sort_pathkeys : NIL;
	ordered_rel->fdw_private = fpinfo;

	if (fpinfo->pushdown_safe && IS_UPPER_REL(input_rel))
	{
		TrinoRelationInfo *sorted = palloc0(sizeof(TrinoRelationInfo));
		ForeignPath *path;

		sorted->pathkeys = root->sort_pathkeys;
		path = TRINO_CREATE_FOREIGN_UPPER_PATH(root, input_rel,
											   root->upper_targets[UPPERREL_ORDERED],
											   fpinfo->rows, fpinfo->startup_cost,
											   fpinfo->total_cost,
											   root->sort_pathkeys,
											   list_make1(sorted));
		add_path(ordered_rel, (Path *) path);
	}
}

/*
 * UPPERREL_FINAL: push LIMIT/OFFSET (and the ORDER BY beneath it) down.
 * The new path's parent is the relation being scanned (base, join or
 * grouped), so GetForeignPlan sees an ordinary scan of it plus the extra
 * clauses carried in the path's fdw_private.
 */
static void
add_foreign_final_paths(PlannerInfo *root, RelOptInfo *input_rel,
						RelOptInfo *final_rel, FinalPathExtraData *extra)
{
	Query	   *parse = root->parse;
	TrinoRelationInfo *ifpinfo = (TrinoRelationInfo *) input_rel->fdw_private;
	TrinoRelationInfo *scan_fpinfo;
	TrinoRelationInfo *fpinfo;
	RelOptInfo *scanrel;
	List	   *pathkeys = NIL;
	Node	   *limit_count;
	Node	   *limit_offset;
	double		rows;
	double		fetched;
	Cost		startup_cost;
	Cost		total_cost;
	ForeignPath *path;

	if (!extra->limit_needed)
		return;
	if (parse->rowMarks != NIL || parse->hasTargetSRFs)
		return;
	if (parse->limitOption != LIMIT_OPTION_COUNT)
		return;
	if (!limit_node_ok(parse->limitCount) || !limit_node_ok(parse->limitOffset))
		return;
	limit_count = parse->limitCount;
	limit_offset = parse->limitOffset;
	if (limit_count && ((Const *) limit_count)->constisnull)
		limit_count = NULL;
	if (limit_offset && ((Const *) limit_offset)->constisnull)
		limit_offset = NULL;
	if (limit_count == NULL && limit_offset == NULL)
		return;

	if (ifpinfo->is_grouped || !IS_UPPER_REL(input_rel))
	{
		/* base, join or grouped relation: no ORDER BY in the query */
		if (parse->sortClause != NIL)
			return;
		scanrel = input_rel;
	}
	else if (ifpinfo->outerrel != NULL && ifpinfo->pushdown_safe)
	{
		/* our ordered rel: the sort goes to Trino together with the limit */
		scanrel = ifpinfo->outerrel;
		pathkeys = ifpinfo->pathkeys;
	}
	else
		return;

	scan_fpinfo = (TrinoRelationInfo *) scanrel->fdw_private;
	if (!scan_fpinfo->pushdown_safe)
		return;
	/* LIMIT applies after filtering, so every qual must be remote */
	if (scan_fpinfo->local_conds != NIL || scan_fpinfo->having_local != NIL)
		return;

	fpinfo = palloc0(sizeof(TrinoRelationInfo));
	fpinfo->pushdown_safe = true;
	fpinfo->outerrel = scanrel;
	fpinfo->pathkeys = pathkeys;
	fpinfo->has_limit = true;
	fpinfo->limit_count = limit_count;
	fpinfo->limit_offset = limit_offset;
	fpinfo->opts = scan_fpinfo->opts;
	fpinfo->relid = scan_fpinfo->relid;
	fpinfo->userid = scan_fpinfo->userid;
	final_rel->fdw_private = fpinfo;

	/*
	 * Cost the remote LIMIT the way core costs a local Limit over the input
	 * path (a fraction of the input's run cost), then take a margin off.
	 * Core's Limit costing assumes the scan can stop early for free, which
	 * hides the real benefits of pushing LIMIT down: Trino can use TopN
	 * instead of a full sort, and we never fetch rows only to discard them
	 * or have to cancel a running query.
	 */
	rows = ifpinfo->rows;
	if (extra->count_est > 0 && extra->count_est < rows)
		rows = extra->count_est;
	fetched = Min(rows + Max(extra->offset_est, 0), Max(ifpinfo->rows, 1.0));
	startup_cost = ifpinfo->startup_cost;
	total_cost = startup_cost +
		(ifpinfo->total_cost - ifpinfo->startup_cost) *
		(fetched / Max(ifpinfo->rows, 1.0)) * 0.9;
	fpinfo->rows = rows;
	fpinfo->startup_cost = startup_cost;
	fpinfo->total_cost = total_cost;

	path = TRINO_CREATE_FOREIGN_UPPER_PATH(root, scanrel,
										   root->upper_targets[UPPERREL_FINAL],
										   rows, startup_cost, total_cost,
										   pathkeys, list_make1(fpinfo));
	add_path(final_rel, (Path *) path);
}

static void
trinoGetForeignUpperPaths(PlannerInfo *root, UpperRelationKind stage,
						  RelOptInfo *input_rel, RelOptInfo *output_rel,
						  void *extra)
{
	TrinoRelationInfo *ifpinfo = (TrinoRelationInfo *) input_rel->fdw_private;

	if (ifpinfo == NULL || !ifpinfo->pushdown_safe)
		return;
	if (output_rel->fdw_private != NULL)
		return;

	switch (stage)
	{
		case UPPERREL_GROUP_AGG:
			add_foreign_grouping_paths(root, input_rel, output_rel,
									   (GroupPathExtraData *) extra);
			break;
		case UPPERREL_ORDERED:
			add_foreign_ordered_paths(root, input_rel, output_rel);
			break;
		case UPPERREL_FINAL:
			add_foreign_final_paths(root, input_rel, output_rel,
									(FinalPathExtraData *) extra);
			break;
		default:
			break;
	}
}

/*
 * Build the list of expressions a join or grouped scan must fetch: what the
 * relation's target needs plus what the local quals need.
 */
static List *
build_scan_tlist(RelOptInfo *rel, List *local_exprs)
{
	TrinoRelationInfo *fpinfo = (TrinoRelationInfo *) rel->fdw_private;
	List	   *vars;

	if (fpinfo->is_grouped)
		return fpinfo->grouped_tlist;

	vars = pull_var_clause((Node *) rel->reltarget->exprs, PVC_RECURSE_PLACEHOLDERS);
	vars = list_concat(vars, pull_var_clause((Node *) local_exprs,
											 PVC_RECURSE_PLACEHOLDERS));
	return add_to_flat_tlist(NIL, vars);
}

static ForeignScan *
trinoGetForeignPlan(PlannerInfo *root, RelOptInfo *foreignrel, Oid foreigntableid,
					ForeignPath *best_path, List *tlist, List *scan_clauses,
					Plan *outer_plan)
{
	TrinoRelationInfo *fpinfo = (TrinoRelationInfo *) foreignrel->fdw_private;
	TrinoRelationInfo *extra = best_path->fdw_private ?
		(TrinoRelationInfo *) linitial(best_path->fdw_private) : NULL;
	List	   *pathkeys = extra ? extra->pathkeys : NIL;
	bool		has_limit = extra ? extra->has_limit : false;
	Node	   *limit_count = extra ? extra->limit_count : NULL;
	Node	   *limit_offset = extra ? extra->limit_offset : NULL;
	Index		scan_relid = 0;
	List	   *remote_conds = NIL;
	List	   *local_exprs = NIL;
	List	   *remote_exprs = NIL;
	List	   *fdw_scan_tlist = NIL;
	List	   *retrieved_attrs;
	List	   *params_list;
	List	   *param_offsets;
	List	   *fdw_private;
	StringInfoData sql;
	ListCell   *lc;

	if (IS_SIMPLE_REL(foreignrel))
	{
		scan_relid = foreignrel->relid;
		foreach(lc, scan_clauses)
		{
			RestrictInfo *ri = lfirst_node(RestrictInfo, lc);

			if (ri->pseudoconstant)
				continue;
			if (list_member_ptr(fpinfo->remote_conds, ri))
				remote_conds = lappend(remote_conds, ri);
			else if (list_member_ptr(fpinfo->local_conds, ri))
				local_exprs = lappend(local_exprs, ri->clause);
			else if (trino_is_foreign_expr(root, foreignrel, ri->clause))
				remote_conds = lappend(remote_conds, ri);	/* join clause */
			else
				local_exprs = lappend(local_exprs, ri->clause);
		}
		foreach(lc, remote_conds)
			remote_exprs = lappend(remote_exprs, ((RestrictInfo *) lfirst(lc))->clause);
	}
	else if (IS_JOIN_REL(foreignrel))
	{
		remote_conds = fpinfo->remote_conds;
		foreach(lc, fpinfo->local_conds)
			local_exprs = lappend(local_exprs, ((RestrictInfo *) lfirst(lc))->clause);
		fdw_scan_tlist = build_scan_tlist(foreignrel, local_exprs);
	}
	else
	{
		/* grouped relation: WHERE and HAVING are found via fpinfo */
		local_exprs = fpinfo->having_local;
		fdw_scan_tlist = build_scan_tlist(foreignrel, local_exprs);
	}
	if (has_limit && local_exprs != NIL)
		elog(ERROR, "trino_fdw: LIMIT pushed down with local quals");

	initStringInfo(&sql);
	trino_deparse_select(&sql, root, foreignrel, fdw_scan_tlist, remote_conds,
						 pathkeys, has_limit, limit_count, limit_offset,
						 &retrieved_attrs, &params_list, &param_offsets);

	fdw_private = list_make5(makeString(sql.data),
							 retrieved_attrs,
							 param_offsets,
							 makeInteger((int) fpinfo->relid),
							 makeString(fpinfo->relation_name ?
										fpinfo->relation_name : ""));

	return make_foreignscan(tlist, local_exprs, scan_relid, params_list,
							fdw_private, fdw_scan_tlist, remote_exprs,
							outer_plan);
}

/* ----------------------------------------------------------------
 * Executor: scan
 * ---------------------------------------------------------------- */

static void
setup_columns(TupleDesc tupdesc, List *retrieved_attrs, int *ncols,
			  TrinoColumn **cols)
{
	ListCell   *lc;
	int			i = 0;

	*ncols = list_length(retrieved_attrs);
	*cols = palloc0(sizeof(TrinoColumn) * Max(*ncols, 1));
	foreach(lc, retrieved_attrs)
	{
		int			attnum = lfirst_int(lc);
		Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);
		TrinoColumn *c = &(*cols)[i++];
		Oid			infunc;

		c->attnum = attnum;
		c->typoid = attr->atttypid;
		c->typmod = attr->atttypmod;
		getTypeInputInfo(attr->atttypid, &infunc, &c->typioparam);
		fmgr_info(infunc, &c->infunc);
	}
}

/*
 * Fill values/nulls (sized for the full tupdesc, pre-set to null) from a
 * Trino row.
 */
static void
convert_row(TjValue *row, TrinoColumn *cols, int ncols, Datum *values,
			bool *nulls, const char *context)
{
	int			i;

	if (row->type != TJ_ARRAY || tj_count(row) < ncols)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_SCHEMA_NOT_FOUND),
				 errmsg("trino_fdw: Trino returned %d column(s) but %d were expected",
						tj_count(row), ncols),
				 errcontext("Trino SQL: %s", context)));

	for (i = 0; i < ncols; i++)
	{
		TrinoColumn *c = &cols[i];
		bool		isnull;

		values[c->attnum - 1] = trino_value_to_datum(tj_nth(row, i), c->typoid,
													 c->typmod, &c->infunc,
													 c->typioparam, &isnull);
		nulls[c->attnum - 1] = isnull;
	}
}

/*
 * Render the SQL template, filling parameter slots with literal values (or
 * with $n placeholders for EXPLAIN when values is NULL).
 */
static char *
render_sql(const char *template, List *param_offsets, List *param_values)
{
	StringInfoData buf;
	ListCell   *lc;
	int			pos = 0;
	int			n = 0;

	initStringInfo(&buf);
	foreach(lc, param_offsets)
	{
		int			off = lfirst_int(lc);

		appendBinaryStringInfo(&buf, template + pos, off - pos);
		pos = off;
		if (param_values)
			appendStringInfoString(&buf, (const char *) list_nth(param_values, n));
		else
			appendStringInfo(&buf, "$%d", n + 1);
		n++;
	}
	appendStringInfoString(&buf, template + pos);
	return buf.data;
}

static void
trinoBeginForeignScan(ForeignScanState *node, int eflags)
{
	ForeignScan *fsplan = (ForeignScan *) node->ss.ps.plan;
	EState	   *estate = node->ss.ps.state;
	TrinoScanState *state;
	Oid			relid;
	Oid			userid;
	ListCell   *lc;

	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	state = palloc0(sizeof(TrinoScanState));
	node->fdw_state = state;

	relid = (Oid) intVal(list_nth(fsplan->fdw_private, TrinoScanPrivateRelid));
#if PG_VERSION_NUM >= 160000
	userid = OidIsValid(fsplan->checkAsUser) ? fsplan->checkAsUser : GetUserId();
#else
	if (fsplan->scan.scanrelid > 0)
	{
		RangeTblEntry *rte = exec_rt_fetch(fsplan->scan.scanrelid, estate);

		userid = rte->checkAsUser ? rte->checkAsUser : GetUserId();
	}
	else
		userid = GetUserId();
#endif
	state->opts = trino_get_options(relid, InvalidOid, userid);
	state->sql_template = strVal(list_nth(fsplan->fdw_private, TrinoScanPrivateSql));
	state->param_offsets = (List *) list_nth(fsplan->fdw_private,
											 TrinoScanPrivateParamOffsets);

	setup_columns(node->ss.ss_ScanTupleSlot->tts_tupleDescriptor,
				  (List *) list_nth(fsplan->fdw_private, TrinoScanPrivateRetrievedAttrs),
				  &state->ncols, &state->cols);

	state->param_exprs = ExecInitExprList(fsplan->fdw_exprs, (PlanState *) node);
	foreach(lc, fsplan->fdw_exprs)
		state->param_types = lappend_oid(state->param_types, exprType(lfirst(lc)));

	state->query_cxt = AllocSetContextCreate(estate->es_query_cxt,
											 "trino_fdw scan",
											 ALLOCSET_DEFAULT_SIZES);
}

static void
start_query(ForeignScanState *node, TrinoScanState *state)
{
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	List	   *values = NIL;
	ListCell   *lc;
	ListCell   *tlc;
	MemoryContext oldcxt;

	/* evaluate parameters into literals */
	forboth(lc, state->param_exprs, tlc, state->param_types)
	{
		ExprState  *es = (ExprState *) lfirst(lc);
		Oid			type = lfirst_oid(tlc);
		StringInfoData buf;
		Datum		d;
		bool		isnull;

		d = ExecEvalExpr(es, econtext, &isnull);
		initStringInfo(&buf);
		trino_deparse_datum(&buf, d, type, isnull);
		values = lappend(values, buf.data);
	}

	oldcxt = MemoryContextSwitchTo(state->query_cxt);
	state->sql = render_sql(state->sql_template, state->param_offsets, values);
	state->query = trino_query_start(state->opts, state->sql, state->query_cxt);
	MemoryContextSwitchTo(oldcxt);
}

static TupleTableSlot *
trinoIterateForeignScan(ForeignScanState *node)
{
	TrinoScanState *state = (TrinoScanState *) node->fdw_state;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	TjValue    *row;
	MemoryContext oldcxt;

	if (state->query == NULL)
		start_query(node, state);

	row = trino_query_fetch_row(state->query);
	if (row == NULL)
		return ExecClearTuple(slot);

	ExecClearTuple(slot);
	memset(slot->tts_isnull, true, sizeof(bool) * slot->tts_tupleDescriptor->natts);

	/* converted datums live in per-tuple memory, reset by ExecScan */
	oldcxt = MemoryContextSwitchTo(node->ss.ps.ps_ExprContext->ecxt_per_tuple_memory);
	convert_row(row, state->cols, state->ncols, slot->tts_values, slot->tts_isnull,
				state->sql);
	MemoryContextSwitchTo(oldcxt);

	return ExecStoreVirtualTuple(slot);
}

static void
close_query(TrinoScanState *state)
{
	if (state->query)
	{
		trino_query_close(state->query);
		state->query = NULL;
	}
	MemoryContextReset(state->query_cxt);
}

static void
trinoReScanForeignScan(ForeignScanState *node)
{
	TrinoScanState *state = (TrinoScanState *) node->fdw_state;

	if (state == NULL)
		return;
	/* the next IterateForeignScan re-evaluates params and restarts */
	close_query(state);
}

static void
trinoEndForeignScan(ForeignScanState *node)
{
	TrinoScanState *state = (TrinoScanState *) node->fdw_state;

	if (state == NULL)
		return;
	close_query(state);
}

static void
trinoExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
	ForeignScan *fsplan = (ForeignScan *) node->ss.ps.plan;
	char	   *sql = strVal(list_nth(fsplan->fdw_private, TrinoScanPrivateSql));
	List	   *offsets = (List *) list_nth(fsplan->fdw_private,
											TrinoScanPrivateParamOffsets);
	char	   *relations = strVal(list_nth(fsplan->fdw_private,
											TrinoScanPrivateRelations));

	if (relations[0] != '\0')
		ExplainPropertyText("Relations", relations, es);
	ExplainPropertyText("Trino SQL", render_sql(sql, offsets, NIL), es);
}

/* ----------------------------------------------------------------
 * Executor: INSERT
 * ---------------------------------------------------------------- */

static List *
insertable_attrs(Relation rel)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	List	   *attrs = NIL;
	int			i;

	for (i = 1; i <= tupdesc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i - 1);

		if (attr->attisdropped || attr->attgenerated)
			continue;
		attrs = lappend_int(attrs, i);
	}
	return attrs;
}

static List *
trinoPlanForeignModify(PlannerInfo *root, ModifyTable *plan,
					   Index resultRelation, int subplan_index)
{
	RangeTblEntry *rte = planner_rt_fetch(resultRelation, root);
	Relation	rel;
	TrinoOptions *opts;
	List	   *target_attrs;
	StringInfoData sql;
	List	   *result;

	if (plan->operation != CMD_INSERT)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
				 errmsg("trino_fdw: this %s cannot be sent to Trino as a whole",
						plan->operation == CMD_UPDATE ? "UPDATE" :
						plan->operation == CMD_DELETE ? "DELETE" : "statement"),
				 errdetail("UPDATE and DELETE on a Trino foreign table run entirely on Trino, "
						   "so the WHERE clause and any new values must be expressions "
						   "Trino can evaluate, the statement cannot join other tables, "
						   "and RETURNING and row-level triggers are not supported."),
				 errhint("Use trino_execute(server, sql) to run arbitrary DML on Trino.")));
	if (plan->onConflictAction != ONCONFLICT_NONE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("trino_fdw: INSERT ... ON CONFLICT is not supported")));

	rel = table_open(rte->relid, NoLock);
	opts = trino_get_options(rte->relid, InvalidOid, rel_check_user(rte));
	target_attrs = insertable_attrs(rel);

	initStringInfo(&sql);
	trino_deparse_insert_prefix(&sql, rel, opts, target_attrs);
	table_close(rel, NoLock);

	result = list_make2(makeString(sql.data), target_attrs);
	return result;
}

static TrinoModifyState *
create_modify_state(Relation rel, Oid userid, const char *sql_prefix,
					List *target_attrs)
{
	TrinoModifyState *ms = palloc0(sizeof(TrinoModifyState));

	ms->rel = rel;
	ms->opts = trino_get_options(RelationGetRelid(rel), InvalidOid, userid);
	ms->sql_prefix = pstrdup(sql_prefix);
	ms->target_attrs = target_attrs;
	initStringInfo(&ms->buf);
	return ms;
}

static Oid
modify_user(ResultRelInfo *rinfo, EState *estate)
{
#if PG_VERSION_NUM >= 160000
	return ExecGetResultRelCheckAsUser(rinfo, estate);
#else
	RangeTblEntry *rte = exec_rt_fetch(rinfo->ri_RangeTableIndex, estate);

	return rte->checkAsUser ? rte->checkAsUser : GetUserId();
#endif
}

static void
trinoBeginForeignModify(ModifyTableState *mtstate, ResultRelInfo *rinfo,
						List *fdw_private, int subplan_index, int eflags)
{
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	rinfo->ri_FdwState =
		create_modify_state(rinfo->ri_RelationDesc,
							modify_user(rinfo, mtstate->ps.state),
							strVal(list_nth(fdw_private, TrinoModifyPrivateSqlPrefix)),
							(List *) list_nth(fdw_private, TrinoModifyPrivateTargetAttrs));
}

static void
trinoBeginForeignInsert(ModifyTableState *mtstate, ResultRelInfo *rinfo)
{
	/* COPY FROM or tuple routing into a foreign partition */
	Relation	rel = rinfo->ri_RelationDesc;
	Oid			userid = modify_user(rinfo, mtstate->ps.state);
	TrinoOptions *opts = trino_get_options(RelationGetRelid(rel), InvalidOid, userid);
	List	   *target_attrs = insertable_attrs(rel);
	StringInfoData sql;

	initStringInfo(&sql);
	trino_deparse_insert_prefix(&sql, rel, opts, target_attrs);
	rinfo->ri_FdwState = create_modify_state(rel, userid, sql.data, target_attrs);
}

static TupleTableSlot **
trinoExecForeignBatchInsert(EState *estate, ResultRelInfo *rinfo,
							TupleTableSlot **slots, TupleTableSlot **planSlots,
							int *numSlots)
{
	TrinoModifyState *ms = (TrinoModifyState *) rinfo->ri_FdwState;
	TupleDesc	tupdesc = RelationGetDescr(ms->rel);
	int			i;

	if (*numSlots == 0)
		return slots;

	resetStringInfo(&ms->buf);
	appendStringInfoString(&ms->buf, ms->sql_prefix);
	for (i = 0; i < *numSlots; i++)
	{
		ListCell   *lc;
		bool		first = true;

		if (i > 0)
			appendStringInfoString(&ms->buf, ", ");
		appendStringInfoChar(&ms->buf, '(');
		foreach(lc, ms->target_attrs)
		{
			int			attnum = lfirst_int(lc);
			Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);
			bool		isnull;
			Datum		d;

			if (!first)
				appendStringInfoString(&ms->buf, ", ");
			first = false;
			d = slot_getattr(slots[i], attnum, &isnull);
			trino_deparse_datum(&ms->buf, d, attr->atttypid, isnull);
		}
		appendStringInfoChar(&ms->buf, ')');
	}

	(void) trino_execute_sql(ms->opts, ms->buf.data);
	return slots;
}

static TupleTableSlot *
trinoExecForeignInsert(EState *estate, ResultRelInfo *rinfo,
					   TupleTableSlot *slot, TupleTableSlot *planSlot)
{
	int			n = 1;

	trinoExecForeignBatchInsert(estate, rinfo, &slot, &planSlot, &n);
	return slot;
}

/*
 * UPDATE and DELETE always run as direct modifications (see
 * trinoPlanDirectModify); PlanForeignModify rejects anything else at plan
 * time.  The executor nevertheless requires these entry points to exist.
 */
static TupleTableSlot *
trinoExecForeignUpdate(EState *estate, ResultRelInfo *rinfo,
					   TupleTableSlot *slot, TupleTableSlot *planSlot)
{
	elog(ERROR, "trino_fdw: row-by-row UPDATE is not supported");
	return NULL;
}

static TupleTableSlot *
trinoExecForeignDelete(EState *estate, ResultRelInfo *rinfo,
					   TupleTableSlot *slot, TupleTableSlot *planSlot)
{
	elog(ERROR, "trino_fdw: row-by-row DELETE is not supported");
	return NULL;
}

static int
trinoGetForeignModifyBatchSize(ResultRelInfo *rinfo)
{
	TrinoModifyState *ms = (TrinoModifyState *) rinfo->ri_FdwState;

	/* RETURNING, WITH CHECK OPTION and row triggers need one row at a time */
	if (rinfo->ri_projectReturning != NULL ||
		rinfo->ri_WithCheckOptions != NIL ||
		(rinfo->ri_TrigDesc &&
		 (rinfo->ri_TrigDesc->trig_insert_after_row ||
		  rinfo->ri_TrigDesc->trig_insert_before_row)))
		return 1;
	if (ms == NULL)				/* EXPLAIN */
		return 1;
	return ms->opts->batch_size;
}

static void
trinoEndForeignModify(EState *estate, ResultRelInfo *rinfo)
{
	/* nothing to release: each batch is its own autocommitted Trino query */
}

static void
trinoEndForeignInsert(EState *estate, ResultRelInfo *rinfo)
{
}

static int
trinoIsForeignRelUpdatable(Relation rel)
{
	return (1 << CMD_INSERT) | (1 << CMD_UPDATE) | (1 << CMD_DELETE);
}

static void
trinoExplainForeignModify(ModifyTableState *mtstate, ResultRelInfo *rinfo,
						  List *fdw_private, int subplan_index, ExplainState *es)
{
	char	   *prefix = strVal(list_nth(fdw_private, TrinoModifyPrivateSqlPrefix));

	ExplainPropertyText("Trino SQL", psprintf("%s(...)", prefix), es);
}

/* ----------------------------------------------------------------
 * Executor: direct UPDATE / DELETE
 *
 * Trino has no row identifiers, so UPDATE and DELETE can only be executed
 * when the whole statement can be sent as one Trino statement.
 * ---------------------------------------------------------------- */

typedef struct TrinoDirectModifyState
{
	TrinoOptions *opts;
	char	   *sql_template;
	List	   *param_offsets;
	List	   *param_exprs;	/* ExprState list */
	List	   *param_types;	/* Oid list */
	bool		set_processed;
	bool		executed;
	int64		num_tuples;
} TrinoDirectModifyState;

/*
 * Assigning to a column of type numeric(10,2) or varchar(5) wraps the new
 * value in a length-coercion call to the same base type.  Trino applies the
 * column's precision itself, so the wrapper can go.
 */
static Expr *
strip_typmod_coercion(Expr *expr)
{
	for (;;)
	{
		if (IsA(expr, FuncExpr))
		{
			FuncExpr   *fe = (FuncExpr *) expr;

			if ((fe->funcformat == COERCE_IMPLICIT_CAST ||
				 fe->funcformat == COERCE_EXPLICIT_CAST) &&
				list_length(fe->args) >= 1 &&
				exprType(linitial(fe->args)) == fe->funcresulttype)
			{
				expr = (Expr *) linitial(fe->args);
				continue;
			}
		}
		if (IsA(expr, RelabelType) &&
			((RelabelType *) expr)->resulttype == exprType((Node *) ((RelabelType *) expr)->arg))
		{
			expr = ((RelabelType *) expr)->arg;
			continue;
		}
		return expr;
	}
}

static bool
trinoPlanDirectModify(PlannerInfo *root, ModifyTable *plan, Index resultRelation,
					  int subplan_index)
{
	CmdType		operation = plan->operation;
	Plan	   *subplan;
	ForeignScan *fscan;
	RelOptInfo *foreignrel;
	RangeTblEntry *rte;
	Relation	rel;
	List	   *target_attrs = NIL;
	List	   *target_exprs = NIL;
	List	   *params_list;
	List	   *param_offsets;
	StringInfoData sql;

	if (operation != CMD_UPDATE && operation != CMD_DELETE)
		return false;
	if (plan->returningLists != NIL || plan->withCheckOptionLists != NIL)
		return false;

	/* the scan feeding the ModifyTable must be a plain scan of the target */
	subplan = outerPlan(plan);
	if (subplan == NULL || !IsA(subplan, ForeignScan))
		return false;
	fscan = (ForeignScan *) subplan;
	if (fscan->scan.scanrelid != resultRelation)
		return false;
	if (fscan->scan.plan.qual != NIL)
		return false;			/* local quals: rows must be fetched */

	foreignrel = find_base_rel(root, resultRelation);
	rte = planner_rt_fetch(resultRelation, root);
	rel = table_open(rte->relid, NoLock);

	/* row-level triggers need the rows */
	if (rel->trigdesc &&
		((operation == CMD_UPDATE &&
		  (rel->trigdesc->trig_update_before_row ||
		   rel->trigdesc->trig_update_after_row)) ||
		 (operation == CMD_DELETE &&
		  (rel->trigdesc->trig_delete_before_row ||
		   rel->trigdesc->trig_delete_after_row))))
	{
		table_close(rel, NoLock);
		return false;
	}

	if (operation == CMD_UPDATE)
	{
		ListCell   *lc;
		ListCell   *lc_col = list_head(root->update_colnos);
		TupleDesc	tupdesc = RelationGetDescr(rel);

		/* non-junk processed_tlist entries line up with update_colnos */
		foreach(lc, root->processed_tlist)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);
			AttrNumber	attnum;
			Expr	   *expr;

			if (tle->resjunk)
				continue;
			if (lc_col == NULL)
				elog(ERROR, "too few entries in update_colnos");
			attnum = lfirst_int(lc_col);
			lc_col = lnext(root->update_colnos, lc_col);

			expr = strip_typmod_coercion(tle->expr);
			if (attnum <= 0 || TupleDescAttr(tupdesc, attnum - 1)->attgenerated ||
				!trino_is_foreign_expr(root, foreignrel, expr))
			{
				table_close(rel, NoLock);
				return false;
			}
			target_attrs = lappend_int(target_attrs, attnum);
			target_exprs = lappend(target_exprs, expr);
		}
	}

	initStringInfo(&sql);
	trino_deparse_direct_modify(&sql, root, foreignrel, operation, target_attrs,
								target_exprs, fscan->fdw_recheck_quals,
								&params_list, &param_offsets);
	table_close(rel, NoLock);

	/* turn the scan into a direct modification */
	fscan->operation = operation;
	fscan->resultRelation = resultRelation;
	fscan->fdw_exprs = params_list;
	fscan->fdw_private = list_make4(makeString(sql.data),
									param_offsets,
									makeInteger((int) rte->relid),
									makeInteger(plan->canSetTag ? 1 : 0));
	fscan->fdw_scan_tlist = NIL;
	fscan->fdw_recheck_quals = NIL;
	return true;
}

static void
trinoBeginDirectModify(ForeignScanState *node, int eflags)
{
	ForeignScan *fsplan = (ForeignScan *) node->ss.ps.plan;
	EState	   *estate = node->ss.ps.state;
	TrinoDirectModifyState *dm;
	Oid			relid;
	Oid			userid;
	ListCell   *lc;

	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	dm = palloc0(sizeof(TrinoDirectModifyState));
	node->fdw_state = dm;

	relid = (Oid) intVal(list_nth(fsplan->fdw_private, TrinoDirectPrivateRelid));
#if PG_VERSION_NUM >= 160000
	userid = OidIsValid(fsplan->checkAsUser) ? fsplan->checkAsUser : GetUserId();
#else
	{
		RangeTblEntry *rte = exec_rt_fetch(fsplan->resultRelation, estate);

		userid = rte->checkAsUser ? rte->checkAsUser : GetUserId();
	}
#endif
	dm->opts = trino_get_options(relid, InvalidOid, userid);
	dm->sql_template = strVal(list_nth(fsplan->fdw_private, TrinoDirectPrivateSql));
	dm->param_offsets = (List *) list_nth(fsplan->fdw_private,
										  TrinoDirectPrivateParamOffsets);
	dm->set_processed = intVal(list_nth(fsplan->fdw_private,
										TrinoDirectPrivateSetProcessed)) != 0;
	dm->param_exprs = ExecInitExprList(fsplan->fdw_exprs, (PlanState *) node);
	foreach(lc, fsplan->fdw_exprs)
		dm->param_types = lappend_oid(dm->param_types, exprType(lfirst(lc)));
	dm->num_tuples = -1;
	(void) estate;
}

static TupleTableSlot *
trinoIterateDirectModify(ForeignScanState *node)
{
	TrinoDirectModifyState *dm = (TrinoDirectModifyState *) node->fdw_state;
	EState	   *estate = node->ss.ps.state;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;

	if (!dm->executed)
	{
		ExprContext *econtext = node->ss.ps.ps_ExprContext;
		List	   *values = NIL;
		ListCell   *lc;
		ListCell   *tlc;
		char	   *sql;
		int64		count;

		forboth(lc, dm->param_exprs, tlc, dm->param_types)
		{
			ExprState  *es = (ExprState *) lfirst(lc);
			StringInfoData buf;
			bool		isnull;
			Datum		d;

			d = ExecEvalExpr(es, econtext, &isnull);
			initStringInfo(&buf);
			trino_deparse_datum(&buf, d, lfirst_oid(tlc), isnull);
			values = lappend(values, buf.data);
		}
		sql = render_sql(dm->sql_template, dm->param_offsets, values);
		count = trino_execute_sql(dm->opts, sql);
		/* a connector that does not report a count gives -1 */
		dm->num_tuples = count < 0 ? 0 : count;
		dm->executed = true;

		if (dm->set_processed)
			estate->es_processed += dm->num_tuples;
		if (node->ss.ps.instrument)
			node->ss.ps.instrument->tuplecount += dm->num_tuples;
	}
	return ExecClearTuple(slot);
}

static void
trinoEndDirectModify(ForeignScanState *node)
{
	/* nothing to release: the statement was autocommitted by Trino */
}

static void
trinoExplainDirectModify(ForeignScanState *node, ExplainState *es)
{
	ForeignScan *fsplan = (ForeignScan *) node->ss.ps.plan;
	char	   *sql = strVal(list_nth(fsplan->fdw_private, TrinoDirectPrivateSql));
	List	   *offsets = (List *) list_nth(fsplan->fdw_private,
											TrinoDirectPrivateParamOffsets);

	ExplainPropertyText("Trino SQL", render_sql(sql, offsets, NIL), es);
}

/* ----------------------------------------------------------------
 * ANALYZE
 * ---------------------------------------------------------------- */

/* count obtained by AnalyzeForeignTable, reused by the sampling function */
static Oid	analyze_relid = InvalidOid;
static double analyze_count = 0;

static double
remote_row_count(Relation rel, TrinoOptions *opts)
{
	StringInfoData sql;
	MemoryContext mcxt;
	TrinoQuery *q;
	TjValue    *row;
	double		count = 0;

	initStringInfo(&sql);
	trino_deparse_analyze_count(&sql, rel, opts);

	mcxt = AllocSetContextCreate(CurrentMemoryContext, "trino_fdw analyze count",
								 ALLOCSET_DEFAULT_SIZES);
	q = trino_query_start(opts, sql.data, mcxt);
	row = trino_query_fetch_row(q);
	if (row && tj_nth(row, 0) && tj_nth(row, 0)->type == TJ_NUMBER)
		count = strtod(tj_nth(row, 0)->str, NULL);
	while (trino_query_fetch_row(q) != NULL)
		;
	trino_query_close(q);
	MemoryContextDelete(mcxt);
	return count;
}

static int
trino_acquire_sample_rows(Relation relation, int elevel, HeapTuple *rows,
						  int targrows, double *totalrows, double *totaldeadrows)
{
	TrinoOptions *opts = trino_get_options(RelationGetRelid(relation), InvalidOid,
										   GetUserId());
	TupleDesc	tupdesc = RelationGetDescr(relation);
	double		count;
	double		percent;
	StringInfoData sql;
	List	   *retrieved_attrs;
	TrinoColumn *cols;
	int			ncols;
	MemoryContext mcxt;
	MemoryContext oldcxt;
	TrinoQuery *q;
	TjValue    *row;
	Datum	   *values;
	bool	   *nulls;
	int			numrows = 0;
	double		rowstoskip = -1;
	double		rstate_seen = 0;
	ReservoirStateData rstate;

	if (analyze_relid == RelationGetRelid(relation))
		count = analyze_count;
	else
		count = remote_row_count(relation, opts);
	analyze_relid = InvalidOid;

	*totalrows = count;
	*totaldeadrows = 0;
	if (count <= 0)
		return 0;

	/* oversample a little so we usually reach targrows after sampling */
	percent = (targrows * 1.25 / count) * 100.0;
	if (percent > 100.0)
		percent = 100.0;

	initStringInfo(&sql);
	trino_deparse_analyze_sample(&sql, relation, opts, percent, &retrieved_attrs);
	setup_columns(tupdesc, retrieved_attrs, &ncols, &cols);
	values = palloc(sizeof(Datum) * tupdesc->natts);
	nulls = palloc(sizeof(bool) * tupdesc->natts);

	reservoir_init_selection_state(&rstate, targrows);

	mcxt = AllocSetContextCreate(CurrentMemoryContext, "trino_fdw analyze",
								 ALLOCSET_DEFAULT_SIZES);
	q = trino_query_start(opts, sql.data, mcxt);
	while ((row = trino_query_fetch_row(q)) != NULL)
	{
		HeapTuple	tuple;
		int			pos;

		CHECK_FOR_INTERRUPTS();
		rstate_seen += 1;

		if (numrows < targrows)
			pos = numrows++;
		else
		{
			/* Vitter's reservoir sampling */
			if (rowstoskip < 0)
				rowstoskip = reservoir_get_next_S(&rstate, rstate_seen, targrows);
			if (rowstoskip <= 0)
			{
#if PG_VERSION_NUM >= 150000
				pos = (int) (targrows * sampler_random_fract(&rstate.randstate));
#else
				pos = (int) (targrows * sampler_random_fract(rstate.randstate));
#endif
				heap_freetuple(rows[pos]);
			}
			else
				pos = -1;
			rowstoskip -= 1;
		}

		if (pos >= 0)
		{
			MemoryContext tmp = AllocSetContextCreate(CurrentMemoryContext,
													  "trino_fdw analyze row",
													  ALLOCSET_SMALL_SIZES);

			oldcxt = MemoryContextSwitchTo(tmp);
			memset(nulls, true, sizeof(bool) * tupdesc->natts);
			convert_row(row, cols, ncols, values, nulls, sql.data);
			MemoryContextSwitchTo(oldcxt);
			tuple = heap_form_tuple(tupdesc, values, nulls);
			rows[pos] = tuple;
			MemoryContextDelete(tmp);
		}
	}
	trino_query_close(q);
	MemoryContextDelete(mcxt);

	ereport(elevel,
			(errmsg("\"%s\": table contains %.0f rows; %d rows in sample",
					RelationGetRelationName(relation), count, numrows)));
	return numrows;
}

static bool
trinoAnalyzeForeignTable(Relation relation, AcquireSampleRowsFunc *func,
						 BlockNumber *totalpages)
{
	TrinoOptions *opts = trino_get_options(RelationGetRelid(relation), InvalidOid,
										   GetUserId());
	double		count = remote_row_count(relation, opts);
	int			width = 0;
	TupleDesc	tupdesc = RelationGetDescr(relation);
	int			i;

	analyze_relid = RelationGetRelid(relation);
	analyze_count = count;

	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

		if (!attr->attisdropped)
			width += get_typavgwidth(attr->atttypid, attr->atttypmod);
	}
	*totalpages = (BlockNumber) ceil(count * (width + MAXALIGN(SizeofHeapTupleHeader)) /
									 BLCKSZ);
	if (*totalpages < 1)
		*totalpages = 1;
	*func = trino_acquire_sample_rows;
	return true;
}

/* ----------------------------------------------------------------
 * IMPORT FOREIGN SCHEMA
 * ---------------------------------------------------------------- */

static List *
trinoImportForeignSchema(ImportForeignSchemaStmt *stmt, Oid serverOid)
{
	ForeignServer *server = GetForeignServer(serverOid);
	TrinoOptions *opts = trino_get_server_options(serverOid, GetUserId());
	char	   *catalog = opts->catalog;
	bool		catalog_from_import = false;
	ListCell   *lc;
	StringInfoData sql;
	MemoryContext mcxt;
	TrinoQuery *q;
	TjValue    *row;
	List	   *commands = NIL;
	StringInfoData cmd;
	char	   *cur_table = NULL;
	int			ncols = 0;

	foreach(lc, stmt->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "catalog") == 0)
		{
			catalog = defGetString(def);
			catalog_from_import = true;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("invalid option \"%s\"", def->defname),
					 errhint("Valid options in this context are: catalog")));
	}
	if (catalog == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_OPTION_NAME_NOT_FOUND),
				 errmsg("trino_fdw: no Trino catalog specified"),
				 errhint("Set the \"catalog\" option on the server, or pass "
						 "OPTIONS (catalog 'name') to IMPORT FOREIGN SCHEMA.")));

	initStringInfo(&sql);
	appendStringInfoString(&sql,
						   "SELECT table_name, column_name, data_type FROM ");
	trino_quote_identifier(&sql, catalog);
	appendStringInfoString(&sql, ".information_schema.columns WHERE table_schema = ");
	trino_quote_literal(&sql, stmt->remote_schema);
	if (stmt->list_type == FDW_IMPORT_SCHEMA_LIMIT_TO)
	{
		bool		first = true;

		appendStringInfoString(&sql, " AND table_name IN (");
		foreach(lc, stmt->table_list)
		{
			RangeVar   *rv = (RangeVar *) lfirst(lc);

			if (!first)
				appendStringInfoString(&sql, ", ");
			first = false;
			trino_quote_literal(&sql, rv->relname);
		}
		appendStringInfoChar(&sql, ')');
	}
	appendStringInfoString(&sql, " ORDER BY table_name, ordinal_position");

	mcxt = AllocSetContextCreate(CurrentMemoryContext, "trino_fdw import",
								 ALLOCSET_DEFAULT_SIZES);
	q = trino_query_start(opts, sql.data, mcxt);
	initStringInfo(&cmd);

	while ((row = trino_query_fetch_row(q)) != NULL)
	{
		const char *table;
		const char *column;
		const char *type;
		bool		skip = false;

		if (tj_count(row) < 3 || tj_nth(row, 0)->type != TJ_STRING ||
			tj_nth(row, 1)->type != TJ_STRING || tj_nth(row, 2)->type != TJ_STRING)
			continue;
		table = tj_nth(row, 0)->str;
		column = tj_nth(row, 1)->str;
		type = tj_nth(row, 2)->str;

		if (stmt->list_type == FDW_IMPORT_SCHEMA_EXCEPT)
		{
			foreach(lc, stmt->table_list)
			{
				RangeVar   *rv = (RangeVar *) lfirst(lc);

				if (strcmp(rv->relname, table) == 0)
					skip = true;
			}
		}
		if (skip)
			continue;

		if (cur_table == NULL || strcmp(cur_table, table) != 0)
		{
			/* finish the previous table */
			if (cur_table != NULL)
			{
				appendStringInfo(&cmd, "\n) SERVER %s OPTIONS (schema_name %s, table_name %s%s%s)",
								 quote_identifier(server->servername),
								 quote_literal_cstr(stmt->remote_schema),
								 quote_literal_cstr(cur_table),
								 catalog_from_import ? ", catalog " : "",
								 catalog_from_import ? quote_literal_cstr(catalog) : "");
				commands = lappend(commands, pstrdup(cmd.data));
			}
			resetStringInfo(&cmd);
			cur_table = pstrdup(table);
			ncols = 0;
			appendStringInfo(&cmd, "CREATE FOREIGN TABLE %s.%s (",
							 quote_identifier(stmt->local_schema),
							 quote_identifier(table));
		}

		appendStringInfo(&cmd, "%s\n  %s %s", ncols > 0 ? "," : "",
						 quote_identifier(column), trino_type_to_pg(type));
		ncols++;
	}
	if (cur_table != NULL)
	{
		appendStringInfo(&cmd, "\n) SERVER %s OPTIONS (schema_name %s, table_name %s%s%s)",
						 quote_identifier(server->servername),
						 quote_literal_cstr(stmt->remote_schema),
						 quote_literal_cstr(cur_table),
						 catalog_from_import ? ", catalog " : "",
						 catalog_from_import ? quote_literal_cstr(catalog) : "");
		commands = lappend(commands, pstrdup(cmd.data));
	}

	trino_query_close(q);
	MemoryContextDelete(mcxt);
	return commands;
}

/* ----------------------------------------------------------------
 * SQL-callable passthrough: trino_query() and trino_execute()
 * ---------------------------------------------------------------- */

static TrinoOptions *
server_options_by_name(const char *servername)
{
	ForeignServer *server = GetForeignServerByName(servername, false);
	AclResult	aclresult;

#if PG_VERSION_NUM >= 160000
	aclresult = object_aclcheck(ForeignServerRelationId, server->serverid,
								GetUserId(), ACL_USAGE);
#else
	aclresult = pg_foreign_server_aclcheck(server->serverid, GetUserId(), ACL_USAGE);
#endif
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FOREIGN_SERVER, server->servername);

	return trino_get_server_options(server->serverid, GetUserId());
}

/*
 * trino_query(server name, sql text) RETURNS SETOF record
 *
 * Runs arbitrary SQL on Trino; the caller supplies a column definition list.
 */
Datum
trino_query(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *servername = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *sql = text_to_cstring(PG_GETARG_TEXT_PP(1));
	TrinoOptions *opts;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcxt;
	MemoryContext mcxt;
	TrinoQuery *q;
	TjValue    *row;
	TrinoColumn *cols;
	int			ncols;
	List	   *attrs = NIL;
	Datum	   *values;
	bool	   *nulls;
	int			i;

	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo) ||
		(rsinfo->allowedModes & SFRM_Materialize) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (rsinfo->expectedDesc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("a column definition list is required for trino_query()"),
				 errhint("Example: SELECT * FROM trino_query('srv', 'SELECT 1') AS t(x integer)")));

	opts = server_options_by_name(servername);

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcxt = MemoryContextSwitchTo(per_query_ctx);
	tupdesc = CreateTupleDescCopy(rsinfo->expectedDesc);
	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;
	MemoryContextSwitchTo(oldcxt);

	for (i = 1; i <= tupdesc->natts; i++)
		attrs = lappend_int(attrs, i);
	setup_columns(tupdesc, attrs, &ncols, &cols);
	values = palloc(sizeof(Datum) * ncols);
	nulls = palloc(sizeof(bool) * ncols);

	mcxt = AllocSetContextCreate(CurrentMemoryContext, "trino_fdw trino_query",
								 ALLOCSET_DEFAULT_SIZES);
	q = trino_query_start(opts, sql, mcxt);
	while ((row = trino_query_fetch_row(q)) != NULL)
	{
		MemoryContext tmp = AllocSetContextCreate(CurrentMemoryContext,
												  "trino_fdw row",
												  ALLOCSET_SMALL_SIZES);

		if (tj_count(row) != ncols)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("trino_fdw: Trino returned %d column(s) but the column definition list has %d",
							tj_count(row), ncols)));
		oldcxt = MemoryContextSwitchTo(tmp);
		memset(nulls, true, sizeof(bool) * ncols);
		convert_row(row, cols, ncols, values, nulls, sql);
		MemoryContextSwitchTo(oldcxt);
		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
		MemoryContextDelete(tmp);
	}
	trino_query_close(q);
	MemoryContextDelete(mcxt);

	PG_RETURN_NULL();
}

/*
 * trino_execute(server name, sql text) RETURNS bigint
 *
 * Runs a statement on Trino and returns its update count (-1 if none).
 */
Datum
trino_execute(PG_FUNCTION_ARGS)
{
	char	   *servername = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *sql = text_to_cstring(PG_GETARG_TEXT_PP(1));
	TrinoOptions *opts = server_options_by_name(servername);

	PG_RETURN_INT64(trino_execute_sql(opts, sql));
}
