/* -*- C -*-
 * 
 * $HEADER$
 *
 */

#include "ompi_config.h"

#include <sys/types.h>
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#include <sys/time.h>
#include <sys/wait.h>
#include <pwd.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "include/constants.h"
#include "mca/pcm/pcm.h"
#include "mca/pcm/base/base.h"
#include "mca/pcm/rsh/src/pcm_rsh.h"
#include "runtime/runtime_types.h"
#include "event/event.h"
#include "util/output.h"
#include "util/argv.h"
#include "util/numtostr.h"

#if 1
#define BOOTAGENT "mca_pcm_rsh_bootproxy"
#else
#define BOOTAGENT "cat"
#endif
#define PRS_BUFSIZE 1024

static int internal_spawn_proc(mca_ns_base_jobid_t jobid, ompi_rte_node_schedule_t *sched,
                               ompi_list_t *hostlist, 
                               int my_start_vpid, int global_start_vpid,
                               int num_procs);


bool
mca_pcm_rsh_can_spawn(void)
{
    /* we can always try to rsh some more...  Might not always work as
     * the caller hopes
     */
    return true;
}


int
mca_pcm_rsh_spawn_procs(mca_ns_base_jobid_t jobid, ompi_list_t *schedlist)
{
    ompi_list_item_t *sched_item, *node_item, *host_item;
    ompi_rte_node_schedule_t *sched;
    ompi_rte_node_allocation_t *node;
    mca_llm_base_hostfile_data_t *data;
    mca_llm_base_hostfile_node_t *host;
    ompi_list_t launch;
    ompi_list_t done;
    int ret, i;
    int width = 1;
    int local_start_vpid = 0;
    int global_start_vpid = 0;
    int num_procs = 0;
    int tmp_count;

    OBJ_CONSTRUCT(&launch, ompi_list_t);
    OBJ_CONSTRUCT(&done, ompi_list_t);

    for (sched_item = ompi_list_get_first(schedlist) ;
         sched_item != ompi_list_get_end(schedlist) ;
         sched_item = ompi_list_get_next(sched_item)) {
        sched = (ompi_rte_node_schedule_t*) sched_item;

        for (node_item = ompi_list_get_first(sched->nodelist) ;
             node_item != ompi_list_get_end(sched->nodelist) ;
             node_item = ompi_list_get_next(node_item)) {
            node = (ompi_rte_node_allocation_t*) node_item;
            if (node->nodes > 0) {
                num_procs += (node->count * node->nodes);
            } else {
                num_procs += node->count;
            }
        }
    }
    
    /* BWB - make sure vpids are reserved */
    local_start_vpid = global_start_vpid;
    
    for (sched_item = ompi_list_get_first(schedlist) ;
         sched_item != ompi_list_get_end(schedlist) ;
         sched_item = ompi_list_get_next(sched_item)) {
        sched = (ompi_rte_node_schedule_t*) sched_item;

        for (node_item = ompi_list_get_first(sched->nodelist) ;
             node_item != ompi_list_get_end(sched->nodelist) ;
             node_item = ompi_list_get_next(node_item) ) {
            node = (ompi_rte_node_allocation_t*) node_item;
            data = (mca_llm_base_hostfile_data_t*) node->data;

            /*
             * make sure I'm the first node in the list and then start
             * our deal.  We rsh me just like everyone else so that we
             * don't have any unexpected environment oddities...
             */
            /* BWB - do front of list check! */
            host_item = ompi_list_get_first(data->hostlist);

            while (host_item != ompi_list_get_end(data->hostlist)) {
                /* find enough entries for this slice to go */
                tmp_count = 0;
                for (i = 0 ;
                     i < width && 
                         host_item != ompi_list_get_end(data->hostlist) ;
                     host_item = ompi_list_get_next(host_item), ++i) { 
                    host = (mca_llm_base_hostfile_node_t*) host_item;
                    tmp_count += host->count;
                }
                /* if we don't have anyone, get us out of here.. */
                if (i ==  0) {
                    continue;
                }

                /* make a launch list */
                ompi_list_splice(&launch, ompi_list_get_end(&launch),
                                 data->hostlist,
                                 ompi_list_get_first(data->hostlist),
                                 host_item);

                /* do the launch to the first node in the list, passing
                   him the rest of the list */
                ret = internal_spawn_proc(jobid, sched, &launch, 
                                          local_start_vpid, global_start_vpid, 
                                          num_procs);
                if  (OMPI_SUCCESS != ret) {
                    /* well, crap!  put ourselves back together, I guess.
                       Should call killjob */
                    ompi_list_join(&done, ompi_list_get_end(&done), &launch);
                    ompi_list_join(data->hostlist, 
                                   ompi_list_get_first(data->hostlist),
                                   &done);
                    return ret;
                }
                local_start_vpid += tmp_count;

                /* copy the list over to the done part */
                ompi_list_join(&done, ompi_list_get_end(&done), &launch);
            }

            /* put the list back where we found it... */
            ompi_list_join(data->hostlist, ompi_list_get_end(data->hostlist), 
                           &done);
        }
    }

    OBJ_DESTRUCT(&done);
    OBJ_DESTRUCT(&launch);

    return OMPI_SUCCESS;
}


