/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University.
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

/*
 *
 */

#include "orte_config.h"
#include "orte/orte_constants.h"

#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */

#include "opal/mca/mca.h"
#include "opal/mca/base/base.h"

#include "opal/event/event.h"
#include "opal/util/output.h"
#include "opal/mca/base/mca_base_param.h"

#include "opal/util/output.h"
#include "opal/util/show_help.h"
#include "opal/util/argv.h"
#include "opal/util/opal_environ.h"

#include "opal/threads/mutex.h"
#include "opal/threads/threads.h"
#include "opal/threads/condition.h"

#include "orte/mca/gpr/gpr.h"
#include "orte/runtime/orte_wait.h"

#include "orte/mca/filem/filem.h"
#include "orte/mca/filem/base/base.h"

#include "filem_rsh.h"

/**********
 * Local Function and Variable Declarations
 **********/
static int  orte_filem_rsh_start_copy(orte_filem_base_request_t *request);
static int  orte_filem_rsh_start_rm(orte_filem_base_request_t *request);

static int  orte_filem_rsh_start_command(orte_filem_base_process_set_t *proc_set,
                                         orte_filem_base_file_set_t    *file_set,
                                         char * command,
                                         orte_filem_base_request_t *request,
                                         int index);
static int start_child(char * command,
                       orte_filem_base_request_t *request,
                       int index);

static int  orte_filem_rsh_query_remote_path(char **remote_ref,
                                             orte_process_name_t *proc,
                                             int *flag);
static void orte_filem_rsh_query_callback(int status,
                                          orte_process_name_t* sender,
                                          orte_buffer_t *buffer,
                                          orte_rml_tag_t tag,
                                          void* cbdata);

static void filem_rsh_waitpid_cb(pid_t pid, int status, void* cbdata);

/* Permission to send functionality */
static int orte_filem_rsh_permission_listener_init(orte_rml_buffer_callback_fn_t rml_cbfunc);
static int orte_filem_rsh_permission_listener_cancel(void);
static void orte_filem_rsh_permission_callback(int status,
                                               orte_process_name_t* sender,
                                               orte_buffer_t *buffer,
                                               orte_rml_tag_t tag,
                                               void* cbdata);
static int orte_filem_rsh_permission_ask(orte_process_name_t* sender, int num_sends);
static int permission_send_done(orte_process_name_t* sender, int num_avail);
static int permission_send_num_allowed(orte_process_name_t* sender, int num_allowed);


/*************
 * Local work pool structure
 *************/
int cur_num_incomming = 0;
int cur_num_outgoing  = 0;

static bool work_pool_all_done = false;
static opal_mutex_t     work_pool_lock;
static opal_condition_t work_pool_cond;

/*
 * work_pool_waiting:
 *  - processes that are waiting for permission to put() to me
 */
opal_list_t work_pool_waiting;
/*
 * work_pool_pending:
 *  - put requests waiting on permission to send to peer
 */
opal_list_t work_pool_pending;
/*
 * work_pool_active:
 * - put requests currently sending
 */
opal_list_t work_pool_active;

struct orte_filem_rsh_work_pool_item_t {
    /** This is an object, so must have a super */
    opal_list_item_t super;

    /** Command to exec */
    char * command;

    /** Pointer to FileM Request */
    orte_filem_base_request_t *request;

    /** Index into the request */
    int index;

    /** Process Set */
    orte_filem_base_process_set_t proc_set;

    /** File Set */
    orte_filem_base_file_set_t file_set;

    /** If this item is active */
    bool active;
};
typedef struct orte_filem_rsh_work_pool_item_t orte_filem_rsh_work_pool_item_t;
OBJ_CLASS_DECLARATION(orte_filem_rsh_work_pool_item_t);

void orte_filem_rsh_work_pool_construct(orte_filem_rsh_work_pool_item_t *obj);
void orte_filem_rsh_work_pool_destruct( orte_filem_rsh_work_pool_item_t *obj);

OBJ_CLASS_INSTANCE(orte_filem_rsh_work_pool_item_t,
                   opal_list_item_t,
                   orte_filem_rsh_work_pool_construct,
                   orte_filem_rsh_work_pool_destruct);

void orte_filem_rsh_work_pool_construct(orte_filem_rsh_work_pool_item_t *obj) {
    obj->command = NULL;
    obj->request = NULL;
    obj->index = 0;

    OBJ_CONSTRUCT(&(obj->proc_set), orte_filem_base_process_set_t);
    OBJ_CONSTRUCT(&(obj->file_set), orte_filem_base_file_set_t);

    obj->active = false;
}

void orte_filem_rsh_work_pool_destruct( orte_filem_rsh_work_pool_item_t *obj) {
    if( NULL != obj->command ) {
        free(obj->command);
        obj->command = NULL;
    }

    if( NULL != obj->request ) {
        OBJ_RELEASE(obj->request);
        obj->request = NULL;
    }

    obj->index = 0;

    OBJ_DESTRUCT(&(obj->proc_set));
    OBJ_DESTRUCT(&(obj->file_set));

    obj->active = false;
}

/*
 * Rsh module
 */
static orte_filem_base_module_t loc_module = {
    /** Initialization Function */
    orte_filem_rsh_module_init,
    /** Finalization Function */
    orte_filem_rsh_module_finalize,

    orte_filem_rsh_put,
    orte_filem_rsh_put_nb,

    orte_filem_rsh_get,
    orte_filem_rsh_get_nb,

    orte_filem_rsh_rm,
    orte_filem_rsh_rm_nb,

    orte_filem_rsh_wait,
    orte_filem_rsh_wait_all
};

