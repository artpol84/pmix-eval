/*
 * $HEADERS$
 */
#include "lam_config.h"
#include <stdio.h>

#include "mpi.h"
#include "mpi/interface/c/bindings.h"

#if LAM_HAVE_WEAK_SYMBOLS && LAM_PROFILING_DEFINES
#pragma weak MPI_Get_address = PMPI_Get_address
#endif

int MPI_Get_address(void *location, MPI_Aint *address) {
    return MPI_SUCCESS;
}
