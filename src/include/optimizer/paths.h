/*-------------------------------------------------------------------------
 *
 * paths.h
 *	  prototypes for various files in optimizer/path
 *
 *
<<<<<<< HEAD
 * Portions Copyright (c) 2005-2008, Greenplum inc
 * Portions Copyright (c) 2012-Present Pivotal Software, Inc.
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
=======
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
>>>>>>> e472b921406407794bab911c64655b8b82375196
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/paths.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PATHS_H
#define PATHS_H

#include "nodes/relation.h"


/*
 * allpaths.c
 */

/* Hook for plugins to replace standard_join_search() */
typedef RelOptInfo *(*join_search_hook_type) (PlannerInfo *root,
														  int levels_needed,
														  List *initial_rels);
extern PGDLLIMPORT join_search_hook_type join_search_hook;


extern RelOptInfo *make_one_rel(PlannerInfo *root, List *joinlist);
extern RelOptInfo *standard_join_search(PlannerInfo *root, int levels_needed,
					 List *initial_rels);

#ifdef OPTIMIZER_DEBUG
extern void debug_print_rel(PlannerInfo *root, RelOptInfo *rel);
#endif

/*
 * indxpath.c
 *	  routines to generate index paths
 */

extern void create_index_paths(PlannerInfo *root, RelOptInfo *rel);
extern List *generate_bitmap_or_paths(PlannerInfo *root, RelOptInfo *rel,
						 List *clauses, List *other_clauses,
						 bool restriction_only);
extern bool relation_has_unique_index_for(PlannerInfo *root, RelOptInfo *rel,
							  List *restrictlist,
							  List *exprlist, List *oprlist);
extern bool match_index_to_operand(Node *operand, int indexcol,
					   IndexOptInfo *index);
extern void expand_indexqual_conditions(IndexOptInfo *index,
							List *indexclauses, List *indexclausecols,
							List **indexquals_p, List **indexqualcols_p);
extern void check_partial_indexes(PlannerInfo *root, RelOptInfo *rel);
extern Expr *adjust_rowcompare_for_index(RowCompareExpr *clause,
							IndexOptInfo *index,
							int indexcol,
							List **indexcolnos,
							bool *var_on_left_p);

/*
 * orindxpath.c
 *	  additional routines for indexable OR clauses
 */
extern bool create_or_index_quals(PlannerInfo *root, RelOptInfo *rel);

/*
 * tidpath.h
 *	  routines to generate tid paths
 */
extern void create_tidscan_paths(PlannerInfo *root, RelOptInfo *rel);

/*
 * joinpath.c
 *	   routines to create join paths
 */
extern void add_paths_to_joinrel(PlannerInfo *root, RelOptInfo *joinrel,
					 RelOptInfo *outerrel, RelOptInfo *innerrel,
					 JoinType jointype, SpecialJoinInfo *sjinfo,
					 List *restrictlist);

/*
 * joinrels.c
 *	  routines to determine which relations to join
 */
extern void join_search_one_level(PlannerInfo *root, int level);
extern RelOptInfo *make_join_rel(PlannerInfo *root,
			  RelOptInfo *rel1, RelOptInfo *rel2);
extern bool have_join_order_restriction(PlannerInfo *root,
							RelOptInfo *rel1, RelOptInfo *rel2);

/*
 * equivclass.c
 *	  routines for managing EquivalenceClasses
 */
typedef bool (*ec_matches_callback_type) (PlannerInfo *root,
													  RelOptInfo *rel,
													  EquivalenceClass *ec,
													  EquivalenceMember *em,
													  void *arg);

extern bool process_equivalence(PlannerInfo *root, RestrictInfo *restrictinfo,
					bool below_outer_join);
extern Expr *canonicalize_ec_expression(Expr *expr,
						   Oid req_type, Oid req_collation);
extern void reconsider_outer_join_clauses(PlannerInfo *root);
extern EquivalenceClass *get_eclass_for_sort_expr(PlannerInfo *root,
						 Expr *expr,
						 List *opfamilies,
						 Oid opcintype,
						 Oid collation,
						 Index sortref,
						 Relids rel,
						 bool create_it);
extern void generate_base_implied_equalities(PlannerInfo *root);
extern List *generate_join_implied_equalities(PlannerInfo *root,
								 Relids join_relids,
								 Relids outer_relids,
								 RelOptInfo *inner_rel);
extern bool exprs_known_equal(PlannerInfo *root, Node *item1, Node *item2);
extern void add_child_rel_equivalences(PlannerInfo *root,
						   AppendRelInfo *appinfo,
						   RelOptInfo *parent_rel,
						   RelOptInfo *child_rel);
