/*
 * $HEADERS$
 */
#include "lam_config.h"
#include <stdio.h>

#include "mpi.h"
#include "mpi/interface/c/bindings.h"

#if LAM_HAVE_WEAK_SYMBOLS && LAM_PROFILING_DEFINES
#pragma weak MPI_Get_elements = PMPI_Get_elements
#endif

int MPI_Get_elements(MPI_Status *status, MPI_Datatype datatype,
		             int *count) {
    return MPI_SUCCESS;
}
