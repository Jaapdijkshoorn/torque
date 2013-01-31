/*
*         OpenPBS (Portable Batch System) v2.3 Software License
*
* Copyright (c) 1999-2000 Veridian Information Solutions, Inc.
* All rights reserved.
*
* ---------------------------------------------------------------------------
* For a license to use or redistribute the OpenPBS software under conditions
* other than those described below, or to purchase support for this software,
* please contact Veridian Systems, PBS Products Department ("Licensor") at:
*
*    www.OpenPBS.org  +1 650 967-4675                  sales@OpenPBS.org
*                        877 902-4PBS (US toll-free)
* ---------------------------------------------------------------------------
*
* This license covers use of the OpenPBS v2.3 software (the "Software") at
* your site or location, and, for certain users, redistribution of the
* Software to other sites and locations.  Use and redistribution of
* OpenPBS v2.3 in source and binary forms, with or without modification,
* are permitted provided that all of the following conditions are met.
* After December 31, 2001, only conditions 3-6 must be met:
*
* 1. Commercial and/or non-commercial use of the Software is permitted
*    provided a current software registration is on file at www.OpenPBS.org.
*    If use of this software contributes to a publication, product, or
*    service, proper attribution must be given; see www.OpenPBS.org/credit.html
*
* 2. Redistribution in any form is only permitted for non-commercial,
*    non-profit purposes.  There can be no charge for the Software or any
*    software incorporating the Software.  Further, there can be no
*    expectation of revenue generated as a consequence of redistributing
*    the Software.
*
* 3. Any Redistribution of source code must retain the above copyright notice
*    and the acknowledgment contained in paragraph 6, this list of conditions
*    and the disclaimer contained in paragraph 7.
*
* 4. Any Redistribution in binary form must reproduce the above copyright
*    notice and the acknowledgment contained in paragraph 6, this list of
*    conditions and the disclaimer contained in paragraph 7 in the
*    documentation and/or other materials provided with the distribution.
*
* 5. Redistributions in any form must be accompanied by information on how to
*    obtain complete source code for the OpenPBS software and any
*    modifications and/or additions to the OpenPBS software.  The source code
*    must either be included in the distribution or be available for no more
*    than the cost of distribution plus a nominal fee, and all modifications
*    and additions to the Software must be freely redistributable by any party
*    (including Licensor) without restriction.
*
* 6. All advertising materials mentioning features or use of the Software must
*    display the following acknowledgment:
*
*     "This product includes software developed by NASA Ames Research Center,
*     Lawrence Livermore National Laboratory, and Veridian Information
*     Solutions, Inc.
*     Visit www.OpenPBS.org for OpenPBS software support,
*     products, and information."
*
* 7. DISCLAIMER OF WARRANTY
*
* THIS SOFTWARE IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND. ANY EXPRESS
* OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
* OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND NON-INFRINGEMENT
* ARE EXPRESSLY DISCLAIMED.
*
* IN NO EVENT SHALL VERIDIAN CORPORATION, ITS AFFILIATED COMPANIES, OR THE
* U.S. GOVERNMENT OR ANY OF ITS AGENCIES BE LIABLE FOR ANY DIRECT OR INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
* LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
* OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
* LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
* NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
* EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
* This license will be governed by the laws of the Commonwealth of Virginia,
* without reference to its choice of law rules.
*/
/*
 * job_route.c - functions to route a job to another queue
 *
 * Included functions are:
 *
 * job_route() - attempt to route a job to a new destination.
 */

#include <pbs_config.h>   /* the master config generated by configure */

#include <pthread.h>

#include <sys/param.h>
#include "pbs_ifl.h"
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include "libpbs.h"
#include "pbs_error.h"
#include "list_link.h"
#include "attribute.h"
#include "server_limits.h"
#include "work_task.h"
#include "server.h"
#include "log.h"
#include "../lib/Liblog/pbs_log.h"
#include "../lib/Liblog/log_event.h"
#include "queue.h"
#include "pbs_job.h"
#include "credential.h"
#include "batch_request.h"
#include "threadpool.h"
#include "resource.h" /* struct resource */
#if __STDC__ != 1
#include <memory.h>
#endif
#include "ji_mutex.h"
#include "mutex_mgr.hpp"
#include "queue_func.h" /*find_queuebyname */

