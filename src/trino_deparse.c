/*-------------------------------------------------------------------------
 *
 * trino_deparse.c
 *	  Turn PostgreSQL plan trees into Trino SQL.
 *
 * Only expressions whose semantics are identical in both systems are shipped.
 * Everything else is evaluated locally, so pushdown is a pure optimisation:
 * the FDW never produces different results because of it.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "commands/defrem.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"

#include "trino_fdw.h"

typedef struct deparse_cxt
{
	PlannerInfo *root;
	Relids		relids;			/* base rels whose columns are local to the
								 * statement being built */
	bool		qualify;		/* prefix columns with r<varno>. (joins) */
	StringInfo	buf;
	List	  **params_list;	/* Param/outer-Var exprs, in order */
	List	  **param_offsets;	/* byte offsets in buf for each */
} deparse_cxt;

typedef struct walker_cxt
{
	PlannerInfo *root;
	Relids		relids;
	bool		allow_aggs;		/* grouped relation: Aggrefs are fine */
} walker_cxt;

static bool foreign_expr_walker(Node *node, walker_cxt *cxt);
static void deparse_expr(Node *node, deparse_cxt *cxt);
static bool try_deparse_datum(StringInfo buf, Datum value, Oid type);
static void deparse_from(deparse_cxt *cxt, RelOptInfo *rel);

/*
 * The base relations whose columns an expression may reference when it is
 * evaluated as part of a scan of rel.  For a grouped relation that is the
 * input relation being aggregated.
 */
static Relids
rel_relids(RelOptInfo *rel)
{
	if (IS_UPPER_REL(rel))
	{
		TrinoRelationInfo *fpinfo = (TrinoRelationInfo *) rel->fdw_private;

		return rel_relids(fpinfo->outerrel);
	}
	return rel->relids;
}

/* ----------------------------------------------------------------
 * Type and operator classification
 * ---------------------------------------------------------------- */

/* Types we know how to write as Trino literals and compare safely */
bool
trino_type_is_deparsable(Oid type)
{
	switch (type)
	{
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case FLOAT4OID:
		case FLOAT8OID:
		case NUMERICOID:
		case TEXTOID:
		case VARCHAROID:
		case BPCHAROID:
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case TIMEOID:
		case UUIDOID:
		case BYTEAOID:
			return true;
		default:
			return false;
	}
}

static bool
type_is_numeric(Oid type)
{
	switch (type)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case FLOAT4OID:
		case FLOAT8OID:
		case NUMERICOID:
			return true;
		default:
			return false;
	}
}

static bool
type_is_string(Oid type)
{
	return type == TEXTOID || type == VARCHAROID || type == BPCHAROID;
}

/*
 * Casts that are exact in both systems and that Trino applies implicitly, so
 * the cast node can simply be dropped when deparsing.
 */
static bool
is_exact_widening_cast(Oid from, Oid to)
{
	switch (from)
	{
		case INT2OID:
			return to == INT4OID || to == INT8OID || to == NUMERICOID;
		case INT4OID:
			return to == INT8OID || to == NUMERICOID;
		case INT8OID:
			return to == NUMERICOID;
		case FLOAT4OID:
			return to == FLOAT8OID;
		default:
			return false;
	}
}

/* Types whose ORDER BY semantics match between PostgreSQL and Trino */
static bool
type_is_sortable(Oid type)
{
	switch (type)
	{
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case FLOAT4OID:
		case FLOAT8OID:
		case NUMERICOID:
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case TIMEOID:
			return true;
		default:
			return false;
	}
}

/* Return the operator's name if it lives in pg_catalog, else NULL */
static char *
pg_catalog_opname(Oid opno)
{
	HeapTuple	tuple;
	Form_pg_operator form;
	char	   *name = NULL;

	tuple = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for operator %u", opno);
	form = (Form_pg_operator) GETSTRUCT(tuple);
	if (form->oprnamespace == PG_CATALOG_NAMESPACE)
		name = pstrdup(NameStr(form->oprname));
	ReleaseSysCache(tuple);
	return name;
}

/* Return the function's name if it lives in pg_catalog, else NULL */
static char *
pg_catalog_funcname(Oid funcid)
{
	HeapTuple	tuple;
	Form_pg_proc form;
	char	   *name = NULL;

	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcid);
	form = (Form_pg_proc) GETSTRUCT(tuple);
	if (form->pronamespace == PG_CATALOG_NAMESPACE)
		name = pstrdup(NameStr(form->proname));
	ReleaseSysCache(tuple);
	return name;
}

typedef enum OpKind
{
	OP_NONE,
	OP_EQUALITY,				/* = <> */
	OP_COMPARE,					/* < <= > >= */
	OP_ARITH,					/* + - * / % */
	OP_CONCAT,					/* || */
	OP_LIKE,					/* ~~ !~~ */
} OpKind;

static OpKind
classify_operator(const char *name)
{
	if (strcmp(name, "=") == 0 || strcmp(name, "<>") == 0)
		return OP_EQUALITY;
	if (strcmp(name, "<") == 0 || strcmp(name, "<=") == 0 ||
		strcmp(name, ">") == 0 || strcmp(name, ">=") == 0)
		return OP_COMPARE;
	if (strcmp(name, "+") == 0 || strcmp(name, "-") == 0 ||
		strcmp(name, "*") == 0 || strcmp(name, "/") == 0 ||
		strcmp(name, "%") == 0)
		return OP_ARITH;
	if (strcmp(name, "||") == 0)
		return OP_CONCAT;
	if (strcmp(name, "~~") == 0 || strcmp(name, "!~~") == 0)
		return OP_LIKE;
	return OP_NONE;
}

/*
 * Check whether a binary operator application is safe to push down, given
 * the operator name and argument types.
 */
static bool
operator_is_safe(const char *name, Oid ltype, Oid rtype)
{
	if (!trino_type_is_deparsable(ltype) || !trino_type_is_deparsable(rtype))
		return false;

	switch (classify_operator(name))
	{
		case OP_EQUALITY:
			/* bytea/uuid/bool/numbers/strings: equality agrees */
			return true;
		case OP_COMPARE:
			/* collation-dependent ordering must stay local */
			return !type_is_collatable(ltype) && !type_is_collatable(rtype) &&
				ltype != BYTEAOID && rtype != BYTEAOID &&
				ltype != BOOLOID && rtype != BOOLOID;
		case OP_ARITH:
			return type_is_numeric(ltype) && type_is_numeric(rtype);
		case OP_CONCAT:
			return type_is_string(ltype) && type_is_string(rtype);
		case OP_LIKE:
			return type_is_string(ltype) && type_is_string(rtype);
		default:
			return false;
	}
}

/*
 * Map a whitelisted pg_catalog function to its Trino name, or NULL if the
 * function is not pushable with the given number of arguments.
 */