/*
 * MCA Functions
 */
orte_filem_base_module_1_0_0_t *
orte_filem_rsh_component_query(int *priority)
{
    opal_output_verbose(10, mca_filem_rsh_component.super.output_handle,
                        "filem:rsh: component_query()");

    *priority = mca_filem_rsh_component.super.priority;

    return &loc_module;
}

int orte_filem_rsh_module_init(void)
{
    int ret;

    opal_output_verbose(10, mca_filem_rsh_component.super.output_handle,
                        "filem:rsh: module_init()");

    /*
     * Allocate the work pools
     */
    OBJ_CONSTRUCT(&work_pool_waiting, opal_list_t);
    OBJ_CONSTRUCT(&work_pool_pending, opal_list_t);
    OBJ_CONSTRUCT(&work_pool_active,  opal_list_t);

    OBJ_CONSTRUCT(&work_pool_lock, opal_mutex_t);
    OBJ_CONSTRUCT(&work_pool_cond, opal_condition_t);

    work_pool_all_done = false;

    /*
     * Start the listener for path resolution
     */ 
    if( ORTE_SUCCESS != (ret = orte_filem_base_listener_init(orte_filem_rsh_query_callback) ) ) {
        opal_output(mca_filem_rsh_component.super.output_handle,
                    "filem:rsh:init Failed to start listener\n");
        return ret;
    }

    /*
     * Start the listener for permission
     */ 
    if( ORTE_SUCCESS != (ret = orte_filem_rsh_permission_listener_init(orte_filem_rsh_permission_callback) ) ) {
        opal_output(mca_filem_rsh_component.super.output_handle,
                    "filem:rsh:init Failed to start listener\n");
        return ret;
    }
    
    return ORTE_SUCCESS;
}

int orte_filem_rsh_module_finalize(void)
{
    opal_list_item_t *item = NULL;

    opal_output_verbose(10, mca_filem_rsh_component.super.output_handle,
                        "filem:rsh: module_finalize()");

    /*
     * Make sure all active requests are completed
     */
    while(0 < opal_list_get_size(&work_pool_active) ) {
        ; /* JJH TODO... */
    }

    /*
     * Stop the listeners
     */
    orte_filem_base_listener_cancel();
    orte_filem_rsh_permission_listener_cancel();

    /*
     * Deallocate the work pools
     */
    while( NULL != (item = opal_list_remove_first(&work_pool_waiting)) ) {
        OBJ_RELEASE(item);
    }
    OBJ_DESTRUCT(&work_pool_waiting);

    while( NULL != (item = opal_list_remove_first(&work_pool_pending)) ) {
        OBJ_RELEASE(item);
    }
    OBJ_DESTRUCT(&work_pool_pending);

    while( NULL != (item = opal_list_remove_first(&work_pool_active)) ) {
        OBJ_RELEASE(item);
    }
    OBJ_DESTRUCT(&work_pool_active);

    OBJ_DESTRUCT(&work_pool_lock);
    OBJ_DESTRUCT(&work_pool_cond);

    return ORTE_SUCCESS;
}

/******************
 * Local functions
 ******************/
int orte_filem_rsh_put(orte_filem_base_request_t *request)
{
    int ret;

    if( ORTE_SUCCESS != (ret = orte_filem_base_prepare_request(request, ORTE_FILEM_MOVE_TYPE_PUT) ) ) {
        opal_output(mca_filem_rsh_component.super.output_handle,
                    "filem:rsh: put(): Failed to preare the request structure (%d)", ret);
        return ret;
    }

    if( ORTE_SUCCESS != (ret = orte_filem_rsh_start_copy(request) ) ) {
        opal_output(mca_filem_rsh_component.super.output_handle,
                    "filem:rsh: put(): Failed to post the request (%d)", ret);
        return ret;
    }

    if( ORTE_SUCCESS != (ret = orte_filem_rsh_wait(request)) ) {
        opal_output(mca_filem_rsh_component.super.output_handle,
                    "filem:rsh: put(): Failed to wait on the request (%d)", ret);
        return ret;
    }

    return ORTE_SUCCESS;
}

int orte_filem_rsh_put_nb(orte_filem_base_request_t *request)
{
    int ret;

    if( ORTE_SUCCESS != (ret = orte_filem_base_prepare_request(request, ORTE_FILEM_MOVE_TYPE_PUT) ) ) {
        opal_output(mca_filem_rsh_component.super.output_handle,
                    "filem:rsh: put(): Failed to preare the request structure (%d)", ret);
        return ret;
    }

    if( ORTE_SUCCESS != (ret = orte_filem_rsh_start_copy(request) ) ) {
        opal_output(mca_filem_rsh_component.super.output_handle,
                    "filem:rsh: put(): Failed to post the request (%d)", ret);
        return ret;
    }

    return ORTE_SUCCESS;
}

int orte_filem_rsh_get(orte_filem_base_request_t *request)
{
    int ret;

    if( ORTE_SUCCESS != (ret = orte_filem_base_prepare_request(request, ORTE_FILEM_MOVE_TYPE_GET) ) ) {
        opal_output(mca_filem_rsh_component.super.output_handle,
                    "filem:rsh: get(): Failed to preare the request structure (%d)", ret);
        return ret;
    }

    if( ORTE_SUCCESS != (ret = orte_filem_rsh_start_copy(request) ) ) {
        opal_output(mca_filem_rsh_component.super.output_handle,
                    "filem:rsh: get(): Failed to post the request (%d)", ret);
        return ret;
    }

    if( ORTE_SUCCESS != (ret = orte_filem_rsh_wait(request)) ) {
        opal_output(mca_filem_rsh_component.super.output_handle,
                    "filem:rsh: get(): Failed to wait on the request (%d)", ret);
        return ret;
    }

    return ORTE_SUCCESS;
}

