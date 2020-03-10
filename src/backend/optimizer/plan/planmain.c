/*-------------------------------------------------------------------------
 *
 * planmain.c
 *	  Routines to plan a single query
 *
 * What's in a name, anyway?  The top-level entry point of the planner/
 * optimizer is over in planner.c, not here as you might think from the
 * file name.  But this is the main code for planning a basic join operation,
 * shorn of features like subselects, inheritance, aggregates, grouping,
 * and so on.  (Those are the things planner.c deals with.)
 *
 * Portions Copyright (c) 2005-2008, Greenplum inc
 * Portions Copyright (c) 2012-Present Pivotal Software, Inc.
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/planmain.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/inherit.h"
#include "optimizer/optimizer.h"
#include "optimizer/orclauses.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/placeholder.h"
#include "optimizer/planmain.h"

#include "catalog/pg_proc.h"
#include "cdb/cdbpath.h"        /* cdbpath_rows() */
#include "cdb/cdbutil.h"
#include "cdb/cdbvars.h"
#include "optimizer/cost.h"


/*
 * query_planner
 *	  Generate a path (that is, a simplified plan) for a basic query,
 *	  which may involve joins but not any fancier features.
 *
 * Since query_planner does not handle the toplevel processing (grouping,
 * sorting, etc) it cannot select the best path by itself.  Instead, it
 * returns the RelOptInfo for the top level of joining, and the caller
 * (grouping_planner) can choose among the surviving paths for the rel.
 *
 * root describes the query to plan
 * qp_callback is a function to compute query_pathkeys once it's safe to do so
 * qp_extra is optional extra data to pass to qp_callback
 *
 * Note: the PlannerInfo node also includes a query_pathkeys field, which
 * tells query_planner the sort order that is desired in the final output
 * plan.  This value is *not* available at call time, but is computed by
 * qp_callback once we have completed merging the query's equivalence classes.
 * (We cannot construct canonical pathkeys until that's done.)
 */
