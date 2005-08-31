/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2004-2005 The Trustees of the University of Tennessee.
 *                         All rights reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart, 
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef MCA_COLL_TUNED_TOPO_H_HAS_BEEN_INCLUDED
#define MCA_COLL_TUNED_TOPO_H_HAS_BEEN_INCLUDED

#include "ompi_config.h"

#if defined(c_plusplus) || defined(__cplusplus)
extern "C"
{
#endif

#define MAXTREEFANOUT 32

typedef struct ompi_coll_tree_t {
    int32_t tree_root;
    int32_t tree_fanout;
    int32_t tree_prev;
    int32_t tree_next[MAXTREEFANOUT];
    int32_t tree_nextsize;
} ompi_coll_tree_t;

typedef struct ompi_coll_bmtree_t {
    int32_t bmtree_root;
    int32_t bmtree_prev;
    int32_t bmtree_next[MAXTREEFANOUT];
    int32_t bmtree_nextsize;
} ompi_coll_bmtree_t;

typedef struct ompi_coll_chain_t {
    int32_t chain_root;
    int32_t chain_prev;
    int32_t chain_next[MAXTREEFANOUT];
    int32_t chain_nextsize;
    int32_t chain_numchain;
} ompi_coll_chain_t;

ompi_coll_tree_t*
ompi_coll_tuned_topo_build_tree( int fanout,
                             struct ompi_communicator_t* com,
                             int root );
int ompi_coll_tuned_topo_destroy_tree( ompi_coll_tree_t** tree );

ompi_coll_bmtree_t*
ompi_coll_tuned_topo_build_bmtree( struct ompi_communicator_t* comm,
                        int root );
int ompi_coll_tuned_topo_destroy_bmtree( ompi_coll_bmtree_t** bmtree );

ompi_coll_chain_t*
ompi_coll_tuned_topo_build_chain( int fanout,
                       struct ompi_communicator_t* com,
                       int root );
int ompi_coll_tuned_topo_destroy_chain( ompi_coll_chain_t** chain );

#if defined(c_plusplus) || defined(__cplusplus)
}
#endif

#endif  /* MCA_COLL_TUNED_TOPO_H_HAS_BEEN_INCLUDED */