int orte_filem_rsh_get_nb(orte_filem_base_request_t *request)
{
    int ret;

    if( ORTE_SUCCESS != (ret = orte_filem_base_prepare_request(request, ORTE_FILEM_MOVE_TYPE_GET) ) ) {
        opal_output(mca_filem_rsh_component.super.output_handle,
                    "filem:rsh: get(): Failed to preare the request structure (%d)", ret);
        return ret;
    }

    if( ORTE_SUCCESS != (ret = orte_filem_rsh_start_copy(request) ) ) {
        opal_output(mca_filem_rsh_component.super.output_handle,
                    "filem:rsh: get(): Failed to post the request (%d)", ret);
        return ret;
    }

    return ORTE_SUCCESS;
}
 
int orte_filem_rsh_rm(orte_filem_base_request_t *request)
{
    int ret = ORTE_SUCCESS, exit_status = ORTE_SUCCESS;

    if( ORTE_SUCCESS != (ret = orte_filem_base_prepare_request(request, ORTE_FILEM_MOVE_TYPE_RM) ) ) {
        opal_output(mca_filem_rsh_component.super.output_handle,
                    "filem:rsh: rm(): Failed to prepare on the request (%d)", ret);
        return ret;
    }

    if( ORTE_SUCCESS != (ret = orte_filem_rsh_start_rm(request) ) ) {
        opal_output(mca_filem_rsh_component.super.output_handle,
                    "filem:rsh: rm(): Failed to start the request (%d)", ret);
        return ret;
    }

    if( ORTE_SUCCESS != (ret = orte_filem_rsh_wait(request)) ) {
        opal_output(mca_filem_rsh_component.super.output_handle,
                    "filem:rsh: rm(): Failed to wait on the request (%d)", ret);
        return ret;
    }

    return exit_status;
}

int orte_filem_rsh_rm_nb(orte_filem_base_request_t *request)
{
    int ret = ORTE_SUCCESS, exit_status = ORTE_SUCCESS;

    if( ORTE_SUCCESS != (ret = orte_filem_base_prepare_request(request, ORTE_FILEM_MOVE_TYPE_RM) ) ) {
        opal_output(mca_filem_rsh_component.super.output_handle,
                    "filem:rsh: rm_nb(): Failed to prepare on the request (%d)", ret);
        return ret;
    }

    if( ORTE_SUCCESS != (ret = orte_filem_rsh_start_rm(request) ) ) {
        opal_output(mca_filem_rsh_component.super.output_handle,
                    "filem:rsh: rm_nb(): Failed to start on the request (%d)", ret);
        return ret;
    }

    return exit_status;
}

int orte_filem_rsh_wait(orte_filem_base_request_t *request)
{
    int exit_status = ORTE_SUCCESS;
    orte_filem_rsh_work_pool_item_t *wp_item = NULL;
    opal_list_item_t *item = NULL;
    int i;
    int num_finished = 0;
    bool found_match = false;

    /*
     * Stage 0: A pass to see if the request is already done
     */
    for(i = 0; i < request->num_mv; ++i) {
        if( request->is_done[i]   == true &&
            request->is_active[i] == true) {
            ++num_finished;
        }
    }

    while(num_finished < request->num_mv ) {
        /*
         * Stage 1: Complete all active requests
         */
        for(i = 0; i < request->num_mv; ++i) {
            /* If the child is still executing (active) then continue
             * checking other children
             */
            if( request->is_done[i]   == false && 
                request->is_active[i] == true) {
                continue;
            }
            /* If the child is done executing (!active), but has not been
             * collected then do so
             */
            else if( request->is_done[i]   == true && 
                     request->is_active[i] == false) {
                /* 
                 * Find the reference in the active pool
                 */
                found_match = false;
                for (item  = opal_list_get_first( &work_pool_active);
                     item != opal_list_get_end(   &work_pool_active);
                     item  = opal_list_get_next(   item) ) {
                    wp_item = (orte_filem_rsh_work_pool_item_t *)item;
                    if(request == wp_item->request &&
                       i       == wp_item->index) {
                        found_match = true;
                        break;
                    }
                }

                /* If no match then assume on the pending list, and continue */
                if( !found_match ) {
                    continue;
                }

                opal_output_verbose(10, mca_filem_rsh_component.super.output_handle,
                                    "filem:rsh: wait(): Transfer complete. Cleanup\n");

                opal_list_remove_item(&work_pool_active, item);

                /* Mark as fully done [active = true, done = true]
                 * It does not make complete sense to call this 'active' when
                 * it is obviously not, but this is a state [true, true] that
                 * should only be reached if the transfer has finished
                 * completely.
                 */
                request->is_done[i]   = true;
                request->is_active[i] = true;

                /* Tell peer we are finished with a send */
                permission_send_done(&(wp_item->proc_set.source), 1);

                OBJ_RELEASE(wp_item);
                wp_item = NULL;

                ++num_finished;
            }
        }

        /*
         * Wait for a child to complete
         */
        if( num_finished < request->num_mv ) {
            OPAL_THREAD_LOCK(&work_pool_lock);
            opal_condition_wait(&work_pool_cond,
                                &work_pool_lock);
            OPAL_THREAD_UNLOCK(&work_pool_lock);
        }
    }

    /*
     * Stage 2: Determine the return value
     */
    for(i = 0; i < request->num_mv; ++i) {
        if( request->exit_status[i] < 0 ) {
            exit_status = request->exit_status[i];
        }
    }

    return exit_status;
}

