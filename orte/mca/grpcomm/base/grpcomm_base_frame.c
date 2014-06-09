/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014      Intel, Inc. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */


#include "orte_config.h"
#include "orte/constants.h"

#include "opal/mca/mca.h"
#include "opal/util/output.h"
#include "opal/mca/base/base.h"

#include "orte/mca/state/state.h"

#include "orte/mca/grpcomm/base/base.h"


/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public mca_base_component_t struct.
 */

#include "orte/mca/grpcomm/base/static-components.h"


// TODO: move to OPAL

double orte_grpcomm_get_timestamp(){
    struct timeval tv;
    gettimeofday(&tv,NULL);
    double ret = tv.tv_sec + tv.tv_usec*1E-6;
    return ret;
}

void orte_grpcomm_add_timestep(orte_grpcomm_collective_t *coll,
                                               char *step_name)
{
    orte_grpcomm_colltimings_t *elem;
    elem = (void*)malloc(sizeof(*elem));
    if( elem == NULL ){
        // Do some error handling
    }
    elem->step_name = strdup(step_name);
    // Remove trailing '\n'-s
    elem->timestep = orte_grpcomm_get_timestamp();
    opal_list_append (&(coll->timings), (opal_list_item_t *)elem);
    opal_output(0,"%s orte_grpcomm_add_timestep: 0x%p %s\n",
                ORTE_NAME_PRINT((&orte_process_info.my_name)), coll, step_name);
}

void orte_grpcomm_output_timings(orte_grpcomm_collective_t *coll)
{
    orte_grpcomm_colltimings_t *el, *prev, *first;
    int count = 0;
    int size = opal_list_get_size(&(coll->timings));
    char *buf = malloc(size*(60+512));
    buf[0] = '\0';
    OPAL_LIST_FOREACH(el, &(coll->timings), orte_grpcomm_colltimings_t){
        count++;
        if( count > 1){
            sprintf(buf,"%s\n%s GRPCOMM Timings %lfs[%lfs]: %s",buf,
                    ORTE_NAME_PRINT((&orte_process_info.my_name)),
                    el->timestep - first->timestep, el->timestep - prev->timestep,
                    el->step_name);
            prev = el;
        }else{
            first = el;
            prev = el;
        }
    }
    sprintf(buf,"%s\n",buf);
    opal_output(0,"%s",buf);
    free(buf);
}

void orte_grpcomm_clear_timings(orte_grpcomm_collective_t *coll)
{
    orte_grpcomm_colltimings_t *el, *prev;
    int count = 0;
    OPAL_LIST_FOREACH(el, &(coll->timings), orte_grpcomm_colltimings_t){
        if( el->step_name )
            free(el->step_name);
    }
    opal_output(0,"%s orte_grpcomm_clear_timings: 0x%p\n",
                ORTE_NAME_PRINT((&orte_process_info.my_name)), coll);
}

// --------------------------------



/*
 * Global variables
 */
orte_grpcomm_base_t orte_grpcomm_base;

orte_grpcomm_base_module_t orte_grpcomm = {0};

static int orte_grpcomm_base_close(void)
{
    /* Close the selected component */
    if( NULL != orte_grpcomm.finalize ) {
        orte_grpcomm.finalize();
    }
    OBJ_DESTRUCT(&orte_grpcomm_base.active_colls);
    OBJ_DESTRUCT(&orte_grpcomm_base.modex_requests);

#if OPAL_HAVE_HWLOC
  if (NULL != orte_grpcomm_base.working_cpuset) {
      hwloc_bitmap_free(orte_grpcomm_base.working_cpuset);
      orte_grpcomm_base.working_cpuset = NULL;
  }
#endif
    return mca_base_framework_components_close(&orte_grpcomm_base_framework, NULL);
}

/**
 * Function for finding and opening either all MCA components, or the one
 * that was specifically requested via a MCA parameter.
 */
