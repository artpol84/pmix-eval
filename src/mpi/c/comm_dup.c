/*
 * $HEADERS$
 */
#include "ompi_config.h"
#include <stdio.h>

#include "mpi.h"
#include "runtime/runtime.h"
#include "mpi/c/bindings.h"
#include "communicator/communicator.h"

#if OMPI_HAVE_WEAK_SYMBOLS && OMPI_PROFILING_DEFINES
#pragma weak MPI_Comm_dup = PMPI_Comm_dup
#endif

#if OMPI_PROFILING_DEFINES
#include "mpi/c/profile/defines.h"
#endif

int MPI_Comm_dup(MPI_Comm comm, MPI_Comm *newcomm) {
    
    /* local variables */
    ompi_communicator_t *comp, *newcomp;
    int rsize, mode, rc=MPI_SUCCESS;
    ompi_proc_t **rprocs;
    
    /* argument checking */
    if ( MPI_PARAM_CHECK ) {
        if (ompi_mpi_finalized)  
            return OMPI_ERRHANDLER_INVOKE(MPI_COMM_WORLD, MPI_ERR_INTERN, 
                                         "MPI_Comm_dup");
        
        if (MPI_COMM_NULL == comm || ompi_comm_invalid (comm))
            return OMPI_ERRHANDLER_INVOKE(MPI_COMM_WORLD, MPI_ERR_COMM, 
                                         "MPI_Comm_dup");
        
        if ( NULL == newcomm )
            return OMPI_ERRHANDLER_INVOKE(comm, MPI_ERR_ARG, 
                                         "MPI_Comm_dup");
    }

    comp = (ompi_communicator_t *) comm;
    if ( OMPI_COMM_IS_INTER ( comp ) ){
        rsize  = comp->c_remote_group->grp_proc_count;
        rprocs = comp->c_remote_group->grp_proc_pointers;
        mode   = OMPI_COMM_CID_INTER;
    }
    else {
        rsize  = 0;
        rprocs = NULL;
        mode   = OMPI_COMM_CID_INTRA;
    }

    newcomp = ompi_comm_set ( comp,                                   /* old comm */
                              comp->c_local_group->grp_proc_count,    /* local_size */
                              comp->c_local_group->grp_proc_pointers, /* local_procs*/
                              rsize,                                  /* remote_size */
                              rprocs,                                 /* remote_procs */
                              comp->c_keyhash,                        /* attrs */
                              comp->error_handler,                    /* error handler */
                              NULL,                              /* coll module,t.b.d */
                              NULL                              /* topo module, t.b.d */
                             );

    if ( MPI_COMM_NULL == newcomp ) { 
        OMPI_ERRHANDLER_INVOKE (comm, MPI_ERR_INTERN, "MPI_Comm_dup");
    }

    /* Determine context id. It is identical to f_2_c_handle */
    rc = ompi_comm_nextcid ( newcomp,  /* new communicator */ 
                             comp,     /* old comm */
                             NULL,     /* bridge comm */
                             NULL,     /* local leader */
                             NULL,     /* remote_leader */
                             mode );   /* mode */
    if ( OMPI_SUCCESS != rc ) {
        *newcomm = MPI_COMM_NULL;
        OMPI_ERRHANDLER_INVOKE(comm, MPI_ERR_INTERN, "MPI_Comm_dup");
    }
    
    *newcomm = newcomp;
    return ( MPI_SUCCESS );
}