int orte_filem_rsh_wait_all(opal_list_t * request_list)
{
    int ret = ORTE_SUCCESS, exit_status = ORTE_SUCCESS;
    opal_list_item_t *item = NULL;

    for (item  = opal_list_get_first( request_list);
         item != opal_list_get_end(   request_list);
         item  = opal_list_get_next(  item) ) {
        orte_filem_base_request_t *request = (orte_filem_base_request_t *) item;

        if( ORTE_SUCCESS != (ret = orte_filem_rsh_wait(request)) ) {
            exit_status = ret;
            goto cleanup;
        }
    }

 cleanup:
    return exit_status;
}

/**************************
 * Support functions
 **************************/
static int orte_filem_rsh_start_copy(orte_filem_base_request_t *request) {
    int ret = ORTE_SUCCESS, exit_status = ORTE_SUCCESS;
    opal_list_item_t *f_item = NULL;
    opal_list_item_t *p_item = NULL;
    char *remote_machine = NULL;
    char *remote_file    = NULL;
    char *command        = NULL;
    char *dir_arg        = NULL;
    int cur_index = 0;

    /* For each file pair */
    for (f_item  = opal_list_get_first( &request->file_sets);
         f_item != opal_list_get_end(   &request->file_sets);
         f_item  = opal_list_get_next(   f_item) ) {
        orte_filem_base_file_set_t * f_set = (orte_filem_base_file_set_t*)f_item;

        /* For each process set */
        for (p_item  = opal_list_get_first( &request->process_sets);
             p_item != opal_list_get_end(   &request->process_sets);
             p_item  = opal_list_get_next(   p_item) ) {
            orte_filem_base_process_set_t * p_set = (orte_filem_base_process_set_t*)p_item;

            opal_output_verbose(10, mca_filem_rsh_component.super.output_handle,
                                "filem:rsh: copy(): %s -> %s Moving file %s to %s\n",
                                ORTE_NAME_PRINT(&p_set->source),
                                ORTE_NAME_PRINT(&p_set->sink),
                                f_set->local_target,
                                f_set->remote_target);

            /*
             * Get the remote machine identifier from the process_name struct
             */
            if( ORTE_SUCCESS != (ret = orte_filem_base_get_proc_node_name(&p_set->source, &remote_machine))) {
                exit_status = ret;
                goto cleanup;
            }

            /*
             * Fix the remote_filename.
             * If it is an absolute path, then assume it is valid for the remote server
             * ow then we must construct the correct path.
             */
            remote_file = strdup(f_set->remote_target);
            if( ORTE_SUCCESS != (ret = orte_filem_rsh_query_remote_path(&remote_file, &p_set->source, &f_set->target_flag) ) ) {
                exit_status = ret;
                goto cleanup;
            }

            /*
             * Transfer the file or directory
             */
            if(ORTE_FILEM_TYPE_DIR == f_set->target_flag) {
                dir_arg = strdup(" -r ");
            }
            else if(ORTE_FILEM_TYPE_UNKNOWN == f_set->target_flag) {
                goto continue_set;
            }
            else {
                dir_arg = strdup("");
            }

            /*
             * If this is the put() routine
             */
            if( request->movement_type == ORTE_FILEM_MOVE_TYPE_PUT ) {
                asprintf(&command, "%s %s %s %s:%s ",
                         mca_filem_rsh_component.cp_command, 
                         dir_arg, 
                         f_set->local_target,
                         remote_machine, 
                         remote_file);
                opal_output_verbose(17, mca_filem_rsh_component.super.output_handle,
                                    "filem:rsh:put about to execute [%s]", command);

                if( ORTE_SUCCESS != (ret = orte_filem_rsh_start_command(p_set,
                                                                        f_set,
                                                                        command,
                                                                        request,
                                                                        cur_index)) ) {
                    exit_status = ret;
                    goto cleanup;
                }
            }
            /*
             * ow it is the get() routine
             */
            else {
                asprintf(&command, "%s %s %s:%s %s ",
                         mca_filem_rsh_component.cp_command, 
                         dir_arg, 
                         remote_machine, 
                         remote_file,
                         f_set->local_target);
                
                opal_output_verbose(17, mca_filem_rsh_component.super.output_handle,
                                    "filem:rsh:get about to execute [%s]", command);
                
                if( ORTE_SUCCESS != (ret = orte_filem_rsh_start_command(p_set,
                                                                        f_set,
                                                                        command,
                                                                        request,
                                                                        cur_index)) ) {
                    exit_status = ret;
                    goto cleanup;
                }
            }

        continue_set:
            /* A small bit of cleanup */
            if( NULL != dir_arg) {
                free(dir_arg);
                dir_arg = NULL;
            }

            if( NULL != remote_file) {
                free(remote_file);
                remote_file = NULL;
            }

            if(NULL != remote_machine) {
                free(remote_machine);
                remote_machine = NULL;
            }

            ++cur_index;
        } /* For each process set */
    } /* For each file pair */

 cleanup:
    if( NULL != command )
        free(command);
    if( NULL != remote_machine)
        free(remote_machine);
    if( NULL != dir_arg) 
        free(dir_arg);
    if( NULL != remote_file)
        free(remote_file);
        
    return exit_status;
}

