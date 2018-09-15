/*-------------------------------------------------------------------------
 *
 * matview.c
 *	  materialized view support
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/matview.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/oid_dispatch.h"
#include "catalog/namespace.h"
#include "commands/cluster.h"
#include "commands/matview.h"
#include "commands/tablecmds.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "postmaster/autostats.h"
#include "rewrite/rewriteHandler.h"
#include "storage/smgr.h"
#include "tcop/tcopprot.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

//#include "cdb/cdbdisp_query.h"
#include "cdb/cdbvars.h"

typedef struct
{
	DestReceiver pub;			/* publicly-known function pointers */
	Oid			transientoid;	/* OID of new heap into which to store */
	Oid			matviewOid;		/* OID of old heap into which to store */
	/* These fields are filled by transientrel_startup: */
	Relation	transientrel;	/* relation to write to */
	CommandId	output_cid;		/* cmin to insert in output tuples */
	int			hi_options;		/* heap_insert performance options */
	BulkInsertState bistate;	/* bulk insert state */
} DR_transientrel;

static void ExecRefreshMatView_finish(DestReceiver *self);
static void transientrel_startup(DestReceiver *self, int operation, TupleDesc typeinfo);
static void transientrel_receive(TupleTableSlot *slot, DestReceiver *self);
static void transientrel_shutdown(DestReceiver *self);
static void transientrel_destroy(DestReceiver *self);
static void refresh_matview_datafill(DestReceiver *dest, Query *query,
						 const char *queryString, RefreshMatViewStmt *stmt);

/*
 * SetMatViewPopulatedState
 *		Mark a materialized view as populated, or not.
 *
 * NOTE: caller must be holding an appropriate lock on the relation.
 */
void
SetMatViewPopulatedState(Relation relation, bool newstate)
{
	Relation	pgrel;
	HeapTuple	tuple;

	Assert(relation->rd_rel->relkind == RELKIND_MATVIEW);

	/*
	 * Update relation's pg_class entry.  Crucial side-effect: other backends
	 * (and this one too!) are sent SI message to make them rebuild relcache
	 * entries.
	 */
	pgrel = heap_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID,
								ObjectIdGetDatum(RelationGetRelid(relation)));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u",
			 RelationGetRelid(relation));

	((Form_pg_class) GETSTRUCT(tuple))->relispopulated = newstate;

	simple_heap_update(pgrel, &tuple->t_self, tuple);

	CatalogUpdateIndexes(pgrel, tuple);

	heap_freetuple(tuple);
	heap_close(pgrel, RowExclusiveLock);

	/*
	 * Advance command counter to make the updated pg_class row locally
	 * visible.
	 */
	CommandCounterIncrement();
}

/*
 * ExecRefreshMatView -- execute a REFRESH MATERIALIZED VIEW command
 *
 * This refreshes the materialized view by creating a new table and swapping
 * the relfilenodes of the new table and the old materialized view, so the OID
 * of the original materialized view is preserved. Thus we do not lose GRANT
 * nor references to this materialized view.
 *
 * If WITH NO DATA was specified, this is effectively like a TRUNCATE;
 * otherwise it is like a TRUNCATE followed by an INSERT using the SELECT
 * statement associated with the materialized view.  The statement node's
 * skipData field shows whether the clause was used.
 *
 * Indexes are rebuilt too, via REINDEX. Since we are effectively bulk-loading
 * the new heap, it's better to create the indexes afterwards than to fill them
 * incrementally while we load.
 *
 * The matview's "populated" state is changed based on whether the contents
 * reflect the result set of the materialized view's query.
 * GPDB:
 * For QD we call it in ExecRefreshMatView() directly while for QE it is called
 * in InitPlan() to do preparation and data filling via new created dest.
 * We add a new parameter queryDesc here. For QD, it is not needed (NULL should
 * be set) but for QE it is needed since dest will be stored in it.
 */