static int orte_grpcomm_base_open(mca_base_open_flag_t flags)
{
    /* init globals */
    OBJ_CONSTRUCT(&orte_grpcomm_base.active_colls, opal_list_t);
    orte_grpcomm_base.coll_id = 0;
    OBJ_CONSTRUCT(&orte_grpcomm_base.modex_requests, opal_list_t);
    orte_grpcomm_base.modex_ready = false;

#if OPAL_HAVE_HWLOC
    orte_grpcomm_base.working_cpuset = NULL;
#endif

    /* register the modex processing event */
    if (ORTE_PROC_IS_APP) {
        orte_state.add_proc_state(ORTE_PROC_STATE_MODEX_READY, orte_grpcomm_base_process_modex, ORTE_MSG_PRI);
    }

    return mca_base_framework_components_open(&orte_grpcomm_base_framework, flags);
}

MCA_BASE_FRAMEWORK_DECLARE(orte, grpcomm, NULL, NULL, orte_grpcomm_base_open, orte_grpcomm_base_close,
                           mca_grpcomm_base_static_components, 0);


orte_grpcomm_collective_t* orte_grpcomm_base_setup_collective(orte_grpcomm_coll_id_t id)
{
    opal_list_item_t *item;
    orte_grpcomm_collective_t *cptr, *coll;

    coll = NULL;
    for (item = opal_list_get_first(&orte_grpcomm_base.active_colls);
         item != opal_list_get_end(&orte_grpcomm_base.active_colls);
         item = opal_list_get_next(item)) {
        cptr = (orte_grpcomm_collective_t*)item;
        if (id == cptr->id) {
            coll = cptr;
            break;
        }
    }
    if (NULL == coll) {
        coll = OBJ_NEW(orte_grpcomm_collective_t);
        opal_output(0,"COLL allocated!: %p\n", coll);
        coll->id = id;
        opal_list_append(&orte_grpcomm_base.active_colls, &coll->super);
    }

    return coll;
}

/* local objects */
static void collective_constructor(orte_grpcomm_collective_t *ptr)
{
    ptr->id = -1;
    ptr->active = false;
    ptr->num_local_recvd = 0;
    OBJ_CONSTRUCT(&ptr->local_bucket, opal_buffer_t);
    ptr->num_peer_buckets = 0;
    ptr->num_global_recvd = 0;
    ptr->locally_complete = false;
    OBJ_CONSTRUCT(&ptr->participants, opal_list_t);
    ptr->cbfunc = NULL;
    ptr->cbdata = NULL;
    OBJ_CONSTRUCT(&ptr->buffer, opal_buffer_t);
    OBJ_CONSTRUCT(&ptr->targets, opal_list_t);
    ptr->next_cb = NULL;
    ptr->next_cbdata = NULL;

#ifdef WANT_ORTE_TIMINGS
    {
        OBJ_CONSTRUCT(&(ptr->timings), opal_list_t);
        orte_grpcomm_add_timestep(ptr,"Initialize");
    }
#endif
}
static void collective_destructor(orte_grpcomm_collective_t *ptr)
{
    opal_list_item_t *item;

    OBJ_DESTRUCT(&ptr->local_bucket);
    while (NULL != (item = opal_list_remove_first(&ptr->participants))) {
        OBJ_RELEASE(item);
    }
    OBJ_DESTRUCT(&ptr->participants);
    OBJ_DESTRUCT(&ptr->buffer);
    while (NULL != (item = opal_list_remove_first(&ptr->targets))) {
        OBJ_RELEASE(item);
    }
    OBJ_DESTRUCT(&ptr->targets);

#ifdef WANT_ORTE_TIMINGS
    {
        orte_grpcomm_add_timestep(ptr,"Finish");
        orte_grpcomm_output_timings(ptr);
        orte_grpcomm_clear_timings(ptr);
        opal_output(0,"TIMINGS: call list destruct\n");
        OPAL_LIST_DESTRUCT(&(ptr->timings));
    }
#endif
}
OBJ_CLASS_INSTANCE(orte_grpcomm_collective_t,
                   opal_list_item_t,
                   collective_constructor,
                   collective_destructor);

OBJ_CLASS_INSTANCE(orte_grpcomm_caddy_t,
                   opal_object_t,
                   NULL, NULL);

OBJ_CLASS_INSTANCE(orte_grpcomm_modex_req_t,
                   opal_list_item_t,
                   NULL, NULL);