static int orte_filem_rsh_start_rm(orte_filem_base_request_t *request)
{
    int ret = ORTE_SUCCESS, exit_status = ORTE_SUCCESS;
    opal_list_item_t *f_item = NULL;
    opal_list_item_t *p_item = NULL;
    char *command        = NULL;
    char *remote_machine = NULL;
    char *remote_targets = NULL;
    char *remote_file    = NULL;
    char *dir_arg        = NULL;
    char **remote_file_set = NULL;
    int argc = 0;
    int cur_index = 0;

    /* For each process set */
    for (p_item  = opal_list_get_first( &request->process_sets);
         p_item != opal_list_get_end(   &request->process_sets);
         p_item  = opal_list_get_next(   p_item) ) {
        orte_filem_base_process_set_t * p_set = (orte_filem_base_process_set_t*)p_item;

        /*
         * Get the remote machine identifier from the process_name struct
         */
        if( ORTE_SUCCESS != (ret = orte_filem_base_get_proc_node_name(&p_set->source, &remote_machine))) {
            exit_status = ret;
            goto cleanup;
        }

        /* For each file pair */
        for (f_item  = opal_list_get_first( &request->file_sets);
             f_item != opal_list_get_end(   &request->file_sets);
             f_item  = opal_list_get_next(   f_item) ) {
            orte_filem_base_file_set_t * f_set = (orte_filem_base_file_set_t*)f_item;

            /*
             * Fix the remote_filename.
             * If it is an absolute path, then assume it is valid for the remote server
             * ow then we must construct the correct path.
             */
            remote_file = strdup(f_set->remote_target);
            if( ORTE_SUCCESS != (ret = orte_filem_rsh_query_remote_path(&remote_file, &p_set->source, &f_set->target_flag) ) ) {
                exit_status = ret;
                goto cleanup;
            }

            if(ORTE_FILEM_TYPE_UNKNOWN == f_set->target_flag) {
                continue;
            }

            opal_argv_append(&argc, &remote_file_set, remote_file);

            /*
             * If we are removing a directory in the mix, then we
             * need the recursive argument.
             */
            if(NULL == dir_arg) {
                if(ORTE_FILEM_TYPE_DIR == f_set->target_flag) {
                    dir_arg = strdup(" -rf ");
                }
            }
        } /* All File Pairs */

        if(NULL == dir_arg) {
            dir_arg = strdup(" -f ");
        }
        
        remote_targets = opal_argv_join(remote_file_set, ' ');

        asprintf(&command, "%s %s rm %s %s ",
                 mca_filem_rsh_component.remote_sh_command, 
                 remote_machine,
                 dir_arg,
                 remote_targets);

        opal_output_verbose(15, mca_filem_rsh_component.super.output_handle,
                            "filem:rsh:rm about to execute [%s]", command);

        if( ORTE_SUCCESS != (ret = orte_filem_rsh_start_command(p_set,
                                                                NULL,
                                                                command,
                                                                request,
                                                                cur_index)) ) {
            exit_status = ret;
            goto cleanup;
        }

        /* A small bit of cleanup */
        if( NULL != dir_arg) {
            free(dir_arg);
            dir_arg = NULL;
        }

        if( NULL != remote_targets) {
            free(remote_targets);
            remote_targets = NULL;
        }
            
        if( NULL != remote_file_set) {
            opal_argv_free(remote_file_set);
            remote_file_set = NULL;
        }

        if(NULL != remote_machine) {
            free(remote_machine);
            remote_machine = NULL;
        }

        ++cur_index;
    } /* Process set */

 cleanup:
    if( NULL != command )
        free(command);
    if( NULL != remote_machine)
        free(remote_machine);
    if( NULL != dir_arg) 
        free(dir_arg);
    if( NULL != remote_targets)
        free(remote_targets);
    if( NULL != remote_file_set)
        opal_argv_free(remote_file_set);

    return exit_status;
}

/******************
 * Local Functions
 ******************/

/******************************
 * Work Pool functions
 ******************************/
static int  orte_filem_rsh_start_command(orte_filem_base_process_set_t *proc_set,
                                         orte_filem_base_file_set_t    *file_set,
                                         char * command,
                                         orte_filem_base_request_t *request,
                                         int index)
{
    orte_filem_rsh_work_pool_item_t *wp_item = NULL;
    int ret;

    proc_set->source.vpid  = 1;
    proc_set->source.jobid = 0;

    /* Construct a work pool item */
    wp_item = OBJ_NEW(orte_filem_rsh_work_pool_item_t);
    /* Copy the Process Set */
    if( NULL != proc_set ) {
        wp_item->proc_set.source.jobid = proc_set->source.jobid;
        wp_item->proc_set.source.vpid  = proc_set->source.vpid;
        wp_item->proc_set.sink.jobid = proc_set->sink.jobid;
        wp_item->proc_set.sink.vpid  = proc_set->sink.vpid;
    }
    /* Copy the File Set */
    if( NULL != file_set ) {
        wp_item->file_set.local_target  = strdup(file_set->local_target);
        wp_item->file_set.remote_target = strdup(file_set->remote_target);
        wp_item->file_set.target_flag   = file_set->target_flag;
    }
    OBJ_RETAIN(request);
    wp_item->command = strdup(command);
    wp_item->request = request;
    wp_item->index   = index;

    /*
     * - put the request on the pending list
     * - wait for the peer to tell us that it can receive
     */
    opal_list_append(&work_pool_pending, &(wp_item->super));

    /*
     * Ask for permission to send this file so we do not overwhelm the peer
     */
    opal_output_verbose(10, mca_filem_rsh_component.super.output_handle,
                        "filem:rsh: start_command(): Ask permission to send from proc %s",
                        ORTE_NAME_PRINT(&(proc_set->source)));
    if( ORTE_SUCCESS != (ret = orte_filem_rsh_permission_ask(&(proc_set->source), 1)) ) {
        return ret;
    }

    return ORTE_SUCCESS;
}