#define ROUTE_RETRY_TIME 10

/* External functions called */
int svr_movejob(job *, char *, int *, struct batch_request *);
long count_proc(char *spec);

/* Local Functions */

int  job_route(job *);

/* Global Data */
extern char *msg_routexceed;
extern char *msg_err_malloc;
extern char *msg_err_noqueue;
extern int LOGLEVEL;
extern pthread_mutex_t *reroute_job_mutex;

extern int route_retry_interval;

/*
 * Add an entry to the list of bad destinations for a job.
 */

void add_dest(

  job *jobp)

  {
  badplace  *bp;
  char      *baddest;
  if (jobp == NULL)
    {
    log_err(-1, __func__, "add_dest called with null jobp");
    return;
    }

  baddest = jobp->ji_qs.ji_destin;
  bp = (badplace *)calloc(1, sizeof(badplace));
  if (bp == NULL)
    {
    log_err(errno, __func__, msg_err_malloc);
    return;
    }

  CLEAR_LINK(bp->bp_link);
  strcpy(bp->bp_dest, baddest);
  append_link(&jobp->ji_rejectdest, &bp->bp_link, bp);
  }  /* END add_dest() */




/*
 * Check the job for a match of dest in the list of rejected destinations.
 *
 * Return: pointer if found, NULL if not.
 */

badplace *is_bad_dest(

  job  *jobp,
  char *dest)

  {
  /* ji_rejectdest is set in add_dest if approved in ??? */

  badplace *bp;
  bp = (badplace *)GET_NEXT(jobp->ji_rejectdest);

  while (bp != NULL)
    {
    if (!strcmp(bp->bp_dest, dest))
      break;

    bp = (badplace *)GET_NEXT(bp->bp_link);
    }

  return(bp);
  }  /* END is_bad_dest() */





/*
 * default_router - basic function for "routing" jobs.
 * Does a round-robin attempt on the destinations as listed,
 * job goes to first destination that takes it.
 *
 * If no destination will accept the job, PBSE_ROUTEREJ is returned,
 * otherwise 0 is returned.
 */

int default_router(

  job              *jobp,
  struct pbs_queue *qp,
  long              retry_time)

  {
  struct array_strings *dest_attr = NULL;
  char                 *destination;
  int                   last;
  int                   local_errno = 0;
  char                  log_buf[LOCAL_LOG_BUF_SIZE];

    if (LOGLEVEL >= 7)
      {
      sprintf(log_buf, "%s", jobp->ji_qs.ji_jobid);
      LOG_EVENT(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, __func__, log_buf);
      }

  if (qp->qu_attr[QR_ATR_RouteDestin].at_flags & ATR_VFLAG_SET)
    {
    dest_attr = qp->qu_attr[QR_ATR_RouteDestin].at_val.at_arst;

    last = dest_attr->as_usedptr;
    }
  else
    {
    last = 0;
    }

  /* loop through all possible destinations */

  jobp->ji_retryok = 0;

  while (1)
    {
    if (jobp->ji_lastdest >= last)
      {
      jobp->ji_lastdest = 0; /* have tried all */

      if (jobp->ji_retryok == 0)
        {
        log_event(
          PBSEVENT_JOB,
          PBS_EVENTCLASS_JOB,
          jobp->ji_qs.ji_jobid,
          pbse_to_txt(PBSE_ROUTEREJ));

        return(PBSE_ROUTEREJ);
        }
      else
        {
        /* set time to retry job */

        jobp->ji_qs.ji_un.ji_routet.ji_rteretry = retry_time;

        return(0);
        }
      }

    destination = dest_attr->as_string[jobp->ji_lastdest++];

    if (is_bad_dest(jobp, destination))
      continue;

    switch (svr_movejob(jobp, destination, &local_errno, NULL))
      {
      case ROUTE_PERM_FAILURE: /* permanent failure */

        add_dest(jobp);

        break;

      case ROUTE_SUCCESS:  /* worked */

      case ROUTE_DEFERRED:  /* deferred */

        return(0);

        /*NOTREACHED*/

        break;

      case ROUTE_RETRY:  /* failed, but try destination again */
        
        jobp->ji_retryok = 1;

        break;

      }
    }

  return(-1);
  }  /* END default_router() */