static const char *
trino_function_name(const char *pgname, int nargs, List *argtypes)
{
	Oid			t0 = nargs > 0 ? linitial_oid(argtypes) : InvalidOid;

	if (nargs == 1)
	{
		if (strcmp(pgname, "lower") == 0 && type_is_string(t0))
			return "lower";
		if (strcmp(pgname, "upper") == 0 && type_is_string(t0))
			return "upper";
		if ((strcmp(pgname, "length") == 0 ||
			 strcmp(pgname, "char_length") == 0 ||
			 strcmp(pgname, "character_length") == 0) &&
			(type_is_string(t0) || t0 == BYTEAOID))
			return "length";
		if (strcmp(pgname, "btrim") == 0 && type_is_string(t0))
			return "trim";
		if (strcmp(pgname, "ltrim") == 0 && type_is_string(t0))
			return "ltrim";
		if (strcmp(pgname, "rtrim") == 0 && type_is_string(t0))
			return "rtrim";
		if (strcmp(pgname, "abs") == 0 && type_is_numeric(t0))
			return "abs";
		if ((strcmp(pgname, "ceil") == 0 || strcmp(pgname, "ceiling") == 0) &&
			type_is_numeric(t0))
			return "ceiling";
		if (strcmp(pgname, "floor") == 0 && type_is_numeric(t0))
			return "floor";
		if (strcmp(pgname, "round") == 0 && type_is_numeric(t0))
			return "round";
		if (strcmp(pgname, "sqrt") == 0 && type_is_numeric(t0))
			return "sqrt";
	}
	else if (nargs == 2)
	{
		Oid			t1 = lsecond_oid(argtypes);

		if (strcmp(pgname, "round") == 0 && t0 == NUMERICOID && t1 == INT4OID)
			return "round";
		if (strcmp(pgname, "substr") == 0 && type_is_string(t0) && t1 == INT4OID)
			return "substr";
	}
	else if (nargs == 3)
	{
		Oid			t1 = lsecond_oid(argtypes);
		Oid			t2 = lthird_oid(argtypes);

		if (strcmp(pgname, "substr") == 0 && type_is_string(t0) &&
			t1 == INT4OID && t2 == INT4OID)
			return "substr";
	}
	return NULL;
}

/*
 * Map a whitelisted aggregate to its Trino name, or NULL.  Only aggregates
 * whose result type and semantics match are accepted; e.g. avg() over
 * integers returns numeric here but double in Trino, so it stays local.
 */
static const char *
trino_aggregate_name(const char *pgname, bool aggstar, List *argtypes)
{
	Oid			t0 = argtypes ? linitial_oid(argtypes) : InvalidOid;

	if (strcmp(pgname, "count") == 0)
	{
		if (aggstar)
			return "count";
		if (list_length(argtypes) == 1 && trino_type_is_deparsable(t0))
			return "count";
		return NULL;
	}
	if (list_length(argtypes) != 1)
		return NULL;
	if (strcmp(pgname, "sum") == 0 && type_is_numeric(t0))
		return "sum";
	if ((strcmp(pgname, "min") == 0 || strcmp(pgname, "max") == 0) &&
		type_is_sortable(t0) && t0 != BOOLOID)
		return pgname;
	if (strcmp(pgname, "avg") == 0 && t0 == FLOAT8OID)
		return "avg";
	if ((strcmp(pgname, "bool_and") == 0 || strcmp(pgname, "every") == 0) &&
		t0 == BOOLOID)
		return "bool_and";
	if (strcmp(pgname, "bool_or") == 0 && t0 == BOOLOID)
		return "bool_or";
	return NULL;
}

/* ----------------------------------------------------------------
 * Pushdown safety check
 * ---------------------------------------------------------------- */

static bool
is_foreign_var(Var *var, walker_cxt *cxt)
{
	return var->varlevelsup == 0 &&
		bms_is_member(var->varno, cxt->relids) &&
		var->varattno > 0;
}