/******************************
 * Child process start and wait functions
 ******************************/
static int start_child(char * command,
                       orte_filem_base_request_t *request,
                       int index)
{
    char **argv = NULL;
    int status, ret;

    opal_output_verbose(10, mca_filem_rsh_component.super.output_handle,
                        "filem:rsh: start_child(): Starting the command [%s]",
                        command);
    /* fork() -> done = false, active = true */
    request->is_done[index]     = false;
    request->is_active[index]   = true;
    request->exit_status[index] = fork();

    if( request->exit_status[index] == 0 ) { /* Child */
        /* Redirect stdout to /dev/null */
        freopen( "/dev/null", "w", stdout);

        argv = opal_argv_split(command, ' ');

        status = execvp(argv[0], argv);

        opal_output(0, "filem:rsh:start_child Failed to exec child [%s] status = %d\n", command, status);
        exit(ORTE_ERROR);
    }
    else if( request->exit_status[index] > 0 ) {
        opal_output_verbose(10, mca_filem_rsh_component.super.output_handle,
                            "filem:rsh: start_child(): Started Child %d Running command [%s]",
                            request->exit_status[index], command);

        /*
         * Register a callback for when this process exits
         */
        if( ORTE_SUCCESS != (ret = orte_wait_cb(request->exit_status[index], filem_rsh_waitpid_cb, NULL) ) ) {
            opal_output(0, "filem:rsh: start_child(): Failed to register a waitpid callback for child [%d] executing the command [%s]\n",
                        request->exit_status[index], command);
            return ret;
        }
    }
    else {
        return ORTE_ERROR;
    }

    return ORTE_SUCCESS;
}


static void filem_rsh_waitpid_cb(pid_t pid, int status, void* cbdata)
{
    orte_filem_rsh_work_pool_item_t *wp_item = NULL;
    orte_filem_base_request_t *request;
    opal_list_item_t *item = NULL;
    int index;

    opal_output_verbose(10, mca_filem_rsh_component.super.output_handle,
                        "filem:rsh: waitpid_cb(): Pid %d finished with status [%d].\n",
                        pid, status);

    /*
     * Find this pid in the active queue
     */
    OPAL_THREAD_LOCK(&work_pool_lock);
    for (item  = opal_list_get_first( &work_pool_active);
         item != opal_list_get_end(   &work_pool_active);
         item  = opal_list_get_next(   item) ) {
        wp_item = (orte_filem_rsh_work_pool_item_t *)item;
        request = wp_item->request;
        index   = wp_item->index;
        if( request->is_done[index]     == false &&
            request->exit_status[index] == pid ) {
            request->exit_status[index] = status;
            /* waitpid() -> done = true, active = false */
            request->is_done[index]     = true;
            request->is_active[index]   = false;
            opal_output_verbose(10, mca_filem_rsh_component.super.output_handle,
                                "filem:rsh: waitpid_cb(): Marked pid %d as complete [status = %d].\n",
                                pid, status);
            break;
        }
    }

    /*
     * Signal in case anyone is waiting for a child to finish.
     */
    opal_condition_signal(&work_pool_cond);
    OPAL_THREAD_UNLOCK(&work_pool_lock);
}

/******************************
 * Path resolution functions
 ******************************/
/*
 * This function is paired with the orte_filem_rsh_query_callback() function on the remote machine
 */
static int orte_filem_rsh_query_remote_path(char **remote_ref, orte_process_name_t *peer, int *flag) {
    int ret;

#if 1 /* JJH: Some debugging */
    /* If it is an absolute path */
    if( *remote_ref[0] == '/' ) {
        *flag = ORTE_FILEM_TYPE_DIR;
        return ORTE_SUCCESS;
    }
#endif

    /* Put our listener on hold */
    orte_filem_base_listener_cancel();

    /* Call the base function */
    if( ORTE_SUCCESS != (ret = orte_filem_base_query_remote_path(remote_ref, peer, flag) ) ) {
        return ret;
    }

    /* Reset the listener */
    if( ORTE_SUCCESS != (ret = orte_filem_base_listener_init(orte_filem_rsh_query_callback) ) ) {
        return ret;
    }

    return ORTE_SUCCESS;
}

/*
 * This function is paired with the orte_filem_rsh_query_remote_path() function on the 
 * requesting machine.
 * This function is responsible for:
 * - Constructing the remote absolute path for the specified file/dir
 * - Verify the existence of the file/dir
 * - Determine if the specified file/dir is in fact a file or dir or unknown if not found.
 * 
 */
static void orte_filem_rsh_query_callback(int status,
                                          orte_process_name_t* peer,
                                          orte_buffer_t *buffer,
                                          orte_rml_tag_t tag,
                                          void* cbdata) 
{
    opal_output_verbose(15, mca_filem_rsh_component.super.output_handle,
                        "filem:rsh: query_callback(%s -> %s)",
                        ORTE_NAME_PRINT(orte_process_info.my_name),
                        ORTE_NAME_PRINT(peer));

    /* Call the base callback function */
    orte_filem_base_query_callback(status, peer, buffer, tag, cbdata);

    /* Reset the listener */
    orte_filem_base_listener_init(orte_filem_rsh_query_callback);
}