RelOptInfo *
query_planner(PlannerInfo *root,
			  query_pathkeys_callback qp_callback, void *qp_extra)
{
	Query	   *parse = root->parse;
	List	   *joinlist;
	RelOptInfo *final_rel;

	/*
	 * Init planner lists to empty.
	 *
	 * NOTE: append_rel_list was set up by subquery_planner, so do not touch
	 * here.
	 */
	root->join_rel_list = NIL;
	root->join_rel_hash = NULL;
	root->join_rel_level = NULL;
	root->join_cur_level = 0;
	root->canon_pathkeys = NIL;
	root->left_join_clauses = NIL;
	root->right_join_clauses = NIL;
	root->full_join_clauses = NIL;
	root->join_info_list = NIL;
	root->placeholder_list = NIL;
	root->fkey_list = NIL;
	root->initial_rels = NIL;

	/*
	 * Make a flattened version of the rangetable for faster access (this is
	 * OK because the rangetable won't change any more), and set up an empty
	 * array for indexing base relations.
	 */
	setup_simple_rel_arrays(root);

	/*
	 * In the trivial case where the jointree is a single RTE_RESULT relation,
	 * bypass all the rest of this function and just make a RelOptInfo and its
	 * one access path.  This is worth optimizing because it applies for
	 * common cases like "SELECT expression" and "INSERT ... VALUES()".
	 */
	Assert(parse->jointree->fromlist != NIL);
	if (list_length(parse->jointree->fromlist) == 1)
	{
		Node	   *jtnode = (Node *) linitial(parse->jointree->fromlist);

		if (IsA(jtnode, RangeTblRef))
		{
			int			varno = ((RangeTblRef *) jtnode)->rtindex;
			RangeTblEntry *rte = root->simple_rte_array[varno];

			Assert(rte != NULL);
			if (rte->rtekind == RTE_RESULT)
			{
				/* Make the RelOptInfo for it directly */
				final_rel = build_simple_rel(root, varno, NULL);

				/*
				 * If query allows parallelism in general, check whether the
				 * quals are parallel-restricted.  (We need not check
				 * final_rel->reltarget because it's empty at this point.
				 * Anything parallel-restricted in the query tlist will be
				 * dealt with later.)  This is normally pretty silly, because
				 * a Result-only plan would never be interesting to
				 * parallelize.  However, if force_parallel_mode is on, then
				 * we want to execute the Result in a parallel worker if
				 * possible, so we must do this.
				 */
				if (root->glob->parallelModeOK &&
					force_parallel_mode != FORCE_PARALLEL_OFF)
					final_rel->consider_parallel =
						is_parallel_safe(root, parse->jointree->quals);

				/*
				 * The only path for it is a trivial Result path.  We cheat a
				 * bit here by using a GroupResultPath, because that way we
				 * can just jam the quals into it without preprocessing them.
				 * (But, if you hold your head at the right angle, a FROM-less
				 * SELECT is a kind of degenerate-grouping case, so it's not
				 * that much of a cheat.)
				 */
				Path *result_path = (Path *)
						 create_group_result_path(root, final_rel,
												  final_rel->reltarget,
												  (List *) parse->jointree->quals);
				add_path(final_rel, result_path);

				/* Select cheapest path (pretty easy in this case...) */
				set_cheapest(final_rel);

				/*
				 * We still are required to call qp_callback, in case it's
				 * something like "SELECT 2+2 ORDER BY 1".
				 */
				(*qp_callback) (root, qp_extra);

				if (Gp_role == GP_ROLE_DISPATCH)
				{
					char		exec_location;

					exec_location = check_execute_on_functions((Node *) parse->targetList);

					if (exec_location == PROEXECLOCATION_MASTER || exec_location == PROEXECLOCATION_INITPLAN)
						CdbPathLocus_MakeEntry(&result_path->locus);
					else if (exec_location == PROEXECLOCATION_ALL_SEGMENTS)
						CdbPathLocus_MakeStrewn(&result_path->locus,
												getgpsegmentCount());
				}
				else
					CdbPathLocus_MakeEntry(&result_path->locus);

				return final_rel;
			}
		}
	}

	/*
	 * Populate append_rel_array with each AppendRelInfo to allow direct
	 * lookups by child relid.
	 */
	setup_append_rel_array(root);

	/*
	 * Construct RelOptInfo nodes for all base relations used in the query.
	 * Appendrel member relations ("other rels") will be added later.
	 *
	 * Note: the reason we find the baserels by searching the jointree, rather
	 * than scanning the rangetable, is that the rangetable may contain RTEs
	 * for rels not actively part of the query, for example views.  We don't
	 * want to make RelOptInfos for them.
	 */
	add_base_rels_to_query(root, (Node *) parse->jointree);

	/*
	 * Examine the targetlist and join tree, adding entries to baserel
	 * targetlists for all referenced Vars, and generating PlaceHolderInfo
	 * entries for all referenced PlaceHolderVars.  Restrict and join clauses
	 * are added to appropriate lists belonging to the mentioned relations. We
	 * also build EquivalenceClasses for provably equivalent expressions. The
	 * SpecialJoinInfo list is also built to hold information about join order
	 * restrictions.  Finally, we form a target joinlist for make_one_rel() to
	 * work from.
	 */
	build_base_rel_tlists(root, root->processed_tlist);

	make_placeholders_for_subplans(root);

	find_placeholders_in_jointree(root);

	find_lateral_references(root);

	joinlist = deconstruct_jointree(root);

	/*
	 * Reconsider any postponed outer-join quals now that we have built up
	 * equivalence classes.  (This could result in further additions or
	 * mergings of classes.)
	 */
	reconsider_outer_join_clauses(root);

	/**
	 * Use the list of equijoined keys to transfer quals between relations.  For example,
	 *   A=B AND f(A) implies A=B AND f(A) and f(B), under some restrictions on f.
	 */
	generate_implied_quals(root);

	/*
	 * If we formed any equivalence classes, generate additional restriction
	 * clauses as appropriate.  (Implied join clauses are formed on-the-fly
	 * later.)
	 */
	generate_base_implied_equalities(root);

	/*
	 * We have completed merging equivalence sets, so it's now possible to
	 * generate pathkeys in canonical form; so compute query_pathkeys and
	 * other pathkeys fields in PlannerInfo.
	 */
	(*qp_callback) (root, qp_extra);

	/*
	 * Examine any "placeholder" expressions generated during subquery pullup.
	 * Make sure that the Vars they need are marked as needed at the relevant
	 * join level.  This must be done before join removal because it might
	 * cause Vars or placeholders to be needed above a join when they weren't
	 * so marked before.
	 */
	fix_placeholder_input_needed_levels(root);

	/*
	 * Remove any useless outer joins.  Ideally this would be done during
	 * jointree preprocessing, but the necessary information isn't available
	 * until we've built baserel data structures and classified qual clauses.
	 */
	joinlist = remove_useless_joins(root, joinlist);

	/*
	 * Also, reduce any semijoins with unique inner rels to plain inner joins.
	 * Likewise, this can't be done until now for lack of needed info.
	 */
	reduce_unique_semijoins(root);

	/*
	 * Now distribute "placeholders" to base rels as needed.  This has to be
	 * done after join removal because removal could change whether a
	 * placeholder is evaluable at a base rel.
	 */
	add_placeholders_to_base_rels(root);

	/*
	 * Construct the lateral reference sets now that we have finalized
	 * PlaceHolderVar eval levels.
	 */
	create_lateral_join_info(root);

	/*
	 * Match foreign keys to equivalence classes and join quals.  This must be
	 * done after finalizing equivalence classes, and it's useful to wait till
	 * after join removal so that we can skip processing foreign keys
	 * involving removed relations.
	 */
	match_foreign_keys_to_quals(root);

	/*
	 * Look for join OR clauses that we can extract single-relation
	 * restriction OR clauses from.
	 */
	extract_restriction_or_clauses(root);

	/*
	 * Now expand appendrels by adding "otherrels" for their children.  We
	 * delay this to the end so that we have as much information as possible
	 * available for each baserel, including all restriction clauses.  That
	 * let us prune away partitions that don't satisfy a restriction clause.
	 * Also note that some information such as lateral_relids is propagated
	 * from baserels to otherrels here, so we must have computed it already.
	 */
	add_other_rels_to_query(root);

	/*
	 * Ready to do the primary planning.
	 */
	final_rel = make_one_rel(root, joinlist);

	/* Modify paths to support unique rowid operation for subquery preds. */
	/* GPDB_12_MERGE_FIXME: we used to do this in create_plan(), but I think
	 * this is a more appropriate place. Moving it here means that we're doing
	 * this before sorting and other "upper planner" stuff. Extract this to
	 * a separate PR for master?
	 */
	if (root->join_info_list)
	{
		ListCell  *lc;

		foreach (lc, final_rel->pathlist)
		{
			Path	   *path = (Path *) lfirst(lc);

			cdbpath_dedup_fixup(root, path);
		}
		foreach (lc, final_rel->partial_pathlist)
		{
			Path	   *path = (Path *) lfirst(lc);

			cdbpath_dedup_fixup(root, path);
		}
	}

	/* Check that we got at least one usable path */
	if (!final_rel || !final_rel->cheapest_total_path ||
		final_rel->cheapest_total_path->param_info != NULL)
		elog(ERROR, "failed to construct the join relation");
	Insist(final_rel->cheapest_startup_path);

	return final_rel;
}

/**
 * Planner configuration related
 */

/**
 * Default configuration information
 */
PlannerConfig *DefaultPlannerConfig(void)
{
	PlannerConfig *c1 = (PlannerConfig *) palloc(sizeof(PlannerConfig));

	c1->gp_enable_minmax_optimization = gp_enable_minmax_optimization;
	c1->gp_enable_multiphase_agg = gp_enable_multiphase_agg;
	c1->gp_enable_direct_dispatch = gp_enable_direct_dispatch;

	c1->gp_cte_sharing = gp_cte_sharing;

	c1->honor_order_by = true;

	c1->is_under_subplan = false;

	c1->force_singleQE = false;

	return c1;
}

/*
 * Copy configuration information
 */
PlannerConfig *
CopyPlannerConfig(const PlannerConfig *c1)
{
	PlannerConfig *c2 = (PlannerConfig *) palloc(sizeof(PlannerConfig));

	memcpy(c2, c1, sizeof(PlannerConfig));
	return c2;
}
