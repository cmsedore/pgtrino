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
#include "optimizer/restrictinfo.h"
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
	TrinoScanPrivateRelid		/* foreign table OID (Integer) */
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
	routine->BeginForeignScan = trinoBeginForeignScan;
	routine->IterateForeignScan = trinoIterateForeignScan;
	routine->ReScanForeignScan = trinoReScanForeignScan;
	routine->EndForeignScan = trinoEndForeignScan;
	routine->ExplainForeignScan = trinoExplainForeignScan;

	routine->PlanForeignModify = trinoPlanForeignModify;
	routine->BeginForeignModify = trinoBeginForeignModify;
	routine->ExecForeignInsert = trinoExecForeignInsert;
	routine->ExecForeignBatchInsert = trinoExecForeignBatchInsert;
	routine->GetForeignModifyBatchSize = trinoGetForeignModifyBatchSize;
	routine->EndForeignModify = trinoEndForeignModify;
	routine->BeginForeignInsert = trinoBeginForeignInsert;
	routine->EndForeignInsert = trinoEndForeignInsert;
	routine->IsForeignRelUpdatable = trinoIsForeignRelUpdatable;
	routine->ExplainForeignModify = trinoExplainForeignModify;

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
				 strcmp(def->defname, "ssl_verify") == 0)
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
	opts->connect_timeout = 10;
	opts->request_timeout = 0;
	opts->startup_cost = 1000.0;
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
#define TRINO_CREATE_FOREIGN_UPPER_PATH(root, rel, target, rows, startup, total, pathkeys, priv) \
	create_foreign_upper_path(root, rel, target, rows, startup, total, pathkeys, NULL, NIL, priv)