void
_ExecRefreshMatView(RefreshMatViewStmt *stmt, const char *queryString,
				    ParamListInfo params, char *completionTag,
					QueryDesc *queryDesc)
{
	Oid			matviewOid;
	Relation	matviewRel;
	RewriteRule *rule;
	List	   *actions;
	Query	   *dataQuery;
	Oid			tableSpace;
	Oid			OIDNewHeap;
	DestReceiver *dest;

	/*
	 * Get a lock until end of transaction.
	 */
	matviewOid = RangeVarGetRelidExtended(stmt->relation,
										  AccessExclusiveLock, false, false,
										  RangeVarCallbackOwnsTable, NULL);
	matviewRel = heap_open(matviewOid, NoLock);

	/* Make sure it is a materialized view. */
	if (matviewRel->rd_rel->relkind != RELKIND_MATVIEW)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("\"%s\" is not a materialized view",
						RelationGetRelationName(matviewRel))));

	/*
	 * We're not using materialized views in the system catalogs.
	 */
	Assert(!IsSystemRelation(matviewRel));

	Assert(!matviewRel->rd_rel->relhasoids);

	/*
	 * Check that everything is correct for a refresh. Problems at this point
	 * are internal errors, so elog is sufficient.
	 */
	if (matviewRel->rd_rel->relhasrules == false ||
		matviewRel->rd_rules->numLocks < 1)
		elog(ERROR,
			 "materialized view \"%s\" is missing rewrite information",
			 RelationGetRelationName(matviewRel));

	if (matviewRel->rd_rules->numLocks > 1)
		elog(ERROR,
			 "materialized view \"%s\" has too many rules",
			 RelationGetRelationName(matviewRel));

	rule = matviewRel->rd_rules->rules[0];
	if (rule->event != CMD_SELECT || !(rule->isInstead))
		elog(ERROR,
			 "the rule for materialized view \"%s\" is not a SELECT INSTEAD OF rule",
			 RelationGetRelationName(matviewRel));

	actions = rule->actions;
	if (list_length(actions) != 1)
		elog(ERROR,
			 "the rule for materialized view \"%s\" is not a single action",
			 RelationGetRelationName(matviewRel));

	/*
	 * The stored query was rewritten at the time of the MV definition, but
	 * has not been scribbled on by the planner.
	 */
	dataQuery = (Query *) linitial(actions);
	Assert(IsA(dataQuery, Query));

	/*
	 * Check for active uses of the relation in the current transaction, such
	 * as open scans.
	 *
	 * NB: We count on this to protect us against problems with refreshing the
	 * data using HEAP_INSERT_FROZEN.
	 */
	CheckTableNotInUse(matviewRel, "REFRESH MATERIALIZED VIEW");

	/*
	 * Tentatively mark the matview as populated or not (this will roll back
	 * if we fail later).
	 */
	SetMatViewPopulatedState(matviewRel, !stmt->skipData);

	tableSpace = matviewRel->rd_rel->reltablespace;

	/* Create the transient table that will receive the regenerated data. */
	OIDNewHeap = make_new_heap(matviewOid, tableSpace, true /* GPDB_93_MERGE_FIXME Is true?*/);

	dest = CreateTransientRelDestReceiver(OIDNewHeap, matviewOid);

 	if (Gp_role == GP_ROLE_DISPATCH)
	{
		/* GPDB: We run it to dispatch the plan but without data filling when
		 * !stmt->skipData else we need to dispatch the utility stmt.
		 */
#if 0
		/* Generate the data, if wanted. */
		if (!stmt->skipData)
#endif
			refresh_matview_datafill(dest, dataQuery, queryString, stmt);
	}

	heap_close(matviewRel, NoLock); /* Put in ExecRefreshMatView_finish()? */

	if (queryDesc != NULL)
		queryDesc->dest = dest;
#if 0
	/* We won't let dest rShutdown() for this case. */
	if (stmt->skipData)
		ExecRefreshMatView_finish(dest);
#endif
}

static void
ExecRefreshMatView_finish(DestReceiver *self)
{
	DR_transientrel *myState = (DR_transientrel *) self;
	Oid			matviewOid = myState->matviewOid;
	Oid			OIDNewHeap = myState->transientoid;

	/*
	 * Swap the physical files of the target and transient tables, then
	 * rebuild the target's indexes and throw away the transient table.
	 */
	finish_heap_swap(matviewOid, OIDNewHeap, false, false, true, true, true,
					 RecentXmin, ReadNextMultiXactId());

	RelationCacheInvalidateEntry(matviewOid);
}

void
ExecRefreshMatView(RefreshMatViewStmt *stmt, const char *queryString,
				   ParamListInfo params, char *completionTag)
{
	_ExecRefreshMatView(stmt, queryString, params, completionTag,
							NULL /* QueryDesc * */);
#if 0
	/* No data filling, we need to dispatch the utility instead. */
 	if (Gp_role == GP_ROLE_DISPATCH && stmt->skipData)
	{
		/* No data filling, so just dispatch the utility simply. */
		CdbDispatchUtilityStatement((Node *) stmt,
									DF_CANCEL_ON_ERROR|
									DF_WITH_SNAPSHOT|
									DF_NEED_TWO_PHASE,
									GetAssignedOidsForDispatch(),
									NULL);
	}
#endif
}

/*
 * refresh_matview_datafill
 * GPDB: We add the stmt parameter since we want to dispatch it so that QE
 * could set up dest based on that via calling _ExecRefreshMatView() directly
 * in InitPlan() as mentioned before.
 */