static int
internal_need_profile(mca_llm_base_hostfile_node_t *start_node,
                      int stderr_is_error, bool *needs_profile)
{
    struct passwd *p;
    char shellpath[PRS_BUFSIZE];
    char** cmdv = NULL;
    char *cmd0 = NULL;
    int cmdc = 0;
    char *printable = NULL;
    char *username = NULL;
    int ret;

    /*
     * Figure out if we need to source the .profile on the other side.
     *
     * The following logic is used:
     *
     * if mca_pcm_rsh_no_profile is 1, don't do profile
     * if mca_pcm_rsh_fast is 1, remote shell is assumed same as local
     * if shell is sh/ksh, run profile, otherwise don't
     */
    if (1 == mca_pcm_rsh_no_profile) {
        *needs_profile = false;
        return OMPI_SUCCESS;
    }

    if (1 == mca_pcm_rsh_fast) {
        p = getpwuid(getuid());
        if (NULL == p) return OMPI_ERROR;
            
        ompi_output_verbose(5, mca_pcm_rsh_output, 
                            "fast boot mode - "
                            "assuming same shell on remote nodes");
        ompi_output_verbose(5, mca_pcm_rsh_output, 
                            "getpwuid: got local shell %s", p->pw_shell);
        strncpy(shellpath, p->pw_shell, PRS_BUFSIZE - 1);
        shellpath[PRS_BUFSIZE - 1] = '\0';
    } else {
        /* we have to look at the other side  and get our shell */
        username = mca_pcm_base_get_username(start_node);

        cmdv = ompi_argv_split(mca_pcm_rsh_agent, ' ');
        cmdc = ompi_argv_count(cmdv);

        ompi_argv_append(&cmdc, &cmdv, start_node->hostname);
        if (NULL != username) {
            ompi_argv_append(&cmdc, &cmdv, "-l");
            ompi_argv_append(&cmdc, &cmdv, username);
        }

        ompi_argv_append(&cmdc, &cmdv, "echo $SHELL");
        printable = ompi_argv_join(cmdv, ' ');
        ompi_output_verbose(5, mca_pcm_rsh_output,
                            "attempting to execute: %s", printable);

        cmd0 = strdup(cmdv[0]);
        shellpath[sizeof(shellpath) - 1] = '\0';
        if (mca_pcm_base_ioexecvp(cmdv, 0, shellpath,
                                  sizeof(shellpath) - 1, 
                                  stderr_is_error)) {
            if (errno == EFAULT) {
                /* BWB - show_help */
                printf("show_help: something on stderr: %s %s %s",
                       start_node->hostname, cmd0, printable);
            } else {
                /* BWB - show_help */
                printf("show_help: fail to rsh: %s %s %s",
                       start_node->hostname, cmd0, printable);
            }

            ret = OMPI_ERROR;
            goto cleanup;
        }

        if ('\n' == shellpath[strlen(shellpath) - 1]) {
            shellpath[strlen(shellpath) - 1] = '\0';
        }
        ompi_output_verbose(5, mca_pcm_rsh_output,
                            "remote shell %s", shellpath);

        if (NULL == strstr(p->pw_shell, "csh") &&
            NULL == strstr(p->pw_shell, "bash")) {
            /* we are neither csh-derived nor bash.  This probably
               means old-school sh or ksh.  Either way, we
               probably want to run .profile... */
            *needs_profile = true;
        }
    }

    ret = OMPI_SUCCESS;

cleanup:

    /* free up everything we used on the way */
    if (NULL != printable) free(printable);
    if (NULL != cmd0) free(cmd0);
    if (NULL != username) free(username);
    ompi_argv_free(cmdv);
    cmdv = NULL;
    cmdc = 0;

    return ret;
}