#else
#define TRINO_CREATE_FOREIGNSCAN_PATH(root, rel, rows, startup, total, pathkeys, outer, priv) \
	create_foreignscan_path(root, rel, NULL, rows, startup, total, pathkeys, outer, NULL, priv)
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
pathkeys_pushable(PlannerInfo *root, RelOptInfo *baserel, List *pathkeys)
{
	ListCell   *lc;

	if (pathkeys == NIL)
		return false;
	foreach(lc, pathkeys)
	{
		if (!trino_is_sortable_pathkey(root, baserel, (PathKey *) lfirst(lc)))
			return false;
	}
	return true;
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

	/*
	 * If the query wants its output sorted in a way Trino can produce, offer
	 * a pre-sorted path too.  The planner compares it against a local Sort.
	 */
	if (pathkeys_pushable(root, baserel, root->query_pathkeys))
	{
		TrinoRelationInfo *sorted = palloc0(sizeof(TrinoRelationInfo));

		*sorted = *fpinfo;
		sorted->pathkeys = root->query_pathkeys;
		sorted->total_cost = fpinfo->total_cost + remote_sort_cost(fpinfo->rows);

		path = TRINO_CREATE_FOREIGNSCAN_PATH(root, baserel, fpinfo->rows,
											 sorted->startup_cost,
											 sorted->total_cost,
											 root->query_pathkeys,
											 baserel->lateral_relids,
											 list_make1(sorted));
		add_path(baserel, (Path *) path);
	}
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
 * UPPERREL_ORDERED: the pre-sorted base path already covers ORDER BY, so we
 * add no path here.  We only record on the ordered rel which base rel it
 * sorts and whether the sort is pushable, for the FINAL stage below.
 */
static void
add_foreign_ordered_paths(PlannerInfo *root, RelOptInfo *input_rel,
						  RelOptInfo *ordered_rel)
{
	TrinoRelationInfo *ifpinfo = (TrinoRelationInfo *) input_rel->fdw_private;
	TrinoRelationInfo *fpinfo;

	if (input_rel->reloptkind != RELOPT_BASEREL)
		return;

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
}

/*
 * UPPERREL_FINAL: push LIMIT/OFFSET (and the ORDER BY beneath it) down.
 * The new path's parent is the base rel, so GetForeignPlan sees an ordinary
 * scan plus the extra clauses carried in the path's fdw_private.
 */
static void
add_foreign_final_paths(PlannerInfo *root, RelOptInfo *input_rel,
						RelOptInfo *final_rel, FinalPathExtraData *extra)
{
	Query	   *parse = root->parse;
	TrinoRelationInfo *ifpinfo = (TrinoRelationInfo *) input_rel->fdw_private;
	TrinoRelationInfo *base_fpinfo;
	TrinoRelationInfo *fpinfo;
	RelOptInfo *baserel;
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

	if (input_rel->reloptkind == RELOPT_BASEREL)
	{
		if (parse->sortClause != NIL)
			return;
		baserel = input_rel;
	}
	else if (input_rel->reloptkind == RELOPT_UPPER_REL &&
			 ifpinfo->outerrel != NULL && ifpinfo->pushdown_safe)
	{
		/* our ordered rel: the sort goes to Trino together with the limit */
		baserel = ifpinfo->outerrel;
		pathkeys = ifpinfo->pathkeys;
	}
	else
		return;

	base_fpinfo = (TrinoRelationInfo *) baserel->fdw_private;
	/* LIMIT applies after filtering, so every qual must be remote */
	if (base_fpinfo->local_conds != NIL)
		return;

	fpinfo = palloc0(sizeof(TrinoRelationInfo));
	fpinfo->pushdown_safe = true;
	fpinfo->outerrel = baserel;
	fpinfo->pathkeys = pathkeys;
	fpinfo->has_limit = true;
	fpinfo->limit_count = limit_count;
	fpinfo->limit_offset = limit_offset;
	fpinfo->opts = base_fpinfo->opts;
	fpinfo->relid = base_fpinfo->relid;
	fpinfo->userid = base_fpinfo->userid;
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

	path = TRINO_CREATE_FOREIGN_UPPER_PATH(root, baserel,
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

	if (ifpinfo == NULL)
		return;
	if (output_rel->fdw_private != NULL)
		return;

	switch (stage)
	{
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
	List	   *remote_conds = NIL;
	List	   *local_exprs = NIL;
	List	   *remote_exprs = NIL;
	List	   *retrieved_attrs;
	List	   *params_list;
	List	   *param_offsets;
	List	   *fdw_private;
	StringInfoData sql;
	ListCell   *lc;

	/* every path we create has the base relation as its parent */
	if (!IS_SIMPLE_REL(foreignrel))
		elog(ERROR, "trino_fdw: unexpected non-base relation in GetForeignPlan");

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
			remote_conds = lappend(remote_conds, ri);
		else
			local_exprs = lappend(local_exprs, ri->clause);
	}
	if (has_limit && local_exprs != NIL)
		elog(ERROR, "trino_fdw: LIMIT pushed down with local quals");

	foreach(lc, remote_conds)
		remote_exprs = lappend(remote_exprs, ((RestrictInfo *) lfirst(lc))->clause);

	initStringInfo(&sql);
	trino_deparse_select(&sql, root, foreignrel, NIL, remote_conds,
						 pathkeys, has_limit, limit_count, limit_offset,
						 &retrieved_attrs, &params_list, &param_offsets);

	fdw_private = list_make4(makeString(sql.data),
							 retrieved_attrs,
							 param_offsets,
							 makeInteger((int) fpinfo->relid));

	return make_foreignscan(tlist, local_exprs, foreignrel->relid, params_list,
							fdw_private, NIL, remote_exprs, outer_plan);
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
				 errmsg("trino_fdw: %s on a Trino foreign table is not supported",
						plan->operation == CMD_UPDATE ? "UPDATE" :
						plan->operation == CMD_DELETE ? "DELETE" : "this operation"),
				 errhint("Use trino_execute(server, sql) to run DML directly on Trino.")));
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
	return (1 << CMD_INSERT);
}

static void
trinoExplainForeignModify(ModifyTableState *mtstate, ResultRelInfo *rinfo,
						  List *fdw_private, int subplan_index, ExplainState *es)
{
	char	   *prefix = strVal(list_nth(fdw_private, TrinoModifyPrivateSqlPrefix));

	ExplainPropertyText("Trino SQL", psprintf("%s(...)", prefix), es);
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
				pos = (int) (targrows * sampler_random_fract(&rstate.randstate));
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
