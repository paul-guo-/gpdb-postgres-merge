/*-------------------------------------------------------------------------
 *
 * aggregatecmds.c
 *
 *	  Routines for aggregate-manipulation commands
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/aggregatecmds.c,v 1.50 2009/10/08 02:39:18 tgl Exp $
 *
 * DESCRIPTION
 *	  The "DefineFoo" routines take the parse tree and pick out the
 *	  appropriate arguments/flags, passing the results to the
 *	  corresponding "FooDefine" routines (in src/catalog) that do
 *	  the actual catalog-munging.  These routines also verify permission
 *	  of the user to execute the command.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/oid_dispatch.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "cdb/cdbvars.h"
#include "cdb/cdbdisp_query.h"

/*
 *	DefineAggregate
 *
 * "oldstyle" signals the old (pre-8.2) style where the aggregate input type
 * is specified by a BASETYPE element in the parameters.  Otherwise,
 * "args" is a pair, whose first element is a list of FunctionParameter structs
 * defining the agg's arguments (both direct and aggregated), and whose second
 * element is an Integer node with the number of direct args, or -1 if this
 * isn't an ordered-set aggregate.
 * "parameters" is a list of DefElem representing the agg's definition clauses.
 */
void
DefineAggregate(List *name, List *args, bool oldstyle, List *parameters,
				bool ordered, const char *queryString)
{
	char	   *aggName;
	Oid			aggNamespace;
	AclResult	aclresult;
	char		aggKind = AGGKIND_NORMAL;
	List	   *transfuncName = NIL;
	List	   *prelimfuncName = NIL; /* MPP */
	List	   *finalfuncName = NIL;
	bool		finalfuncExtraArgs = false;
	List	   *sortoperatorName = NIL;
	TypeName   *baseType = NULL;
	TypeName   *transType = NULL;
	char	   *initval = NULL;
	int			numArgs;
	int			numDirectArgs = 0;
	oidvector  *parameterTypes;
	ArrayType  *allParameterTypes;
	ArrayType  *parameterModes;
	ArrayType  *parameterNames;
	List	   *parameterDefaults;
	Oid			variadicArgType;
	Oid			transTypeId;
	ListCell   *pl;
	List	   *orig_args = args;

	/* Convert list of names to a name and namespace */
	aggNamespace = QualifiedNameGetCreationNamespace(name, &aggName);

	/* Check we have creation rights in target namespace */
	aclresult = pg_namespace_aclcheck(aggNamespace, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
					   get_namespace_name(aggNamespace));

	/* Deconstruct the output of the aggr_args grammar production */
	if (!oldstyle)
	{
		Assert(list_length(args) == 2);
		numDirectArgs = intVal(lsecond(args));
		if (numDirectArgs >= 0)
			aggKind = AGGKIND_ORDERED_SET;
		else
			numDirectArgs = 0;
		args = (List *) linitial(args);
	}

	/* Examine aggregate's definition clauses */
	foreach(pl, parameters)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);

		/*
		 * sfunc1, stype1, and initcond1 are accepted as obsolete spellings
		 * for sfunc, stype, initcond.
		 */
		if (pg_strcasecmp(defel->defname, "sfunc") == 0)
			transfuncName = defGetQualifiedName(defel);
		else if (pg_strcasecmp(defel->defname, "sfunc1") == 0)
			transfuncName = defGetQualifiedName(defel);
		else if (pg_strcasecmp(defel->defname, "finalfunc") == 0)
			finalfuncName = defGetQualifiedName(defel);
		else if (pg_strcasecmp(defel->defname, "finalfunc_extra") == 0)
			finalfuncExtraArgs = defGetBoolean(defel);
		else if (pg_strcasecmp(defel->defname, "sortop") == 0)
			sortoperatorName = defGetQualifiedName(defel);
		else if (pg_strcasecmp(defel->defname, "basetype") == 0)
			baseType = defGetTypeName(defel);
		else if (pg_strcasecmp(defel->defname, "hypothetical") == 0)
		{
			if (defGetBoolean(defel))
			{
				if (aggKind == AGGKIND_NORMAL)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
							 errmsg("only ordered-set aggregates can be hypothetical")));
				aggKind = AGGKIND_HYPOTHETICAL;
			}
		}
		else if (pg_strcasecmp(defel->defname, "stype") == 0)
			transType = defGetTypeName(defel);
		else if (pg_strcasecmp(defel->defname, "stype1") == 0)
			transType = defGetTypeName(defel);
		else if (pg_strcasecmp(defel->defname, "initcond") == 0)
			initval = defGetString(defel);
		else if (pg_strcasecmp(defel->defname, "initcond1") == 0)
			initval = defGetString(defel);
		else if (pg_strcasecmp(defel->defname, "prefunc") == 0) /* MPP */
			prelimfuncName = defGetQualifiedName(defel);
		else
			ereport(WARNING,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("aggregate attribute \"%s\" not recognized",
							defel->defname)));
	}

	/*
	 * make sure we have our required definitions
	 */
	if (transType == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("aggregate stype must be specified")));
	if (transfuncName == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("aggregate sfunc must be specified")));

	/*
	 * MPP: Ordered aggregates do not support prefuncs
	 */
	if (aggKind == AGGKIND_ORDERED_SET && prelimfuncName != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("ordered aggregate prefunc is not supported")));

	/*
	 * look up the aggregate's input datatype(s).
	 */
	if (oldstyle)
	{
		/*
		 * Old style: use basetype parameter.  This supports aggregates of
		 * zero or one input, with input type ANY meaning zero inputs.
		 *
		 * Historically we allowed the command to look like basetype = 'ANY'
		 * so we must do a case-insensitive comparison for the name ANY. Ugh.
		 */
		Oid			aggArgTypes[1];

		if (baseType == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate input type must be specified")));

		if (pg_strcasecmp(TypeNameToString(baseType), "ANY") == 0)
		{
			numArgs = 0;
			aggArgTypes[0] = InvalidOid;
		}
		else
		{
			numArgs = 1;
			aggArgTypes[0] = typenameTypeId(NULL, baseType, NULL);
		}
		parameterTypes = buildoidvector(aggArgTypes, numArgs);
		allParameterTypes = NULL;
		parameterModes = NULL;
		parameterNames = NULL;
		parameterDefaults = NIL;
		variadicArgType = InvalidOid;
	}
	else
	{
		/*
		 * New style: args is a list of FunctionParameters (possibly zero of
		 * 'em).  We share functioncmds.c's code for processing them.
		 */
		Oid			requiredResultType;

		if (baseType != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("basetype is redundant with aggregate input type specification")));

		numArgs = list_length(args);
		interpret_function_parameter_list(args,
										  InvalidOid,
										  true, /* is an aggregate */
										  queryString,
										  &parameterTypes,
										  &allParameterTypes,
										  &parameterModes,
										  &parameterNames,
										  &parameterDefaults,
										  &variadicArgType,
										  &requiredResultType);
		/* Parameter defaults are not currently allowed by the grammar */
		Assert(parameterDefaults == NIL);
		/* There shouldn't have been any OUT parameters, either */
		Assert(requiredResultType == InvalidOid);
	}

	/*
	 * look up the aggregate's transtype.
	 *
	 * transtype can't be a pseudo-type, since we need to be able to store
	 * values of the transtype.  However, we can allow polymorphic transtype
	 * in some cases (AggregateCreate will check).	Also, we allow "internal"
	 * for functions that want to pass pointers to private data structures;
	 * but allow that only to superusers, since you could crash the system (or
	 * worse) by connecting up incompatible internal-using functions in an
	 * aggregate.
	 */
	transTypeId = typenameTypeId(NULL, transType, NULL);
	if (get_typtype(transTypeId) == TYPTYPE_PSEUDO &&
		!IsPolymorphicType(transTypeId))
	{
		if (transTypeId == INTERNALOID && superuser())
			 /* okay */ ;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate transition data type cannot be %s",
							format_type_be(transTypeId))));
	}

	/*
	 * Most of the argument-checking is done inside of AggregateCreate
	 */
	AggregateCreate(aggName,	/* aggregate name */
					aggNamespace,		/* namespace */
					aggKind,
					numArgs,
					numDirectArgs,
					parameterTypes,
					PointerGetDatum(allParameterTypes),
					PointerGetDatum(parameterModes),
					PointerGetDatum(parameterNames),
					parameterDefaults,
					variadicArgType,
					transfuncName,		/* step function name */
					prelimfuncName,		/* prelim function name */
					finalfuncName,		/* final function name */
					finalfuncExtraArgs,
					sortoperatorName,	/* sort operator name */
					transTypeId,	/* transition data type */
					initval);		/* initial condition */

	if (Gp_role == GP_ROLE_DISPATCH)
	{
		DefineStmt * stmt = makeNode(DefineStmt);
		stmt->kind = OBJECT_AGGREGATE;
		stmt->oldstyle = oldstyle;  
		stmt->defnames = name;
		stmt->args = orig_args;
		stmt->definition = parameters;
		CdbDispatchUtilityStatement((Node *) stmt,
									DF_CANCEL_ON_ERROR|
									DF_WITH_SNAPSHOT|
									DF_NEED_TWO_PHASE,
									GetAssignedOidsForDispatch(),
									NULL);
	}
}