static bool
foreign_expr_walker(Node *node, walker_cxt *cxt)
{
	if (node == NULL)
		return true;

	switch (nodeTag(node))
	{
		case T_Var:
			{
				Var		   *var = (Var *) node;

				if (!is_foreign_var(var, cxt))
				{
					/*
					 * A Var from another relation (a parameterized path)
					 * becomes a runtime parameter, like postgres_fdw.
					 */
					if (var->varlevelsup == 0 && var->varattno > 0 &&
						!bms_is_member(var->varno, cxt->relids))
						return trino_type_is_deparsable(var->vartype);
					return false;
				}
				return trino_type_is_deparsable(var->vartype);
			}
		case T_Const:
			{
				Const	   *c = (Const *) node;

				if (!trino_type_is_deparsable(c->consttype))
					return false;
				if (c->constisnull)
					return true;
				return try_deparse_datum(NULL, c->constvalue, c->consttype);
			}
		case T_Param:
			{
				Param	   *p = (Param *) node;

				if (p->paramkind != PARAM_EXTERN && p->paramkind != PARAM_EXEC)
					return false;
				return trino_type_is_deparsable(p->paramtype);
			}
		case T_RelabelType:
			{
				RelabelType *r = (RelabelType *) node;

				if (!trino_type_is_deparsable(r->resulttype))
					return false;
				return foreign_expr_walker((Node *) r->arg, cxt);
			}
		case T_OpExpr:
			{
				OpExpr	   *op = (OpExpr *) node;
				char	   *name;

				if (list_length(op->args) != 2)
					return false;
				name = pg_catalog_opname(op->opno);
				if (name == NULL)
					return false;
				if (!operator_is_safe(name,
									  exprType(linitial(op->args)),
									  exprType(lsecond(op->args))))
					return false;
				return foreign_expr_walker((Node *) op->args, cxt);
			}
		case T_DistinctExpr:
			{
				DistinctExpr *op = (DistinctExpr *) node;
				char	   *name;

				if (list_length(op->args) != 2)
					return false;
				name = pg_catalog_opname(op->opno);
				if (name == NULL || strcmp(name, "=") != 0)
					return false;
				if (!operator_is_safe(name,
									  exprType(linitial(op->args)),
									  exprType(lsecond(op->args))))
					return false;
				return foreign_expr_walker((Node *) op->args, cxt);
			}
		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *sa = (ScalarArrayOpExpr *) node;
				char	   *name;
				Node	   *arr;
				Oid			elemtype;

				name = pg_catalog_opname(sa->opno);
				if (name == NULL)
					return false;
				/* x = ANY(...) -> IN,  x <> ALL(...) -> NOT IN */
				if (!((strcmp(name, "=") == 0 && sa->useOr) ||
					  (strcmp(name, "<>") == 0 && !sa->useOr)))
					return false;
				arr = (Node *) lsecond(sa->args);
				elemtype = get_element_type(exprType(arr));
				if (!OidIsValid(elemtype))
					return false;
				if (!operator_is_safe(name, exprType(linitial(sa->args)), elemtype))
					return false;
				if (!foreign_expr_walker(linitial(sa->args), cxt))
					return false;
				if (IsA(arr, Const))
				{
					Const	   *c = (Const *) arr;
					ArrayType  *a;
					Datum	   *elems;
					bool	   *nulls;
					int			n,
								i;
					int16		elmlen;
					bool		elmbyval;
					char		elmalign;

					if (c->constisnull)
						return false;
					a = DatumGetArrayTypeP(c->constvalue);
					if (ARR_NDIM(a) > 1)
						return false;
					get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
					deconstruct_array(a, elemtype, elmlen, elmbyval, elmalign,
									  &elems, &nulls, &n);
					for (i = 0; i < n; i++)
						if (!nulls[i] && !try_deparse_datum(NULL, elems[i], elemtype))
							return false;
					return true;
				}
				if (IsA(arr, ArrayExpr))
				{
					ArrayExpr  *ae = (ArrayExpr *) arr;

					if (ae->multidims)
						return false;
					return foreign_expr_walker((Node *) ae->elements, cxt);
				}
				return false;
			}
		case T_BoolExpr:
			return foreign_expr_walker((Node *) ((BoolExpr *) node)->args, cxt);
		case T_NullTest:
			{
				NullTest   *nt = (NullTest *) node;

				if (nt->argisrow)
					return false;
				/* IS NULL is safe for any column type */
				if (IsA(nt->arg, Var) && is_foreign_var((Var *) nt->arg, cxt))
					return true;
				return foreign_expr_walker((Node *) nt->arg, cxt);
			}
		case T_CoalesceExpr:
			{
				CoalesceExpr *ce = (CoalesceExpr *) node;

				if (!trino_type_is_deparsable(ce->coalescetype))
					return false;
				return foreign_expr_walker((Node *) ce->args, cxt);
			}
		case T_MinMaxExpr:
			{
				MinMaxExpr *mm = (MinMaxExpr *) node;

				if (!type_is_sortable(mm->minmaxtype))
					return false;
				return foreign_expr_walker((Node *) mm->args, cxt);
			}
		case T_FuncExpr:
			{
				FuncExpr   *fe = (FuncExpr *) node;
				char	   *name;
				List	   *argtypes = NIL;
				ListCell   *lc;

				if (fe->funcretset || fe->funcvariadic)
					return false;
				/* casts are not pushed unless they are exact widenings */
				if (fe->funcformat == COERCE_EXPLICIT_CAST ||
					fe->funcformat == COERCE_IMPLICIT_CAST)
				{
					if (list_length(fe->args) == 1 &&
						is_exact_widening_cast(exprType(linitial(fe->args)),
											   fe->funcresulttype))
						return foreign_expr_walker(linitial(fe->args), cxt);
					return false;
				}
				name = pg_catalog_funcname(fe->funcid);
				if (name == NULL)
					return false;
				foreach(lc, fe->args)
					argtypes = lappend_oid(argtypes, exprType(lfirst(lc)));
				if (trino_function_name(name, list_length(fe->args), argtypes) == NULL)
					return false;
				if (!trino_type_is_deparsable(fe->funcresulttype))
					return false;
				return foreign_expr_walker((Node *) fe->args, cxt);
			}
		case T_Aggref:
			{
				Aggref	   *agg = (Aggref *) node;
				char	   *name;
				List	   *argtypes = NIL;
				ListCell   *lc;
				walker_cxt	inner = *cxt;

				if (!cxt->allow_aggs)
					return false;
				if (agg->aggsplit != AGGSPLIT_SIMPLE ||
					agg->aggkind != AGGKIND_NORMAL ||
					agg->aggorder != NIL || agg->aggdirectargs != NIL ||
					agg->aggvariadic)
					return false;
				if (agg->aggdistinct != NIL && list_length(agg->args) != 1)
					return false;
				name = pg_catalog_funcname(agg->aggfnoid);
				if (name == NULL)
					return false;
				foreach(lc, agg->args)
				{
					TargetEntry *tle = (TargetEntry *) lfirst(lc);

					if (tle->resjunk)
						return false;
					argtypes = lappend_oid(argtypes, exprType((Node *) tle->expr));
				}
				if (trino_aggregate_name(name, agg->aggstar, argtypes) == NULL)
					return false;
				if (!trino_type_is_deparsable(agg->aggtype))
					return false;
				/* no nested aggregates inside the arguments or the filter */
				inner.allow_aggs = false;
				foreach(lc, agg->args)
				{
					TargetEntry *tle = (TargetEntry *) lfirst(lc);

					if (!foreign_expr_walker((Node *) tle->expr, &inner))
						return false;
				}
				if (agg->aggfilter &&
					!foreign_expr_walker((Node *) agg->aggfilter, &inner))
					return false;
				return true;
			}
		case T_List:
			{
				ListCell   *lc;

				foreach(lc, (List *) node)
					if (!foreign_expr_walker((Node *) lfirst(lc), cxt))
						return false;
				return true;
			}
		default:
			return false;
	}
}

/*
 * Can this expression be evaluated by Trino with identical results, as part
 * of a scan of rel (a base, join or grouped relation)?
 */
bool
trino_is_foreign_expr(PlannerInfo *root, RelOptInfo *rel, Expr *expr)
{
	walker_cxt	cxt;

	cxt.root = root;
	cxt.relids = rel_relids(rel);
	cxt.allow_aggs = IS_UPPER_REL(rel);

	if (!foreign_expr_walker((Node *) expr, &cxt))
		return false;

	/* no mutable functions: now(), random(), ... must run locally */
	if (contain_mutable_functions((Node *) expr))
		return false;

	return true;
}

/*
 * Is this expression one that must be sent as a parameter: a Param or a
 * Var of some other relation?  (Used to classify join clause arguments.)
 */
bool
trino_is_foreign_param(PlannerInfo *root, RelOptInfo *rel, Expr *expr)
{
	if (IsA(expr, Param))
		return true;
	if (IsA(expr, Var))
	{
		Var		   *var = (Var *) expr;

		return var->varlevelsup == 0 &&
			!bms_is_member(var->varno, rel_relids(rel));
	}
	return false;
}

/*
 * Find the expression of rel that a pathkey sorts by, or NULL if the sort
 * cannot be expressed as a Trino ORDER BY with identical results.
 */
static Expr *
pathkey_expr(PlannerInfo *root, RelOptInfo *rel, PathKey *pathkey)
{
	EquivalenceClass *ec = pathkey->pk_eclass;
	Relids		relids = rel_relids(rel);
	ListCell   *lc;

	if (ec->ec_has_volatile)
		return NULL;

	foreach(lc, ec->ec_members)
	{
		EquivalenceMember *em = (EquivalenceMember *) lfirst(lc);
		Expr	   *expr = em->em_expr;
		Oid			type;
		Oid			opclass;

		if (em->em_is_child || em->em_is_const)
			continue;
		if (!bms_is_subset(em->em_relids, relids))
			continue;
		while (IsA(expr, RelabelType))
			expr = ((RelabelType *) expr)->arg;
		/* plain columns, or aggregates when sorting a grouped relation */
		if (!IsA(expr, Var) && !(IsA(expr, Aggref) && IS_UPPER_REL(rel)))
			continue;
		type = exprType((Node *) expr);
		if (!type_is_sortable(type) || type_is_collatable(type))
			continue;
		if (!trino_is_foreign_expr(root, rel, expr))
			continue;
		/* only the type's default btree ordering maps to ASC/DESC */
		opclass = GetDefaultOpClass(type, BTREE_AM_OID);
		if (OidIsValid(opclass) &&
			get_opclass_family(opclass) == pathkey->pk_opfamily)
			return expr;
	}
	return NULL;
}

bool
trino_is_sortable_pathkey(PlannerInfo *root, RelOptInfo *rel, PathKey *pathkey)
{
	return pathkey_expr(root, rel, pathkey) != NULL;
}