static int
internal_spawn_proc(mca_ns_base_jobid_t jobid, ompi_rte_node_schedule_t *sched,
                    ompi_list_t *hostlist, int my_start_vpid, 
                    int global_start_vpid, int num_procs)
{
    int kidstdin[2];            /* child stdin pipe */
    bool needs_profile = false;
    mca_llm_base_hostfile_node_t *start_node;
    char** cmdv = NULL;
    char *cmd0 = NULL;
    int cmdc = 0;
    char *printable = NULL;
    int stderr_is_error = mca_pcm_rsh_ignore_stderr == 0 ? 1 : 0;
    char *username = NULL;
    int ret;
    pid_t pid;
    FILE *fp;
#if 0
    int status;			/* exit status */
#endif
    int i;
    char *tmp;

    start_node = (mca_llm_base_hostfile_node_t*) ompi_list_get_first(hostlist);

    /*
     * Check to see if we need to do the .profile thing
     */
    ret = internal_need_profile(start_node, stderr_is_error,
                                &needs_profile);
    if (OMPI_SUCCESS != ret) {
        goto cleanup;
    }
    

    /*
     * Build up start array
     */

    /* build up the rsh command part */
    cmdv = ompi_argv_split(mca_pcm_rsh_agent, ' ');
    cmdc = ompi_argv_count(cmdv);

    ompi_argv_append(&cmdc, &cmdv, start_node->hostname);
    username = mca_pcm_base_get_username(start_node);
    if (NULL != username) {
        ompi_argv_append(&cmdc, &cmdv, "-l");
        ompi_argv_append(&cmdc, &cmdv, username);
    }

    /* add the start of .profile thing if required */
    if (needs_profile) {
        ompi_argv_append(&cmdc, &cmdv, "( ! [ -e ./.profile] || . ./.profile;");
    }

    /* build the command to start */
    ompi_argv_append(&cmdc, &cmdv, BOOTAGENT);
#if 1
    /* starting vpid for launchee's procs */
    tmp = ltostr(my_start_vpid);
    ompi_argv_append(&cmdc, &cmdv, "--local_start_vpid");
    ompi_argv_append(&cmdc, &cmdv, tmp);
    free(tmp);

    /* global starting vpid for this pcm spawn */
    tmp = ltostr(global_start_vpid);
    ompi_argv_append(&cmdc, &cmdv, "--global_start_vpid");
    ompi_argv_append(&cmdc, &cmdv, tmp);
    free(tmp);

    /* number of procs in this pcm spawn */
    tmp = ltostr(num_procs);
    ompi_argv_append(&cmdc, &cmdv, "--num_procs");
    ompi_argv_append(&cmdc, &cmdv, tmp);
    free(tmp);
#endif
    /* add the end of the .profile thing if required */
    if (needs_profile) {
        ompi_argv_append(&cmdc, &cmdv, ")");
    }

    /*
     * Start the process already
     */
    
    if (pipe(kidstdin)) {
        ret = OMPI_ERROR;
        goto cleanup;
    }

    if ((pid = fork()) < 0) {
        ret = OMPI_ERROR;
        goto cleanup;
    } else if (pid == 0) {
        /* child */

        if ((dup2(kidstdin[0], 0) < 0)) {
            perror(cmdv[0]);
            exit(errno);
        }

        if (close(kidstdin[0]) || close(kidstdin[1])) {
            perror(cmdv[0]);
            exit(errno);
        }

        /* Ensure that we close all other file descriptors */

        for (i = 3; i < FD_SETSIZE; i++)
            close(i);

        execvp(cmdv[0], cmdv);
        exit(errno);

    } else {
        /* parent */

#if 0
        if (close(kidstdin[0])) {
            kill(pid, SIGTERM);
            ret = OMPI_ERROR;
            goto proc_cleanup;
        }
#endif

        /* send our stuff down the wire */
        fp = fdopen(kidstdin[1], "a");
        if (fp == NULL) { perror("fdopen"); abort(); }
        ret = mca_pcm_base_send_schedule(fp, jobid, sched, start_node->count);
        fclose(fp);
        if (OMPI_SUCCESS != ret) {
            kill(pid, SIGTERM);
            goto proc_cleanup;
        }
    }
    
    ret = OMPI_SUCCESS;

 proc_cleanup:

#if 0
   /* TSW - this needs to be fixed - however, ssh is not existing - and for
    * now this at least gives us stdout/stderr.
   */
    
    /* Wait for the command to exit.  */
  do {
#if OMPI_HAVE_THREADS
    int rc = waitpid(pid, &status, 0);
#else
    int rc = waitpid(pid, &status, WNOHANG);
    if(rc == 0) {
        ompi_event_loop(OMPI_EVLOOP_ONCE);
    }
#endif
    if (rc < 0) {
        ret = OMPI_ERROR; 
        break;
    }
  } while (!WIFEXITED(status));

  if (WEXITSTATUS(status)) {
    errno = WEXITSTATUS(status);

    ret = OMPI_ERROR;
  }
#endif

 cleanup:
    /* free up everything we used on the way */
    if (NULL != printable) free(printable);
    if (NULL != cmd0) free(cmd0);
    if (NULL != username) free(username);
    ompi_argv_free(cmdv);
    cmdv = NULL;
    cmdc = 0;

    return ret;
}