/*
 * job_route - route a job to another queue
 *
 * NOTE: This MUST always called by a thread holding queue's mutex.
 *
 * This is only called for jobs in a routing queue.
 * Loop over all the possible destinations for the route queue.
 * Check each one to see if it is ok to try it.  It could have been
 * tried before and returned a rejection.  If so, skip to the next
 * destination.  If it is ok to try it, look to see if it is a local
 * queue.  If so, it is an internal procedure to try/do the move.
 * If not, a child process is created to deal with it in the
 * function net_route(), see svr_movejob.c.
 *
 * Returns: 0 on success, non-zero (error number) on failure
 */

int job_route(

  job *jobp)      /* job to route */

  {
  int               bad_state = 0;
  time_t            life;
  time_t            time_now = time(NULL);
  char              log_buf[LOCAL_LOG_BUF_SIZE];

  struct pbs_queue *qp;
  long              retry_time;
  
  if (LOGLEVEL >= 7)
    {
    sprintf(log_buf, "%s", jobp->ji_qs.ji_jobid);
    log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, __func__, log_buf);
    }
  
  qp = get_jobs_queue(&jobp);
  
  if (jobp == NULL)
    {
    return(PBSE_JOB_RECYCLED);
    }

  if (qp == NULL)
    {
    return(PBSE_BADSTATE);
    }

  mutex_mgr qp_mutex = mutex_mgr(qp->qu_mutex, true);

  /* see if the job is able to be routed */
  switch (jobp->ji_qs.ji_state)
    {

    case JOB_STATE_TRANSIT:

      return(0);  /* already going, ignore it */

      /*NOTREACHED*/

      break;

    case JOB_STATE_QUEUED:

      /* NO-OP */

      break;   /* ok to try */

    case JOB_STATE_HELD:

      /* job may be acceptable */

      bad_state = !qp->qu_attr[QR_ATR_RouteHeld].at_val.at_long;

      break;

    case JOB_STATE_WAITING:

      /* job may be acceptable */

      bad_state = !qp->qu_attr[QR_ATR_RouteWaiting].at_val.at_long;

      break;

    default:

      snprintf(log_buf, sizeof(log_buf), "%s %d %s", 
        pbse_to_txt(PBSE_BADSTATE), jobp->ji_qs.ji_state, __func__);

      log_event(PBSEVENT_DEBUG,PBS_EVENTCLASS_JOB,jobp->ji_qs.ji_jobid,log_buf);
      
      return(PBSE_NONE);

      /*NOTREACHED*/

      break;
    }

  /* check the queue limits, can we route any (more) */
  if (qp->qu_attr[QA_ATR_Started].at_val.at_long == 0)
    {
    /* queue not started - no routing */

    return(0);
    }

  if ((qp->qu_attr[QA_ATR_MaxRun].at_flags & ATR_VFLAG_SET) &&
      (qp->qu_attr[QA_ATR_MaxRun].at_val.at_long <= qp->qu_njstate[JOB_STATE_TRANSIT]))
    {
    /* max number of jobs being routed */

    return(0);
    }

  /* what is the retry time and life time of a job in this queue */

  if (qp->qu_attr[QR_ATR_RouteRetryTime].at_flags & ATR_VFLAG_SET)
    {
    retry_time =
      (long)time_now +
      qp->qu_attr[QR_ATR_RouteRetryTime].at_val.at_long;
    }
  else
    {
    retry_time = (long)time_now + PBS_NET_RETRY_TIME;
    }

  if (qp->qu_attr[QR_ATR_RouteLifeTime].at_flags & ATR_VFLAG_SET)
    {
    life =
      jobp->ji_qs.ji_un.ji_routet.ji_quetime +
      qp->qu_attr[QR_ATR_RouteLifeTime].at_val.at_long;
    }
  else
    {
    life = 0; /* forever */
    }

  if (life && (life < time_now))
    {
    log_event(PBSEVENT_JOB,PBS_EVENTCLASS_JOB,jobp->ji_qs.ji_jobid,msg_routexceed);

    /* job too long in queue */
    return(PBSE_ROUTEEXPD);
    }

  if (bad_state)
    {
    /* not currently routing this job */

    return(PBSE_NONE);
    }

  /* default_router and site_alt_router expect the queue to 
     be unlocked. */
  if (qp->qu_attr[QR_ATR_AltRouter].at_val.at_long == 0)
    {
    qp_mutex.unlock();
    return(default_router(jobp, qp, retry_time));
    }

  qp_mutex.unlock();
  return(site_alt_router(jobp, qp, retry_time));
  }  /* END job_route() */