/* ----------------------------------------------------------------
 * Literals
 * ---------------------------------------------------------------- */

void
trino_quote_identifier(StringInfo buf, const char *ident)
{
	appendStringInfoChar(buf, '"');
	for (; *ident; ident++)
	{
		if (*ident == '"')
			appendStringInfoChar(buf, '"');
		appendStringInfoChar(buf, *ident);
	}
	appendStringInfoChar(buf, '"');
}

void
trino_quote_literal(StringInfo buf, const char *str)
{
	appendStringInfoChar(buf, '\'');
	for (; *str; str++)
	{
		if (*str == '\'')
			appendStringInfoChar(buf, '\'');
		appendStringInfoChar(buf, *str);
	}
	appendStringInfoChar(buf, '\'');
}

static void
append_fsec(StringInfo buf, fsec_t fsec)
{
	if (fsec != 0)
		appendStringInfo(buf, ".%06d", (int) fsec);
}

static bool
deparse_date(StringInfo buf, DateADT date)
{
	int			y,
				m,
				d;

	if (DATE_NOT_FINITE(date))
		return false;
	j2date(date + POSTGRES_EPOCH_JDATE, &y, &m, &d);
	if (y <= 0)
		return false;
	if (buf)
		appendStringInfo(buf, "DATE '%04d-%02d-%02d'", y, m, d);
	return true;
}

static bool
deparse_timestamp(StringInfo buf, Timestamp ts, bool with_tz)
{
	struct pg_tm tm;
	fsec_t		fsec;

	if (TIMESTAMP_NOT_FINITE(ts))
		return false;
	/* tzp == NULL: no zone conversion, i.e. UTC fields for timestamptz */
	if (timestamp2tm(ts, NULL, &tm, &fsec, NULL, NULL) != 0)
		return false;
	if (tm.tm_year <= 0)
		return false;
	if (buf)
	{
		appendStringInfo(buf, "TIMESTAMP '%04d-%02d-%02d %02d:%02d:%02d",
						 tm.tm_year, tm.tm_mon, tm.tm_mday,
						 tm.tm_hour, tm.tm_min, tm.tm_sec);
		append_fsec(buf, fsec);
		appendStringInfoString(buf, with_tz ? " UTC'" : "'");
	}
	return true;
}

static bool
deparse_time(StringInfo buf, TimeADT t)
{
	struct pg_tm tm;
	fsec_t		fsec;

	time2tm(t, &tm, &fsec);
	if (buf)
	{
		appendStringInfo(buf, "TIME '%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
		append_fsec(buf, fsec);
		appendStringInfoChar(buf, '\'');
	}
	return true;
}

static bool
deparse_interval(StringInfo buf, Interval *iv)
{
	if (iv->month != 0)
	{
		int			months = iv->month;
		bool		neg = months < 0;

		if (iv->day != 0 || iv->time != 0)
			return false;
		if (neg)
			months = -months;
		if (buf)
			appendStringInfo(buf, "INTERVAL '%s%d-%d' YEAR TO MONTH",
							 neg ? "-" : "", months / 12, months % 12);
		return true;
	}
	else
	{
		int64		time = iv->time;
		int32		day = iv->day;
		bool		neg = false;
		int64		h,
					m,
					s,
					us;

		if ((day < 0 && time > 0) || (day > 0 && time < 0))
			return false;		/* mixed signs: not expressible */
		if (day < 0 || time < 0)
		{
			neg = true;
			day = -day;
			time = -time;
		}
		h = time / USECS_PER_HOUR;
		time -= h * USECS_PER_HOUR;
		m = time / USECS_PER_MINUTE;
		time -= m * USECS_PER_MINUTE;
		s = time / USECS_PER_SEC;
		us = time - s * USECS_PER_SEC;
		/* Trino wants hours < 24; a Trino interval is a plain duration */
		day += (int32) (h / 24);
		h %= 24;
		if (buf)
		{
			appendStringInfo(buf, "INTERVAL '%s%d %02d:%02d:%02d",
							 neg ? "-" : "", day, (int) h, (int) m, (int) s);
			if (us != 0)
				appendStringInfo(buf, ".%03d", (int) (us / 1000));
			appendStringInfoString(buf, "' DAY TO SECOND");
		}
		return true;
	}
}

static void
append_hex(StringInfo buf, const unsigned char *data, int len)
{
	static const char hexdigits[] = "0123456789ABCDEF";
	int			i;

	for (i = 0; i < len; i++)
	{
		appendStringInfoChar(buf, hexdigits[data[i] >> 4]);
		appendStringInfoChar(buf, hexdigits[data[i] & 0xf]);
	}
}

/*
 * Write a Datum as a Trino literal.  Returns false if the value has no
 * faithful Trino representation.  buf may be NULL to only test.
 */
static bool
try_deparse_datum(StringInfo buf, Datum value, Oid type)
{
	switch (type)
	{
		case BOOLOID:
			if (buf)
				appendStringInfoString(buf, DatumGetBool(value) ? "TRUE" : "FALSE");
			return true;
		case INT2OID:
			if (buf)
				appendStringInfo(buf, "%d", (int) DatumGetInt16(value));
			return true;
		case INT4OID:
			if (buf)
				appendStringInfo(buf, "%d", DatumGetInt32(value));
			return true;
		case INT8OID:
			if (buf)
				appendStringInfo(buf, INT64_FORMAT, DatumGetInt64(value));
			return true;
		case FLOAT4OID:
		case FLOAT8OID:
			{
				double		d = (type == FLOAT4OID) ?
					(double) DatumGetFloat4(value) : DatumGetFloat8(value);

				if (buf)
				{
					if (isnan(d))
						appendStringInfoString(buf, "nan()");
					else if (isinf(d))
						appendStringInfoString(buf, d > 0 ? "infinity()" : "-infinity()");
					else
					{
						char	   *s = OidOutputFunctionCall(
															  type == FLOAT4OID ? F_FLOAT4OUT : F_FLOAT8OUT, value);

						appendStringInfo(buf, "%s '%s'",
										 type == FLOAT4OID ? "REAL" : "DOUBLE", s);
					}
				}
				return true;
			}
		case NUMERICOID:
			{
				Numeric		n = DatumGetNumeric(value);

				if (numeric_is_nan(n) || numeric_is_inf(n))
					return false;
				if (buf)
				{
					char	   *s = OidOutputFunctionCall(F_NUMERIC_OUT, value);

					appendStringInfo(buf, "DECIMAL '%s'", s);
				}
				return true;
			}
		case TEXTOID:
		case VARCHAROID:
		case BPCHAROID:
			if (buf)
			{
				char	   *s = TextDatumGetCString(value);

				trino_quote_literal(buf, s);
			}
			return true;
		case DATEOID:
			return deparse_date(buf, DatumGetDateADT(value));
		case TIMESTAMPOID:
			return deparse_timestamp(buf, DatumGetTimestamp(value), false);
		case TIMESTAMPTZOID:
			return deparse_timestamp(buf, DatumGetTimestampTz(value), true);
		case TIMEOID:
			return deparse_time(buf, DatumGetTimeADT(value));
		case UUIDOID:
			if (buf)
			{
				char	   *s = OidOutputFunctionCall(F_UUID_OUT, value);

				appendStringInfo(buf, "UUID '%s'", s);
			}
			return true;
		case BYTEAOID:
			if (buf)
			{
				bytea	   *b = DatumGetByteaPP(value);

				appendStringInfoString(buf, "X'");
				append_hex(buf, (unsigned char *) VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b));
				appendStringInfoChar(buf, '\'');
			}
			return true;
		case INTERVALOID:
			return deparse_interval(buf, DatumGetIntervalP(value));
		case JSONOID:
		case JSONBOID:
			if (buf)
			{
				char	   *s = OidOutputFunctionCall(
												  type == JSONOID ? F_JSON_OUT : F_JSONB_OUT, value);

				appendStringInfoString(buf, "JSON ");
				trino_quote_literal(buf, s);
			}
			return true;
		case INETOID:
			if (buf)
			{
				char	   *s = OidOutputFunctionCall(F_INET_OUT, value);

				if (strchr(s, '/'))
					return false;
				appendStringInfo(buf, "IPADDRESS '%s'", s);
			}
			return true;
		default:
			{
				/* one-dimensional arrays of supported element types */
				Oid			elemtype = get_element_type(type);

				if (OidIsValid(elemtype))
				{
					ArrayType  *a = DatumGetArrayTypeP(value);
					Datum	   *elems;
					bool	   *nulls;
					int			n,
								i;
					int16		elmlen;
					bool		elmbyval;
					char		elmalign;

					if (ARR_NDIM(a) > 1)
						return false;
					get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
					deconstruct_array(a, elemtype, elmlen, elmbyval, elmalign,
									  &elems, &nulls, &n);
					if (buf)
						appendStringInfoString(buf, "ARRAY[");
					for (i = 0; i < n; i++)
					{
						if (buf && i > 0)
							appendStringInfoString(buf, ", ");
						if (nulls[i])
						{
							if (buf)
								appendStringInfoString(buf, "NULL");
							continue;
						}
						if (!try_deparse_datum(buf, elems[i], elemtype))
							return false;
					}
					if (buf)
						appendStringInfoChar(buf, ']');
					return true;
				}
				return false;
			}
	}
}

