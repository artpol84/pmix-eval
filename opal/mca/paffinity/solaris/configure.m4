# -*- shell-script -*-
#
# Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
#                         University Research and Technology
#                         Corporation.  All rights reserved.
# Copyright (c) 2004-2005 The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# Copyright (c) 2004-2005 High Performance Computing Center Stuttgart, 
#                         University of Stuttgart.  All rights reserved.
# Copyright (c) 2004-2005 The Regents of the University of California.
#                         All rights reserved.
# Copyright (c) 2007      Cisco, Inc. All rights reserved.
# Copyright (c) 2008      Sun Microsystems, Inc. All rights reserved.
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

# MCA_paffinity_solaris_CONFIG([action-if-found], [action-if-not-found])
# -----------------------------------------------------------
AC_DEFUN([MCA_paffinity_solaris_CONFIG],[
    #check to see if we have <sys/procset.h>
    AC_CHECK_HEADER([sys/procset.h], [happy=yes], [happy=no])

    if test "$happy" = "yes"; then
        # check for processor_bind()
        AC_CHECK_FUNC([processor_bind],[happy=yes],[happy=no])
    fi

    if test "$happy" = "yes"; then
       # check for whether header has P_PID defined
       AC_MSG_CHECKING([if P_PID is defined])
       AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[#include <sys/procset.h>]], [[int i = P_PID;]])],
                         [happy=yes],[happy=no])
       AC_MSG_RESULT([$happy ])
    fi

    if test "$happy" = "yes"; then
       $1
    else
       $2
    fi
])dnl