/*
 * RemoveAggregate
 *		Deletes an aggregate.
 */
void
RemoveAggregate(RemoveFuncStmt *stmt)
{
	List	   *aggName = stmt->name;
	List	   *aggArgs = stmt->args;
	Oid			procOid;
	HeapTuple	tup;
	ObjectAddress object;

	/* Look up function and make sure it's an aggregate */
	procOid = LookupAggNameTypeNames(aggName, aggArgs, stmt->missing_ok);

	if (!OidIsValid(procOid))
	{
		/* we only get here if stmt->missing_ok is true */
		ereport(NOTICE,
				(errmsg("aggregate %s(%s) does not exist, skipping",
						NameListToString(aggName),
						TypeNameListToString(aggArgs))));
		return;
	}

	/*
	 * Find the function tuple, do permissions and validity checks
	 */
	tup = SearchSysCache(PROCOID,
						 ObjectIdGetDatum(procOid),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for function %u", procOid);

	/* Permission check: must own agg or its namespace */
	if (!pg_proc_ownercheck(procOid, GetUserId()) &&
	  !pg_namespace_ownercheck(((Form_pg_proc) GETSTRUCT(tup))->pronamespace,
							   GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_PROC,
					   NameListToString(aggName));

	ReleaseSysCache(tup);

	/*
	 * Do the deletion
	 */
	object.classId = ProcedureRelationId;
	object.objectId = procOid;
	object.objectSubId = 0;

	performDeletion(&object, stmt->behavior);
	
	if (Gp_role == GP_ROLE_DISPATCH)
	{
		CdbDispatchUtilityStatement((Node *) stmt,
									DF_CANCEL_ON_ERROR|
									DF_WITH_SNAPSHOT|
									DF_NEED_TWO_PHASE,
									NIL,
									NULL);
	}
}


void
RenameAggregate(List *name, List *args, const char *newname)
{
	Oid			procOid;
	Oid			namespaceOid;
	HeapTuple	tup;
	Form_pg_proc procForm;
	Relation	rel;
	AclResult	aclresult;

	rel = heap_open(ProcedureRelationId, RowExclusiveLock);

	/* Look up function and make sure it's an aggregate */
	procOid = LookupAggNameTypeNames(name, args, false);

	tup = SearchSysCacheCopy(PROCOID,
							 ObjectIdGetDatum(procOid),
							 0, 0, 0);
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for function %u", procOid);
	procForm = (Form_pg_proc) GETSTRUCT(tup);

	namespaceOid = procForm->pronamespace;

	/* make sure the new name doesn't exist */
	if (SearchSysCacheExists(PROCNAMEARGSNSP,
							 CStringGetDatum(newname),
							 PointerGetDatum(&procForm->proargtypes),
							 ObjectIdGetDatum(namespaceOid),
							 0))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_FUNCTION),
				 errmsg("function %s already exists in schema \"%s\"",
						funcname_signature_string(newname,
												  procForm->pronargs,
												  NIL,
											   procForm->proargtypes.values),
						get_namespace_name(namespaceOid))));

	/* must be owner */
	if (!pg_proc_ownercheck(procOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_PROC,
					   NameListToString(name));

	/* must have CREATE privilege on namespace */
	aclresult = pg_namespace_aclcheck(namespaceOid, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
					   get_namespace_name(namespaceOid));

	/* rename */
	namestrcpy(&(((Form_pg_proc) GETSTRUCT(tup))->proname), newname);
	simple_heap_update(rel, &tup->t_self, tup);
	CatalogUpdateIndexes(rel, tup);

	heap_close(rel, NoLock);
	heap_freetuple(tup);
}

/*
 * Change aggregate owner
 */
void
AlterAggregateOwner(List *name, List *args, Oid newOwnerId)
{
	Oid			procOid;

	/* Look up function and make sure it's an aggregate */
	procOid = LookupAggNameTypeNames(name, args, false);

	/* The rest is just like a function */
	AlterFunctionOwner_oid(procOid, newOwnerId);
}