/*
 * Append a Datum as a Trino literal, raising an error if impossible.
 */
void
trino_deparse_datum(StringInfo buf, Datum value, Oid type, bool isnull)
{
	if (isnull)
	{
		appendStringInfoString(buf, "NULL");
		return;
	}
	if (!try_deparse_datum(buf, value, type))
		ereport(ERROR,
				(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
				 errmsg("trino_fdw: cannot represent a value of type %s as a Trino literal",
						format_type_be(type))));
}

/* ----------------------------------------------------------------
 * Expression deparsing
 * ---------------------------------------------------------------- */

char *
trino_get_column_name(Oid relid, AttrNumber attnum)
{
	List	   *options = GetForeignColumnOptions(relid, attnum);
	ListCell   *lc;

	foreach(lc, options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "column_name") == 0)
			return defGetString(def);
	}
	return get_attname(relid, attnum, false);
}

static void
deparse_var(Var *var, deparse_cxt *cxt)
{
	if (bms_is_member(var->varno, cxt->relids) && var->varlevelsup == 0)
	{
		RangeTblEntry *rte = planner_rt_fetch(var->varno, cxt->root);

		if (cxt->qualify)
			appendStringInfo(cxt->buf, "r%d.", var->varno);
		trino_quote_identifier(cxt->buf, trino_get_column_name(rte->relid, var->varattno));
	}
	else
	{
		/* outer-relation Var: becomes a runtime parameter */
		if (cxt->params_list == NULL)
			elog(ERROR, "trino_fdw: unexpected outer Var in deparse");
		*cxt->params_list = lappend(*cxt->params_list, var);
		*cxt->param_offsets = lappend_int(*cxt->param_offsets, cxt->buf->len);
	}
}

static void
deparse_const(Const *c, deparse_cxt *cxt)
{
	if (c->constisnull)
	{
		appendStringInfoString(cxt->buf, "NULL");
		return;
	}
	if (!try_deparse_datum(cxt->buf, c->constvalue, c->consttype))
		elog(ERROR, "trino_fdw: constant unexpectedly not deparsable");
}

static void
deparse_param(Param *p, deparse_cxt *cxt)
{
	if (cxt->params_list == NULL)
		elog(ERROR, "trino_fdw: unexpected Param in deparse");
	*cxt->params_list = lappend(*cxt->params_list, p);
	*cxt->param_offsets = lappend_int(*cxt->param_offsets, cxt->buf->len);
}

static void
deparse_op_expr(OpExpr *op, deparse_cxt *cxt)
{
	char	   *name = pg_catalog_opname(op->opno);
	bool		not_like = strcmp(name, "!~~") == 0;

	appendStringInfoChar(cxt->buf, '(');
	deparse_expr(linitial(op->args), cxt);
	if (classify_operator(name) == OP_LIKE)
		appendStringInfoString(cxt->buf, not_like ? " NOT LIKE " : " LIKE ");
	else
		appendStringInfo(cxt->buf, " %s ", name);
	deparse_expr(lsecond(op->args), cxt);
	/* PostgreSQL LIKE has an implicit backslash escape; Trino has none */
	if (classify_operator(name) == OP_LIKE)
		appendStringInfoString(cxt->buf, " ESCAPE '\\'");
	appendStringInfoChar(cxt->buf, ')');
}

static void
deparse_scalar_array_op(ScalarArrayOpExpr *sa, deparse_cxt *cxt)
{
	char	   *name = pg_catalog_opname(sa->opno);
	bool		negate = strcmp(name, "<>") == 0;
	Node	   *arr = (Node *) lsecond(sa->args);
	Oid			elemtype = get_element_type(exprType(arr));
	int			nelems;

	if (IsA(arr, Const))
	{
		Const	   *c = (Const *) arr;
		ArrayType  *a = DatumGetArrayTypeP(c->constvalue);

		nelems = ArrayGetNItems(ARR_NDIM(a), ARR_DIMS(a));
	}
	else
		nelems = list_length(((ArrayExpr *) arr)->elements);

	if (nelems == 0)
	{
		/* x IN () is a syntax error; the result is simply FALSE / TRUE */
		appendStringInfoString(cxt->buf, negate ? "TRUE" : "FALSE");
		return;
	}

	appendStringInfoChar(cxt->buf, '(');
	deparse_expr(linitial(sa->args), cxt);
	appendStringInfo(cxt->buf, "%s IN (", negate ? " NOT" : "");

	if (IsA(arr, Const))
	{
		Const	   *c = (Const *) arr;
		ArrayType  *a = DatumGetArrayTypeP(c->constvalue);
		Datum	   *elems;
		bool	   *nulls;
		int			n,
					i;
		int16		elmlen;
		bool		elmbyval;
		char		elmalign;

		get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
		deconstruct_array(a, elemtype, elmlen, elmbyval, elmalign,
						  &elems, &nulls, &n);
		for (i = 0; i < n; i++)
		{
			if (i > 0)
				appendStringInfoString(cxt->buf, ", ");
			if (nulls[i])
				appendStringInfoString(cxt->buf, "NULL");
			else if (!try_deparse_datum(cxt->buf, elems[i], elemtype))
				elog(ERROR, "trino_fdw: constant unexpectedly not deparsable");
		}
	}
	else
	{
		ArrayExpr  *ae = (ArrayExpr *) arr;
		ListCell   *lc;
		bool		first = true;

		foreach(lc, ae->elements)
		{
			if (!first)
				appendStringInfoString(cxt->buf, ", ");
			first = false;
			deparse_expr((Node *) lfirst(lc), cxt);
		}
	}
	appendStringInfoString(cxt->buf, "))");
}