static void
refresh_matview_datafill(DestReceiver *dest, Query *query,
						 const char *queryString, RefreshMatViewStmt *stmt)
{
	List	   *rewritten;
	PlannedStmt *plan;
	QueryDesc  *queryDesc;
	ScanDirection dir;
	Oid         relationOid = InvalidOid;   /* relation that is modified */
	AutoStatsCmdType cmdType = AUTOSTATS_CMDTYPE_SENTINEL;  /* command type */

	/* Rewrite, copying the given Query to make sure it's not changed */
	rewritten = QueryRewrite((Query *) copyObject(query));

	/* SELECT should never rewrite to more or less than one SELECT query */
	if (list_length(rewritten) != 1)
		elog(ERROR, "unexpected rewrite result for REFRESH MATERIALIZED VIEW");
	query = (Query *) linitial(rewritten);

	/* Check for user-requested abort. */
	CHECK_FOR_INTERRUPTS();

	/* Plan the query which will generate data for the refresh. */
	plan = pg_plan_query(query, 0, NULL);

	/* We need to dispatch RefreshMatView to set up dest on QEs. */
	plan->RefreshMatView = stmt;

	/*
	 * Use a snapshot with an updated command ID to ensure this query sees
	 * results of any previously executed queries.	(This could only matter if
	 * the planner executed an allegedly-stable function that changed the
	 * database contents, but let's do it anyway to be safe.)
	 */
	PushCopiedSnapshot(GetActiveSnapshot());
	UpdateActiveSnapshotCommandId();

	/* Create a QueryDesc, redirecting output to our tuple receiver */
	queryDesc = CreateQueryDesc(plan, queryString,
								GetActiveSnapshot(), InvalidSnapshot,
								dest, NULL, 0);

	/* call ExecutorStart to prepare the plan for execution */
	ExecutorStart(queryDesc, EXEC_FLAG_WITHOUT_OIDS);

	if (Gp_role == GP_ROLE_DISPATCH && !stmt->skipData)
		autostats_get_cmdtype(queryDesc, &cmdType, &relationOid);

	/*
	 * Normally, we run the plan to completion; but if skipData is specified,
	 * just do tuple receiver startup and shutdown.
	 */
	if (stmt->skipData)
		dir = NoMovementScanDirection;
	else
		dir = ForwardScanDirection;

	/* run the plan */
	ExecutorRun(queryDesc, dir, 0L);

	dest->rDestroy(dest);

	/* and clean up */
	ExecutorFinish(queryDesc);
	ExecutorEnd(queryDesc);

	if (Gp_role == GP_ROLE_DISPATCH && !stmt->skipData)
		auto_stats(cmdType, relationOid, queryDesc->es_processed, false /* inFunction */);

	FreeQueryDesc(queryDesc);

	PopActiveSnapshot();
}

DestReceiver *
CreateTransientRelDestReceiver(Oid transientoid, Oid matviewOid)
{
	DR_transientrel *self = (DR_transientrel *) palloc0(sizeof(DR_transientrel));

	self->pub.receiveSlot = transientrel_receive;
	self->pub.rStartup = transientrel_startup;
	self->pub.rShutdown = transientrel_shutdown;
	self->pub.rDestroy = transientrel_destroy;
	self->pub.mydest = DestTransientRel;
	self->transientoid = transientoid;
	self->matviewOid = matviewOid;

	return (DestReceiver *) self;
}

/*
 * transientrel_startup --- executor startup
 */
static void
transientrel_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	DR_transientrel *myState = (DR_transientrel *) self;
	Relation	transientrel;

	transientrel = heap_open(myState->transientoid, NoLock);

	/*
	 * Fill private fields of myState for use by later routines
	 */
	myState->transientrel = transientrel;
	myState->output_cid = GetCurrentCommandId(true);

	/*
	 * We can skip WAL-logging the insertions, unless PITR or streaming
	 * replication is in use. We can skip the FSM in any case.
	 */
	myState->hi_options = HEAP_INSERT_SKIP_FSM | HEAP_INSERT_FROZEN;
	if (!XLogIsNeeded())
		myState->hi_options |= HEAP_INSERT_SKIP_WAL;
	myState->bistate = GetBulkInsertState();

	/* Not using WAL requires smgr_targblock be initially invalid */
	Assert(RelationGetTargetBlock(transientrel) == InvalidBlockNumber);
}

/*
 * transientrel_receive --- receive one tuple
 */
static void
transientrel_receive(TupleTableSlot *slot, DestReceiver *self)
{
	DR_transientrel *myState = (DR_transientrel *) self;
	HeapTuple	tuple;

	/*
	 * get the heap tuple out of the tuple table slot, making sure we have a
	 * writable copy
	 */
	tuple = ExecMaterializeSlot(slot);

	heap_insert(myState->transientrel,
				tuple,
				myState->output_cid,
				myState->hi_options,
				myState->bistate,
				GetCurrentTransactionId());

	/* We know this is a newly created relation, so there are no indexes */
}

/*
 * transientrel_shutdown --- executor end
 */
static void
transientrel_shutdown(DestReceiver *self)
{
	DR_transientrel *myState = (DR_transientrel *) self;

	FreeBulkInsertState(myState->bistate);

	/* If we skipped using WAL, must heap_sync before commit */
	if (myState->hi_options & HEAP_INSERT_SKIP_WAL)
		heap_sync(myState->transientrel);

	/* close transientrel, but keep lock until commit */
	heap_close(myState->transientrel, NoLock);

	ExecRefreshMatView_finish(self);

	myState->transientrel = NULL;
}

/*
 * transientrel_destroy --- release DestReceiver object
 */
static void
transientrel_destroy(DestReceiver *self)
{
	pfree(self);
}