int reroute_job(

  job *pjob)

  {
  int        rc = PBSE_NONE;
  char       log_buf[LOCAL_LOG_BUF_SIZE];

  if (LOGLEVEL >= 7)
    {
    sprintf(log_buf, "%s", pjob->ji_qs.ji_jobid);
    LOG_EVENT(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, __func__, log_buf);
    }
    
  rc = job_route(pjob);

  if (rc == PBSE_ROUTEREJ)
    job_abt(&pjob, pbse_to_txt(PBSE_ROUTEREJ));
  else if (rc == PBSE_ROUTEEXPD)
    job_abt(&pjob, msg_routexceed);
  else if (rc == PBSE_QUENOEN)
    job_abt(&pjob, msg_err_noqueue);

  return(rc);      
  } /* END reroute_job() */




/*
 * queue_route - route any "ready" jobs in a specific queue
 *
 * look for any job in the queue whose route retry time has
 * passed.

 * If the queue is "started" and if the number of jobs in the
 * Transiting state is less than the max_running limit, then
 * attempt to route it.
 *
 * Be sure to free vp. It is a string that was allocated
 * in handle_queue_routing_retries
 */

void *queue_route(

  void *vp)

  {
  pbs_queue *pque;
  job       *pjob = NULL;
  char      *queue_name;
  char       log_buf[LOCAL_LOG_BUF_SIZE];

  int       iter = -1;

  queue_name = (char *)vp;

  if (queue_name == NULL)
    {
    sprintf(log_buf, "NULL queue name");
    log_err(-1, __func__, log_buf);
    return(NULL);
    }

  while (1)
    {
    pthread_mutex_lock(reroute_job_mutex);
    /* Before we attempt to service this queue, make sure we can find it. */
    pque = find_queuebyname(queue_name);
    if (pque == NULL)
      {
      sprintf(log_buf, "Could not find queue %s", queue_name);
      log_err(-1, __func__, log_buf);
      free(queue_name);
      return(NULL);
      }

    mutex_mgr que_mutex(pque->qu_mutex, true);
  
    if (LOGLEVEL >= 7)
      {
      snprintf(log_buf, sizeof(log_buf), "routing any ready jobs in queue: %s", queue_name);
      log_event(PBSEVENT_SYSTEM, PBS_EVENTCLASS_QUEUE, __func__, log_buf);
      }

    while ((pjob = next_job(pque->qu_jobs,&iter)) != NULL)
      {
      /* We only want to try if routing has been tried at least once - this is to let
       * req_commit have the first crack at routing always. */

      if (pjob->ji_commit_done == 0) /* when req_commit is done it will set ji_commit_done to 1 */
        {
        unlock_ji_mutex(pjob, __func__, "1", LOGLEVEL);
        continue;
        }

      /* queue must be unlocked when calling reroute_job */
      que_mutex.unlock();
      reroute_job(pjob);
      unlock_ji_mutex(pjob, __func__, "2", LOGLEVEL);

      /* need to relock queue when we go to call next_job */
      pque = find_queuebyname(queue_name);
      if (pque == NULL)
        {
        sprintf(log_buf, "Could not find queue %s", queue_name);
        log_err(-1, __func__, log_buf);
        free(queue_name);
        return(NULL);
        }

      que_mutex.mark_as_locked();
      }

    /* we come out of the while loop with the queue locked.
       We don't want it locked while we sleep */
    que_mutex.unlock();
    pthread_mutex_unlock(reroute_job_mutex);
    sleep(route_retry_interval);
    } /* END while (1) */

  free(queue_name);
  return(NULL);
  } /* END queue_route() */