/******************************
 * Permission functions
 ******************************/
static int orte_filem_rsh_permission_listener_init(orte_rml_buffer_callback_fn_t rml_cbfunc)
{
    int ret;

    if( ORTE_SUCCESS != (ret = orte_rml.recv_buffer_nb(ORTE_NAME_WILDCARD,
                                                       ORTE_RML_TAG_FILEM_RSH,
                                                       ORTE_RML_PERSISTENT,
                                                       rml_cbfunc,
                                                       NULL)) ) {
        opal_output(mca_filem_rsh_component.super.output_handle,
                    "filem:rsh: listener_init: Failed to register the receive callback (%d)",
                    ret);
        return ret;
    }

    return ORTE_SUCCESS;
}

static int orte_filem_rsh_permission_listener_cancel(void)
{
    int ret;

    if( ORTE_SUCCESS != (ret = orte_rml.recv_cancel(ORTE_NAME_WILDCARD, ORTE_RML_TAG_FILEM_RSH) ) ) {
        opal_output(mca_filem_rsh_component.super.output_handle,
                    "filem:rsh: listener_cancel: Failed to deregister the receive callback (%d)",
                    ret);
        return ret;
    }

    return ORTE_SUCCESS;
}

static void orte_filem_rsh_permission_callback(int status,
                                               orte_process_name_t* sender,
                                               orte_buffer_t *buffer,
                                               orte_rml_tag_t tag,
                                               void* cbdata)
{
    orte_filem_rsh_work_pool_item_t *wp_item = NULL;
    opal_list_item_t *item = NULL;
    int ret;
    orte_std_cntr_t n;
    int num_req, num_allowed = 0;
    int perm_flag, i;

    opal_output_verbose(10, mca_filem_rsh_component.super.output_handle,
                        "filem:rsh: permission_callback(? ?): Peer %s ...",
                        ORTE_NAME_PRINT(sender));

    /*
     * Receive the flag indicating if this is:
     * - Asking for permission (ORTE_FILEM_RSH_ASK)
     * - Allowing us to send (ORTE_FILEM_RSH_ALLOW)
     */
    n = 1;
    if (ORTE_SUCCESS != (ret = orte_dss.unpack(buffer, &perm_flag, &n, ORTE_INT))) {
        goto cleanup;
    }

    /* Asking for permission to send */
    if( ORTE_FILEM_RSH_ASK == perm_flag ) {
        opal_output_verbose(10, mca_filem_rsh_component.super.output_handle,
                            "filem:rsh: permission_callback(ASK): Peer %s Asking permission to send [Used %d of %d]",
                            ORTE_NAME_PRINT(sender),
                            cur_num_incomming,
                            orte_filem_rsh_max_incomming);

        /*
         * Receive the requested amount
         */
        n = 1;
        if (ORTE_SUCCESS != (ret = orte_dss.unpack(buffer, &num_req, &n, ORTE_INT))) {
            goto cleanup;
        }

        /*
         * Determine how many we can allow
         * if none then put a request on the waiting list
         * ow tell the peer to start sending now.
         * Send back number allowed to be started
         */
        if( orte_filem_rsh_max_incomming < cur_num_incomming + 1) {
            /* Add to the waiting list */
            opal_output_verbose(10, mca_filem_rsh_component.super.output_handle,
                                "filem:rsh: permission_callback(ASK): Add Peer %s request to waiting list",
                                ORTE_NAME_PRINT(sender));

            wp_item = OBJ_NEW(orte_filem_rsh_work_pool_item_t);
            wp_item->proc_set.source.jobid = sender->jobid;
            wp_item->proc_set.source.vpid  = sender->vpid;

            opal_list_append(&work_pool_waiting, &(wp_item->super));
        }
        /* Start the transfer immediately */
        else {
            num_allowed = 1;
            cur_num_incomming += 1;

            opal_output_verbose(10, mca_filem_rsh_component.super.output_handle,
                                "filem:rsh: permission_callback(ASK): Respond to Peer %s with %d",
                                ORTE_NAME_PRINT(sender), num_allowed);

            permission_send_num_allowed(sender, num_allowed);
        }
    }
    /* Allowing us to start some number of sends */
    else if( ORTE_FILEM_RSH_ALLOW == perm_flag ) {
        opal_output_verbose(10, mca_filem_rsh_component.super.output_handle,
                            "filem:rsh: permission_callback(ALLOW): Peer %s Allowing me to send",
                            ORTE_NAME_PRINT(sender));

        /*
         * Receive the allowed transmit amount
         */
        n = 1;
        if (ORTE_SUCCESS != (ret = orte_dss.unpack(buffer, &num_req, &n, ORTE_INT))) {
            goto cleanup;
        }

        /*
         * For each alloacted spot for transmit
         * - Get a pending request directed at this peer
         * - Start the pending request
         */
        for(i = 0; i < num_req; ++i ) {
            if( 0 >= opal_list_get_size(&work_pool_pending) ) {
                opal_output_verbose(10, mca_filem_rsh_component.super.output_handle,
                                    "filem:rsh: permission_callback(ALLOW): No more pending sends to peer %s...",
                                    ORTE_NAME_PRINT(sender));
                break;
            }

            for (item  = opal_list_get_first( &work_pool_pending);
                 item != opal_list_get_end(   &work_pool_pending);
                 item  = opal_list_get_next(   item) ) {
                wp_item = (orte_filem_rsh_work_pool_item_t *)item;
                if(sender->jobid == wp_item->proc_set.source.jobid &&
                   sender->vpid  == wp_item->proc_set.source.vpid ) {
                    opal_list_remove_item( &work_pool_pending, item);
                    break;
                }
            }

            if( item == opal_list_get_end(&work_pool_pending) ) {
                opal_output_verbose(10, mca_filem_rsh_component.super.output_handle,
                                    "filem:rsh: permission_callback(ALLOW): Unable to find message on the pending list\n");
            }

            opal_output_verbose(10, mca_filem_rsh_component.super.output_handle,
                                "filem:rsh: permission_callback(ALLOW): Starting to send to peer %s... (# pending = %d)",
                                ORTE_NAME_PRINT(sender), (int)opal_list_get_size(&work_pool_pending));
            wp_item->active = true;
            opal_list_append(&work_pool_active, &(wp_item->super));
            if( ORTE_SUCCESS != (ret = start_child(wp_item->command,
                                                   wp_item->request,
                                                   wp_item->index)) ) {
                goto cleanup;
            }
        }
    }
    /* Peer said they are done sending one or more files */
    else if( ORTE_FILEM_RSH_DONE == perm_flag ) {
        opal_output_verbose(10, mca_filem_rsh_component.super.output_handle,
                            "filem:rsh: permission_callback(DONE): Peer %s is done sending to me",
                            ORTE_NAME_PRINT(sender));

        /*
         * Receive the number of open slots
         */
        n = 1;
        if (ORTE_SUCCESS != (ret = orte_dss.unpack(buffer, &num_req, &n, ORTE_INT))) {
            goto cleanup;
        }

        cur_num_incomming -= num_req;

        /*
         * For each open slot, notify a waiting peer that it may send
         */
        for(i = 0; i < num_req; ++i ) {
            item  = opal_list_get_first( &work_pool_waiting);
            if( item != opal_list_get_end(   &work_pool_waiting) ) {
                wp_item = (orte_filem_rsh_work_pool_item_t *)item;

                num_allowed = 1;
                cur_num_incomming += 1;
                opal_list_remove_item(&work_pool_waiting, item);

                permission_send_num_allowed(&(wp_item->proc_set.source), num_allowed);

                OBJ_RELEASE(wp_item);
            }
        }
    }

 cleanup:
    return;
}