static void
deparse_func_expr(FuncExpr *fe, deparse_cxt *cxt)
{
	char	   *pgname = pg_catalog_funcname(fe->funcid);
	List	   *argtypes = NIL;
	ListCell   *lc;
	const char *tname;
	bool		first = true;

	/* exact widening cast: Trino coerces implicitly, so just emit the arg */
	if (fe->funcformat == COERCE_EXPLICIT_CAST ||
		fe->funcformat == COERCE_IMPLICIT_CAST)
	{
		deparse_expr((Node *) linitial(fe->args), cxt);
		return;
	}

	foreach(lc, fe->args)
		argtypes = lappend_oid(argtypes, exprType(lfirst(lc)));
	tname = trino_function_name(pgname, list_length(fe->args), argtypes);

	appendStringInfo(cxt->buf, "%s(", tname);
	foreach(lc, fe->args)
	{
		if (!first)
			appendStringInfoString(cxt->buf, ", ");
		first = false;
		deparse_expr((Node *) lfirst(lc), cxt);
	}
	appendStringInfoChar(cxt->buf, ')');
}

static void
deparse_aggref(Aggref *agg, deparse_cxt *cxt)
{
	char	   *pgname = pg_catalog_funcname(agg->aggfnoid);
	List	   *argtypes = NIL;
	ListCell   *lc;
	bool		first = true;

	foreach(lc, agg->args)
		argtypes = lappend_oid(argtypes, exprType((Node *) ((TargetEntry *) lfirst(lc))->expr));

	appendStringInfo(cxt->buf, "%s(", trino_aggregate_name(pgname, agg->aggstar, argtypes));
	if (agg->aggstar)
		appendStringInfoChar(cxt->buf, '*');
	else
	{
		if (agg->aggdistinct)
			appendStringInfoString(cxt->buf, "DISTINCT ");
		foreach(lc, agg->args)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);

			if (!first)
				appendStringInfoString(cxt->buf, ", ");
			first = false;
			deparse_expr((Node *) tle->expr, cxt);
		}
	}
	appendStringInfoChar(cxt->buf, ')');
	if (agg->aggfilter)
	{
		appendStringInfoString(cxt->buf, " FILTER (WHERE ");
		deparse_expr((Node *) agg->aggfilter, cxt);
		appendStringInfoChar(cxt->buf, ')');
	}
}

static void
deparse_expr(Node *node, deparse_cxt *cxt)
{
	switch (nodeTag(node))
	{
		case T_Aggref:
			deparse_aggref((Aggref *) node, cxt);
			break;
		case T_Var:
			deparse_var((Var *) node, cxt);
			break;
		case T_Const:
			deparse_const((Const *) node, cxt);
			break;
		case T_Param:
			deparse_param((Param *) node, cxt);
			break;
		case T_RelabelType:
			deparse_expr((Node *) ((RelabelType *) node)->arg, cxt);
			break;
		case T_OpExpr:
			deparse_op_expr((OpExpr *) node, cxt);
			break;
		case T_DistinctExpr:
			{
				DistinctExpr *de = (DistinctExpr *) node;

				appendStringInfoChar(cxt->buf, '(');
				deparse_expr(linitial(de->args), cxt);
				appendStringInfoString(cxt->buf, " IS DISTINCT FROM ");
				deparse_expr(lsecond(de->args), cxt);
				appendStringInfoChar(cxt->buf, ')');
				break;
			}
		case T_ScalarArrayOpExpr:
			deparse_scalar_array_op((ScalarArrayOpExpr *) node, cxt);
			break;
		case T_BoolExpr:
			{
				BoolExpr   *be = (BoolExpr *) node;
				ListCell   *lc;
				bool		first = true;
				const char *op;

				if (be->boolop == NOT_EXPR)
				{
					appendStringInfoString(cxt->buf, "(NOT ");
					deparse_expr(linitial(be->args), cxt);
					appendStringInfoChar(cxt->buf, ')');
					break;
				}
				op = be->boolop == AND_EXPR ? " AND " : " OR ";
				appendStringInfoChar(cxt->buf, '(');
				foreach(lc, be->args)
				{
					if (!first)
						appendStringInfoString(cxt->buf, op);
					first = false;
					deparse_expr((Node *) lfirst(lc), cxt);
				}
				appendStringInfoChar(cxt->buf, ')');
				break;
			}
		case T_NullTest:
			{
				NullTest   *nt = (NullTest *) node;

				appendStringInfoChar(cxt->buf, '(');
				deparse_expr((Node *) nt->arg, cxt);
				appendStringInfoString(cxt->buf, nt->nulltesttype == IS_NULL ?
									   " IS NULL)" : " IS NOT NULL)");
				break;
			}
		case T_CoalesceExpr:
			{
				CoalesceExpr *ce = (CoalesceExpr *) node;
				ListCell   *lc;
				bool		first = true;

				appendStringInfoString(cxt->buf, "COALESCE(");
				foreach(lc, ce->args)
				{
					if (!first)
						appendStringInfoString(cxt->buf, ", ");
					first = false;
					deparse_expr((Node *) lfirst(lc), cxt);
				}
				appendStringInfoChar(cxt->buf, ')');
				break;
			}
		case T_MinMaxExpr:
			{
				MinMaxExpr *mm = (MinMaxExpr *) node;
				ListCell   *lc;
				bool		first = true;

				appendStringInfoString(cxt->buf, mm->op == IS_GREATEST ?
									   "GREATEST(" : "LEAST(");
				foreach(lc, mm->args)
				{
					if (!first)
						appendStringInfoString(cxt->buf, ", ");
					first = false;
					deparse_expr((Node *) lfirst(lc), cxt);
				}
				appendStringInfoChar(cxt->buf, ')');
				break;
			}
		case T_FuncExpr:
			deparse_func_expr((FuncExpr *) node, cxt);
			break;
		default:
			elog(ERROR, "trino_fdw: unsupported expression type %d for deparse",
				 (int) nodeTag(node));
	}
}

/* ----------------------------------------------------------------
 * Statements
 * ---------------------------------------------------------------- */

/* "catalog"."schema"."table", using whatever parts are configured */
void
trino_deparse_relation(StringInfo buf, Relation rel, TrinoOptions *opts)
{
	const char *table = opts->table_name ? opts->table_name :
		RelationGetRelationName(rel);

	if (opts->catalog && opts->schema_name)
	{
		trino_quote_identifier(buf, opts->catalog);
		appendStringInfoChar(buf, '.');
	}
	if (opts->schema_name)
	{
		trino_quote_identifier(buf, opts->schema_name);
		appendStringInfoChar(buf, '.');
	}
	trino_quote_identifier(buf, table);
}

