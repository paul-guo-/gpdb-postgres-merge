/*-------------------------------------------------------------------------
 *
 * catalog.h
 *	  prototypes for functions in backend/catalog/catalog.c
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/catalog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CATALOG_H
#define CATALOG_H

#include "catalog/pg_class.h"
#include "storage/relfilenode.h"
#include "utils/relcache.h"

<<<<<<< HEAD
#include "catalog/oid_dispatch.h"

#define OIDCHARS		10		/* max chars printed by %u */
/*
 * In PostgreSQL, this is called just TABLESPACE_VERSION_DIRECTORY. But in 
 * GPDB, you should use tablespace_version_directory() function instead.
 * This constant has been renamed so that we catch and know to modify all
 * upstream uses of TABLESPACE_VERSION_DIRECTORY.
 */
#define GP_TABLESPACE_VERSION_DIRECTORY	"GPDB_" GP_MAJORVERSION "_" \
									CppAsString2(CATALOG_VERSION_NO)

extern const char *forkNames[];
=======
>>>>>>> e472b921406407794bab911c64655b8b82375196
extern ForkNumber forkname_to_number(char *forkName);

extern char *GetDatabasePath(Oid dbNode, Oid spcNode);

<<<<<<< HEAD
extern void reldir_and_filename(RelFileNode rnode, BackendId backend, ForkNumber forknum,
					char **dir, char **filename);
extern char *aorelpathbackend(RelFileNode node, BackendId backend, int32 segno);

/* First argument is a RelFileNodeBackend */
#define relpath(rnode, forknum) \
		relpathbackend((rnode).node, (rnode).backend, (forknum))

/* First argument is a RelFileNode */
#define relpathperm(rnode, forknum) \
		relpathbackend((rnode), InvalidBackendId, (forknum))
=======
>>>>>>> e472b921406407794bab911c64655b8b82375196

#define aorelpath(rnode, segno) \
		aorelpathbackend((rnode).node, (rnode).backend, (segno))

extern bool IsSystemRelation(Relation relation);
extern bool IsToastRelation(Relation relation);

extern bool IsSystemClass(Form_pg_class reltuple);
extern bool IsToastClass(Form_pg_class reltuple);

extern bool IsSystemNamespace(Oid namespaceId);
extern bool IsToastNamespace(Oid namespaceId);
extern bool IsAoSegmentNamespace(Oid namespaceId);

extern bool IsReservedName(const char *name);
extern char* GetReservedPrefix(const char *name);

extern bool IsSharedRelation(Oid relationId);

extern Oid GetNewOid(Relation relation);
extern Oid GetNewOidWithIndex(Relation relation, Oid indexId,
				   AttrNumber oidcolumn);
extern Oid GetNewRelFileNode(Oid reltablespace, Relation pg_class,
				  char relpersistence);

const char *tablespace_version_directory(void);

#endif   /* CATALOG_H */