static int orte_filem_rsh_permission_ask(orte_process_name_t* source,
                                         int num_sends)
{
    int ret, exit_status = ORTE_SUCCESS;
    orte_buffer_t loc_buffer;
    int perm_flag = ORTE_FILEM_RSH_ASK;

    OBJ_CONSTRUCT(&loc_buffer, orte_buffer_t);

    if (ORTE_SUCCESS != (ret = orte_dss.pack(&loc_buffer, &perm_flag, 1, ORTE_INT))) {
        exit_status = ret;
        goto cleanup;
    }

    if (ORTE_SUCCESS != (ret = orte_dss.pack(&loc_buffer, &num_sends, 1, ORTE_INT))) {
        exit_status = ret;
        goto cleanup;
    }

    if (0 > (ret = orte_rml.send_buffer(source, &loc_buffer, ORTE_RML_TAG_FILEM_RSH, 0))) {
        exit_status = ret;
        goto cleanup;
    }
    
 cleanup:
    OBJ_DESTRUCT(&loc_buffer);

    return exit_status;
}

static int permission_send_done(orte_process_name_t* peer, int num_avail) {
    int ret, exit_status = ORTE_SUCCESS;
    orte_buffer_t loc_buffer;
    int perm_flag = ORTE_FILEM_RSH_DONE;

    OBJ_CONSTRUCT(&loc_buffer, orte_buffer_t);

    if (ORTE_SUCCESS != (ret = orte_dss.pack(&loc_buffer, &perm_flag, 1, ORTE_INT))) {
        exit_status = ret;
        goto cleanup;
    }

    if (ORTE_SUCCESS != (ret = orte_dss.pack(&loc_buffer, &num_avail, 1, ORTE_INT))) {
        exit_status = ret;
        goto cleanup;
    }

    if (0 > (ret = orte_rml.send_buffer(peer, &loc_buffer, ORTE_RML_TAG_FILEM_RSH, 0))) {
        exit_status = ret;
        goto cleanup;
    }

 cleanup:
    OBJ_DESTRUCT(&loc_buffer);

    return exit_status;
}

static int permission_send_num_allowed(orte_process_name_t* peer, int num_allowed)
{
    int ret, exit_status = ORTE_SUCCESS;
    orte_buffer_t loc_buffer;
    int perm_flag = ORTE_FILEM_RSH_ALLOW;

    OBJ_CONSTRUCT(&loc_buffer, orte_buffer_t);

    if (ORTE_SUCCESS != (ret = orte_dss.pack(&loc_buffer, &perm_flag, 1, ORTE_INT))) {
        exit_status = ret;
        goto cleanup;
    }

    if (ORTE_SUCCESS != (ret = orte_dss.pack(&loc_buffer, &num_allowed, 1, ORTE_INT))) {
        exit_status = ret;
        goto cleanup;
    }

    if (0 > (ret = orte_rml.send_buffer(peer, &loc_buffer, ORTE_RML_TAG_FILEM_RSH, 0))) {
        exit_status = ret;
        goto cleanup;
    }

 cleanup:
    OBJ_DESTRUCT(&loc_buffer);

    return exit_status;
}