static void
deparse_target_list(deparse_cxt *cxt, RelOptInfo *rel, List *tlist,
					List **retrieved_attrs)
{
	StringInfo	buf = cxt->buf;
	bool		first = true;

	*retrieved_attrs = NIL;

	if (tlist != NIL)
	{
		/* join or grouped relation: each entry is an expression to fetch */
		ListCell   *lc;
		int			i = 0;

		foreach(lc, tlist)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);

			if (!first)
				appendStringInfoString(buf, ", ");
			first = false;
			deparse_expr((Node *) tle->expr, cxt);
			*retrieved_attrs = lappend_int(*retrieved_attrs, ++i);
		}
	}
	else
	{
		TrinoRelationInfo *fpinfo = (TrinoRelationInfo *) rel->fdw_private;
		RangeTblEntry *rte = planner_rt_fetch(rel->relid, cxt->root);
		Relation	relation = table_open(rte->relid, NoLock);
		TupleDesc	tupdesc = RelationGetDescr(relation);
		bool		whole_row = bms_is_member(0 - FirstLowInvalidHeapAttributeNumber,
											  fpinfo->attrs_used);
		int			i;

		for (i = 1; i <= tupdesc->natts; i++)
		{
			Form_pg_attribute attr = TupleDescAttr(tupdesc, i - 1);

			if (attr->attisdropped)
				continue;
			if (whole_row ||
				bms_is_member(i - FirstLowInvalidHeapAttributeNumber, fpinfo->attrs_used))
			{
				if (!first)
					appendStringInfoString(buf, ", ");
				first = false;
				trino_quote_identifier(buf, trino_get_column_name(rte->relid, i));
				*retrieved_attrs = lappend_int(*retrieved_attrs, i);
			}
		}
		table_close(relation, NoLock);
	}

	if (first)
		appendStringInfoString(buf, "1");	/* no columns needed */
}

static const char *
join_type_name(JoinType jointype)
{
	switch (jointype)
	{
		case JOIN_INNER:
			return "INNER";
		case JOIN_LEFT:
			return "LEFT";
		case JOIN_RIGHT:
			return "RIGHT";
		case JOIN_FULL:
			return "FULL";
		default:
			elog(ERROR, "trino_fdw: unsupported join type %d", (int) jointype);
			return NULL;
	}
}

/*
 * FROM clause for a base or join relation.  Base relations get the alias
 * r<varno> when columns need qualification.
 */
static void
deparse_from(deparse_cxt *cxt, RelOptInfo *rel)
{
	TrinoRelationInfo *fpinfo = (TrinoRelationInfo *) rel->fdw_private;
	StringInfo	buf = cxt->buf;

	if (IS_SIMPLE_REL(rel))
	{
		RangeTblEntry *rte = planner_rt_fetch(rel->relid, cxt->root);
		Relation	relation = table_open(rte->relid, NoLock);

		trino_deparse_relation(buf, relation, fpinfo->opts);
		table_close(relation, NoLock);
		if (cxt->qualify)
			appendStringInfo(buf, " r%d", rel->relid);
		return;
	}

	appendStringInfoChar(buf, '(');
	deparse_from(cxt, fpinfo->outerrel);
	appendStringInfo(buf, " %s JOIN ", join_type_name(fpinfo->jointype));
	deparse_from(cxt, fpinfo->innerrel);
	appendStringInfoString(buf, " ON ");
	if (fpinfo->joinclauses == NIL)
		appendStringInfoString(buf, "TRUE");
	else
	{
		ListCell   *lc;
		bool		first = true;

		foreach(lc, fpinfo->joinclauses)
		{
			RestrictInfo *ri = (RestrictInfo *) lfirst(lc);

			if (!first)
				appendStringInfoString(buf, " AND ");
			first = false;
			deparse_expr((Node *) ri->clause, cxt);
		}
	}
	appendStringInfoChar(buf, ')');
}

static void
deparse_conds(deparse_cxt *cxt, List *rinfos, const char *keyword)
{
	ListCell   *lc;
	bool		first = true;

	if (rinfos == NIL)
		return;
	appendStringInfo(cxt->buf, " %s ", keyword);
	foreach(lc, rinfos)
	{
		Node	   *clause = (Node *) lfirst(lc);

		if (IsA(clause, RestrictInfo))
			clause = (Node *) ((RestrictInfo *) clause)->clause;
		if (!first)
			appendStringInfoString(cxt->buf, " AND ");
		first = false;
		deparse_expr(clause, cxt);
	}
}

static void
deparse_pathkeys(deparse_cxt *cxt, RelOptInfo *rel, List *pathkeys)
{
	StringInfo	buf = cxt->buf;
	ListCell   *lc;
	bool		first = true;

	appendStringInfoString(buf, " ORDER BY ");
	foreach(lc, pathkeys)
	{
		PathKey    *pk = (PathKey *) lfirst(lc);
		Expr	   *expr = pathkey_expr(cxt->root, rel, pk);
		bool		asc;

		if (expr == NULL)
			elog(ERROR, "trino_fdw: pathkey unexpectedly not deparsable");
		if (!first)
			appendStringInfoString(buf, ", ");
		first = false;
		deparse_expr((Node *) expr, cxt);
#if PG_VERSION_NUM >= 180000
		asc = (pk->pk_cmptype == COMPARE_LT);
#else
		asc = (pk->pk_strategy == BTLessStrategyNumber);
#endif
		appendStringInfoString(buf, asc ? " ASC" : " DESC");
		appendStringInfoString(buf, pk->pk_nulls_first ? " NULLS FIRST" : " NULLS LAST");
	}
}

/*
 * GROUP BY: the grouping expressions, located in the grouped tlist via their
 * sortgroupref.
 */
static void
deparse_group_by(deparse_cxt *cxt, List *tlist)
{
	Query	   *query = cxt->root->parse;
	ListCell   *lc;
	bool		first = true;

	if (query->groupClause == NIL)
		return;
	appendStringInfoString(cxt->buf, " GROUP BY ");
	foreach(lc, query->groupClause)
	{
		SortGroupClause *grp = (SortGroupClause *) lfirst(lc);
		TargetEntry *tle = get_sortgroupref_tle(grp->tleSortGroupRef, tlist);

		if (!first)
			appendStringInfoString(cxt->buf, ", ");
		first = false;
		deparse_expr((Node *) tle->expr, cxt);
	}
}

/*
 * Build the SELECT statement for a scan of rel, which may be a base
 * relation, a pushed-down join, or a pushed-down aggregate over either.
 *
 * For a base relation tlist is NIL and the columns in attrs_used are
 * fetched; otherwise tlist lists the expressions to fetch, in order.
 * remote_conds are the WHERE clauses (ignored for a grouped relation, whose
 * WHERE clauses belong to its input and whose HAVING is in its fpinfo).
 */