extern void mutate_eclass_expressions(PlannerInfo *root,
						  Node *(*mutator) (),
						  void *context,
						  bool include_child_exprs);
extern List *generate_implied_equalities_for_column(PlannerInfo *root,
									   RelOptInfo *rel,
									   ec_matches_callback_type callback,
									   void *callback_arg,
									   Relids prohibited_rels);
extern bool have_relevant_eclass_joinclause(PlannerInfo *root,
								RelOptInfo *rel1, RelOptInfo *rel2);
extern bool has_relevant_eclass_joinclause(PlannerInfo *root,
							   RelOptInfo *rel1);
extern bool eclass_useful_for_merging(EquivalenceClass *eclass,
						  RelOptInfo *rel);
extern bool is_redundant_derived_clause(RestrictInfo *rinfo, List *clauselist);

/*
 * pathkeys.c
 *	  utilities for matching and building path keys
 */
typedef enum
{
	PATHKEYS_EQUAL,				/* pathkeys are identical */
	PATHKEYS_BETTER1,			/* pathkey 1 is a superset of pathkey 2 */
	PATHKEYS_BETTER2,			/* vice versa */
	PATHKEYS_DIFFERENT			/* neither pathkey includes the other */
} PathKeysComparison;

<<<<<<< HEAD
typedef struct
{
	Node *replaceThis;
	Node *withThis;
	int numReplacementsDone;
} ReplaceExpressionMutatorReplacement;

extern PathKey *makePathKey(EquivalenceClass *eclass, Oid opfamily,
							int strategy, bool nulls_first);

extern Node * replace_expression_mutator(Node *node, void *context);
extern void generate_implied_quals(PlannerInfo *root);

extern List *canonicalize_pathkeys(PlannerInfo *root, List *pathkeys);
=======
>>>>>>> e472b921406407794bab911c64655b8b82375196
extern PathKeysComparison compare_pathkeys(List *keys1, List *keys2);
extern bool pathkeys_contained_in(List *keys1, List *keys2);
extern Path *get_cheapest_path_for_pathkeys(List *paths, List *pathkeys,
							   Relids required_outer,
							   CostSelector cost_criterion);
extern Path *get_cheapest_fractional_path_for_pathkeys(List *paths,
										  List *pathkeys,
										  Relids required_outer,
										  double fraction);
extern List *build_index_pathkeys(PlannerInfo *root, IndexOptInfo *index,
					 ScanDirection scandir);

Var *
find_indexkey_var(PlannerInfo *root, RelOptInfo *rel, AttrNumber varattno);

extern List *convert_subquery_pathkeys(PlannerInfo *root, RelOptInfo *rel,
						  List *subquery_pathkeys);
extern List *build_join_pathkeys(PlannerInfo *root,
					RelOptInfo *joinrel,
					JoinType jointype,
					List *outer_pathkeys);

PathKey *
cdb_make_pathkey_for_expr(PlannerInfo  *root,
                          Node     *expr,
                          List     *eqopname,
						  bool		canonical);
PathKey *
cdb_pull_up_pathkey(PlannerInfo    *root,
                    PathKey        *pathkey,
                    Relids          relids,
                    List           *targetlist,
                    List           *newvarlist,
                    Index           newrelid);

extern List *make_pathkeys_for_groupclause(PlannerInfo *root,
										   List *groupclause,
										   List *tlist);
extern List *make_pathkeys_for_sortclauses(PlannerInfo *root,
							  List *sortclauses,
<<<<<<< HEAD
							  List *tlist,
							  bool canonicalize);
extern void make_distribution_keys_for_groupclause(PlannerInfo *root, List *groupclause, List *tlist,
									   List **partition_dist_keys,
									   List **partition_dist_exprs);
=======
							  List *tlist);
>>>>>>> e472b921406407794bab911c64655b8b82375196
extern void initialize_mergeclause_eclasses(PlannerInfo *root,
								RestrictInfo *restrictinfo);
extern void update_mergeclause_eclasses(PlannerInfo *root,
							RestrictInfo *restrictinfo);
extern List *find_mergeclauses_for_pathkeys(PlannerInfo *root,
							   List *pathkeys,
							   bool outer_keys,
							   List *restrictinfos);
extern List *select_outer_pathkeys_for_merge(PlannerInfo *root,
								List *mergeclauses,
								RelOptInfo *joinrel);
extern List *make_inner_pathkeys_for_merge(PlannerInfo *root,
							  List *mergeclauses,
							  List *outer_pathkeys);
extern List *truncate_useless_pathkeys(PlannerInfo *root,
						  RelOptInfo *rel,
						  List *pathkeys);
extern bool has_useful_pathkeys(PlannerInfo *root, RelOptInfo *rel);

#endif   /* PATHS_H */