void
trino_deparse_select(StringInfo buf, PlannerInfo *root, RelOptInfo *rel,
					 List *tlist, List *remote_conds, List *pathkeys,
					 bool has_limit, Node *limit_count, Node *limit_offset,
					 List **retrieved_attrs, List **params_list,
					 List **param_offsets)
{
	TrinoRelationInfo *fpinfo = (TrinoRelationInfo *) rel->fdw_private;
	RelOptInfo *scanrel = IS_UPPER_REL(rel) ? fpinfo->outerrel : rel;
	TrinoRelationInfo *scan_fpinfo = (TrinoRelationInfo *) scanrel->fdw_private;
	deparse_cxt cxt;

	cxt.root = root;
	cxt.relids = scanrel->relids;
	cxt.qualify = !IS_SIMPLE_REL(scanrel);
	cxt.buf = buf;
	cxt.params_list = params_list;
	cxt.param_offsets = param_offsets;
	*params_list = NIL;
	*param_offsets = NIL;

	appendStringInfoString(buf, "SELECT ");
	deparse_target_list(&cxt, scanrel, tlist, retrieved_attrs);
	appendStringInfoString(buf, " FROM ");
	deparse_from(&cxt, scanrel);

	if (IS_UPPER_REL(rel))
	{
		deparse_conds(&cxt, scan_fpinfo->remote_conds, "WHERE");
		deparse_group_by(&cxt, fpinfo->grouped_tlist);
		deparse_conds(&cxt, fpinfo->having_remote, "HAVING");
	}
	else
		deparse_conds(&cxt, remote_conds, "WHERE");

	if (pathkeys != NIL)
		deparse_pathkeys(&cxt, rel, pathkeys);

	if (has_limit)
	{
		/* Trino syntax: ORDER BY ... OFFSET n ROWS LIMIT m */
		if (limit_offset != NULL)
		{
			appendStringInfoString(buf, " OFFSET ");
			deparse_expr(limit_offset, &cxt);
			appendStringInfoString(buf, " ROWS");
		}
		if (limit_count != NULL)
		{
			appendStringInfoString(buf, " LIMIT ");
			deparse_expr(limit_count, &cxt);
		}
	}
}

/*
 * UPDATE rel SET "c" = expr, ... WHERE conds  /  DELETE FROM rel WHERE conds
 */
void
trino_deparse_direct_modify(StringInfo buf, PlannerInfo *root, RelOptInfo *rel,
							CmdType operation, List *target_attrs,
							List *target_exprs, List *remote_exprs,
							List **params_list, List **param_offsets)
{
	TrinoRelationInfo *fpinfo = (TrinoRelationInfo *) rel->fdw_private;
	RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);
	Relation	relation = table_open(rte->relid, NoLock);
	deparse_cxt cxt;

	cxt.root = root;
	cxt.relids = rel->relids;
	cxt.qualify = false;
	cxt.buf = buf;
	cxt.params_list = params_list;
	cxt.param_offsets = param_offsets;
	*params_list = NIL;
	*param_offsets = NIL;

	if (operation == CMD_UPDATE)
	{
		ListCell   *lc;
		ListCell   *lc2;
		bool		first = true;

		appendStringInfoString(buf, "UPDATE ");
		trino_deparse_relation(buf, relation, fpinfo->opts);
		appendStringInfoString(buf, " SET ");
		forboth(lc, target_attrs, lc2, target_exprs)
		{
			if (!first)
				appendStringInfoString(buf, ", ");
			first = false;
			trino_quote_identifier(buf, trino_get_column_name(rte->relid, lfirst_int(lc)));
			appendStringInfoString(buf, " = ");
			deparse_expr((Node *) lfirst(lc2), &cxt);
		}
	}
	else
	{
		appendStringInfoString(buf, "DELETE FROM ");
		trino_deparse_relation(buf, relation, fpinfo->opts);
	}
	deparse_conds(&cxt, remote_exprs, "WHERE");
	table_close(relation, NoLock);
}

/*
 * Human-readable description of a pushed-down join or aggregate for
 * EXPLAIN, e.g. "Aggregate on ((public.t1 a) INNER JOIN (public.t2 b))".
 */
static void
append_relation_name(StringInfo buf, PlannerInfo *root, RelOptInfo *rel)
{
	TrinoRelationInfo *fpinfo = (TrinoRelationInfo *) rel->fdw_private;

	if (IS_SIMPLE_REL(rel))
	{
		RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);
		char	   *namespace = get_namespace_name(get_rel_namespace(rte->relid));
		char	   *relname = get_rel_name(rte->relid);
		const char *refname = rte->eref->aliasname;

		appendStringInfo(buf, "%s.%s", quote_identifier(namespace),
						 quote_identifier(relname));
		if (strcmp(refname, relname) != 0)
			appendStringInfo(buf, " %s", quote_identifier(refname));
		return;
	}
	if (IS_UPPER_REL(rel))
	{
		appendStringInfoString(buf, "Aggregate on (");
		append_relation_name(buf, root, fpinfo->outerrel);
		appendStringInfoChar(buf, ')');
		return;
	}
	appendStringInfoChar(buf, '(');
	append_relation_name(buf, root, fpinfo->outerrel);
	appendStringInfo(buf, ") %s JOIN (", join_type_name(fpinfo->jointype));
	append_relation_name(buf, root, fpinfo->innerrel);
	appendStringInfoChar(buf, ')');
}

char *
trino_relation_name(PlannerInfo *root, RelOptInfo *rel)
{
	StringInfoData buf;

	initStringInfo(&buf);
	append_relation_name(&buf, root, rel);
	return buf.data;
}

/* INSERT INTO rel ("a", "b") VALUES  -- rows are appended by the caller */
void
trino_deparse_insert_prefix(StringInfo buf, Relation rel, TrinoOptions *opts,
							List *target_attrs)
{
	ListCell   *lc;
	bool		first = true;

	appendStringInfoString(buf, "INSERT INTO ");
	trino_deparse_relation(buf, rel, opts);
	appendStringInfoString(buf, " (");
	foreach(lc, target_attrs)
	{
		int			attnum = lfirst_int(lc);

		if (!first)
			appendStringInfoString(buf, ", ");
		first = false;
		trino_quote_identifier(buf, trino_get_column_name(RelationGetRelid(rel), attnum));
	}
	appendStringInfoString(buf, ") VALUES ");
}

void
trino_deparse_analyze_count(StringInfo buf, Relation rel, TrinoOptions *opts)
{
	appendStringInfoString(buf, "SELECT count(*) FROM ");
	trino_deparse_relation(buf, rel, opts);
}

void
trino_deparse_analyze_sample(StringInfo buf, Relation rel, TrinoOptions *opts,
							 double percent, List **retrieved_attrs)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	bool		first = true;
	int			i;

	*retrieved_attrs = NIL;
	appendStringInfoString(buf, "SELECT ");
	for (i = 1; i <= tupdesc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i - 1);

		if (attr->attisdropped)
			continue;
		if (!first)
			appendStringInfoString(buf, ", ");
		first = false;
		trino_quote_identifier(buf, trino_get_column_name(RelationGetRelid(rel), i));
		*retrieved_attrs = lappend_int(*retrieved_attrs, i);
	}
	if (first)
		appendStringInfoString(buf, "1");
	appendStringInfoString(buf, " FROM ");
	trino_deparse_relation(buf, rel, opts);
	if (percent < 100.0)
		appendStringInfo(buf, " TABLESAMPLE BERNOULLI (%.4f)", percent);
}
